// SPDX-License-Identifier: GPL-3.0-or-later
// MIDI devices and patch banks (#136, docs/features/midi-devices.md): installed devices with their patches, and a
// MIDI track's program and bank, which reach its instrument as bank select and program change.
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;

namespace
{
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

	// A MIDI track routed to an instrument track that plays the note probe, on the dummy device.
	struct PatchTrack
	{
		PatchTrack() : engine(gnubase::audio::makeBackends())
		{
			run(client, "audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}, {"captureSeconds", 1}});
			const Json instrument = run(client, "track.add", {{"type", "instrument"}}).at("channel");
			run(client, "channel.set", {{"channel", instrument.at("id")}, {"instrument", "noteProbe"}});
			channel = run(client, "track.add", {{"type", "midi"}}).at("channel").at("id").get<uint64_t>();
			run(client, "channel.set", {{"channel", channel}, {"output", instrument.at("id")}});
			run(client, "midi.capture", {{"start", true}});
		}

		Json set(Json parameters)
		{
			parameters["channel"] = channel;
			return run(client, "channel.set", std::move(parameters)).at("channel");
		}

		// The MIDI 1.0 bytes the instrument got in the next frames.
		std::vector<std::vector<int>> played(int frames = 64)
		{
			run(client, "audio.render", {{"frames", frames}});
			std::vector<std::vector<int>> bytes;
			const Json capture = run(client, "midi.capture");
			for (const Json& captured : capture.at("captured"))
				bytes.push_back(captured.at("bytes").get<std::vector<int>>());
			return bytes;
		}

		Engine engine;
		Client client{engine};
		uint64_t channel = 0;
	};

	using Bytes = std::vector<std::vector<int>>;
}

TEST_CASE("MIDI devices are installed from the GM template or empty, and removed", "[unit]")
{
	Engine engine;
	Client client(engine);
	CHECK(run(client, "settings.get", {{"key", "midi.devices"}}).at("value") == Json::object());

	const Json gm = run(client, "midiDevice.install", {{"name", "Sound Module"}, {"template", "gm"}}).at("device");
	CHECK(gm.at("name") == "Sound Module");
	const Json& banks = gm.at("banks");
	REQUIRE(banks.size() == 1);
	CHECK(banks[0].at("name") == "GM");
	const Json& patches = banks[0].at("patches");
	REQUIRE(patches.size() == 128);
	CHECK(patches[0] == Json{{"name", "Acoustic Grand Piano"}, {"folder", "Piano"}, {"program", 0}});
	CHECK(patches[4].at("name") == "Electric Piano 1");
	CHECK(patches[127] == Json{{"name", "Gunshot"}, {"folder", "Sound Effects"}, {"program", 127}});

	const Json empty = run(client, "midiDevice.install", {{"name", "My Synth"}}).at("device");
	CHECK(empty == Json{{"name", "My Synth"}, {"banks", Json::array()}});
	const Json devices = run(client, "settings.get", {{"key", "midi.devices"}}).at("value");
	CHECK(devices.size() == 2);
	CHECK(devices.at("Sound Module").at("banks") == banks);

	CHECK_THAT(reject(client, "midiDevice.install", {{"name", "My Synth"}}), ContainsSubstring("already"));
	CHECK_THAT(reject(client, "midiDevice.install", {{"name", ""}}), ContainsSubstring("name"));
	run(client, "midiDevice.remove", {{"name", "My Synth"}});
	CHECK_THAT(reject(client, "midiDevice.remove", {{"name", "My Synth"}}), ContainsSubstring("no MIDI device"));
	CHECK(run(client, "settings.get", {{"key", "midi.devices"}}).at("value").size() == 1);

	// Patches are user settings: settings.set edits them, a broken patch is refused.
	Json edited = devices;
	edited.at("Sound Module").at("banks")[0].at("patches")[0].at("name") = "My Piano";
	run(client, "settings.set", {{"key", "midi.devices"}, {"value", edited}});
	Json broken = devices;
	broken.at("Sound Module").at("banks")[0].at("patches")[0].at("program") = 128;
	CHECK(!client.call("settings.set", {{"key", "midi.devices"}, {"value", broken}}).ok());
}

TEST_CASE("a MIDI track's program, bank and device are set, undone and saved", "[integration]")
{
	PatchTrack track;
	const Json fresh = run(track.client, "project.get").at("channels")[1];
	CHECK(fresh.at("program").is_null());
	CHECK(fresh.at("bank").is_null());
	CHECK(fresh.at("device") == "");

	const Json set = track.set({{"program", 5}, {"bank", 300}, {"device", "Sound Module"}});
	CHECK(set.at("program") == 5);
	CHECK(set.at("bank") == 300);
	CHECK(set.at("device") == "Sound Module");
	CHECK_THAT(reject(track.client, "channel.set", {{"channel", track.channel}, {"program", 128}}),
		ContainsSubstring("program"));
	CHECK_THAT(
		reject(track.client, "channel.set", {{"channel", track.channel}, {"bank", 16384}}), ContainsSubstring("bank"));
	const Json instrument = run(track.client, "project.get").at("channels")[0];
	CHECK_THAT(reject(track.client, "channel.set", {{"channel", instrument.at("id")}, {"program", 1}}),
		ContainsSubstring("only MIDI channels"));

	const Json before = run(track.client, "project.get");
	const gnubase::test::TempDir dir;
	const std::string file = (dir.path() / "patches.dawproject").u8string();
	run(track.client, "project.save", {{"path", file}});
	run(track.client, "project.new");
	run(track.client, "project.load", {{"path", file}});
	CHECK(run(track.client, "project.get").at("channels") == before.at("channels"));

	track.set({{"program", nullptr}, {"bank", nullptr}});
	run(track.client, "history.undo");
	CHECK(run(track.client, "project.get").at("channels")[1].at("program") == 5);
}

TEST_CASE("a patch selected for a MIDI track reaches its instrument as bank select and program change", "[integration]")
{
	PatchTrack track;
	CHECK(track.played().empty());

	// Selected while stopped: sent at once, bank select MSB and LSB before the program change, on MIDI channel 1.
	track.set({{"program", 5}, {"bank", 300}});
	CHECK(track.played() == Bytes{{0xB0, 0, 2}, {0xB0, 32, 44}, {0xC0, 5}});
	CHECK(track.played().empty());

	// Again where playback starts, so the instrument has it wherever playback begins.
	run(track.client, "transport.play");
	CHECK(track.played() == Bytes{{0xB0, 0, 2}, {0xB0, 32, 44}, {0xC0, 5}});
	CHECK(track.played().empty());
	run(track.client, "transport.stop");
	track.played();

	// Without a bank, the program change alone; without a program, nothing.
	track.set({{"bank", nullptr}, {"program", 0}});
	CHECK(track.played() == Bytes{{0xC0, 0}});
	track.set({{"program", nullptr}, {"bank", 2}});
	CHECK(track.played().empty());

	// A device's patch: its program and bank, as the inspector sends them.
	run(track.client, "midiDevice.install", {{"name", "Sound Module"}, {"template", "gm"}});
	track.set({{"device", "Sound Module"}, {"program", 4}, {"bank", nullptr}});
	CHECK(track.played() == Bytes{{0xC0, 4}});
}

TEST_CASE("other changes of a MIDI track's channel don't send its patch again", "[integration]")
{
	PatchTrack track;
	track.set({{"program", 5}, {"bank", 300}});
	track.played();

	// Stopped: the new MIDI volume alone.
	track.set({{"midiVolume", 90}});
	CHECK(track.played() == Bytes{{0xB0, 7, 90}});

	// Playing: the patch where playback starts, then the new MIDI volume alone, so a program change a part has played
	// stays.
	run(track.client, "transport.play");
	CHECK(track.played() == Bytes{{0xB0, 0, 2}, {0xB0, 32, 44}, {0xC0, 5}, {0xB0, 7, 90}});
	track.set({{"midiVolume", 100}});
	CHECK(track.played() == Bytes{{0xB0, 7, 100}});

	// A new program goes out at once, with the controllers after it.
	track.set({{"program", 6}});
	CHECK(track.played() == Bytes{{0xB0, 0, 2}, {0xB0, 32, 44}, {0xC0, 6}, {0xB0, 7, 100}});
}

TEST_CASE("a MIDI track's patch, MIDI volume and pan go out on its MIDI channel", "[integration]")
{
	PatchTrack track;
	CHECK(run(track.client, "project.get").at("channels")[1].at("midiChannel").is_null());
	const Json set =
		track.set({{"midiChannel", 4}, {"program", 5}, {"bank", 300}, {"midiVolume", 90}, {"midiPan", 20}});
	CHECK(set.at("midiChannel") == 4);
	CHECK(track.played() == Bytes{{0xB4, 0, 2}, {0xB4, 32, 44}, {0xC4, 5}, {0xB4, 7, 90}, {0xB4, 10, 20}});
	CHECK(track.played().empty());

	// On another channel they go out again, there: Any plays them on channel 1.
	track.set({{"midiChannel", nullptr}});
	CHECK(track.played() == Bytes{{0xB0, 0, 2}, {0xB0, 32, 44}, {0xC0, 5}, {0xB0, 7, 90}, {0xB0, 10, 20}});
	track.set({{"midiChannel", 15}});
	CHECK(track.played() == Bytes{{0xBF, 0, 2}, {0xBF, 32, 44}, {0xCF, 5}, {0xBF, 7, 90}, {0xBF, 10, 20}});

	// Automation of CC 7 plays on its channel too, as automationLane.targets offers it on channel 1, and overrides its
	// MIDI volume.
	const Json added = run(track.client, "automationLane.add",
		{{"track", run(track.client, "project.get").at("channels")[1].at("track")},
			{"target", {{"type", "controller"}, {"number", 7}, {"channel", 0}}}});
	run(track.client, "automationPoint.add",
		{{"lane", added.at("lane").at("id")}, {"points", {{{"position", 0}, {"value", 1.0}}}}});
	run(track.client, "transport.play");
	const Bytes playing = track.played();
	CHECK(std::count(playing.begin(), playing.end(), std::vector<int>{0xBF, 7, 127}) == 1);
	CHECK(std::count(playing.begin(), playing.end(), std::vector<int>{0xBF, 7, 90}) == 0);
	CHECK(std::count(playing.begin(), playing.end(), std::vector<int>{0xB0, 7, 127}) == 0);
	run(track.client, "transport.stop");

	CHECK_THAT(reject(track.client, "channel.set", {{"channel", track.channel}, {"midiChannel", 16}}),
		ContainsSubstring("midiChannel"));
	const Json instrument = run(track.client, "project.get").at("channels")[0];
	CHECK_THAT(reject(track.client, "channel.set", {{"channel", instrument.at("id")}, {"midiChannel", 1}}),
		ContainsSubstring("only MIDI channels"));

	// Saved with the project, and undone.
	const Json before = run(track.client, "project.get");
	const gnubase::test::TempDir dir;
	const std::string file = (dir.path() / "channel.dawproject").u8string();
	run(track.client, "project.save", {{"path", file}});
	run(track.client, "project.new");
	run(track.client, "project.load", {{"path", file}});
	CHECK(run(track.client, "project.get").at("channels") == before.at("channels"));
	track.set({{"midiChannel", 3}});
	run(track.client, "history.undo");
	CHECK(run(track.client, "project.get").at("channels")[1].at("midiChannel") == 15);
}

TEST_CASE(
	"a MIDI track's channel plays its parts', input and automation events on it, and Any keeps theirs", "[integration]")
{
	PatchTrack track;
	constexpr int64_t quarter = 1720320; // ticks
	const Json trackId = run(track.client, "project.get").at("channels")[1].at("track");
	const Json part = run(track.client, "midiPart.add", {{"track", trackId}, {"position", 0}, {"length", 4 * quarter}})
						  .at("part")
						  .at("id");
	run(track.client, "note.add",
		{{"part", part},
			{"notes", {{{"position", 0}, {"length", quarter}, {"pitch", 60}, {"velocity", 0.8}, {"channel", 5}}}}});
	run(track.client, "controller.add",
		{{"part", part},
			{"controllers",
				{{{"position", 0}, {"number", 1}, {"value", 1.0}, {"channel", 5}},
					{{"position", 0}, {"type", "pitchBend"}, {"value", 1.0}, {"channel", 7}}}}});
	// Automation of CC 11 at 127, pitch bend all the way down and channel pressure at 127, on channel 1.
	for (const auto& [target, value] : {std::pair{Json{{"type", "controller"}, {"number", 11}, {"channel", 0}}, 1.0},
			 std::pair{Json{{"type", "pitchBend"}, {"channel", 0}}, 0.0},
			 std::pair{Json{{"type", "channelPressure"}, {"channel", 0}}, 1.0}})
	{
		const Json lane = run(track.client, "automationLane.add", {{"track", trackId}, {"target", target}});
		run(track.client, "automationPoint.add",
			{{"lane", lane.at("lane").at("id")}, {"points", {{{"position", 0}, {"value", value}}}}});
	}
	// The status and first data byte of what the instrument got from the start of playback.
	const auto played = [&track]
	{
		run(track.client, "transport.locate", {{"position", 0}});
		track.played();
		run(track.client, "transport.play");
		Bytes heads;
		for (const std::vector<int>& bytes : track.played())
			heads.push_back({bytes[0], bytes[1]});
		run(track.client, "transport.stop");
		track.played();
		std::sort(heads.begin(), heads.end());
		return heads;
	};

	// Any: each event on its own channel, the automation on channel 1 (reference manual p. 351).
	CHECK(played() == Bytes{{0x95, 60}, {0xB0, 11}, {0xB5, 1}, {0xD0, 127}, {0xE0, 0}, {0xE7, 127}});

	// A channel: everything on it.
	track.set({{"midiChannel", 2}});
	CHECK(played() == Bytes{{0x92, 60}, {0xB2, 1}, {0xB2, 11}, {0xD2, 127}, {0xE2, 0}, {0xE2, 127}});

	// What the track's input plays, also on it, as the transport stands: notes, controllers, pitch bend, pressure and
	// program changes.
	track.set({{"monitor", true}});
	track.played();
	run(track.client, "midi.inject",
		{{"channel", track.channel},
			{"messages",
				{{{"position", 0}, {"bytes", {0x94, 64, 100}}}, {{"position", 0}, {"bytes", {0xB6, 74, 10}}},
					{{"position", 0}, {"bytes", {0xE6, 0, 64}}}, {{"position", 0}, {"bytes", {0xD6, 50}}},
					{{"position", 0}, {"bytes", {0xC6, 7}}}}}});
	CHECK(track.played() == Bytes{{0x92, 64, 100}, {0xB2, 74, 10}, {0xE2, 0, 64}, {0xD2, 50}, {0xC2, 7}});
	run(track.client, "midi.inject",
		{{"channel", track.channel}, {"messages", {{{"position", 0}, {"bytes", {0x84, 64, 0}}}}}});
	CHECK(track.played() == Bytes{{0x82, 64, 0}});
	// A key held while the channel changes: its note-off ends the note its note-on played, the next note plays on the
	// new channel.
	run(track.client, "midi.inject",
		{{"channel", track.channel}, {"messages", {{{"position", 0}, {"bytes", {0x94, 65, 100}}}}}});
	CHECK(track.played() == Bytes{{0x92, 65, 100}});
	track.set({{"midiChannel", 3}});
	track.played();
	run(track.client, "midi.inject",
		{{"channel", track.channel},
			{"messages",
				{{{"position", 0}, {"bytes", {0x84, 65, 0}}}, {{"position", 0}, {"bytes", {0x94, 66, 100}}}}}});
	CHECK(track.played() == Bytes{{0x82, 65, 0}, {0x93, 66, 100}});
	track.set({{"midiChannel", nullptr}});
	track.played();
	run(track.client, "midi.inject",
		{{"channel", track.channel}, {"messages", {{{"position", 0}, {"bytes", {0x94, 64, 100}}}}}});
	CHECK(track.played() == Bytes{{0x94, 64, 100}});
	track.set({{"monitor", false}});

	// A drum sound's own channel wins; one on Any plays on the track's.
	const Json gm = run(track.client, "project.get").at("drumMaps").at(0).at("id");
	const Json map = run(track.client, "drumMap.add", {{"copy", gm}, {"track", trackId}}).at("map").at("id");
	run(track.client, "drumMap.set", {{"map", map}, {"sounds", {{{"pitch", 60}, {"channel", nullptr}}}}});
	run(track.client, "note.add",
		{{"part", part}, {"notes", {{{"position", 0}, {"length", quarter}, {"pitch", 36}, {"velocity", 0.8}}}}});
	track.set({{"midiChannel", 2}});
	const Bytes drums = played();
	CHECK(std::count(drums.begin(), drums.end(), std::vector<int>{0x92, 60}) == 1);
	CHECK(std::count(drums.begin(), drums.end(), std::vector<int>{0x99, 36}) == 1);
}

TEST_CASE("a MIDI track's channel plays its sound slots' output events, and a slot's own channel wins", "[integration]")
{
	PatchTrack track;
	constexpr int64_t quarter = 1720320; // ticks
	const Json trackId = run(track.client, "project.get").at("channels")[1].at("track");
	// Arco, the default slot, switches with the key C0 (24); Staccato with controller 32 at 64, back to 0 when it
	// ends; Pizzicato with the key C#0 on its own channel 7.
	const Json map = run(track.client, "articulationMap.add",
		{{"name", "Strings"}, {"groups", {"Technique", "Length"}},
			{"articulations",
				{{{"name", "Pizzicato"}, {"type", "direction"}, {"group", 0}},
					{{"name", "Staccato"}, {"type", "attribute"}, {"group", 1}}}},
			{"slots",
				{{{"name", "Arco"}, {"trigger", 36},
					 {"outputs", {{{"status", "note"}, {"data1", 24}, {"data2", 100}}}}},
					{{"name", "Staccato"}, {"articulations", {"Staccato"}}, {"trigger", 38},
						{"outputs",
							{{{"status", "controller"}, {"data1", 32}, {"data2", 64}},
								{{"status", "controller"}, {"data1", 32}, {"data2", 0}, {"off", true}}}}},
					{{"name", "Pizzicato"}, {"articulations", {"Pizzicato"}}, {"trigger", 37}, {"channel", 6},
						{"outputs", {{{"status", "note"}, {"data1", 25}, {"data2", 100}}}}}}}})
						 .at("map")
						 .at("id");
	run(track.client, "track.set", {{"track", trackId}, {"articulationMap", map}});
	track.set({{"midiChannel", 4}});
	// The status and first data byte of each message.
	const auto heads = [](const Bytes& played)
	{
		Bytes result;
		for (const std::vector<int>& bytes : played)
			result.push_back({bytes[0], bytes[1]});
		return result;
	};

	SECTION("in playback")
	{
		const Json part = run(track.client, "midiPart.add", {{"track", trackId}, {"position", 0}, {"length", quarter}})
							  .at("part")
							  .at("id");
		const auto note = [&](int64_t position, int pitch, Json articulations)
		{
			run(track.client, "note.add",
				{{"part", part},
					{"notes",
						{{{"position", position}, {"length", quarter / 32}, {"pitch", pitch}, {"velocity", 0.8},
							{"articulations", std::move(articulations)}}}}});
		};
		note(0, 60, Json::array());           // Arco
		note(quarter / 16, 61, {"Staccato"}); // Staccato
		note(quarter / 8, 62, Json::array()); // Pizzicato from the direction
		run(track.client, "controller.add",
			{{"part", part},
				{"controllers",
					{{{"position", quarter / 8}, {"type", "articulation"}, {"articulation", "Pizzicato"}}}}});
		run(track.client, "transport.play");
		const Bytes played = heads(track.played(4096));
		run(track.client, "transport.stop");
		const auto count = [&played](const std::vector<int>& head)
		{ return std::count(played.begin(), played.end(), head); };
		CHECK(count({0x94, 24}) == 1); // Arco's key switch on the track's channel
		CHECK(count({0x94, 60}) == 1);
		CHECK(count({0xB4, 32}) == 2); // Staccato's on and off events there too
		CHECK(count({0x94, 61}) == 1);
		CHECK(count({0x96, 25}) == 1); // Pizzicato's on its own channel
		CHECK(count({0x96, 62}) == 1);
		CHECK(count({0x90, 24}) + count({0xB0, 32}) + count({0x90, 60}) == 0);
	}
	SECTION("from triggers played into the track")
	{
		track.set({{"monitor", true}});
		track.played();
		const auto play = [&](const Bytes& messages)
		{
			Json injected = Json::array();
			for (const std::vector<int>& bytes : messages)
				injected.push_back({{"position", 0}, {"bytes", bytes}});
			run(track.client, "midi.inject", {{"channel", track.channel}, {"messages", injected}});
			return heads(track.played());
		};
		// D1 on channel 2 selects Staccato: controller 32 at 64 on the track's channel, as the notes after it.
		CHECK(play({{0x91, 38, 100}, {0x81, 38, 0}, {0x91, 60, 100}, {0x81, 60, 0}}) ==
			Bytes{{0xB4, 32}, {0x94, 60}, {0x84, 60}});
		// C#1 selects Pizzicato: Staccato's off event on the track's channel, the key switch C#0 and the notes on the
		// slot's own channel.
		CHECK(play({{0x91, 37, 100}, {0x81, 37, 0}, {0x91, 60, 100}, {0x81, 60, 0}}) ==
			Bytes{{0xB4, 32}, {0x96, 25}, {0x86, 25}, {0x96, 60}, {0x86, 60}});
		// C1 selects Arco: its key switch C0 on the track's channel.
		CHECK(play({{0x91, 36, 100}, {0x81, 36, 0}}) == Bytes{{0x94, 24}, {0x84, 24}});

		// D1 selects Staccato again, then the track goes to channel 3: Staccato's off event on channel 5, where its on
		// event went, and its on event on channel 3, as the track's patch goes out again there. D1 again sends
		// nothing, the notes play on channel 3.
		CHECK(play({{0x91, 38, 100}, {0x81, 38, 0}}) == Bytes{{0xB4, 32}});
		track.set({{"midiChannel", 2}});
		const Bytes moved = track.played();
		CHECK(moved == Bytes{{0xB4, 32, 0}, {0xB2, 32, 64}});
		CHECK(play({{0x91, 38, 100}, {0x81, 38, 0}, {0x91, 60, 100}, {0x81, 60, 0}}) == Bytes{{0x92, 60}, {0x82, 60}});

		// Without latch the default slot comes back on the track's channel where the trigger key is released, not on
		// the trigger's: Staccato's off event, then Arco's key switch C0.
		run(track.client, "articulationMap.set", {{"map", map}, {"latch", false}});
		track.played();
		CHECK(play({{0x91, 38, 100}, {0x91, 61, 100}, {0x81, 61, 0}, {0x81, 38, 0}}) ==
			Bytes{{0x92, 61}, {0x82, 61}, {0xB2, 32}, {0x92, 24}, {0x82, 24}});

		// Program change triggers: program 39 selects Staccato on the track's channel; program 10 is no trigger and
		// plays there too.
		run(track.client, "articulationMap.set", {{"map", map}, {"remoteTrigger", "programChange"}});
		track.played();
		CHECK(play({{0xC1, 38}, {0x91, 60, 100}, {0x81, 60, 0}, {0xC1, 9}}) ==
			Bytes{{0xB2, 32}, {0x92, 60}, {0x82, 60}, {0xC2, 9}});
	}
}

TEST_CASE("a send before the MIDI modifiers gets the notes played into the track on its MIDI channel", "[integration]")
{
	PatchTrack track;
	// The modifiers transpose by an octave, the send's transformer by a semitone.
	track.set({{"midiChannel", 2}, {"monitor", true}, {"modifiers", {{"transpose", 12}}},
		{"midiSends", {{"pre", true}, {"slots", {{{"effect", "transformer"}, {"parameters", {{"value", 1}}}}}}}}});
	track.played();
	run(track.client, "midi.inject",
		{{"channel", track.channel}, {"messages", {{{"position", 0}, {"bytes", {0x94, 64, 100}}}}}});
	Bytes played = track.played();
	std::sort(played.begin(), played.end());
	CHECK(played == Bytes{{0x92, 65, 100}, {0x92, 76, 100}});
}

TEST_CASE("automation lanes of one controller on several channels all play on a MIDI track's channel", "[integration]")
{
	PatchTrack track;
	const Json trackId = run(track.client, "project.get").at("channels")[1].at("track");
	// CC 11 at 127 on channel 1 and at 0 on channel 2, as midi.extractAutomation makes them from a part of both.
	for (const auto& [channel, value] : {std::pair{0, 1.0}, std::pair{1, 0.0}})
	{
		const Json lane = run(track.client, "automationLane.add",
			{{"track", trackId}, {"target", {{"type", "controller"}, {"number", 11}, {"channel", channel}}}});
		run(track.client, "automationPoint.add",
			{{"lane", lane.at("lane").at("id")}, {"points", {{{"position", 0}, {"value", value}}}}});
	}
	const auto played = [&track]
	{
		run(track.client, "transport.locate", {{"position", 0}});
		track.played();
		run(track.client, "transport.play");
		Bytes bytes = track.played();
		run(track.client, "transport.stop");
		track.played();
		std::sort(bytes.begin(), bytes.end());
		return bytes;
	};
	CHECK(played() == Bytes{{0xB0, 11, 127}, {0xB1, 11, 0}});
	// On channel 3 both lanes play there, each its own points.
	track.set({{"midiChannel", 2}});
	CHECK(played() == Bytes{{0xB2, 11, 0}, {0xB2, 11, 127}});
}
