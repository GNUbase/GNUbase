// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/options.h"

#include "platform/audio/device.h"
#include "platform/files.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// shellapi.h needs windows.h first.
#include <shellapi.h>
#endif

namespace gnubase::app
{
	namespace
	{
		double parseNumber(const std::string& option, const std::string& text)
		{
			char* end = nullptr;
			const double value = std::strtod(text.c_str(), &end);
			if (text.empty() || end != text.c_str() + text.size() || !std::isfinite(value))
				throw UsageError(option + " needs a number, not \"" + text + "\"");
			return value;
		}

		int64_t parseInteger(const std::string& option, const std::string& text)
		{
			char* end = nullptr;
			const long long value = std::strtoll(text.c_str(), &end, 10);
			if (text.empty() || end != text.c_str() + text.size())
				throw UsageError(option + " needs a whole number, not \"" + text + "\"");
			return value;
		}

		// The audio backends of this build, e.g. "wasapi, dummy".
		std::string backendNames()
		{
			std::string names;
			for (const std::unique_ptr<audio::Backend>& backend : audio::makeBackends())
				names.append(names.empty() ? "" : ", ").append(backend->name());
			return names;
		}
	}

	std::vector<std::string> commandLineArguments(int argc, char** argv)
	{
#ifdef _WIN32
		struct LocalFreer
		{
			void operator()(LPWSTR* arguments) const { LocalFree(static_cast<void*>(arguments)); }
		};
		int count = 0;
		const std::unique_ptr<LPWSTR, LocalFreer> wide(CommandLineToArgvW(GetCommandLineW(), &count));
		if (wide && count == argc)
		{
			std::vector<std::string> args;
			args.reserve(static_cast<size_t>(count));
			for (int i = 1; i < count; ++i)
				args.push_back(std::filesystem::path(wide.get()[i]).u8string());
			return args;
		}
#endif
		return {argv + std::min(argc, 1), argv + argc};
	}

	Options parseOptions(const std::vector<std::string>& args)
	{
		Options options;
		bool mcp = false;
		for (size_t i = 0; i < args.size(); ++i)
		{
			const std::string& arg = args[i];
			const auto value = [&args, &i, &arg]() -> const std::string&
			{
				if (i + 1 >= args.size())
					throw UsageError(arg + " needs a value");
				return args[++i];
			};
			if (arg == "--help" || arg == "-h")
			{
				options.help = true;
			}
			else if (arg == "--version")
			{
				options.version = true;
			}
			else if (arg == "--headless")
			{
				options.headless = true;
			}
			else if (arg == "--mcp-stdio")
			{
				options.transport = Transport::Stdio;
				mcp = true;
			}
			else if (arg == "--mcp-http" || arg.rfind("--mcp-http=", 0) == 0)
			{
				options.transport = Transport::Http;
				options.port = 0;
				if (arg != "--mcp-http")
				{
					const int64_t port = parseInteger("--mcp-http", arg.substr(11));
					if (port < 1 || port > 65535)
					{
						throw UsageError("--mcp-http needs a port from 1 to 65535");
					}
					options.port = static_cast<int>(port);
				}
				mcp = true;
			}
			else if (arg == "--project")
			{
				options.project = value();
			}
			else if (arg == "--script")
			{
				options.script = value();
			}
			else if (arg == "--audio")
			{
				options.audio = value();
				const auto backends = audio::makeBackends();
				if (std::none_of(backends.begin(), backends.end(),
						[&options](const std::unique_ptr<audio::Backend>& backend)
						{ return backend->name() == options.audio; }))
					throw UsageError(
						"there is no audio backend " + options.audio + "; this build has " + backendNames());
			}
			else if (arg == "--sample-rate")
			{
				options.sampleRate = parseNumber(arg, value());
			}
			else if (arg == "--block-size")
			{
				options.blockSize = parseInteger(arg, value());
			}
			else if (arg == "--settings")
			{
				options.settings = value();
			}
			else
			{
				throw UsageError("unknown option " + arg);
			}
		}
		if (((mcp && options.transport == Transport::Stdio) || !options.script.empty()) && !options.headless &&
			!options.help && !options.version)
			throw UsageError("--mcp-stdio and --script need --headless");
		if (mcp && !options.script.empty())
			throw UsageError("--script runs a script and exits; it doesn't serve MCP");
		return options;
	}

	std::filesystem::path settingsDirectory(const Options& options)
	{
		if (!options.settings.empty())
			return std::filesystem::u8path(options.settings);
		return files::settingsDirectory();
	}

	std::string usage()
	{
		return R"(Usage: gnubase [options]

Opens GNUbase's main window, or with --headless runs without UI.

  --headless            run without UI: the engine and its MCP server
  --mcp-stdio           MCP over stdin and stdout (the default with --headless)
  --mcp-http[=PORT]     MCP over HTTP on 127.0.0.1, on PORT or a free port; prints the URL and the access token.
                        Headless it serves until Ctrl+C; with the UI, until GNUbase quits, with the UI tools too
  --project FILE        load a DAWproject file
  --script FILE         run a Lua script, then exit (trusted: all standard libraries but os.exit)
  --audio BACKEND       the audio backend: )" +
			backendNames() + R"(; by default dummy, which has no hardware
  --sample-rate HZ      the audio device's sample rate (default 48000)
  --block-size FRAMES   frames per audio block (default 256)
  --settings DIR        keep the user settings in DIR, e.g. for a portable install; by default the folder in
                        GNUBASE_SETTINGS_DIR or the user's settings folder
  --version             print the version
  -h, --help            print this help

Exit codes: 0 done, 1 unexpected error, 2 invalid command line, 3 audio device or project could not be set up,
4 the script failed.
)";
	}
}
