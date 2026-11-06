// SPDX-License-Identifier: GPL-3.0-or-later
// The tempo track, its tempo events, the project tempo modes and tap tempo (#155).
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;
using gnubase::time::g_ticksPerQuarter;

namespace
{
	// At the default 120 BPM and 48 kHz, a quarter note lasts 24000 samples.
	constexpr int64_t g_quarterSamples = 24000;

	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	std::string reject(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " succeeded: " << result.value.dump());
		REQUIRE(!result.ok());
		return result.error;
	}

	ObjectId addTrack(Client& client, Json parameters)
	{
		return run(client, "track.add", std::move(parameters)).at("track").at("id").get<ObjectId>();
	}

	Json point(int64_t quarters, double bpm, bool ramp = false)
	{
		return {{"position", quarters * g_ticksPerQuarter}, {"bpm", bpm}, {"ramp", ramp}};
	}

	std::vector<std::string> eventNames(Client& client)
	{
		std::vector<std::string> names;
		while (const auto message = client.poll())
			if (const auto* event = std::get_if<Event>(&*message))
				names.push_back(event->name);
		return names;
	}

	const RenderChannel& renderChannel(const RenderSnapshot& snapshot, ObjectId track)
	{
		const auto found = std::find_if(snapshot.channels.begin(), snapshot.channels.end(),
			[track](const RenderChannel& channel) { return channel.track == track; });
		REQUIRE(found != snapshot.channels.end());
		return *found;
	}

	// A MIDI part at position, four quarters long in the track's unit at 120 BPM, with a note from its first to its
	// second quarter and a controller at its second.
	ObjectId addPartWithNote(Client& client, ObjectId track, int64_t position)
	{
		const ObjectId part =
			run(client, "midiPart.add", {{"track", track}, {"position", position}, {"length", 4 * g_quarterSamples}})
				.at("part")
				.at("id")
				.get<ObjectId>();
		run(client, "note.add",
			{{"part", part},
				{"notes",
					{{{"position", g_ticksPerQuarter}, {"length", g_ticksPerQuarter}, {"pitch", 60},
						{"velocity", 1.0}}}}});
		run(client, "controller.add",
			{{"part", part}, {"controllers", {{{"position", 2 * g_ticksPerQuarter}, {"number", 7}, {"value", 1.0}}}}});
		return part;
	}

	Json midiPartJson(Client& client, ObjectId id)
	{
		const Json parts = run(client, "project.get").at("midiParts");
		const auto found =
			std::find_if(parts.begin(), parts.end(), [id](const Json& candidate) { return candidate.at("id") == id; });
		REQUIRE(found != parts.end());
		return *found;
	}

	// The parts of the midiPart.changed events since the last call, in order. Each event's part must be the part as
	// project.get has it, so that a client that follows the events holds no stale part.
	std::vector<ObjectId> changedParts(Client& client)
	{
		std::vector<Json> reported;
		while (const auto message = client.poll())
			if (const auto* event = std::get_if<Event>(&*message);
				event != nullptr && event->name == "midiPart.changed")
				reported.push_back(event->data.at("part"));
		std::vector<ObjectId> parts;
		for (const Json& part : reported)
		{
			parts.push_back(part.at("id").get<ObjectId>());
			Json current = midiPartJson(client, parts.back());
			current.erase("rootKeyTranspose"); // derived by project.get, not in the events
			CHECK(part == current);
		}
		return parts;
	}
}

TEST_CASE("a project has one tempo track at most, without a channel, at the top level", "[unit]")
{
	Engine engine;
	Client client(engine);
	const Json added = run(client, "track.add", {{"type", "tempo"}});
	CHECK(added.at("track").at("name") == "Tempo");
	CHECK(added.at("channel").is_null());
	const ObjectId tempo = added.at("track").at("id").get<ObjectId>();
	CHECK(engine.renderSnapshots().acquire().channels.empty());
	CHECK_THAT(reject(client, "track.add", {{"type", "tempo"}}), ContainsSubstring("one tempo track"));
	CHECK_THAT(reject(client, "track.duplicate", {{"track", tempo}}), ContainsSubstring("one tempo track"));
	const ObjectId folder = addTrack(client, {{"type", "folder"}});
	CHECK_THAT(reject(client, "track.move", {{"track", tempo}, {"parent", folder}, {"index", 0}}),
		ContainsSubstring("folder"));

	// Removing the tempo track keeps the tempo: the track only shows it.
	run(client, "tempo.add", point(4, 90.0));
	run(client, "track.remove", {{"track", tempo}});
	CHECK(run(client, "project.get").at("tempoTrack").size() == 2);
	CHECK_THAT(reject(client, "track.add", {{"type", "tempo"}, {"parent", folder}}), ContainsSubstring("folder"));
}

TEST_CASE("fixed tempo and tempo track mode each keep their tempo", "[unit]")
{
	Engine engine;
	Client client(engine);
	Client listener(engine);
	Json project = run(client, "project.get");
	CHECK(project.at("tempoTrackActive") == false); // a new project has a fixed tempo
	CHECK(project.at("fixedTempo") == 120.0);

	// In fixed tempo mode, the tempo track's events wait.
	run(client, "tempo.add", point(4, 60.0));
	project = run(client, "project.get");
	CHECK(project.at("tempo") == Json::array({point(0, 120.0)}));
	CHECK(project.at("tempoTrack") == Json::array({point(0, 120.0), point(4, 60.0)}));

	eventNames(listener);
	run(client, "project.set", {{"tempoTrackActive", true}});
	const std::vector<std::string> events = eventNames(listener);
	CHECK(std::find(events.begin(), events.end(), "tempo.changed") != events.end());
	project = run(client, "project.get");
	CHECK(project.at("tempo") == Json::array({point(0, 120.0), point(4, 60.0)}));
	CHECK(engine.renderSnapshots().acquire().tempo.points().size() == 2);

	// The transport's tempo is the tempo track's first event in tempo track mode, the fixed tempo otherwise.
	run(client, "project.set", {{"tempo", 100.0}});
	CHECK(run(client, "project.get").at("tempo") == Json::array({point(0, 100.0), point(4, 60.0)}));
	run(client, "project.set", {{"tempoTrackActive", false}, {"tempo", 140.0}});
	project = run(client, "project.get");
	CHECK(project.at("tempo") == Json::array({point(0, 140.0)}));
	CHECK(project.at("fixedTempo") == 140.0);
	CHECK(project.at("tempoTrack") == Json::array({point(0, 100.0), point(4, 60.0)}));

	run(client, "history.undo");
	project = run(client, "project.get");
	CHECK(project.at("tempoTrackActive") == true);
	CHECK(project.at("fixedTempo") == 120.0);
	CHECK(project.at("tempo") == Json::array({point(0, 100.0), point(4, 60.0)}));
}

TEST_CASE("editing the tempo moves musical-time events and keeps linear-time events in place", "[unit]")
{
	Engine engine;
	Client client(engine);
	run(client, "project.set", {{"tempoTrackActive", true}});
	const ObjectId media = run(client, "media.add",
		{{"path", "/audio/loop.wav"}, {"sampleRate", 48000.0}, {"channels", 2}, {"length", 192000}})
							   .at("media")
							   .at("id")
							   .get<ObjectId>();
	const ObjectId clip = run(client, "clip.add", {{"media", media}}).at("clip").at("id").get<ObjectId>();
	const ObjectId musical = addTrack(client, {{"type", "audio"}});
	const ObjectId linear = addTrack(client, {{"type", "audio"}, {"timeBase", "linear"}});
	const ObjectId midi = addTrack(client, {{"type", "midi"}});
	run(client, "audioEvent.add", {{"track", musical}, {"clip", clip}, {"position", 8 * g_ticksPerQuarter}});
	run(client, "audioEvent.add", {{"track", linear}, {"clip", clip}, {"position", 8 * g_quarterSamples}});
	const ObjectId part = run(client, "midiPart.add",
		{{"track", midi}, {"position", 8 * g_ticksPerQuarter}, {"length", 4 * g_ticksPerQuarter}})
							  .at("part")
							  .at("id")
							  .get<ObjectId>();
	run(client, "note.add",
		{{"part", part},
			{"notes", {{{"position", 0}, {"length", g_ticksPerQuarter}, {"pitch", 60}, {"velocity", 1.0}}}}});

	const auto starts = [&engine, musical, linear, midi]
	{
		const RenderSnapshot& snapshot = engine.renderSnapshots().acquire();
		return std::vector<int64_t>{renderChannel(snapshot, musical).audioEvents.at(0).start,
			renderChannel(snapshot, linear).audioEvents.at(0).start, renderChannel(snapshot, midi).notes.at(0).start};
	};
	CHECK(starts() == std::vector<int64_t>{8 * g_quarterSamples, 8 * g_quarterSamples, 8 * g_quarterSamples});

	// Half the tempo from the fourth quarter: what is musical starts four quarters of 24000 samples later.
	run(client, "tempo.add", point(4, 60.0, false));
	CHECK(starts() == std::vector<int64_t>{12 * g_quarterSamples, 8 * g_quarterSamples, 12 * g_quarterSamples});
	// Moving the tempo event moves them again, the linear event stays.
	run(client, "tempo.set", {{"events", {{{"position", 4 * g_ticksPerQuarter}, {"to", 6 * g_ticksPerQuarter}}}}});
	CHECK(starts() == std::vector<int64_t>{10 * g_quarterSamples, 8 * g_quarterSamples, 10 * g_quarterSamples});
	run(client, "tempo.remove", {{"positions", {6 * g_ticksPerQuarter}}});
	CHECK(starts() == std::vector<int64_t>{8 * g_quarterSamples, 8 * g_quarterSamples, 8 * g_quarterSamples});
}

TEST_CASE("notes and controllers in parts on linear tracks keep their time when the tempo changes", "[unit]")
{
	Engine engine;
	Client client(engine);
	run(client, "project.set", {{"tempoTrackActive", true}});
	const ObjectId linear = addTrack(client, {{"type", "midi"}, {"timeBase", "linear"}});
	const ObjectId musical = addTrack(client, {{"type", "midi"}});
	const auto addPart = [&client](ObjectId track, int64_t position, int64_t length)
	{
		const ObjectId part =
			run(client, "midiPart.add", {{"track", track}, {"position", position}, {"length", length}})
				.at("part")
				.at("id")
				.get<ObjectId>();
		run(client, "note.add",
			{{"part", part},
				{"notes",
					{{{"position", g_ticksPerQuarter}, {"length", g_ticksPerQuarter}, {"pitch", 60},
						{"velocity", 1.0}}}}});
		run(client, "controller.add",
			{{"part", part}, {"controllers", {{{"position", 2 * g_ticksPerQuarter}, {"number", 7}, {"value", 1.0}}}}});
		return part;
	};
	// A part from the eighth quarter, four quarters long: in samples on the linear track, in ticks on the other.
	addPart(linear, 8 * g_quarterSamples, 4 * g_quarterSamples);
	addPart(musical, 8 * g_ticksPerQuarter, 4 * g_ticksPerQuarter);

	// Where the note starts and ends and the controller is, in samples.
	const auto times = [&engine](ObjectId track)
	{
		const RenderChannel& channel = renderChannel(engine.renderSnapshots().acquire(), track);
		REQUIRE(channel.notes.size() == 1);
		REQUIRE(channel.controllers.size() == 1);
		return std::vector<int64_t>{
			channel.notes.at(0).start, channel.notes.at(0).end, channel.controllers.at(0).position};
	};
	const std::vector<int64_t> before{9 * g_quarterSamples, 10 * g_quarterSamples, 10 * g_quarterSamples};
	CHECK(times(linear) == before);
	CHECK(times(musical) == before);

	// Half the tempo from the fourth quarter, and a ramp inside the parts: the linear part's notes and controllers stay
	// where they were, the musical one's follow the tempo.
	run(client, "tempo.add", point(4, 60.0));
	run(client, "tempo.add", point(7, 90.0, true));
	const auto near = [](const std::vector<int64_t>& a, const std::vector<int64_t>& b)
	{
		return std::equal(
			a.begin(), a.end(), b.begin(), b.end(), [](int64_t x, int64_t y) { return std::abs(x - y) <= 1; });
	};
	CHECK(near(times(linear), before));
	CHECK_FALSE(near(times(musical), before));
	run(client, "project.set", {{"tempo", 200.0}});
	run(client, "tempo.set", {{"events", {{{"position", 7 * g_ticksPerQuarter}, {"bpm", 45.0}}}}});
	CHECK(near(times(linear), before));
	run(client, "tempo.remove", {{"positions", {4 * g_ticksPerQuarter}}});
	run(client, "project.set", {{"tempoTrackActive", false}});
	CHECK(near(times(linear), before));
	CHECK(times(musical) == before); // the fixed 120 BPM again

	// Undo puts the notes back as they were with each tempo.
	run(client, "history.undo");
	run(client, "history.undo");
	CHECK(near(times(linear), before));
}

TEST_CASE(
	"shared copies of a linear part become real copies when a tempo change retimes their notes differently", "[unit]")
{
	Engine engine;
	Client client(engine);
	run(client, "project.set", {{"tempoTrackActive", true}});
	const ObjectId linear = addTrack(client, {{"type", "midi"}, {"timeBase", "linear"}});
	const ObjectId musical = addTrack(client, {{"type", "midi"}});
	// A part at the project start with a note from its first to its second quarter.
	const ObjectId part =
		run(client, "midiPart.add", {{"track", linear}, {"position", 0}, {"length", 4 * g_quarterSamples}})
			.at("part")
			.at("id")
			.get<ObjectId>();
	run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", g_ticksPerQuarter}, {"length", g_ticksPerQuarter}, {"pitch", 60}, {"velocity", 1.0}}}}});
	const auto copy = [&client, part](Json parameters)
	{
		parameters["events"] = {part};
		parameters["shared"] = true;
		return run(client, "event.copy", parameters).at("events").at(0).get<ObjectId>();
	};
	const ObjectId later = copy({{"delta", 8 * g_quarterSamples}});  // after the tempo event below
	const ObjectId other = copy({{"ticks", 0}, {"track", musical}}); // a musical copy
	const auto midiPart = [&client](ObjectId id)
	{
		const Json parts = run(client, "project.get").at("midiParts");
		const auto found =
			std::find_if(parts.begin(), parts.end(), [id](const Json& candidate) { return candidate.at("id") == id; });
		REQUIRE(found != parts.end());
		return *found;
	};
	REQUIRE(midiPart(later).at("shared") != 0);
	REQUIRE(midiPart(later).at("shared") == midiPart(other).at("shared"));

	// Where the notes start and end on a track, in samples.
	const auto notes = [&engine](ObjectId track)
	{
		std::vector<int64_t> times;
		for (const auto& note : renderChannel(engine.renderSnapshots().acquire(), track).notes)
		{
			times.push_back(note.start);
			times.push_back(note.end);
		}
		std::sort(times.begin(), times.end());
		return times;
	};
	const auto near = [](const std::vector<int64_t>& a, const std::vector<int64_t>& b)
	{
		return std::equal(
			a.begin(), a.end(), b.begin(), b.end(), [](int64_t x, int64_t y) { return std::abs(x - y) <= 1; });
	};
	const std::vector<int64_t> linearBefore{
		g_quarterSamples, 2 * g_quarterSamples, 9 * g_quarterSamples, 10 * g_quarterSamples};
	REQUIRE(near(notes(linear), linearBefore));
	changedParts(client);

	// Half the tempo from the sixth quarter: the copy after it retimes its notes, the first part and the musical copy
	// don't, so the group can't share notes any more. Every copy keeps its notes where they were. Only the copy that
	// leaves the group changes.
	run(client, "tempo.add", point(6, 60.0));
	CHECK(changedParts(client) == std::vector<ObjectId>{later});
	CHECK(near(notes(linear), linearBefore));
	CHECK(notes(musical) == std::vector<int64_t>{g_quarterSamples, 2 * g_quarterSamples});
	CHECK(midiPart(later).at("shared") == 0);
	const Json first = midiPart(part);
	CHECK(first.at("shared") != 0);
	CHECK(first.at("shared") == midiPart(other).at("shared")); // these two still hold the same notes
	CHECK(first.at("notes") == midiPart(other).at("notes"));
	CHECK(midiPart(later).at("notes").at(0).at("id") != first.at("notes").at(0).at("id"));

	// A tempo change before them all retimes the linear part but not its musical copy: no copy is left shared. Both
	// change: the musical copy's notes stay where they are, but get new ids outside the group.
	run(client, "project.set", {{"tempo", 90.0}});
	CHECK(changedParts(client) == std::vector<ObjectId>{part, other});
	CHECK(near(notes(linear), linearBefore));
	CHECK(midiPart(part).at("shared") == 0);
	CHECK(midiPart(other).at("shared") == 0);

	// Undo shares them again.
	run(client, "history.undo");
	run(client, "history.undo");
	CHECK(midiPart(later).at("shared") == midiPart(part).at("shared"));
	CHECK(midiPart(later).at("notes") == midiPart(part).at("notes"));
}

TEST_CASE("shared copies that still hold the same notes after a tempo change stay shared with each other", "[unit]")
{
	Engine engine;
	Client client(engine);
	run(client, "project.set", {{"tempoTrackActive", true}});
	const ObjectId linear = addTrack(client, {{"type", "midi"}, {"timeBase", "linear"}});
	const ObjectId musical = addTrack(client, {{"type", "midi"}});
	const ObjectId part = addPartWithNote(client, linear, 0);
	const auto copy = [&client, part](Json parameters)
	{
		parameters["events"] = {part};
		parameters["shared"] = true;
		return run(client, "event.copy", parameters).at("events").at(0).get<ObjectId>();
	};
	// Two linear copies after the tempo event below, and a musical copy.
	const ObjectId later = copy({{"delta", 8 * g_quarterSamples}});
	const ObjectId latest = copy({{"delta", 12 * g_quarterSamples}});
	const ObjectId other = copy({{"ticks", 0}, {"track", musical}});
	const Json group = midiPartJson(client, part).at("shared");
	REQUIRE(group != 0);
	for (const ObjectId id : {later, latest, other})
		REQUIRE(midiPartJson(client, id).at("shared") == group);

	// Half the tempo from the sixth quarter retimes both later copies the same way: they share their new notes in a
	// group of their own, and the first part keeps its group with the musical copy.
	run(client, "tempo.add", point(6, 60.0));
	const Json first = midiPartJson(client, part);
	const Json second = midiPartJson(client, later);
	const Json third = midiPartJson(client, latest);
	CHECK(first.at("shared") == group);
	CHECK(midiPartJson(client, other).at("shared") == group);
	CHECK(second.at("shared") != 0);
	CHECK(second.at("shared") != group);
	CHECK(third.at("shared") == second.at("shared"));
	CHECK(third.at("notes") == second.at("notes"));
	CHECK(second.at("notes").at(0).at("id") != first.at("notes").at(0).at("id"));
	// An edit of one of them reaches the other, not the first group.
	const auto noteId = second.at("notes").at(0).at("id").get<ObjectId>();
	run(client, "note.set", {{"part", later}, {"notes", {{{"id", noteId}, {"pitch", 64}}}}});
	CHECK(midiPartJson(client, latest).at("notes").at(0).at("pitch") == 64);
	CHECK(midiPartJson(client, part).at("notes").at(0).at("pitch") == 60);

	// Undo shares all four again, and redo splits them again.
	run(client, "history.undo");
	run(client, "history.undo");
	for (const ObjectId id : {later, latest, other})
		CHECK(midiPartJson(client, id).at("shared") == group);
	run(client, "history.redo");
	CHECK(midiPartJson(client, later).at("shared") == second.at("shared"));
	CHECK(midiPartJson(client, latest).at("shared") == second.at("shared"));
	CHECK(midiPartJson(client, latest).at("notes") == second.at("notes"));
	CHECK(midiPartJson(client, other).at("shared") == group);
}

TEST_CASE("the largest set of shared copies that still match keeps their group when a tempo change splits it", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId linear = addTrack(client, {{"type", "midi"}, {"timeBase", "linear"}});
	const ObjectId musical = addTrack(client, {{"type", "midi"}});
	const ObjectId part = addPartWithNote(client, linear, 0);
	const auto copy = [&client, part, musical](int64_t ticks)
	{
		return run(client, "event.copy", {{"events", {part}}, {"shared", true}, {"ticks", ticks}, {"track", musical}})
			.at("events")
			.at(0)
			.get<ObjectId>();
	};
	const ObjectId one = copy(0);
	const ObjectId two = copy(8 * g_ticksPerQuarter);
	const Json group = midiPartJson(client, part).at("shared");
	const Json notes = midiPartJson(client, one).at("notes");
	REQUIRE(group != 0);
	REQUIRE(midiPartJson(client, two).at("shared") == group);
	changedParts(client);

	// A new project tempo retimes the linear part, not the two musical copies: they keep the group and their notes'
	// ids, and the linear part, the group's first, becomes a real copy. Only it changes.
	run(client, "project.set", {{"tempo", 90.0}});
	CHECK(changedParts(client) == std::vector<ObjectId>{part});
	CHECK(midiPartJson(client, one).at("shared") == group);
	CHECK(midiPartJson(client, two).at("shared") == group);
	CHECK(midiPartJson(client, one).at("notes") == notes);
	const Json first = midiPartJson(client, part);
	CHECK(first.at("shared") == 0);
	CHECK(first.at("notes").at(0).at("id") != notes.at(0).at("id"));
	// An edit of one musical copy reaches the other, not the linear part.
	const auto noteId = notes.at(0).at("id").get<ObjectId>();
	run(client, "note.set", {{"part", one}, {"notes", {{{"id", noteId}, {"pitch", 64}}}}});
	CHECK(midiPartJson(client, two).at("notes").at(0).at("pitch") == 64);
	CHECK(midiPartJson(client, part).at("notes").at(0).at("pitch") == 60);

	// Undo shares all three again, redo splits them again.
	run(client, "history.undo");
	run(client, "history.undo");
	CHECK(midiPartJson(client, part).at("shared") == group);
	CHECK(midiPartJson(client, part).at("notes") == notes);
	run(client, "history.redo");
	CHECK(midiPartJson(client, part).at("shared") == 0);
	CHECK(midiPartJson(client, two).at("shared") == group);
}

TEST_CASE("a tempo change reports only the linear parts whose notes or controllers it retimes", "[unit]")
{
	Engine engine;
	Client client(engine);
	run(client, "project.set", {{"tempoTrackActive", true}});
	const ObjectId linear = addTrack(client, {{"type", "midi"}, {"timeBase", "linear"}});
	const ObjectId early = addPartWithNote(client, linear, 0);
	const ObjectId late = addPartWithNote(client, linear, 8 * g_quarterSamples);
	changedParts(client);

	// A tempo event between the parts retimes the later one's notes only.
	run(client, "tempo.add", point(6, 60.0));
	CHECK(changedParts(client) == std::vector<ObjectId>{late});
	// One after both changes neither.
	run(client, "tempo.add", point(40, 90.0));
	CHECK(changedParts(client).empty());
	// The project tempo, the first tempo event's, retimes the earlier part; the later one lies after the 60 BPM event,
	// where the tempo stays, so its notes keep their ticks from its start.
	run(client, "project.set", {{"tempo", 100.0}});
	CHECK(changedParts(client) == std::vector<ObjectId>{early});
	// Removing the 60 BPM event retimes the later part only.
	run(client, "tempo.remove", {{"positions", {6 * g_ticksPerQuarter}}});
	CHECK(changedParts(client) == std::vector<ObjectId>{late});

	// A part whose note ends before a tempo event and whose controller comes after it: only the controller moves, and
	// the part is reported.
	const ObjectId controllers = run(client, "midiPart.add",
		{{"track", linear}, {"position", 20 * g_quarterSamples}, {"length", 4 * g_quarterSamples}})
									 .at("part")
									 .at("id")
									 .get<ObjectId>();
	run(client, "note.add",
		{{"part", controllers},
			{"notes", {{{"position", 0}, {"length", g_ticksPerQuarter}, {"pitch", 60}, {"velocity", 1.0}}}}});
	run(client, "controller.add",
		{{"part", controllers},
			{"controllers", {{{"position", 3 * g_ticksPerQuarter}, {"number", 7}, {"value", 1.0}}}}});
	const Json before = midiPartJson(client, controllers);
	changedParts(client);
	// The part starts at ten seconds, 50/3 quarters at 100 BPM: the tempo event comes two quarters into it.
	run(client, "tempo.add", {{"position", (50 * g_ticksPerQuarter / 3) + (2 * g_ticksPerQuarter)}, {"bpm", 50.0}});
	CHECK(changedParts(client) == std::vector<ObjectId>{controllers});
	const Json after = midiPartJson(client, controllers);
	CHECK(after.at("notes") == before.at("notes"));
	CHECK(after.at("controllers") != before.at("controllers"));
}

TEST_CASE("a tempo change that splits a group whose first part is musical reports both parts", "[unit]")
{
	Engine engine;
	Client client(engine);
	run(client, "project.set", {{"tempoTrackActive", true}});
	const ObjectId linear = addTrack(client, {{"type", "midi"}, {"timeBase", "linear"}});
	const ObjectId musical = addTrack(client, {{"type", "midi"}});
	const ObjectId first =
		run(client, "midiPart.add", {{"track", musical}, {"position", 0}, {"length", 4 * g_ticksPerQuarter}})
			.at("part")
			.at("id")
			.get<ObjectId>();
	run(client, "note.add",
		{{"part", first},
			{"notes",
				{{{"position", g_ticksPerQuarter}, {"length", g_ticksPerQuarter}, {"pitch", 60}, {"velocity", 1.0}}}}});
	const ObjectId copy = run(client, "event.copy",
		{{"events", {first}}, {"shared", true}, {"ticks", 8 * g_ticksPerQuarter}, {"track", linear}})
							  .at("events")
							  .at(0)
							  .get<ObjectId>();
	const Json notes = midiPartJson(client, first).at("notes");
	REQUIRE(midiPartJson(client, copy).at("shared") == midiPartJson(client, first).at("shared"));
	REQUIRE(midiPartJson(client, copy).at("shared") != 0);
	changedParts(client);

	// Half the tempo from the second quarter retimes the linear copy's notes, not the musical part's: the musical
	// part keeps its notes but is no longer shared, and the copy becomes a real copy. Both change.
	run(client, "tempo.add", point(2, 60.0));
	CHECK(changedParts(client) == std::vector<ObjectId>{first, copy});
	CHECK(midiPartJson(client, first).at("shared") == 0);
	CHECK(midiPartJson(client, first).at("notes") == notes);
	CHECK(midiPartJson(client, copy).at("shared") == 0);
}

TEST_CASE("tapping the project tempo and importing a MIDI file's tempo keep linear parts' notes at their time",
	"[integration]")
{
	Engine engine;
	Client client(engine);
	const ObjectId linear = addTrack(client, {{"type", "midi"}, {"timeBase", "linear"}});
	addPartWithNote(client, linear, 8 * g_quarterSamples);
	const auto times = [&engine, linear]
	{
		const RenderChannel& channel = renderChannel(engine.renderSnapshots().acquire(), linear);
		REQUIRE(channel.notes.size() == 1);
		REQUIRE(channel.controllers.size() == 1);
		return std::vector<int64_t>{
			channel.notes.at(0).start, channel.notes.at(0).end, channel.controllers.at(0).position};
	};
	const std::vector<int64_t> before{9 * g_quarterSamples, 10 * g_quarterSamples, 10 * g_quarterSamples};
	REQUIRE(times() == before);
	const auto near = [&times, &before]
	{
		const std::vector<int64_t> now = times();
		return std::equal(now.begin(), now.end(), before.begin(), before.end(),
			[](int64_t x, int64_t y) { return std::abs(x - y) <= 1; });
	};

	// The tapped tempo is the project tempo, in fixed tempo mode and then in tempo track mode, where it is the first
	// tempo event's, which the part is in.
	REQUIRE(run(client, "tempo.tap", {{"bpm", 93.0}, {"mode", "project"}}).at("applied") == "project");
	CHECK(near());
	run(client, "project.set", {{"tempoTrackActive", true}});
	run(client, "tempo.add", point(20, 140.0));
	REQUIRE(run(client, "tempo.tap", {{"bpm", 75.0}, {"mode", "project"}}).at("applied") == "project");
	CHECK(near());

	// A MIDI file from another project whose tempo changes before the part and inside its note: 50 BPM from 1.5 s,
	// 170 BPM from 4.8 s.
	const gnubase::test::TempDir dir;
	const std::string path = (dir.path() / "tempo.mid").string();
	{
		Engine source;
		Client sourceClient(source);
		run(sourceClient, "project.set", {{"tempoTrackActive", true}});
		addTrack(sourceClient, {{"type", "midi"}});
		run(sourceClient, "tempo.add", point(3, 50.0));
		run(sourceClient, "tempo.add", {{"position", 23 * g_ticksPerQuarter / 4}, {"bpm", 170.0}});
		run(sourceClient, "midiFile.export", {{"path", path}});
	}
	run(client, "midiFile.import", {{"path", path}, {"markers", false}});
	REQUIRE(run(client, "project.get").at("tempo").size() == 3);
	CHECK(near());
	run(client, "history.undo");
	CHECK(near());
}

TEST_CASE("a track's events, parts, markers and automation keep their time when its time base switches, and then "
		  "follow the tempo or not",
	"[unit]")
{
	Engine engine;
	Client client(engine);
	run(client, "project.set", {{"tempoTrackActive", true}});
	const ObjectId media = run(client, "media.add",
		{{"path", "/audio/loop.wav"}, {"sampleRate", 48000.0}, {"channels", 2}, {"length", 192000}})
							   .at("media")
							   .at("id")
							   .get<ObjectId>();
	const ObjectId clip = run(client, "clip.add", {{"media", media}}).at("clip").at("id").get<ObjectId>();
	const ObjectId audio = addTrack(client, {{"type", "audio"}});
	const ObjectId midi = addTrack(client, {{"type", "midi"}});
	const ObjectId markers = addTrack(client, {{"type", "marker"}});
	run(client, "audioEvent.add", {{"track", audio}, {"clip", clip}, {"position", 8 * g_ticksPerQuarter}});
	const ObjectId part = run(client, "midiPart.add",
		{{"track", midi}, {"position", 8 * g_ticksPerQuarter}, {"length", 4 * g_ticksPerQuarter}})
							  .at("part")
							  .at("id")
							  .get<ObjectId>();
	run(client, "note.add",
		{{"part", part},
			{"notes", {{{"position", 0}, {"length", g_ticksPerQuarter}, {"pitch", 60}, {"velocity", 1.0}}}}});
	run(client, "marker.add", {{"track", markers}, {"position", 8 * g_ticksPerQuarter}});
	const ObjectId lane = run(client, "automationLane.add", {{"track", audio}, {"target", {{"type", "volume"}}}})
							  .at("lane")
							  .at("id")
							  .get<ObjectId>();
	run(client, "automationPoint.add",
		{{"lane", lane}, {"points", {{{"position", 8 * g_ticksPerQuarter}, {"value", -6.0}}}}});

	// Where the audio event, the note, the marker and the automation point are, in samples.
	const auto times = [&]
	{
		const RenderSnapshot& snapshot = engine.renderSnapshots().acquire();
		const RenderChannel& channel = renderChannel(snapshot, audio);
		return std::vector<int64_t>{channel.audioEvents.at(0).start, renderChannel(snapshot, midi).notes.at(0).start,
			run(client, "marker.list", {{"track", markers}}).at("markers").at(0).at("positionSamples").get<int64_t>(),
			channel.automation.at(0).points.at(0).position};
	};
	const std::vector<int64_t> at8(4, 8 * g_quarterSamples);
	CHECK(times() == at8);

	// Linear: switching keeps them where they are, and a slower tempo before them leaves them there.
	for (const ObjectId track : {audio, midi, markers})
		run(client, "track.set", {{"track", track}, {"timeBase", "linear"}});
	CHECK(times() == at8);
	run(client, "tempo.add", point(4, 60.0));
	CHECK(times() == at8);

	// Musical again: they stay at their time, two quarters of 60 BPM after the fourth quarter, and follow the tempo.
	for (const ObjectId track : {audio, midi, markers})
		run(client, "track.set", {{"track", track}, {"timeBase", "musical"}});
	CHECK(times() == at8);
	run(client, "tempo.remove", {{"positions", {4 * g_ticksPerQuarter}}});
	CHECK(times() == std::vector<int64_t>(4, 6 * g_quarterSamples));
}

TEST_CASE("new tracks get the time base of the user setting editing.defaultTimeBase", "[unit]")
{
	Engine engine;
	Client client(engine);
	const auto timeBase = [&client](const std::string& type, Json parameters = Json::object())
	{
		parameters["type"] = type;
		return run(client, "track.add", parameters).at("track").at("timeBase").get<std::string>();
	};
	CHECK(run(client, "settings.get", {{"key", "editing.defaultTimeBase"}}).at("value") == "musical");
	CHECK(timeBase("audio") == "musical");

	run(client, "settings.set", {{"key", "editing.defaultTimeBase"}, {"value", "linear"}});
	// marker.add without a marker track adds one with the setting's time base: its position is in that track's unit,
	// which project.get reports for the track the marker names.
	const Json marker = run(client, "marker.add", {{"position", g_quarterSamples}}).at("marker");
	const Json tracks = run(client, "project.get").at("tracks");
	const auto markerTrack = std::find_if(
		tracks.begin(), tracks.end(), [&marker](const Json& track) { return track.at("id") == marker.at("track"); });
	REQUIRE(markerTrack != tracks.end());
	CHECK(markerTrack->at("timeBase") == "linear");
	CHECK(marker.at("position") == g_quarterSamples);
	for (const char* type : {"audio", "instrument", "midi", "group", "effect", "vca", "marker"})
	{
		INFO(type);
		CHECK(timeBase(type) == "linear");
	}
	// Tracks without events of their own have none.
	for (const char* type : {"folder", "ruler", "tempo", "signature"})
	{
		INFO(type);
		CHECK(timeBase(type) == "musical");
	}
	CHECK(timeBase("audio", {{"timeBase", "musical"}}) == "musical"); // a time base given wins

	// Following the primary time format: bars and beats make musical tracks, any other format linear ones.
	run(client, "settings.set", {{"key", "editing.defaultTimeBase"}, {"value", "followTimeFormat"}});
	CHECK(timeBase("midi") == "musical");
	run(client, "project.set", {{"timeFormat", "seconds"}});
	CHECK(timeBase("midi") == "linear");
	run(client, "project.set", {{"timeFormat", "barsBeats"}});
	CHECK(timeBase("midi") == "musical");
	CHECK_THAT(reject(client, "settings.set", {{"key", "editing.defaultTimeBase"}, {"value", "seconds"}}),
		ContainsSubstring("defaultTimeBase"));
}

TEST_CASE("tempo events ramp or jump, and the first one stays", "[unit]")
{
	Engine engine;
	Client client(engine);
	run(client, "project.set", {{"tempoTrackActive", true}});
	// Ramps from 120 to 60 BPM over four quarters: they take 4 * ln(2) seconds, more than four quarters at 120.
	run(client, "tempo.add", point(4, 60.0, true));
	const gnubase::time::TempoMap& tempo = engine.renderSnapshots().acquire().tempo;
	CHECK(tempo.bpmAt(2 * g_ticksPerQuarter) == 90.0);
	CHECK(std::abs(tempo.toSeconds(4 * g_ticksPerQuarter) - (4.0 * std::log(2.0))) < 1e-9);

	// A tempo event at a position that has one changes it.
	run(client, "tempo.add", point(4, 80.0, false));
	CHECK(run(client, "project.get").at("tempo") == Json::array({point(0, 120.0), point(4, 80.0)}));

	CHECK_THAT(reject(client, "tempo.remove", {{"positions", {0}}}), ContainsSubstring("first tempo event"));
	CHECK_THAT(reject(client, "tempo.set", {{"events", {{{"position", 0}, {"to", g_ticksPerQuarter}}}}}),
		ContainsSubstring("project start"));
	CHECK_THAT(reject(client, "tempo.set", {{"events", {{{"position", 4 * g_ticksPerQuarter}, {"to", 0}}}}}),
		ContainsSubstring("project start"));
	CHECK_THAT(reject(client, "tempo.set", {{"events", {{{"position", g_ticksPerQuarter}, {"bpm", 90.0}}}}}),
		ContainsSubstring("no tempo event"));
	CHECK_THAT(reject(client, "tempo.add", point(8, 1000.0)), ContainsSubstring("1 to 999"));
	CHECK_THAT(
		reject(client, "tempo.set", {{"events", {{{"position", 0}, {"bpm", 0.5}}}}}), ContainsSubstring("1 to 999"));

	// Changes happen at once: two events can swap places.
	run(client, "tempo.add", point(8, 100.0));
	run(client, "tempo.set",
		{{"events",
			{{{"position", 4 * g_ticksPerQuarter}, {"to", 8 * g_ticksPerQuarter}},
				{{"position", 8 * g_ticksPerQuarter}, {"to", 4 * g_ticksPerQuarter}, {"ramp", true}}}}});
	CHECK(run(client, "project.get").at("tempo") ==
		Json::array({point(0, 120.0), point(4, 100.0, true), point(8, 80.0)}));
	CHECK_THAT(reject(client, "tempo.set",
				   {{"events", {{{"position", 4 * g_ticksPerQuarter}, {"to", 8 * g_ticksPerQuarter}}}}}),
		ContainsSubstring("two tempo events"));
	// A tempo event at the cursor (Insert Tempo Event at Cursor), in ticks at the tempo before it.
	run(client, "transport.locate", {{"position", 2 * g_quarterSamples}});
	const int64_t cursor = engine.renderSnapshots().acquire().tempo.toTicks(2 * g_quarterSamples, 48000.0);
	const Json atCursor = run(client, "tempo.add", {{"position", "cursor"}, {"bpm", 70.0}, {"ramp", false}});
	CHECK(atCursor.at("position") == cursor);
	CHECK(atCursor.at("tempo").at("tempoTrack").size() == 4);
}

TEST_CASE("new tempo events ramp, jump or follow the event before them by the user setting", "[unit]")
{
	Engine engine; // with user settings in memory
	Client client(engine);
	const auto add = [&client](int64_t quarters, double bpm)
	{ run(client, "tempo.add", {{"position", quarters * g_ticksPerQuarter}, {"bpm", bpm}}); };
	run(client, "tempo.add", point(4, 90.0, true));
	add(8, 100.0); // auto: like the ramp before it
	run(client, "settings.set", {{"key", "editing.tempoType"}, {"value", "step"}});
	add(12, 110.0);
	run(client, "settings.set", {{"key", "editing.tempoType"}, {"value", "ramp"}});
	add(16, 120.0);
	CHECK(run(client, "project.get").at("tempoTrack") ==
		Json::array(
			{point(0, 120.0), point(4, 90.0, true), point(8, 100.0, true), point(12, 110.0), point(16, 120.0, true)}));
}

TEST_CASE("a tapped tempo sets the project tempo or adds a tempo event, by the tap tempo mode", "[unit]")
{
	Engine engine; // with user settings in memory
	Client client(engine);

	// Automatic in fixed tempo mode: the project tempo, rounded to 0.001 BPM.
	Json tapped = run(client, "tempo.tap", {{"bpm", 93.12345}});
	CHECK(tapped == Json{{"bpm", 93.123}, {"applied", "project"}, {"position", nullptr}});
	CHECK(run(client, "project.get").at("fixedTempo") == 93.123);

	run(client, "project.set", {{"tempoTrackActive", true}, {"tempo", 120.0}});
	run(client, "transport.locate", {{"position", 4 * g_quarterSamples}});
	// Automatic in tempo track mode: at the selection's start, else at the cursor while stopped.
	tapped = run(client, "tempo.tap", {{"bpm", 100.0}, {"selection", 2 * g_ticksPerQuarter}});
	CHECK(tapped.at("position") == 2 * g_ticksPerQuarter);
	tapped = run(client, "tempo.tap", {{"bpm", 110.0}});
	CHECK(tapped.at("applied") == "event");
	// Four quarters at 120 BPM are two seconds: two quarters at 120, then one second at 100 BPM, 5/3 quarters.
	CHECK(tapped.at("position") == (2 * g_ticksPerQuarter) + (5 * g_ticksPerQuarter / 3));
	CHECK(run(client, "tempo.tap", {{"bpm", 110.0}, {"mode", "cursor"}}).at("position") == tapped.at("position"));

	// Last playback start position: where playback started.
	run(client, "transport.locate", {{"position", g_quarterSamples}});
	run(client, "transport.play");
	run(client, "transport.locate", {{"position", 6 * g_quarterSamples}});
	tapped = run(client, "tempo.tap", {{"bpm", 130.0}});
	CHECK(tapped.at("position") == g_ticksPerQuarter);
	run(client, "transport.stop");

	// Selection start without a selection, and display only, change nothing.
	const Json before = run(client, "project.get").at("tempo");
	CHECK(run(client, "tempo.tap", {{"bpm", 140.0}, {"mode", "selection"}}).at("applied") == "none");
	CHECK(run(client, "tempo.tap", {{"bpm", 140.0}, {"mode", "display"}}).at("applied") == "none");
	CHECK(run(client, "project.get").at("tempo") == before);

	// Set project tempo in tempo track mode: the first tempo event. The mode defaults to the user setting.
	run(client, "settings.set", {{"key", "transport.tapTempoMode"}, {"value", "project"}});
	CHECK(run(client, "tempo.tap", {{"bpm", 90.0}}).at("applied") == "project");
	CHECK(run(client, "project.get").at("tempo")[0].at("bpm") == 90.0);
}

TEST_CASE("the tempo modes and the tempo track's events are saved with the project", "[integration]")
{
	const gnubase::test::TempDir dir;
	const std::string file = (dir.path() / "tempo.dawproject").u8string();
	Engine engine;
	Client client(engine);
	addTrack(client, {{"type", "tempo"}});
	run(client, "tempo.add", point(4, 90.0, true));
	run(client, "project.set", {{"tempo", 110.0}}); // the fixed tempo
	run(client, "project.save", {{"path", file}});
	const Json before = run(client, "project.get");
	run(client, "project.new");
	run(client, "project.load", {{"path", file}});
	const Json after = run(client, "project.get");
	for (const char* key : {"tempo", "tempoTrackActive", "tempoTrack", "fixedTempo"})
		CHECK(after.at(key) == before.at(key));
	CHECK(after.at("tracks").at(0).at("type") == "tempo");
}

TEST_CASE("the tempo track is exported and imported as an XML file with the time signature events", "[integration]")
{
	const gnubase::test::TempDir dir;
	const std::string file = (dir.path() / "tempo.xml").u8string();
	Engine engine;
	Client client(engine);
	// The tempo track's events, which fixed tempo mode keeps too, and the time signature events.
	run(client, "project.set", {{"tempo", 110.0}});
	run(client, "tempo.set", {{"events", {{{"position", 0}, {"bpm", 93.457}}}}});
	run(client, "tempo.add", point(4, 90.0, true));
	run(client, "tempo.add", point(8, 140.5, false));
	run(client, "signature.add", {{"bar", 2}, {"numerator", 3}, {"denominator", 4}});
	run(client, "signature.add", {{"bar", 5}, {"numerator", 7}, {"denominator", 8}});
	const Json exported = run(client, "tempo.export", {{"path", file}});
	CHECK(exported.at("tempoEvents") == 3);
	CHECK(exported.at("signatureEvents") == 3);
	const Json before = run(client, "project.get");
	CHECK(!before.at("tempoTrackActive").get<bool>());

	// Importing replaces the tempo track and the time signature events, and the tempo changes play.
	run(client, "project.new");
	run(client, "project.set", {{"tempo", 100.0}});
	run(client, "signature.add", {{"bar", 3}, {"numerator", 5}, {"denominator", 4}});
	const Json empty = run(client, "project.get");
	eventNames(client);
	const Json imported = run(client, "tempo.import", {{"path", file}});
	CHECK(imported.at("tempo").at("tempoTrack") == before.at("tempoTrack"));
	const std::vector<std::string> names = eventNames(client);
	CHECK(std::count(names.begin(), names.end(), "tempo.changed") == 1);
	CHECK(std::count(names.begin(), names.end(), "signature.changed") == 1);
	Json after = run(client, "project.get");
	CHECK(after.at("tempoTrack") == before.at("tempoTrack"));
	CHECK(after.at("signatures") == before.at("signatures"));
	CHECK(after.at("tempoTrackActive").get<bool>());
	CHECK(after.at("tempo") == before.at("tempoTrack"));
	CHECK(after.at("fixedTempo") == 100.0);

	// One undo step.
	run(client, "history.undo");
	after = run(client, "project.get");
	for (const char* key : {"tempo", "tempoTrackActive", "tempoTrack", "fixedTempo", "signatures"})
		CHECK(after.at(key) == empty.at(key));
	run(client, "history.redo");

	// Exported again, the imported tempo track gives the same file.
	const std::string again = (dir.path() / "again.xml").u8string();
	run(client, "tempo.export", {{"path", again}});
	CHECK(gnubase::test::readText(dir.path() / "again.xml") == gnubase::test::readText(dir.path() / "tempo.xml"));
}

TEST_CASE("a tempo track file with a single tempo leaves fixed tempo mode as it is", "[integration]")
{
	const gnubase::test::TempDir dir;
	const std::string file = (dir.path() / "tempo.xml").u8string();
	Engine engine;
	Client client(engine);
	run(client, "tempo.set", {{"events", {{{"position", 0}, {"bpm", 87.0}}}}});
	run(client, "tempo.export", {{"path", file}});
	run(client, "project.new");
	run(client, "project.set", {{"tempo", 100.0}});
	run(client, "tempo.import", {{"path", file}});
	const Json after = run(client, "project.get");
	CHECK(!after.at("tempoTrackActive").get<bool>());
	CHECK(after.at("fixedTempo") == 100.0);
	CHECK(after.at("tempoTrack")[0].at("bpm") == 87.0);
}

TEST_CASE("importing a file that isn't a tempo track file changes nothing", "[integration]")
{
	const gnubase::test::TempDir dir;
	Engine engine;
	Client client(engine);
	run(client, "tempo.add", point(4, 90.0));
	const Json before = run(client, "project.get");
	CHECK_THAT(reject(client, "tempo.import", {{"path", (dir.path() / "missing.xml").u8string()}}),
		ContainsSubstring("missing.xml"));
	const std::string project = (dir.path() / "project.xml").u8string();
	std::ofstream(std::filesystem::u8path(project)) << R"(<?xml version="1.0"?><Project version="1.0"/>)";
	CHECK_THAT(reject(client, "tempo.import", {{"path", project}}), ContainsSubstring("no tempo track"));
	const std::string broken = (dir.path() / "broken.xml").u8string();
	std::ofstream(std::filesystem::u8path(broken)) << "<TempoTrack><Transport>";
	CHECK_THAT(reject(client, "tempo.import", {{"path", broken}}), ContainsSubstring("broken.xml"));
	const std::string invalid = (dir.path() / "invalid.xml").u8string();
	std::ofstream(std::filesystem::u8path(invalid))
		<< R"(<TempoTrack version="1.0"><Transport><Tempo unit="bpm" value="2000"/></Transport></TempoTrack>)";
	CHECK_THAT(reject(client, "tempo.import", {{"path", invalid}}), ContainsSubstring("invalid"));
	const Json after = run(client, "project.get");
	CHECK(after.at("tempoTrack") == before.at("tempoTrack"));
	CHECK(after.at("signatures") == before.at("signatures"));
	CHECK_THAT(reject(client, "tempo.export", {{"path", (dir.path() / "no" / "such" / "folder.xml").u8string()}}),
		ContainsSubstring("folder.xml"));
}
