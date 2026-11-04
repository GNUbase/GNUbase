// SPDX-License-Identifier: GPL-3.0-or-later
// Render in place and freeze (#116, docs/features/render-in-place.md): track.renderInPlace, track.freeze,
// track.unfreeze and mixdown.export's render points.
#include "engine/audiofile/audiofile.h"
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
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;

namespace
{
	namespace fs = std::filesystem;

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

TEST_CASE("a frozen track stays frozen in the project file", "[integration]")
{
	Session project;
	const Session::Track audio = project.addTrack("Ramp");
	const Json frozen = run(project.client, "track.freeze", {{"tracks", {audio.track}}, {"tail", 0}});
	const std::string file = (project.folder.path() / "frozen.dawproject").u8string();
	run(project.client, "project.save", {{"path", file}});
	run(project.client, "project.load", {{"path", file}});
	CHECK(project.track(audio.track).at("frozen") == frozen.at("tracks").at(0).at("frozen"));
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
	run(project.client, "track.freeze", {{"tracks", {audio.track}}, {"tail", 0}});
	CHECK_THAT(reject(project.client, "track.renderInPlace", {{"tracks", {audio.track}}, {"processing", "dry"}}),
		ContainsSubstring("frozen"));
	CHECK(run(project.client, "project.get").at("tracks").size() == 2);
}
