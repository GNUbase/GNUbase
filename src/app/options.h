// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace gnubase::app
{
	// How MCP clients connect to headless GNUbase.
	enum class Transport : uint8_t
	{
		Stdio,
		Http,
	};

	struct Options
	{
		bool help = false;
		bool version = false;
		bool headless = false;
		Transport transport = Transport::Stdio;
		int port = 0;                // HTTP: 0 picks a free port
		std::string project;         // a DAWproject file to load, or none
		std::string script;          // a Lua file to run instead of serving MCP, or none
		std::string audio = "dummy"; // the audio device
		double sampleRate = 48000.0;
		int64_t blockSize = 256;
	};

	// Exit codes, for scripts and CI.
	inline constexpr int g_exitOk = 0;
	inline constexpr int g_exitError = 1;  // something unexpected went wrong
	inline constexpr int g_exitUsage = 2;  // the command line is invalid
	inline constexpr int g_exitSetup = 3;  // the audio device or the project couldn't be set up
	inline constexpr int g_exitScript = 4; // the Lua script raised an error

	class UsageError : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};

	// The command-line arguments after the program name. Throws UsageError for invalid ones.
	Options parseOptions(const std::vector<std::string>& args);

	// The text for --help.
	std::string usage();
}
