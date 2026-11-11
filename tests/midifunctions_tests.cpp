// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/rendersnapshot.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/midi/functions.h"
#include "engine/midi/midivalues.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using namespace gnubase::engine::midi;
using Catch::Matchers::ContainsSubstring;
using gnubase::time::g_ticksPerQuarter;
using gnubase::time::Samples;
using gnubase::time::Ticks;
using gnubase::time::TimeBase;

namespace
{
	constexpr Ticks g_quarter = g_ticksPerQuarter;
	constexpr uint16_t g_velocity = 40000;

	// Builds a part with notes and controllers whose ids are given, so tests can name them.
	struct PartBuilder
	{
		MidiPart part;

		PartBuilder() { part.length = 16 * g_quarter; }

		PartBuilder& note(
			ObjectId id, Ticks position, Ticks length, int pitch, int channel = 0, uint16_t velocity = g_velocity)
		{
			part.notes[id] = Note{id, position, length, static_cast<uint8_t>(pitch), velocity,
				static_cast<uint8_t>(channel), g_defaultReleaseVelocity, std::nullopt, 1.0, 0.0, {}};
			return *this;
		}

		PartBuilder& controller(
			ObjectId id, Ticks position, ControllerType type, int number, uint32_t value, int channel = 0)
		{
			part.controllers[id] = Controller{
				id, position, type, static_cast<uint8_t>(number), value, static_cast<uint8_t>(channel), {}, {}};
			return *this;
		}

		PartBuilder& cc(ObjectId id, Ticks position, int number, int value, int channel = 0)
		{
			return controller(id, position, ControllerType::Control, number,
				controllerFromMidi1(static_cast<uint8_t>(value)), channel);
		}
	};

	Ticks start(const MidiPart& part, ObjectId note)
	{
		return part.notes.at(note).position;
	}

	Ticks length(const MidiPart& part, ObjectId note)
	{
		return part.notes.at(note).length;
	}

	std::set<ObjectId> noteIds(const MidiPart& part)
	{
		std::set<ObjectId> ids;
		for (const auto& [id, note] : part.notes)
			ids.insert(id);
		return ids;
	}

	std::set<ObjectId> controllerIds(const MidiPart& part)
	{
		std::set<ObjectId> ids;
		for (const auto& [id, controller] : part.controllers)
			ids.insert(id);
		return ids;
	}

	std::vector<ControllerType> controllerTypes(const MidiPart& part)
	{
		std::vector<ControllerType> types;
		types.reserve(part.controllers.size());
		for (const auto& [id, controller] : part.controllers)
			types.push_back(controller.type);
		std::sort(types.begin(), types.end());
		return types;
	}

	// A project with tracks added the way track.add adds them.
	ObjectId addTrack(Project& project, TrackType type, const std::string& name)
	{
		Track track;
		track.id = project.newId();
		track.type = type;
		track.name = name;
		const Channel channel{project.newId(), track.id,
			type == TrackType::Midi ? ChannelFormat::None : ChannelFormat::Stereo, 0, 0, {}, 0.0, 0.0};
		track.channel = channel.id;
		project.channels[channel.id] = channel;
		project.tracks.push_back(track.id);
		project.trackById[track.id] = track;
		return track.id;
	}

	// position and length are in the track's time base: ticks, or samples on a linear track.
	ObjectId addPart(Project& project, ObjectId track, int64_t position, int64_t length, const MidiPart& content = {})
	{
		MidiPart part = content;
		part.id = project.newId();
		part.track = track;
		part.name = project.trackById.at(track).name;
		part.position = {project.trackById.at(track).timeBase, position};
		part.length = length;
		std::map<ObjectId, Note> notes;
		for (auto& [id, note] : part.notes)
		{
			note.id = project.newId();
			notes[note.id] = note;
		}
		part.notes = std::move(notes);
		std::map<ObjectId, Controller> controllers;
		for (auto& [id, controller] : part.controllers)
		{
			controller.id = project.newId();
			controllers[controller.id] = controller;
		}
		part.controllers = std::move(controllers);
		project.midiParts[part.id] = part;
		return part.id;
	}

	// The notes of a part as (position, pitch, length, channel), in order.
	std::vector<std::vector<int64_t>> notesOf(const MidiPart& part)
	{
		std::vector<std::vector<int64_t>> notes;
		notes.reserve(part.notes.size());
		for (const auto& [id, note] : part.notes)
			notes.push_back({note.position, note.pitch, note.length, note.channel});
		std::sort(notes.begin(), notes.end());
		return notes;
	}

	// The events of a controller on channel 1 as (position, MIDI 1.0 value), in order.
	std::vector<std::pair<Ticks, int>> laneOf(const MidiPart& part, int number)
	{
		std::vector<std::pair<Ticks, int>> lane;
		for (const auto& [id, controller] : part.controllers)
			if (controller.type == ControllerType::Control && controller.number == number && controller.channel == 0)
				lane.emplace_back(controller.position, controllerToMidi1(controller.value));
		std::sort(lane.begin(), lane.end());
		return lane;
	}

	// New ids for the events a function adds, above the ids the tests give.
	struct NewIds
	{
		ObjectId next = 1000;
		NewId function()
		{
			return [this] { return next++; };
		}
	};

	const MidiPart& onlyPartOn(const Project& project, ObjectId track)
	{
		const MidiPart* found = nullptr;
		for (const auto& [id, part] : project.midiParts)
			if (part.track == track)
			{
				REQUIRE(found == nullptr);
				found = &part;
			}
		REQUIRE(found != nullptr);
		return *found;
	}
}

TEST_CASE("transpose moves notes by semitones and by octaves into MIDI's range", "[unit]")
{
	TransposeSettings settings;
	settings.semitones = 5;
	CHECK(transposePitch(60, settings) == 65);
	CHECK(transposePitch(125, settings) == 118); // 130 is above 127: an octave lower
	settings.semitones = -12;
	CHECK(transposePitch(5, settings) == 5); // -7: an octave higher again

	MidiPart part = PartBuilder()
						.note(1, 0, g_quarter, 60)
						.note(2, g_quarter, g_quarter, 64)
						.controller(3, 0, ControllerType::PolyPressure, 60, 100)
						.controller(4, 0, ControllerType::Control, 60, 100)
						.part;
	settings.semitones = 2;
	transpose(part, NoteSelection({2}), settings);
	CHECK(part.notes.at(1).pitch == 60);
	CHECK(part.notes.at(2).pitch == 66);
	CHECK(part.controllers.at(3).number == 60); // a selection leaves poly pressure alone

	transpose(part, NoteSelection(), settings);
	CHECK(part.notes.at(1).pitch == 62);
	CHECK(part.notes.at(2).pitch == 68);
	CHECK(part.controllers.at(3).number == 62); // poly pressure follows its notes
	CHECK(part.controllers.at(4).number == 60); // a controller number is no pitch
}

TEST_CASE("transpose keeps notes in range by octaves, or at the limit they crossed", "[unit]")
{
	TransposeSettings settings;
	settings.keepInRange = true;
	settings.lowest = 60;
	settings.highest = 71;
	settings.semitones = 14;
	CHECK(transposePitch(60, settings) == 62); // 74 is above B: an octave lower
	settings.semitones = -3;
	CHECK(transposePitch(60, settings) == 69); // 57 is below C: an octave higher

	// A range narrower than an octave: as far as possible.
	settings.lowest = 60;
	settings.highest = 62;
	settings.semitones = 5;
	CHECK(transposePitch(60, settings) == 62); // 65 doesn't fit in any octave: the highest note
	settings.semitones = -5;
	CHECK(transposePitch(64, settings) == 60); // 59: the lowest note

	// Both limits on one pitch put every note there.
	settings.lowest = 67;
	settings.highest = 67;
	settings.semitones = 0;
	for (const int pitch : {0, 30, 66, 67, 68, 127})
		CHECK(transposePitch(pitch, settings) == 67);
}

TEST_CASE("transpose corrects notes from one scale to another", "[unit]")
{
	TransposeSettings settings;
	settings.from = Scale{0, ScaleType::Major};
	settings.to = Scale{0, ScaleType::Minor};
	// C major to C minor: E, A and B become E flat, A flat and B flat; C, D, F and G stay.
	const std::map<int, int> cToCMinor{
		{60, 60}, {62, 62}, {64, 63}, {65, 65}, {67, 67}, {69, 68}, {71, 70}, {72, 72}, {40, 39}};
	for (const auto& [pitch, expected] : cToCMinor)
		CHECK(transposePitch(pitch, settings) == expected);
	// A note outside C major goes to the closest note of C minor, the lower one on a tie: C sharp to C.
	CHECK(transposePitch(61, settings) == 60);

	// C major to A minor: the same notes, a third lower.
	settings.to = Scale{9, ScaleType::Minor};
	CHECK(transposePitch(60, settings) == 57);
	CHECK(transposePitch(64, settings) == 60);
	CHECK(transposePitch(67, settings) == 64);

	// To a pentatonic scale every note snaps to its closest scale note.
	settings.to = Scale{0, ScaleType::MajorPentatonic};
	CHECK(transposePitch(65, settings) == 64); // F: E and G are equally close, E is lower
	CHECK(transposePitch(71, settings) == 72); // B: C is closer than A

	// Semitones first, then the scale correction.
	settings.to = Scale{0, ScaleType::Minor};
	settings.semitones = 2;
	CHECK(transposePitch(62, settings) == 63); // D up to E, which C minor turns into E flat

	CHECK(scaleTypeFromName("harmonicMinor") == ScaleType::HarmonicMinor);
	CHECK(std::string(scaleTypeName(ScaleType::WholeTone)) == "wholeTone");
	CHECK(!scaleTypeFromName("chromatic").has_value());
}

TEST_CASE("legato extends notes to the next note on their channel", "[unit]")
{
	const MidiPart original = PartBuilder()
								  .note(1, 0, g_quarter / 4, 60)
								  .note(2, 0, g_quarter / 4, 64) // a chord with note 1
								  .note(3, g_quarter, g_quarter / 4, 62)
								  .note(4, g_quarter / 2, 10, 70, 1) // on another channel
								  .note(5, 2 * g_quarter, g_quarter / 4, 65)
								  .part;
	MidiPart part = original;
	legato(part, NoteSelection(), 0, false);
	CHECK(length(part, 1) == g_quarter);
	CHECK(length(part, 2) == g_quarter);
	CHECK(length(part, 3) == g_quarter);
	CHECK(length(part, 4) == 10);            // no next note on channel 2
	CHECK(length(part, 5) == g_quarter / 4); // the last note keeps its length

	SECTION("with an overlap or a gap")
	{
		part = original;
		legato(part, NoteSelection(), 100, false);
		CHECK(length(part, 1) == g_quarter + 100);
		part = original;
		legato(part, NoteSelection(), -100, false);
		CHECK(length(part, 1) == g_quarter - 100);
		part = original;
		legato(part, NoteSelection(), -10 * g_quarter, false);
		CHECK(length(part, 1) == 1); // never shorter than a tick
	}
	SECTION("only the selected notes")
	{
		part = original;
		legato(part, NoteSelection({1, 5}), 0, false);
		CHECK(length(part, 1) == g_quarter); // to note 3, which is not selected
		CHECK(length(part, 2) == g_quarter / 4);
		CHECK(length(part, 3) == g_quarter / 4);
	}
	SECTION("between selected notes only")
	{
		part = original;
		legato(part, NoteSelection({1, 5}), 0, true);
		CHECK(length(part, 1) == 2 * g_quarter); // to note 5, past note 3
		CHECK(length(part, 3) == g_quarter / 4);
	}
}

TEST_CASE("fixed lengths and fixed velocities set the selected notes' length and velocity", "[unit]")
{
	MidiPart part = PartBuilder().note(1, 0, 10, 60).note(2, 100, 20, 62, 0, 100).note(3, 200, 30, 64).part;
	fixLengths(part, NoteSelection({1, 2}), g_quarter / 2);
	CHECK(length(part, 1) == g_quarter / 2);
	CHECK(length(part, 2) == g_quarter / 2);
	CHECK(length(part, 3) == 30);

	fixVelocities(part, NoteSelection(), 1000);
	for (const auto& [id, note] : part.notes)
		CHECK(note.velocity == 1000);
}

TEST_CASE("sustain pedal to note length extends notes to the pedal's release and removes the pedal", "[unit]")
{
	MidiPart part = PartBuilder()
						.cc(10, g_quarter / 2, 64, 127) // pedal down during note 1
						.cc(11, 4 * g_quarter, 64, 0)   // and up
						.cc(12, 0, 7, 100)              // volume stays
						.cc(13, 0, 64, 127, 1)          // channel 2: down and never up
						.note(1, 0, g_quarter, 60)
						.note(2, 2 * g_quarter, g_quarter / 2, 64)
						.note(3, 3 * g_quarter, g_quarter / 2, 60) // the same pitch as note 1, struck again
						.note(4, 5 * g_quarter, g_quarter / 2, 67) // after the release
						.note(5, 0, g_quarter / 4, 60, 1)          // channel 2
						.note(6, 0, g_quarter / 4, 62, 2)          // channel 3 has no pedal
						.part;
	pedalToNoteLength(part, NoteSelection(), part.length);
	CHECK(length(part, 1) == 3 * g_quarter); // up to the next C
	CHECK(length(part, 2) == 2 * g_quarter); // up to the release
	CHECK(length(part, 3) == g_quarter);     // up to the release
	CHECK(length(part, 4) == g_quarter / 2);
	CHECK(length(part, 5) == part.length); // the pedal is never released: to the part's end
	CHECK(length(part, 6) == g_quarter / 4);
	CHECK(controllerIds(part) == std::set<ObjectId>{12});

	SECTION("a note that ends where the pedal goes down is not held")
	{
		MidiPart edge =
			PartBuilder().cc(1, g_quarter, 64, 127).cc(2, 2 * g_quarter, 64, 0).note(3, 0, g_quarter, 60).part;
		pedalToNoteLength(edge, NoteSelection(), edge.length);
		CHECK(length(edge, 3) == g_quarter);
	}
}

TEST_CASE("delete overlaps shortens notes of the same pitch (mono) or of any pitch (poly)", "[unit]")
{
	const MidiPart original = PartBuilder()
								  .note(1, 0, 2 * g_quarter, 60)
								  .note(2, g_quarter, 2 * g_quarter, 60)
								  .note(3, g_quarter / 2, 2 * g_quarter, 64)
								  .note(4, g_quarter / 2, g_quarter, 67)        // a chord with note 3
								  .note(5, g_quarter / 4, 4 * g_quarter, 60, 1) // the same pitch on another channel
								  .part;
	SECTION("mono")
	{
		MidiPart part = original;
		deleteOverlaps(part, NoteSelection(), OverlapMode::Mono);
		CHECK(length(part, 1) == g_quarter);
		CHECK(length(part, 2) == 2 * g_quarter);
		CHECK(length(part, 3) == 2 * g_quarter);
		CHECK(length(part, 5) == 4 * g_quarter);
	}
	SECTION("poly")
	{
		MidiPart part = original;
		deleteOverlaps(part, NoteSelection(), OverlapMode::Poly);
		CHECK(length(part, 1) == g_quarter / 2); // up to the chord
		CHECK(length(part, 3) == g_quarter / 2); // up to note 2
		CHECK(length(part, 4) == g_quarter / 2);
		CHECK(length(part, 2) == 2 * g_quarter); // the last one
		CHECK(length(part, 5) == 4 * g_quarter); // alone on its channel
	}
	SECTION("only the selected notes get shorter")
	{
		MidiPart part = original;
		deleteOverlaps(part, NoteSelection({3}), OverlapMode::Poly);
		CHECK(length(part, 1) == 2 * g_quarter);
		CHECK(length(part, 3) == g_quarter / 2);
	}
}

TEST_CASE("delete doubles keeps one of the notes with the same pitch, channel and position", "[unit]")
{
	MidiPart part = PartBuilder()
						.note(1, 0, 10, 60)
						.note(2, 0, 20, 60) // longer: kept
						.note(3, 0, 20, 60)
						.note(4, 0, 10, 60, 1) // another channel
						.note(5, 1, 10, 60)    // another position
						.note(6, 0, 10, 61)    // another pitch
						.note(7, 50, 10, 70)
						.note(8, 50, 10, 70)
						.part;
	deleteDoubles(part, NoteSelection({1, 2, 3, 4, 5, 6, 7}));
	CHECK(noteIds(part) == std::set<ObjectId>{2, 4, 5, 6, 7, 8}); // 8 isn't selected, so 7 has no double
	deleteDoubles(part, NoteSelection());
	CHECK(noteIds(part) == std::set<ObjectId>{2, 4, 5, 6, 7});
}

TEST_CASE("delete controllers removes controllers, pitch bend and pressure", "[unit]")
{
	MidiPart part = PartBuilder()
						.cc(1, 0, 7, 100)
						.cc(2, 0, 64, 127)
						.controller(3, 0, ControllerType::PitchBend, 0, g_pitchBendCenter)
						.controller(4, 0, ControllerType::ChannelPressure, 0, 5)
						.controller(5, 0, ControllerType::PolyPressure, 60, 5)
						.controller(6, 0, ControllerType::Program, 4, g_noBank)
						.controller(7, 0, ControllerType::SysEx, 0, 0)
						.note(8, 0, 10, 60)
						.part;
	deleteControllers(part);
	CHECK(controllerIds(part) == std::set<ObjectId>{6, 7});
	CHECK(noteIds(part) == std::set<ObjectId>{8});
}

TEST_CASE("delete continuous controllers keeps switches such as the sustain pedal", "[unit]")
{
	for (const int number : {1, 2, 7, 10, 11, 74, 91})
		CHECK(isContinuousController(number));
	for (const int number : {0, 6, 32, 38, 64, 65, 66, 67, 68, 69, 96, 99, 101, 120, 121, 123, 127})
		CHECK(!isContinuousController(number));

	MidiPart part = PartBuilder()
						.cc(1, 0, 1, 50)
						.cc(2, 10, 7, 100)
						.cc(3, 20, 64, 127)
						.cc(4, 30, 0, 1)
						.controller(5, 0, ControllerType::PitchBend, 0, g_pitchBendCenter)
						.part;
	deleteContinuousControllers(part);
	CHECK(controllerIds(part) == std::set<ObjectId>{3, 4, 5});
}

TEST_CASE("delete notes removes notes that are too short or too soft", "[unit]")
{
	const MidiPart original = PartBuilder()
								  .note(1, 0, 10, 60, 0, 100)     // short and soft
								  .note(2, 0, 10, 62, 0, 50000)   // short
								  .note(3, 0, 1000, 64, 0, 100)   // soft
								  .note(4, 0, 1000, 65, 0, 50000) // neither
								  .part;
	DeleteNotesSettings settings;
	MidiPart part = original;
	settings.minimumLength = 100;
	deleteNotes(part, NoteSelection(), settings);
	CHECK(noteIds(part) == std::set<ObjectId>{3, 4});

	part = original;
	settings.minimumLength.reset();
	settings.minimumVelocity = 1000;
	deleteNotes(part, NoteSelection(), settings);
	CHECK(noteIds(part) == std::set<ObjectId>{2, 4});

	part = original;
	settings.minimumLength = 100;
	deleteNotes(part, NoteSelection(), settings);
	CHECK(noteIds(part) == std::set<ObjectId>{4});

	part = original;
	settings.both = true;
	deleteNotes(part, NoteSelection(), settings);
	CHECK(noteIds(part) == std::set<ObjectId>{2, 3, 4});

	part = original;
	settings.both = false;
	deleteNotes(part, NoteSelection({1, 4}), settings);
	CHECK(noteIds(part) == std::set<ObjectId>{2, 3, 4});
}

TEST_CASE("restrict polyphony ends the oldest notes so that at most the given number sound", "[unit]")
{
	SECTION("a chord with too many notes loses its lowest")
	{
		MidiPart part = PartBuilder()
							.note(1, 0, g_quarter, 60)
							.note(2, 0, g_quarter, 64)
							.note(3, 0, g_quarter, 67)
							.note(4, 0, g_quarter, 71)
							.part;
		restrictPolyphony(part, NoteSelection(), 3);
		CHECK(noteIds(part) == std::set<ObjectId>{2, 3, 4});
	}
	SECTION("a new note ends the one that started first")
	{
		MidiPart part = PartBuilder()
							.note(1, 0, 4 * g_quarter, 60)
							.note(2, g_quarter, 4 * g_quarter, 64)
							.note(3, 2 * g_quarter, 4 * g_quarter, 67)
							.note(4, 3 * g_quarter, g_quarter, 72)
							.note(5, 0, 8 * g_quarter, 40, 1) // another channel has voices of its own
							.part;
		restrictPolyphony(part, NoteSelection(), 2);
		CHECK(length(part, 1) == 2 * g_quarter);
		CHECK(length(part, 2) == 2 * g_quarter);
		CHECK(length(part, 3) == 4 * g_quarter);
		CHECK(length(part, 4) == g_quarter);
		CHECK(length(part, 5) == 8 * g_quarter);
	}
	SECTION("one voice makes a monophonic line")
	{
		MidiPart part = PartBuilder().note(1, 0, g_quarter, 60).note(2, g_quarter / 2, g_quarter, 62).part;
		restrictPolyphony(part, NoteSelection(), 1);
		CHECK(length(part, 1) == g_quarter / 2);
		CHECK(length(part, 2) == g_quarter);
	}
}

TEST_CASE("thin out removes dense and repeated controller events but keeps the curve's shape", "[unit]")
{
	PartBuilder builder;
	// Controller 1 rises in 64 steps of 10 ticks, then falls; the value repeats at the top.
	ObjectId id = 1;
	for (int i = 0; i <= 64; ++i)
		builder.cc(id++, Ticks{i} * 10, 1, i);
	builder.cc(id++, 645, 1, 64); // a repeated value
	for (int i = 1; i <= 64; ++i)
		builder.cc(id++, 650 + (Ticks{i} * 10), 1, 64 - i);
	const ObjectId last = id - 1;
	builder.cc(500, 5, 7, 10).cc(501, 6, 7, 20); // another controller: a lane of its own
	builder.controller(600, 0, ControllerType::Program, 3, g_noBank)
		.controller(601, 1, ControllerType::Program, 4, g_noBank);
	MidiPart part = builder.part;
	thinOut(part, 100);

	std::vector<std::pair<Ticks, int>> curve;
	for (const auto& [controllerId, controller] : part.controllers)
		if (controller.type == ControllerType::Control && controller.number == 1)
			curve.emplace_back(controller.position, controllerToMidi1(controller.value));
	std::sort(curve.begin(), curve.end());
	REQUIRE(curve.size() >= 3);
	CHECK(curve.front() == std::pair<Ticks, int>{0, 0});
	CHECK(curve.back() == std::pair<Ticks, int>{1290, 0}); // the last event stays
	CHECK(part.controllers.count(last) == 1);
	CHECK(std::find(curve.begin(), curve.end(), std::pair<Ticks, int>{640, 64}) != curve.end()); // the turning point
	CHECK(part.controllers.count(66) == 0);                                                      // the repeated value
	for (size_t i = 1; i + 1 < curve.size(); ++i)
		if (curve[i].first != 640)
			CHECK(curve[i].first - curve[i - 1].first >= 100);
	CHECK(part.controllers.count(500) == 1);
	CHECK(part.controllers.count(501) == 1); // the last of its lane
	CHECK(part.controllers.count(600) == 1); // program changes are never thinned
	CHECK(part.controllers.count(601) == 1);
}

TEST_CASE("thin out keeps a step where it happens", "[unit]")
{
	// Controller 1 steps to 64 shortly after the start and holds it: the repeat a bar later must not take its place.
	MidiPart part =
		PartBuilder().cc(1, 0, 1, 0).cc(2, 50000, 1, 64).cc(3, 4 * g_quarter, 1, 64).cc(4, 8 * g_quarter, 1, 127).part;
	thinOut(part, g_quarter / 16);
	CHECK(laneOf(part, 1) == std::vector<std::pair<Ticks, int>>{{0, 0}, {50000, 64}, {8 * g_quarter, 127}});
}

TEST_CASE("thin out leaves switches, bank select and parameter numbers alone", "[unit]")
{
	// NRPN 1 = 64, then NRPN 2 = 64: the second data entry repeats the first one's value but sets another parameter.
	MidiPart part = PartBuilder()
						.cc(1, 0, 99, 0)
						.cc(2, 0, 98, 1)
						.cc(3, 0, 6, 64)
						.cc(4, 10, 99, 0)
						.cc(5, 10, 98, 2)
						.cc(6, 10, 6, 64)
						.cc(7, 20, 64, 127) // the sustain pedal, pressed again and again
						.cc(8, 30, 64, 127)
						.cc(9, 40, 0, 1) // bank select
						.cc(10, 50, 0, 1)
						.part;
	const MidiPart original = part;
	thinOut(part, 100);
	CHECK(part.controllers == original.controllers);
}

TEST_CASE("reverse plays the notes' starts in reverse order and keeps their lengths", "[unit]")
{
	MidiPart part = PartBuilder()
						.note(1, g_quarter, 10, 60)
						.note(2, 2 * g_quarter, 20, 62)
						.note(3, 4 * g_quarter, 30, 64)
						.cc(4, 0, 7, 10)
						.cc(5, 3 * g_quarter, 7, 20)
						.part;
	MidiPart selected = part;
	reverse(selected, NoteSelection({1, 2}), selected.length, NewIds().function());
	CHECK(start(selected, 1) == 2 * g_quarter);
	CHECK(start(selected, 2) == g_quarter);
	CHECK(start(selected, 3) == 4 * g_quarter);
	CHECK(selected.controllers == part.controllers);

	reverse(part, NoteSelection(), part.length, NewIds().function());
	// Every event from 0 to 4 quarters: positions mirror around 2 quarters.
	CHECK(start(part, 1) == 3 * g_quarter);
	CHECK(start(part, 2) == 2 * g_quarter);
	CHECK(start(part, 3) == 0);
	CHECK(length(part, 1) == 10);
	CHECK(length(part, 3) == 30);
	// The volume was 10 for 3 quarters, then 20 for one: now 20 for one, then 10 for 3, and 20 again after them.
	CHECK(laneOf(part, 7) == std::vector<std::pair<Ticks, int>>{{0, 20}, {g_quarter, 10}, {4 * g_quarter, 20}});
	CHECK(part.controllers.at(5).position == 0);
	CHECK(part.controllers.at(4).position == g_quarter);
}

TEST_CASE("invert mirrors the notes in time: a note starts where its mirror image ended", "[unit]")
{
	MidiPart part = PartBuilder()
						.note(1, g_quarter, g_quarter, 60)
						.note(2, 3 * g_quarter, g_quarter / 2, 62)
						.cc(3, 2 * g_quarter, 7, 10)
						.part;
	MidiPart selected = part;
	invert(selected, NoteSelection({1, 2}), selected.length, NewIds().function());
	// The notes span 1 to 3.5 quarters.
	CHECK(start(selected, 1) == 5 * g_quarter / 2);
	CHECK(start(selected, 2) == g_quarter);
	CHECK(selected.controllers == part.controllers);

	invert(part, NoteSelection(), part.length, NewIds().function());
	CHECK(start(part, 1) == 5 * g_quarter / 2);
	CHECK(start(part, 2) == g_quarter);
	// The volume held from 2 to 3.5 quarters, now from 1 to 2.5 and on.
	CHECK(laneOf(part, 7) == std::vector<std::pair<Ticks, int>>{{g_quarter, 10}});
	CHECK(length(part, 1) == g_quarter);
}

TEST_CASE("reverse and invert play controller values backwards and leave program changes in place", "[unit]")
{
	const MidiPart original = PartBuilder()
								  .note(1, 0, g_quarter, 60)
								  .note(2, 3 * g_quarter, g_quarter, 62)
								  .cc(3, 0, 64, 127)           // the sustain pedal down
								  .cc(4, 2 * g_quarter, 64, 0) // and up
								  .cc(5, 0, 7, 100)            // the volume, set once
								  .cc(6, g_quarter / 2, 0, 1)  // bank select and program change
								  .controller(7, g_quarter / 2, ControllerType::Program, 4, 1)
								  .controller(8, g_quarter, ControllerType::SysEx, 0, 0)
								  .part;
	SECTION("invert")
	{
		MidiPart part = original;
		invert(part, NoteSelection(), part.length, NewIds().function());
		// From 0 to 4 quarters: the pedal held the first 2, now the last 2, and is released at the end.
		CHECK(start(part, 1) == 3 * g_quarter);
		CHECK(start(part, 2) == 0);
		CHECK(laneOf(part, 64) == std::vector<std::pair<Ticks, int>>{{0, 0}, {2 * g_quarter, 127}, {4 * g_quarter, 0}});
		CHECK(laneOf(part, 7) == std::vector<std::pair<Ticks, int>>{{0, 100}});
		CHECK(laneOf(part, 0) == std::vector<std::pair<Ticks, int>>{{g_quarter / 2, 1}});
		CHECK(part.controllers.at(7) == original.controllers.at(7));
		CHECK(part.controllers.at(8) == original.controllers.at(8));
	}
	SECTION("reverse")
	{
		MidiPart part = original;
		reverse(part, NoteSelection(), part.length, NewIds().function());
		// The starts span 0 to 3 quarters.
		CHECK(start(part, 1) == 3 * g_quarter);
		CHECK(start(part, 2) == 0);
		CHECK(laneOf(part, 64) == std::vector<std::pair<Ticks, int>>{{0, 0}, {g_quarter, 127}, {3 * g_quarter, 0}});
		CHECK(laneOf(part, 7) == std::vector<std::pair<Ticks, int>>{{0, 100}});
		CHECK(part.controllers.at(7) == original.controllers.at(7));
		CHECK(part.controllers.at(8) == original.controllers.at(8));
	}
	SECTION("values overwritten at the same tick go")
	{
		MidiPart part = PartBuilder().cc(1, 0, 1, 10).cc(2, g_quarter, 1, 20).cc(3, g_quarter, 1, 30).part;
		invert(part, NoteSelection(), part.length, NewIds().function());
		CHECK(laneOf(part, 1) == std::vector<std::pair<Ticks, int>>{{0, 10}, {g_quarter, 30}});
	}
}

TEST_CASE(
	"reverse and invert return a lane to its value before the part's end, where playback still plays it", "[unit]")
{
	// A one-bar part: playback drops controllers at or after its end.
	PartBuilder builder;
	builder.note(1, 0, g_quarter, 60)
		.note(2, 3 * g_quarter, g_quarter, 62) // the last note ends exactly at the part's end
		.cc(3, 0, 64, 127)                     // the sustain pedal down
		.cc(4, 2 * g_quarter, 64, 0)           // and up
		.cc(5, 0, 7, 100)                      // the volume
		.cc(6, 3 * g_quarter, 7, 50);
	builder.part.length = 4 * g_quarter;
	const MidiPart original = builder.part;
	const Ticks lastTick = (4 * g_quarter) - 1;
	SECTION("invert")
	{
		MidiPart part = original;
		invert(part, NoteSelection(), part.length, NewIds().function());
		CHECK(laneOf(part, 64) == std::vector<std::pair<Ticks, int>>{{0, 0}, {2 * g_quarter, 127}, {lastTick, 0}});
		CHECK(laneOf(part, 7) == std::vector<std::pair<Ticks, int>>{{0, 50}, {g_quarter, 100}, {lastTick, 50}});
	}
	SECTION("invert with a note past the part's end")
	{
		MidiPart part = original;
		part.length = 7 * g_quarter / 2;
		invert(part, NoteSelection(), part.length, NewIds().function());
		CHECK(
			laneOf(part, 64) == std::vector<std::pair<Ticks, int>>{{0, 0}, {2 * g_quarter, 127}, {part.length - 1, 0}});
	}
	SECTION("invert where the value that plays last lands past the part's end")
	{
		// The range is 0 to 5 quarters. The mirror image's last value lands at 4.5 quarters, where playback drops it,
		// so the lane returns to its value on the part's last tick even though that value is the same.
		MidiPart part = PartBuilder()
							.note(1, 0, g_quarter, 60)
							.note(2, 3 * g_quarter, 2 * g_quarter, 62) // runs past the part's end
							.cc(3, 0, 64, 0)
							.cc(4, g_quarter / 2, 64, 127)
							.cc(5, 3 * g_quarter, 64, 0)
							.cc(6, 0, 7, 50)
							.cc(7, g_quarter / 2, 7, 100)
							.cc(8, 3 * g_quarter, 7, 50)
							.part;
		part.length = 4 * g_quarter;
		invert(part, NoteSelection(), part.length, NewIds().function());
		CHECK(laneOf(part, 64) ==
			std::vector<std::pair<Ticks, int>>{{0, 0}, {2 * g_quarter, 127}, {lastTick, 0}, {9 * g_quarter / 2, 0}});
		CHECK(laneOf(part, 7) ==
			std::vector<std::pair<Ticks, int>>{{0, 50}, {2 * g_quarter, 100}, {lastTick, 50}, {9 * g_quarter / 2, 50}});
	}
	SECTION("reverse in a part shortened to end before its last events")
	{
		MidiPart part = PartBuilder()
							.note(1, 0, g_quarter, 60)
							.note(2, 2 * g_quarter, g_quarter, 62)
							.cc(3, 0, 64, 127)
							.cc(4, 3 * g_quarter, 64, 0) // past the part's end
							.part;
		part.length = 5 * g_quarter / 2;
		reverse(part, NoteSelection(), part.length, NewIds().function());
		// The starts span 0 to 3 quarters; the pedal is released on the part's last tick.
		CHECK(laneOf(part, 64) == std::vector<std::pair<Ticks, int>>{{0, 127}, {part.length - 1, 0}});
	}
	SECTION("invert in a part that ends before the range")
	{
		MidiPart part = original;
		for (auto& [id, note] : part.notes)
			note.position += g_quarter;
		for (auto& [id, controller] : part.controllers)
			controller.position += g_quarter;
		part.length = g_quarter / 2; // ends before the first event
		invert(part, NoteSelection(), part.length, NewIds().function());
		// The range is 1 to 5 quarters: the lane ends at its value where the range starts, not before it, so it needs
		// no event to return to it.
		CHECK(laneOf(part, 64) == std::vector<std::pair<Ticks, int>>{{g_quarter, 0}, {3 * g_quarter, 127}});
	}
}

TEST_CASE("MIDI functions leave empty parts alone", "[unit]")
{
	const PartBuilder builder;
	const MidiPart empty = builder.part;
	MidiPart part = empty;
	TransposeSettings settings;
	settings.semitones = 3;
	transpose(part, NoteSelection(), settings);
	legato(part, NoteSelection(), 0, true);
	fixLengths(part, NoteSelection(), 10);
	fixVelocities(part, NoteSelection(), 10);
	pedalToNoteLength(part, NoteSelection(), part.length);
	deleteOverlaps(part, NoteSelection(), OverlapMode::Poly);
	deleteDoubles(part, NoteSelection());
	deleteControllers(part);
	deleteContinuousControllers(part);
	deleteNotes(part, NoteSelection(), DeleteNotesSettings{10, 10, false});
	restrictPolyphony(part, NoteSelection(), 1);
	thinOut(part, 100);
	NewIds ids;
	reverse(part, NoteSelection(), part.length, ids.function());
	invert(part, NoteSelection(), part.length, ids.function());
	CHECK(part.notes.empty());
	CHECK(part.controllers.empty());
	CHECK(sameProperties(part, empty));
}

TEST_CASE("merge copies the events in a range into a new part", "[unit]")
{
	Project project = newProject();
	const ObjectId piano = addTrack(project, TrackType::Instrument, "Piano");
	const ObjectId strings = addTrack(project, TrackType::Midi, "Strings");
	project.channels.at(project.trackById.at(strings).channel).output = project.trackById.at(piano).channel;
	addPart(project, piano, 0, 8 * g_quarter,
		PartBuilder()
			.note(1, 0, g_quarter, 60)                 // before the range
			.note(2, 2 * g_quarter, 8 * g_quarter, 62) // in it, cut at its part's end
			.cc(3, g_quarter, 7, 90)                   // before the range: chased
			.cc(4, g_quarter / 2, 7, 80)
			.controller(5, 0, ControllerType::Program, 9, g_noBank)
			.part);
	addPart(project, strings, 4 * g_quarter, 4 * g_quarter,
		PartBuilder().note(1, 0, g_quarter, 72).note(2, 4 * g_quarter, g_quarter, 74).part); // 2 is past its part

	MergeSettings settings;
	const double rate = project.sampleRate;
	settings.start = project.tempo.toSamples(2 * g_quarter, rate);
	settings.end = project.tempo.toSamples(10 * g_quarter, rate);
	settings.includeChase = true;
	const PartChanges changes = merge(project, settings);

	REQUIRE(changes.tracks.size() == 1);
	const Track& merged = project.trackById.at(changes.tracks[0]);
	CHECK(merged.type == TrackType::Midi);
	CHECK(project.tracks.back() == merged.id);
	CHECK(project.channels.at(merged.channel).output == project.trackById.at(piano).channel);
	REQUIRE(changes.addedParts.size() == 1);
	const MidiPart& part = project.midiParts.at(changes.addedParts[0]);
	CHECK(part.track == merged.id);
	CHECK(part.position.value == 2 * g_quarter);
	CHECK(part.length == 8 * g_quarter);
	CHECK(notesOf(part) ==
		std::vector<std::vector<int64_t>>{{0, 62, 6 * g_quarter, 0}, {2 * g_quarter, 72, g_quarter, 0}});
	// The chased controller and program, at the start.
	std::vector<std::pair<int, int>> chased;
	for (const auto& [id, controller] : part.controllers)
	{
		CHECK(controller.position == 0);
		chased.emplace_back(static_cast<int>(controller.type), controller.number);
	}
	std::sort(chased.begin(), chased.end());
	CHECK(chased ==
		std::vector<std::pair<int, int>>{
			{static_cast<int>(ControllerType::Control), 7}, {static_cast<int>(ControllerType::Program), 9}});
	for (const auto& [id, controller] : part.controllers)
		if (controller.type == ControllerType::Control)
			CHECK(controllerToMidi1(controller.value) == 90);

	SECTION("into a track, erasing what is there")
	{
		Project into = project;
		const ObjectId target = addTrack(into, TrackType::Midi, "Target");
		const ObjectId inside = addPart(into, target, 3 * g_quarter, g_quarter, PartBuilder().note(1, 0, 10, 50).part);
		const ObjectId across = addPart(
			into, target, 0, 4 * g_quarter, PartBuilder().note(1, 0, 10, 51).note(2, 3 * g_quarter, 10, 52).part);
		MergeSettings erase = settings;
		erase.tracks = {strings};
		erase.destination = target;
		erase.eraseDestination = true;
		erase.includeChase = false;
		const PartChanges result = merge(into, erase);
		CHECK(result.tracks.empty());
		CHECK(result.removedParts == std::vector<ObjectId>{inside});
		CHECK(result.changedParts == std::vector<ObjectId>{across});
		CHECK(notesOf(into.midiParts.at(across)) == std::vector<std::vector<int64_t>>{{0, 51, 10, 0}});
		REQUIRE(result.addedParts.size() == 1);
		CHECK(notesOf(into.midiParts.at(result.addedParts[0])) ==
			std::vector<std::vector<int64_t>>{{2 * g_quarter, 72, g_quarter, 0}});
	}
}

TEST_CASE("merge compares positions with the locators in samples", "[unit]")
{
	Project project = newProject();
	project.sampleRate = 44100.0;
	project.tempo = gnubase::time::TempoMap(130.0);
	const ObjectId track = addTrack(project, TrackType::Instrument, "Keys");
	const ObjectId source = addPart(project, track, 0, 32 * g_quarter,
		PartBuilder()
			.note(1, 7 * g_quarter, 10, 59)
			.note(2, 8 * g_quarter, 10, 60)  // on bar 3, the left locator
			.note(3, 16 * g_quarter, 10, 62) // on bar 5, the right locator
			.part);
	MergeSettings settings;
	settings.start = project.tempo.toSamples(8 * g_quarter, project.sampleRate);
	settings.end = project.tempo.toSamples(16 * g_quarter, project.sampleRate);
	// Converted back to ticks, the left locator lies after the downbeat of bar 3.
	REQUIRE(project.tempo.toTicks(settings.start, project.sampleRate) > 8 * g_quarter);
	settings.tracks = {track};
	settings.destination = track;
	settings.eraseDestination = true;
	const PartChanges changes = merge(project, settings);
	REQUIRE(changes.addedParts.size() == 1);
	const MidiPart& merged = project.midiParts.at(changes.addedParts[0]);
	CHECK(merged.position.value == 8 * g_quarter);
	CHECK(notesOf(merged) == std::vector<std::vector<int64_t>>{{0, 60, 10, 0}});
	CHECK(notesOf(project.midiParts.at(source)) ==
		std::vector<std::vector<int64_t>>{{7 * g_quarter, 59, 10, 0}, {16 * g_quarter, 62, 10, 0}});
}

TEST_CASE("merge and bounce keep the events of parts on linear tracks", "[unit]")
{
	Project project = newProject();
	const ObjectId track = addTrack(project, TrackType::Instrument, "Keys");
	project.trackById.at(track).timeBase = TimeBase::Linear;
	const auto samples = [&project](Ticks tick) { return project.tempo.toSamples(tick, project.sampleRate); };
	// Parts of 2 bars and 1 bar, in samples: far fewer than the ticks of the events in them.
	const ObjectId first = addPart(project, track, 0, samples(8 * g_quarter),
		PartBuilder()
			.note(1, 0, g_quarter, 60)
			.note(2, 6 * g_quarter, 4 * g_quarter, 62) // cut at the part's end
			.note(3, 9 * g_quarter, g_quarter, 63)     // after the part's end
			.cc(4, 7 * g_quarter, 1, 5)
			.part);
	const ObjectId second = addPart(project, track, samples(12 * g_quarter), samples(4 * g_quarter),
		PartBuilder().note(1, 3 * g_quarter, g_quarter, 64).part);
	const std::vector<std::vector<int64_t>> expected = {
		{0, 60, g_quarter, 0}, {6 * g_quarter, 62, 2 * g_quarter, 0}, {15 * g_quarter, 64, g_quarter, 0}};

	SECTION("bounce")
	{
		const PartChanges changes = bounce(project, {first, second});
		REQUIRE(changes.addedParts.size() == 1);
		const MidiPart& part = project.midiParts.at(changes.addedParts[0]);
		CHECK(part.position == gnubase::time::Position{TimeBase::Linear, 0});
		CHECK(part.length == samples(16 * g_quarter));
		CHECK(notesOf(part) == expected);
		REQUIRE(part.controllers.size() == 1);
		CHECK(part.controllers.begin()->second.position == 7 * g_quarter);
	}
	SECTION("merge")
	{
		MergeSettings settings;
		settings.end = samples(16 * g_quarter);
		const PartChanges changes = merge(project, settings);
		REQUIRE(changes.addedParts.size() == 1);
		const MidiPart& part = project.midiParts.at(changes.addedParts[0]);
		CHECK(part.length == 16 * g_quarter);
		CHECK(notesOf(part) == expected);
		CHECK(part.controllers.size() == 1);
	}
}

TEST_CASE("dissolve copies events into new tracks by channel or by pitch and mutes the parts", "[unit]")
{
	Project project = newProject();
	const ObjectId drums = addTrack(project, TrackType::Instrument, "Drums");
	const ObjectId after = addTrack(project, TrackType::Audio, "Audio");
	const ObjectId source = addPart(project, drums, 4 * g_quarter, 16 * g_quarter,
		PartBuilder()
			.note(1, 0, 10, 36)
			.note(2, g_quarter, 10, 38, 1)
			.note(3, 2 * g_quarter, 10, 36, 1)
			.note(4, 9 * g_quarter, 10, 42)
			.controller(5, 0, ControllerType::PolyPressure, 38, 5, 1)
			.cc(6, 0, 7, 100, 1)
			.controller(7, 0, ControllerType::SysEx, 0, 0)
			.part);
	const MidiPart original = project.midiParts.at(source);
	// The part keeps its events, muted (reference manual p. 1122).
	const auto mutedAsItWas = [&]
	{
		const MidiPart& part = project.midiParts.at(source);
		CHECK(part.mute);
		CHECK(part.notes == original.notes);
		CHECK(part.controllers == original.controllers);
	};

	SECTION("by channel")
	{
		const PartChanges changes = dissolve(project, {source}, DissolveBy::Channel, false);
		REQUIRE(changes.tracks.size() == 2);
		CHECK(project.tracks == std::vector<ObjectId>{drums, changes.tracks[0], changes.tracks[1], after});
		CHECK(project.trackById.at(changes.tracks[0]).name == "Drums Ch 1");
		CHECK(project.trackById.at(changes.tracks[1]).name == "Drums Ch 2");
		for (const ObjectId track : changes.tracks)
		{
			CHECK(project.trackById.at(track).type == TrackType::Midi);
			CHECK(
				project.channels.at(project.trackById.at(track).channel).output == project.trackById.at(drums).channel);
		}
		const MidiPart& first = onlyPartOn(project, changes.tracks[0]);
		CHECK(first.position.value == 4 * g_quarter);
		CHECK(first.length == 16 * g_quarter);
		CHECK(!first.mute);
		CHECK(notesOf(first) == std::vector<std::vector<int64_t>>{{0, 36, 10, 0}, {9 * g_quarter, 42, 10, 0}});
		// The SysEx, of no channel, goes with the first new part.
		CHECK(controllerTypes(first) == std::vector<ControllerType>{ControllerType::SysEx});
		const MidiPart& second = onlyPartOn(project, changes.tracks[1]);
		CHECK(notesOf(second) == std::vector<std::vector<int64_t>>{{g_quarter, 38, 10, 1}, {2 * g_quarter, 36, 10, 1}});
		CHECK(second.controllers.size() == 2);
		CHECK(changes.changedParts == std::vector<ObjectId>{source});
		CHECK(changes.removedParts.empty());
		mutedAsItWas();
		// The copies have ids of their own.
		for (const auto& [id, note] : first.notes)
			CHECK(original.notes.count(id) == 0);
	}
	SECTION("the new tracks take the track's MIDI modifiers, so their notes play as the part's did")
	{
		Modifiers modifiers;
		modifiers.transpose = 3;
		modifiers.velocityShift = -5;
		modifiers.lengthCompression = {3, 2};
		modifiers.random[0] = {RandomTarget::Velocity, -4, 4};
		modifiers.range[1] = {RangeTarget::NoteFilter, 30, 90};
		project.channels.at(project.trackById.at(drums).channel).modifiers = modifiers;
		const PartChanges changes = dissolve(project, {source}, DissolveBy::Channel, false);
		REQUIRE(changes.tracks.size() == 2);
		for (const ObjectId track : changes.tracks)
			CHECK(project.channels.at(project.trackById.at(track).channel).modifiers == modifiers);
	}
	SECTION("by pitch, with the parts shortened to their bars")
	{
		const PartChanges changes = dissolve(project, {source}, DissolveBy::Pitch, true);
		REQUIRE(changes.tracks.size() == 3);
		CHECK(project.trackById.at(changes.tracks[0]).name == "Drums C1"); // 36, with middle C (60) as C3
		CHECK(project.trackById.at(changes.tracks[1]).name == "Drums D1");
		CHECK(project.trackById.at(changes.tracks[2]).name == "Drums F#1");
		const MidiPart& kick = onlyPartOn(project, changes.tracks[0]);
		CHECK(kick.position.value == 4 * g_quarter); // bar 2
		CHECK(kick.length == 4 * g_quarter);
		CHECK(notesOf(kick) == std::vector<std::vector<int64_t>>{{0, 36, 10, 0}, {2 * g_quarter, 36, 10, 1}});
		// The controller and the SysEx belong to no pitch.
		CHECK(controllerTypes(kick) == std::vector<ControllerType>{ControllerType::Control, ControllerType::SysEx});
		const MidiPart& snare = onlyPartOn(project, changes.tracks[1]);
		CHECK(controllerTypes(snare) == std::vector<ControllerType>{ControllerType::PolyPressure});
		const MidiPart& hat = onlyPartOn(project, changes.tracks[2]);
		CHECK(hat.position.value == 12 * g_quarter); // bar 4
		CHECK(hat.length == 4 * g_quarter);
		CHECK(notesOf(hat) == std::vector<std::vector<int64_t>>{{g_quarter, 42, 10, 0}});
		mutedAsItWas();
	}
	SECTION("to lanes of the track, below the lanes its parts are on")
	{
		project.midiParts.at(source).color = "#ff0000";
		project.midiParts.at(source).name = "Beat";
		const ObjectId take = addPart(project, drums, 0, g_quarter);
		project.midiParts.at(take).lane = 1;
		const PartChanges changes = dissolve(project, {source}, DissolveBy::Pitch, false, DissolveTo::Lanes);
		CHECK(changes.tracks.empty());
		CHECK(project.tracks == std::vector<ObjectId>{drums, after});
		REQUIRE(changes.addedParts.size() == 3);
		const std::vector<std::string> names = {"Beat C1", "Beat D1", "Beat F#1"};
		for (size_t i = 0; i < 3; ++i)
		{
			const MidiPart& piece = project.midiParts.at(changes.addedParts[i]);
			CHECK(piece.track == drums);
			CHECK(piece.lane == static_cast<int>(i) + 2);
			CHECK(piece.name == names[i]);
			CHECK(piece.color == "#ff0000");
			CHECK(piece.position.value == 4 * g_quarter);
			CHECK(piece.length == 16 * g_quarter);
		}
		CHECK(notesOf(project.midiParts.at(changes.addedParts[2])) ==
			std::vector<std::vector<int64_t>>{{9 * g_quarter, 42, 10, 0}});
		mutedAsItWas();
	}
	SECTION("new parts keep a muted part's mute, transpose and velocity")
	{
		MidiPart& part = project.midiParts.at(source);
		part.mute = true;
		part.transpose = 3;
		part.velocity = -10;
		const PartChanges changes = dissolve(project, {source}, DissolveBy::Channel, false);
		CHECK(changes.changedParts.empty());
		REQUIRE(changes.addedParts.size() == 2);
		for (const ObjectId id : changes.addedParts)
		{
			const MidiPart& piece = project.midiParts.at(id);
			CHECK(piece.mute);
			CHECK(piece.transpose == 3);
			CHECK(piece.velocity == -10);
		}
	}
	SECTION("a part with nothing to separate stays as it is")
	{
		const ObjectId empty = addPart(project, drums, 0, g_quarter);
		const ObjectId sysex =
			addPart(project, drums, 0, g_quarter, PartBuilder().controller(1, 0, ControllerType::SysEx, 0, 0).part);
		const PartChanges changes = dissolve(project, {empty, sysex}, DissolveBy::Channel, false);
		CHECK(changes.tracks.empty());
		CHECK(changes.changedParts.empty());
		CHECK(!project.midiParts.at(empty).mute);
		CHECK(!project.midiParts.at(sysex).mute);
	}
}

TEST_CASE("bounce combines parts on a track into one", "[unit]")
{
	Project project = newProject();
	const ObjectId track = addTrack(project, TrackType::Instrument, "Keys");
	const ObjectId first = addPart(project, track, 4 * g_quarter, 2 * g_quarter,
		PartBuilder().note(1, 0, 4 * g_quarter, 60).note(2, 3 * g_quarter, 10, 61).cc(3, g_quarter, 1, 5).part);
	const ObjectId second = addPart(project, track, g_quarter, g_quarter, PartBuilder().note(1, 0, 10, 64).part);
	const ObjectId other = addPart(project, track, 20 * g_quarter, g_quarter);
	const PartChanges changes = bounce(project, {first, second});
	CHECK(changes.removedParts == std::vector<ObjectId>{first, second});
	REQUIRE(changes.addedParts.size() == 1);
	const MidiPart& part = project.midiParts.at(changes.addedParts[0]);
	CHECK(part.track == track);
	CHECK(part.name == "Keys");
	CHECK(part.position.value == g_quarter);
	CHECK(part.length == 5 * g_quarter);
	// Note 1 is cut at its part's end; note 2 started after it and is gone.
	CHECK(notesOf(part) == std::vector<std::vector<int64_t>>{{0, 64, 10, 0}, {3 * g_quarter, 60, 2 * g_quarter, 0}});
	REQUIRE(part.controllers.size() == 1);
	CHECK(part.controllers.begin()->second.position == 4 * g_quarter);
	CHECK(project.midiParts.count(other) == 1);
}

TEST_CASE("bounce removes muted parts and plays the parts' transpose and velocity into the notes", "[unit]")
{
	Project project = newProject();
	const ObjectId track = addTrack(project, TrackType::Instrument, "Keys");
	const uint16_t velocity = velocityFromMidi1(100);
	const ObjectId up = addPart(project, track, 0, g_quarter,
		PartBuilder().note(1, 0, 10, 60, 0, velocity).note(2, 10, 10, 120, 0, velocity).part);
	project.midiParts.at(up).transpose = 12; // 120 would go to 132, which doesn't play
	project.midiParts.at(up).velocity = 40;  // 100 plays at 127
	project.midiParts.at(up).lane = 2;
	const ObjectId down = addPart(
		project, track, 2 * g_quarter, g_quarter, PartBuilder().note(1, 0, 10, 64, 0, velocityFromMidi1(30)).part);
	project.midiParts.at(down).velocity = -50; // a note-on plays at 1 at least
	project.midiParts.at(down).lane = 1;
	const ObjectId muted =
		addPart(project, track, 8 * g_quarter, g_quarter, PartBuilder().note(1, 0, 10, 50, 0, velocity).part);
	project.midiParts.at(muted).mute = true;

	const PartChanges changes = bounce(project, {up, down, muted});
	CHECK(changes.removedParts == std::vector<ObjectId>{up, down, muted});
	REQUIRE(changes.addedParts.size() == 1);
	const MidiPart& part = project.midiParts.at(changes.addedParts[0]);
	// The muted part adds neither notes nor length.
	CHECK(part.length == 3 * g_quarter);
	CHECK(part.lane == 1);
	CHECK(part.transpose == 0);
	CHECK(part.velocity == 0);
	CHECK(notesOf(part) == std::vector<std::vector<int64_t>>{{0, 72, 10, 0}, {2 * g_quarter, 64, 10, 0}});
	std::vector<int> velocities;
	velocities.reserve(part.notes.size());
	for (const auto& [id, note] : part.notes)
		velocities.push_back(velocityToMidi1(note.velocity));
	std::sort(velocities.begin(), velocities.end());
	CHECK(velocities == std::vector<int>{1, 127});

	SECTION("only muted parts are removed")
	{
		Project other = newProject();
		const ObjectId keys = addTrack(other, TrackType::Instrument, "Keys");
		const ObjectId silent = addPart(other, keys, 0, g_quarter, PartBuilder().note(1, 0, 10, 60).part);
		other.midiParts.at(silent).mute = true;
		const PartChanges result = bounce(other, {silent});
		CHECK(result.removedParts == std::vector<ObjectId>{silent});
		CHECK(result.addedParts.empty());
		CHECK(other.midiParts.empty());
	}
}

TEST_CASE("merge plays the parts' transpose and velocity and, with includeInserts, the MIDI modifiers into the notes",
	"[unit]")
{
	Project project = newProject();
	const ObjectId piano = addTrack(project, TrackType::Instrument, "Piano");
	const ObjectId part = addPart(project, piano, 0, g_quarter,
		PartBuilder()
			.note(1, 0, 10, 60, 0, velocityFromMidi1(64))
			.note(2, 0, 10, 70, 0, velocityFromMidi1(64))
			.note(3, 20, 10, 126, 0, velocityFromMidi1(64)) // transposed beyond 127 by the part: doesn't play
			.part);
	project.midiParts.at(part).transpose = 2;
	project.midiParts.at(part).velocity = 10;
	Modifiers& modifiers = project.channels.at(project.trackById.at(piano).channel).modifiers;
	modifiers.transpose = 5;
	modifiers.lengthCompression = {2, 1};
	modifiers.random[0] = {RandomTarget::Position, g_quarter / 4, g_quarter / 4};
	modifiers.range[0] = {RangeTarget::NoteFilter, 0, 70};
	MergeSettings settings;
	settings.end = project.tempo.toSamples(4 * g_quarter, project.sampleRate);
	settings.tracks = {piano};
	const auto merged = [&project](const PartChanges& changes) -> const MidiPart&
	{
		REQUIRE(changes.addedParts.size() == 1);
		const MidiPart& result = project.midiParts.at(changes.addedParts[0]);
		CHECK(result.transpose == 0);
		CHECK(result.velocity == 0);
		for (const auto& [id, note] : result.notes)
			CHECK(velocityToMidi1(note.velocity) == 74);
		return result;
	};

	SECTION("without includeInserts, the parts' transpose and velocity")
	{
		CHECK(notesOf(merged(merge(project, settings))) ==
			std::vector<std::vector<int64_t>>{{0, 62, 10, 0}, {0, 72, 10, 0}});
	}
	SECTION("with includeInserts, also the track's MIDI modifiers, as playback applies them")
	{
		settings.includeInserts = true;
		// 60 + 2 + 5 plays 1/16 later and twice as long; 70 + 7 is outside the note filter.
		CHECK(
			notesOf(merged(merge(project, settings))) == std::vector<std::vector<int64_t>>{{g_quarter / 4, 67, 20, 0}});
	}
	SECTION("bypassed modifiers don't count")
	{
		settings.includeInserts = true;
		modifiers.bypass = true;
		CHECK(notesOf(merged(merge(project, settings))) ==
			std::vector<std::vector<int64_t>>{{0, 62, 10, 0}, {0, 72, 10, 0}});
	}
}

TEST_CASE("merge with includeInserts starts a note that a random position moves before its part at the part's start",
	"[unit]")
{
	Project project = newProject();
	const ObjectId piano = addTrack(project, TrackType::Instrument, "Piano");
	addPart(
		project, piano, 4 * g_quarter, 4 * g_quarter, PartBuilder().note(1, 0, 10, 60).note(2, g_quarter, 10, 62).part);
	project.channels.at(project.trackById.at(piano).channel).modifiers.random[0] = {
		RandomTarget::Position, -g_quarter / 4, -g_quarter / 4};
	MergeSettings settings;
	settings.start = project.tempo.toSamples(4 * g_quarter, project.sampleRate); // the left locator on the part's start
	settings.end = project.tempo.toSamples(8 * g_quarter, project.sampleRate);
	settings.tracks = {piano};
	settings.includeInserts = true;
	const PartChanges changes = merge(project, settings);
	REQUIRE(changes.addedParts.size() == 1);
	const MidiPart& part = project.midiParts.at(changes.addedParts[0]);
	CHECK(part.position.value == 4 * g_quarter);
	// As playback plays them: the first stays at the part's start with its length, the other moves.
	CHECK(notesOf(part) == std::vector<std::vector<int64_t>>{{0, 60, 10, 0}, {g_quarter - (g_quarter / 4), 62, 10, 0}});
}

TEST_CASE("merge leaves out muted parts and, by default, the tracks that don't play", "[unit]")
{
	Project project = newProject();
	const ObjectId piano = addTrack(project, TrackType::Instrument, "Piano");
	const ObjectId organ = addTrack(project, TrackType::Instrument, "Organ");
	const ObjectId strings = addTrack(project, TrackType::Midi, "Strings");
	const ObjectId vocals = addTrack(project, TrackType::Audio, "Vocals");
	const ObjectId group = addTrack(project, TrackType::Group, "Group");
	const auto channel = [&project](ObjectId track) -> Channel&
	{ return project.channels.at(project.trackById.at(track).channel); };
	channel(strings).output = channel(organ).id;
	addPart(project, piano, 0, g_quarter, PartBuilder().note(1, 0, 10, 60).part);
	addPart(project, organ, 0, g_quarter, PartBuilder().note(1, 0, 10, 62).part);
	addPart(project, strings, 0, g_quarter, PartBuilder().note(1, 0, 10, 64).part);
	const ObjectId muted = addPart(project, piano, 0, g_quarter, PartBuilder().note(1, 0, 10, 65).part);
	project.midiParts.at(muted).mute = true;

	CHECK(playingTracks(project) == std::vector<ObjectId>{piano, organ, strings});
	SECTION("a muted part is left out")
	{
		MergeSettings settings;
		settings.end = project.tempo.toSamples(4 * g_quarter, project.sampleRate);
		const PartChanges changes = merge(project, settings);
		REQUIRE(changes.addedParts.size() == 1);
		CHECK(notesOf(project.midiParts.at(changes.addedParts[0])) ==
			std::vector<std::vector<int64_t>>{{0, 60, 10, 0}, {0, 62, 10, 0}, {0, 64, 10, 0}});
	}
	SECTION("muted channels")
	{
		channel(piano).mute = true;
		CHECK(playingTracks(project) == std::vector<ObjectId>{organ, strings});
		// A MIDI track plays through its instrument's channel.
		channel(organ).mute = true;
		CHECK(playingTracks(project).empty());
	}
	SECTION("solo")
	{
		channel(organ).solo = true;
		// The MIDI track that plays into the soloed channel is heard with it.
		CHECK(playingTracks(project) == std::vector<ObjectId>{organ, strings});
		// Soloed and muted, it doesn't play.
		channel(organ).mute = true;
		CHECK(playingTracks(project).empty());
		channel(organ).mute = false;
		channel(organ).solo = false;
		channel(vocals).solo = true;
		CHECK(playingTracks(project).empty());
	}
	SECTION("a soloed MIDI track and the instrument it plays through")
	{
		channel(strings).solo = true;
		CHECK(playingTracks(project) == std::vector<ObjectId>{organ, strings});
	}
	SECTION("a muted group channel that a track plays into")
	{
		channel(piano).output = channel(group).id;
		channel(group).mute = true;
		CHECK(playingTracks(project) == std::vector<ObjectId>{organ, strings});
	}
	SECTION("a disabled track")
	{
		project.trackById.at(organ).enabled = false;
		CHECK(playingTracks(project) == std::vector<ObjectId>{piano});
	}
}

TEST_CASE("velocity adds, compresses, limits, fixes and varies the velocities of the selected notes", "[unit]")
{
	const auto run = [](const VelocitySettings& settings, const NoteSelection& selection = {})
	{
		PartBuilder builder;
		builder.note(1, 0, 10, 60, 0, 6553).note(2, 10, 10, 62, 0, 32768).note(3, 20, 10, 64, 0, 58982);
		builder.part.id = 7;
		changeVelocities(builder.part, selection, settings);
		std::vector<double> velocities;
		velocities.reserve(builder.part.notes.size());
		for (const auto& [id, note] : builder.part.notes)
			velocities.push_back(std::round(note.velocity / 655.35) / 100.0); // to two decimals
		return velocities;
	};
	VelocitySettings settings;
	settings.amount = 0.2;
	CHECK(run(settings) == std::vector<double>{0.3, 0.7, 1.0});
	settings.amount = -0.2;
	CHECK(run(settings) == std::vector<double>{0.0, 0.3, 0.7});
	CHECK(run(settings, NoteSelection({2})) == std::vector<double>{0.1, 0.3, 0.9});

	settings.type = VelocityChange::Compress;
	settings.ratio = 0.5;
	CHECK(run(settings) == std::vector<double>{0.05, 0.25, 0.45});
	settings.ratio = 2.0;
	CHECK(run(settings) == std::vector<double>{0.2, 1.0, 1.0});

	settings.type = VelocityChange::Limit;
	settings.lower = 0.2;
	settings.upper = 0.6;
	CHECK(run(settings) == std::vector<double>{0.2, 0.5, 0.6});

	settings.type = VelocityChange::Fixed;
	settings.amount = 0.4;
	CHECK(run(settings) == std::vector<double>{0.4, 0.4, 0.4});

	// Variance lowers each note by up to its part, the same each time.
	settings.type = VelocityChange::Variance;
	settings.variance = 0.0;
	CHECK(run(settings) == std::vector<double>{0.1, 0.5, 0.9});
	settings.variance = 0.5;
	const std::vector<double> varied = run(settings);
	CHECK(run(settings) == varied);
	const std::vector<double> before = {0.1, 0.5, 0.9};
	bool lowered = false;
	for (size_t i = 0; i < 3; ++i)
	{
		CHECK(varied[i] <= before[i]);
		CHECK(varied[i] >= (before[i] * 0.5) - 0.01);
		lowered = lowered || varied[i] < before[i];
	}
	CHECK(lowered);
}

TEST_CASE("extract MIDI automation moves continuous controllers into automation lanes", "[unit]")
{
	Project project = newProject();
	const ObjectId track = addTrack(project, TrackType::Instrument, "Keys");
	const ObjectId part = addPart(project, track, 4 * g_quarter, 4 * g_quarter,
		PartBuilder()
			.note(1, 0, 10, 60)
			.cc(2, g_quarter, 7, 127)
			.cc(3, 0, 7, 0)
			.cc(4, 0, 1, 64, 2)                                // modulation on channel 3
			.cc(5, 0, 64, 127)                                 // the sustain pedal, a switch, stays
			.cc(6, 8 * g_quarter, 7, 50)                       // past the part's end, doesn't play: stays
			.controller(7, 0, ControllerType::PitchBend, 0, 5) // stays
			.part);
	const ObjectId muted = addPart(project, track, 16 * g_quarter, g_quarter, PartBuilder().cc(1, 0, 7, 10).part);
	project.midiParts.at(muted).mute = true;
	// A lane the track has for CC 7 on channel 1 keeps its points outside the part.
	AutomationLane existing;
	existing.id = project.newId();
	existing.track = track;
	existing.target.type = AutomationTargetType::Controller;
	existing.target.number = 7;
	for (const Ticks at : {Ticks{0}, 5 * g_quarter, 8 * g_quarter})
	{
		AutomationPoint point;
		point.id = project.newId();
		point.position = {TimeBase::Musical, at};
		point.value = 0.25;
		point.shape = AutomationShape::Step;
		existing.points[point.id] = point;
	}
	existing.read = false; // the extracted controllers have to play
	project.automation[existing.id] = existing;

	const AutomationChanges changes = extractAutomation(project, {part, muted});
	CHECK(changes.changedParts == std::vector<ObjectId>{part});
	CHECK(changes.changedLanes == std::vector<ObjectId>{existing.id});
	REQUIRE(changes.addedLanes.size() == 1);

	// (position, value) of a lane's points, in order.
	const auto pointsOf = [&project](ObjectId lane)
	{
		std::vector<std::pair<int64_t, double>> points;
		for (const auto& [id, point] : project.automation.at(lane).points)
		{
			CHECK(point.shape == AutomationShape::Step);
			points.emplace_back(point.position.value, std::round(point.value * 1000.0) / 1000.0);
		}
		std::sort(points.begin(), points.end());
		return points;
	};
	CHECK(pointsOf(existing.id) ==
		std::vector<std::pair<int64_t, double>>{
			{0, 0.25}, {4 * g_quarter, 0.0}, {5 * g_quarter, 1.0}, {8 * g_quarter, 0.25}});
	CHECK(project.automation.at(existing.id).read);
	const AutomationLane& modulation = project.automation.at(changes.addedLanes[0]);
	CHECK(modulation.track == track);
	CHECK(modulation.target.type == AutomationTargetType::Controller);
	CHECK(modulation.target.number == 1);
	CHECK(modulation.target.channel == 2);
	CHECK(modulation.read);
	CHECK(pointsOf(modulation.id) == std::vector<std::pair<int64_t, double>>{{4 * g_quarter, 0.5}});

	CHECK(controllerIds(project.midiParts.at(part)).size() == 3);
	CHECK(laneOf(project.midiParts.at(part), 7) == std::vector<std::pair<Ticks, int>>{{8 * g_quarter, 50}});
	CHECK(laneOf(project.midiParts.at(part), 64).size() == 1);
	CHECK(project.midiParts.at(muted).controllers.size() == 1);

	SECTION("on a linear track, the points are in samples")
	{
		Project linear = newProject();
		const ObjectId keys = addTrack(linear, TrackType::Instrument, "Keys");
		linear.trackById.at(keys).timeBase = TimeBase::Linear;
		const auto second = static_cast<Samples>(linear.sampleRate);
		const ObjectId onLinear = addPart(linear, keys, second, second, PartBuilder().cc(1, g_quarter, 7, 127).part);
		const AutomationChanges result = extractAutomation(linear, {onLinear});
		REQUIRE(result.addedLanes.size() == 1);
		const AutomationPoint& point = linear.automation.at(result.addedLanes[0]).points.begin()->second;
		CHECK(point.position.base == TimeBase::Linear);
		CHECK(point.position.value == second + linear.tempo.toSamples(g_quarter, linear.sampleRate));
	}

	SECTION("overlapping parts keep each other's points and clear only the lane's earlier ones")
	{
		Project overlap = newProject();
		const ObjectId keys = addTrack(overlap, TrackType::Instrument, "Keys");
		const ObjectId first =
			addPart(overlap, keys, 0, 4 * g_quarter, PartBuilder().cc(1, 2 * g_quarter, 7, 127).part);
		const ObjectId second =
			addPart(overlap, keys, 2 * g_quarter, 4 * g_quarter, PartBuilder().cc(1, g_quarter, 7, 0).part);
		AutomationLane lane;
		lane.id = overlap.newId();
		lane.track = keys;
		lane.target.type = AutomationTargetType::Controller;
		lane.target.number = 7;
		// The first point lies in the second part's range, the other after it.
		for (const Ticks at : {5 * g_quarter, 7 * g_quarter})
		{
			AutomationPoint point;
			point.id = overlap.newId();
			point.position = {TimeBase::Musical, at};
			point.value = 0.25;
			point.shape = AutomationShape::Step;
			lane.points[point.id] = point;
		}
		overlap.automation[lane.id] = lane;

		const AutomationChanges result = extractAutomation(overlap, {first, second});
		CHECK(result.changedLanes == std::vector<ObjectId>{lane.id});
		CHECK(result.addedLanes.empty());
		std::vector<std::pair<int64_t, double>> points;
		for (const auto& [id, point] : overlap.automation.at(lane.id).points)
			points.emplace_back(point.position.value, std::round(point.value * 1000.0) / 1000.0);
		std::sort(points.begin(), points.end());
		CHECK(points ==
			std::vector<std::pair<int64_t, double>>{{2 * g_quarter, 1.0}, {3 * g_quarter, 0.0}, {7 * g_quarter, 0.25}});
	}
}

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

	struct Fixture
	{
		Engine engine;
		Client client{engine};
		ObjectId track = 0;
		ObjectId part = 0;
		std::vector<ObjectId> notes;

		Fixture()
		{
			track = run(client, "track.add", {{"type", "instrument"}, {"name", "Piano"}})
						.at("track")
						.at("id")
						.get<ObjectId>();
			part = run(client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", 8 * g_quarter}})
					   .at("part")
					   .at("id")
					   .get<ObjectId>();
			const Json added = run(client, "note.add",
				{{"part", part},
					{"notes",
						{{{"position", 0}, {"length", 2 * g_quarter}, {"pitch", 60}, {"velocity", 0.5}},
							{{"position", g_quarter}, {"length", 10}, {"pitch", 64}, {"velocity", 0.5}},
							{{"position", g_quarter}, {"length", 10}, {"pitch", 64}, {"velocity", 0.5}}}}})
								   .at("notes");
			for (const Json& note : added)
				notes.push_back(note.at("id").get<ObjectId>());
			// The sustain pedal, down and up, and a dense volume curve.
			run(client, "controller.add",
				{{"part", part},
					{"controllers",
						{{{"position", 0}, {"number", 64}, {"value", 1.0}},
							{{"position", 2 * g_quarter}, {"number", 64}, {"value", 0.0}},
							{{"position", 0}, {"number", 7}, {"value", 0.1}},
							{{"position", 5}, {"number", 7}, {"value", 0.2}},
							{{"position", 10}, {"number", 7}, {"value", 0.3}}}}});
		}

		Json partJson() { return run(client, "project.get").at("midiParts")[0]; }
	};
}

TEST_CASE("every MIDI function is a command that records an undo step", "[integration]")
{
	Fixture fixture;
	Client& client = fixture.client;
	const Json parts = {fixture.part};
	const std::vector<std::pair<std::string, Json>> commands = {
		{"midi.transpose",
			{{"parts", parts}, {"semitones", 2}, {"fromScale", {{"root", 0}, {"type", "major"}}},
				{"toScale", {{"root", 0}, {"type", "minor"}}}, {"lowest", 0}, {"highest", 127}}},
		{"midi.legato", {{"parts", parts}, {"overlap", -5}}},
		{"midi.fixLengths", {{"parts", parts}, {"notes", {fixture.notes[0]}}, {"length", 20}}},
		{"midi.fixVelocities", {{"parts", parts}, {"velocity", 1.0}}},
		{"midi.velocity", {{"parts", parts}, {"type", "compress"}, {"ratio", 1.5}}},
		{"midi.pedalToNoteLength", {{"parts", parts}}},
		{"midi.deleteOverlaps", {{"parts", parts}, {"mode", "poly"}}},
		{"midi.deleteDoubles", {{"parts", parts}}},
		{"midi.deleteControllers", {{"parts", parts}}},
		{"midi.deleteContinuousControllers", {{"parts", parts}}},
		{"midi.deleteNotes",
			{{"parts", parts}, {"minimumLength", 15}, {"minimumVelocity", 0.6}, {"removeWhen", "both"}}},
		{"midi.restrictPolyphony", {{"parts", parts}, {"voices", 1}}},
		{"midi.thinOut", {{"parts", parts}, {"minimumDistance", 10}}},
		{"midi.reverse", {{"parts", parts}}},
		{"midi.invert", {{"parts", parts}}},
		{"midi.dissolve", {{"parts", parts}, {"by", "pitch"}}},
		{"midi.dissolve", {{"parts", parts}, {"by", "pitch"}, {"to", "lanes"}}},
		{"midi.bounce", {{"parts", parts}}},
		{"midi.extractAutomation", {{"parts", parts}}},
		{"midi.merge", {{"start", 0}, {"end", 48000}}},
	};
	for (const auto& [command, parameters] : commands)
	{
		INFO(command);
		const Json before = run(client, "project.get");
		run(client, command, parameters);
		const Json after = run(client, "project.get");
		run(client, "history.undo");
		CHECK(run(client, "project.get") == before);
		run(client, "history.redo");
		CHECK(run(client, "project.get") == after);
		run(client, "history.undo");
	}
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Merge MIDI in loop");
}

TEST_CASE("MIDI function commands change parts, announce them and check their parameters", "[integration]")
{
	Fixture fixture;
	Client& client = fixture.client;
	while (client.poll())
	{
	}
	const Json result = run(client, "midi.deleteDoubles", {{"parts", {fixture.part}}});
	CHECK(result.at("parts") == Json{fixture.part});
	CHECK(fixture.partJson().at("notes").size() == 2);
	bool announced = false;
	while (const std::optional<Message> message = client.poll())
		if (const auto* event = std::get_if<Event>(&*message); event != nullptr && event->name == "midiPart.changed")
			announced = event->data.at("part").at("notes").size() == 2;
	CHECK(announced);

	// A function that changes nothing records no step.
	const size_t steps = run(client, "history.get").at("steps").size();
	CHECK(run(client, "midi.deleteDoubles", {{"parts", {fixture.part}}}).at("parts").empty());
	CHECK(run(client, "history.get").at("steps").size() == steps);

	const ObjectId removed = fixture.notes[2];
	CHECK_THAT(
		reject(client, "midi.legato", {{"parts", {fixture.part}}, {"notes", {removed}}}), ContainsSubstring("no note"));
	CHECK_THAT(reject(client, "midi.legato", {{"parts", {12345}}}), ContainsSubstring("no MIDI part 12345"));
	CHECK_THAT(reject(client, "midi.legato", {{"parts", Json::array()}}), ContainsSubstring("parts"));
	CHECK_THAT(reject(client, "midi.fixLengths", {{"parts", {fixture.part}}, {"length", 0}}),
		ContainsSubstring("length must be positive"));
	CHECK_THAT(reject(client, "midi.fixVelocities", {{"parts", {fixture.part}}, {"velocity", 2}}),
		ContainsSubstring("velocity"));
	CHECK_THAT(
		reject(client, "midi.transpose", {{"parts", {fixture.part}}, {"fromScale", {{"root", 0}, {"type", "major"}}}}),
		ContainsSubstring("toScale"));
	CHECK_THAT(reject(client, "midi.transpose", {{"parts", {fixture.part}}, {"lowest", 70}, {"highest", 60}}),
		ContainsSubstring("lowest"));
	CHECK_THAT(reject(client, "midi.restrictPolyphony", {{"parts", {fixture.part}}, {"voices", 0}}),
		ContainsSubstring("voices"));
	CHECK_THAT(reject(client, "midi.deleteNotes", {{"parts", {fixture.part}}}), ContainsSubstring("minimum"));
	CHECK_THAT(reject(client, "midi.velocity", {{"parts", {fixture.part}}, {"type", "add"}}),
		ContainsSubstring("amount is required"));
	CHECK_THAT(reject(client, "midi.velocity", {{"parts", {fixture.part}}, {"type", "compress"}, {"ratio", 4}}),
		ContainsSubstring("ratio must be 0 to 3"));
	CHECK_THAT(
		reject(client, "midi.velocity", {{"parts", {fixture.part}}, {"type", "limit"}, {"lower", 0.6}, {"upper", 0.5}}),
		ContainsSubstring("lower"));
	CHECK_THAT(reject(client, "midi.dissolve",
				   {{"parts", {fixture.part}}, {"by", "pitch"}, {"to", "lanes"}, {"optimize", true}}),
		ContainsSubstring("optimize"));
	CHECK_THAT(reject(client, "midi.merge", {{"start", 100}, {"end", 100}}), ContainsSubstring("end"));
	const ObjectId audio = run(client, "track.add", {{"type", "audio"}}).at("track").at("id").get<ObjectId>();
	CHECK_THAT(reject(client, "midi.merge", {{"start", 0}, {"end", 100}, {"destination", audio}}),
		ContainsSubstring("MIDI or instrument track"));

	// Several functions in one transaction are one step.
	run(client, "history.transaction",
		{{"description", "Clean up"},
			{"commands",
				{{{"command", "midi.fixLengths"}, {"parameters", {{"parts", {fixture.part}}, {"length", 30}}}},
					{{"command", "midi.legato"}, {"parameters", {{"parts", {fixture.part}}}}}}}});
	const Json notes = fixture.partJson().at("notes");
	CHECK(notes[0].at("length") == g_quarter);
	CHECK(notes[1].at("length") == 30);
}

TEST_CASE("merge, dissolve and bounce commands return and announce the parts and tracks they make", "[integration]")
{
	Fixture fixture;
	Client& client = fixture.client;
	const Json dissolved = run(client, "midi.dissolve", {{"parts", {fixture.part}}, {"by", "channel"}});
	CHECK(dissolved.at("tracks").size() == 1);
	CHECK(dissolved.at("parts").size() == 1);
	CHECK(dissolved.at("changedParts") == Json{fixture.part});
	CHECK(dissolved.at("removedParts").empty());
	const Json project = run(client, "project.get");
	CHECK(project.at("tracks").size() == 2);
	CHECK(project.at("tracks")[1].at("name") == "Piano Ch 1");
	CHECK(project.at("midiParts")[0].at("mute") == true);

	// The muted part stays out of the merge; the new track plays through the piano.
	const Json merged =
		run(client, "midi.merge", {{"start", 0}, {"end", 48000}, {"destination", fixture.track}, {"name", "Merged"}});
	CHECK(merged.at("tracks").empty());
	const auto mergedPart = merged.at("parts")[0].get<ObjectId>();
	const Json bounced = run(client, "midi.bounce", {{"parts", {mergedPart}}});
	CHECK(bounced.at("parts").size() == 1);
	CHECK(bounced.at("removedParts") == Json{mergedPart});
	const Json last = run(client, "project.get").at("midiParts");
	bool found = false;
	for (const Json& part : last)
		if (part.at("id") == bounced.at("parts")[0])
		{
			found = true;
			CHECK(part.at("name") == "Merged");
			CHECK(part.at("notes").size() == 3);
			CHECK(part.at("controllers").size() == 4); // the pedal's release is at the range's end
		}
	CHECK(found);

	// includeInserts plays the track's MIDI modifiers into the merged notes.
	const Json channel = run(client, "project.get").at("tracks")[0].at("channel");
	run(client, "channel.set", {{"channel", channel}, {"modifiers", {{"transpose", 12}}}});
	const Json withInserts = run(client, "midi.merge",
		{{"start", 0}, {"end", 48000}, {"tracks", {fixture.track}}, {"destination", fixture.track},
			{"includeInserts", true}});
	const auto insertsPart = withInserts.at("parts")[0].get<ObjectId>();
	std::vector<int> pitches;
	const Json withInsertsProject = run(client, "project.get");
	for (const Json& part : withInsertsProject.at("midiParts"))
		if (part.at("id") == insertsPart)
			for (const Json& note : part.at("notes"))
				pitches.push_back(note.at("pitch").get<int>());
	std::sort(pitches.begin(), pitches.end());
	CHECK(pitches == std::vector<int>{72, 76, 76});
}

TEST_CASE(
	"Extract MIDI Automation takes a shared copy out of its copies, which keep their controllers", "[integration]")
{
	Fixture fixture;
	Client& client = fixture.client;
	const auto copyAt = [&](Ticks ticks)
	{
		return run(client, "event.copy", {{"events", {fixture.part}}, {"ticks", ticks}, {"shared", true}})
			.at("events")
			.at(0)
			.get<ObjectId>();
	};
	const ObjectId second = copyAt(8 * g_quarter);
	const ObjectId third = copyAt(16 * g_quarter);
	const auto partOf = [&client](ObjectId id)
	{
		const Json parts = run(client, "project.get").at("midiParts");
		const auto found =
			std::find_if(parts.begin(), parts.end(), [id](const Json& part) { return part.at("id") == id; });
		REQUIRE(found != parts.end());
		return *found;
	};
	const Json before = partOf(second);
	REQUIRE(before.at("shared") != 0);

	CHECK(run(client, "midi.extractAutomation", {{"parts", {second}}}).at("parts") == Json{second});

	// Its own controllers went into points in its own range only.
	const Json extracted = partOf(second);
	CHECK(extracted.at("shared") == 0);
	CHECK(extracted.at("controllers").size() == 2); // the sustain pedal stays
	CHECK(extracted.at("notes").size() == before.at("notes").size());
	std::vector<int64_t> positions;
	const Json lanes = run(client, "automationLane.list", {{"track", fixture.track}}).at("lanes");
	for (const Json& lane : lanes)
	{
		CHECK(lane.at("target").at("number") == 7);
		for (const Json& point : lane.at("points"))
			positions.push_back(point.at("position").get<int64_t>());
	}
	std::sort(positions.begin(), positions.end());
	CHECK(positions == std::vector<int64_t>{8 * g_quarter, (8 * g_quarter) + 5, (8 * g_quarter) + 10});

	// The other copies play as before, and are still copies of each other.
	for (const ObjectId other : {fixture.part, third})
	{
		const Json part = partOf(other);
		CHECK(part.at("controllers") == before.at("controllers"));
		CHECK(part.at("shared") == before.at("shared"));
	}

	run(client, "history.undo");
	CHECK(partOf(second) == before);
	CHECK(run(client, "automationLane.list", {{"track", fixture.track}}).at("lanes").empty());
}

TEST_CASE("Bounce MIDI keeps the events of shared copies, which have the same ids", "[integration]")
{
	Fixture fixture;
	Client& client = fixture.client;
	const auto copy =
		run(client, "event.copy", {{"events", {fixture.part}}, {"ticks", 8 * g_quarter}, {"shared", true}})
			.at("events")
			.at(0)
			.get<ObjectId>();
	const Json bounced = run(client, "midi.bounce", {{"parts", {fixture.part, copy}}});
	REQUIRE(bounced.at("parts").size() == 1);
	const Json project = run(client, "project.get");
	REQUIRE(project.at("midiParts").size() == 1);
	const Json& part = project.at("midiParts")[0];
	CHECK(part.at("shared") == 0);
	std::vector<std::pair<int64_t, int>> notes;
	for (const Json& note : part.at("notes"))
		notes.emplace_back(note.at("position").get<int64_t>(), note.at("pitch").get<int>());
	std::sort(notes.begin(), notes.end());
	const int64_t second = 8 * g_quarter;
	CHECK(notes ==
		std::vector<std::pair<int64_t, int>>{{0, 60}, {g_quarter, 64}, {g_quarter, 64}, {second, 60},
			{second + g_quarter, 64}, {second + g_quarter, 64}});
	std::vector<std::pair<int64_t, int>> controllers;
	for (const Json& controller : part.at("controllers"))
		controllers.emplace_back(controller.at("position").get<int64_t>(), controller.at("number").get<int>());
	std::sort(controllers.begin(), controllers.end());
	CHECK(controllers ==
		std::vector<std::pair<int64_t, int>>{{0, 7}, {0, 64}, {5, 7}, {10, 7}, {2 * g_quarter, 64}, {second, 7},
			{second, 64}, {second + 5, 7}, {second + 10, 7}, {second + (2 * g_quarter), 64}});
}

namespace
{
	// What a track plays, from the render snapshot: (start in samples, pitch, MIDI channel), in order.
	std::vector<std::vector<int64_t>> playedOn(Engine& engine, ObjectId track)
	{
		std::vector<std::vector<int64_t>> played;
		for (const RenderChannel& channel : engine.renderSnapshots().acquire().channels)
			if (channel.track == track)
				for (const RenderNote& note : channel.notes)
					played.push_back({note.start, note.pitch, note.channel});
		std::sort(played.begin(), played.end());
		return played;
	}

	ObjectId addInstrumentTrack(Client& client)
	{
		return run(client, "track.add", {{"type", "instrument"}}).at("track").at("id").get<ObjectId>();
	}

	// A drum map on the track, with these sounds.
	void useDrumMap(Client& client, ObjectId track, const Json& sounds)
	{
		const auto map = run(client, "drumMap.add").at("map").at("id").get<ObjectId>();
		run(client, "drumMap.set", {{"map", map}, {"sounds", sounds}});
		run(client, "track.set", {{"track", track}, {"drumMap", map}});
	}

	// A chord track of F, which the track follows with Chords over its own C.
	void followChords(Client& client, ObjectId track)
	{
		run(client, "chord.add", {{"position", 0}, {"chord", "F"}});
		run(client, "chord.follow", {{"track", track}, {"mode", "chords"}, {"sync", "known"}, {"chord", "C"}});
	}

	Json onlyPartJson(Client& client, ObjectId track)
	{
		Json found;
		const Json project = run(client, "project.get");
		for (const Json& part : project.at("midiParts"))
			if (part.at("track") == track)
			{
				REQUIRE(found.is_null());
				found = part;
			}
		REQUIRE(!found.is_null());
		return found;
	}

	std::vector<int> storedPitches(const Json& part)
	{
		std::vector<int> pitches;
		for (const Json& note : part.at("notes"))
			pitches.push_back(note.at("pitch").get<int>());
		std::sort(pitches.begin(), pitches.end());
		return pitches;
	}

	constexpr Samples g_eightQuarters = Samples{8} * 24000; // at 120 BPM and 48 kHz
}

TEST_CASE("merge with Erase Destination takes a part out of its shared copies before erasing from it", "[integration]")
{
	Fixture fixture;
	Client& client = fixture.client;
	const ObjectId destination = addInstrumentTrack(client);
	const auto eventAt = [&](ObjectId target, int64_t position, int pitch)
	{
		run(client, "note.add",
			{{"part", target},
				{"notes", {{{"position", position}, {"length", 10}, {"pitch", pitch}, {"velocity", 0.5}}}}});
	};
	const auto makePart = [&](int64_t position)
	{
		return run(client, "midiPart.add", {{"track", destination}, {"position", position}, {"length", 8 * g_quarter}})
			.at("part")
			.at("id")
			.get<ObjectId>();
	};
	// In the range from the fourth quarter to the part's fifth: the note at 0 stays, the one at 4 quarters goes.
	const ObjectId cut = makePart(0);
	eventAt(cut, 0, 40);
	eventAt(cut, 4 * g_quarter, 41);
	const auto copyOf = [&](ObjectId part, const Json& to)
	{
		Json parameters = to;
		parameters["events"] = {part};
		parameters["shared"] = true;
		return run(client, "event.copy", parameters).at("events").at(0).get<ObjectId>();
	};
	const ObjectId later = copyOf(cut, {{"ticks", 32 * g_quarter}});
	const ObjectId otherTrack = addInstrumentTrack(client);
	const ObjectId elsewhere = copyOf(cut, {{"ticks", 48 * g_quarter}, {"track", otherTrack}});
	// A shared part with nothing in the range stays shared.
	const ObjectId untouched = makePart(64 * g_quarter);
	eventAt(untouched, 0, 50);
	const ObjectId untouchedCopy = copyOf(untouched, {{"ticks", 16 * g_quarter}});

	const auto partJson = [&](ObjectId id)
	{
		const Json project = run(client, "project.get");
		Json found;
		for (const Json& part : project.at("midiParts"))
			if (part.at("id") == id)
				found = part;
		REQUIRE(!found.is_null());
		return found;
	};
	const Json beforeCut = partJson(cut);
	const Json beforeLater = partJson(later);
	REQUIRE(beforeCut.at("shared") != 0);

	const Json merged = run(client, "midi.merge",
		{{"start", 3 * 24000}, {"end", 5 * 24000}, {"tracks", {fixture.track}}, {"destination", destination},
			{"eraseDestination", true}});
	CHECK(merged.at("changedParts") == Json{cut});

	CHECK(storedPitches(partJson(cut)) == std::vector<int>{40});
	CHECK(partJson(cut).at("shared") == 0);
	// The copies outside the range keep everything, with their ids.
	CHECK(partJson(later) == beforeLater);
	CHECK(storedPitches(partJson(elsewhere)) == std::vector<int>{40, 41});
	CHECK(partJson(later).at("shared") != 0);
	// The notes of the part that left have new ids, so they aren't the copies' any more.
	CHECK(partJson(cut).at("notes").at(0).at("id") != beforeCut.at("notes").at(0).at("id"));
	CHECK(partJson(untouched).at("shared") != 0);
	CHECK(partJson(untouched).at("shared") == partJson(untouchedCopy).at("shared"));

	run(client, "history.undo");
	CHECK(partJson(cut) == beforeCut);
	CHECK(partJson(later) == beforeLater);
}

TEST_CASE(
	"bounce plays the same on a track whose drum map or chord track maps notes before the transpose", "[integration]")
{
	Fixture fixture;
	Client& client = fixture.client;
	const ObjectId track = fixture.track;
	const ObjectId second =
		run(client, "midiPart.add", {{"track", track}, {"position", 8 * g_quarter}, {"length", 4 * g_quarter}})
			.at("part")
			.at("id")
			.get<ObjectId>();
	run(client, "note.add",
		{{"part", second}, {"notes", {{{"position", 0}, {"length", 10}, {"pitch", 62}, {"velocity", 0.5}}}}});
	run(client, "midiPart.set", {{"part", fixture.part}, {"transpose", 2}});
	run(client, "midiPart.set", {{"part", second}, {"transpose", 2}});

	const auto bounceAndCompare = [&](int keptTranspose)
	{
		const auto before = playedOn(fixture.engine, track);
		REQUIRE(!before.empty());
		const Json bounced = run(client, "midi.bounce", {{"parts", {fixture.part, second}}});
		REQUIRE(bounced.at("parts").size() == 1);
		CHECK(playedOn(fixture.engine, track) == before);
		CHECK(onlyPartJson(client, track).at("transpose") == keptTranspose);
		return before;
	};

	SECTION("O-notes that differ from the pitches keep the transpose on the new part")
	{
		// Written into the notes, 60 + 2 would play 62's O-note, 90, instead of 30 + 2.
		useDrumMap(client, track,
			{{{"pitch", 60}, {"oNote", 30}}, {{"pitch", 62}, {"oNote", 90}, {"channel", 3}},
				{{"pitch", 64}, {"oNote", 34}}, {{"pitch", 66}, {"oNote", 80}}});
		const auto before = bounceAndCompare(2);
		CHECK(before.front()[1] == 32);
		CHECK(storedPitches(onlyPartJson(client, track)) == std::vector<int>{60, 62, 64, 64});
	}

	SECTION("a track that follows the chord track keeps the transpose on the new part")
	{
		followChords(client, track);
		bounceAndCompare(2);
		CHECK(storedPitches(onlyPartJson(client, track)) == std::vector<int>{60, 62, 64, 64});
	}

	SECTION("a drum map that moves no note here takes the transpose into the notes")
	{
		useDrumMap(client, track, {{{"pitch", 10}, {"oNote", 20}}});
		bounceAndCompare(0);
		CHECK(storedPitches(onlyPartJson(client, track)) == std::vector<int>{62, 64, 66, 66});
	}

	SECTION("a sound that is muted stays muted, wherever the transpose would take it")
	{
		useDrumMap(client, track, {{{"pitch", 60}, {"mute", true}}});
		const auto before = bounceAndCompare(0);
		CHECK(before.size() == 3);
		CHECK(storedPitches(onlyPartJson(client, track)) == std::vector<int>{60, 64, 66, 66});
	}

	SECTION("a transpose that leads to a muted sound stays on the new part")
	{
		// Written into the notes, 64 + 2 would be 66, which doesn't play.
		useDrumMap(client, track, {{{"pitch", 66}, {"mute", true}}});
		const auto before = bounceAndCompare(2);
		CHECK(before.size() == 4);
		CHECK(storedPitches(onlyPartJson(client, track)) == std::vector<int>{60, 62, 64, 64});
	}

	SECTION("a transpose that leads to a sound on another channel stays on the new part")
	{
		// The O-notes shift alike, but 60 plays on the note's channel and 62, which 60 + 2 would be, on channel 3.
		useDrumMap(client, track,
			{{{"pitch", 60}, {"oNote", 30}}, {{"pitch", 62}, {"oNote", 32}, {"channel", 3}},
				{{"pitch", 64}, {"oNote", 34}, {"channel", 3}}, {{"pitch", 66}, {"oNote", 36}, {"channel", 3}}});
		const auto before = bounceAndCompare(2);
		CHECK(before.front() == std::vector<int64_t>{0, 32, 0});
		CHECK(storedPitches(onlyPartJson(client, track)) == std::vector<int>{60, 62, 64, 64});
	}

	SECTION("different transposes that only the notes could hold make a part for each")
	{
		run(client, "midiPart.set", {{"part", second}, {"transpose", 5}});
		useDrumMap(client, track, {{{"pitch", 60}, {"oNote", 30}}});
		const auto before = playedOn(fixture.engine, track);
		const Json bounced = run(client, "midi.bounce", {{"parts", {fixture.part, second}}});
		REQUIRE(bounced.at("parts").size() == 2);
		CHECK(playedOn(fixture.engine, track) == before);
		std::vector<std::pair<int, std::vector<int>>> made;
		const Json project = run(client, "project.get");
		for (const Json& id : bounced.at("parts"))
			for (const Json& part : project.at("midiParts"))
				if (part.at("id") == id)
					made.emplace_back(part.at("transpose").get<int>(), storedPitches(part));
		std::sort(made.begin(), made.end());
		CHECK(made == std::vector<std::pair<int, std::vector<int>>>{{2, {60, 64, 64}}, {5, {62}}});
	}

	SECTION("a note hidden past its part's end doesn't keep the transpose on the part")
	{
		run(client, "note.add",
			{{"part", second},
				{"notes", {{{"position", 4 * g_quarter}, {"length", 10}, {"pitch", 70}, {"velocity", 0.5}}}}});
		useDrumMap(client, track, {{{"pitch", 72}, {"oNote", 20}}}); // 70 + 2 would play 20
		const auto before = playedOn(fixture.engine, track);
		const Json bounced = run(client, "midi.bounce", {{"parts", {fixture.part, second}}});
		REQUIRE(bounced.at("parts").size() == 1);
		CHECK(playedOn(fixture.engine, track) == before);
		CHECK(onlyPartJson(client, track).at("transpose") == 0);
	}

	SECTION("different transposes go into the notes if they play the same there")
	{
		run(client, "midiPart.set", {{"part", second}, {"transpose", 5}});
		useDrumMap(client, track, {{{"pitch", 10}, {"oNote", 20}}});
		bounceAndCompare(0);
		CHECK(storedPitches(onlyPartJson(client, track)) == std::vector<int>{62, 66, 66, 67});
	}
}

TEST_CASE("merge writes what the drum map and the chord track play, for a destination that plays them", "[integration]")
{
	Fixture fixture;
	Client& client = fixture.client;
	const ObjectId track = fixture.track;
	run(client, "midiPart.set", {{"part", fixture.part}, {"transpose", 2}});
	const auto mergeOnto = [&](const Json& extra)
	{
		Json parameters = extra;
		parameters["start"] = 0;
		parameters["end"] = g_eightQuarters;
		parameters["tracks"] = {track};
		return run(client, "midi.merge", parameters);
	};
	const auto newTrackPlays = [&](const std::vector<std::vector<int64_t>>& expected)
	{
		const Json merged = mergeOnto(Json::object());
		REQUIRE(merged.at("tracks").size() == 1);
		const auto added = merged.at("tracks").at(0).get<ObjectId>();
		CHECK(playedOn(fixture.engine, added) == expected);
		CHECK(onlyPartJson(client, added).at("transpose") == 0);
		return added;
	};

	SECTION("onto a new track, with the sounds' O-notes and channels")
	{
		useDrumMap(client, track, {{{"pitch", 60}, {"oNote", 30}, {"channel", 3}}, {{"pitch", 64}, {"mute", true}}});
		const auto before = playedOn(fixture.engine, track);
		REQUIRE(before == std::vector<std::vector<int64_t>>{{0, 32, 3}});
		newTrackPlays(before); // the muted sound's notes are left out
	}

	SECTION("onto a new track, as the chord track maps them")
	{
		followChords(client, track);
		const auto before = playedOn(fixture.engine, track);
		REQUIRE(!before.empty());
		newTrackPlays(before);
	}

	SECTION("a part across a chord change follows it note by note")
	{
		followChords(client, track);
		run(client, "chord.add", {{"position", g_quarter}, {"chord", "G"}});
		const auto before = playedOn(fixture.engine, track);
		REQUIRE(before.size() == 3);
		newTrackPlays(before);
	}

	SECTION("a part later in the project plays the chord at its place, in a range that starts there")
	{
		const ObjectId later =
			run(client, "midiPart.add", {{"track", track}, {"position", 8 * g_quarter}, {"length", 4 * g_quarter}})
				.at("part")
				.at("id")
				.get<ObjectId>();
		run(client, "note.add",
			{{"part", later}, {"notes", {{{"position", 0}, {"length", 10}, {"pitch", 60}, {"velocity", 0.5}}}}});
		run(client, "chord.add", {{"position", 0}, {"chord", "F"}});
		run(client, "chord.add", {{"position", 8 * g_quarter}, {"chord", "G"}});
		run(client, "chord.follow", {{"track", track}, {"mode", "chords"}, {"sync", "known"}, {"chord", "C"}});
		const auto all = playedOn(fixture.engine, track);
		const std::vector<std::vector<int64_t>> before{all.back()};
		REQUIRE(before[0][0] == g_eightQuarters);
		const Json merged =
			run(client, "midi.merge", {{"start", g_eightQuarters}, {"end", 2 * g_eightQuarters}, {"tracks", {track}}});
		REQUIRE(merged.at("tracks").size() == 1);
		CHECK(playedOn(fixture.engine, merged.at("tracks").at(0).get<ObjectId>()) == before);
	}

	SECTION("onto the track it follows the chord track on, keeping the notes' pitches")
	{
		run(client, "midiPart.set", {{"part", fixture.part}, {"transpose", 0}});
		followChords(client, track);
		const auto before = playedOn(fixture.engine, track);
		mergeOnto({{"destination", track}});
		run(client, "midiPart.remove", {{"part", fixture.part}});
		CHECK(playedOn(fixture.engine, track) == before);
		CHECK(storedPitches(onlyPartJson(client, track)) == std::vector<int>{60, 64, 64});
	}

	SECTION("onto the track's own drum map, keeping the notes' pitches where they play the same")
	{
		run(client, "midiPart.set", {{"part", fixture.part}, {"transpose", 0}});
		useDrumMap(client, track, {{{"pitch", 60}, {"oNote", 30}}, {{"pitch", 64}, {"oNote", 34}}});
		const auto before = playedOn(fixture.engine, track);
		mergeOnto({{"destination", track}});
		run(client, "midiPart.remove", {{"part", fixture.part}});
		CHECK(playedOn(fixture.engine, track) == before);
		CHECK(storedPitches(onlyPartJson(client, track)) == std::vector<int>{60, 64, 64});
	}

	SECTION("onto a destination with a drum map, through the sound that plays the pitch")
	{
		const ObjectId destination = addInstrumentTrack(client);
		// 62 and 66 play on this track as 72 and 76: the map sends 70 and 71 to them.
		useDrumMap(client, destination,
			{{{"pitch", 62}, {"oNote", 72}}, {{"pitch", 66}, {"oNote", 76}}, {{"pitch", 70}, {"oNote", 62}},
				{{"pitch", 71}, {"oNote", 66}}});
		const auto before = playedOn(fixture.engine, track);
		mergeOnto({{"destination", destination}});
		CHECK(playedOn(fixture.engine, destination) == before);
		CHECK(storedPitches(onlyPartJson(client, destination)) == std::vector<int>{70, 71, 71});
	}

	SECTION("onto a destination whose sound for the pitch is muted, through another sound that plays it")
	{
		const ObjectId destination = addInstrumentTrack(client);
		useDrumMap(client, destination, {{{"pitch", 62}, {"mute", true}}, {{"pitch", 70}, {"oNote", 62}}});
		const auto before = playedOn(fixture.engine, track);
		mergeOnto({{"destination", destination}});
		CHECK(playedOn(fixture.engine, destination) == before);
		CHECK(storedPitches(onlyPartJson(client, destination)) == std::vector<int>{66, 66, 70});
	}

	SECTION("onto a destination whose sound for the pitch is on another channel, through one on the note's")
	{
		const ObjectId destination = addInstrumentTrack(client);
		useDrumMap(client, destination, {{{"pitch", 62}, {"channel", 5}}, {{"pitch", 70}, {"oNote", 62}}});
		const auto before = playedOn(fixture.engine, track);
		mergeOnto({{"destination", destination}});
		CHECK(playedOn(fixture.engine, destination) == before);
		CHECK(storedPitches(onlyPartJson(client, destination)) == std::vector<int>{66, 66, 70});
	}

	SECTION("a pitch the destination's drum map plays no sound for stays as it played")
	{
		const ObjectId destination = addInstrumentTrack(client);
		useDrumMap(client, destination, {{{"pitch", 62}, {"oNote", 72}}});
		mergeOnto({{"destination", destination}});
		CHECK(storedPitches(onlyPartJson(client, destination)) == std::vector<int>{62, 66, 66});
	}
}

TEST_CASE("merge refuses when no MIDI track plays; extract MIDI automation announces lanes and parts", "[integration]")
{
	Fixture fixture;
	Client& client = fixture.client;
	const Json channel = run(client, "project.get").at("tracks")[0].at("channel");
	run(client, "channel.set", {{"channel", channel}, {"mute", true}});
	CHECK_THAT(reject(client, "midi.merge", {{"start", 0}, {"end", 48000}}), ContainsSubstring("plays"));
	// Named tracks merge, muted or not.
	run(client, "midi.merge", {{"start", 0}, {"end", 48000}, {"tracks", {fixture.track}}});

	while (client.poll())
	{
	}
	const Json result = run(client, "midi.extractAutomation", {{"parts", {fixture.part}}});
	CHECK(result.at("parts") == Json{fixture.part});
	CHECK(result.at("lanes").size() == 1);
	CHECK(result.at("changedLanes").empty());
	const Json lanes = run(client, "automationLane.list", {{"track", fixture.track}}).at("lanes");
	REQUIRE(lanes.size() == 1);
	CHECK(lanes[0].at("target").at("type") == "controller");
	CHECK(lanes[0].at("target").at("number") == 7);
	CHECK(lanes[0].at("points").size() == 3);
	CHECK(fixture.partJson().at("controllers").size() == 2); // the sustain pedal
	bool laneAdded = false;
	bool partChanged = false;
	while (const std::optional<Message> message = client.poll())
		if (const auto* event = std::get_if<Event>(&*message))
		{
			laneAdded =
				laneAdded || (event->name == "automationLane.added" && event->data.at("lane").at("points").size() == 3);
			partChanged = partChanged || event->name == "midiPart.changed";
		}
	CHECK(laneAdded);
	CHECK(partChanged);
	// Once more, nothing is left to extract.
	CHECK(run(client, "midi.extractAutomation", {{"parts", {fixture.part}}}).at("parts").empty());
}

TEST_CASE("sustain pedal to note length holds notes to the end of a part on a linear track", "[integration]")
{
	Engine engine;
	Client client{engine};
	const auto track =
		run(client, "track.add", {{"type", "instrument"}, {"timeBase", "linear"}}).at("track").at("id").get<ObjectId>();
	const Json project = run(client, "project.get");
	const gnubase::time::TempoMap tempo(project.at("tempo")[0].at("bpm").get<double>());
	const Samples bar = tempo.toSamples(4 * g_quarter, project.at("sampleRate").get<double>());
	const auto part = run(client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", bar}})
						  .at("part")
						  .at("id")
						  .get<ObjectId>();
	run(client, "note.add",
		{{"part", part}, {"notes", {{{"position", 0}, {"length", g_quarter}, {"pitch", 60}, {"velocity", 0.5}}}}});
	run(client, "controller.add",
		{{"part", part}, {"controllers", {{{"position", g_quarter / 2}, {"number", 64}, {"value", 1.0}}}}});
	run(client, "midi.pedalToNoteLength", {{"parts", {part}}});
	const Json notes = run(client, "project.get").at("midiParts")[0].at("notes");
	REQUIRE(notes.size() == 1);
	CHECK(notes[0].at("length") == 4 * g_quarter); // the pedal is never released: to the part's end, a bar
}

TEST_CASE("invert releases the sustain pedal within a part on a linear track", "[integration]")
{
	Engine engine;
	Client client{engine};
	const auto track =
		run(client, "track.add", {{"type", "instrument"}, {"timeBase", "linear"}}).at("track").at("id").get<ObjectId>();
	const Json project = run(client, "project.get");
	const gnubase::time::TempoMap tempo(project.at("tempo")[0].at("bpm").get<double>());
	const Samples bar = tempo.toSamples(4 * g_quarter, project.at("sampleRate").get<double>());
	const auto part = run(client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", bar}})
						  .at("part")
						  .at("id")
						  .get<ObjectId>();
	run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", 0}, {"length", g_quarter}, {"pitch", 60}, {"velocity", 0.5}},
					{{"position", 3 * g_quarter}, {"length", g_quarter}, {"pitch", 62}, {"velocity", 0.5}}}}});
	run(client, "controller.add",
		{{"part", part},
			{"controllers",
				{{{"position", 0}, {"number", 64}, {"value", 1.0}},
					{{"position", 2 * g_quarter}, {"number", 64}, {"value", 0.0}}}}});
	run(client, "midi.invert", {{"parts", {part}}});
	const Json inverted = run(client, "project.get");
	std::vector<std::pair<Ticks, double>> pedal;
	for (const Json& controller : inverted.at("midiParts")[0].at("controllers"))
		pedal.emplace_back(controller.at("position").get<Ticks>(), controller.at("value").get<double>());
	std::sort(pedal.begin(), pedal.end());
	// The part is a bar long in ticks: the pedal goes up on its last tick, not at its end, where playback drops it.
	CHECK(pedal == std::vector<std::pair<Ticks, double>>{{0, 0.0}, {2 * g_quarter, 1.0}, {(4 * g_quarter) - 1, 0.0}});
}
