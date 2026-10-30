// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <optional>

namespace gnubase::app
{
	// GNUbase's resources folder, with its fonts, icons and themes: `resources` next to the program, as an
	// installation has it, else the source tree's, for builds that run where they were built. Nullopt if neither
	// exists.
	std::optional<std::filesystem::path> findResources(
		const std::filesystem::path& program, const std::filesystem::path& sourceTree);

	// For this program, built from the source tree it was configured in.
	std::optional<std::filesystem::path> findResources();
}
