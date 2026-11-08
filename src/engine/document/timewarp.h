// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Time warp, Process Tempo, Process Bars, Set Definition from Tempo and copying warp markers (#159,
// docs/features/time-warp.md).
namespace gnubase::engine
{
	class Engine;
	class ProjectModel;

	// tempo.warp, tempo.process, tempo.processBars, event.setDefinitionFromTempo and event.pasteWarpMarkers.
	void registerTimeWarpCommands(ProjectModel& model, Engine& engine);
}
