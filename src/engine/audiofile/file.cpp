// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audiofile/file.h"

#include <atomic>
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
	namespace
	{
		std::atomic<ReplacePath> g_lastReplacePath{ReplacePath::None};

		// errno holds C library error numbers, but the renames report the system's own codes, on Windows its error
		// codes rather than errno's: store the portable condition such a code stands for, EIO where none fits (#1446).
		void setErrno(const std::error_code& error)
		{
			const std::error_condition condition = error.default_error_condition();
			errno = condition.category() == std::generic_category() ? condition.value() : EIO;
		}
	}

#ifdef _WIN32
	namespace
	{
		void setErrno(DWORD error)
		{
			setErrno(std::error_code(static_cast<int>(error), std::system_category()));
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

		// The rename with POSIX semantics (FileRenameInfoEx; NTFS, Windows 10 1607 and later): it replaces the
		// destination atomically even while readers have it open, which the classic rename refuses, as long as every
		// handle in the destination shares deletion (FILE_SHARE_DELETE), as the engine's readers' do; a handle that
		// doesn't answers ERROR_SHARING_VIOLATION (32), and the caller falls back to replaceFileMovingAside (#1387).
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
			// FileRenameInfoEx, not FileRenameInfo: only the Ex class reads the Flags member; the classic one takes
			// its first byte as ReplaceIfExists and ignores the POSIX flag, which makes this the classic rename.
			const BOOL replaced =
				SetFileInformationByHandle(handle, FileRenameInfoEx, info, static_cast<DWORD>(buffer.size()));
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
		{
			g_lastReplacePath = ReplacePath::Rename;
			return true;
		}
#ifdef _WIN32
		// The classic rename can't replace a file another handle has open; the POSIX-semantic one may (see
		// posixReplace).
		const bool replaced = posixReplace(std::filesystem::u8path(from), std::filesystem::u8path(to));
		if (replaced)
			g_lastReplacePath = ReplacePath::PosixRename;
		return replaced;
#else
		setErrno(error);
		return false;
#endif
	}

	ReplacePath lastReplacePath()
	{
		return g_lastReplacePath;
	}

	bool replaceFileMovingAside(const std::string& from, const std::string& to, const std::string& aside)
	{
		std::error_code error;
		std::filesystem::rename(to, aside, error);
		if (error)
		{
			setErrno(error);
			return false;
		}
		std::filesystem::rename(from, to, error);
		if (error)
		{
			// The new file didn't arrive: put the old one back, so a failed replacement leaves it in place.
			std::error_code ignored;
			std::filesystem::rename(aside, to, ignored);
			setErrno(error);
			return false;
		}
		std::filesystem::remove(aside, error); // if this fails, a cleanup of stale temporaries ends the aside
		g_lastReplacePath = ReplacePath::Aside;
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
