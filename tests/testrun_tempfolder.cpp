// SPDX-License-Identifier: GPL-3.0-or-later
// Every run of the tests gets a temp folder of its own, removed when the run ends. An engine without a settings folder
// keeps the recordings and frozen files of an unsaved project in the temp folder, and a shared one fills up on a
// machine that runs the tests all day.
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

#ifndef _WIN32
#include <sys/wait.h>
#endif

namespace
{
	void setTempFolder(const std::filesystem::path& folder)
	{
#ifdef _WIN32
		_wputenv_s(L"TMP", folder.c_str());
		_wputenv_s(L"TEMP", folder.c_str());
#else
		setenv("TMPDIR", folder.c_str(), 1);
#endif
	}

	class TempFolderOfTheRun final : public Catch::EventListenerBase
	{
	public:
		using Catch::EventListenerBase::EventListenerBase;

		void testRunStarting(const Catch::TestRunInfo& /*info*/) override
		{
			m_shared = std::filesystem::temp_directory_path();
			m_folder = std::make_unique<gnubase::test::TempDir>();
			setTempFolder(m_folder->path());
		}

		void testRunEnded(const Catch::TestRunStats& /*stats*/) override
		{
			setTempFolder(m_shared);
			m_folder.reset();
		}

	private:
		std::filesystem::path m_shared;
		std::unique_ptr<gnubase::test::TempDir> m_folder;
	};
}

CATCH_REGISTER_LISTENER(TempFolderOfTheRun)

#ifdef GNUBASE_TESTS
namespace
{
	constexpr std::string_view g_reported = "temp folder of the run: ";
}

// Run by the test below in a child process: reports the run's temp folder and leaves a file in it.
TEST_CASE("a test run reports its temp folder", "[.][unit][temp-folder-child]")
{
	std::filesystem::path folder = std::filesystem::temp_directory_path();
	if (!folder.has_filename())
		folder = folder.parent_path(); // without the trailing separator
	REQUIRE(std::ofstream(folder / "left behind.txt") << "a recording");
	std::cout << g_reported << folder.u8string() << '\n';
}

TEST_CASE("a test run's temp folder is gone when the run ends", "[integration]")
{
	const gnubase::test::TempDir dir;
	const std::filesystem::path out = dir.path() / "out.txt";
	const std::string command =
		"\"" + std::string(GNUBASE_TESTS) + "\" \"[temp-folder-child]\" > \"" + out.u8string() + "\"";
#ifdef _WIN32
	// cmd.exe removes the outer quotes. As UTF-16, so that paths with any characters come through.
	// NOLINTNEXTLINE(bugprone-command-processor): fixed test command
	const int status = _wsystem(std::filesystem::u8path("\"" + command + "\"").wstring().c_str());
#else
	const int raw = std::system(command.c_str()); // NOLINT(bugprone-command-processor): fixed test command
	const int status = WIFEXITED(raw) ? WEXITSTATUS(raw) : -1;
#endif
	const std::string output = gnubase::test::readText(out);
	INFO(output);
	CHECK(status == 0);
	std::istringstream lines(output);
	std::filesystem::path folder;
	for (std::string line; std::getline(lines, line);)
	{
		if (!line.empty() && line.back() == '\r')
			line.pop_back();
		if (line.rfind(g_reported, 0) == 0)
			folder = std::filesystem::u8path(line.substr(g_reported.size()));
	}
	REQUIRE(!folder.empty());
	CHECK(folder.filename().u8string().rfind("gnubase-test-", 0) == 0);
	CHECK(std::filesystem::equivalent(folder.parent_path(), std::filesystem::temp_directory_path())); // ours
	CHECK(!std::filesystem::exists(folder));
}
#endif
