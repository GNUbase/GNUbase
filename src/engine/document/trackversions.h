// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"

#include <string>

// Track versions (#165, docs/features/track-versions.md): trackVersion.add, trackVersion.duplicate, trackVersion.set,
// trackVersion.remove, trackVersion.activate and trackVersion.assignCommonId.
namespace gnubase::engine
{
	class Engine;
	class ProjectModel;

	void registerTrackVersionCommands(ProjectModel& model, Engine& engine);

	// An ID that no version of any track has: versions made together get one.
	int newVersionId(const Project& project);

	// vN, N one more than the number of the track's versions, or the next number none of their names has.
	std::string newVersionName(const Track& track);

	// A copy of a track version with its events and parts, for track: new ids, its audio events in the copies of their
	// parts, joined by the copies of their crossfades and in the same stack order. A shared copy among its MIDI parts
	// stays one, with the notes and controllers it shares, as track.duplicate copies them.
	TrackVersion copyVersion(Project& project, const TrackVersion& version, ObjectId track);
}
