// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace gnubase::engine
{
	class Engine;
	class ProjectModel;
}

// The controller lanes (#128, reference manual pp. 1157-1188): docs/features/controller-lanes.md describes them.
namespace gnubase::engine::midi
{
	// What a controller lane shows: a property of the notes, or the controller events of a type. The controller
	// types have controller.add's names. articulations is the Articulations lane (#1112, reference manual p. 1287):
	// the directions and the notes' attributes of the track's articulation map, which controller.line doesn't draw.
	inline constexpr std::array<const char*, 10> g_laneTypes = {"velocity", "offVelocity", "probability",
		"velocityVariance", "controller", "pitchBend", "channelPressure", "polyPressure", "program", "articulations"};

	bool isLaneType(const std::string& type);
	// Whether the lane shows a property of the notes rather than controller events.
	bool isNoteLane(const std::string& type);

	// Whether a note of a part plays, by its play probability, and the velocity it plays with, by its velocity
	// variance. Both are the same on every playback and for any number of threads, so rendering stays deterministic,
	// and note.apply makes permanent what plays: a number from 0 to 1 that the part's and the note's ids fix.
	// ponytail: the same choice on every pass of a cycle; a seed per pass would vary it, at the cost of state on the
	// audio thread.
	bool notePlays(const Note& note, ObjectId part);
	uint16_t playedVelocity(const Note& note, ObjectId part);

	// The notes of a part by key, for its poly pressure: find gives the note whose key a poly pressure event presses,
	// of the notes of its pitch and channel the one that sounds at its position, the latest started if several do, or
	// null if none does. The pressure plays where that note plays, after the MIDI modifiers and the transposition
	// (docs/features/midi-modifiers.md). The notes must outlive it.
	class PressedNotes
	{
	public:
		explicit PressedNotes(const std::map<ObjectId, Note>& notes);
		const Note* find(const Controller& pressure) const;

	private:
		// A key's notes by start, and for each the latest end of it and those before it.
		struct Key
		{
			std::vector<const Note*> notes;
			std::vector<time::Ticks> ends;
		};
		std::map<uint16_t, Key> m_keys; // by channel * 128 + pitch
	};

	// Registers note.apply and controller.line.
	void registerControllerLaneCommands(ProjectModel& model, Engine& engine);
}
