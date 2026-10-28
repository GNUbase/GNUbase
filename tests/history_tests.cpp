// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;
using gnubase::time::g_ticksPerQuarter;

namespace
{
	// Runs a command that must succeed and returns its result.
	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	// Runs a command that must fail and returns why it failed.
	std::string reject(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " succeeded: " << result.value.dump());
		REQUIRE(!result.ok());
		return result.error;
	}

	ObjectId addTrack(Client& client, const std::string& type = "audio")
	{
		return run(client, "track.add", {{"type", type}}).at("track").at("id").get<ObjectId>();
	}

	int64_t position(Client& client)
	{
		return run(client, "history.get").at("position").get<int64_t>();
	}

	// The names of the events a client received so far.
	std::vector<std::string> eventNames(Client& client)
	{
		std::vector<std::string> names;
		while (const auto message = client.poll())
			if (const auto* event = std::get_if<Event>(&*message))
				names.push_back(event->name);
		return names;
	}

	bool contains(const std::vector<std::string>& names, const std::string& name)
	{
		return std::find(names.begin(), names.end(), name) != names.end();
	}

	// Sends random document commands. Many fail because they pick unsuitable objects or values; that is part of the
	// test, since failed commands must leave no trace in the project or its history.
	class RandomEditor
	{
	public:
		RandomEditor(Client& client, uint32_t seed) : m_client(client), m_random(seed) {}

		void edit()
		{
			const Json project = run(m_client, "project.get");
			// Weights: adding is likelier than removing, so projects grow.
			std::discrete_distribution<int> choose(
				{6, 4, 2, 1, 3, 1, 1, 2, 4, 2, 1, 4, 2, 1, 4, 2, 2, 1, 1, 3, 2, 1, 1, 2});
			switch (choose(m_random))
			{
			case 0:
				addTrack(project);
				break;
			case 1:
				call("track.set",
					{{"track", pick(project, "tracks")}, {"name", "T" + std::to_string(number(99))},
						{"color", chance() ? Json("#2080c0") : Json(nullptr)}, {"timeBase", timeBase()}});
				break;
			case 2:
				call("track.move",
					{{"track", pick(project, "tracks")}, {"parent", folderOrTop(project)}, {"index", number(3)}});
				break;
			case 3:
				call("track.remove", {{"track", pick(project, "tracks")}});
				break;
			case 4:
				call("channel.set", {{"channel", pick(project, "channels")}, {"output", output(project)}});
				break;
			case 5:
				call("bus.add",
					{{"direction", chance() ? "input" : "output"}, {"name", "Bus"}, {"channels", 1 + number(1)}});
				break;
			case 6:
				call("bus.remove", {{"bus", pick(project, "busses")}});
				break;
			case 7:
				addClip();
				break;
			case 8:
				call("audioEvent.add",
					{{"track", pick(project, "tracks", "audio")}, {"clip", pick(project, "clips")},
						{"position", number(8) * g_ticksPerQuarter}, {"offset", number(1000)}});
				break;
			case 9:
				call("audioEvent.set",
					{{"event", pick(project, "audioEvents")}, {"position", number(8) * g_ticksPerQuarter},
						{"length", 1 + number(20000)}});
				break;
			case 10:
				call("audioEvent.remove", {{"event", pick(project, "audioEvents")}});
				break;
			case 11:
				call("midiPart.add",
					{{"track", pick(project, "tracks", chance() ? "midi" : "instrument")},
						{"position", number(8) * g_ticksPerQuarter}, {"length", (1 + number(8)) * g_ticksPerQuarter}});
				break;
			case 12:
				call("midiPart.set",
					{{"part", pick(project, "midiParts")}, {"name", "P" + std::to_string(number(9))},
						{"position", number(8) * g_ticksPerQuarter}});
				break;
			case 13:
				call("midiPart.remove", {{"part", pick(project, "midiParts")}});
				break;
			case 14:
				call("note.add", {{"part", pick(project, "midiParts")}, {"notes", notes()}});
				break;
			case 15:
				removeFromPart(project, "note.remove", "notes");
				break;
			case 16:
				call("controller.add",
					{{"part", pick(project, "midiParts")},
						{"controllers",
							{{{"position", number(8) * g_ticksPerQuarter}, {"number", number(127)},
								{"value", static_cast<double>(number(100)) / 100.0}}}}});
				break;
			case 17:
				removeFromPart(project, "controller.remove", "controllers");
				break;
			case 18:
				call("project.set",
					{{"sampleRate", std::vector<double>{44100.0, 48000.0, 96000.0}[number(2)]},
						{"name", "Song " + std::to_string(number(9))}});
				break;
			case 19:
				addMarker(project);
				break;
			case 20:
				call("marker.set",
					{{"marker", pick(project, "markers")}, {"position", number(8) * g_ticksPerQuarter},
						{"name", "M" + std::to_string(number(9))}, {"track", pick(project, "tracks", "marker")}});
				break;
			case 21:
				call("marker.remove", {{"markers", {pick(project, "markers")}}});
				break;
			case 22:
				call("marker.activateTrack", {{"track", pick(project, "tracks", "marker")}});
				break;
			default:
				call("history.transaction",
					{{"description", "Write"},
						{"commands",
							{{{"command", "note.add"},
								 {"parameters", {{"part", pick(project, "midiParts")}, {"notes", notes()}}}},
								{{"command", "track.set"},
									{"parameters", {{"track", pick(project, "tracks")}, {"name", "Written"}}}}}}});
				break;
			}
		}

	private:
		// A random number from 0 to max.
		int64_t number(int64_t max) { return std::uniform_int_distribution<int64_t>(0, max)(m_random); }

		bool chance() { return number(1) == 1; }

		void call(const std::string& command, Json parameters) { m_client.call(command, std::move(parameters)); }

		// A random id from a list in project.get, optionally of one track type; sometimes one that doesn't exist.
		ObjectId pick(const Json& project, const char* list, const char* type = nullptr)
		{
			std::vector<ObjectId> ids;
			for (const Json& object : project.at(list))
				if (type == nullptr || object.at("type") == type)
					ids.push_back(object.at("id").get<ObjectId>());
			if (ids.empty() || number(19) == 0)
				return 999999;
			return ids[static_cast<size_t>(number(static_cast<int64_t>(ids.size()) - 1))];
		}

		ObjectId folderOrTop(const Json& project) { return chance() ? 0 : pick(project, "tracks", "folder"); }

		ObjectId output(const Json& project)
		{
			const int64_t kind = number(2);
			return kind == 0 ? 0 : pick(project, kind == 1 ? "busses" : "channels");
		}

		const char* timeBase() { return chance() ? "musical" : "linear"; }

		void addTrack(const Json& project)
		{
			const char* const types[] = {"audio", "instrument", "midi", "folder", "group", "effect", "marker"};
			Json parameters = {{"type", types[number(6)]}, {"timeBase", timeBase()}};
			if (chance())
				parameters["parent"] = pick(project, "tracks", "folder");
			if (parameters["type"] == "folder")
				parameters["groupChannel"] = chance();
			if (number(3) == 0)
				parameters["channels"] = 1 + number(1);
			call("track.add", parameters);
		}

		// A position or cycle marker, on the active marker track or a picked one.
		void addMarker(const Json& project)
		{
			Json parameters = {{"position", number(8) * g_ticksPerQuarter}};
			if (chance())
				parameters["track"] = pick(project, "tracks", "marker");
			if (chance())
			{
				parameters["type"] = "cycle";
				parameters["end"] = parameters["position"].get<int64_t>() + (1 + number(4)) * g_ticksPerQuarter;
			}
			call("marker.add", parameters);
		}

		void addClip()
		{
			const Result media = m_client.call("media.add",
				{{"path", "/audio/take" + std::to_string(number(99)) + ".wav"}, {"sampleRate", 48000.0},
					{"channels", 2}, {"length", 48000 + number(48000)}});
			if (media.ok())
				call("clip.add", {{"media", media.value.at("media").at("id")}});
		}

		Json notes()
		{
			Json notes = Json::array();
			for (int64_t i = number(2); i >= 0; --i)
				notes.push_back({{"position", number(16) * g_ticksPerQuarter / 4}, {"length", g_ticksPerQuarter / 4},
					{"pitch", 36 + number(48)}, {"velocity", static_cast<double>(number(100)) / 100.0}});
			return notes;
		}

		void removeFromPart(const Json& project, const std::string& command, const char* list)
		{
			const Json& parts = project.at("midiParts");
			if (parts.empty())
				return;
			const Json& part = parts[static_cast<size_t>(number(static_cast<int64_t>(parts.size()) - 1))];
			if (part.at(list).empty())
				return;
			const Json& item =
				part.at(list)[static_cast<size_t>(number(static_cast<int64_t>(part.at(list).size()) - 1))];
			call(command, {{"part", part.at("id")}, {list, {item.at("id")}}});
		}

		Client& m_client;
		std::mt19937 m_random;
	};
}

TEST_CASE("random edits followed by a full undo restore the project exactly", "[unit]")
{
	for (const uint32_t seed : {1U, 2U, 3U})
	{
		INFO("seed " << seed);
		Engine engine;
		Client client(engine);
		const Json empty = run(client, "project.get");
		RandomEditor editor(client, seed);
		for (int i = 0; i < 40; ++i)
			editor.edit();
		const Json initial = run(client, "project.get");
		const int64_t start = position(client);

		for (int i = 0; i < 150; ++i)
			editor.edit();
		const Json final = run(client, "project.get");
		const int64_t end = position(client);
		REQUIRE(end - start > 50); // enough edits succeeded to mean something

		run(client, "history.undo", {{"steps", end - start}});
		CHECK(run(client, "project.get") == initial);
		run(client, "history.redo", {{"steps", end - start}});
		CHECK(run(client, "project.get") == final);
		run(client, "history.undo", {{"steps", end}});
		CHECK(run(client, "project.get") == empty);
		CHECK(engine.renderSnapshots().acquire().channels.empty());
	}
}

TEST_CASE("undo and redo restore a change, announce it and publish a snapshot", "[unit]")
{
	Engine engine;
	Client client(engine);
	Client listener(engine);
	const Json added = run(client, "track.add", {{"type", "audio"}});
	eventNames(listener);

	run(client, "history.undo");
	CHECK(run(client, "project.get").at("tracks").empty());
	CHECK(engine.renderSnapshots().acquire().channels.empty());
	std::vector<std::string> names = eventNames(listener);
	CHECK(contains(names, "track.removed"));
	CHECK(contains(names, "channel.removed"));
	CHECK(contains(names, "history.changed"));

	run(client, "history.redo");
	const Json tracks = run(client, "project.get").at("tracks");
	REQUIRE(tracks.size() == 1);
	CHECK(tracks[0] == added.at("track")); // the same track with the same id
	CHECK(engine.renderSnapshots().acquire().channels.size() == 1);
	names = eventNames(listener);
	CHECK(contains(names, "track.added"));
	CHECK(contains(names, "channel.added"));
}

TEST_CASE("the history lists every step with its action, time and details", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId track = addTrack(client);
	run(client, "track.set", {{"track", track}, {"name", "Bass"}});

	Json history = run(client, "history.get");
	CHECK(history.at("position") == 2);
	CHECK(history.at("maxSteps") == 0); // no limit
	REQUIRE(history.at("steps").size() == 2);
	CHECK(history.at("steps")[0].at("action") == "Add track");
	CHECK(history.at("steps")[1].at("action") == "Change track");
	const std::string time = history.at("steps")[0].at("time").get<std::string>();
	CHECK(time.size() == 20); // e.g. 2026-10-27T21:05:00Z
	CHECK(time.back() == 'Z');

	run(client, "history.describe", {{"step", 0}, {"details", "first try"}});
	run(client, "history.undo");
	history = run(client, "history.get");
	CHECK(history.at("position") == 1);
	CHECK(history.at("steps")[0].at("details") == "first try");
	CHECK(history.at("steps").size() == 2); // the undone step can still be redone
	CHECK_THAT(reject(client, "history.describe", {{"step", 2}, {"details", "x"}}), ContainsSubstring("step"));
}

TEST_CASE("a new change discards the steps that could be redone", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId first = addTrack(client);
	addTrack(client);
	run(client, "history.undo");
	const ObjectId third = addTrack(client, "midi");
	CHECK(third > first + 2); // the undone track's ids are not given out again
	CHECK(run(client, "history.get").at("steps").size() == 2);
	CHECK_THAT(reject(client, "history.redo"), ContainsSubstring("nothing to redo"));
}

TEST_CASE("undo and redo move several steps at once, like the history separator", "[unit]")
{
	Engine engine;
	Client client(engine);
	for (int i = 0; i < 5; ++i)
		addTrack(client);
	run(client, "history.undo", {{"steps", 3}});
	CHECK(run(client, "project.get").at("tracks").size() == 2);
	run(client, "history.redo", {{"steps", 2}});
	CHECK(run(client, "project.get").at("tracks").size() == 4);
	CHECK_THAT(reject(client, "history.undo", {{"steps", 5}}), ContainsSubstring("4"));
	CHECK_THAT(reject(client, "history.undo", {{"steps", 0}}), ContainsSubstring("steps"));
	run(client, "history.undo", {{"steps", 4}});
	CHECK_THAT(reject(client, "history.undo"), ContainsSubstring("nothing to undo"));
}

TEST_CASE("the number of undo steps can be limited", "[unit]")
{
	Engine engine;
	Client client(engine);
	for (int i = 0; i < 5; ++i)
		addTrack(client);
	run(client, "history.set", {{"maxSteps", 3}});
	Json history = run(client, "history.get");
	CHECK(history.at("maxSteps") == 3);
	CHECK(history.at("steps").size() == 3); // the oldest steps were dropped
	CHECK(history.at("position") == 3);

	addTrack(client);
	CHECK(run(client, "history.get").at("steps").size() == 3);
	run(client, "history.undo", {{"steps", 3}});
	CHECK(run(client, "project.get").at("tracks").size() == 3);
	CHECK_THAT(reject(client, "history.set", {{"maxSteps", -1}}), ContainsSubstring("maxSteps"));
}

TEST_CASE("a transaction is one undo step and fails as a whole", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId track = addTrack(client, "instrument");
	const ObjectId part =
		run(client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", 4 * g_ticksPerQuarter}})
			.at("part")
			.at("id")
			.get<ObjectId>();
	const Json before = run(client, "project.get");
	const Json note = {{"position", 0}, {"length", g_ticksPerQuarter}, {"pitch", 60}, {"velocity", 0.8}};

	const Json result = run(client, "history.transaction",
		{{"description", "Write melody"},
			{"commands",
				{{{"command", "note.add"}, {"parameters", {{"part", part}, {"notes", {note}}}}},
					{{"command", "midiPart.set"}, {"parameters", {{"part", part}, {"name", "Melody"}}}},
					{{"command", "note.add"}, {"parameters", {{"part", part}, {"notes", {note, note}}}}}}}});
	CHECK(result.at("results").size() == 3);
	CHECK(result.at("results")[2].at("notes").size() == 2);
	Json history = run(client, "history.get");
	CHECK(history.at("steps").back().at("action") == "Write melody");
	run(client, "history.undo");
	CHECK(run(client, "project.get") == before);

	const int64_t steps = position(client);
	CHECK_THAT(reject(client, "history.transaction",
				   {{"description", "Broken"},
					   {"commands",
						   {{{"command", "note.add"}, {"parameters", {{"part", part}, {"notes", {note}}}}},
							   {{"command", "track.set"}, {"parameters", {{"track", 999}, {"name", "x"}}}}}}}),
		ContainsSubstring("commands[1] (track.set): no track 999"));
	CHECK(run(client, "project.get") == before);
	CHECK(position(client) == steps);
	CHECK_THAT(
		reject(client, "history.transaction",
			{{"description", "Nested"}, {"commands", {{{"command", "history.undo"}, {"parameters", Json::object()}}}}}),
		ContainsSubstring("can't be part of a transaction"));
	CHECK_THAT(reject(client, "history.transaction",
				   {{"description", "Bad"},
					   {"commands", {{{"command", "note.add"}, {"parameters", {{"part", part}, {"notes", 5}}}}}}}),
		ContainsSubstring("commands[0].parameters.notes must be array"));
}

TEST_CASE("queries and failed commands leave no undo step", "[unit]")
{
	Engine engine;
	Client client(engine);
	run(client, "project.get");
	reject(client, "track.add", {{"type", "audio"}, {"channels", 0}});
	reject(client, "track.set", {{"track", 999}});
	CHECK(position(client) == 0);
	CHECK_THAT(reject(client, "history.undo"), ContainsSubstring("nothing to undo"));
}

TEST_CASE("a new project starts with an empty history", "[unit]")
{
	Engine engine;
	Client client(engine);
	addTrack(client);
	run(client, "history.set", {{"maxSteps", 10}});
	run(client, "project.new");
	const Json history = run(client, "history.get");
	CHECK(history.at("steps").empty());
	CHECK(history.at("maxSteps") == 10); // a setting, not part of the project
	CHECK_THAT(reject(client, "history.undo"), ContainsSubstring("nothing to undo"));
}
