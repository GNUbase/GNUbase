// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace gnubase::engine
{
	class Engine;
	class ProjectModel;
}

namespace gnubase::engine::midi
{
	// Registers the quantize commands (#132): quantizing note starts, ends and lengths, resetting and freezing, the
	// project's quantize settings and auto quantize, and quantize presets with grooves. Presets belong to the engine,
	// so every project has them; the user's presets last as long as the engine.
	void registerQuantizeCommands(ProjectModel& model, Engine& engine);
}
