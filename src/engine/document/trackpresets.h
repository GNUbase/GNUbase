// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Track presets and track import and export (#164, docs/features/track-presets.md): track.savePreset, track.presets,
// track.loadPreset, track.addFromPreset, track.export, track.importSources and track.import.
namespace gnubase::engine
{
	class Engine;
	class ProjectModel;

	void registerTrackPresetCommands(ProjectModel& model, Engine& engine);
}
