// SPDX-License-Identifier: GPL-3.0-or-later
// The audio part editor's side of the editor views (#146, docs/features/audio-part-editor.md): editor.open with audio
// parts, the Visibility tab's audio tracks and the selected audio events. tests/audioparteditor_ui_tests.cpp drives
// the editor in the app.
#include "engine/engine.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using namespace gnubase;
using engine::Json;

namespace
{
	constexpr int64_t g_second = 48000;

	struct Fixture
	{
		Json call(const std::string& command, Json parameters = Json::object())
		{
			const engine::Result result = client.call(command, std::move(parameters));
			INFO(command << " failed: " << result.error);
			REQUIRE(result.ok());
			return result.value;
		}

		std::string fails(const std::string& command, Json parameters)
		{
			const engine::Result result = client.call(command, std::move(parameters));
			REQUIRE(!result.ok());
			return result.error;
		}

		uint64_t track(const std::string& type)
		{
			return call("track.add", {{"type", type}, {"timeBase", "linear"}}).at("track").at("id").get<uint64_t>();
		}

		// An audio part from position for two seconds, with an audio event of a second at its start.
		std::pair<uint64_t, uint64_t> part(uint64_t onTrack, int64_t position)
		{
			if (clip == 0)
			{
				const Json media = call("media.add",
					{{"path", "take.wav"}, {"sampleRate", 48000}, {"channels", 1}, {"length", 10 * g_second}})
									   .at("media");
				clip = call("clip.add", {{"media", media.at("id")}}).at("clip").at("id").get<uint64_t>();
			}
			const auto event = call("audioEvent.add",
				{{"track", onTrack}, {"clip", clip}, {"position", position}, {"offset", 0}, {"length", g_second}})
								   .at("event")
								   .at("id")
								   .get<uint64_t>();
			const auto made = call("event.toPart", {{"events", {event}}}).at("events").at(0).get<uint64_t>();
			call("event.resize", {{"events", {made}}, {"edge", "end"}, {"delta", g_second}});
			return {made, event};
		}

		Json view() { return call("editor.get", {{"editor", "window"}}); }

		engine::Engine engine;
		engine::Client client{engine};
		uint64_t clip = 0;
	};

	std::vector<uint64_t> ids(const Json& json)
	{
		return json.get<std::vector<uint64_t>>();
	}
}

TEST_CASE("an editor opens audio parts in the audio part editor, in the track list's order", "[unit]")
{
	Fixture f;
	const uint64_t vocals = f.track("audio");
	const uint64_t drums = f.track("audio");
	const auto [drumPart, drumEvent] = f.part(drums, 0);
	const auto [later, laterEvent] = f.part(vocals, 4 * g_second);
	const auto [first, firstEvent] = f.part(vocals, 0);

	const Json opened = f.call("editor.open", {{"editor", "window"}, {"parts", {drumPart, later, first}}});
	CHECK(opened.at("content") == "audioPart");
	CHECK(ids(opened.at("parts")) == std::vector<uint64_t>{first, later, drumPart});
	CHECK(opened.at("activePart") == first);
	CHECK(opened.at("activeTrack") == vocals);
	CHECK(opened.at("eventSelection").empty());

	// Audio parts show only in the audio part editor, and an editor shows parts of one kind.
	const uint64_t keys = f.track("midi");
	const auto midiPart = f.call("midiPart.add", {{"track", keys}, {"position", 0}, {"length", 1920}})
							  .at("part")
							  .at("id")
							  .get<uint64_t>();
	CHECK_THAT(f.fails("editor.open", {{"editor", "window"}, {"parts", {first, midiPart}}}),
		ContainsSubstring("all MIDI parts or all audio parts"));
	CHECK_THAT(f.fails("editor.open", {{"editor", "lower"}, {"parts", {first}}, {"content", "key"}}),
		ContainsSubstring("audio part editor"));
	CHECK_THAT(f.fails("editor.open", {{"editor", "lower"}, {"parts", {midiPart}}, {"content", "audioPart"}}),
		ContainsSubstring("audio part editor"));
	CHECK_THAT(
		f.fails("editor.open", {{"editor", "lower"}, {"parts", {firstEvent}}}), ContainsSubstring("no MIDI part"));
	CHECK_THAT(
		f.fails("editor.set", {{"editor", "window"}, {"content", "list"}}), ContainsSubstring("audio part editor"));
	CHECK_THAT(f.fails("editor.set", {{"editor", "window"}, {"parts", {midiPart}}}), ContainsSubstring("audio part"));
	CHECK_THAT(f.fails("editor.input", {{"editor", "window"}, {"notes", {{{"pitch", 60}}}}}),
		ContainsSubstring("audio part editor"));

	// MIDI parts open in the key editor again.
	CHECK(f.call("editor.open", {{"editor", "window"}, {"parts", {midiPart}}}).at("content") == "key");
	CHECK(f.call("editor.open", {{"editor", "window"}, {"parts", {first}}}).at("content") == "audioPart");

	// The Visibility tab shows and hides the audio parts of audio tracks.
	Json shown = f.call("editor.set", {{"editor", "window"}, {"showTracks", {drums}}});
	CHECK(ids(shown.at("parts")) == std::vector<uint64_t>{first, drumPart});
	CHECK(shown.at("activePart") == drumPart);
	shown = f.call("editor.set", {{"editor", "window"}, {"exclusive", true}, {"showTracks", {vocals}}});
	CHECK(ids(shown.at("parts")) == std::vector<uint64_t>{first, later});
	CHECK_THAT(
		f.fails("editor.set", {{"editor", "window"}, {"showTracks", {keys}}}), ContainsSubstring("no audio parts"));

	// A removed part leaves the editor, and comes back with an undo.
	f.call("event.remove", {{"events", {later}}});
	CHECK(ids(f.view().at("parts")) == std::vector<uint64_t>{first});
	f.call("history.undo");
	CHECK(ids(f.view().at("parts")) == std::vector<uint64_t>{first, later});
}

TEST_CASE("the audio part editor's selection is audio events of its editable parts", "[unit]")
{
	Fixture f;
	const uint64_t vocals = f.track("audio");
	const auto [first, firstEvent] = f.part(vocals, 0);
	const auto [second, secondEvent] = f.part(vocals, 4 * g_second);
	const uint64_t outside =
		f.call("audioEvent.add",
			 {{"track", vocals}, {"clip", f.clip}, {"position", 8 * g_second}, {"offset", 0}, {"length", g_second}})
			.at("event")
			.at("id")
			.get<uint64_t>();
	f.call("editor.open", {{"editor", "window"}, {"parts", {first, second}}});

	const Json selected =
		f.call("editor.set", {{"editor", "window"}, {"eventSelection", {secondEvent, firstEvent, secondEvent}}});
	CHECK(ids(selected.at("eventSelection")) == std::vector<uint64_t>{secondEvent, firstEvent});
	CHECK_THAT(f.fails("editor.set", {{"editor", "window"}, {"eventSelection", {outside}}}),
		ContainsSubstring("isn't in an editable part"));
	CHECK_THAT(f.fails("editor.set", {{"editor", "window"}, {"eventSelection", {first}}}),
		ContainsSubstring("isn't in an editable part"));

	// Only the active part's events stay selected in Active Part mode (p. 702), and removed events leave.
	CHECK(ids(f.call("editor.set", {{"editor", "window"}, {"editingMode", "activePart"}}).at("eventSelection")) ==
		std::vector<uint64_t>{firstEvent});
	f.call("editor.set", {{"editor", "window"}, {"editingMode", "allParts"}, {"eventSelection", {firstEvent}}});
	f.call("event.remove", {{"events", {firstEvent}}});
	CHECK(f.view().at("eventSelection").empty());

	// The selection is view state: no undo step; closing the editor clears it.
	const Json history = f.call("history.get");
	f.call("editor.set", {{"editor", "window"}, {"eventSelection", {secondEvent}}});
	CHECK(f.call("history.get") == history);
	CHECK(f.call("editor.close", {{"editor", "window"}}).at("eventSelection").empty());
}

TEST_CASE("editor.set refuses notes of audio parts and shows the tracks of a content given with it", "[unit]")
{
	Fixture f;
	const uint64_t vocals = f.track("audio");
	const auto [audioPart, audioEvent] = f.part(vocals, 0);
	const uint64_t keys = f.track("midi");
	const uint64_t bass = f.track("midi");
	const auto midiPart = [&](uint64_t track)
	{
		return f.call("midiPart.add", {{"track", track}, {"position", 0}, {"length", 1920}})
			.at("part")
			.at("id")
			.get<uint64_t>();
	};
	const uint64_t keysPart = midiPart(keys);
	const uint64_t bassPart = midiPart(bass);
	f.call("editor.open", {{"editor", "window"}, {"parts", {audioPart}}});

	// An audio part's notes and controller events: a refusal that names it, not a lookup that fails.
	CHECK_THAT(f.fails("editor.set", {{"editor", "window"}, {"selection", {{{"part", audioPart}, {"note", 1}}}}}),
		ContainsSubstring("part " + std::to_string(audioPart) + " isn't a MIDI part"));
	CHECK_THAT(f.fails("editor.set",
				   {{"editor", "window"}, {"controllerSelection", {{{"part", audioPart}, {"controller", 1}}}}}),
		ContainsSubstring("part " + std::to_string(audioPart) + " isn't a MIDI part"));
	CHECK(f.view().at("parts") == Json::array({audioPart}));

	// showTracks shows the parts of the content given with it: the key editor's MIDI parts.
	const Json shown = f.call(
		"editor.set", {{"editor", "window"}, {"parts", {keysPart}}, {"content", "key"}, {"showTracks", {bass}}});
	CHECK(shown.at("content") == "key");
	CHECK(ids(shown.at("parts")) == std::vector<uint64_t>{keysPart, bassPart});
	// And the other way round, the audio part editor's audio parts.
	const Json back = f.call("editor.set",
		{{"editor", "window"}, {"exclusive", true}, {"content", "audioPart"}, {"showTracks", {vocals}}});
	CHECK(back.at("content") == "audioPart");
	CHECK(ids(back.at("parts")) == std::vector<uint64_t>{audioPart});
}
