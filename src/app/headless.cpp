// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/headless.h"

#include "app/scripttool.h"

#include "engine/engine.h"
#include "mcp/server.h"
#include "mcp/transport.h"
#include "platform/log.h"
#include "scripting/runtime.h"

#include <chrono>
#include <filesystem>
#include <optional>
#include <ostream>
#include <string>
#include <thread>

namespace gnubase::app
{
	volatile std::sig_atomic_t g_stopRequested = 0;

	namespace
	{
		// Runs one setup command and reports what went wrong.
		bool setUp(engine::Client& client, const std::string& command, const engine::Json& parameters,
			const std::string& what, std::ostream& err)
		{
			const engine::Result result = client.call(command, parameters);
			if (!result.ok())
			{
				err << "gnubase: " << what << ": " << result.error << '\n';
				logging::error("{}: {}", what, result.error);
			}
			return result.ok();
		}
	}

	int runHeadless(const Options& options, std::istream& in, std::ostream& out, std::ostream& err)
	{
		engine::Engine engine;
		{
			engine::Client client(engine);
			if (!setUp(client, "audio.setDevice",
					{{"sampleRate", options.sampleRate}, {"blockSize", options.blockSize}},
					"cannot set up the audio device", err))
				return g_exitSetup;
			if (!options.project.empty() &&
				!setUp(client, "project.load", {{"path", options.project}}, "cannot load " + options.project, err))
				return g_exitSetup;
		}

		if (!options.script.empty())
		{
			logging::info("Headless, running {}", options.script);
			scripting::Runtime lua(engine, scripting::Trust::Trusted, out);
			if (const std::optional<std::string> error = lua.runFile(std::filesystem::u8path(options.script)))
			{
				err << "gnubase: " << *error << '\n';
				logging::error("{}", *error);
				return g_exitScript;
			}
			return g_exitOk;
		}

		mcp::Server server(engine);
		addScriptTool(server, engine);
		if (options.transport == Transport::Stdio)
		{
			logging::info("Headless, serving MCP on stdio");
			mcp::serveStdio(server, in, out);
			return g_exitOk;
		}

		const std::string token = mcp::makeToken();
		const mcp::HttpServer http(server, token, options.port);
		out << "MCP server: " << http.url() << "\nAccess token: " << token << '\n';
		out.flush(); // for whoever waits for the URL
		while (g_stopRequested == 0)
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		logging::info("Headless run stopped");
		return g_exitOk;
	}
}
