// SPDX-License-Identifier: GPL-3.0-or-later
// Changing the notes and controllers of a MIDI part (#27): note.set and controller.set.
#include "engine/engine.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

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
				{{{"id", c}, {"position", 960}, {"pitch", 72}, {"velocity", 0.0}, {"channel", 9}},
					{{"id", e}, {"length", 240}}}}})
							 .at("notes");
	REQUIRE(changed.size() == 2);
	CHECK(changed[0] ==
		Json{{"id", c}, {"position", 960}, {"length", 480}, {"pitch", 72}, {"velocity", 0.0}, {"channel", 9}});
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
	CHECK(
		changed == Json::array({{{"id", volume}, {"position", 240}, {"number", 11}, {"value", 0.0}, {"channel", 2}}}));
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
