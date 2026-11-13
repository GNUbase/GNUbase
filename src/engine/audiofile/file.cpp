// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audiofile/file.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// io.h and fcntl.h after windows.h.
#include <fcntl.h>
#include <io.h>
#endif

namespace gnubase::engine::detail
{
#ifdef _WIN32
	namespace
	{
		// A file opened only for reading, which others may delete or rename meanwhile, as on the other platforms: the
		// engine reads the files it plays, and their peaks, on threads of its own, such as a frozen track's freeze
		// file, and fopen's files can't be deleted while they are open (#951).
		File openForReading(const std::filesystem::path& path)
		{
			HANDLE handle =
				CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
					OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (handle == INVALID_HANDLE_VALUE)
			{
				const DWORD error = GetLastError();
				errno = EIO;
				if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_INVALID_NAME)
					errno = ENOENT;
				else if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION)
					errno = EACCES;
				return nullptr;
			}
			// The descriptor owns the handle from here, and the FILE the descriptor.
			const int descriptor = _open_osfhandle(reinterpret_cast<intptr_t>(handle), _O_RDONLY | _O_BINARY);
			if (descriptor == -1)
			{
				CloseHandle(handle);
				return nullptr;
			}
			std::FILE* file = _fdopen(descriptor, "rb");
			if (file == nullptr)
				_close(descriptor);
			return File(file);
		}
	}
#endif

	File openFile(const std::string& path, const char* mode)
	{
#ifdef _WIN32
		if (std::strcmp(mode, "rb") == 0)
			return openForReading(std::filesystem::u8path(path));
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
