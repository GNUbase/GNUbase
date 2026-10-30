// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/settings/store.h"

#include <cstdint>
#include <string>

namespace gnubase::engine
{
	class Engine;
}

namespace gnubase::engine::settings
{
	// The user's settings for GNUbase as a whole, in settings.json in the settings folder. Each one is read where its
	// feature lives; the preferences dialog (#162) shows them all.
	inline constexpr Key<double> g_uiScale{"ui.scale"};
	inline constexpr Key<std::string> g_uiTheme{"ui.theme"};
	inline constexpr Key<std::string> g_audioAutoMonitoring{"audio.autoMonitoring"};
	inline constexpr Key<std::string> g_audioBackend{"audio.backend"};
	inline constexpr Key<std::string> g_audioDevice{"audio.device"};
	inline constexpr Key<double> g_audioSampleRate{"audio.sampleRate"};
	inline constexpr Key<int64_t> g_audioBlockSize{"audio.blockSize"};

	// settings.json: every user setting, its version and the migrations from its older versions. A new setting
	// only needs a definition; renaming one or changing what its values mean raises the version and adds a
	// migration, and tests/data/settings gets a file of the new version.
	const Format& userFormat();

	// settings.get, settings.set, settings.reset and settings.list for the store, which only the command thread uses.
	void registerCommands(Engine& engine, Store& store);
}
