// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"

#include <vector>

// Hitpoints and slices (#143, docs/features/hitpoints.md): finding where sounds start in a clip, editing and filtering
// those positions, and making slices, events, regions, markers, a groove and MIDI notes of them.
namespace gnubase::engine
{
	class Engine;
	class JobContext;
	class ProjectModel;

	// The onsets of a media file as hitpoints, intensities relative to the sharpest. Reads the file; throws
	// CommandError if it can't. With a job, checks for job.cancel and reports its progress between blocks.
	std::vector<Hitpoint> detectHitpoints(const MediaFile& media, JobContext* job = nullptr);

	// clip.detectHitpoints (a job), clip.setHitpoints, clip.setHitpointFilter, clip.addHitpoint, clip.setHitpoint,
	// clip.removeHitpoints and event.fromHitpoints.
	void registerHitpointCommands(ProjectModel& model, Engine& engine);
}
