// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"

#include <vector>

// Link groups and VCA faders (#110, docs/features/link-groups.md): linkGroup.add, linkGroup.set and
// linkGroup.remove. channel.set passes changes on to linked channels.
namespace gnubase::engine
{
	class CommandContext;
	class Engine;
	class ProjectModel;

	void registerLinkGroupCommands(ProjectModel& model, Engine& engine);

	// Takes the channels that are gone out of their link groups, and VCA faders that are gone out of the groups they
	// control: a group that loses its VCA fader links the volume again (reference manual p. 518), one that loses its
	// last channel goes. Emits linkGroup.changed and linkGroup.removed.
	void removeFromLinkGroups(Project& project, const std::vector<ObjectId>& gone, CommandContext& context);

	// For a project just loaded, whose link groups are otherwise valid: VCA faders that would control themselves, in
	// the group they control or through others, leave the groups they control, from the first group on, and groups
	// without channels go.
	void dropLinkGroupLoops(Project& project);
}
