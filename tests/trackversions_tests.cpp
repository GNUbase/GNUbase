// SPDX-License-Identifier: GPL-3.0-or-later
// Track versions (#165, reference manual pp. 230-237): trackVersion.add, .duplicate, .set, .remove, .activate and
// .assignCommonId switch a track between sets of events and parts. docs/features/track-versions.md describes them.
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using gnubase::engine::Client;
using gnubase::engine::Engine;
using gnubase::engine::Json;

namespace
{
	constexpr int64_t g_quarter = gnubase::time::g_ticksPerQuarter;

	// Frame f of the ramp file is (f + 1) / 65536: exact in float, so each event is told apart sample by sample.
	float ramp(int64_t frame)
	{
		return static_cast<float>(frame + 1) / 65536.0f;
	}

	struct Fixture
	{
		Json call(const std::string& command, const Json& parameters = Json::object())
		{
			const gnubase::engine::Result result = client.call(command, parameters);
			INFO(command << ": " << result.error);
			REQUIRE(result.ok());
			return result.value;
		}

		std::string fails(const std::string& command, const Json& parameters)
		{
			const gnubase::engine::Result result = client.call(command, parameters);
			REQUIRE(!result.ok());
			return result.error;
		}

		int64_t track(const char* type, const char* timeBase = "linear")
		{
			return call("track.add", {{"type", type}, {"timeBase", timeBase}}).at("track").at("id").get<int64_t>();
		}

		// A clip of a ramp file, 5000 frames at 48 kHz.
		int64_t clip()
		{
			gnubase::test::Audio audio;
			audio.sampleRate = 48000;
			for (int64_t frame = 0; frame < 5000; ++frame)
				audio.samples.push_back(ramp(frame));
			const std::filesystem::path file = dir.path() / "ramp.wav";
			gnubase::test::writeWav(file, audio);
			const Json media = call("media.add", {{"path", file.u8string()}}).at("media");
			return call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<int64_t>();
		}

		int64_t event(int64_t onTrack, int64_t ofClip, int64_t position, int64_t offset, int64_t length)
		{
			return call("audioEvent.add",
				{{"track", onTrack}, {"clip", ofClip}, {"position", position}, {"offset", offset}, {"length", length}})
				.at("event")
				.at("id")
				.get<int64_t>();
		}

		Json trackJson(int64_t id)
		{
			const Json project = call("project.get");
			for (const Json& item : project.at("tracks"))
				if (item.at("id") == id)
					return item;
			FAIL("no track " << id);
		}

		// The ids of the project's objects of a kind on a track.
		std::vector<int64_t> on(const char* kind, int64_t onTrack)
		{
			std::vector<int64_t> ids;
			const Json project = call("project.get");
			for (const Json& item : project.at(kind))
				if (item.at("track") == onTrack)
					ids.push_back(item.at("id").get<int64_t>());
			return ids;
		}

		std::vector<std::string> names(int64_t id)
		{
			std::vector<std::string> result;
			const Json track = trackJson(id);
			for (const Json& version : track.at("versions"))
				result.push_back(version.at("name").get<std::string>());
			return result;
		}

		int active(int64_t id) { return trackJson(id).at("activeVersion").get<int>(); }

		// Plays from the project start and returns the left output channel's first frames.
		std::vector<float> render(int64_t frames)
		{
			call("transport.locate", {{"position", 0}});
			call("transport.play");
			call("audio.render", {{"frames", frames}});
			std::vector<float> output =
				call("audio.capture", {{"channel", 0}, {"start", m_rendered}, {"frames", frames}})
					.at("samples")
					.get<std::vector<float>>();
			call("transport.stop");
			call("audio.render", {{"frames", 256}});
			m_rendered += frames + 256;
			return output;
		}

		gnubase::test::TempDir dir;
		Engine engine;
		Client client{engine};

	private:
		int64_t m_rendered = 0;
	};
}

TEST_CASE("a new track version hides the events of the active one until that is activated again", "[integration]")
{
	Fixture f;
	f.call("audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}, {"captureSeconds", 1}});
	const int64_t clip = f.clip();
	const int64_t vocals = f.track("audio");
	const int64_t guitar = f.track("audio");
	const int64_t take = f.event(vocals, clip, 0, 0, 1000);
	f.event(guitar, clip, 2000, 0, 1000);

	// Every audio, instrument and MIDI track has its first version.
	CHECK(f.names(vocals) == std::vector<std::string>{"v1"});
	CHECK(f.active(vocals) == 1);
	CHECK(f.trackJson(vocals).at("versions").at(0).at("id") == 1);
	CHECK(f.render(1000).at(10) == ramp(10));

	// New Version on both tracks: one new ID, empty and active, so nothing plays.
	const Json added = f.call("trackVersion.add", {{"tracks", {vocals, guitar}}});
	REQUIRE(added.at("tracks").size() == 2);
	const int id = f.active(vocals);
	CHECK(id > 1);
	CHECK(f.active(guitar) == id);
	CHECK(f.names(vocals) == std::vector<std::string>{"v1", "v2"});
	CHECK(f.call("project.get").at("audioEvents").empty());
	CHECK(f.render(1000).at(10) == 0.0f);
	CHECK(f.call("history.get").at("steps").back().at("action") == "New Version");

	// What is added now belongs to the new version.
	const int64_t second = f.event(vocals, clip, 0, 3000, 1000);
	CHECK(f.render(1000).at(10) == ramp(3010));

	// Activating the first version on both brings their events back, and keeps the second version's.
	f.call("trackVersion.activate", {{"tracks", {vocals, guitar}}, {"version", 1}});
	CHECK(f.on("audioEvents", vocals) == std::vector<int64_t>{take});
	CHECK(f.on("audioEvents", guitar).size() == 1);
	CHECK(f.render(1000).at(10) == ramp(10));
	f.call("trackVersion.activate", {{"tracks", {vocals}}, {"version", id}});
	CHECK(f.on("audioEvents", vocals) == std::vector<int64_t>{second});
	CHECK(f.on("audioEvents", guitar).size() == 1); // its first version is still active

	// Undo takes the steps back one by one.
	f.call("history.undo");
	CHECK(f.on("audioEvents", vocals) == std::vector<int64_t>{take});
	f.call("history.undo");
	f.call("history.undo");
	f.call("history.undo");
	CHECK(f.names(vocals) == std::vector<std::string>{"v1"});
	CHECK(f.on("audioEvents", vocals) == std::vector<int64_t>{take});
	f.call("history.redo");
	CHECK(f.active(guitar) == id);
	CHECK(f.call("project.get").at("audioEvents").empty());

	CHECK_THAT(f.fails("trackVersion.activate", {{"tracks", {vocals}}, {"version", 99}}),
		ContainsSubstring("none of the tracks"));
	const auto folder = f.call("track.add", {{"type", "folder"}}).at("track").at("id").get<int64_t>();
	CHECK_THAT(f.fails("trackVersion.add", {{"tracks", {folder}}}), ContainsSubstring("has no versions"));
	CHECK(!f.call("project.get").at("tracks").back().contains("versions"));
	CHECK_THAT(f.fails("trackVersion.add", {{"tracks", {12345}}}), ContainsSubstring("no track"));
}

TEST_CASE("Duplicate Version copies the active version's events and parts with new ids", "[unit]")
{
	Fixture f;
	const int64_t clip = f.clip();
	const int64_t audio = f.track("audio");
	const int64_t first = f.event(audio, clip, 0, 0, 1000);
	const int64_t second = f.event(audio, clip, 500, 1000, 1000);
	f.call("crossfade.set", {{"from", first}, {"to", second}});
	const int64_t midi = f.track("midi", "musical");
	const auto part = f.call("midiPart.add", {{"track", midi}, {"position", 0}, {"length", 4 * g_quarter}})
						  .at("part")
						  .at("id")
						  .get<int64_t>();
	f.call("note.add",
		{{"part", part}, {"notes", {{{"position", 0}, {"length", g_quarter}, {"pitch", 60}, {"velocity", 1}}}}});

	f.call("trackVersion.duplicate", {{"tracks", {audio, midi}}});
	CHECK(f.call("history.get").at("steps").back().at("action") == "Duplicate Version");
	const std::vector<int64_t> copies = f.on("audioEvents", audio);
	REQUIRE(copies.size() == 2);
	CHECK(copies.at(0) > second);
	const Json project = f.call("project.get");
	for (const Json& event : project.at("audioEvents"))
		if (event.at("id") == copies.at(1))
		{
			CHECK(event.at("position") == 500);
			CHECK(event.at("crossfade").at("from") == copies.at(0)); // joins the copy
		}
	const std::vector<int64_t> parts = f.on("midiParts", midi);
	REQUIRE(parts.size() == 1);
	CHECK(parts.at(0) != part);
	for (const Json& item : project.at("midiParts"))
		CHECK(item.at("notes").size() == 1);

	// Editing the copy leaves the original as it was.
	f.call("event.remove", {{"events", {copies.at(0), parts.at(0)}}});
	f.call("trackVersion.activate", {{"tracks", {audio, midi}}, {"version", 1}});
	CHECK(f.on("audioEvents", audio) == std::vector<int64_t>{first, second});
	CHECK(f.on("midiParts", midi) == std::vector<int64_t>{part});
}

TEST_CASE("versions get default names, can be renamed and deleted", "[unit]")
{
	Fixture f;
	const int64_t clip = f.clip();
	const int64_t track = f.track("audio");
	const int64_t original = f.event(track, clip, 0, 0, 1000);
	f.call("trackVersion.add", {{"tracks", {track}}});
	f.call("trackVersion.add", {{"tracks", {track}}, {"name", "Chorus"}});
	CHECK(f.names(track) == std::vector<std::string>{"v1", "v2", "Chorus"});
	f.call("trackVersion.set", {{"tracks", {track}}, {"name", "Bridge"}});
	CHECK(f.names(track) == std::vector<std::string>{"v1", "v2", "Bridge"});
	CHECK(f.call("history.get").at("steps").back().at("action") == "Rename Version");
	CHECK(!f.fails("trackVersion.set", {{"tracks", {track}}, {"name", ""}}).empty());

	// Delete Version activates the version before the deleted one, the next one for the first.
	const int bridge = f.active(track);
	f.event(track, clip, 0, 2000, 500);
	f.call("trackVersion.remove", {{"tracks", {track}}});
	CHECK(f.names(track) == std::vector<std::string>{"v1", "v2"});
	CHECK(f.active(track) != bridge);
	CHECK(f.on("audioEvents", track).empty()); // v2 is empty
	f.call("trackVersion.activate", {{"tracks", {track}}, {"version", 1}});
	f.call("trackVersion.remove", {{"tracks", {track}}});
	CHECK(f.names(track) == std::vector<std::string>{"v2"});
	CHECK(f.call("project.get").at("audioEvents").empty());
	// A new version counts on from the number of versions.
	f.call("trackVersion.add", {{"tracks", {track}}});
	CHECK(f.names(track) == std::vector<std::string>{"v2", "v3"});
	f.call("trackVersion.remove", {{"tracks", {track}}});
	CHECK_THAT(f.fails("trackVersion.remove", {{"tracks", {track}}}), ContainsSubstring("only one"));

	// Undo brings the deleted versions back with their events.
	f.call("history.undo");
	f.call("history.undo");
	f.call("history.undo");
	CHECK(f.names(track) == std::vector<std::string>{"v1", "v2"});
	CHECK(f.on("audioEvents", track) == std::vector<int64_t>{original});
}

TEST_CASE("Assign Common Version ID lets versions made apart be activated together", "[unit]")
{
	Fixture f;
	const int64_t a = f.track("audio");
	const int64_t b = f.track("instrument", "musical");
	f.call("trackVersion.add", {{"tracks", {a}}});
	f.call("trackVersion.add", {{"tracks", {b}}});
	CHECK(f.active(a) != f.active(b));
	f.call("trackVersion.assignCommonId", {{"tracks", {a, b}}});
	const int common = f.active(a);
	CHECK(f.active(b) == common);
	CHECK(common > 3);
	f.call("trackVersion.activate", {{"tracks", {a, b}}, {"version", 1}});
	CHECK(f.active(b) == 1);
	f.call("trackVersion.activate", {{"tracks", {a, b}}, {"version", common}});
	CHECK(f.active(a) == common);
	CHECK(f.active(b) == common);
}

TEST_CASE("the project file, track.duplicate, time bases and the pool keep inactive versions", "[integration]")
{
	Fixture f;
	const int64_t clip = f.clip();
	const int64_t track = f.track("audio", "musical");
	const int64_t original = f.event(track, clip, g_quarter, 0, 1000);
	const int64_t midi = f.track("midi", "musical");
	f.call("midiPart.add", {{"track", midi}, {"position", g_quarter}, {"length", 4 * g_quarter}});
	f.call("trackVersion.add", {{"tracks", {track, midi}}});
	const int second = f.active(track);

	// The pool counts the inactive version's event: its clip stays.
	CHECK(f.call("pool.removeUnused").at("clips").empty());
	CHECK_THAT(f.fails("clip.remove", {{"clip", clip}}), ContainsSubstring("used by audio event"));
	// track.removeEmpty keeps a track with versions.
	CHECK(f.call("track.removeEmpty").at("tracks").empty());

	// A new time base moves the inactive version's events too: at 120 BPM a quarter is 24000 samples.
	f.call("track.set", {{"track", track}, {"timeBase", "linear"}});

	const std::string path = (f.dir.path() / "versions.dawproject").u8string();
	f.call("project.save", {{"path", path}, {"embedMedia", false}});
	f.call("project.load", {{"path", path}});
	CHECK(f.names(track) == std::vector<std::string>{"v1", "v2"});
	CHECK(f.active(track) == second);
	CHECK(f.call("project.get").at("audioEvents").empty());
	f.call("trackVersion.activate", {{"tracks", {track, midi}}, {"version", 1}});
	const Json project = f.call("project.get");
	REQUIRE(project.at("audioEvents").size() == 1);
	CHECK(project.at("audioEvents").at(0).at("id") == original);
	CHECK(project.at("audioEvents").at(0).at("position") == 24000);
	REQUIRE(project.at("midiParts").size() == 1);
	CHECK(project.at("midiParts").at(0).at("position") == g_quarter);
	// New ids come after those of the inactive versions.
	f.call("trackVersion.activate", {{"tracks", {track}}, {"version", second}});
	CHECK(f.event(track, clip, 0, 0, 100) > original);

	// A copy of the track has copies of every version, with ids of their own.
	const auto copy = f.call("track.duplicate", {{"track", track}}).at("track").at("id").get<int64_t>();
	CHECK(f.names(copy) == std::vector<std::string>{"v1", "v2"});
	f.call("trackVersion.activate", {{"tracks", {copy}}, {"version", 1}});
	const std::vector<int64_t> copied = f.on("audioEvents", copy);
	REQUIRE(copied.size() == 1);
	CHECK(copied.at(0) != original);
	f.call("trackVersion.activate", {{"tracks", {track}}, {"version", 1}});
	CHECK(f.on("audioEvents", track) == std::vector<int64_t>{original});
}

TEST_CASE("media.relinkResampled rescales the events of inactive versions too", "[integration]")
{
	Fixture f;
	const int64_t clip = f.clip();
	const int64_t track = f.track("audio");
	const int64_t first = f.event(track, clip, 0, 100, 1000);
	const int64_t second = f.event(track, clip, 900, 1200, 1000);
	f.call("crossfade.set", {{"from", first}, {"to", second}, {"length", 50}, {"outLength", 80}});
	const Json before = f.call("project.get").at("audioEvents");
	REQUIRE(before.size() == 2);
	f.call("trackVersion.add", {{"tracks", {track}}});

	// The ramp file's 96 kHz copy, twice the frames.
	gnubase::test::Audio audio;
	audio.sampleRate = 96000;
	audio.samples.assign(10000, 0.0f);
	const std::filesystem::path file = f.dir.path() / "ramp96.wav";
	gnubase::test::writeWav(file, audio);
	const Json media = f.call("project.get").at("media").at(0).at("id");
	f.call("media.relinkResampled", {{"files", {{{"media", media}, {"path", file.u8string()}}}}});

	f.call("trackVersion.activate", {{"tracks", {track}}, {"version", 1}});
	const Json events = f.call("project.get").at("audioEvents");
	REQUIRE(events.size() == 2);
	CHECK(events.at(0).at("offset") == 200);
	CHECK(events.at(0).at("length") == 2000);
	CHECK(events.at(1).at("offset") == 2 * before.at(1).at("offset").get<int64_t>());
	const auto outLength = before.at(1).at("crossfade").at("outLength").get<int64_t>();
	CHECK(outLength == 80);
	CHECK(events.at(1).at("crossfade").at("outLength") == 2 * outLength);
}

TEST_CASE("a tempo change retimes the linear MIDI parts of inactive versions too", "[unit]")
{
	Fixture f;
	constexpr int64_t quarterSamples = 24000; // at 48 kHz and 120 BPM
	const auto addPart = [&f](int64_t track)
	{
		const Json part =
			f.call("midiPart.add", {{"track", track}, {"position", 8 * quarterSamples}, {"length", 4 * quarterSamples}})
				.at("part");
		f.call("note.add",
			{{"part", part.at("id")},
				{"notes", {{{"position", g_quarter}, {"length", g_quarter}, {"pitch", 60}, {"velocity", 1.0}}}}});
	};
	const int64_t versioned = f.track("midi");
	const int64_t plain = f.track("midi");
	addPart(versioned);
	addPart(plain);
	f.call("trackVersion.add", {{"tracks", {versioned}}});
	f.call("project.set", {{"tempo", 60.0}});
	f.call("trackVersion.activate", {{"tracks", {versioned}}, {"version", 1}});

	const auto note = [&f](int64_t track) -> std::vector<int64_t>
	{
		const Json project = f.call("project.get");
		for (const Json& part : project.at("midiParts"))
			if (part.at("track") == track)
			{
				const Json& found = part.at("notes").at(0);
				return std::vector<int64_t>{found.at("position").get<int64_t>(), found.at("length").get<int64_t>()};
			}
		FAIL("no part on track " << track);
	};
	CHECK(note(plain) == std::vector<int64_t>{g_quarter / 2, g_quarter / 2});
	CHECK(note(versioned) == note(plain));
}
