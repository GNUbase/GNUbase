// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Hitpoints and slices (#143, docs/features/hitpoints.md): finding where sounds start in a clip, editing and filtering
// those positions, and making slices, events, regions, markers, a groove and MIDI notes of them.
namespace gnubase::engine
{
	class Engine;
	class Jobs;
	class ProjectModel;

	// clip.detectHitpoints (a job), clip.setHitpoints, clip.setHitpointFilter, clip.addHitpoint, clip.setHitpoint,
	// clip.removeHitpoints and event.fromHitpoints.
	void registerHitpointCommands(ProjectModel& model, Engine& engine, Jobs& jobs);
}
