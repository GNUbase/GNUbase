// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

// The MIDI functions (#133): edits that process the notes and controllers of MIDI parts, such as transpose, legato or
// delete doubles, and edits that make parts from other parts, such as merge, dissolve and bounce. They change the
// document directly; the commands in functioncommands.cpp check their parameters, run them and record the undo steps.
//
// Notes on different MIDI channels are separate voices: they never shorten, delete or limit each other.
namespace gnubase::engine::midi
{
	// The notes a function works on: every note of a part, or the selected ones.
	class NoteSelection
	{
	public:
		NoteSelection() = default; // every note
		explicit NoteSelection(std::set<ObjectId> notes) : m_all(false), m_notes(std::move(notes)) {}

		bool all() const { return m_all; }
		bool contains(ObjectId note) const { return m_all || m_notes.count(note) != 0; }

	private:
		bool m_all = true;
		std::set<ObjectId> m_notes;
	};

	// Makes the id of an event a function adds.
	using NewId = std::function<ObjectId()>;

	// A part's length in ticks, also on a linear track, whose parts are measured in samples.
	time::Ticks lengthInTicks(const Project& project, const MidiPart& part);

	// Transpose.

	enum class ScaleType : uint8_t
	{
		Major,
		Minor, // natural minor
		HarmonicMinor,
		MelodicMinor,
		Dorian,
		Phrygian,
		Lydian,
		Mixolydian,
		Locrian,
		MajorPentatonic,
		MinorPentatonic,
		Blues,
		WholeTone,
		// The scales of altered chords (#939): dominant sevenths with #11, b13, b9 and b13, altered tensions, b9 or
		// #9, and the diminished seventh chord.
		LydianDominant,
		MixolydianFlat6,
		PhrygianDominant,
		Altered,
		HalfWholeDiminished, // half step first, the dominant diminished scale
		WholeHalfDiminished, // whole step first
	};

	const char* scaleTypeName(ScaleType type);
	std::optional<ScaleType> scaleTypeFromName(const std::string& name);
	std::vector<std::string> scaleTypeNames();
	// A scale type's notes as intervals from its root, from the lowest up.
	std::vector<int> scaleSteps(ScaleType type);

	struct Scale
	{
		int root = 0; // pitch class: 0 is C, 11 is B
		ScaleType type = ScaleType::Major;
	};

	struct TransposeSettings
	{
		int semitones = 0;
		// Scale correction: moves every note from its degree in the current scale to the same degree in the new one
		// (C major to C minor turns E into E flat). Notes outside the current scale, and every note when the scales
		// have a different number of degrees, go to the closest note of the new scale, the lower one on a tie.
		std::optional<Scale> from;
		std::optional<Scale> to;
		// Keep notes in range: a note that would leave the range moves by octaves into it, and where no octave fits,
		// to the limit it crossed. Lowest equal to highest puts every note on that pitch.
		bool keepInRange = false;
		int lowest = 0;
		int highest = 127;
	};

	// The closest pitch of the scale, the lower one on a tie. Real-time safe.
	int nearestInScale(int pitch, const Scale& scale) noexcept;

	// The new pitch of a note, 0 to 127: notes that would leave MIDI's range move by octaves into it.
	int transposePitch(int pitch, const TransposeSettings& settings);
	// Transposes the selected notes; with every note selected, poly pressure events follow their pitch.
	void transpose(MidiPart& part, const NoteSelection& selection, const TransposeSettings& settings);

	// Note lengths and velocities.

	// Extends each selected note to the start of the next note on its channel, or of the next selected note with
	// betweenSelected, plus overlap (negative: a gap). Notes without a next note keep their length; no note gets
	// shorter than one tick.
	void legato(MidiPart& part, const NoteSelection& selection, time::Ticks overlap, bool betweenSelected);
	void fixLengths(MidiPart& part, const NoteSelection& selection, time::Ticks length);
	void fixVelocities(MidiPart& part, const NoteSelection& selection, uint16_t velocity);

	// The Velocity dialog's changes (reference manual pp. 1126-1127).
	enum class VelocityChange : uint8_t
	{
		Add,      // adds amount
		Compress, // multiplies by ratio: below 1 compresses the differences, above 1 expands them
		Limit,    // keeps velocities from lower to upper
		Fixed,    // sets them to amount
		Variance, // lowers each by a random part of it, up to variance
	};

	// Velocities are 0 to 1 here, as the commands take them.
	struct VelocitySettings
	{
		VelocityChange type = VelocityChange::Add;
		double amount = 0.0;   // Add: -1 to 1; Fixed: 0 to 1
		double ratio = 1.0;    // Compress: 0 to 3
		double lower = 0.0;    // Limit: 0 to 1, not above upper
		double upper = 1.0;    // Limit: 0 to 1
		double variance = 0.0; // Variance: 0 to 1, the largest part of its velocity that a note loses
	};

	// Changes the velocities of the selected notes; they stay within 0 to 1. Variance takes a note's random part from
	// the ids of the part and the note, so it is the same each time.
	void changeVelocities(MidiPart& part, const NoteSelection& selection, const VelocitySettings& settings);

	// Extends each selected note that ends while the sustain pedal (controller 64) on its channel is down to where the
	// pedal is released, or the part's end (partLength ticks, see lengthInTicks) if it never is, but no further than
	// the next note of the same pitch. Then removes the sustain pedal events of the channels the selection plays on.
	void pedalToNoteLength(MidiPart& part, const NoteSelection& selection, time::Ticks partLength);

	// Deleting.

	enum class OverlapMode : uint8_t
	{
		Mono, // a note ends where the next note of the same pitch starts
		Poly, // a note ends where the next note of any pitch starts; notes that start together don't overlap
	};

	// Shortens the selected notes so that none still sounds when the next note starts.
	void deleteOverlaps(MidiPart& part, const NoteSelection& selection, OverlapMode mode);
	// Of selected notes with the same pitch, channel and position, keeps the longest one (the first added on a tie).
	void deleteDoubles(MidiPart& part, const NoteSelection& selection);
	// Removes control changes, pitch bend and both kinds of pressure; program changes and SysEx stay.
	void deleteControllers(MidiPart& part);

	// Controllers with a continuous range of values: everything but bank select, data entry, the switches 64 to 69
	// (sustain, portamento, sostenuto, soft pedal, legato and hold 2), data increment and decrement, the parameter
	// numbers (NRPN, RPN) and the channel mode messages 120 to 127.
	bool isContinuousController(int number);
	// Removes the control changes of continuous controllers; switches such as the sustain pedal stay.
	void deleteContinuousControllers(MidiPart& part);

	struct DeleteNotesSettings
	{
		std::optional<time::Ticks> minimumLength; // shorter notes are deleted
		std::optional<uint16_t> minimumVelocity;  // softer notes are deleted
		bool both = false; // with both minimums: delete only notes that are too short and too soft
	};

	void deleteNotes(MidiPart& part, const NoteSelection& selection, const DeleteNotesSettings& settings);

	// Shortens selected notes so that at most voices of them sound at once on each channel. A new note ends the one
	// that started first, the lowest of notes that started together; a note that would lose its whole length is
	// deleted.
	void restrictPolyphony(MidiPart& part, const NoteSelection& selection, int voices);

	// Thins out continuous controllers, pitch bend and pressure, per controller and channel: removes events that
	// repeat the value before them, and events closer than minimumDistance to the event kept before them, unless the
	// curve turns there, the next event is at least minimumDistance away or it is the last one. Switches, bank select,
	// data entry and the other controllers isContinuousController excludes stay, like program changes and SysEx.
	void thinOut(MidiPart& part, time::Ticks minimumDistance);

	// Order.

	// Reverse and invert mirror a range in time. With every note selected, each lane of a controller that holds a
	// value (continuous controllers, the switches 64 to 69, pitch bend and pressure) plays its values backwards: the
	// value that held from one event to the next, or to the range's end, holds as long in the mirror image. Values
	// overwritten at the same tick go, and where the range ends the lane returns to the value it had there, with a
	// new event (newId) if needed. That event goes on the part's last tick (partLength ticks, see lengthInTicks) if the
	// range reaches the part's end, since playback drops events from there on. Program changes, SysEx, bank select,
	// data entry, the parameter numbers and channel mode messages stay in place.

	// Reverses the order in which the selected notes start, keeping their lengths: the last one starts where the
	// first one started.
	void reverse(MidiPart& part, const NoteSelection& selection, time::Ticks partLength, const NewId& newId);
	// Mirrors the selected notes in time: where a note ended, its mirror image starts.
	void invert(MidiPart& part, const NoteSelection& selection, time::Ticks partLength, const NewId& newId);

	// Automation.

	struct AutomationChanges
	{
		std::vector<ObjectId> addedLanes;
		std::vector<ObjectId> changedLanes;
		std::vector<ObjectId> changedParts;
	};

	// Extract MIDI Automation (reference manual p. 1129): moves the continuous controllers (isContinuousController)
	// that play in the parts into automation lanes of their tracks, a lane per controller and channel. Each event
	// becomes a step point where it plays, with its value scaled to 0 to 1. A lane the track already has for the
	// controller loses its points in the part's range first, but not those an earlier part of the same call added, so
	// overlapping parts keep each other's, and reads from then on; otherwise a lane is added. A shared copy leaves its
	// copies, which keep their controllers; all its events get new ids, so the play probability, velocity variance and
	// random MIDI modifier values of its notes, drawn from its part's and their own ids, draw anew. Pitch bend,
	// pressure, switches such as the sustain pedal, events past a part's end and the events of muted parts stay, as
	// none of them would play the same as automation.
	AutomationChanges extractAutomation(Project& project, const std::vector<ObjectId>& parts);

	// Functions that make parts. They report what they changed, so the commands can announce it.

	struct PartChanges
	{
		std::vector<ObjectId> tracks; // tracks added, each with its channel
		std::vector<ObjectId> addedParts;
		std::vector<ObjectId> changedParts;
		std::vector<ObjectId> removedParts;
	};

	struct MergeSettings
	{
		time::Samples start = 0; // the range, in project samples like the locators
		time::Samples end = 0;
		std::vector<ObjectId> tracks; // the MIDI and instrument tracks to merge; empty: those that play
		ObjectId destination = 0;     // a MIDI or instrument track; 0 adds a MIDI track
		bool eraseDestination = false;
		bool includeChase = false;
		bool includeInserts = false; // also the tracks' MIDI modifiers (Include Inserts)
		std::string name;            // the new part's; empty: the destination track's
	};

	// The MIDI and instrument tracks that play, in track order, by playback's rules: neither their channel nor a
	// channel it plays into is muted, silenced by another channel's solo or on a disabled track. Sends aren't
	// followed: a track heard only through a send doesn't count.
	std::vector<ObjectId> playingTracks(const Project& project);

	// Copies the notes and controllers that start in the range on the tracks into a new part between start and end
	// on the destination track, leaving out muted parts, which don't play. Events are in the range when their
	// position in samples is, as in playback; the new part starts at the first of them if that is earlier than start
	// in ticks. Notes keep their lengths, cut where their part ends. They get their parts' transpose and velocity as
	// playback applies them, and with includeInserts also their tracks' MIDI modifiers (reference manual pp. 1096,
	// 1120), with the random values playback draws; a note they move out of the range or that doesn't play is left
	// out. Notes are written as they play through their tracks' drum maps and the chord track; a destination with its
	// own gets the pitch that plays them, if one does, else what played, which it then maps. The new part gets the
	// root key and Global Transpose setting the parts its notes come from share, or, when they differ, the project
	// root key and follow, and then each note the difference its part's global transposition makes transposed into
	// it, so that it keeps the pitch it played with (#954): with includeInserts the modifiers' note ranges see the
	// pitch that plays, with the source part's global transpose, before the new part's comes off; without it only the
	// difference goes into the note, as the modifiers stay on the track. eraseDestination first
	// removes the destination track's parts that lie in the range and the events of other parts that start in it, a
	// shared copy leaving its copies for that: its notes get new ids, so their play probability, velocity variance and
	// random MIDI modifier values draw anew. includeChase adds the last controller value of every controller, program
	// and channel before start at the new part's start, and in any case the new part starts with the directions of
	// the tracks' articulation maps in effect at start (#175). What a source part's notes can't hold (a pitch beyond
	// 0-127, its velocity under a note's velocity variance or the destination's modifiers) stays on a new part of its
	// own with the part's setting (#1240, #1253): the first new part, on the first lane, is the one with the shared
	// setting, else the one of the earliest source part, and has the controllers; the others follow in the order of
	// their earliest source parts on new lanes, with the articulation events only. A part of its own that no note goes
	// into (all of them left out, or playing nowhere) is left out, and the next one takes its place (#1491). A new
	// destination track goes to the end of the track list, routed to the instrument of the first track merged.
	PartChanges merge(Project& project, const MergeSettings& settings);

	enum class DissolveBy : uint8_t
	{
		Channel, // notes and controllers by their MIDI channel; SysEx stays
		Pitch,   // notes and poly pressure by their pitch; other events stay
	};

	enum class DissolveTo : uint8_t
	{
		Tracks, // new MIDI tracks after the track, routed to the same instrument
		Lanes,  // new lanes of the track, below the lanes its parts are on
	};

	// Copies the events of the parts into new parts at the same place, one per channel or pitch found in the parts of
	// a track, on a new track or lane for each, and mutes the parts (reference manual pp. 1121-1122). Events that
	// belong to no channel or pitch go with the first new part of their part, so none is lost; a
	// part with nothing to separate stays as it is. The new parts keep the part's mute, transpose, velocity, root key
	// and Global Transpose setting, on
	// lanes also its name, with the channel or pitch added, and color. optimize shortens new parts on tracks to the
	// bars their events play in; parts on lanes keep the part's place.
	PartChanges dissolve(Project& project, const std::vector<ObjectId>& parts, DissolveBy by, bool optimize,
		DissolveTo to = DissolveTo::Tracks);

	// Combines parts on one track into a single part from the first one's start to the last one's end, named after
	// the earliest, on the lowest lane they are on (reference manual p. 1123). Muted parts are removed and add
	// nothing. Events keep their time; notes are cut where their part ended. The parts' transpose and velocity go into
	// the notes as playback applies them. The new part gets the root key and Global Transpose setting the parts
	// share, or, when they differ, the project root key and follow, and then each note the difference its part's
	// global transposition makes played into its pitch, so that it keeps the pitch it played with (#954). What the
	// notes of a part can't hold stays on a part of its own, for each part on its own (#1064): a pitch beyond 0-127
	// that plays, or one a drum map or the chord track, which map a note before the part's transpose and the global
	// transposition, would play differently, keeps the part's root key and Global Transpose setting, and its
	// transpose too unless the notes hold that alone; a velocity the track's MIDI modifiers would play differently
	// keeps the part's velocity. Each setting makes a new part, on a lane free of the others and of the parts that
	// stay. The track's MIDI modifiers stay on the track, and the copied notes get new ids, so the play probability
	// and velocity variance of a note, drawn from its part's and its own id, draw anew.
	PartChanges bounce(Project& project, const std::vector<ObjectId>& parts);
}
