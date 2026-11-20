// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/process.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

#ifdef _WIN32
#include <cstring>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// dbghelp.h needs windows.h first.
#include <dbghelp.h>
#endif

using Catch::Matchers::ContainsSubstring;

namespace
{
	// Runs `program`, gnubase-crashtest or a copy of it, which opens a log session in the folder and crashes the way
	// it is told, and returns its exit code. Fails if it still runs after a minute and a half: the crash handler ends a
	// crashed process within 35 s.
	int crash(const std::filesystem::path& program, const std::filesystem::path& dir, const std::string& how)
	{
		gnubase::platform::ChildProcess process =
			gnubase::platform::ChildProcess::start(program, {dir.u8string(), how}, dir / "stderr.txt");
		const std::optional<int> code = process.wait(std::chrono::seconds(90));
		if (!code)
		{
			process.kill();
			FAIL("the crashed process still runs");
		}
		return code.value_or(-1);
	}

	int crash(const std::filesystem::path& dir, const std::string& how)
	{
		return crash(GNUBASE_CRASHTEST, dir, how);
	}

#ifdef _WIN32
	constexpr unsigned g_accessViolation = 0xC0000005U;
	constexpr unsigned g_illegalInstruction = 0xC000001DU;

	// The exception code a minidump records, or nothing if it records no exception (abort) or isn't one.
	std::optional<unsigned> dumpedException(const std::string& dump)
	{
		MINIDUMP_HEADER header{};
		if (dump.size() < sizeof(header))
			return std::nullopt;
		std::memcpy(&header, dump.data(), sizeof(header));
		if (header.Signature != MINIDUMP_SIGNATURE)
			return std::nullopt;
		for (ULONG32 i = 0; i < header.NumberOfStreams; ++i)
		{
			MINIDUMP_DIRECTORY entry{};
			const size_t at = header.StreamDirectoryRva + (size_t{i} * sizeof(entry));
			if (at + sizeof(entry) > dump.size())
				return std::nullopt;
			std::memcpy(&entry, dump.data() + at, sizeof(entry));
			if (entry.StreamType != ExceptionStream)
				continue;
			MINIDUMP_EXCEPTION_STREAM stream{};
			if (entry.Location.Rva + sizeof(stream) > dump.size())
				return std::nullopt;
			std::memcpy(&stream, dump.data() + entry.Location.Rva, sizeof(stream));
			return stream.ExceptionRecord.ExceptionCode;
		}
		return std::nullopt;
	}

	// A copy of gnubase-crashtest in a folder of its own, without gnubase-crashdump next to it.
	std::filesystem::path crashTestAlone(const std::filesystem::path& dir)
	{
		const std::filesystem::path program = dir / "alone" / std::filesystem::path(GNUBASE_CRASHTEST).filename();
		std::filesystem::create_directories(program.parent_path());
		std::filesystem::copy_file(GNUBASE_CRASHTEST, program);
		return program;
	}
#endif
}

TEST_CASE("a crash is recorded in the log", "[integration]")
{
	const std::string how = GENERATE(as<std::string>{}, "segv", "abort");
	CAPTURE(how);
	const gnubase::test::TempDir dir;
	const int code = crash(dir.path(), how);
	CHECK(code != 0);
	const std::string log = gnubase::test::readText(dir.path() / "crash.log");
	CHECK_THAT(log, ContainsSubstring("about to crash"));
	CHECK_THAT(log, ContainsSubstring("Crash: "));
#ifdef _WIN32
	// The process ends with the code it would have without the handler: the exception's, or abort's 3. The dump
	// shows the exception.
	const std::string dump = gnubase::test::readText(dir.path() / "crash.dmp");
	CHECK(!dump.empty());
	if (how == "segv")
	{
		CHECK(static_cast<unsigned>(code) == g_accessViolation);
		CHECK(dumpedException(dump) == g_accessViolation);
	}
	else
	{
		CHECK(code == 3);
		CHECK(dumpedException(dump) == std::nullopt);
	}
#endif
}

#ifdef _WIN32
TEST_CASE("without gnubase-crashdump, a crashed process writes its minidump itself", "[integration]")
{
	const std::string how = GENERATE(as<std::string>{}, "segv", "abort");
	CAPTURE(how);
	const gnubase::test::TempDir dir;
	const int code = crash(crashTestAlone(dir.path()), dir.path(), how);
	CHECK(static_cast<unsigned>(code) == (how == "segv" ? g_accessViolation : 3U));
	CHECK_THAT(gnubase::test::readText(dir.path() / "crash.log"), ContainsSubstring("Crash: "));
	const std::string dump = gnubase::test::readText(dir.path() / "crash.dmp");
	CHECK(!dump.empty());
	CHECK(dumpedException(dump) == (how == "segv" ? std::optional<unsigned>(g_accessViolation) : std::nullopt));
}

TEST_CASE("of two threads crashing at once, the first crash is in the dump and gives the exit code", "[integration]")
{
	const bool alone = GENERATE(false, true);
	CAPTURE(alone);
	// The two crashes come in either order. Most runs report both, so the second one came while the first was handled:
	// it waits for the first to end the process, instead of ending it with its own code once the dump is written.
	int bothReported = 0;
	for (int run = 0; run < 5; ++run)
	{
		CAPTURE(run);
		const gnubase::test::TempDir dir;
		const int code = alone ? crash(crashTestAlone(dir.path()), dir.path(), "twice") : crash(dir.path(), "twice");
		const auto exception = static_cast<unsigned>(code);
		CHECK((exception == g_accessViolation || exception == g_illegalInstruction));
		CHECK(dumpedException(gnubase::test::readText(dir.path() / "crash.dmp")) == exception);
		const std::string log = gnubase::test::readText(dir.path() / "crash.log");
		if (log.find("Crash: ") != log.rfind("Crash: "))
			++bothReported;
	}
	CHECK(bothReported > 0);
}

TEST_CASE("a crash ends the process even while another thread holds the heap's lock", "[integration]")
{
	// gnubase-crashdump writes the dump from outside the process, so it doesn't wait for the lock.
	const gnubase::test::TempDir dir;
	CHECK(static_cast<unsigned>(crash(dir.path(), "heaplocked")) == g_accessViolation);
	CHECK(dumpedException(gnubase::test::readText(dir.path() / "crash.dmp")) == g_accessViolation);
}

TEST_CASE("a crash while the log session ends ends the process with its exit code", "[integration]")
{
	// Depending on the timing, the crash handler takes the crash and writes a dump, or ends the process without one
	// because it is being removed, or it is gone and Windows ends the process. A dump that was begun is finished: the
	// session's end doesn't stop gnubase-crashdump while it writes one.
	for (int run = 0; run < 5; ++run)
	{
		CAPTURE(run);
		const gnubase::test::TempDir dir;
		CHECK(static_cast<unsigned>(crash(dir.path(), "ending")) == g_accessViolation);
		if (std::filesystem::exists(dir.path() / "crash.dmp"))
			CHECK(dumpedException(gnubase::test::readText(dir.path() / "crash.dmp")) == g_accessViolation);
	}
}

TEST_CASE("a crash whose filter finds the handler removed ends the process without a dump", "[integration]")
{
	// The filter was entered just before the session ended: the events and gnubase-crashdump are gone, so it ends
	// the process with the exception's code at once.
	const gnubase::test::TempDir dir;
	CHECK(static_cast<unsigned>(crash(dir.path(), "removed")) == g_accessViolation);
	CHECK(!std::filesystem::exists(dir.path() / "crash.dmp"));
}

TEST_CASE("a log session starts gnubase-crashdump and ends it with the session", "[integration]")
{
	const gnubase::test::TempDir dir;
	CHECK(crash(dir.path(), "helper") == 0); // 5: no helper while the session runs, 6: one left after it
}

TEST_CASE("if gnubase-crashdump is gone, a crashed process writes its minidump itself", "[integration]")
{
	const gnubase::test::TempDir dir;
	CHECK(static_cast<unsigned>(crash(dir.path(), "helperkilled")) == g_accessViolation);
	CHECK(dumpedException(gnubase::test::readText(dir.path() / "crash.dmp")) == g_accessViolation);
}
#endif
