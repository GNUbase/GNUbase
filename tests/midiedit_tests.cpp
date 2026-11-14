// SPDX-License-Identifier: GPL-3.0-or-later
// Changing the notes and controllers of a MIDI part (#27): note.set, note.move and controller.set.
#include "engine/engine.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using gnubase::engine::Client;
using gnubase::engine::Engine;
using gnubase::engine::Json;

namespace
{
	Json call(Client& client, const std::string& command, const Json& parameters = Json::object())
	{
		const gnubase::engine::Result result = client.call(command, parameters);
		INFO(command << ": " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	// The data of the first event with the name among those that arrived, or null.
	Json takeEvent(Client& client, const std::string& name)
	{
		Json found;
		while (const std::optional<gnubase::engine::Message> message = client.poll())
		{
			const auto* event = std::get_if<gnubase::engine::Event>(&*message);
			if (event != nullptr && event->name == name && found.is_null())
				found = event->data;
		}
		return found;
	}

	// A MIDI part with a C and an E; returns the part's id.
	int64_t partWithNotes(Client& client)
	{
		const Json track = call(client, "track.add", {{"type", "midi"}}).at("track");
		const Json part =
			call(client, "midiPart.add", {{"track", track.at("id")}, {"position", 0}, {"length", 3840}}).at("part");
		call(client, "note.add",
			{{"part", part.at("id")},
				{"notes",
					{{{"position", 0}, {"length", 480}, {"pitch", 60}, {"velocity", 1.0}},
						{{"position", 480}, {"length", 480}, {"pitch", 64}, {"velocity", 0.5}, {"channel", 1}}}}});
		return part.at("id").get<int64_t>();
	}

	Json storedNotes(Client& client)
	{
		return call(client, "project.get").at("midiParts").at(0).at("notes");
	}
}

TEST_CASE("note.set changes notes of a MIDI part as one undo step", "[unit]")
{
	Engine engine;
	Client client(engine);
	const int64_t part = partWithNotes(client);
	const Json before = storedNotes(client);
	const int64_t c = before.at(0).at("id").get<int64_t>();
	const int64_t e = before.at(1).at("id").get<int64_t>();
	takeEvent(client, "");

	// Only what is given changes.
	const Json changed = call(client, "note.set",
		{{"part", part},
			{"notes",
				{{{"id", c}, {"position", 960}, {"pitch", 72}, {"velocity", 0.0}, {"releaseVelocity", 1.0},
					 {"channel", 9}},
					{{"id", e}, {"length", 240}}}}})
							 .at("notes");
	REQUIRE(changed.size() == 2);
	CHECK(changed[0] ==
		Json{{"id", c}, {"position", 960}, {"length", 480}, {"pitch", 72}, {"velocity", 0.0}, {"releaseVelocity", 1.0},
			{"channel", 9}});
	CHECK(changed[1].at("length") == 240);
	CHECK(changed[1].at("position") == 480);
	CHECK(changed[1].at("pitch") == 64);
	CHECK(changed[1].at("channel") == 1);
	CHECK_THAT(changed[1].at("velocity").get<double>(), WithinAbs(0.5, 1e-4));
	CHECK(storedNotes(client) == changed);
	CHECK(takeEvent(client, "note.changed") == Json{{"part", part}, {"notes", changed}});

	const Json history = call(client, "history.get");
	CHECK(history.at("steps").back().at("action") == "Change notes");
	call(client, "history.undo");
	CHECK(storedNotes(client) == before);
	call(client, "history.redo");
	CHECK(storedNotes(client) == changed);

	// A note may be changed inside a transaction too.
	call(client, "history.transaction",
		{{"description", "Transpose"},
			{"commands",
				{{{"command", "note.set"},
					{"parameters", {{"part", part}, {"notes", {{{"id", e}, {"pitch", 65}}}}}}}}}});
	CHECK(storedNotes(client).at(1).at("pitch") == 65);
}

TEST_CASE("note.set checks every change before it changes anything", "[unit]")
{
	Engine engine;
	Client client(engine);
	const int64_t part = partWithNotes(client);
	const Json before = storedNotes(client);
	const int64_t c = before.at(0).at("id").get<int64_t>();
	const auto fails = [&](const Json& note)
	{ return client.call("note.set", {{"part", part}, {"notes", {{{"id", c}, {"pitch", 61}}, note}}}).error; };

	CHECK_THAT(fails({{"id", 999}}), ContainsSubstring("notes[1]: the part has no note 999"));
	CHECK_THAT(fails({{"id", c}, {"pitch", 128}}), ContainsSubstring("notes[1].pitch must be 0 to 127"));
	CHECK_THAT(fails({{"id", c}, {"channel", 16}}), ContainsSubstring("notes[1].channel must be 0 to 15"));
	CHECK_THAT(fails({{"id", c}, {"length", 0}}), ContainsSubstring("notes[1].length must be positive"));
	CHECK_THAT(fails({{"id", c}, {"position", -1}}), ContainsSubstring("notes[1].position must not be negative"));
	CHECK_THAT(fails({{"id", c}, {"velocity", 1.5}}), ContainsSubstring("notes[1].velocity must be 0 to 1"));
	CHECK_THAT(client.call("note.set", {{"part", 999}, {"notes", Json::array()}}).error,
		ContainsSubstring("no MIDI part 999"));
	CHECK(storedNotes(client) == before); // the valid first change didn't happen either
}

namespace
{
	// The notes of one part as project.get lists them.
	Json notesOf(Client& client, int64_t part)
	{
		const Json project = call(client, "project.get"); // the answer must outlive the loop over its parts
		for (const Json& each : project.at("midiParts"))
			if (each.at("id") == part)
				return each.at("notes");
		return Json::array();
	}

	// The data of the first event of each of the names that arrived, by name.
	std::map<std::string, Json> takeEvents(Client& client, const std::vector<std::string>& names)
	{
		std::map<std::string, Json> found;
		while (const std::optional<gnubase::engine::Message> message = client.poll())
		{
			const auto* event = std::get_if<gnubase::engine::Event>(&*message);
			if (event != nullptr && std::find(names.begin(), names.end(), event->name) != names.end() &&
				found.count(event->name) == 0)
				found[event->name] = event->data;
		}
		return found;
	}

	constexpr int64_t g_sixteenth = 1720320 / 4;
}

TEST_CASE("note.move moves notes to another part as one undo step, keeping every field and their ids", "[unit]")
{
	Engine engine;
	Client client(engine);
	const int64_t track = call(client, "track.add", {{"type", "midi"}}).at("track").at("id").get<int64_t>();
	const int64_t other = call(client, "track.add", {{"type", "instrument"}}).at("track").at("id").get<int64_t>();
	const int64_t source =
		call(client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", 8 * g_sixteenth}})
			.at("part")
			.at("id")
			.get<int64_t>();
	const int64_t target =
		call(client, "midiPart.add", {{"track", other}, {"position", 0}, {"length", 8 * g_sixteenth}})
			.at("part")
			.at("id")
			.get<int64_t>();
	// The first note has every field a note has; the second is plain.
	const Json added = call(client, "note.add",
		{{"part", source},
			{"notes",
				{{{"position", 100000}, {"length", g_sixteenth}, {"pitch", 60}, {"velocity", 0.75},
					 {"releaseVelocity", 0.25}, {"channel", 9}, {"probability", 0.5}, {"velocityVariance", -0.25},
					 {"articulations", Json::array({"Pizzicato"})},
					 {"expressions", Json::array({{{"type", "volume"}, {"position", 120}, {"value", 0.5}}})}},
					{{"position", g_sixteenth}, {"length", g_sixteenth / 2}, {"pitch", 64}, {"velocity", 0.5}}}}})
						   .at("notes");
	const int64_t first = added.at(0).at("id").get<int64_t>();
	const int64_t second = added.at(1).at("id").get<int64_t>();
	// Quantizing gives the first note, which the grid moves, its origin, one more field the move must keep.
	call(client, "quantize.apply", {{"part", source}, {"settings", {{"grid", "1/16"}}}});
	const Json before = notesOf(client, source);
	REQUIRE(before.size() == 2);
	REQUIRE(before.at(0).contains("origin"));
	takeEvent(client, "");

	const Json moved = call(client, "note.move",
		{{"part", source}, {"notes", {first, second}}, {"target", target}, {"ticks", g_sixteenth}, {"semitones", 5}})
						   .at("notes");
	// Every note as it was, at the drop's time and pitch, with its id.
	Json expected = before;
	for (Json& note : expected)
	{
		note["position"] = note.at("position").get<int64_t>() + g_sixteenth;
		note["pitch"] = note.at("pitch").get<int>() + 5;
	}
	CHECK(moved == expected);
	CHECK(moved.at(0).at("id") == first);
	CHECK(moved.at(1).at("id") == second);
	// The notes kept their distance to one another.
	CHECK(expected.at(1).at("position").get<int64_t>() - expected.at(0).at("position").get<int64_t>() ==
		before.at(1).at("position").get<int64_t>() - before.at(0).at("position").get<int64_t>());
	REQUIRE(notesOf(client, target) == expected);
	CHECK(notesOf(client, source).empty());
	const std::map<std::string, Json> events = takeEvents(client, {"note.removed", "note.added"});
	CHECK(events.at("note.removed") == Json{{"part", source}, {"notes", {first, second}}});
	CHECK(events.at("note.added") == Json{{"part", target}, {"notes", expected}});

	const Json history = call(client, "history.get");
	CHECK(history.at("steps").back().at("action") == "Move notes");
	call(client, "history.undo");
	CHECK(notesOf(client, source) == before);
	CHECK(notesOf(client, target).empty());
	call(client, "history.redo");
	CHECK(notesOf(client, source).empty());
	CHECK(notesOf(client, target) == expected);
}

TEST_CASE("note.move refuses what doesn't fit and changes nothing then", "[unit]")
{
	Engine engine;
	Client client(engine);
	const int64_t track = call(client, "track.add", {{"type", "midi"}}).at("track").at("id").get<int64_t>();
	const int64_t other = call(client, "track.add", {{"type", "instrument"}}).at("track").at("id").get<int64_t>();
	const int64_t source =
		call(client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", 8 * g_sixteenth}})
			.at("part")
			.at("id")
			.get<int64_t>();
	const int64_t target =
		call(client, "midiPart.add", {{"track", other}, {"position", 0}, {"length", 8 * g_sixteenth}})
			.at("part")
			.at("id")
			.get<int64_t>();
	const Json added = call(client, "note.add",
		{{"part", source},
			{"notes",
				{{{"position", 0}, {"length", g_sixteenth}, {"pitch", 60}, {"velocity", 1.0}},
					{{"position", g_sixteenth}, {"length", g_sixteenth}, {"pitch", 64}, {"velocity", 0.5}}}}})
						   .at("notes");
	const int64_t first = added.at(0).at("id").get<int64_t>();
	const int64_t second = added.at(1).at("id").get<int64_t>();
	const Json before = notesOf(client, source);
	const auto move = [&](const Json& parameters) { return client.call("note.move", parameters).error; };

	CHECK_THAT(move({{"part", source}, {"notes", {first}}, {"target", 999}}), ContainsSubstring("no MIDI part 999"));
	CHECK_THAT(move({{"part", 999}, {"notes", {first}}, {"target", target}}), ContainsSubstring("no MIDI part 999"));
	CHECK_THAT(move({{"part", source}, {"notes", {999}}, {"target", target}}),
		ContainsSubstring("notes[0]: the part has no note 999"));
	CHECK_THAT(move({{"part", source}, {"notes", {first}}, {"target", target}, {"ticks", -1}}),
		ContainsSubstring("notes[0] would land before the target part's start"));
	CHECK_THAT(move({{"part", source}, {"notes", {first}}, {"target", target}, {"semitones", 68}}),
		ContainsSubstring("notes[0] would land outside the pitches"));
	CHECK_THAT(move({{"part", source}, {"notes", {first}}, {"target", target}, {"semitones", -61}}),
		ContainsSubstring("notes[0] would land outside the pitches"));
	// A valid note with an invalid one moves nothing.
	CHECK_THAT(move({{"part", source}, {"notes", {first, 999}}, {"target", target}}),
		ContainsSubstring("notes[1]: the part has no note 999"));
	CHECK(notesOf(client, source) == before);
	CHECK(notesOf(client, target).empty());
	// The shared copies of the source and the target must stay equal, so a move between them is refused.
	const int64_t copy =
		call(client, "event.copy", {{"events", {source}}, {"ticks", 8 * g_sixteenth}, {"shared", true}})
			.at("events")
			.at(0)
			.get<int64_t>();
	REQUIRE(notesOf(client, copy) == before);
	CHECK_THAT(move({{"part", source}, {"notes", {first}}, {"target", copy}}),
		ContainsSubstring("shared copies of one group"));
	CHECK(notesOf(client, source) == before);
	CHECK(notesOf(client, copy) == before);

	// A note may reach past the target part's end, as note.add and note.set allow, and a move within the source
	// part itself just offsets the notes, keeping their ids.
	CHECK(call(client, "note.move",
			  {{"part", source}, {"notes", {second}}, {"target", target}, {"ticks", 8 * g_sixteenth}})
			  .at("notes")
			  .at(0)
			  .at("position") == 8 * g_sixteenth + g_sixteenth);
	CHECK(call(client, "note.move", {{"part", source}, {"notes", {first}}, {"target", source}, {"ticks", 120}})
			  .at("notes")
			  .at(0)
			  .at("id") == first);
	CHECK(notesOf(client, source).at(0).at("position") == 120);
}

TEST_CASE("note.move keeps the shared copies of the source and of the target consistent", "[unit]")
{
	Engine engine;
	Client client(engine);
	const int64_t track = call(client, "track.add", {{"type", "midi"}}).at("track").at("id").get<int64_t>();
	const int64_t other = call(client, "track.add", {{"type", "instrument"}}).at("track").at("id").get<int64_t>();
	const int64_t source =
		call(client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", 8 * g_sixteenth}})
			.at("part")
			.at("id")
			.get<int64_t>();
	const int64_t target =
		call(client, "midiPart.add", {{"track", other}, {"position", 0}, {"length", 8 * g_sixteenth}})
			.at("part")
			.at("id")
			.get<int64_t>();
	const Json added = call(client, "note.add",
		{{"part", source},
			{"notes", Json::array({{{"position", 0}, {"length", g_sixteenth}, {"pitch", 60}, {"velocity", 1.0}}})}})
						   .at("notes");
	const int64_t note = added.at(0).at("id").get<int64_t>();
	const int64_t sourceCopy =
		call(client, "event.copy", {{"events", {source}}, {"ticks", 8 * g_sixteenth}, {"shared", true}})
			.at("events")
			.at(0)
			.get<int64_t>();
	const int64_t targetCopy =
		call(client, "event.copy", {{"events", {target}}, {"ticks", 8 * g_sixteenth}, {"shared", true}})
			.at("events")
			.at(0)
			.get<int64_t>();
	REQUIRE(notesOf(client, sourceCopy).size() == 1);
	REQUIRE(notesOf(client, targetCopy).empty());

	const Json moved = call(client, "note.move", {{"part", source}, {"notes", {note}}, {"target", target}}).at("notes");
	// The copy of the source loses the note with it, and the copy of the target get it with its id.
	CHECK(notesOf(client, sourceCopy).empty());
	REQUIRE(notesOf(client, targetCopy) == moved);
	CHECK(moved.at(0).at("id") == note);

	call(client, "history.undo");
	CHECK(notesOf(client, sourceCopy).size() == 1);
	CHECK(notesOf(client, targetCopy).empty());
}

TEST_CASE("controller.set changes controllers of a MIDI part as one undo step", "[unit]")
{
	Engine engine;
	Client client(engine);
	const int64_t part = partWithNotes(client);
	const Json added = call(client, "controller.add",
		{{"part", part}, {"controllers", {{{"position", 0}, {"number", 7}, {"value", 1.0}, {"channel", 2}}}}})
						   .at("controllers");
	const int64_t volume = added.at(0).at("id").get<int64_t>();
	takeEvent(client, "");

	const Json changed = call(client, "controller.set",
		{{"part", part}, {"controllers", {{{"id", volume}, {"position", 240}, {"number", 11}, {"value", 0.0}}}}})
							 .at("controllers");
	CHECK(changed ==
		Json::array({{{"id", volume}, {"position", 240}, {"type", "controller"}, {"number", 11}, {"value", 0.0},
			{"channel", 2}}}));
	CHECK(takeEvent(client, "controller.changed") == Json{{"part", part}, {"controllers", changed}});
	CHECK(call(client, "history.get").at("steps").back().at("action") == "Change controllers");
	call(client, "history.undo");
	CHECK(call(client, "project.get").at("midiParts").at(0).at("controllers") == added);

	const auto fails = [&](const Json& controller)
	{ return client.call("controller.set", {{"part", part}, {"controllers", {controller}}}).error; };
	CHECK_THAT(fails({{"id", 999}}), ContainsSubstring("controllers[0]: the part has no controller 999"));
	CHECK_THAT(fails({{"id", volume}, {"number", 128}}), ContainsSubstring("controllers[0].number must be 0 to 127"));
	CHECK_THAT(fails({{"id", volume}, {"value", -0.1}}), ContainsSubstring("controllers[0].value must be 0 to 1"));
	CHECK_THAT(fails({{"id", volume}, {"channel", 16}}), ContainsSubstring("controllers[0].channel must be 0 to 15"));
	CHECK_THAT(
		fails({{"id", volume}, {"position", -1}}), ContainsSubstring("controllers[0].position must not be negative"));
	CHECK(call(client, "project.get").at("midiParts").at(0).at("controllers") == added);
}

TEST_CASE("controller.set checks each change against the controller event's type", "[unit]")
{
	Engine engine;
	Client client(engine);
	const int64_t part = partWithNotes(client);
	const Json added = call(client, "controller.add",
		{{"part", part},
			{"controllers",
				{{{"position", 0}, {"type", "program"}, {"number", 5}, {"bank", 300}},
					{{"position", 10}, {"type", "pitchBend"}, {"value", 1.0}},
					{{"position", 20}, {"type", "sysEx"}, {"data", {0xF0, 0x01, 0xF7}}}}}})
						   .at("controllers");
	const Json program = added.at(0).at("id");
	const Json bend = added.at(1).at("id");
	const Json sysEx = added.at(2).at("id");
	const auto stored = [&] { return call(client, "project.get").at("midiParts").at(0).at("controllers"); };

	const auto fails = [&](const Json& controller)
	{ return client.call("controller.set", {{"part", part}, {"controllers", {controller}}}).error; };
	// A program change has a bank, not a value; what a type doesn't use can't be set.
	CHECK_THAT(fails({{"id", program}, {"value", 0.5}}),
		ContainsSubstring("controllers[0].value doesn't apply to program events"));
	CHECK_THAT(fails({{"id", bend}, {"number", 3}}),
		ContainsSubstring("controllers[0].number doesn't apply to pitchBend events"));
	CHECK_THAT(fails({{"id", sysEx}, {"channel", 3}}),
		ContainsSubstring("controllers[0].channel doesn't apply to sysEx events"));
	CHECK_THAT(fails({{"id", bend}, {"bank", 1}}), ContainsSubstring("controllers[0].bank doesn't apply"));
	CHECK_THAT(fails({{"id", program}, {"bank", 16384}}), ContainsSubstring("controllers[0].bank must be 0 to 16383"));
	CHECK_THAT(fails({{"id", program}, {"number", 128}}), ContainsSubstring("controllers[0].number must be 0 to 127"));
	CHECK_THAT(fails({{"id", sysEx}, {"data", {0xF0, 0x01}}}), ContainsSubstring("0xF7"));
	CHECK_THAT(fails({{"id", bend}, {"type", "program"}}), ContainsSubstring("controllers[0].number is required"));
	CHECK_THAT(fails({{"id", program}, {"type", "sysEx"}}), ContainsSubstring("controllers[0].data is required"));
	CHECK(stored() == added);

	// Types, banks and SysEx data change too. A new type keeps the fields it shares with the old one.
	const Json changed = call(client, "controller.set",
		{{"part", part},
			{"controllers",
				{{{"id", program}, {"number", 6}, {"bank", 2}}, {{"id", sysEx}, {"data", {0xF0, 0x02, 0x03, 0xF7}}},
					{{"id", bend}, {"type", "controller"}, {"number", 1}, {"channel", 4}}}}})
							 .at("controllers");
	CHECK(changed ==
		Json::array({{{"id", program}, {"position", 0}, {"type", "program"}, {"number", 6}, {"bank", 2},
						 {"channel", 0}},
			{{"id", sysEx}, {"position", 20}, {"type", "sysEx"}, {"data", {0xF0, 0x02, 0x03, 0xF7}}},
			{{"id", bend}, {"position", 10}, {"type", "controller"}, {"number", 1}, {"value", 1.0}, {"channel", 4}}}));
	CHECK(stored() == Json::array({changed[0], changed[2], changed[1]})); // by id: program, bend, sysEx

	// A null bank removes it.
	const Json noBank =
		call(client, "controller.set", {{"part", part}, {"controllers", {{{"id", program}, {"bank", nullptr}}}}})
			.at("controllers");
	CHECK(!noBank.at(0).contains("bank"));
	CHECK(noBank.at(0).at("number") == 6);
}
