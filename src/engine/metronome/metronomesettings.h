// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/schema.h"
#include "engine/time/signaturemap.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// The metronome's settings (#101): when it clicks, its sounds and where they go, the count-in and the click patterns.
// They belong to the project, are saved with it and are not undo steps, like the quantize settings.
namespace gnubase::engine::metronome
{
	using ObjectId = uint64_t;

	// Accents, from 1 (the sound of a bar's first beat) to 4. A pattern's clicks each use one of them, or none.
	inline constexpr int g_accents = 4;
	inline constexpr int g_maxClicks = 64; // clicks in a pattern
	inline constexpr int g_maxCountInBars = 16;
	inline constexpr double g_minLevel = -144.0; // dB, silence
	inline constexpr double g_maxLevel = 6.0;
	inline constexpr double g_minFrequency = 20.0; // Hz
	inline constexpr double g_maxFrequency = 20000.0;

	// The built-in click sound of an accent: a short sine tone.
	struct Beep
	{
		double frequency = 1000.0; // Hz
		double level = 0.0;        // dB
	};

	// A sample file as the click sound of an accent. Without a path, or if the file can't be read, the beep plays.
	struct Sample
	{
		std::string path; // UTF-8
		double level = 0.0;
	};

	// The note the MIDI click sends for an accent.
	struct MidiNote
	{
		int note = 76;         // 0-127
		double velocity = 1.0; // 0-1
	};

	enum class Sounds : uint8_t
	{
		Beeps,
		Samples,
	};

	// The time signature and tempo a count-in follows.
	enum class CountInSignature : uint8_t
	{
		Start,   // the signature and tempo at the position where playback starts, for every count-in bar
		Project, // the bars before the start as the project has them, with their signatures and tempo
		Custom,  // customSignature at the tempo at the start
	};

	// The clicks of a bar, evenly spread over it, each with an accent from 1 to 4 or 0 for none.
	struct ClickPattern
	{
		time::TimeSignature signature;
		std::vector<uint8_t> accents; // 1 to g_maxClicks clicks
	};

	struct MetronomeSettings
	{
		bool click = false;         // the metronome click is on
		bool whilePlaying = true;   // it clicks during playback
		bool whileRecording = true; // it clicks while recording (#96, #97)

		bool audio = true;             // the audio click
		std::vector<ObjectId> outputs; // the output busses it plays through; none: the main mix
		double level = 0.0;            // dB, on top of each accent's level
		Sounds sounds = Sounds::Beeps;
		std::array<Beep, g_accents> beeps{{{1500.0, 0.0}, {1000.0, -6.0}, {1000.0, -12.0}, {800.0, -18.0}}};
		std::array<Sample, g_accents> samples;

		bool midi = false;        // the MIDI click
		ObjectId destination = 0; // the instrument channel it plays, or 0
		int channel = 1;          // MIDI channel 1-16
		std::array<MidiNote, g_accents> notes{{{76, 1.0}, {77, 0.75}, {77, 0.5}, {77, 0.25}}};

		bool countIn = false; // starts that count in (transport.play with countIn) do
		int countInBars = 1;
		CountInSignature countInSignature = CountInSignature::Start;
		time::TimeSignature customSignature;

		// Patterns of the signatures that don't click the default one, sorted by numerator, then denominator.
		std::vector<ClickPattern> patterns;
	};

	bool operator==(const MetronomeSettings& a, const MetronomeSettings& b);
	inline bool operator!=(const MetronomeSettings& a, const MetronomeSettings& b)
	{
		return !(a == b);
	}

	// A click per beat: the first one with accent 1, the others with accent 2.
	ClickPattern defaultPattern(time::TimeSignature signature);
	// The signature's pattern in settings, or its default pattern.
	ClickPattern patternFor(const MetronomeSettings& settings, time::TimeSignature signature);

	// The settings as metronome.get returns them.
	Json toJson(const MetronomeSettings& settings);
	// The schema of what metronome.set takes; every property is optional.
	Json settingsSchema();

	// Applies what metronome.set takes to settings: the given properties, and in sounds arrays (beeps, samples, notes)
	// the given properties of each accent, by index. Only checks values; whether outputs, destination and sample files
	// exist is up to the caller. strict: throws CommandError for an invalid value and changes nothing; otherwise, as
	// for a project file, skips invalid values and keeps the rest.
	void apply(MetronomeSettings& settings, const Json& json, bool strict);

	// Sets a signature's pattern, or with no accents goes back to its default pattern. Throws CommandError for
	// patterns of more than g_maxClicks clicks or accents above g_accents.
	void setPattern(MetronomeSettings& settings, time::TimeSignature signature, const Json& accents);
}
