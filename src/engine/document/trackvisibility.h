// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"

#include <string>

// What the project window's track list hides (#166, docs/features/track-visibility.md, reference manual pp. 80-82):
// hidden tracks, the visibility configurations, and the mixers whose channel visibility follows the tracks'.
namespace gnubase::engine
{
	class CommandContext;
	class Engine;
	class ProjectModel;

	// After mixer.set changed the mixer's view: if the mixer follows the track visibility (the lower zone's, and the
	// synced window), the tracks of its track channels take its visibility, and the other mixer that follows takes
	// theirs. Emits trackVisibility.changed and mixer.changed for what changed.
	void followMixer(Project& project, const std::string& mixer, CommandContext& context);

	// trackVisibility.get and trackVisibility.set.
	void registerTrackVisibilityCommands(ProjectModel& model, Engine& engine);
}
