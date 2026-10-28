// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/options.h"

#include <cmath>
#include <cstdlib>

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
				if (options.audio != "dummy")
				{
					throw UsageError("there is no audio device " + options.audio + " yet, only \"dummy\"");
				}
			}
			else if (arg == "--sample-rate")
			{
				options.sampleRate = parseNumber(arg, value());
			}
			else if (arg == "--block-size")
			{
				options.blockSize = parseInteger(arg, value());
			}
			else
			{
				throw UsageError("unknown option " + arg);
			}
		}
		if ((mcp || !options.script.empty()) && !options.headless && !options.help && !options.version)
			throw UsageError("the MCP transports and --script need --headless for now");
		if (mcp && !options.script.empty())
			throw UsageError("--script runs a script and exits; it doesn't serve MCP");
		return options;
	}

	std::string usage()
	{
		return R"(Usage: gnubase [options]

  --headless            run without UI: the engine and its MCP server
  --mcp-stdio           MCP over stdin and stdout (the default with --headless)
  --mcp-http[=PORT]     MCP over HTTP on 127.0.0.1, on PORT or a free port; prints the URL and the access token
                        and serves until Ctrl+C
  --project FILE        load a DAWproject file
  --script FILE         run a Lua script, then exit (trusted: all standard libraries but os.exit)
  --audio dummy         the audio device: only "dummy" (no hardware) so far
  --sample-rate HZ      the audio device's sample rate (default 48000)
  --block-size FRAMES   frames per audio block (default 256)
  --version             print the version
  -h, --help            print this help

Exit codes: 0 done, 1 unexpected error, 2 invalid command line, 3 audio device or project could not be set up,
4 the script failed.
)";
	}
}
