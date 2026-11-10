// SPDX-License-Identifier: GPL-3.0-or-later
// Render in place and freeze (#116, docs/features/render-in-place.md): track.renderInPlace, track.freeze,
// track.unfreeze and mixdown.export's render points.
#include "engine/audiofile/audiofile.h"
#include "engine/dawproject/dawproject.h"
#include "engine/document/document.h"
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;

namespace
{
	namespace fs = std::filesystem;

	constexpr int64_t g_scrubFrames = 480; // how far each scrub of a test goes

	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	std::string reject(Client& client, const std::string& command, Json parameters)
	{
		const Result result = client.call(command, std::move(parameters));
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
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (run(client, "plugin.get", {{"slot", slot}}).at("slot").at("status") != "active" &&
				std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			REQUIRE(run(client, "plugin.get", {{"slot", slot}}).at("slot").at("status") == "active");
			run(client, "audio.updatePlugins");
			run(client, "parameter.set",
				{{"channel", channel}, {"target", {{"type", "plugin"}, {"slot", slot}, {"parameter", 0}}},
					{"value", gain}});
			return slot.get<ObjectId>();
		}

		// The main mix from 0 to 40000 as a 32-bit float file's samples.
		// Waits until the plugin slots of these channels have their plugins active, and the graph has them. An
		// unfreeze makes the plugins of the track's slots again, on the plugin job thread: until they are active, the
		// slots pass their audio on unchanged (#951).
		void activate(const std::vector<ObjectId>& channels)
		{
			const auto active = [&]
			{
				for (const ObjectId channel : channels)
				{
					const Json slots = run(client, "plugin.list", {{"channel", channel}}).at("slots");
					if (!std::all_of(
							slots.begin(), slots.end(), [](const Json& slot) { return slot.at("status") == "active"; }))
						return false;
				}
				return true;
			};
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
			while (!active() && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			REQUIRE(active());
			run(client, "audio.updatePlugins");
		}

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

TEST_CASE("undo brings back the freeze file that was frozen then", "[integration]")
{
	Session project;
	const Session::Track audio = project.addTrack("Ramp");
	const ObjectId slot = project.insertGain(audio.channel, -6.0);
	run(project.client, "audio.render", {{"frames", 4096}});
	const Json first = run(project.client, "track.freeze", {{"tracks", {audio.track}}, {"tail", 0}});
	const std::vector<float> frozen = project.mainMix();

	run(project.client, "track.unfreeze", {{"tracks", {audio.track}}});
	run(project.client, "parameter.set",
		{{"channel", audio.channel}, {"target", {{"type", "plugin"}, {"slot", slot}, {"parameter", 0}}},
			{"value", 0.0}});
	run(project.client, "audio.render", {{"frames", 4096}});
	const Json second = run(project.client, "track.freeze", {{"tracks", {audio.track}}, {"tail", 0}});
	REQUIRE(second.at("tracks").at(0).at("frozen").at("path") != first.at("tracks").at(0).at("frozen").at("path"));
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

	// Frozen again with another gain, the scrub reads the new file.
	run(project.client, "track.unfreeze", {{"tracks", {audio.track}}});
	run(project.client, "parameter.set",
		{{"channel", audio.channel}, {"target", {{"type", "plugin"}, {"slot", slot}, {"parameter", 0}}},
			{"value", 0.0}});
	run(project.client, "audio.render", {{"frames", 4096}});
	const Json second = run(project.client, "track.freeze", {{"tracks", {audio.track}}, {"tail", 0}});
	const std::string secondPath = second.at("tracks").at(0).at("frozen").at("path").get<std::string>();
	REQUIRE(secondPath != first.at("tracks").at(0).at("frozen").at("path"));
	const std::vector<float> secondFile = samplesOf(secondPath);
	REQUIRE(firstDifference(secondFile, firstFile) >= 0);
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
		fs::remove(fs::u8path(freeze.at("path").get<std::string>()));
		const fs::path file = project.folder.path() / "missing.dawproject";
		run(project.client, "project.save", {{"path", file.u8string()}});
		run(project.client, "project.load", {{"path", file.u8string()}});
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
	Json state;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	do
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		state = run(project.client, "job.get", {{"job", job.at("job").at("id")}}).at("job");
	} while (state.at("state") == "running" && std::chrono::steady_clock::now() < deadline);
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
	do
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		state = run(project.client, "job.get", {{"job", again.at("job").at("id")}}).at("job");
	} while (state.at("state") == "running" && std::chrono::steady_clock::now() < deadline);
	REQUIRE(state.at("state") == "done");
	// Reported for each track that plays it.
	const Json missing = state.at("result").at("missing");
	REQUIRE(missing.size() == 2);
	CHECK(missing.at(0) == Json{{"track", audio.track}, {"path", path}});
	CHECK(missing.at(1).at("path") == path);
	CHECK(!fs::exists(project.folder.path() / "again" / "Freeze"));
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

TEST_CASE("render in place refuses what it can't render", "[integration]")
{
	Session project;
	const Session::Track audio = project.addTrack("Ramp");
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
