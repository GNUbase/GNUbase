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

		// audio.setDevice's parameters for the backend's default device, at the options' sample rate and block size.
		engine::Json deviceParameters(const Options& options, const std::string& backend)
		{
			engine::Json parameters = {{"backend", backend}};
			if (options.sampleRate)
				parameters["sampleRate"] = *options.sampleRate;
			if (options.blockSize)
				parameters["blockSize"] = *options.blockSize;
			return parameters;
		}

		bool loadProject(engine::Client& client, const Options& options, std::ostream& err)
		{
			return options.project.empty() ||
				setUp(client, "project.load", {{"path", options.project}}, "cannot load " + options.project, err);
		}

		// A user setting that is a string; empty if it can't be read.
		std::string stringSetting(engine::Client& client, const std::string& key)
		{
			const engine::Result result = client.call("settings.get", {{"key", key}});
			return result.ok() ? result.value.at("value").get<std::string>() : std::string();
		}
	}

	bool setUpEngine(engine::Engine& engine, const Options& options, std::ostream& err)
	{
		engine::Client client(engine, "app");
		return setUp(client, "audio.setDevice",
				   deviceParameters(options, options.audio.empty() ? "dummy" : options.audio),
				   "cannot set up the audio device", err) &&
			loadProject(client, options, err);
	}

	bool setUpWindowedEngine(engine::Engine& engine, const Options& options, std::ostream& err)
	{
		engine::Client client(engine, "app");
		engine::Json parameters;
		if (!options.audio.empty())
		{
			parameters = deviceParameters(options, options.audio);
		}
		else
		{
			parameters = deviceParameters(options, stringSetting(client, "audio.backend"));
			if (const std::string device = stringSetting(client, "audio.device"); !device.empty())
				parameters["device"] = device;
		}
		parameters["fallBackToDummy"] = true;
		parameters["start"] = true;
		const engine::Result result = client.call("audio.setDevice", parameters);
		if (!result.ok())
		{
			err << "gnubase: cannot set up the audio device: " << result.error << '\n';
			logging::error("Cannot set up the audio device: {}", result.error);
			return false;
		}
		if (const engine::Json& failed = result.value.at("failed"); !failed.is_null())
			err << "gnubase: cannot use the audio device (" << failed.at("backend").get<std::string>() << ' '
				<< failed.at("device").get<std::string>()
				<< "), playing on the dummy device instead: " << failed.at("reason").get<std::string>() << '\n';
		const engine::Json& device = result.value.at("device");
		logging::info("Audio on {} {}", device.at("backend").get<std::string>(), device.at("id").get<std::string>());
		return loadProject(client, options, err);
	}

	int runHeadless(const Options& options, std::istream& in, std::ostream& out, std::ostream& err)
	{
		engine::Engine engine(audio::makeBackends(), std::filesystem::u8path(options.settings));
		if (!setUpEngine(engine, options, err))
			return g_exitSetup;

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
