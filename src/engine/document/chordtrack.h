// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"
#include "engine/midi/harmony.h"
#include "engine/schema.h"

#include <string>
#include <utility>
#include <vector>

// The chord track (#173, #933, reference manual pp. 165, 1319-1345): its JSON and what the tracks that follow it
// play. docs/features/chord-track.md describes it; chordtrack.cpp has its commands, chord.*.
namespace gnubase::engine
{
	struct RenderSnapshot;
	class CommandContext;

	// The chord track as chord.get describes it, chord.changed announces it and the project file keeps it: the chord
	// track (0 for none); its chord events in timeline order, positions in the track's time base and start in samples,
	// with their chord, scale and voicing (pitches) and, made from audio, the audio event and the chords detected; its
	// scale events alike; its settings; and the tracks that follow it, with their settings and source.
	Json chordJson(const Project& project);
	// Reads it back into a project that has the chord track, as the chord commands would make it: unknown chords are
	// undefined, unknown scales, events before the project start and tracks that can't follow are left out, and
	// unknown settings are their defaults.
	void chordFromJson(Project& project, const Json& json);

	// The harmonies of a chord progression, by position in ticks, each until the next.
	using Progression = std::vector<std::pair<time::Ticks, midi::Harmony>>;

	// The chord track's progression: its chord events with their scales and voicings.
	Progression chordProgression(const Project& project);

	// What a track's notes play as it follows the chord track (reference manual p. 1339): the pitch a note at a
	// position (ticks) plays, mapped from the harmony the track's notes were played in to the chord track's there,
	// with the track's voicings and the chord track's Shift Chords. Nothing follows a muted chord track.
	class ChordFollower
	{
	public:
		ChordFollower(const Project& project, ObjectId track);
		bool active() const { return !m_mode.empty(); }
		int pitch(int pitch, time::Ticks at) const;

	private:
		std::string m_mode; // empty while the track doesn't follow
		int m_voice = 0;
		time::Ticks m_shift = 0;
		Progression m_from;
		Progression m_to;
	};

	// What the chord track plays (#933): Live Input of the tracks that have it, and with Acoustic Feedback the chord
	// events' voicings on the audition track. Adds them to the snapshot's channels.
	void renderChordTrack(const Project& project, RenderSnapshot& snapshot);

	// A recorded note, by position in ticks.
	struct ChordNote
	{
		time::Ticks start = 0;
		time::Ticks end = 0;
		int pitch = 60;
	};

	// Recording chord events (reference manual p. 1345): the chords the notes make replace the chord events from
	// `from` to `to`, as Create Chord Events with bass notes and tensions makes them. Announces chord.changed.
	void recordChordEvents(Project& project, const std::vector<ChordNote>& notes, time::Ticks from, time::Ticks to,
		CommandContext& context);
}
