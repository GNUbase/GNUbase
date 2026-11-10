// SPDX-License-Identifier: GPL-3.0-or-later
// The in-place editor's engine side (#131, docs/features/in-place-editor.md): editor.inPlace opens and closes a MIDI
// track's in-place editor, an editor view named inPlace:<track> that shows every MIDI part of its track.
// tests/inplaceeditor_ui_tests.cpp drives it in the app.
#include "engine/engine.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <mutex>
#include <string>
#include <utility>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using namespace gnubase;
using engine::Json;

namespace
{
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
			return call("track.add", {{"type", type}}).at("track").at("id").get<uint64_t>();
		}

		uint64_t part(uint64_t track, int64_t position)
		{
			return call("midiPart.add", {{"track", track}, {"position", position}, {"length", 1920}})
				.at("part")
				.at("id")
				.get<uint64_t>();
		}

		Json view(uint64_t track) { return call("editor.get", {{"editor", name(track)}}); }

		static std::string name(uint64_t track) { return "inPlace:" + std::to_string(track); }

		engine::Engine engine;
		engine::Client client{engine};
	};

	std::vector<uint64_t> ids(const Json& json)
	{
		return json.get<std::vector<uint64_t>>();
	}
}

TEST_CASE("reading open in-place editors leaves the command log unchanged", "[unit]")
{
	Fixture f;
	const Json initial = f.call("engine.commandLog");
	CHECK(f.call("editor.inPlace").at("tracks").empty());
	CHECK(f.call("engine.commandLog") == initial);
	const uint64_t midi = f.track("midi");
	f.call("editor.inPlace", {{"tracks", {midi}}, {"open", true}});
	const Json changed = f.call("engine.commandLog");
	CHECK(changed.at("commands").back().at("command") == "editor.inPlace");
	CHECK(changed.at("commands").back().at("parameters").at("tracks") == Json::array({midi}));
	CHECK(f.call("editor.inPlace").at("tracks") == Json::array({midi}));
	CHECK(f.call("editor.inPlace", {{"open", false}}).at("tracks") == Json::array({midi}));
	CHECK(f.call("engine.commandLog") == changed);
}

TEST_CASE("the in-place editor opens and closes on MIDI tracks and shows all of a track's parts", "[unit]")
{
	Fixture f;
	std::mutex mutex;
	std::vector<Json> changes;
	const engine::Listener listener(f.engine,
		[&](const engine::Event& event)
		{
			const std::scoped_lock lock(mutex);
			if (event.name == "editor.changed")
				changes.push_back(event.data);
		});
	const uint64_t midi = f.track("midi");
	const uint64_t instrument = f.track("instrument");
	const uint64_t second = f.part(midi, 1920);
	const uint64_t first = f.part(midi, 0);
	f.part(instrument, 0);
	CHECK(f.call("editor.inPlace").at("tracks") == Json::array());
	CHECK_FALSE(f.view(midi).at("open").get<bool>());

	// Opening: the result lists every track whose in-place editor is open, in the track list's order.
	CHECK(f.call("editor.inPlace", {{"tracks", {instrument, midi}}, {"open", true}}).at("tracks") ==
		Json::array({midi, instrument}));
	const Json view = f.view(midi);
	CHECK(view.at("open").get<bool>());
	CHECK(view.at("editor") == Fixture::name(midi));
	CHECK(ids(view.at("parts")) == std::vector<uint64_t>{first, second});
	CHECK(view.at("activePart") == first);
	CHECK(view.at("content") == "key");
	{
		const std::scoped_lock lock(mutex);
		REQUIRE(changes.size() == 2);
		CHECK(changes[0].at("editor") == Fixture::name(instrument));
		CHECK(changes[1].at("editor") == Fixture::name(midi));
	}
	// The other editors stay as they were.
	CHECK_FALSE(f.call("editor.get", {{"editor", "window"}}).at("open").get<bool>());

	// A part added to the track shows; the selection and the key editor's settings work as in the other editors.
	const uint64_t third = f.part(midi, 3840);
	CHECK(ids(f.view(midi).at("parts")) == std::vector<uint64_t>{first, second, third});
	const uint64_t note =
		f.call("note.add",
			 {{"part", second}, {"notes", {{{"position", 0}, {"length", 240}, {"pitch", 60}, {"velocity", 0.5}}}}})
			.at("notes")
			.at(0)
			.at("id")
			.get<uint64_t>();
	const Json set = f.call("editor.set",
		{{"editor", Fixture::name(midi)}, {"selection", {{{"part", second}, {"note", note}}}}, {"activePart", second},
			{"lengthQuantize", "1/8"}});
	CHECK(set.at("selection").size() == 1);
	CHECK(set.at("activePart") == second);
	CHECK(set.at("lengthQuantize") == "1/8");

	// Without open, each track's editor toggles.
	CHECK(f.call("editor.inPlace", {{"tracks", {midi}}}).at("tracks") == Json::array({instrument}));
	CHECK_FALSE(f.view(midi).at("open").get<bool>());
	CHECK(f.view(midi).at("parts") == Json::array());
	CHECK(f.call("editor.inPlace", {{"tracks", {midi, instrument}}}).at("tracks") == Json::array({midi}));
	// editor.close closes it too.
	CHECK_FALSE(f.call("editor.close", {{"editor", Fixture::name(midi)}}).at("open").get<bool>());
	CHECK(f.call("editor.inPlace").at("tracks") == Json::array());
}

TEST_CASE("the in-place editor refuses tracks without notes and changing its parts", "[unit]")
{
	Fixture f;
	const uint64_t midi = f.track("midi");
	const uint64_t audio = f.track("audio");
	const uint64_t other = f.track("midi");
	const uint64_t part = f.part(midi, 0);
	const uint64_t foreign = f.part(other, 0);
	CHECK_THAT(f.fails("editor.inPlace", {{"tracks", {audio}}}), ContainsSubstring("MIDI or instrument track"));
	CHECK_THAT(f.fails("editor.inPlace", {{"tracks", {999}}}), ContainsSubstring("no track 999"));
	CHECK(f.call("editor.inPlace").at("tracks") == Json::array()); // nothing opened
	CHECK_THAT(f.fails("editor.get", {{"editor", "inPlace:" + std::to_string(audio)}}),
		ContainsSubstring("MIDI or instrument track"));
	// The schema names them, for MCP clients and Lua.
	for (const engine::CommandInfo& info : f.engine.commands())
		if (info.name == "editor.get")
			CHECK(info.parameters.at("properties").at("editor").at("pattern") == "^(lower|window|inPlace:[0-9]+)$");
	CHECK_THAT(f.fails("editor.get", {{"editor", "lowerZone"}}), ContainsSubstring("lower|window|inPlace:"));
	CHECK_THAT(f.fails("editor.get", {{"editor", "inPlace:" + std::string(20, '9')}}),
		ContainsSubstring("lower, window or inPlace:"));
	CHECK_THAT(f.fails("editor.open", {{"editor", Fixture::name(midi)}, {"parts", {part}}}),
		ContainsSubstring("editor.inPlace"));

	f.call("editor.inPlace", {{"tracks", {midi}}, {"open", true}});
	CHECK_THAT(f.fails("editor.set", {{"editor", Fixture::name(midi)}, {"parts", {foreign}}}),
		ContainsSubstring("shows its track's parts"));
	CHECK_THAT(f.fails("editor.set", {{"editor", Fixture::name(midi)}, {"showTracks", {other}}}),
		ContainsSubstring("shows its track's parts"));
	CHECK_THAT(f.fails("editor.set", {{"editor", Fixture::name(midi)}, {"globalTracks", Json::array()}}),
		ContainsSubstring("only the editor window"));
	// A track without parts opens with none.
	const uint64_t empty = f.track("instrument");
	CHECK(f.call("editor.inPlace", {{"tracks", {empty}}, {"open", true}}).at("tracks") == Json::array({midi, empty}));
	CHECK(f.view(empty).at("parts") == Json::array());
}

TEST_CASE("a removed track's in-place editor closes, and a new project closes them all", "[unit]")
{
	Fixture f;
	const uint64_t midi = f.track("midi");
	const uint64_t kept = f.track("midi");
	f.part(midi, 0);
	f.call("editor.inPlace", {{"tracks", {midi, kept}}, {"open", true}});
	f.call("track.remove", {{"track", midi}});
	CHECK(f.call("editor.inPlace").at("tracks") == Json::array({kept}));
	f.call("history.undo");
	CHECK(f.call("editor.inPlace").at("tracks") == Json::array({midi, kept}));
	f.call("project.new");
	CHECK(f.call("editor.inPlace").at("tracks") == Json::array());
}
