// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audiofile/file.h"

#include <cerrno>
#include <filesystem>
#include <system_error>

namespace gnubase::engine::detail
{
	File openFile(const std::string& path, const char* mode)
	{
#ifdef _WIN32
		const std::wstring wideMode(mode, mode + std::char_traits<char>::length(mode));
		std::FILE* file = nullptr;
		if (_wfopen_s(&file, std::filesystem::u8path(path).c_str(), wideMode.c_str()) != 0)
			return nullptr;
		return File(file);
#else
		return File(std::fopen(path.c_str(), mode));
#endif
	}

	bool seekFile(std::FILE* file, int64_t offset, int origin)
	{
#ifdef _WIN32
		return _fseeki64(file, offset, origin) == 0;
#else
		return fseeko(file, static_cast<off_t>(offset), origin) == 0;
#endif
	}

	int64_t tellFile(std::FILE* file)
	{
#ifdef _WIN32
		return _ftelli64(file);
#else
		return static_cast<int64_t>(ftello(file));
#endif
	}

	std::string lastError()
	{
		return std::error_code(errno, std::generic_category()).message();
	}
}
