// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"
#include "engine/midi/harmony.h"
#include "engine/schema.h"

#include <string>
#include <utility>
#include <vector>

// The chord track (#173, reference manual pp. 165, 1319-1345): its JSON and what the tracks that follow it play.
// docs/features/chord-track.md describes it; chordtrack.cpp has its commands, chord.*.
namespace gnubase::engine
{
	// The chord track as chord.get describes it, chord.changed announces it and the project file keeps it: the chord
	// track (0 for none); its chord events in timeline order, positions in the track's time base and start in samples,
	// with their chord, scale and voicing (pitches); its scale events alike; octaveOffset, adaptiveVoicings and
	// automaticScales; and the tracks that follow it, with their mode and source.
	Json chordJson(const Project& project);
	// Reads it back into a project that has the chord track.
	void chordFromJson(Project& project, const Json& json);

	// The harmonies of a chord progression, by position in ticks, each until the next.
	using Progression = std::vector<std::pair<time::Ticks, midi::Harmony>>;

	// The chord track's progression: its chord events with their scales and voicings.
	Progression chordProgression(const Project& project);

	// What a track's notes play as it follows the chord track (reference manual p. 1339): the pitch a note at a
	// position (ticks) plays, mapped from the harmony the track's notes were played in to the chord track's there.
	class ChordFollower
	{
	public:
		ChordFollower(const Project& project, ObjectId track);
		bool active() const { return !m_mode.empty(); }
		int pitch(int pitch, time::Ticks at) const;

	private:
		std::string m_mode; // empty while the track doesn't follow
		Progression m_from;
		Progression m_to;
	};
}
