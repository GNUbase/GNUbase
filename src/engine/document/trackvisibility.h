// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"

#include <string>
#include <vector>

// What the project window's track list hides (#166, docs/features/track-visibility.md, reference manual pp. 80-82):
// hidden tracks, the visibility configurations, and the mixers whose channel visibility follows the tracks'; and its
// track heights and folded folders (#491, docs/features/track-list.md, pp. 217, 225).
namespace gnubase::engine
{
	class CommandContext;
	class Engine;
	class ProjectModel;

	// After mixer.set changed the mixer's view: if the mixer follows the track visibility (the lower zone's, and the
	// synced window), the tracks of its track channels take its visibility, and the other mixer that follows takes
	// theirs. Emits trackVisibility.changed and mixer.changed for what changed.
	void followMixer(Project& project, const std::string& mixer, CommandContext& context);

	// After every edit, with the project before it: a track that joins or leaves a hidden folder, that goes or that
	// gets a channel changes what trackVisibility.get returns without a list changing, as hidden and the
	// configurations keep their folders without the tracks inside them (#1475, #1533). Emits trackVisibility.changed
	// when what it returns for the visibility the edit left differs, and syncs the mixers that follow in any case
	// (#1539).
	void followTrackFolders(Project& project, CommandContext& context, const Project& before);
	// The same after undo or redo steps, with what trackVisibility.get returned before them (#1475).
	void followTrackFolders(Project& project, CommandContext& context, const Json& before);

	// What a loaded project keeps of its trackVisibility, once its tracks are there: the checks trackVisibility.set
	// makes, so an unknown sync, configurations without a name or with a name twice, an active name that isn't there
	// and ids that aren't tracks don't come in, and hidden folders hide the tracks inside them (#906). The
	// mixers that follow then hide those tracks' channels (#1366).
	void sanitizeTrackVisibility(Project& project);

	// As trackLayout.get returns it: the heights and folded folders of the tracks that are in the project. The project
	// keeps those of removed tracks too, for undo.
	Json trackLayoutJson(const Project& project);

	// As trackVisibility.get returns it: without the tracks that are gone, and whether the tracks hidden differ from
	// the active configuration's.
	Json trackVisibilityJson(const Project& project);
	// The tracks the project window hides, sorted: hidden with the tracks inside its folders, nested ones too.
	std::vector<ObjectId> hiddenTracks(const Project& project);

	// trackVisibility.get and trackVisibility.set.
	void registerTrackVisibilityCommands(ProjectModel& model, Engine& engine);
	// trackLayout.get and trackLayout.set: the track list's track heights and folded folders (#491).
	void registerTrackLayoutCommands(ProjectModel& model, Engine& engine);
}
