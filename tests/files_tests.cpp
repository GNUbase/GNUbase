// SPDX-License-Identifier: GPL-3.0-or-later
// Platform file helpers (src/platform/files.h): the identity a path names on disk, which tells two spellings of one
// file apart from two files.
#include "platform/files.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

namespace
{
	namespace fs = std::filesystem;

	void write(const fs::path& file)
	{
		std::ofstream out(file, std::ios::binary);
		out << "x";
	}

	TEST_CASE("a file's identity is the file, not the spelling of its path", "[unit]")
	{
		const gnubase::test::TempDir folder;
		CHECK_FALSE(gnubase::files::fileIdentity(folder.path() / "absent.wav").has_value());

		write(folder.path() / "one.wav");
		write(folder.path() / "two.wav");
		const auto one = gnubase::files::fileIdentity(folder.path() / "one.wav");
		const auto two = gnubase::files::fileIdentity(folder.path() / "two.wav");
		REQUIRE(one.has_value());
		REQUIRE(two.has_value());
		CHECK(*one == gnubase::files::fileIdentity(folder.path() / "one.wav")); // the same file, asked again
		CHECK_FALSE(*one == *two);                                              // two files are two identities

		// A letter's case the file system folds but the name doesn't: on Windows and macOS Übung.wav and übung.wav are
		// one file, so one identity; on Linux they are two names and the second isn't there.
		const fs::path upper = folder.path() / fs::u8path("Übung.wav");
		const fs::path lower = folder.path() / fs::u8path("übung.wav");
		write(upper);
		const auto folded = gnubase::files::fileIdentity(lower);
		if (gnubase::files::currentOs() == gnubase::files::Os::Linux)
			CHECK_FALSE(folded.has_value());
		else
			CHECK(folded == gnubase::files::fileIdentity(upper));
	}
}
