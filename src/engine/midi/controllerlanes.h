// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"

#include <array>
#include <cstdint>
#include <string>

namespace gnubase::engine
{
	class Engine;
	class ProjectModel;
}

// The controller lanes (#128, reference manual pp. 1157-1188): docs/features/controller-lanes.md describes them.
namespace gnubase::engine::midi
{
	// What a controller lane shows: a property of the notes, or the controller events of a type. The controller
	// types have controller.add's names.
	inline constexpr std::array<const char*, 9> g_laneTypes = {"velocity", "offVelocity", "probability",
		"velocityVariance", "controller", "pitchBend", "channelPressure", "polyPressure", "program"};

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

	// Registers note.apply and controller.line.
	void registerControllerLaneCommands(ProjectModel& model, Engine& engine);
}
