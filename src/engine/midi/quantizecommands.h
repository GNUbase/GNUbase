// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/settings/store.h"

namespace gnubase::engine
{
	class Engine;
	class ProjectModel;
}

namespace gnubase::engine::midi
{
	// quantize-presets.json in the settings folder: the user's quantize presets, the setting quantize.presets, as
	// a list of {name, settings} with the settings as quantize.presets returns them. Factory presets aren't in it.
	const settings::Format& quantizePresetsFormat();

	// Registers the quantize commands (#132): quantizing note starts, ends and lengths, resetting and freezing, the
	// project's quantize settings and auto quantize, and quantize presets with grooves. Presets belong to the user,
	// so every project has them: the engine reads the user's presets from its settings folder when it starts and
	// saves them there whenever one changes.
	void registerQuantizeCommands(ProjectModel& model, Engine& engine);
}
