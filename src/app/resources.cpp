// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/resources.h"

#include "platform/process.h"

#include <system_error>

namespace gnubase::app
{
	std::optional<std::filesystem::path> findResources(
		const std::filesystem::path& program, const std::filesystem::path& sourceTree)
	{
		std::error_code error;
		for (const std::filesystem::path& candidate : {program.parent_path() / "resources", sourceTree})
		{
			if (std::filesystem::is_directory(candidate, error))
				return candidate;
		}
		return std::nullopt;
	}

	std::optional<std::filesystem::path> findResources()
	{
		return findResources(platform::currentExecutablePath(), std::filesystem::u8path(GNUBASE_SOURCE_RESOURCES_DIR));
	}
}
