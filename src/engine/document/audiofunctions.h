// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The audio functions (#147, docs/features/audio-functions.md): detecting silence, the spectrum and the statistics of
// a clip's audio.
namespace gnubase::engine
{
	class Engine;
	class ProjectModel;

	// clip.detectSilence, event.stripSilence, clip.spectrum and clip.statistics.
	void registerAudioFunctionCommands(ProjectModel& model, Engine& engine);
}
