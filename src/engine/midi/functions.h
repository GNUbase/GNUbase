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
	};

	const char* scaleTypeName(ScaleType type);
	std::optional<ScaleType> scaleTypeFromName(const std::string& name);
	std::vector<std::string> scaleTypeNames();

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
		std::vector<ObjectId> tracks; // the MIDI and instrument tracks to merge; empty: all of them
		ObjectId destination = 0;     // a MIDI or instrument track; 0 adds a MIDI track
		bool eraseDestination = false;
		bool includeChase = false;
		std::string name; // the new part's; empty: the destination track's
	};

	// Copies the notes and controllers that start in the range on the tracks into a new part between start and end
	// on the destination track. Events are in the range when their position in samples is, as in playback; the new
	// part starts at the first of them if that is earlier than start in ticks. Notes keep their lengths, cut where
	// their part ends. eraseDestination first removes the destination track's parts that lie in the range and the
	// events of other parts that start in it. includeChase adds the last controller value of every controller, program
	// and channel before start at the new part's start. A new destination track goes to the end of the track list,
	// routed to the instrument of the first track merged.
	PartChanges merge(Project& project, const MergeSettings& settings);

	enum class DissolveBy : uint8_t
	{
		Channel, // notes and controllers by their MIDI channel; SysEx stays
		Pitch,   // notes and poly pressure by their pitch; other events stay
	};

	// Moves the events of the parts into new parts on new MIDI tracks, one track per channel or pitch found in the
	// parts of a track, added after it and routed to the same instrument. Events that belong to no channel or pitch
	// stay in their part; parts left empty are removed. optimize shortens the new parts to the bars their events
	// play in.
	PartChanges dissolve(Project& project, const std::vector<ObjectId>& parts, DissolveBy by, bool optimize);

	// Combines parts on one track into a single part from the first one's start to the last one's end, named after
	// the earliest. Events keep their time; notes are cut where their part ended.
	PartChanges bounce(Project& project, const std::vector<ObjectId>& parts);
}
