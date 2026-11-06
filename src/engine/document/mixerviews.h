// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"
#include "engine/schema.h"

#include <string>

// What the mixers hide (#106, docs/features/mixer-window.md): the lower zone's mixer and the mixer windows each have
// their channel visibility and channel type filter, kept in the project as settings, not as undo steps.
namespace gnubase::engine
{
	class Engine;
	class ProjectModel;

	// The mixer type of a channel (g_mixerChannelTypes) of the project: a bus's direction, or its track's type, where a
	// folder's group channel is a group channel. Empty for an id that is neither a channel nor a bus.
	std::string mixerChannelType(const Project& project, ObjectId id);

	// A mixer's view as mixer.get returns it: {mixer, hidden, hiddenTypes}, without the channels that are gone.
	Json mixerViewJson(const Project& project, const std::string& mixer);

	// mixer.get and mixer.set, and the track visibility's commands (trackvisibility.h).
	void registerMixerCommands(ProjectModel& model, Engine& engine);
}
