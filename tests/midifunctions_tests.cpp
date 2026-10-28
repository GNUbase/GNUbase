// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/midi/functions.h"
#include "engine/midi/midivalues.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using namespace gnubase::engine::midi;
using Catch::Matchers::ContainsSubstring;
using gnubase::time::g_ticksPerQuarter;
using gnubase::time::Ticks;

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
			part.notes[id] =
				Note{id, position, length, static_cast<uint8_t>(pitch), velocity, static_cast<uint8_t>(channel)};
			return *this;
		}

		PartBuilder& controller(
			ObjectId id, Ticks position, ControllerType type, int number, uint32_t value, int channel = 0)
		{
			part.controllers[id] =
				Controller{id, position, type, static_cast<uint8_t>(number), value, static_cast<uint8_t>(channel), {}};
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
		const Channel channel{project.newId(), track.id, type == TrackType::Midi ? 0 : 2, 0, 0, {}};
		track.channel = channel.id;
		project.channels[channel.id] = channel;
		project.tracks.push_back(track.id);
		project.trackById[track.id] = track;
		return track.id;
	}

	ObjectId addPart(Project& project, ObjectId track, Ticks position, Ticks length, const MidiPart& content = {})
	{
		MidiPart part = content;
		part.id = project.newId();
		part.track = track;
		part.name = project.trackById.at(track).name;
		part.position = {gnubase::time::TimeBase::Musical, position};
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
	pedalToNoteLength(part, NoteSelection());
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
		pedalToNoteLength(edge, NoteSelection());
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
	reverse(selected, NoteSelection({1, 2}));
	CHECK(start(selected, 1) == 2 * g_quarter);
	CHECK(start(selected, 2) == g_quarter);
	CHECK(start(selected, 3) == 4 * g_quarter);
	CHECK(selected.controllers == part.controllers);

	reverse(part, NoteSelection());
	// Every event from 0 to 4 quarters: positions mirror around 2 quarters.
	CHECK(start(part, 1) == 3 * g_quarter);
	CHECK(start(part, 2) == 2 * g_quarter);
	CHECK(start(part, 3) == 0);
	CHECK(length(part, 1) == 10);
	CHECK(length(part, 3) == 30);
	CHECK(part.controllers.at(4).position == 4 * g_quarter);
	CHECK(part.controllers.at(5).position == g_quarter);
}

TEST_CASE("invert mirrors the notes in time: a note starts where its mirror image ended", "[unit]")
{
	MidiPart part = PartBuilder()
						.note(1, g_quarter, g_quarter, 60)
						.note(2, 3 * g_quarter, g_quarter / 2, 62)
						.cc(3, 2 * g_quarter, 7, 10)
						.part;
	MidiPart selected = part;
	invert(selected, NoteSelection({1, 2}));
	// The notes span 1 to 3.5 quarters.
	CHECK(start(selected, 1) == 5 * g_quarter / 2);
	CHECK(start(selected, 2) == g_quarter);
	CHECK(selected.controllers == part.controllers);

	invert(part, NoteSelection());
	CHECK(start(part, 1) == 5 * g_quarter / 2);
	CHECK(start(part, 2) == g_quarter);
	CHECK(part.controllers.at(3).position == 5 * g_quarter / 2);
	CHECK(length(part, 1) == g_quarter);
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
	pedalToNoteLength(part, NoteSelection());
	deleteOverlaps(part, NoteSelection(), OverlapMode::Poly);
	deleteDoubles(part, NoteSelection());
	deleteControllers(part);
	deleteContinuousControllers(part);
	deleteNotes(part, NoteSelection(), DeleteNotesSettings{10, 10, false});
	restrictPolyphony(part, NoteSelection(), 1);
	thinOut(part, 100);
	reverse(part, NoteSelection());
	invert(part, NoteSelection());
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

TEST_CASE("dissolve moves events into new tracks by channel or by pitch", "[unit]")
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
		CHECK(notesOf(first) == std::vector<std::vector<int64_t>>{{0, 36, 10, 0}, {9 * g_quarter, 42, 10, 0}});
		const MidiPart& second = onlyPartOn(project, changes.tracks[1]);
		CHECK(notesOf(second) == std::vector<std::vector<int64_t>>{{g_quarter, 38, 10, 1}, {2 * g_quarter, 36, 10, 1}});
		CHECK(second.controllers.size() == 2);
		// The SysEx stays in the original part.
		CHECK(changes.changedParts == std::vector<ObjectId>{source});
		CHECK(project.midiParts.at(source).notes.empty());
		CHECK(controllerTypes(project.midiParts.at(source)) == std::vector<ControllerType>{ControllerType::SysEx});
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
		const MidiPart& snare = onlyPartOn(project, changes.tracks[1]);
		CHECK(snare.controllers.size() == 1); // its poly pressure
		const MidiPart& hat = onlyPartOn(project, changes.tracks[2]);
		CHECK(hat.position.value == 12 * g_quarter); // bar 4
		CHECK(hat.length == 4 * g_quarter);
		CHECK(notesOf(hat) == std::vector<std::vector<int64_t>>{{g_quarter, 42, 10, 0}});
		CHECK(controllerTypes(project.midiParts.at(source)) ==
			std::vector<ControllerType>{ControllerType::Control, ControllerType::SysEx});
	}
	SECTION("a part left empty is removed")
	{
		project.midiParts.at(source).controllers.clear();
		const PartChanges changes = dissolve(project, {source}, DissolveBy::Pitch, false);
		CHECK(changes.removedParts == std::vector<ObjectId>{source});
		CHECK(project.midiParts.count(source) == 0);
	}
	SECTION("an empty part makes no tracks")
	{
		const ObjectId empty = addPart(project, drums, 0, g_quarter);
		const PartChanges changes = dissolve(project, {empty}, DissolveBy::Channel, false);
		CHECK(changes.tracks.empty());
		CHECK(project.midiParts.count(empty) == 1);
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
		{"midi.bounce", {{"parts", parts}}},
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
	CHECK(dissolved.at("changedParts").empty());
	CHECK(dissolved.at("removedParts") == Json{fixture.part});
	const Json project = run(client, "project.get");
	CHECK(project.at("tracks").size() == 2);
	CHECK(project.at("tracks")[1].at("name") == "Piano Ch 1");

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
}
