// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

// C stdio files with UTF-8 paths and 64-bit positions on every platform, for the codecs that read and write
// through FILE.
namespace gnubase::engine::detail
{
	struct FileCloser
	{
		void operator()(std::FILE* file) const { std::fclose(file); }
	};

	using File = std::unique_ptr<std::FILE, FileCloser>;

	// mode as for fopen. Null if the file can't be opened; lastError() says why.
	File openFile(const std::string& path, const char* mode);

	// origin is SEEK_SET, SEEK_CUR or SEEK_END.
	bool seekFile(std::FILE* file, int64_t offset, int origin);

	// -1 on failure.
	int64_t tellFile(std::FILE* file);

	// What the last failed file operation reported, e.g. "No such file or directory".
	std::string lastError();
}
