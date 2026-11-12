// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"
#include "engine/document/rendersnapshot.h"
#include "engine/midi/harmony.h"
#include "engine/schema.h"

#include <string>
#include <vector>

// The chord pads and the chord assistant (#174, reference manual pp. 1346-1391): their JSON, the voicings the pads
// play and the music theory behind the pad modifiers, the scale-based presets and the assistant's suggestions.
// docs/features/chord-pads.md describes them; chordpads.cpp has their commands, chordPads.*.
namespace gnubase::engine
{
	// The chord pads as chordPads.get describes them, chordPads.changed announces them and the project file keeps them.
	Json chordPadsJson(const Project& project);
	// Reads them back; what the JSON leaves out keeps its default.
	void chordPadsFromJson(Project& project, const Json& json);

	// The pitches each pad plays, from the lowest up, empty for an unassigned pad: the basic voicing of its chord moved
	// by the player's octave offset and inverted by its voicing (reference manual p. 1376), or, for a pad with adaptive
	// voicing while another pad is the adaptive voicing reference, the inversion nearest to the reference's voicing.
	std::vector<std::vector<int>> chordPadVoicings(const ChordPads& pads);

	// What a MIDI input's notes on the pads' remote keys play (reference manual p. 1386), for the render snapshot.
	ChordPadKeys chordPadKeys(const ChordPads& pads);

	// The music theory of the pads and the assistant.
	namespace chordpads
	{
		// The scale-based presets (reference manual p. 1370): "major", "minor", "dorian", "phrygian", "lydian",
		// "mixolydian" and "locrian".
		const std::vector<std::string>& presetNames();
		// The preset's chords on a root (pitch class): the triads on the seven degrees of its scale.
		std::vector<midi::ChordSymbol> presetChords(const std::string& preset, int root);

		// The chord's scale degree in the key (root, and a preset's scale such as "major", "minor" or "dorian") as a
		// roman numeral, such as "V", "ii", "vii°" or, outside the scale, "bVII"; lower case for minor and diminished
		// chords, ° for diminished, + for augmented. Empty for an undefined chord.
		std::string degreeName(const midi::ChordSymbol& chord, int key, const std::string& scale = "major");
		// The same in the Nashville Number System (reference manual p. 1390): the degree as a number, such as "4",
		// "2m", "7°" or "b7".
		std::string nashvilleName(const midi::ChordSymbol& chord, int key, const std::string& scale = "major");

		// The chord with more (steps > 0) or fewer tensions (reference manual p. 1351): each step adds the next of its
		// type's tensions (major: maj7, or 7 on the key's fifth degree, then 9 and 13; minor: 7, 9, 11; others: 7) or
		// takes the last one away.
		midi::ChordSymbol withTensions(const midi::ChordSymbol& chord, int steps, int key);

		// The chord moved by semitones: its root and its bass note.
		midi::ChordSymbol transposed(const midi::ChordSymbol& chord, int semitones);

		// A voicing (bass note, then the upper notes from the lowest up) with its upper notes inverted: each step up
		// moves the lowest upper note an octave up, each step down the highest an octave down.
		std::vector<int> inverted(std::vector<int> voicing, int steps);

		// The List assistant's suggestions for the chord after a reference chord (reference manual p. 1373): chords on
		// the twelve roots, major, minor, diminished, 7, maj7 and m7, without the reference. Each has its usefulness in
		// the key (root, and a preset's scale): 0 (common: all its notes are in the key's scale), 1 (adventurous: one
		// isn't) or 2 (far: more aren't). They come by level, the common ones first. Of one level, algorithm "cadence"
		// orders them by how strongly they follow it (a fifth down, a fifth up, the relative chords, a second, a third,
		// then by the distance on the circle of fifths); "commonNotes" keeps those that share exactly commonNotes notes
		// with it.
		struct Suggestion
		{
			midi::ChordSymbol chord;
			int level = 0;
		};
		std::vector<Suggestion> suggestions(const midi::ChordSymbol& reference, int key, const std::string& scale,
			const std::string& algorithm, int commonNotes);
	}
}
