// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The key editor's chord editing (#127, reference manual pp. 1215-1218): chord types, recognizing the chord of some
// pitches and changing their voicing. docs/features/key-editor.md describes them.
namespace gnubase::engine::midi
{
	// A chord type: its name as note.voicing takes it, the suffix after the root's name, and its intervals from the
	// root in semitones.
	struct ChordType
	{
		const char* name;
		const char* suffix;
		std::vector<int> intervals;
	};

	// The 3-note chords (triads) and the 4-note chords of the Chord Editing section, in that order.
	const std::vector<ChordType>& chordTypes();
	const ChordType* chordType(const std::string& name);

	// A chord: its root's pitch class (0 is C) and type.
	struct Chord
	{
		int root = 0;
		const ChordType* type = nullptr;

		// Such as "C", "F#m" or "A#maj7".
		std::string name() const;
	};

	// The chord the pitches make, in any octave and order (arpeggiated notes too), or nothing if they make none of
	// the types or no type exactly. Of several roots that fit, the lowest note's.
	std::optional<Chord> recognizeChord(const std::vector<int>& pitches);

	// The pitches transposed to fit the type, on the root of the chord they make, or else of the lowest note: from the
	// lowest up, each moves to the nearest pitch of a chord tone that no lower note took since every tone last had one
	// (down on a tie). They go back in the order given and stay within 0-127.
	std::vector<int> fitChord(const std::vector<int>& pitches, const ChordType& type);

	// How note.voicing changes a chord's voicing.
	enum class Voicing : uint8_t
	{
		InvertUp,   // the lowest note goes up by octaves until it is the highest
		InvertDown, // the highest note goes down by octaves until it is the lowest
		Drop2,      // the second highest note goes down an octave
		Drop3,      // the third highest
		Drop24,     // the second and fourth highest
	};
	std::optional<Voicing> voicingFromName(const std::string& name);

	// The pitches revoiced, in the order given, so each stays its note's. Nothing when there are too few notes for
	// the voicing or a pitch would leave 0-127.
	std::optional<std::vector<int>> revoice(const std::vector<int>& pitches, Voicing voicing);

	// A pitch's name, such as "C3" for middle C (60), as the editors show it.
	std::string pitchName(int pitch);
}
