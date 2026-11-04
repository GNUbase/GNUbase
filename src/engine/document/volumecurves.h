// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Event volume curves (#139, docs/features/volume-curves.md): audioEvent.setVolumeCurve, event.removeVolumeCurve and
// range.volumeCurve. Playback applies them (makePlaylist in src/engine/audio/playback.cpp).
namespace gnubase::engine
{
	class Engine;
	class ProjectModel;

	void registerVolumeCurveCommands(ProjectModel& model, Engine& engine);
}
