// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audiofile/file.h"

#include <cerrno>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <system_error>
#include <vector>

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
		void setErrno(DWORD error)
		{
			errno = EIO;
			if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_INVALID_NAME)
				errno = ENOENT;
			else if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION)
				errno = EACCES;
		}

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
				setErrno(GetLastError());
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

		// The rename with POSIX semantics (FileRenameInfoEx; NTFS, Windows 10 1607 and later): the one replacement
		// that could take the place of a file readers have open atomically, which the classic rename refuses. On
		// Windows 11 build 26200 it answers ERROR_ACCESS_DENIED (5) for every open target, whatever the access, the
		// share mode or the process of its handles, although the POSIX deletion of the same file succeeds; other
		// versions may take it, so replaceFile tries it before giving up (#1387).
		bool posixReplace(const std::filesystem::path& from, const std::filesystem::path& to)
		{
			HANDLE handle =
				CreateFileW(from.c_str(), DELETE | SYNCHRONIZE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
					nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (handle == INVALID_HANDLE_VALUE)
			{
				setErrno(GetLastError());
				return false;
			}
			const DWORD length = static_cast<DWORD>(std::wcslen(to.c_str()) * sizeof(wchar_t));
			std::vector<unsigned char> buffer(sizeof(FILE_RENAME_INFO) + length);
			auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
			info->RootDirectory = nullptr;
			info->FileNameLength = length;
			std::memcpy(info->FileName, to.c_str(), length);
			info->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
			const BOOL replaced =
				SetFileInformationByHandle(handle, FileRenameInfo, info, static_cast<DWORD>(buffer.size()));
			if (!replaced)
				setErrno(GetLastError());
			CloseHandle(handle);
			return replaced;
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

	bool replaceFile(const std::string& from, const std::string& to)
	{
		std::error_code error;
		std::filesystem::rename(from, to, error);
		if (!error)
			return true;
#ifdef _WIN32
		// The classic rename can't replace a file another handle has open; the POSIX-semantic one may (see
		// posixReplace).
		return posixReplace(std::filesystem::u8path(from), std::filesystem::u8path(to));
#else
		errno = error.value();
		return false;
#endif
	}

	bool replaceFileMovingAside(const std::string& from, const std::string& to, const std::string& aside)
	{
		std::error_code error;
		std::filesystem::rename(to, aside, error);
		if (error)
		{
			errno = error.value();
			return false;
		}
		std::filesystem::rename(from, to, error);
		if (error)
		{
			// The new file didn't arrive: put the old one back, so a failed replacement leaves it in place.
			std::error_code ignored;
			std::filesystem::rename(aside, to, ignored);
			errno = error.value();
			return false;
		}
		std::filesystem::remove(aside, error); // if this fails, a cleanup of stale temporaries ends the aside
		return true;
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
