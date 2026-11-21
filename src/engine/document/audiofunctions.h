// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The audio functions (#147, docs/features/audio-functions.md): detecting silence, the spectrum and the statistics of
// a clip's audio.
namespace gnubase::engine
{
	class Engine;
	class Jobs;
	class ProjectModel;

	// clip.detectSilence, event.stripSilence, clip.spectrum, clip.statistics and clip.estimatePitch.
	void registerAudioFunctionCommands(ProjectModel& model, Engine& engine, Jobs& jobs);
}
