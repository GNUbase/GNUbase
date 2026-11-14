// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"

#include <string>
#include <vector>

// Drum tracks (#182, docs/features/drum-track.md): instrument tracks that play the built-in drum machine, with its
// pads' names and samples in their channel.
namespace gnubase::engine
{
	class Engine;
	class ProjectModel;

	// drum.create, drum.loadPad, drum.setPad, drum.copyPad and drum.resetPad.
	void registerDrumCommands(ProjectModel& model, Engine& engine);

	// The channel's drum machine pads that play something (their instrument isn't Off), from the lowest note up.
	std::vector<int> playingDrumPads(const Channel& channel);

	// A pad's name: its own, else its sample's clip's, else the General MIDI sound's of its note.
	std::string drumPadName(const Project& project, const Channel& channel, int pad);

	// The drum machine's channel of a drum track, or nullptr for any other track.
	const Channel* drumChannelOf(const Project& project, ObjectId track);
}
