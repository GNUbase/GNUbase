// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace gnubase::engine
{
	class Engine;
	class ProjectModel;
}

namespace gnubase::engine::midi
{
	// Registers the MIDI function commands (midi.transpose, midi.legato, midi.merge and so on) with the project
	// model: each one checks its parameters, runs its function from functions.h and records an undo step.
	void registerFunctionCommands(ProjectModel& model, Engine& engine);
}
