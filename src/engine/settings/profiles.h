// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <filesystem>

// Profiles (#163, docs/features/profiles.md, reference manual pp. 1579-1583): named sets of the user settings that
// belong to a user's way of working, which the user switches between, with a restart, and takes to other computers.
namespace gnubase::engine
{
	class Engine;

	namespace settings
	{
		// The files of the settings folder that a profile holds: the user settings (preferences, key commands, global
		// workspaces, toolbars and the like) and the quantize presets. Not the recent projects, templates and
		// recordings.
		inline constexpr std::array<const char*, 2> g_profileFiles = {"settings.json", "quantize-presets.json"};

		// Before the settings are read at startup: if profile.activate chose another profile, the active profile's
		// files go into its folder (profiles/<name>) and the chosen one's come into the settings folder. Does nothing
		// without a folder; logs what fails and then leaves the files and the active profile as they were.
		void switchProfile(const std::filesystem::path& directory);

		// Drops the profile that profile.activate chose for the next start, so that the active one stays: for the
		// safe start mode's Delete program preferences, which deletes the active profile's files. Throws
		// std::system_error if profiles.json can't be written.
		void cancelProfileSwitch(const std::filesystem::path& directory);

		// profile.list, profile.add, profile.duplicate, profile.rename, profile.remove, profile.activate,
		// profile.export and profile.import, on the profiles in the folder. They fail without one.
		void registerProfileCommands(Engine& engine, const std::filesystem::path& directory);
	}
}
