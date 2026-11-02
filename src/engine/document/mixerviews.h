// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"

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

	// mixer.get and mixer.set.
	void registerMixerCommands(ProjectModel& model, Engine& engine);
}
