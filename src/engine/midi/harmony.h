// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/midi/functions.h"

#include <optional>
#include <string>
#include <vector>

// The music theory of the chord track (#173, reference manual pp. 1319-1345): chord symbols with a root, a type,
// tensions and a bass note, scales, voicings, recognizing the chord of some pitches and mapping a pitch from one chord
// to another as a track that follows the chord track does. docs/features/chord-track.md describes them.
namespace gnubase::engine::midi
{
	// A chord event's chord: a root (pitch class, 0 is C; -1 for an undefined chord, "X"), a type ("maj", "min",
	// "dim", "aug", "sus2" or "sus4"), tensions in the order of chordTensions() and a bass note (-1: the root).
	struct ChordSymbol
	{
		int root = -1;
		std::string type = "maj";
		std::vector<std::string> tensions;
		int bass = -1;
	};

	bool operator==(const ChordSymbol& a, const ChordSymbol& b);

	const std::vector<std::string>& chordTypeNames();
	// "6", "7" (the minor seventh), "maj7", "b9", "9", "#9", "11", "#11", "b13" and "13".
	const std::vector<std::string>& chordTensions();

	// The chord a text names, as the chord editor's text input takes it: a root (C to B with # or b), a type (m or min,
	// maj, dim, aug or +, sus2, sus4 or sus), tensions (6, 7, maj7, 9, 11 and 13, of which 9, 11 and 13 add the
	// seventh unless after add, maj or a sixth or seventh; b9, #9, #11, b13; m7b5 is dim with the seventh), and a bass
	// note after a slash, such as "Cmaj7", "Dm7(9)", "G7(b9,b13)" or "C/E". "X" and "" are an undefined chord.
	std::optional<ChordSymbol> parseChord(const std::string& text);
	// Its name, which parseChord takes back: such as "Cmaj7", "Dm7(9)", "Cadd(9)", "C/E", or "X" when undefined.
	std::string chordName(const ChordSymbol& chord);

	// The chord's notes as intervals from its root, 0 to 11, without repeats: its type's from the lowest up, then its
	// tensions' in their order. The bass note isn't one of them. None for an undefined chord.
	std::vector<int> chordIntervals(const ChordSymbol& chord);

	// Scales are the transpose function's (functions.h). parseScale takes a root and a scale type's name, such as
	// "D dorian", and scaleName gives it back.
	std::optional<Scale> parseScale(const std::string& text);
	std::string scaleName(const Scale& scale);

	// The scale an automatic scale event gives a chord (reference manual p. 1333, Automatic Scales): major, with the
	// seventh mixolydian and with #11 lydian; minor, with maj7 melodic minor and with the sixth, seventh or 13 dorian;
	// diminished locrian; augmented whole tone; sus2 and sus4 major,
	// with the seventh mixolydian. None for an undefined chord.
	std::optional<Scale> automaticScale(const ChordSymbol& chord);

	// The chord that the pitches make, in any octave and order, or none: at least three pitch classes, which a chord
	// type on one of them holds with the others as its tensions. Of several, the one with the fewest tensions, then
	// the one on the lowest note, then the first type. Without tensions, extra notes are left out; with bass, a lowest
	// note that isn't the root is the bass note.
	std::optional<ChordSymbol> recognizeChordSymbol(const std::vector<int>& pitches, bool bass, bool tensions);

	// Basic voicings (reference manual pp. 1333-1335): the bass note (or root) in the octave from C2 (48), the chord's
	// notes above it in close position from C3 (60), both moved by octaveOffset octaves. With adaptive and the voicing
	// of the chord before, the upper notes take the inversion, within C2 + 4 to C5 (84) moved alike, that moves them
	// the least. From the lowest up; none for an undefined chord.
	struct VoicingSettings
	{
		int octaveOffset = 0; // -3 to 3
		bool adaptive = true;
	};
	std::vector<int> voiceChord(
		const ChordSymbol& chord, const VoicingSettings& settings, const std::vector<int>& previous = {});

	// How a track follows the chord track (reference manual p. 1340): "off", "chordsAndScales", "chords", "scales",
	// "rootNotes" or "voicings".
	const std::vector<std::string>& followModeNames();

	// What a pitch played over one chord (from) plays over another (to), as the mode maps it. Every mode but voicings
	// first moves it by the step between the roots, the nearer way (-5 to 6 semitones). rootNotes stops there; chords
	// moves it to the nearest note of the new chord and scales to the nearest of the new scale (down on a tie);
	// chordsAndScales keeps the notes of the old chord the same notes of the new one (root, third and so on) and the
	// others the same degree of the new scale. voicings plays the voice of the new chord's voicing that the pitch is in
	// the old one's, in the same octave (a voice beyond the new voicing's plays its highest); a pitch outside the
	// voicing maps as chords does. Unchanged when either chord is undefined.
	struct Harmony
	{
		ChordSymbol chord;
		Scale scale;               // automaticScale(chord) or a scale event's
		std::vector<int> voicing;  // voiceChord(chord, ...)
	};
	int followPitch(int pitch, const std::string& mode, const Harmony& from, const Harmony& to);
}
