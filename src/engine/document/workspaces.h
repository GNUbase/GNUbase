// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Workspaces (#163, docs/features/workspaces.md, reference manual pp. 1576-1578): named window layouts, global ones
// for every project in the user setting ui.workspaces and project ones saved with the project, each a copy of the
// user setting ui.layout with the windows that are open and where they are.
namespace gnubase::engine
{
	class Engine;
	class ProjectModel;

	// workspace.get, workspace.add, workspace.update, workspace.activate, workspace.remove, workspace.rename and
	// workspace.move.
	void registerWorkspaceCommands(ProjectModel& model, Engine& engine);
}
