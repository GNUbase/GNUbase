// SPDX-License-Identifier: GPL-3.0-or-later
// Render in place and freeze (#116, docs/features/render-in-place.md): track.renderInPlace, track.freeze,
// track.unfreeze and mixdown.export's render points.
#include "engine/audiofile/audiofile.h"
#include "engine/dawproject/dawproject.h"
#include "engine/document/document.h"
#include "engine/document/rendersnapshot.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/plugins/hostmanager.h"
#include "engine/processors/builtinplugin.h"
#include "plugin/plugin.h"
#include "testutils/heldjob.h"
#include "testutils/tempfolder.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;
using gnubase::test::waitForJob;

namespace
{
	namespace fs = std::filesystem;

	constexpr int64_t g_scrubFrames = 480; // how far each scrub of a test goes

	// What a render's plugin was told: the modes set before each activation, and how many blocks of each mode it got.
	struct Modes
	{
		std::vector<gnubase::plugin::ProcessMode> activated; // [plugin job thread]
		std::atomic<int> realtimeBlocks{0};
		std::atomic<int> offlineBlocks{0};
	};

	// A plugin that passes everything on to another one and notes the modes in Modes.
	class ModeRecorder final : public gnubase::plugin::Plugin
	{
	public:
		ModeRecorder(std::unique_ptr<gnubase::plugin::Plugin> plugin, std::shared_ptr<Modes> modes)
			: m_plugin(std::move(plugin)), m_modes(std::move(modes))
		{
		}
		const gnubase::plugin::Descriptor& descriptor() const override { return m_plugin->descriptor(); }
		bool init() override { return m_plugin->init(); }
		bool setProcessMode(gnubase::plugin::ProcessMode mode) override
		{
			m_mode = mode;
			return m_plugin->setProcessMode(mode);
		}
		bool activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames) override
		{
			m_modes->activated.push_back(m_mode);
			return m_plugin->activate(sampleRate, minFrames, maxFrames);
		}
		void deactivate() override { m_plugin->deactivate(); }
		bool startProcessing() noexcept override { return m_plugin->startProcessing(); }
		void stopProcessing() noexcept override { m_plugin->stopProcessing(); }
		void reset() noexcept override { (*m_plugin).reset(); }
		gnubase::plugin::ProcessStatus process(const gnubase::plugin::Process& process) noexcept override
		{
			++(process.mode == gnubase::plugin::ProcessMode::Offline ? m_modes->offlineBlocks
																	 : m_modes->realtimeBlocks);
			return m_plugin->process(process);
		}
		void* extension(std::string_view id) override { return m_plugin->extension(id); }
		void onMainThread() override { m_plugin->onMainThread(); }

	private:
		std::unique_ptr<gnubase::plugin::Plugin> m_plugin;
		std::shared_ptr<Modes> m_modes;
		gnubase::plugin::ProcessMode m_mode = gnubase::plugin::ProcessMode::Realtime;
	};

	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		if (result.value.contains("job") &&
			(command == "track.freeze" || command == "track.renderInPlace" || command == "mixdown.export"))
		{
			const Json job = gnubase::test::waitForJob(client, result.value.at("job").at("id").get<uint64_t>());
			INFO(job.dump());
			REQUIRE(job.at("state") == "done");
			return job.at("result");
		}
		return result.value;
	}

	std::string reject(Client& client, const std::string& command, Json parameters)
	{
		const Result result = client.call(command, std::move(parameters));
		if (result.ok() && result.value.contains("job"))
		{
			const Json job = gnubase::test::waitForJob(client, result.value.at("job").at("id").get<uint64_t>());
			INFO(job.dump());
			REQUIRE(job.at("state") == "failed");
			return job.at("error").get<std::string>();
		}
		INFO(command << " succeeded: " << result.value.dump());
		REQUIRE(!result.ok());
		return result.error;
	}

	std::vector<float> samplesOf(const std::string& path)
	{
		const std::unique_ptr<AudioFileReader> reader = AudioFileReader::open(path);
		const AudioFileInfo info = reader->info();
		std::vector<float> samples(static_cast<size_t>(info.frames * info.channels));
		REQUIRE(reader->read(samples.data(), static_cast<size_t>(info.frames)) == static_cast<size_t>(info.frames));
		return samples;
	}

	// Where two sample sequences first differ, or -1 if they are the same.
	int64_t firstDifference(const std::vector<float>& actual, const std::vector<float>& expected)
	{
		const size_t common = std::min(actual.size(), expected.size());
		for (size_t i = 0; i < common; ++i)
			if (actual[i] != expected[i])
			{
				UNSCOPED_INFO("sample " << i << " is " << actual[i] << " instead of " << expected[i]);
				return static_cast<int64_t>(i);
			}
		if (actual.size() != expected.size())
		{
			UNSCOPED_INFO(actual.size() << " samples instead of " << expected.size());
			return static_cast<int64_t>(common);
		}
		return -1;
	}

	// The peak of the difference of two renders of the same length: 0 when they null.
	float nullTest(const std::vector<float>& actual, const std::vector<float>& expected)
	{
		REQUIRE(actual.size() == expected.size());
		float peak = 0.0f;
		for (size_t i = 0; i < actual.size(); ++i)
			peak = std::max(peak, std::abs(actual[i] - expected[i]));
		return peak;
	}

	// How far a render is from the original reduced by reductionDb dB: 0 when actual has that reduction, to float
	// precision. A freeze rendered before its plugin plays is unchanged, corresponding to 0 dB reduction.
	float turnedDownBy(const std::vector<float>& actual, const std::vector<float>& original, float reductionDb)
	{
		std::vector<float> expected = original;
		const float factor = std::pow(10.0f, -reductionDb / 20.0f);
		for (float& sample : expected)
			sample *= factor;
		return nullTest(actual, expected);
	}

	float peakOf(const std::vector<float>& samples)
	{
		float peak = 0.0f;
		for (const float sample : samples)
			peak = std::max(peak, std::abs(sample));
		return peak;
	}

	// A project with a ramp, every sample different, to play on linear audio tracks through the main mix.
	class Session
	{
	public:
		Session()
		{
			run(client, "audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}, {"captureSeconds", 1}});
			gnubase::test::Audio audio;
			audio.channels = 2;
			for (int frame = 0; frame < 6000; ++frame)
				for (int c = 0; c < 2; ++c)
					audio.samples.push_back((c == 0 ? 1.0f : -0.5f) * static_cast<float>(frame + 1) / 8192.0f);
			const fs::path file = media.path() / "ramp.wav";
			gnubase::test::writeWav(file, audio);
			const Json added = run(client, "media.add", {{"path", file.u8string()}});
			clip = run(client, "clip.add", {{"media", added.at("media").at("id")}}).at("clip").at("id").get<ObjectId>();
			// Freeze files go next to the project file, rendered tracks into its media folder.
			run(client, "project.save", {{"path", (media.path() / "project.dawproject").u8string()}});
		}

		struct Track
		{
			ObjectId track = 0;
			ObjectId channel = 0;
		};

		Track addTrack(const std::string& name, int channels = 2)
		{
			const Json added = run(client, "track.add",
				{{"type", "audio"}, {"timeBase", "linear"}, {"channels", channels}, {"name", name}});
			const Track track{added.at("track").at("id").get<ObjectId>(), added.at("channel").at("id").get<ObjectId>()};
			run(client, "audioEvent.add", {{"track", track.track}, {"clip", clip}, {"position", 1000}});
			return track;
		}

		// A test synth playing three notes, on a linear instrument track.
		Track addSynth()
		{
			const Json added = run(client, "track.add",
				{{"type", "instrument"}, {"timeBase", "linear"}, {"instrument", "testSynth"}, {"name", "Synth"}});
			const Track track{added.at("track").at("id").get<ObjectId>(), added.at("channel").at("id").get<ObjectId>()};
			const Json part = run(client, "midiPart.add", {{"track", track.track}, {"position", 0}, {"length", 48000}});
			// A quarter note is 24000 samples at 120 BPM.
			constexpr int64_t sixteenth = 1720320 / 4;
			run(client, "note.add",
				{{"part", part.at("part").at("id")},
					{"notes",
						{{{"position", 0}, {"length", sixteenth}, {"pitch", 60}, {"velocity", 0.8}},
							{{"position", 2 * sixteenth}, {"length", sixteenth}, {"pitch", 67}, {"velocity", 0.6}},
							{{"position", 4 * sixteenth}, {"length", sixteenth}, {"pitch", 72}, {"velocity", 0.7}}}}});
			return track;
		}

		// Inserts the built-in gain plugin at gain dB and waits until it plays.
		ObjectId insertGain(ObjectId channel, double gain)
		{
			const Json slot =
				run(client, "plugin.insert", {{"channel", channel}, {"plugin", "builtin:gainPan"}}).at("slot").at("id");
			activate({channel});
			setGain(channel, slot.get<ObjectId>(), gain);
			return slot.get<ObjectId>();
		}

		void setGain(ObjectId channel, ObjectId slot, double gain)
		{
			run(client, "parameter.set",
				{{"channel", channel}, {"target", {{"type", "plugin"}, {"slot", slot}, {"parameter", 0}}},
					{"value", gain}});
		}

		// Waits until the plugin slots of these channels have their plugins active, and the graph has them. An
		// unfreeze makes the plugins of the track's slots again, on the plugin job thread: until they are active, the
		// slots pass their audio on unchanged (#951).
		void activate(const std::vector<ObjectId>& channels)
		{
			for (const ObjectId channel : channels)
			{
				const Json slots = run(client, "plugin.list", {{"channel", channel}}).at("slots");
				for (const Json& slot : slots)
					gnubase::test::waitForPlugin(client, slot.at("id"));
			}
		}

		// The main mix from 0 to 40000 as a 32-bit float file's samples.
		std::vector<float> mainMix()
		{
			const Json result = run(client, "mixdown.export",
				{{"folder", folder.path().u8string()}, {"name", "mix"}, {"start", 0}, {"end", 40000},
					{"sampleFormat", "float32"}, {"conflicts", "overwrite"}});
			return samplesOf(result.at("files").at(0).at("path").get<std::string>());
		}

		size_t slotNodes()
		{
			const Json graph = run(client, "graph.get");
			return static_cast<size_t>(std::count_if(graph.at("nodes").begin(), graph.at("nodes").end(),
				[](const Json& node) { return node.at("kind") == "pluginSlot"; }));
		}

		Json track(ObjectId id)
		{
			Json found;
			const Json project = run(client, "project.get");
			for (const Json& item : project.at("tracks"))
				if (item.at("id") == id)
					found = item;
			REQUIRE(found.is_object());
			return found;
		}

		// A channel's monitor and record enable buttons.
		std::pair<bool, bool> buttons(ObjectId channel)
		{
			Json state;
			const Json current = run(client, "project.get");
			for (const Json& found : current.at("channels"))
				if (found.at("id") == channel)
					state = found;
			REQUIRE(state.is_object());
			return std::pair{state.at("monitor").get<bool>(), state.at("recordEnable").get<bool>()};
		}

		Engine engine;
		Client client{engine};
		gnubase::test::TempDir media;
		gnubase::test::TempDir folder;
		ObjectId clip = 0;
	};
}

TEST_CASE("mixdown.export renders a channel from a point inside its strip", "[integration]")
{
	Session project;
	const Session::Track mono = project.addTrack("Mono", 1);
	project.insertGain(mono.channel, -6.0);
	run(project.client, "channel.set", {{"channel", mono.channel}, {"pre", {{"gain", 6.0}}}, {"volume", -10.0}});
	run(project.client, "audio.render", {{"frames", 4096}}); // the pre gain glides there
	const auto at = [&project, &mono](const char* point)
	{
		const Json result = run(project.client, "mixdown.export",
			{{"folder", project.folder.path().u8string()}, {"name", point}, {"channels", {mono.channel}},
				{"start", 1000}, {"end", 7000}, {"sampleFormat", "float32"}, {"point", point}});
		return samplesOf(result.at("files").at(0).at("path").get<std::string>());
	};
	const std::vector<float> source = at("source");
	const std::vector<float> inserts = at("inserts");
	const std::vector<float> strip = at("strip");
	const std::vector<float> output = at("output");
	// A mono channel's points before the panner are mono; the ramp's left channel plays on it.
	REQUIRE(source.size() == 6000);
	CHECK(inserts.size() == 6000);
	CHECK(strip.size() == 6000);
	CHECK(output.size() == 12000); // after the panner
	CHECK(source[99] == 100.0f / 8192.0f);
	CHECK(std::abs(inserts[99] - source[99]) < 1e-6f); // +6 dB, then -6 dB
	CHECK(firstDifference(strip, inserts) == -1);      // the strip's sections are off
	CHECK(std::abs(peakOf(output) - (peakOf(strip) * std::pow(10.0f, -10.0f / 20.0f) * 0.70794578f)) < 1e-4f);

	CHECK_THAT(reject(project.client, "mixdown.export",
				   {{"folder", project.folder.path().u8string()}, {"name", "bus"}, {"start", 0}, {"end", 100},
					   {"point", "strip"}}),
		ContainsSubstring("bus"));
}

TEST_CASE("a frozen track null-tests against the live render", "[integration]")
{
	Session project;
	const Session::Track audio = project.addTrack("Ramp");
	project.insertGain(audio.channel, -6.0);
	const Session::Track synth = project.addSynth();
	project.insertGain(synth.channel, 3.0);
	run(project.client, "channel.set", {{"channel", audio.channel}, {"volume", -3.0}, {"pan", 0.25}});
	run(project.client, "channel.set",
		{{"channel", synth.channel}, {"eq", {{"bands", {{{"band", 2}, {"on", true}, {"gain", 6.0}}}}}}});
	run(project.client, "audio.render", {{"frames", 4096}});
	const std::vector<float> live = project.mainMix();
	CHECK(peakOf(live) > 0.1f);
	CHECK(project.slotNodes() == 2);

	const Json frozen = run(project.client, "track.freeze", {{"tracks", {audio.track, synth.track}}, {"tail", 1}});
	REQUIRE(frozen.at("tracks").size() == 2);
	const Json freeze = frozen.at("tracks").at(0).at("frozen");
	CHECK(fs::u8path(freeze.at("path").get<std::string>()) == project.media.path() / "Freeze" / "Ramp.wav");
	CHECK(freeze.at("length") == 7000); // to the end of the event, and silence after it
	CHECK(frozen.at("tracks").at(1).at("frozen").at("length").get<int64_t>() >= 30000 + 2400); // with the release

	// The freeze files play instead of the plugins, and sound the same. They are what the live chain played, but the
	// EQ after them doesn't start each render in the same state: what is left of earlier renders decays far below
	// what a 24-bit file holds.
	CHECK(project.slotNodes() == 0);
	CHECK(nullTest(project.mainMix(), live) < 1e-20f);

	// The fader still plays: a frozen track is turned down like any other.
	run(project.client, "channel.set", {{"channel", audio.channel}, {"volume", -60.0}});
	run(project.client, "audio.render", {{"frames", 4096}});
	CHECK(firstDifference(project.mainMix(), live) >= 0);
	run(project.client, "channel.set", {{"channel", audio.channel}, {"volume", -3.0}});
	run(project.client, "audio.render", {{"frames", 4096}});

	// Unfreezing plays the plugins again, and undoing it plays the freeze files.
	run(project.client, "track.unfreeze", {{"tracks", {audio.track, synth.track}}});
	CHECK(project.track(audio.track).at("frozen").is_null());
	CHECK(project.slotNodes() == 2);
	project.activate({audio.channel, synth.channel});
	CHECK(nullTest(project.mainMix(), live) < 1e-20f);
	run(project.client, "history.undo");
	CHECK(project.track(synth.track).at("frozen") == frozen.at("tracks").at(1).at("frozen"));
	CHECK(project.slotNodes() == 0);
	CHECK(nullTest(project.mainMix(), live) < 1e-20f);
}

TEST_CASE("an instrument track frozen without its inserts plays them on its freeze file", "[integration]")
{
	// Include Inserts for Instrument Tracks off (reference manual p. 992): the freeze file holds the instrument, and
	// the track's slots still play, on the file. An audio track's file holds its slots in any case.
	Session project;
	const Session::Track audio = project.addTrack("Ramp");
	project.insertGain(audio.channel, -6.0);
	const Session::Track synth = project.addSynth();
	const ObjectId slot = project.insertGain(synth.channel, 3.0);
	run(project.client, "channel.set", {{"channel", synth.channel}, {"pre", {{"gain", -2.0}}}});
	run(project.client, "audio.render", {{"frames", 4096}});
	const std::vector<float> unautomated = project.mainMix();
	// The slot's automation plays on the file too.
	const ObjectId lane = run(project.client, "automationLane.add",
		{{"track", synth.track}, {"target", {{"type", "plugin"}, {"slot", slot}, {"parameter", 0}}}})
							  .at("lane")
							  .at("id")
							  .get<ObjectId>();
	run(project.client, "automationPoint.add",
		{{"lane", lane},
			{"points",
				{{{"position", 0}, {"value", -12.0}, {"shape", "step"}},
					{{"position", 20000}, {"value", 0.0}, {"shape", "step"}}}}});
	const std::vector<float> live = project.mainMix();
	REQUIRE(nullTest(live, unautomated) > 0.01f);

	const Json frozen = run(project.client, "track.freeze",
		{{"tracks", {audio.track, synth.track}}, {"tail", 1}, {"includeInserts", false}});
	CHECK(frozen.at("tracks").at(0).at("frozen").at("inserts") == true);
	CHECK(frozen.at("tracks").at(1).at("frozen").at("inserts") == false);
	CHECK(project.slotNodes() == 1);
	project.activate({synth.channel});
	CHECK(nullTest(project.mainMix(), live) < 1e-20f);

	// The slot plays on the file: without its automation, turning it down turns the track down.
	run(project.client, "automationLane.set", {{"lane", lane}, {"read", false}});
	project.setGain(synth.channel, slot, -18.0);
	run(project.client, "audio.render", {{"frames", 4096}});
	CHECK(nullTest(project.mainMix(), live) > 0.01f);

	// The project file keeps it.
	const fs::path file = project.folder.path() / "instrument.dawproject";
	run(project.client, "project.save", {{"path", file.u8string()}});
	run(project.client, "project.load", {{"path", file.u8string()}});
	CHECK(project.track(synth.track).at("frozen").at("inserts") == false);
	CHECK(project.slotNodes() == 1);

	// Only an instrument track can be frozen without its inserts: a file that says so of an audio track doesn't load.
	Json saved = run(project.client, "project.get");
	CHECK_NOTHROW(projectFromJson(saved));
	for (Json& track : saved.at("tracks"))
		if (track.at("id") == audio.track)
			track.at("frozen")["inserts"] = false;
	CHECK_THROWS_AS(projectFromJson(saved), std::invalid_argument);
}

TEST_CASE("undo brings back the freeze file that was frozen then", "[integration]")
{
	Session project;
	const Session::Track audio = project.addTrack("Ramp");
	const ObjectId slot = project.insertGain(audio.channel, -6.0);
	run(project.client, "audio.render", {{"frames", 4096}});
	const Json first = run(project.client, "track.freeze", {{"tracks", {audio.track}}, {"tail", 0}});
	const std::vector<float> frozen = project.mainMix();

	// The unfreeze makes the plugin again. Frozen again once the plugin plays and with a gain other than 0 dB (which
	// the slot passes on before it plays), the new freeze file can only come from the plugin.
	run(project.client, "track.unfreeze", {{"tracks", {audio.track}}});
	project.activate({audio.channel});
	project.setGain(audio.channel, slot, -18.0);
	run(project.client, "audio.render", {{"frames", 4096}});
	const Json second = run(project.client, "track.freeze", {{"tracks", {audio.track}}, {"tail", 0}});
	REQUIRE(second.at("tracks").at(0).at("frozen").at("path") != first.at("tracks").at(0).at("frozen").at("path"));
	const std::vector<float> firstFile = samplesOf(first.at("tracks").at(0).at("frozen").at("path").get<std::string>());
	const std::vector<float> secondFile =
		samplesOf(second.at("tracks").at(0).at("frozen").at("path").get<std::string>());
	REQUIRE(peakOf(firstFile) > 0.1f);
	REQUIRE(turnedDownBy(secondFile, firstFile, 12.0f) < 1e-5f); // -18 dB instead of -6 dB
	CHECK(nullTest(project.mainMix(), frozen) > 0.1f);

	for (int step = 0; step < 3; ++step) // the freeze, the gain and the unfreeze
		run(project.client, "history.undo");
	REQUIRE(project.track(audio.track).at("frozen") == first.at("tracks").at(0).at("frozen"));
	CHECK(nullTest(project.mainMix(), frozen) < 1e-20f);
}

TEST_CASE("scrubbing a frozen track reads the freeze file of that time", "[integration]")
{
	Session project;
	const Session::Track audio = project.addTrack("Ramp");
	const ObjectId slot = project.insertGain(audio.channel, -6.0);
	run(project.client, "audio.render", {{"frames", 4096}});
	const Json first = run(project.client, "track.freeze", {{"tracks", {audio.track}}, {"tail", 0}});
	const std::vector<float> firstFile = samplesOf(first.at("tracks").at(0).at("frozen").at("path").get<std::string>());
	const double rate = run(project.client, "project.get").at("sampleRate").get<double>();
	// The freeze file plays as a media file of the channel's id, but it is none of the project's.
	CHECK(project.client.call("audition.start", {{"media", audio.channel}, {"position", 0}}).error ==
		"no media file " + std::to_string(audio.channel));

	// Scrubs 480 frames from where the last scrub got to, to position, in as long: at normal speed, so it plays
	// the freeze file's frames there, forwards or backwards. The audition goes straight to the outputs. The scrub
	// goes on throughout, so it keeps its file readers open.
	constexpr int64_t frames = g_scrubFrames;
	run(project.client, "audition.scrub", {{"track", audio.track}, {"position", 1000}});
	const auto scrubs = [&project, &audio, rate](int64_t position, const std::vector<float>& file)
	{
		constexpr int64_t frames = g_scrubFrames;
		const size_t start = run(project.client, "audio.capture", {{"channel", 0}, {"start", 0}, {"frames", 1048576}})
								 .at("samples")
								 .size();
		run(project.client, "audition.scrub",
			{{"track", audio.track}, {"position", position}, {"seconds", static_cast<double>(frames) / rate}});
		run(project.client, "audio.render", {{"frames", 600}});
		size_t wrong = 0;
		for (int channel = 0; channel < 2; ++channel)
		{
			const auto output =
				run(project.client, "audio.capture", {{"channel", channel}, {"start", start}, {"frames", frames}})
					.at("samples")
					.get<std::vector<float>>();
			REQUIRE(output.size() == static_cast<size_t>(frames));
			for (int64_t k = 0; k < frames; ++k)
			{
				// Forwards from position - frames, or backwards from position + frames.
				const int64_t frame = position > 1000 ? position - frames + k : position + frames - k;
				const float expected = file[static_cast<size_t>((frame * 2) + channel)];
				if (output[static_cast<size_t>(k)] != expected && wrong++ == 0)
					UNSCOPED_INFO("channel " << channel << ", frame " << k << ": " << output[static_cast<size_t>(k)]
											 << " instead of " << expected);
			}
		}
		return wrong == 0;
	};
	CHECK(scrubs(1000 + frames, firstFile));

	// Frozen again with another gain, the scrub reads the new file. The unfreeze makes the plugin again, so the freeze
	// waits until it plays and uses a gain other than 0 dB (which the slot passes on before it plays).
	run(project.client, "track.unfreeze", {{"tracks", {audio.track}}});
	project.activate({audio.channel});
	project.setGain(audio.channel, slot, -18.0);
	run(project.client, "audio.render", {{"frames", 4096}});
	const Json second = run(project.client, "track.freeze", {{"tracks", {audio.track}}, {"tail", 0}});
	const std::string secondPath = second.at("tracks").at(0).at("frozen").at("path").get<std::string>();
	REQUIRE(secondPath != first.at("tracks").at(0).at("frozen").at("path"));
	const std::vector<float> secondFile = samplesOf(secondPath);
	REQUIRE(peakOf(firstFile) > 0.1f);
	REQUIRE(turnedDownBy(secondFile, firstFile, 12.0f) < 1e-5f); // -18 dB instead of -6 dB
	CHECK(scrubs(1000, secondFile));

	// Undone back to the first freeze, it reads the first file again.
	for (int step = 0; step < 3; ++step) // the freeze, the gain and the unfreeze
		run(project.client, "history.undo");
	REQUIRE(project.track(audio.track).at("frozen") == first.at("tracks").at(0).at("frozen"));
	CHECK(scrubs(1000 + frames, firstFile));
	run(project.client, "audition.stop");
}

TEST_CASE("a frozen track stays frozen in the project file", "[integration]")
{
	Session project;
	const Session::Track audio = project.addTrack("Ramp");
	const Json frozen = run(project.client, "track.freeze", {{"tracks", {audio.track}}, {"tail", 0}});
	const Json freeze = frozen.at("tracks").at(0).at("frozen");
	const std::vector<float> rendered = samplesOf(freeze.at("path").get<std::string>());

	SECTION("with the freeze file in it")
	{
		// Like the media, the freeze file goes into the file and comes out into its media folder.
		const fs::path file = project.folder.path() / "frozen.dawproject";
		run(project.client, "project.save", {{"path", file.u8string()}});
		run(project.client, "project.load", {{"path", file.u8string()}});
		const Json loaded = project.track(audio.track).at("frozen");
		CHECK(fs::u8path(loaded.at("path").get<std::string>()).parent_path().parent_path() ==
			project.folder.path() / "frozen media");
		CHECK(loaded.at("length") == freeze.at("length"));
		CHECK(firstDifference(samplesOf(loaded.at("path").get<std::string>()), rendered) == -1);
	}

	SECTION("in it once for the tracks that play it")
	{
		// A duplicated frozen track plays the same freeze file.
		run(project.client, "track.duplicate", {{"track", audio.track}});
		const fs::path file = project.folder.path() / "twice.dawproject";
		run(project.client, "project.save", {{"path", file.u8string()}});
		run(project.client, "project.load", {{"path", file.u8string()}});
		const Json tracks = run(project.client, "project.get").at("tracks");
		REQUIRE(tracks.size() == 2);
		CHECK(tracks.at(0).at("frozen") == tracks.at(1).at("frozen"));
		CHECK(firstDifference(samplesOf(tracks.at(1).at("frozen").at("path").get<std::string>()), rendered) == -1);
	}

	SECTION("referring to it where it was, if it isn't there")
	{
		// The result lists it once for the tracks that play it, as it does missing media files (#1736).
		run(project.client, "track.duplicate", {{"track", audio.track}});
		fs::remove(fs::u8path(freeze.at("path").get<std::string>()));
		const fs::path file = project.folder.path() / "missing.dawproject";
		CHECK(run(project.client, "project.save", {{"path", file.u8string()}}).at("missing") ==
			Json::array({freeze.at("path")}));
		run(project.client, "project.load", {{"path", file.u8string()}});
		CHECK(project.track(audio.track).at("frozen") == freeze);
	}

	SECTION("not from another track's entry of the same name in another case (#871)")
	{
		// A build before #1736 referred to a missing freeze file relatively, which entries looked up in any case
		// would take for another track's embedded file.
		run(project.client, "track.duplicate", {{"track", audio.track}});
		const fs::path file = project.folder.path() / "cases.dawproject";
		run(project.client, "project.save", {{"path", file.u8string()}});
		std::map<std::string, std::string> entries = gnubase::test::readZip(file.u8string());
		Json extension = Json::parse(entries.at("gnubase.json"));
		const std::string other = extension.at("frozen").begin().key() == std::to_string(audio.track)
			? std::next(extension.at("frozen").begin()).key()
			: extension.at("frozen").begin().key();
		extension.at("frozen")[other] = "FREEZE/ramp.wav";
		entries["gnubase.json"] = extension.dump();
		gnubase::test::writeZip(file.u8string(), entries);
		run(project.client, "project.load", {{"path", file.u8string()}});
		CHECK(project.track(std::stoull(other)).at("frozen").at("path") ==
			(project.folder.path() / "FREEZE" / "ramp.wav").lexically_normal().u8string());
		CHECK(firstDifference(
				  samplesOf(project.track(audio.track).at("frozen").at("path").get<std::string>()), rendered) == -1);
	}

	SECTION("from a file without the freeze paths of its own (#871)")
	{
		// A file from before "frozen" in gnubase.json loads the paths the project itself holds.
		run(project.client, "project.save", {{"embedMedia", false}});
		const std::string file = (project.media.path() / "project.dawproject").u8string();
		std::map<std::string, std::string> entries = gnubase::test::readZip(file);
		Json extension = Json::parse(entries.at("gnubase.json"));
		REQUIRE(extension.erase("frozen") == 1);
		entries["gnubase.json"] = extension.dump();
		gnubase::test::writeZip(file, entries);
		run(project.client, "project.load", {{"path", file}});
		CHECK(project.track(audio.track).at("frozen") == freeze);
	}

	SECTION("referring to it in the project's folder, which moves")
	{
		run(project.client, "project.save", {{"embedMedia", false}});
		const fs::path moved = project.folder.path() / "moved";
		fs::copy(project.media.path(), moved, fs::copy_options::recursive);
		run(project.client, "project.load", {{"path", (moved / "project.dawproject").u8string()}});
		CHECK(fs::u8path(project.track(audio.track).at("frozen").at("path").get<std::string>()) ==
			moved / "Freeze" / "Ramp.wav");
	}
}

#ifndef _WIN32
TEST_CASE("a save fails on a freeze file that the file system can't say is there", "[integration]")
{
	Session project;
	const Session::Track audio = project.addTrack("Ramp");
	run(project.client, "track.freeze", {{"tracks", {audio.track}}, {"tail", 0}});
	const fs::path freeze = fs::u8path(project.track(audio.track).at("frozen").at("path").get<std::string>());
	run(project.client, "project.save", {{"embedMedia", false}});
	const fs::path file = project.media.path() / "project.dawproject";
	const std::string before = gnubase::test::readText(file);
	fs::permissions(freeze.parent_path(), fs::perms::none);
	std::error_code error;
	if (fs::exists(freeze, error) || !error)
	{
		fs::permissions(freeze.parent_path(), fs::perms::owner_all);
		SKIP("the folder can be searched here anyway, e.g. by root");
	}
	const Result failed = project.client.call("project.save", Json::object());
	fs::permissions(freeze.parent_path(), fs::perms::owner_all);
	CHECK(!failed.ok());
	CHECK_THAT(failed.error, ContainsSubstring("Ramp.wav"));
	CHECK(gnubase::test::readText(file) == before);
	CHECK(!fs::exists(fs::u8path(file.u8string() + ".saving")));
}
#endif

TEST_CASE("freeze files of the same name, also in another case, keep apart in the project file", "[integration]")
{
	Session project;
	const Session::Track one = project.addTrack("One");
	project.insertGain(one.channel, -6.0);
	const Session::Track two = project.addTrack("Two");
	const Session::Track three = project.addTrack("Three");
	run(project.client, "audio.render", {{"frames", 4096}});
	const Json frozen =
		run(project.client, "track.freeze", {{"tracks", {one.track, two.track, three.track}}, {"tail", 0}});
	const auto pathOf = [&frozen](size_t i)
	{ return fs::u8path(frozen.at("tracks").at(i).at("frozen").at("path").get<std::string>()); };

	// Two different files named Ramp.wav and ramp.wav, frozen in folders of their own, say before the project's first
	// save and after it; and the third track's, which isn't there any more, in the project's Freeze folder.
	run(project.client, "project.save", {{"embedMedia", false}});
	const fs::path saved = project.media.path() / "project.dawproject";
	const gnubase::test::TempDir first;
	const gnubase::test::TempDir second;
	fs::copy_file(pathOf(0), first.path() / "Ramp.wav");
	fs::copy_file(pathOf(1), second.path() / "ramp.wav");
	const std::vector<float> oneSamples = samplesOf((first.path() / "Ramp.wav").u8string());
	const std::vector<float> twoSamples = samplesOf((second.path() / "ramp.wav").u8string());
	REQUIRE(firstDifference(oneSamples, twoSamples) >= 0);
	const fs::path gone = project.media.path() / "Freeze" / "RAMP.wav";
	Project file = loadDawProject(saved.u8string(), (project.folder.path() / "media").u8string());
	const auto refer = [&file](ObjectId track, const fs::path& path)
	{
		std::optional<Freeze>& freeze = file.trackById.at(track).frozen;
		REQUIRE(freeze.has_value());
		if (freeze)
			freeze->path = path.u8string();
	};
	refer(one.track, first.path() / "Ramp.wav");
	refer(two.track, second.path() / "ramp.wav");
	refer(three.track, gone);
	DawProjectSaveOptions options;
	options.embedMedia = true;
	saveDawProject(file, saved.u8string(), options);

	run(project.client, "project.load", {{"path", saved.u8string()}});
	const auto loaded = [&project](ObjectId track)
	{ return project.track(track).at("frozen").at("path").get<std::string>(); };
	CHECK(firstDifference(samplesOf(loaded(one.track)), oneSamples) == -1);
	CHECK(firstDifference(samplesOf(loaded(two.track)), twoSamples) == -1);
	// The missing one is still referred to where it was, not to another track's file.
	CHECK(fs::u8path(loaded(three.track)) == gone);
}

TEST_CASE("embedded media files whose names differ only in case load from entries of their own (#871)", "[integration]")
{
	// Kick.wav and kick.wav from two folders: the file's entries are looked up in any case, so the save names them
	// apart in any case, as it does freeze files.
	Session project;
	const Session::Track track = project.addTrack("Drums");
	const gnubase::test::TempDir first;
	const gnubase::test::TempDir second;
	const std::vector<std::pair<fs::path, float>> files{
		{first.path() / "Kick.wav", 0.25f}, {second.path() / "kick.wav", -0.75f}};
	std::vector<ObjectId> media;
	for (const auto& [file, level] : files)
	{
		gnubase::test::writeWav(file, {std::vector<float>(1000, level), 1, 48000});
		media.push_back(
			run(project.client, "media.add", {{"path", file.u8string()}}).at("media").at("id").get<ObjectId>());
		const Json clip = run(project.client, "clip.add", {{"media", media.back()}}).at("clip");
		run(project.client, "audioEvent.add", {{"track", track.track}, {"clip", clip.at("id")}, {"position", 0}});
	}
	const fs::path saved = project.folder.path() / "kicks.dawproject";
	run(project.client, "project.save", {{"path", saved.u8string()}});
	std::map<std::string, std::string> entries = gnubase::test::readZip(saved.u8string());
	const std::string renamed = "audio/" + std::to_string(media[1]) + "-kick.wav";
	CHECK(entries.count("audio/Kick.wav") == 1);
	CHECK(entries.count(renamed) == 1);
	CHECK(entries.count("audio/kick.wav") == 0);

	SECTION("as this build saves them")
	{
	}

	SECTION("as earlier builds saved them, apart only in case")
	{
		// Those named the second one audio/kick.wav: each media file still loads from the entry of its exact name.
		REQUIRE(entries.count(renamed) == 1);
		entries["audio/kick.wav"] = entries.at(renamed);
		entries.erase(renamed);
		for (auto& [name, text] : entries)
		{
			if (name.rfind("audio/", 0) == 0)
				continue; // the files themselves; project.xml and gnubase.json refer to them
			for (size_t at = text.find(renamed); at != std::string::npos; at = text.find(renamed, at))
				text.replace(at, renamed.size(), "audio/kick.wav");
		}
		gnubase::test::writeZip(saved.u8string(), entries);
	}

	run(project.client, "project.load", {{"path", saved.u8string()}});
	const Json loaded = run(project.client, "project.get").at("media");
	for (size_t i = 0; i < media.size(); ++i)
	{
		const auto found =
			std::find_if(loaded.begin(), loaded.end(), [&](const Json& entry) { return entry.at("id") == media[i]; });
		REQUIRE(found != loaded.end());
		CHECK(samplesOf(found->at("path").get<std::string>()) == std::vector<float>(1000, files[i].second));
	}
}

TEST_CASE("embedded files that differ only in the case of a non-ASCII letter keep apart on load", "[integration]")
{
	Session project;
	const Session::Track one = project.addTrack("One");
	project.insertGain(one.channel, -6.0);
	const Session::Track two = project.addTrack("Two");
	run(project.client, "audio.render", {{"frames", 4096}});
	const Json frozen = run(project.client, "track.freeze", {{"tracks", {one.track, two.track}}, {"tail", 0}});
	const auto pathOf = [&frozen](size_t i)
	{ return fs::u8path(frozen.at("tracks").at(i).at("frozen").at("path").get<std::string>()); };

	// Übung.wav and übung.wav in two folders go into the file as two entries, which the zip tells apart, but the
	// file systems of Windows and macOS see as one name: loading must not extract the second over the first. On a
	// file system that keeps the names apart, as Linux's do, this passes also without that care. It holds for every
	// embedded file: two media files of those names, each played by an event, go in as audio/Übung.wav and
	// audio/übung.wav, as the save folds the case of ASCII letters only.
	const fs::path oneName = fs::u8path("Übung.wav");
	const fs::path twoName = fs::u8path("übung.wav");
	const gnubase::test::TempDir third;
	const gnubase::test::TempDir fourth;
	const auto addMedia = [&project](const fs::path& file, float level, ObjectId track)
	{
		gnubase::test::Audio audio;
		audio.channels = 2;
		audio.samples.assign(2000, level);
		gnubase::test::writeWav(file, audio);
		const Json added = run(project.client, "media.add", {{"path", file.u8string()}});
		const Json clip = run(project.client, "clip.add", {{"media", added.at("media").at("id")}});
		run(project.client, "audioEvent.add",
			{{"track", track}, {"clip", clip.at("clip").at("id")}, {"position", 20000}});
		return added.at("media").at("id").get<ObjectId>();
	};
	const ObjectId oneMedia = addMedia(third.path() / oneName, 0.25f, one.track);
	const ObjectId twoMedia = addMedia(fourth.path() / twoName, -0.75f, two.track);
	const std::vector<float> oneMediaSamples = samplesOf((third.path() / oneName).u8string());
	const std::vector<float> twoMediaSamples = samplesOf((fourth.path() / twoName).u8string());

	run(project.client, "project.save", {{"embedMedia", false}});
	const fs::path saved = project.media.path() / "project.dawproject";
	const gnubase::test::TempDir first;
	const gnubase::test::TempDir second;
	fs::copy_file(pathOf(0), first.path() / oneName);
	fs::copy_file(pathOf(1), second.path() / twoName);
	const std::vector<float> oneSamples = samplesOf((first.path() / oneName).u8string());
	const std::vector<float> twoSamples = samplesOf((second.path() / twoName).u8string());
	REQUIRE(firstDifference(oneSamples, twoSamples) >= 0);
	Project file = loadDawProject(saved.u8string(), (project.folder.path() / "media").u8string());
	const auto refer = [&file](ObjectId track, const fs::path& path)
	{
		std::optional<Freeze>& freeze = file.trackById.at(track).frozen;
		REQUIRE(freeze.has_value());
		if (freeze)
			freeze->path = path.u8string();
	};
	refer(one.track, first.path() / oneName);
	refer(two.track, second.path() / twoName);
	DawProjectSaveOptions options;
	options.embedMedia = true;
	saveDawProject(file, saved.u8string(), options);

	run(project.client, "project.load", {{"path", saved.u8string()}});
	const auto loaded = [&project](ObjectId track)
	{ return project.track(track).at("frozen").at("path").get<std::string>(); };
	CHECK(firstDifference(samplesOf(loaded(one.track)), oneSamples) == -1);
	CHECK(firstDifference(samplesOf(loaded(two.track)), twoSamples) == -1);
	const auto loadedMedia = [&project](ObjectId id)
	{
		std::string path;
		const Json current = run(project.client, "project.get");
		for (const Json& media : current.at("media"))
			if (media.at("id") == id)
				path = media.at("path").get<std::string>();
		REQUIRE(!path.empty());
		return path;
	};
	// Extracted next to the file, each from its own entry, which kept its name in the file: the second track's, which
	// loads later, is numbered only where the file system reads the two names as one.
	const fs::path extracted = project.media.path() / "project media" / "audio";
	const bool folds = fs::equivalent(extracted / oneName, extracted / twoName);
	CHECK(fs::u8path(loadedMedia(oneMedia)) == extracted / oneName);
	CHECK(fs::u8path(loadedMedia(twoMedia)) == extracted / (folds ? fs::u8path("2-übung.wav") : twoName));
	CHECK(firstDifference(samplesOf(loadedMedia(oneMedia)), oneMediaSamples) == -1);
	CHECK(firstDifference(samplesOf(loadedMedia(twoMedia)), twoMediaSamples) == -1);
}

TEST_CASE("a backup carries the freeze files along", "[integration]")
{
	Session project;
	const Session::Track audio = project.addTrack("Ramp");
	const Json frozen = run(project.client, "track.freeze", {{"tracks", {audio.track}}, {"tail", 0}});
	const std::vector<float> rendered = samplesOf(frozen.at("tracks").at(0).at("frozen").at("path").get<std::string>());
	// A duplicated frozen track plays the same freeze file, which the backup copies once.
	run(project.client, "track.duplicate", {{"track", audio.track}});
	const auto paths = [&project]
	{
		std::vector<std::string> result;
		const Json current = run(project.client, "project.get");
		for (const Json& track : current.at("tracks"))
			result.push_back(track.at("frozen").at("path").get<std::string>());
		return result;
	};
	const fs::path folder = project.folder.path() / "backup";
	const Json job = run(project.client, "project.backup", {{"folder", folder.u8string()}, {"name", "copy"}});
	Json state = waitForJob(project.client, job.at("job").at("id").get<uint64_t>());
	INFO(state.dump());
	REQUIRE(state.at("state") == "done");
	CHECK(state.at("result").at("missing").empty());
	CHECK(state.at("result").at("files") == 2); // the ramp and the freeze file

	run(project.client, "project.load", {{"path", (folder / "copy.dawproject").u8string()}});
	const std::string path = project.track(audio.track).at("frozen").at("path").get<std::string>();
	CHECK(fs::u8path(path) == folder / "Freeze" / "Ramp.wav");
	CHECK(paths() == std::vector<std::string>{path, path});
	CHECK(firstDifference(samplesOf(path), rendered) == -1);

	// A freeze file that can't be read is reported, and keeps its path.
	fs::remove(fs::u8path(path));
	const Json again =
		run(project.client, "project.backup", {{"folder", (project.folder.path() / "again").u8string()}});
	state = waitForJob(project.client, again.at("job").at("id").get<uint64_t>());
	REQUIRE(state.at("state") == "done");
	// Reported for each track that plays it.
	const Json missing = state.at("result").at("missing");
	REQUIRE(missing.size() == 2);
	CHECK(missing.at(0) == Json{{"track", audio.track}, {"path", path}});
	CHECK(missing.at(1).at("path") == path);
	CHECK(!fs::exists(project.folder.path() / "again" / "Freeze"));
}

TEST_CASE("media.prepareArchive copies freeze files from outside the project folder into its Freeze folder (#871)",
	"[integration]")
{
	Session project;
	const Session::Track audio = project.addTrack("Ramp");
	const Json frozen = run(project.client, "track.freeze", {{"tracks", {audio.track}}, {"tail", 0}});
	const std::string freeze = frozen.at("tracks").at(0).at("frozen").at("path").get<std::string>();
	const auto copy =
		run(project.client, "track.duplicate", {{"track", audio.track}}).at("track").at("id").get<ObjectId>();
	// Saved into another folder without its files, the project still plays the freeze file from the old one.
	const fs::path folder = project.folder.path() / "moved";
	fs::create_directories(folder);
	run(project.client, "project.save", {{"path", (folder / "moved.dawproject").u8string()}, {"embedMedia", false}});

	SECTION("for the tracks that play it")
	{
		const Json job = run(project.client, "media.prepareArchive").at("job");
		const Json done = waitForJob(project.client, job.at("id").get<uint64_t>());
		REQUIRE(done.at("state") == "done");
		// Copied once for the two tracks that play it, which play the copy now, as one undo step with the media's.
		const std::string copied = (folder / "Freeze" / fs::u8path(freeze).filename()).u8string();
		CHECK(done.at("result").at("copied").at(1) ==
			Json{{"tracks", {audio.track, copy}}, {"from", freeze}, {"to", copied}});
		CHECK(project.track(audio.track).at("frozen").at("path") == copied);
		CHECK(project.track(copy).at("frozen").at("path") == copied);
		CHECK(firstDifference(samplesOf(copied), samplesOf(freeze)) == -1);
		run(project.client, "history.undo");
		CHECK(project.track(audio.track).at("frozen").at("path") == freeze);
		CHECK(project.track(copy).at("frozen").at("path") == freeze);
	}

	SECTION("but not for a track unfrozen meanwhile")
	{
		const Json done =
			gnubase::test::whileHeld(project.engine, project.client, "media.prepareArchive", Json::object(),
				[&project, copy](uint64_t) { run(project.client, "track.unfreeze", {{"tracks", {copy}}}); });
		REQUIRE(done.at("state") == "done");
		const Json copied = done.at("result").at("copied").at(1);
		CHECK(copied.at("tracks") == Json::array({audio.track}));
		CHECK(done.at("result").at("changedTracks") == Json::array({copy}));
		CHECK(project.track(audio.track).at("frozen").at("path") == copied.at("to"));
	}

	SECTION("but removes the copy that no track plays any more")
	{
		const Json done = gnubase::test::whileHeld(project.engine, project.client, "media.prepareArchive",
			Json::object(), [&project, &audio, copy](uint64_t)
			{ run(project.client, "track.unfreeze", {{"tracks", {audio.track, copy}}}); });
		REQUIRE(done.at("state") == "done");
		CHECK(done.at("result").at("copied").size() == 1); // the media file
		CHECK(done.at("result").at("changedTracks") == Json::array({audio.track, copy}));
		CHECK(!fs::exists(folder / "Freeze" / fs::u8path(freeze).filename()));
	}

	SECTION("and lists one it can't read as missing, whose tracks keep its path")
	{
		fs::remove(fs::u8path(freeze));
		const Json job = run(project.client, "media.prepareArchive").at("job");
		const Json done = waitForJob(project.client, job.at("id").get<uint64_t>());
		REQUIRE(done.at("state") == "done");
		CHECK(done.at("result").at("missing") == Json::array({{{"tracks", {audio.track, copy}}, {"path", freeze}}}));
		CHECK(done.at("result").at("copied").size() == 1);
		CHECK(project.track(audio.track).at("frozen").at("path") == freeze);
		CHECK(project.track(copy).at("frozen").at("path") == freeze);
	}

	SECTION("but not with media, which limits it to those media files")
	{
		const Json media = run(project.client, "project.get").at("media");
		REQUIRE(media.size() == 1);
		const Json job = run(project.client, "media.prepareArchive", {{"media", {media.at(0).at("id")}}}).at("job");
		const Json done = waitForJob(project.client, job.at("id").get<uint64_t>());
		REQUIRE(done.at("state") == "done");
		CHECK(done.at("result").at("copied").size() == 1);
		CHECK(done.at("result").at("copied").at(0).contains("media"));
		CHECK(project.track(audio.track).at("frozen").at("path") == freeze);
		CHECK(!fs::exists(folder / "Freeze"));
	}
}

TEST_CASE("track.relinkFreeze skips what it can't relink (#871)", "[integration]")
{
	Session project;
	const Session::Track audio = project.addTrack("Ramp");
	const Session::Track loose = project.addTrack("Loose");
	const Json frozen = run(project.client, "track.freeze", {{"tracks", {audio.track}}, {"tail", 0}});
	const std::string freeze = frozen.at("tracks").at(0).at("frozen").at("path").get<std::string>();
	const fs::path copy = project.folder.path() / "copy.wav";
	fs::copy_file(fs::u8path(freeze), copy);
	const fs::path mono = project.folder.path() / "mono.wav";
	gnubase::test::writeWav(mono, {std::vector<float>(100, 0.5f), 1, 48000});
	const Json skipped = run(project.client, "track.relinkFreeze",
		{{"files",
			{{{"track", audio.track}, {"from", "elsewhere.wav"}, {"path", copy.u8string()}},
				{{"track", audio.track}, {"from", freeze}, {"path", mono.u8string()}},
				{{"track", audio.track}, {"from", freeze}, {"path", (project.folder.path() / "gone.wav").u8string()}},
				{{"track", loose.track}, {"from", freeze}, {"path", copy.u8string()}},
				{{"track", 9999}, {"from", freeze}, {"path", copy.u8string()}}}}});
	CHECK(skipped.at("relinked").empty());
	CHECK(skipped.at("skipped") == Json::array({audio.track, audio.track, audio.track, loose.track, 9999}));
	const Json entry = {{"track", audio.track}, {"from", freeze}, {"path", copy.u8string()}};
	CHECK(run(project.client, "track.relinkFreeze", {{"files", {entry}}, {"project", 9999}}).at("relinked").empty());
	CHECK(run(project.client, "track.relinkFreeze", {{"files", {entry}}}).at("relinked") == Json::array({audio.track}));
	CHECK(project.track(audio.track).at("frozen").at("path") == copy.u8string());
}

TEST_CASE(
	"the first save of a project copies its freeze files into the project's Freeze folder (#871)", "[integration]")
{
	const gnubase::test::TempDir dir;
	Engine engine(gnubase::audio::makeBackends(), dir.path() / "settings");
	Client client(engine);
	run(client, "audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}, {"captureSeconds", 1}});
	gnubase::test::writeWav(dir.path() / "tone.wav", {std::vector<float>(4800, 0.5f), 1, 48000});
	const auto track =
		run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id").get<ObjectId>();
	const Json media = run(client, "media.add", {{"path", (dir.path() / "tone.wav").u8string()}}).at("media");
	const Json clip = run(client, "clip.add", {{"media", media.at("id")}}).at("clip");
	run(client, "audioEvent.add", {{"track", track}, {"clip", clip.at("id")}, {"position", 0}});
	const Json frozen = run(client, "track.freeze", {{"tracks", {track}}, {"tail", 0}});
	const fs::path freeze = fs::u8path(frozen.at("tracks").at(0).at("frozen").at("path").get<std::string>());
	CHECK(freeze.parent_path() == dir.path() / "settings" / "Recordings" / "Freeze");

	const Json saved = run(client, "project.save", {{"path", (dir.path() / "Song.dawproject").u8string()}});
	REQUIRE(saved.at("job").is_object());
	REQUIRE(waitForJob(client, saved.at("job").at("id").get<uint64_t>()).at("state") == "done");
	const Json project = run(client, "project.get");
	CHECK(project.at("tracks").at(0).at("frozen").at("path") == (dir.path() / "Freeze" / freeze.filename()).u8string());
	CHECK(project.at("media").at(0).at("path") == (dir.path() / "tone.wav").u8string()); // not in Recordings: it stays
}

TEST_CASE(
	"freezing and render in place refuse an unsaved project without a folder to write into (#2001)", "[integration]")
{
	const gnubase::test::TempDir dir;
	const fs::path file = dir.path() / "not a folder";
	REQUIRE(std::ofstream(file) << "a file");
	gnubase::test::writeWav(dir.path() / "tone.wav", {std::vector<float>(4800, 0.5f), 1, 48000});
	const gnubase::test::TempFolderAt temporary(file); // and there is no settings folder
	Engine engine;
	Client client(engine);
	run(client, "audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}, {"captureSeconds", 1}});
	const auto track =
		run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track").at("id").get<ObjectId>();
	const Json media = run(client, "media.add", {{"path", (dir.path() / "tone.wav").u8string()}}).at("media");
	const Json clip = run(client, "clip.add", {{"media", media.at("id")}}).at("clip");
	run(client, "audioEvent.add", {{"track", track}, {"clip", clip.at("id")}, {"position", 0}});
	const Json before = run(client, "project.get");
	CHECK_THAT(reject(client, "track.freeze", {{"tracks", {track}}, {"tail", 0}}), ContainsSubstring("no folder"));
	CHECK_THAT(reject(client, "track.renderInPlace", {{"tracks", {track}}}), ContainsSubstring("no folder"));
	CHECK(run(client, "project.get") == before);
	CHECK(!fs::exists(fs::current_path() / "Freeze"));
}

TEST_CASE("a cancelled backup leaves no Freeze folder behind (#871)", "[integration]")
{
	Session project;
	const Session::Track synth = project.addSynth();
	run(project.client, "track.freeze", {{"tracks", {synth.track}}, {"tail", 0}});
	// With no media to copy (no event plays the ramp), the backup's first progress is the freeze file's, before it
	// makes the Freeze folder: held there and cancelled, it stops once it has made it.
	const fs::path folder = project.folder.path() / "backup";
	const Json cancelled = gnubase::test::whileHeld(project.engine, project.client, "project.backup",
		{{"folder", folder.u8string()}, {"removeUnused", true}},
		[&project](uint64_t job) { run(project.client, "job.cancel", {{"job", job}}); });
	CHECK(cancelled.at("state") == "cancelled");
	CHECK(!fs::exists(folder));
}

TEST_CASE("a frozen track neither monitors nor records", "[integration]")
{
	Session project;
	const Session::Track audio = project.addTrack("Ramp");
	const Session::Track other = project.addTrack("Other");
	const auto buttons = [&project](ObjectId channel) { return project.buttons(channel); };
	run(project.client, "channel.set", {{"channel", audio.channel}, {"monitor", true}, {"recordEnable", true}});

	// Freezing turns both buttons off in the same step, and they can't be turned on again while it is frozen.
	run(project.client, "track.freeze", {{"tracks", {audio.track}}, {"tail", 0}});
	CHECK(buttons(audio.channel) == std::pair{false, false});
	CHECK_THAT(reject(project.client, "channel.set", {{"channel", audio.channel}, {"monitor", true}}),
		ContainsSubstring("frozen"));
	CHECK_THAT(reject(project.client, "channel.set", {{"channel", audio.channel}, {"recordEnable", true}}),
		ContainsSubstring("frozen"));

	// A channel linked to it doesn't pass them on to it.
	run(project.client, "linkGroup.add", {{"channels", {audio.channel, other.channel}}, {"record", true}});
	run(project.client, "channel.set", {{"channel", other.channel}, {"monitor", true}, {"recordEnable", true}});
	CHECK(buttons(other.channel) == std::pair{true, true});
	CHECK(buttons(audio.channel) == std::pair{false, false});

	// Undoing the freeze brings them back.
	for (int step = 0; step < 3; ++step) // the buttons of the other channel, the link group and the freeze
		run(project.client, "history.undo");
	CHECK(project.track(audio.track).at("frozen").is_null());
	CHECK(buttons(audio.channel) == std::pair{true, true});

	SECTION("nor through a VCA fader")
	{
		run(project.client, "track.freeze", {{"tracks", {audio.track}}, {"tail", 0}});
		const auto vca = run(project.client, "track.add", {{"type", "vca"}}).at("channel").at("id").get<ObjectId>();
		run(project.client, "linkGroup.add", {{"channels", {audio.channel, other.channel}}, {"vca", vca}});
		run(project.client, "channel.set", {{"channel", vca}, {"monitor", true}, {"recordEnable", true}});
		CHECK(buttons(other.channel) == std::pair{true, true});
		CHECK(buttons(audio.channel) == std::pair{false, false});
	}

	SECTION("nor when a file says it does")
	{
		// A file from a build that let a frozen track keep its buttons: they are off when it loads.
		run(project.client, "track.freeze", {{"tracks", {audio.track}}, {"tail", 0}});
		run(project.client, "project.save", {{"embedMedia", false}});
		const fs::path saved = project.media.path() / "project.dawproject";
		Project file = loadDawProject(saved.u8string(), (project.folder.path() / "media").u8string());
		file.channels.at(audio.channel).monitor = true;
		file.channels.at(audio.channel).recordEnable = true;
		DawProjectSaveOptions options;
		options.embedMedia = false;
		saveDawProject(file, saved.u8string(), options);
		run(project.client, "project.load", {{"path", saved.u8string()}});
		CHECK(!project.track(audio.track).at("frozen").is_null());
		CHECK(buttons(audio.channel) == std::pair{false, false});
	}
}

TEST_CASE("a MIDI track that plays a frozen instrument track neither monitors nor records", "[integration]")
{
	Session project;
	const Session::Track synth = project.addSynth();
	const Json added = run(project.client, "track.add", {{"type", "midi"}, {"name", "Keys"}});
	const auto keys = added.at("channel").at("id").get<ObjectId>();
	const auto loose = run(project.client, "track.add", {{"type", "midi"}}).at("channel").at("id").get<ObjectId>();
	run(project.client, "channel.set", {{"channel", keys}, {"output", synth.channel}, {"recordEnable", true}});
	run(project.client, "channel.set", {{"channel", loose}, {"monitor", true}});

	// The freeze file plays its notes instead of the instrument.
	run(project.client, "track.freeze", {{"tracks", {synth.track}}, {"tail", 0}});
	CHECK(project.buttons(keys) == std::pair{false, false});
	CHECK(project.buttons(loose) == std::pair{true, false}); // it plays no frozen track
	CHECK_THAT(reject(project.client, "channel.set", {{"channel", keys}, {"recordEnable", true}}),
		ContainsSubstring("frozen"));
	// Nor does a MIDI track go to it with its buttons on.
	CHECK_THAT(reject(project.client, "channel.set", {{"channel", loose}, {"output", synth.channel}}),
		ContainsSubstring("frozen"));
	run(project.client, "channel.set", {{"channel", loose}, {"monitor", false}, {"output", synth.channel}});

	run(project.client, "track.unfreeze", {{"tracks", {synth.track}}});
	run(project.client, "channel.set", {{"channel", keys}, {"recordEnable", true}});
	CHECK(project.buttons(keys) == std::pair{false, true});
}

TEST_CASE("freezing refuses what it can't freeze", "[integration]")
{
	Session project;
	const Session::Track audio = project.addTrack("Ramp");
	const auto midi = run(project.client, "track.add", {{"type", "midi"}}).at("track").at("id").get<ObjectId>();
	const auto empty = run(project.client, "track.add", {{"type", "audio"}}).at("track").at("id").get<ObjectId>();
	CHECK_THAT(reject(project.client, "track.freeze", {{"tracks", {midi}}}), ContainsSubstring("audio and instrument"));
	CHECK_THAT(reject(project.client, "track.freeze", {{"tracks", {empty}}}), ContainsSubstring("nothing"));
	CHECK_THAT(
		reject(project.client, "track.freeze", {{"tracks", {audio.track, audio.track}}}), ContainsSubstring("twice"));
	CHECK_THAT(
		reject(project.client, "track.unfreeze", {{"tracks", {audio.track}}}), ContainsSubstring("isn't frozen"));
	run(project.client, "track.freeze", {{"tracks", {audio.track}}, {"tail", 0}});
	CHECK_THAT(
		reject(project.client, "track.freeze", {{"tracks", {audio.track}}}), ContainsSubstring("frozen already"));
}

TEST_CASE("render in place puts what a track plays on a new audio track", "[integration]")
{
	Session project;
	const Session::Track mono = project.addTrack("Mono", 1);
	const ObjectId gain = project.insertGain(mono.channel, -6.0);
	run(project.client, "channel.set", {{"channel", mono.channel}, {"volume", -3.0}, {"pan", -0.5}});
	run(project.client, "audio.render", {{"frames", 4096}});
	const std::vector<float> live = project.mainMix();

	SECTION("with its channel settings")
	{
		const Json result =
			run(project.client, "track.renderInPlace", {{"tracks", {mono.track}}, {"sourceTracks", "mute"}});
		REQUIRE(result.at("tracks").size() == 1);
		const ObjectId rendered = result.at("tracks").at(0).get<ObjectId>();
		const Json state = run(project.client, "project.get");
		// Below the track, as a mono track with the fader and pan, playing one event from where the track's starts.
		std::vector<ObjectId> order;
		for (const Json& track : state.at("tracks"))
			order.push_back(track.at("id").get<ObjectId>());
		REQUIRE(order.size() == 2);
		CHECK(order[1] == rendered);
		const Json track = project.track(rendered);
		CHECK(track.at("name") == "Mono (Rendered)");
		CHECK(track.at("type") == "audio");
		for (const Json& channel : state.at("channels"))
			if (channel.at("track") == rendered)
			{
				CHECK(channel.at("format") == "mono");
				CHECK(channel.at("volume") == -3.0);
				CHECK(channel.at("pan") == -0.5);
				CHECK(channel.at("slots").empty());
			}
		for (const Json& event : state.at("audioEvents"))
			if (event.at("id") == result.at("events").at(0))
				CHECK(event.at("position") == 1000);
		CHECK(project.track(mono.track).at("enabled") == true);
		run(project.client, "audio.render", {{"frames", 4096}}); // the mute glides there
		CHECK(firstDifference(project.mainMix(), live) == -1);
		run(project.client, "history.undo");
		CHECK(run(project.client, "project.get").at("tracks").size() == 1);
	}

	SECTION("through its complete signal path")
	{
		const Json result = run(project.client, "track.renderInPlace",
			{{"tracks", {mono.track}}, {"processing", "completeSignalPath"}, {"sourceTracks", "remove"},
				{"name", "printed"}});
		const Json state = run(project.client, "project.get");
		REQUIRE(state.at("tracks").size() == 1);
		for (const Json& channel : state.at("channels"))
		{
			CHECK(channel.at("format") == "stereo"); // the panner's output
			CHECK(channel.at("volume") == 0.0);
		}
		for (const Json& media : state.at("media"))
			if (media.at("id") == result.at("media").at(0))
				CHECK(
					fs::u8path(media.at("path").get<std::string>()) == project.media.path() / "Audio" / "printed.wav");
		CHECK(firstDifference(project.mainMix(), live) == -1);
	}

	SECTION("dry, with a range")
	{
		const Json result = run(project.client, "track.renderInPlace",
			{{"tracks", {mono.track}}, {"processing", "dry"}, {"start", 2000}, {"end", 3000},
				{"sourceTracks", "disable"}});
		CHECK(project.track(mono.track).at("enabled") == false);
		std::string path;
		const Json state = run(project.client, "project.get");
		for (const Json& media : state.at("media"))
			if (media.at("id") == result.at("media").at(0))
				path = media.at("path").get<std::string>();
		const std::vector<float> dry = samplesOf(path);
		REQUIRE(dry.size() == 1000);
		CHECK(dry[0] == 1001.0f / 8192.0f); // the ramp's left channel, from frame 1000 of the event
		// A copy of the plugin plays on the new track.
		CHECK(project.slotNodes() == 2);
		const Json copy = run(project.client, "plugin.get", {{"slot", state.at("slots").back().at("id")}});
		CHECK(copy.at("slot").at("id") != gain);
		CHECK(copy.at("slot").at("plugin") == "builtin:gainPan");
	}
}

TEST_CASE("render in place separates events or combines adjacent blocks on one destination track", "[integration]")
{
	Session project;
	const auto source = project.addTrack("Mono", 1);
	const Json initial = run(project.client, "project.get").at("audioEvents").at(0).at("id");
	run(project.client, "audioEvent.set", {{"event", initial}, {"length", 1000}});
	for (const int64_t position : {2000, 5000})
		run(project.client, "audioEvent.add",
			{{"track", source.track}, {"clip", project.clip}, {"position", position}, {"length", 1000}});
	const std::string mode = GENERATE("separateEvents", "blockEvents", "oneEvent");
	const Json rendered = run(project.client, "track.renderInPlace",
		{{"tracks", {source.track}}, {"mode", mode}, {"sourceTracks", "disable"}});
	REQUIRE(rendered.at("tracks").size() == 1);
	std::vector<int64_t> expectedPositions{1000};
	if (mode == "separateEvents")
		expectedPositions = {1000, 2000, 5000};
	else if (mode == "blockEvents")
		expectedPositions = {1000, 5000};
	const size_t count = expectedPositions.size();
	REQUIRE(rendered.at("events").size() == count);
	REQUIRE(rendered.at("media").size() == count);
	const Json state = run(project.client, "project.get");
	std::vector<int64_t> positions;
	for (const auto& event : state.at("audioEvents"))
		if (event.at("track") == rendered.at("tracks").at(0))
			positions.push_back(event.at("position").get<int64_t>());
	CHECK(positions == expectedPositions);
	std::set<std::string> paths;
	for (const auto& media : state.at("media"))
		if (std::find(rendered.at("media").begin(), rendered.at("media").end(), media.at("id")) !=
			rendered.at("media").end())
		{
			paths.insert(media.at("path").get<std::string>());
			const auto samples = samplesOf(media.at("path").get<std::string>());
			CHECK(samples.front() == 1.0f / 8192.0f);
			size_t expectedLength = 1000;
			if (mode == "oneEvent")
				expectedLength = 5000;
			else if (mode == "blockEvents" && media.at("id") == rendered.at("media").at(0))
				expectedLength = 2000;
			CHECK(samples.size() == expectedLength);
		}
	CHECK(paths.size() == count);
	CHECK(project.track(source.track).at("enabled") == false);
	run(project.client, "history.undo");
	CHECK(run(project.client, "project.get").at("tracks").size() == 1);
	CHECK(project.track(source.track).at("enabled") == true);
	run(project.client, "history.redo");
	CHECK(run(project.client, "project.get").at("tracks").size() == 2);
}

TEST_CASE("render in place renders only selected audio events even under an overlapping event", "[integration]")
{
	Session project;
	const auto source = project.addTrack("Mono", 1);
	const Json selected = run(project.client, "project.get").at("audioEvents").at(0).at("id");
	run(project.client, "audioEvent.add",
		{{"track", source.track}, {"clip", project.clip}, {"position", 1000}, {"offset", 2000}, {"length", 1000}});
	const Json rendered = run(project.client, "track.renderInPlace",
		{{"tracks", {source.track}}, {"events", {selected}}, {"start", 1000}, {"end", 2000}});
	const Json state = run(project.client, "project.get");
	for (const auto& media : state.at("media"))
		if (media.at("id") == rendered.at("media").at(0))
		{
			const auto samples = samplesOf(media.at("path").get<std::string>());
			REQUIRE(samples.size() == 1000);
			CHECK(samples.front() == 1.0f / 8192.0f);
			CHECK(samples.back() == 1000.0f / 8192.0f);
		}
	CHECK(state.at("audioEvents").size() == 3);
}

TEST_CASE("render in place mutes the source events and hides the source tracks it is told to", "[integration]")
{
	// Mute Source Events and Hide Source Tracks (reference manual pp. 1473, 1476). The mute is in the render's undo
	// step; what the project window hides is a setting, not an undo step (trackVisibility.set), so undo leaves it.
	Session project;
	const Session::Track source = project.addTrack("Ramp");
	const Json event = run(project.client, "project.get").at("audioEvents").at(0).at("id");
	CHECK_THAT(reject(project.client, "track.renderInPlace", {{"tracks", {source.track}}, {"sourceEvents", "mute"}}),
		ContainsSubstring("events"));
	const Json rendered = run(project.client, "track.renderInPlace",
		{{"tracks", {source.track}}, {"events", {event}}, {"sourceEvents", "mute"}, {"hideSourceTracks", true}});
	const auto muted = [&project, &event]
	{
		bool found = false;
		const Json state = run(project.client, "project.get");
		for (const Json& each : state.at("audioEvents"))
			if (each.at("id") == event)
				found = each.at("mute").get<bool>();
		return found;
	};
	CHECK(muted());
	CHECK(run(project.client, "trackVisibility.get").at("hidden") == Json::array({source.track}));
	CHECK(rendered.at("tracks").size() == 1);
	run(project.client, "history.undo");
	CHECK_FALSE(muted());
	CHECK(run(project.client, "trackVisibility.get").at("hidden") == Json::array({source.track}));
}

TEST_CASE("hiding the source tracks still works once a hidden track has been removed", "[integration]")
{
	// The stored list keeps the removed track for undo, which trackVisibility.set refuses: the render leaves it out.
	Session project;
	const Session::Track gone = project.addTrack("Ramp");
	const Session::Track source = project.addTrack("Ramp");
	run(project.client, "trackVisibility.set", {{"hidden", {gone.track}}});
	run(project.client, "track.remove", {{"track", gone.track}});
	const Json rendered =
		run(project.client, "track.renderInPlace", {{"tracks", {source.track}}, {"hideSourceTracks", true}});
	CHECK(rendered.at("tracks").size() == 1);
	CHECK(run(project.client, "trackVisibility.get").at("hidden") == Json::array({source.track}));
}

TEST_CASE("render in place rejects invalid event selections without starting a job", "[integration]")
{
	Session project;
	const auto source = project.addTrack("Mono", 1);
	const auto other = project.addTrack("Other", 1);
	const Json selected = run(project.client, "project.get").at("audioEvents").at(0).at("id");
	for (const Json& events : {Json::array(), Json::array({999999}), Json::array({selected, selected})})
		CHECK(!project.client.call("track.renderInPlace", {{"tracks", {source.track}}, {"events", events}}).ok());
	CHECK_THAT(reject(project.client, "track.renderInPlace", {{"tracks", {other.track}}, {"events", {selected}}}),
		ContainsSubstring("selected tracks"));
	CHECK(run(project.client, "track.renderLocks").at("locks").empty());
}

TEST_CASE("render in place isolates selected MIDI parts and uses their whole boundaries", "[integration]")
{
	Session project;
	const auto synth = project.addSynth();
	const Json part = run(project.client, "project.get").at("midiParts").at(0).at("id");
	const Json expected = run(project.client, "track.renderInPlace",
		{{"tracks", {synth.track}}, {"events", {part}}, {"mode", "separateEvents"}});
	std::vector<float> original;
	const Json before = run(project.client, "project.get");
	for (const auto& media : before.at("media"))
		if (media.at("id") == expected.at("media").at(0))
			original = samplesOf(media.at("path").get<std::string>());
	REQUIRE(original.size() == size_t{48000} * 2);
	run(project.client, "history.undo");
	const Json other = run(project.client, "midiPart.add", {{"track", synth.track}, {"position", 0}, {"length", 48000}})
						   .at("part")
						   .at("id");
	run(project.client, "note.add",
		{{"part", other}, {"notes", {{{"position", 0}, {"length", 1720320}, {"pitch", 36}, {"velocity", 1.0}}}}});
	const Json rendered = run(project.client, "track.renderInPlace", {{"tracks", {synth.track}}, {"events", {part}}});
	const Json after = run(project.client, "project.get");
	for (const auto& media : after.at("media"))
		if (media.at("id") == rendered.at("media").at(0))
			CHECK(firstDifference(samplesOf(media.at("path").get<std::string>()), original) == -1);
}

TEST_CASE("separate and block renders keep the parts feeding an instrument through a MIDI send", "[integration]")
{
	Session project;
	const auto synth = project.addSynth();
	const Json midi =
		run(project.client, "track.add", {{"type", "midi"}, {"timeBase", "linear"}, {"output", synth.channel}});
	for (const int64_t position : {0, 24000})
	{
		const Json part = run(project.client, "midiPart.add",
			{{"track", midi.at("track").at("id")}, {"position", position}, {"length", 12000}})
							  .at("part")
							  .at("id");
		run(project.client, "note.add",
			{{"part", part}, {"notes", {{{"position", 0}, {"length", 430080}, {"pitch", 36}, {"velocity", 0.9}}}}});
	}
	const std::string mode = GENERATE("separateEvents", "blockEvents");
	const auto audioOf = [&](const Json& result)
	{
		const Json state = run(project.client, "project.get");
		std::vector<std::vector<float>> audio;
		for (const auto& id : result.at("media"))
		{
			const auto found = std::find_if(state.at("media").begin(), state.at("media").end(),
				[&](const Json& media) { return media.at("id") == id; });
			REQUIRE(found != state.at("media").end());
			audio.push_back(samplesOf(found->at("path").get<std::string>()));
			CHECK(peakOf(audio.back()) > 0.01f);
		}
		return audio;
	};
	// Direct routing provides the expected files: one per part, or one block including the overlapping send parts.
	const Json baseline = run(project.client, "track.renderInPlace", {{"tracks", {synth.track}}, {"mode", mode}});
	REQUIRE(baseline.at("media").size() == (mode == "separateEvents" ? size_t{3} : size_t{1}));
	const auto expected = audioOf(baseline);
	run(project.client, "history.undo");
	run(project.client, "channel.set",
		{{"channel", midi.at("channel").at("id")}, {"output", 0},
			{"midiSends",
				{{"slots",
					{{{"effect", "transformer"}, {"parameters", {{"value", 0}}}, {"output", synth.channel}}}}}}});
	const Json rendered = run(project.client, "track.renderInPlace", {{"tracks", {synth.track}}, {"mode", mode}});
	REQUIRE(rendered.at("tracks").size() == 1);
	const auto actual = audioOf(rendered);
	REQUIRE(actual.size() == expected.size());
	for (size_t i = 0; i < expected.size(); ++i)
		CHECK(firstDifference(actual[i], expected[i]) == -1);
}

TEST_CASE("separate renders exclude later events from their tail and remove each source once", "[integration]")
{
	Session project;
	const auto source = project.addTrack("Mono", 1);
	const Json first = run(project.client, "project.get").at("audioEvents").at(0).at("id");
	run(project.client, "audioEvent.set", {{"event", first}, {"length", 1000}});
	run(project.client, "audioEvent.add",
		{{"track", source.track}, {"clip", project.clip}, {"position", 3000}, {"length", 1000}});
	const Json rendered = run(project.client, "track.renderInPlace",
		{{"tracks", {source.track}}, {"mode", "separateEvents"}, {"tail", 0.1}, {"sourceTracks", "remove"}});
	REQUIRE(rendered.at("tracks").size() == 1);
	REQUIRE(rendered.at("media").size() == 2);
	const Json state = run(project.client, "project.get");
	CHECK(state.at("tracks").size() == 1);
	for (const auto& media : state.at("media"))
		if (media.at("id") == rendered.at("media").at(0))
			CHECK(samplesOf(media.at("path").get<std::string>()).size() == 1000);
	run(project.client, "history.undo");
	CHECK(project.track(source.track).at("name") == "Mono");
}

TEST_CASE("separate dry renders copy the source effects once and preserve the track's audio", "[integration]")
{
	Session project;
	const auto source = project.addTrack("Mono", 1);
	run(project.client, "audioEvent.add",
		{{"track", source.track}, {"clip", project.clip}, {"position", 10000}, {"length", 1000}});
	project.insertGain(source.channel, -6.0);
	const auto expected = project.mainMix();
	const Json rendered = run(project.client, "track.renderInPlace",
		{{"tracks", {source.track}}, {"mode", "separateEvents"}, {"processing", "dry"}, {"sourceTracks", "remove"}});
	REQUIRE(rendered.at("tracks").size() == 1);
	REQUIRE(rendered.at("events").size() == 2);
	const Json state = run(project.client, "project.get");
	REQUIRE(state.at("channels").size() == 1);
	CHECK(state.at("slots").size() == 1);
	project.activate({state.at("channels").at(0).at("id").get<ObjectId>()});
	CHECK(firstDifference(project.mainMix(), expected) == -1);
}

TEST_CASE("a failed second event render removes the first file and releases every lock", "[integration]")
{
	Session project;
	const auto source = project.addTrack("Mono", 1);
	run(project.client, "audioEvent.add",
		{{"track", source.track}, {"clip", project.clip}, {"position", 10000}, {"length", 1000}});
	project.insertGain(source.channel, -6.0);
	const auto made = std::make_shared<std::atomic<int>>(0);
	project.engine.setRenderPluginFactory(
		[made](const std::string& plugin, uint32_t channels,
			gnubase::plugin::Host& host) -> std::unique_ptr<gnubase::plugin::Plugin>
		{
			if (++*made == 2)
				return nullptr;
			return processors::BuiltinFactory(channels).create(plugin, host);
		});
	const Json before = run(project.client, "project.get");
	const Json history = run(project.client, "history.get");
	CHECK_THAT(reject(project.client, "track.renderInPlace", {{"tracks", {source.track}}, {"mode", "separateEvents"}}),
		ContainsSubstring("plugin"));
	CHECK(made->load() == 2);
	CHECK(run(project.client, "project.get") == before);
	CHECK(run(project.client, "history.get") == history);
	CHECK(run(project.client, "track.renderLocks").at("locks").empty());
	CHECK(fs::directory_iterator(project.media.path() / "Audio") == fs::directory_iterator());
}

TEST_CASE("a rendered event under a transpose event is independent of it", "[integration]")
{
	// The render holds the transposition: following the transpose track, it would be transposed again (#1368).
	Session project;
	const Session::Track mono = project.addTrack("Mono", 1);
	run(project.client, "transpose.add", {{"position", 0}, {"value", 5}});
	const Json result =
		run(project.client, "track.renderInPlace", {{"tracks", {mono.track}}, {"sourceTracks", "mute"}});
	const Json rendered = result.at("events").at(0);
	const Json state = run(project.client, "project.get");
	for (const Json& event : state.at("audioEvents"))
		if (event.at("id") == rendered)
			CHECK(event.at("globalTranspose") == "independent");
	const RenderSnapshot& snapshot = project.engine.renderSnapshots().acquire();
	bool found = false;
	for (const RenderChannel& channel : snapshot.channels)
		for (const RenderAudioEvent& event : channel.audioEvents)
			if (event.id == rendered.get<ObjectId>())
			{
				found = true;
				CHECK(snapshot.media.at(static_cast<size_t>(event.media)).pitch.empty());
			}
	CHECK(found);
}

TEST_CASE("a rendered event without transpose events gets the project root key and follows it", "[integration]")
{
	// As a recorded event (#1368, reference manual p. 420): changing the root key afterwards shifts the render.
	Session project;
	const Session::Track mono = project.addTrack("Mono", 1);
	run(project.client, "project.set", {{"rootKey", "D"}});
	const Json result =
		run(project.client, "track.renderInPlace", {{"tracks", {mono.track}}, {"sourceTracks", "mute"}});
	const auto rendered = result.at("events").at(0).get<ObjectId>();
	bool found = false;
	const Json state = run(project.client, "project.get");
	for (const Json& event : state.at("audioEvents"))
		if (event.at("id") == rendered)
		{
			found = true;
			CHECK(event.at("rootKey") == "D");
			CHECK(event.at("globalTranspose") == "follow");
		}
	CHECK(found);
	using Pitch = std::vector<dsp::PitchPoint>;
	const auto pitch = [&project, rendered]() -> std::optional<Pitch>
	{
		const RenderSnapshot& snapshot = project.engine.renderSnapshots().acquire();
		for (const RenderChannel& channel : snapshot.channels)
			for (const RenderAudioEvent& event : channel.audioEvents)
				if (event.id == rendered)
					return snapshot.media.at(static_cast<size_t>(event.media)).pitch;
		return std::nullopt;
	};
	CHECK(pitch() == Pitch{});
	run(project.client, "project.set", {{"rootKey", "E"}});
	CHECK(pitch() == Pitch{{0.0, 2.0}});
}

TEST_CASE("render in place refuses what it can't render", "[integration]")
{
	Session project;
	const Session::Track audio = project.addTrack("Ramp");
	CHECK_THAT(reject(project.client, "track.renderInPlace", {{"tracks", {audio.track}}, {"name", ""}}),
		ContainsSubstring("parameters.name must be at least 1 character"));
	const auto empty = run(project.client, "track.add", {{"type", "audio"}}).at("track").at("id").get<ObjectId>();
	CHECK_THAT(reject(project.client, "track.renderInPlace", {{"tracks", {audio.track}}, {"start", 0}}),
		ContainsSubstring("both"));
	CHECK_THAT(reject(project.client, "track.renderInPlace", {{"tracks", {audio.track}}, {"start", 10}, {"end", 10}}),
		ContainsSubstring("empty"));
	CHECK_THAT(reject(project.client, "track.renderInPlace", {{"tracks", {empty}}}), ContainsSubstring("nothing"));

	// The complete signal path ends after the mute: a muted track, or one that another track's solo silences, would
	// render silence. Before the fader, mute and solo don't matter.
	const Json complete = {{"tracks", {audio.track}}, {"processing", "completeSignalPath"}};
	const auto tracks = [&project] { return run(project.client, "project.get").at("tracks").size(); };
	run(project.client, "channel.set", {{"channel", audio.channel}, {"mute", true}});
	CHECK_THAT(reject(project.client, "track.renderInPlace", complete), ContainsSubstring("silent"));
	run(project.client, "track.renderInPlace", {{"tracks", {audio.track}}});
	run(project.client, "history.undo");
	run(project.client, "channel.set", {{"channel", audio.channel}, {"mute", false}});
	const Session::Track soloed = project.addTrack("Soloed");
	run(project.client, "channel.set", {{"channel", soloed.channel}, {"solo", true}});
	CHECK_THAT(reject(project.client, "track.renderInPlace", complete), ContainsSubstring("silent"));
	run(project.client, "channel.set", {{"channel", audio.channel}, {"solo", true}});
	run(project.client, "track.renderInPlace", complete);
	run(project.client, "history.undo");
	run(project.client, "channel.set", {{"channel", audio.channel}, {"solo", false}});
	run(project.client, "channel.set", {{"channel", soloed.channel}, {"solo", false}});
	// A disabled track plays nothing, also in a range.
	run(project.client, "track.set", {{"track", audio.track}, {"enabled", false}});
	CHECK_THAT(reject(project.client, "track.renderInPlace", {{"tracks", {audio.track}}, {"start", 0}, {"end", 100}}),
		ContainsSubstring("disabled"));
	run(project.client, "track.set", {{"track", audio.track}, {"enabled", true}});
	CHECK(tracks() == 3);

	run(project.client, "track.freeze", {{"tracks", {audio.track}}, {"tail", 0}});
	CHECK_THAT(reject(project.client, "track.renderInPlace", {{"tracks", {audio.track}}, {"processing", "dry"}}),
		ContainsSubstring("frozen"));
	CHECK(tracks() == 3);
}

namespace
{
	// The path of a media file of the project.
	std::string mediaPath(Session& project, const Json& id)
	{
		std::string path;
		const Json state = run(project.client, "project.get");
		for (const Json& media : state.at("media"))
			if (media.at("id") == id)
				path = media.at("path").get<std::string>();
		REQUIRE(!path.empty());
		return path;
	}
}

TEST_CASE("render in place takes its tail in bars and renders into a folder of its own (#760)", "[integration]")
{
	Session project;
	const Session::Track audio = project.addTrack("Ramp");
	// The ramp plays on past the range, so the tail is as long as it is asked to be.
	const auto rendered = [&project, &audio](Json parameters)
	{
		parameters["tracks"] = {audio.track};
		parameters["start"] = 2000;
		parameters["end"] = 3000;
		const Json result = run(project.client, "track.renderInPlace", parameters);
		const std::string path = mediaPath(project, result.at("media").at(0));
		run(project.client, "history.undo");
		return path;
	};
	// At 120 BPM, a bar of 4/4 is 2 s: 0.01 bars are 20 ms, 960 samples at 48 kHz.
	const fs::path renders = project.folder.path() / "Renders";
	REQUIRE(!fs::exists(renders));
	const std::string bars = rendered({{"tailBars", 0.01}, {"folder", renders.u8string()}});
	CHECK(fs::u8path(bars).parent_path() == renders);
	CHECK(samplesOf(bars).size() == 2 * (1000 + 960));
	CHECK(firstDifference(samplesOf(bars), samplesOf(rendered({{"tail", 0.02}}))) == -1);
	// A bar of 3/4 is 1.5 s.
	run(project.client, "signature.add", {{"bar", 0}, {"numerator", 3}, {"denominator", 4}});
	CHECK(samplesOf(rendered({{"tailBars", 0.01}})).size() == 2 * (1000 + 720));

	CHECK_THAT(reject(project.client, "track.renderInPlace", {{"tracks", {audio.track}}, {"tail", 1}, {"tailBars", 1}}),
		ContainsSubstring("not both"));
	CHECK_THAT(reject(project.client, "track.renderInPlace", {{"tracks", {audio.track}}, {"folder", "relative"}}),
		ContainsSubstring("absolute"));
}

TEST_CASE("render in place mixes tracks down to one audio file (#760)", "[integration]")
{
	Session project;
	const Session::Track one = project.addTrack("One");
	const Session::Track two = project.addTrack("Two", 1);
	run(project.client, "channel.set", {{"channel", two.channel}, {"volume", -6.0}, {"pan", -0.5}});
	run(project.client, "audioEvent.add", {{"track", two.track}, {"clip", project.clip}, {"position", 3000}});
	run(project.client, "audio.render", {{"frames", 4096}});
	const std::vector<float> live = project.mainMix();
	// Each track's complete signal path over the range of both, from 1000 to the end of Two's second event.
	const auto alone = [&project](ObjectId track)
	{
		const Json result = run(project.client, "track.renderInPlace",
			{{"tracks", {track}}, {"processing", "completeSignalPath"}, {"start", 1000}, {"end", 9000}});
		std::vector<float> samples = samplesOf(mediaPath(project, result.at("media").at(0)));
		run(project.client, "history.undo");
		return samples;
	};
	std::vector<float> sum = alone(one.track);
	const std::vector<float> second = alone(two.track);
	REQUIRE(sum.size() == second.size());
	for (size_t i = 0; i < sum.size(); ++i)
		sum[i] += second[i];
	const fs::path audio = project.media.path() / "Audio";
	REQUIRE(fs::exists(audio / "One.wav"));

	const Json result = run(project.client, "track.renderInPlace",
		{{"tracks", {one.track, two.track}}, {"processing", "completeSignalPath"}, {"mixdown", true},
			{"sourceTracks", "mute"}, {"name", "Both"}});
	REQUIRE(result.at("tracks").size() == 1);
	REQUIRE(result.at("events").size() == 1);
	CHECK(project.track(result.at("tracks").at(0).get<ObjectId>()).at("name") == "Both (Rendered)");
	const std::string path = mediaPath(project, result.at("media").at(0));
	CHECK(fs::u8path(path).filename() == "Both.wav");
	CHECK(firstDifference(samplesOf(path), sum) == -1);
	// The tracks' own renders are gone: only the renders above and the mixdown are left.
	const auto wavs = [&audio]
	{
		std::set<std::string> names;
		for (const auto& entry : fs::directory_iterator(audio))
			if (entry.path().extension() == ".wav")
				names.insert(entry.path().filename().u8string());
		return names;
	};
	CHECK(wavs() == std::set<std::string>{"Both.wav", "One.wav", "Two.wav"});
	const Json state = run(project.client, "project.get");
	for (const Json& event : state.at("audioEvents"))
		if (event.at("id") == result.at("events").at(0))
			CHECK(event.at("position") == 1000);
	run(project.client, "audio.render", {{"frames", 4096}}); // both mutes glide there
	CHECK(nullTest(project.mainMix(), live) < 1e-6f);
	run(project.client, "history.undo");
	CHECK(run(project.client, "project.get").at("tracks").size() == 2);

	// In an integer format, the sum is rounded to it; the name becomes a file name in the folder.
	const Json int24 = run(project.client, "track.renderInPlace",
		{{"tracks", {one.track, two.track}}, {"processing", "completeSignalPath"}, {"mixdown", true},
			{"sampleFormat", "int24"}, {"name", "AC/DC: live"}});
	CHECK(project.track(int24.at("tracks").at(0).get<ObjectId>()).at("name") == "AC/DC: live (Rendered)");
	const std::string rounded = mediaPath(project, int24.at("media").at(0));
	CHECK(fs::u8path(rounded) == audio / "AC_DC_ live.wav");
	CHECK(AudioFileReader::open(rounded)->info().sampleFormat == SampleFormat::Int24);
	const std::vector<float> samples = samplesOf(rounded);
	REQUIRE(samples.size() == sum.size());
	float error = 0.0f;
	for (size_t i = 0; i < sum.size(); ++i)
		error = std::max(error, std::abs(samples[i] - sum[i]));
	CHECK(error <= 1.0f / 8388608.0f); // half a step of 24 bits, and rounding of the float sum
	run(project.client, "history.undo");

	const Json both = {one.track, two.track};
	CHECK_THAT(reject(project.client, "track.renderInPlace",
				   {{"tracks", {one.track}}, {"processing", "completeSignalPath"}, {"mixdown", true}}),
		ContainsSubstring("several tracks"));
	CHECK_THAT(reject(project.client, "track.renderInPlace", {{"tracks", both}, {"mixdown", true}}),
		ContainsSubstring("complete signal path"));
	CHECK_THAT(
		reject(project.client, "track.renderInPlace",
			{{"tracks", both}, {"processing", "completeSignalPath"}, {"mixdown", true}, {"mode", "separateEvents"}}),
		ContainsSubstring("oneEvent"));
	const Json group = run(project.client, "track.add", {{"type", "group"}});
	run(project.client, "channel.set", {{"channel", two.channel}, {"output", group.at("channel").at("id")}});
	CHECK_THAT(reject(project.client, "track.renderInPlace",
				   {{"tracks", both}, {"processing", "completeSignalPath"}, {"mixdown", true}}),
		ContainsSubstring("one output"));
}

TEST_CASE("a frozen instrument track's MIDI send to another instrument track stays out of the graph", "[integration]")
{
	Session project;
	const Session::Track synth = project.addSynth();
	const Session::Track other = project.addSynth();
	REQUIRE(run(project.client, "track.freeze", {{"tracks", {synth.track}}, {"tail", 0}}).at("tracks").size() == 1);
	run(project.client, "channel.set",
		{{"channel", synth.channel},
			{"midiSends",
				{{"slots",
					{{{"effect", "transformer"}, {"parameters", {{"value", 1}}}, {"output", other.channel}}}}}}});
	run(project.client, "midi.capture", {{"start", true}});
	run(project.client, "transport.locate", {{"position", 0}});
	run(project.client, "transport.play");
	run(project.client, "audio.render", {{"frames", 48000}});
	// The other track plays its own notes, none a semitone up from the frozen one's.
	const Json captured = run(project.client, "midi.capture").at("captured");
	INFO(captured.dump());
	CHECK(!captured.empty());
	for (const Json& message : captured)
		CHECK(message.at("bytes").at(1).get<int>() % 12 != 1);
	// The send keeps its output, which plays again once the track is unfrozen.
	run(project.client, "track.unfreeze", {{"tracks", {synth.track}}});
	run(project.client, "midi.capture", {{"start", true}});
	run(project.client, "transport.locate", {{"position", 0}});
	run(project.client, "audio.render", {{"frames", 48000}});
	const Json again = run(project.client, "midi.capture").at("captured");
	INFO(again.dump());
	CHECK(std::any_of(again.begin(), again.end(),
		[](const Json& message) { return message.at("bytes").at(1).get<int>() % 12 == 1; }));
}

TEST_CASE("background renders lock their tracks and let another client edit elsewhere", "[integration]")
{
	Session session;
	const auto source = session.addTrack("Source");
	const auto other = session.addTrack("Other");
	Client remote(session.engine, "remote");
	const std::string command = GENERATE(std::string("track.freeze"), std::string("track.renderInPlace"));
	const Json before = run(session.client, "history.get");
	const Json finished = gnubase::test::whileHeld(session.engine, session.client, command,
		{{"tracks", {source.track}}, {"tail", 0}},
		[&](uint64_t)
		{
			CHECK_THAT(reject(remote, "channel.set", {{"channel", source.channel}, {"pre", {{"gain", -6}}}}),
				ContainsSubstring("being rendered"));
			CHECK_THAT(reject(remote, "track.endRender", {{"token", 1}}), ContainsSubstring("only a render job"));
			CHECK_THAT(reject(remote, "track.remove", {{"track", source.track}}), ContainsSubstring("being rendered"));
			run(remote, "channel.set", {{"channel", other.channel}, {"volume", -9}});
			// The fader is after both render points, so it remains editable too.
			run(remote, "channel.set", {{"channel", source.channel}, {"volume", -7}});
			CHECK(run(remote, "history.get").at("position") == before.at("position").get<int>() + 2);
		});
	INFO(finished.dump());
	REQUIRE(finished.at("state") == "done");
	CHECK(run(remote, "history.get").at("position") == before.at("position").get<int>() + 3);
	run(remote, "channel.set", {{"channel", source.channel}, {"volume", -3}});
}

TEST_CASE("background track renders name their completed undo step", "[integration]")
{
	Session session;
	const auto source = session.addTrack("Source");
	const bool freeze = GENERATE(true, false);
	run(session.client, freeze ? "track.freeze" : "track.renderInPlace", {{"tracks", {source.track}}, {"tail", 0}});
	CHECK(
		run(session.client, "history.get").at("steps").back().at("action") == (freeze ? "Freeze" : "Render in place"));
}

TEST_CASE("render locks allow version names and live input settings", "[integration]")
{
	Session session;
	const bool audio = GENERATE(false, true);
	const auto source = audio ? session.addTrack("Source") : session.addSynth();
	const Json input =
		run(session.client, "bus.add", {{"direction", "input"}, {"format", "stereo"}}).at("bus").at("id");
	const std::string command = GENERATE(std::string("track.freeze"), std::string("track.renderInPlace"));
	const Json finished = gnubase::test::whileHeld(session.engine, session.client, command,
		{{"tracks", {source.track}}, {"tail", 0}},
		[&](uint64_t)
		{
			run(session.client, "trackVersion.set", {{"tracks", {source.track}}, {"name", "Renamed while rendering"}});
			if (audio)
			{
				run(session.client, "channel.set", {{"channel", source.channel}, {"input", input}});
			}
			else
			{
				run(session.client, "channel.set", {{"channel", source.channel}, {"midiInput", "none"}});
				run(session.client, "channel.set",
					{{"channel", source.channel}, {"inputTransformer", {{"mode", "track"}}}});
			}
			run(session.client, "history.undo");
			run(session.client, "history.redo");
		});
	INFO(finished.dump());
	REQUIRE(finished.at("state") == "done");
}

TEST_CASE("undo and redo cannot change a rendering track's audio input", "[integration]")
{
	Session session;
	const auto source = session.addTrack("Source");
	const bool redo = GENERATE(false, true);
	const std::string command = GENERATE(std::string("track.freeze"), std::string("track.renderInPlace"));
	run(session.client, "channel.set", {{"channel", source.channel}, {"pre", {{"gain", -6}}}});
	if (redo)
		run(session.client, "history.undo");
	const Json before = run(session.client, "history.get");
	const Json project = run(session.client, "project.get");
	const Json finished =
		gnubase::test::whileHeld(session.engine, session.client, command, {{"tracks", {source.track}}, {"tail", 0}},
			[&](uint64_t)
			{
				CHECK_THAT(reject(session.client, redo ? "history.redo" : "history.undo", Json::object()),
					ContainsSubstring("being rendered"));
				CHECK(run(session.client, "history.get") == before);
				CHECK(run(session.client, "project.get") == project);
			});
	INFO(finished.dump());
	REQUIRE(finished.at("state") == "done");
}

TEST_CASE("private renders instantiate only selected channels and their audio dependencies", "[integration]")
{
	Session session;
	const auto source = session.addTrack("Source");
	const auto unrelated = session.addTrack("Unrelated");
	const Json group = run(session.client, "track.add", {{"type", "group"}, {"name", "Group"}});
	const ObjectId groupChannel = group.at("channel").at("id").get<ObjectId>();
	run(session.client, "channel.set", {{"channel", source.channel}, {"output", groupChannel}});
	session.insertGain(source.channel, -6);
	run(session.client, "plugin.insert", {{"channel", unrelated.channel}, {"plugin", "builtin:gainPan"}});
	session.activate({unrelated.channel});
	const auto made = std::make_shared<std::atomic<int>>(0);
	session.engine.setRenderPluginFactory(
		[made](const std::string& plugin, uint32_t channels, gnubase::plugin::Host& host)
		{
			++*made;
			return processors::BuiltinFactory(channels).create(plugin, host);
		});
	const Json result = run(session.client, "mixdown.export",
		{{"folder", session.folder.path().u8string()}, {"name", "Group"}, {"channels", {groupChannel}}, {"start", 0},
			{"end", 7000}, {"sampleFormat", "float32"}});
	CHECK(made->load() == 1);
	CHECK(peakOf(samplesOf(result.at("files").at(0).at("path").get<std::string>())) > 0.1f);
}

TEST_CASE("a private render fails if a live active plugin cannot be instantiated", "[integration]")
{
	Session session;
	const auto source = session.addTrack("Source");
	session.insertGain(source.channel, -6);
	session.engine.setRenderPluginFactory(
		[](const std::string&, uint32_t, gnubase::plugin::Host&) -> std::unique_ptr<gnubase::plugin::Plugin>
		{ return nullptr; });
	const std::string command = GENERATE(std::string("track.freeze"), std::string("track.renderInPlace"));
	const Json history = run(session.client, "history.get");
	CHECK_THAT(reject(session.client, command, {{"tracks", {source.track}}, {"tail", 0}}), ContainsSubstring("plugin"));
	CHECK(run(session.client, "history.get") == history);
	CHECK(run(session.client, "track.renderLocks").at("locks").empty());
}

TEST_CASE("a private render waits for a live plugin that is still loading", "[integration]")
{
	Session session;
	const auto source = session.addTrack("Source");
	session.engine.setRenderPluginFactory(
		[](const std::string&, uint32_t, gnubase::plugin::Host&) -> std::unique_ptr<gnubase::plugin::Plugin>
		{ return nullptr; });
	// The plugin job thread waits, so the live slot is still loading when the export starts (#725): the export
	// must still have its plugin, and fails without it.
	std::promise<void> release;
	std::future<void> blocked = session.engine.pluginHosts().run([go = release.get_future().share()] { go.wait(); });
	run(session.client, "plugin.insert", {{"channel", source.channel}, {"plugin", "builtin:gainPan"}});
	const Result started = session.client.call("mixdown.export",
		{{"folder", session.folder.path().u8string()}, {"name", "loading"}, {"start", 0}, {"end", 7000}});
	CHECK(
		run(session.client, "plugin.list", {{"channel", source.channel}}).at("slots").at(0).at("status") == "loading");
	release.set_value();
	blocked.get();
	REQUIRE(started.ok());
	const Json finished = waitForJob(session.client, started.value.at("job").at("id").get<uint64_t>());
	CHECK(finished.at("state") == "failed");
	// The error, which File > Export > Quick Audio Export shows, names the plugin and its channel.
	CHECK_THAT(finished.value("error", std::string()), ContainsSubstring("Gain and pan on Source"));
}

TEST_CASE("a private render's plugins are activated and process offline", "[integration]")
{
	Session session;
	const auto source = session.addTrack("Source");
	session.insertGain(source.channel, -6);
	const auto modes = std::make_shared<Modes>();
	session.engine.setRenderPluginFactory(
		[modes](const std::string& plugin, uint32_t channels, gnubase::plugin::Host& host)
		{ return std::make_unique<ModeRecorder>(processors::BuiltinFactory(channels).create(plugin, host), modes); });
	run(session.client, "mixdown.export",
		{{"folder", session.folder.path().u8string()}, {"name", "offline"}, {"start", 0}, {"end", 7000}});
	// The job has ended, and its plugin with it (#725).
	CHECK(modes->activated == std::vector<gnubase::plugin::ProcessMode>{gnubase::plugin::ProcessMode::Offline});
	CHECK(modes->offlineBlocks.load() > 0);
	CHECK(modes->realtimeBlocks.load() == 0);
}

TEST_CASE("cancelling a background track render unlocks without an undo step or files", "[integration]")
{
	Session session;
	const auto source = session.addTrack("Source");
	const std::string command = GENERATE(std::string("track.freeze"), std::string("track.renderInPlace"));
	const Json before = run(session.client, "project.get");
	const Json history = run(session.client, "history.get");
	const Json finished =
		gnubase::test::whileHeld(session.engine, session.client, command, {{"tracks", {source.track}}, {"tail", 0}},
			[&](uint64_t job) { run(session.client, "job.cancel", {{"job", job}}); });
	CHECK(finished.at("state") == "cancelled");
	CHECK(run(session.client, "project.get") == before);
	CHECK(run(session.client, "history.get") == history);
	run(session.client, "track.remove", {{"track", source.track}});
}

TEST_CASE("a background mixdown keeps its starting audio while clients edit the project", "[integration]")
{
	Session session;
	const auto source = session.addTrack("Source");
	const auto expected = session.mainMix();
	Client remote(session.engine, "remote");
	const Json finished = gnubase::test::whileHeld(session.engine, session.client, "mixdown.export",
		{{"folder", session.folder.path().u8string()}, {"name", "snapshot"}, {"start", 0}, {"end", 40000},
			{"sampleFormat", "float32"}},
		[&](uint64_t)
		{
			run(remote, "channel.set", {{"channel", source.channel}, {"volume", -12}});
			run(remote, "track.remove", {{"track", source.track}});
			CHECK(run(remote, "track.renderLocks").at("locks").empty());
		});
	INFO(finished.dump());
	REQUIRE(finished.at("state") == "done");
	CHECK(firstDifference(samplesOf(finished.at("result").at("files").at(0).at("path").get<std::string>()), expected) ==
		-1);
}

TEST_CASE("a render range locks intersecting audio and transactions but allows events outside it", "[integration]")
{
	Session session;
	const auto source = session.addTrack("Source");
	const auto outside =
		run(session.client, "audioEvent.add", {{"track", source.track}, {"clip", session.clip}, {"position", 10000}})
			.at("event")
			.at("id");
	const Json project = run(session.client, "project.get");
	const auto inside = project.at("audioEvents").at(0).at("id");
	const Json finished = gnubase::test::whileHeld(session.engine, session.client, "track.renderInPlace",
		{{"tracks", {source.track}}, {"start", 1000}, {"end", 7000}},
		[&](uint64_t)
		{
			CHECK_THAT(reject(session.client, "audioEvent.set", {{"event", inside}, {"volume", -3}}),
				ContainsSubstring("being rendered"));
			run(session.client, "audioEvent.set", {{"event", outside}, {"volume", -3}});
			CHECK_THAT(
				reject(session.client, "history.transaction",
					{{"description", "Change locked audio"},
						{"commands",
							{{{"command", "audioEvent.set"}, {"parameters", {{"event", inside}, {"mute", true}}}}}}}),
				ContainsSubstring("being rendered"));
			run(session.client, "history.undo"); // the unrelated event's edit
			CHECK_THAT(reject(session.client, "track.move", {{"track", source.track}, {"parent", 0}, {"index", 0}}),
				ContainsSubstring("being rendered"));
		});
	INFO(finished.dump());
	REQUIRE(finished.at("state") == "done");
	CHECK(finished.at("result").at("tracks").size() == 1);
}

TEST_CASE("a held render refuses changes of the project settings every render hears", "[integration]")
{
	Session session;
	const auto source = session.addTrack("Source");
	// Only the project-wide part of the lock check catches these (#1850).
	const Json setting =
		GENERATE(Json{{"rootKey", "D"}}, Json{{"panLaw", "-6dB"}}, Json{{"tempo", 90}}, Json{{"sampleRate", 44100}});
	const Json before = run(session.client, "project.get");
	const Json finished = gnubase::test::whileHeld(session.engine, session.client, "track.renderInPlace",
		{{"tracks", {source.track}}, {"tail", 0}},
		[&](uint64_t)
		{
			INFO(setting.dump());
			CHECK_THAT(reject(session.client, "project.set", setting), ContainsSubstring("being rendered"));
		});
	INFO(finished.dump());
	REQUIRE(finished.at("state") == "done");
	const Json after = run(session.client, "project.get");
	for (const char* key : {"rootKey", "panLaw", "tempo", "sampleRate"})
		CHECK(after.value(key, Json()) == before.value(key, Json()));
}

TEST_CASE("a failed background render removes files and unlocks its tracks", "[integration]")
{
	Session session;
	const auto source = session.addTrack("Source");
	const Json before = run(session.client, "history.get");
	const auto progress = std::make_shared<int>(0);
	session.engine.setJobProgressHook(
		[progress](uint64_t, const std::string&)
		{
			if (++*progress == 3)
				throw std::runtime_error("render failed");
		});
	const auto started = session.client.call("track.freeze", {{"tracks", {source.track}}, {"tail", 0}});
	REQUIRE(started.ok());
	const Json finished = waitForJob(session.client, started.value.at("job").at("id").get<uint64_t>());
	CHECK(finished.at("state") == "failed");
	CHECK(finished.at("error") == "render failed");
	CHECK(run(session.client, "track.renderLocks").at("locks").empty());
	CHECK(run(session.client, "history.get") == before);
	CHECK(fs::directory_iterator(session.media.path() / "Freeze") == fs::directory_iterator());
	run(session.client, "track.remove", {{"track", source.track}});
}

TEST_CASE("a background instrument render locks the MIDI tracks feeding it", "[integration]")
{
	Session session;
	const auto synth = session.addSynth();
	const Json midi =
		run(session.client, "track.add", {{"type", "midi"}, {"timeBase", "linear"}, {"output", synth.channel}});
	const auto track = midi.at("track").at("id");
	const auto channel = midi.at("channel").at("id");
	const Json unrelated = run(session.client, "track.add", {{"type", "midi"}, {"output", 0}}).at("channel").at("id");
	const Json finished = gnubase::test::whileHeld(session.engine, session.client, "track.freeze",
		{{"tracks", {synth.track}}, {"tail", 0}},
		[&](uint64_t)
		{
			// Newly routed inputs and newly inserted slots have no id in the render's starting input.
			CHECK_THAT(reject(session.client, "channel.set", {{"channel", unrelated}, {"output", synth.channel}}),
				ContainsSubstring("being rendered"));
			CHECK_THAT(
				reject(session.client, "plugin.insert", {{"channel", synth.channel}, {"plugin", "builtin:gainPan"}}),
				ContainsSubstring("being rendered"));
			CHECK_THAT(reject(session.client, "track.remove", {{"track", track}}), ContainsSubstring("being rendered"));
			CHECK_THAT(reject(session.client, "channel.set", {{"channel", channel}, {"output", 0}}),
				ContainsSubstring("being rendered"));
			const Json locks = run(session.client, "track.renderLocks").at("locks").at(0).at("tracks");
			CHECK(std::find(locks.begin(), locks.end(), track) != locks.end());
		});
	INFO(finished.dump());
	REQUIRE(finished.at("state") == "done");
	run(session.client, "track.remove", {{"track", track}});
}

TEST_CASE("undo cannot move a rendering track but can undo a move elsewhere", "[integration]")
{
	Session session;
	const auto source = session.addTrack("Source");
	const auto other = session.addTrack("Other");
	run(session.client, "track.move", {{"track", source.track}, {"parent", 0}, {"index", 1}});
	const Json finished = gnubase::test::whileHeld(session.engine, session.client, "track.freeze",
		{{"tracks", {source.track}}, {"tail", 0}},
		[&](uint64_t)
		{
			CHECK_THAT(reject(session.client, "history.undo", Json::object()), ContainsSubstring("being rendered"));
			run(session.client, "track.move", {{"track", other.track}, {"parent", 0}, {"index", 1}});
			run(session.client, "history.undo");
		});
	INFO(finished.dump());
	REQUIRE(finished.at("state") == "done");
}

TEST_CASE("engine shutdown releases running track renders before the sample loader", "[integration]")
{
	const std::string command = GENERATE(std::string("track.freeze"), std::string("track.renderInPlace"));
	const gnubase::test::TempDir folder;
	const fs::path path = folder.path() / "sample.wav";
	gnubase::test::writeWav(path, {std::vector<float>(12000, 0.25f), 2, 48000});
	std::promise<void> rendering;
	const auto atProgress = rendering.get_future();
	std::atomic<bool> stopped{false};
	auto engine = std::make_unique<Engine>();
	Engine* renderingEngine = engine.get();
	{
		Client client(*engine);
		run(client, "audio.setDevice", {{"blockSize", 64}, {"captureSeconds", 0}});
		run(client, "project.save", {{"path", (folder.path() / "project.dawproject").u8string()}});
		const Json media = run(client, "media.add", {{"path", path.u8string()}}).at("media");
		const Json clip = run(client, "clip.add", {{"media", media.at("id")}}).at("clip").at("id");
		const Json sampler = run(client, "sampler.create").at("track").at("id");
		run(client, "sampler.load", {{"track", sampler}, {"clip", clip}});
		const Json part = run(
			client, "midiPart.add", {{"track", sampler}, {"position", 0}, {"length", gnubase::time::g_ticksPerQuarter}})
							  .at("part")
							  .at("id");
		run(client, "note.add",
			{{"part", part},
				{"notes",
					{{{"position", 0}, {"length", gnubase::time::g_ticksPerQuarter}, {"pitch", 60},
						{"velocity", 0.8}}}}});
		const Json audio = run(client, "track.add", {{"type", "audio"}}).at("track").at("id");
		run(client, "audioEvent.add", {{"track", audio}, {"clip", clip}, {"position", 0}});

		engine->setJobProgressHook(
			[&, renderingEngine](uint64_t, const std::string&)
			{
				rendering.set_value();
				// Keep the render past its first block until shutdown refuses command posting. Its cleanup then
				// cannot remove the TrackRender from the project, including the second, unstarted work function.
				for (;;)
				{
					const auto reply = std::make_shared<std::promise<Result>>();
					auto result = reply->get_future();
					renderingEngine->post("engine.version", Json::object(),
						[reply](Result value) { reply->set_value(std::move(value)); });
					if (!result.get().ok())
					{
						stopped = true;
						throw JobCancelled();
					}
				}
			});
		const Result started = client.call(command, {{"tracks", {sampler, audio}}, {"tail", 0}});
		REQUIRE(started.ok());
		REQUIRE(atProgress.wait_for(std::chrono::seconds(30)) == std::future_status::ready);
	}
	engine.reset();
	CHECK(stopped.load());
	const fs::path output = folder.path() / (command == "track.freeze" ? "Freeze" : "Audio");
	CHECK(fs::directory_iterator(output) == fs::directory_iterator());
}
