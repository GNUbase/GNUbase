// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <optional>
#include <vector>

namespace gnubase::test
{
	// Makes the temporary folder (std::filesystem::temp_directory_path) folder while it lives, e.g. a file, so that an
	// engine without a settings folder has no folder for its recordings. Sets TMP and TEMP on Windows and TMPDIR
	// elsewhere, and puts them back as they were, also unset. Not while other threads read them.
	class TempFolderAt
	{
	public:
		explicit TempFolderAt(const std::filesystem::path& folder);
		~TempFolderAt();
		TempFolderAt(const TempFolderAt&) = delete;
		TempFolderAt& operator=(const TempFolderAt&) = delete;
		TempFolderAt(TempFolderAt&&) = delete;
		TempFolderAt& operator=(TempFolderAt&&) = delete;

	private:
		std::vector<std::optional<std::filesystem::path::string_type>> m_previous; // per variable; empty if unset
	};
}
