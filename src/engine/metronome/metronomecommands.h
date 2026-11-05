// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace gnubase::engine
{
	class Engine;
	class ProjectModel;
}

namespace gnubase::engine::metronome
{
	// Registers metronome.get, metronome.set and metronome.setPattern (#101): the project's metronome settings. The
	// metronome itself plays in the audio engine; transport.play counts in.
	void registerMetronomeCommands(ProjectModel& model, Engine& engine);
}
