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

	// mode as for fopen. Null if the file can't be opened; lastError() says why. A file opened with "rb" can be deleted
	// or renamed while it is open, on Windows too.
	File openFile(const std::string& path, const char* mode);

	// Moves from onto to, atomically replacing to: no reader ever sees the name missing or half of the new file, and a
	// reader in the old file reads on. On Windows, where the classic rename refuses to replace a file that another
	// handle has open, a POSIX-semantic rename takes over; where that is refused too, this returns false and the
	// caller falls back to replaceFileMovingAside. False on failure; lastError() says why.
	bool replaceFile(const std::string& from, const std::string& to);

	// Replaces to with from by first moving to to aside, which on Windows works while readers have to open, as long as
	// their handles share deletion; a reader in the moved file reads on. The aside is removed afterwards; if that
	// fails, the caller's cleanup ends it. If from can't take to's place, the aside moves back, so a failed
	// replacement leaves the old file. False on failure; lastError() says why.
	bool replaceFileMovingAside(const std::string& from, const std::string& to, const std::string& aside);

	// Which way the last successful replacement went: Rename for replaceFile's classic rename, PosixRename for its
	// Windows rename with POSIX semantics, Aside for replaceFileMovingAside, None before the first. For tests that
	// show the atomic path is taken where it can be.
	enum class ReplacePath : uint8_t
	{
		None,
		Rename,
		PosixRename,
		Aside
	};

	ReplacePath lastReplacePath();

	// origin is SEEK_SET, SEEK_CUR or SEEK_END.
	bool seekFile(std::FILE* file, int64_t offset, int origin);

	// -1 on failure.
	int64_t tellFile(std::FILE* file);

	// What the last failed file operation reported, e.g. "No such file or directory".
	std::string lastError();
}
