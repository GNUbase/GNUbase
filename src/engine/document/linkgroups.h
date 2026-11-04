// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Link groups and VCA faders (#110, docs/features/link-groups.md): linkGroup.add, linkGroup.set and
// linkGroup.remove. channel.set passes changes on to linked channels.
namespace gnubase::engine
{
	class Engine;
	class ProjectModel;

	void registerLinkGroupCommands(ProjectModel& model, Engine& engine);
}
