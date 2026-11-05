// SPDX-License-Identifier: GPL-3.0-or-later
// MIDI devices and patch banks (#136, docs/features/midi-devices.md): installed devices with their patches, and a
// MIDI track's program and bank, which reach its instrument as bank select and program change.
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

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
