// SPDX-License-Identifier: GPL-3.0-or-later
// The chord track's music theory (#173, reference manual pp. 1319-1345): chord symbols and their text input, scales,
// recognizing chords, basic voicings and the Follow Chord Track modes over reference progressions.
#include "engine/midi/harmony.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace gnubase::engine::midi;

namespace
{
	ChordSymbol chord(const std::string& text)
	{
		const std::optional<ChordSymbol> parsed = parseChord(text);
		INFO(text);
		REQUIRE(parsed.has_value());
		return parsed.value_or(ChordSymbol());
	}

	std::string recognized(const std::vector<int>& pitches, bool bass = true, bool tensions = true)
	{
		const std::optional<ChordSymbol> found = recognizeChordSymbol(pitches, bass, tensions);
		return found.has_value() ? chordName(*found) : std::string("none");
	}

	std::string scales(const std::string& text)
	{
		const std::optional<Scale> scale = parseScale(text);
		return scale.has_value() ? scaleName(*scale) : std::string("none");
	}

	std::string automatic(const std::string& text)
	{
		const std::optional<Scale> scale = automaticScale(chord(text));
		return scale.has_value() ? scaleName(*scale) : std::string("none");
	}

	Harmony harmony(const std::string& text, const std::vector<int>& previous = {})
	{
		Harmony result;
		result.chord = chord(text);
		result.scale = automaticScale(result.chord).value_or(Scale());
		result.voicing = voiceChord(result.chord, VoicingSettings(), previous);
		return result;
	}

	std::vector<int> follow(const std::vector<int>& pitches, const std::string& mode, const Harmony& from,
		const Harmony& to)
	{
		std::vector<int> result;
		result.reserve(pitches.size());
		for (const int pitch : pitches)
			result.push_back(followPitch(pitch, mode, from, to));
		return result;
	}
}

TEST_CASE("chord symbols: the chord editor's text input and the names of chord events", "[unit]")
{
	const ChordSymbol c = chord("C");
	CHECK(c.root == 0);
	CHECK(c.type == "maj");
	CHECK(c.tensions.empty());
	CHECK(c.bass == -1);
	CHECK(chord("Dm7").root == 2);
	CHECK(chord("Dm7").type == "min");
	CHECK(chord("Dm7").tensions == std::vector<std::string>{"7"});
	CHECK(chordName(chord("Bbmaj7")) == "A#maj7");
	CHECK(chordName(chord("C9")) == "C7(9)"); // 9, 11 and 13 bring the seventh
	CHECK(chordName(chord("Cmaj9")) == "Cmaj7(9)");
	CHECK(chordName(chord("C add 9")) == "Cadd9");
	CHECK(chordName(chord("C7#9")) == "C7(#9)");
	CHECK(chordName(chord("Cmin")) == "Cm");
	CHECK(chordName(chord("Csus")) == "Csus4");
	CHECK(chordName(chord("C+")) == "Caug");
	CHECK(chordName(chord("Cm7b5")) == "Cm7b5");
	CHECK(chord("Cm7b5").type == "dim");
	CHECK(chord("C/E").bass == 4);
	CHECK(chordName(chord("C/C")) == "C");
	CHECK(chord("X").root == -1);
	CHECK(chord("").root == -1);
	CHECK(chordName(ChordSymbol()) == "X");
	CHECK(!parseChord("H").has_value());
	CHECK(!parseChord("Cxyz").has_value());
	CHECK(!parseChord("C/").has_value());
	// Every name reads back as itself.
	for (const std::string name : {"C", "C#m", "Dm7", "Ebmaj7", "Fdim", "Gaug", "Asus2", "Bsus4", "C6", "Cm6", "C7(b9)",
			 "C7(9,13)", "Cadd9", "Cadd(9,#11)", "Am7/G", "Cm7b5", "Cmmaj7"})
	{
		INFO(name);
		CHECK(chordName(chord(chordName(chord(name)))) == chordName(chord(name)));
	}
	CHECK(chordIntervals(chord("Cmaj7")) == std::vector<int>{0, 4, 7, 11});
	CHECK(chordIntervals(chord("G7(b9)")) == std::vector<int>{0, 4, 7, 10, 1});
	CHECK(chordIntervals(chord("C13")) == std::vector<int>{0, 4, 7, 10, 9});
	CHECK(chordIntervals(chord("Dm/F")) == std::vector<int>{0, 3, 7});
	CHECK(chordIntervals(chord("X")).empty());
}

TEST_CASE("scales: their names and the automatic scales of chords", "[unit]")
{
	CHECK(scaleName({2, ScaleType::Dorian}) == "D dorian");
	CHECK(scales("F# lydian") == "F# lydian");
	CHECK(scales("Bb major") == "A# major");
	CHECK(!parseScale("C bebop").has_value());
	CHECK(!parseScale("Cmajor").has_value());
	CHECK(automatic("C") == "C major");
	CHECK(automatic("G7") == "G mixolydian");
	CHECK(automatic("Fmaj7(#11)") == "F lydian");
	CHECK(automatic("Am") == "A minor");
	CHECK(automatic("Dm7") == "D dorian");
	CHECK(automatic("Cmmaj7") == "C melodicMinor");
	CHECK(automatic("Bdim") == "B locrian");
	CHECK(automatic("Caug") == "C wholeTone");
	CHECK(automatic("Dsus4") == "D major");
	CHECK(automatic("X") == "none");
}

TEST_CASE("recognizing the chords of MIDI notes", "[unit]")
{
	CHECK(recognized({60, 64, 67}) == "C");
	CHECK(recognized({64, 67, 72}) == "C/E"); // the lowest note is the bass note
	CHECK(recognized({64, 67, 72}, false) == "C");
	CHECK(recognized({57, 60, 64, 67}) == "Am7"); // not C6/A: on the lowest note
	CHECK(recognized({60, 64, 67, 69}) == "C6");
	CHECK(recognized({60, 64, 67, 71}) == "Cmaj7");
	CHECK(recognized({60, 64, 67, 71}, true, false) == "C"); // tensions left out
	CHECK(recognized({62, 65, 69, 72, 76}) == "Dm7(9)");
	CHECK(recognized({55, 59, 62, 65}) == "G7");
	CHECK(recognized({59, 62, 65}) == "Bdim");
	CHECK(recognized({60, 62}) == "none");
	CHECK(recognized({60, 61, 62}) == "none");
	CHECK(recognized({}) == "none");
}

TEST_CASE("basic voicings: close position from C3, the bass note an octave lower, adaptive inversions", "[unit]")
{
	CHECK(voiceChord(chord("C"), {}) == std::vector<int>{48, 60, 64, 67});
	CHECK(voiceChord(chord("G"), {}) == std::vector<int>{55, 67, 71, 74});
	CHECK(voiceChord(chord("Cmaj7/E"), {}) == std::vector<int>{52, 60, 64, 67, 71});
	CHECK(voiceChord(chord("C"), {1, true}) == std::vector<int>{60, 72, 76, 79});
	// Adaptive: G after C takes the inversion nearest to C's upper notes; without adaptive, it doesn't.
	const std::vector<int> c = voiceChord(chord("C"), {});
	CHECK(voiceChord(chord("G"), {0, true}, c) == std::vector<int>{55, 59, 62, 67});
	CHECK(voiceChord(chord("G"), {0, false}, c) == std::vector<int>{55, 67, 71, 74});
	CHECK(voiceChord(chord("F"), {0, true}, c) == std::vector<int>{53, 60, 65, 69});
	CHECK(voiceChord(chord("X"), {}).empty());
}

TEST_CASE("Follow Chord Track modes map a C major chord through the I-vi-IV-V progression", "[unit]")
{
	const std::vector<int> triad = {60, 64, 67};
	const Harmony c = harmony("C");
	const Harmony am = harmony("Am");
	const Harmony f = harmony("F");
	const Harmony g = harmony("G");
	SECTION("Root Notes moves by the step between the roots, the nearer way")
	{
		CHECK(follow(triad, "rootNotes", c, am) == std::vector<int>{57, 61, 64});
		CHECK(follow(triad, "rootNotes", c, f) == std::vector<int>{65, 69, 72});
		CHECK(follow(triad, "rootNotes", c, g) == std::vector<int>{55, 59, 62});
	}
	SECTION("Chords moves to the nearest notes of the new chord")
	{
		CHECK(follow(triad, "chords", c, am) == std::vector<int>{57, 60, 64});
		CHECK(follow(triad, "chords", c, f) == std::vector<int>{65, 69, 72});
		CHECK(follow(triad, "chords", c, g) == std::vector<int>{55, 59, 62});
		CHECK(follow({69}, "chords", c, harmony("Cm")) == std::vector<int>{67});
	}
	SECTION("Scales moves to the nearest notes of the new scale")
	{
		// A melody over C, C D E F G, over Am (A minor) and over D7 (D mixolydian).
		const std::vector<int> melody = {60, 62, 64, 65, 67};
		CHECK(follow(melody, "scales", c, am) == std::vector<int>{57, 59, 60, 62, 64});
		CHECK(follow(melody, "scales", c, harmony("D7")) == std::vector<int>{62, 64, 66, 67, 69});
	}
	SECTION("Chords & Scales keeps chord notes the same notes and others the same degree")
	{
		CHECK(follow({60, 64, 67, 62, 65}, "chordsAndScales", c, am) == std::vector<int>{57, 60, 64, 59, 62});
		// Over C minor the third is minor and the sixth degree a minor sixth; Chords takes the nearest chord note.
		CHECK(follow({64, 69}, "chordsAndScales", c, harmony("Cm")) == std::vector<int>{63, 68});
	}
	SECTION("Voicings plays the same voices of the new chord's voicing")
	{
		const Harmony gAfterC = harmony("G", c.voicing);
		CHECK(follow({48, 60, 64, 67, 72}, "voicings", c, gAfterC) == std::vector<int>{55, 59, 62, 67, 71});
		CHECK(follow({48, 60, 64, 67}, "voicings", c, harmony("Cmaj7")) == std::vector<int>{48, 60, 64, 67});
		CHECK(follow({48, 60, 64, 67, 71}, "voicings", harmony("Cmaj7"), c) == std::vector<int>{48, 60, 64, 67, 67});
	}
	SECTION("nothing moves over an undefined chord or with Off")
	{
		CHECK(follow(triad, "chords", c, harmony("X")) == triad);
		CHECK(follow(triad, "chords", harmony("X"), am) == triad);
		CHECK(follow(triad, "off", c, am) == triad);
	}
}
