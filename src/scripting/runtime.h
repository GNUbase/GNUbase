// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct lua_Debug; // NOLINT(readability-identifier-naming): Lua's own name
struct lua_State;

// GNUbase's Lua 5.4 scripting. The API is generated from the engine's command registry, like the MCP tools.
namespace gnubase::scripting
{
	enum class Trust : uint8_t
	{
		Untrusted, // no io, os, debug, package, require, dofile, loadfile or binary chunks, e.g. scripts in projects
		Trusted,   // every standard library except os.exit, which would skip GNUbase's shutdown
	};

	// A Lua state with GNUbase's API in the global table gnubase:
	// - gnubase.<area>.<name>(parameters): every engine command, e.g. gnubase.track.add{type = "audio"} for
	//   track.add. Returns the result as a table, or raises an error with the reason.
	// - gnubase.on(name, handler): calls handler(event) for every event with that name ("*" for all) while the
	//   script runs commands or waits. An event is a table with name, data and request.
	// - gnubase.waitFor(name, seconds): takes the next event with that name, waiting up to seconds (default 10);
	//   nil on timeout.
	// - gnubase.events(): takes every event that arrived and hasn't been taken.
	// - gnubase.test: the test framework (testing.lua). The test cases a script registers run when it ends; their
	//   report is printed, and if one fails, the script fails. So no command area may be called test.
	// print writes to the output given to the constructor. Use a runtime on one thread, never an audio thread.
	// Functions that aren't engine commands can be added next to them, e.g. the UI's gnubase.ui (app/uitools.h).
	class Runtime
	{
	public:
		Runtime(engine::Engine& engine, Trust trust, std::ostream& output);
		~Runtime();
		Runtime(const Runtime&) = delete;
		Runtime& operator=(const Runtime&) = delete;

		// Runs Lua code. Returns nothing on success, otherwise the error with a traceback. chunkName appears in
		// error messages, e.g. "check.lua:3: too quiet".
		std::optional<std::string> run(std::string_view code, const std::string& chunkName);

		// Runs a Lua file. A first line starting with # (e.g. #!/usr/bin/env ...) is ignored.
		std::optional<std::string> runFile(const std::filesystem::path& file);

		// Stops every later run with an error once its Lua code has run longer than the limit. Time spent in engine
		// commands counts too, but only Lua code is interrupted. Zero, the default, means no limit.
		void setTimeLimit(std::chrono::milliseconds limit) { m_timeLimit = limit; }

		// Adds the function as gnubase.<area>.<name>, e.g. "ui.click" as gnubase.ui.click. Lua calls it like a
		// command: with a table of parameters, getting its result as a table, and binary JSON, such as a PNG file's
		// bytes, as a string. What it throws raises a Lua error with the message. Throws std::logic_error if the name
		// is taken.
		using Function = std::function<engine::Json(const engine::Json& parameters)>;
		void addFunction(const std::string& name, Function function);

		// Takes the events that arrived and calls their gnubase.on handlers, for a runtime whose code doesn't run
		// commands or wait, such as the UI's (app/appui.h). Returns a handler's error, if one fails.
		std::optional<std::string> dispatchEvents();

		// The Lua state, for libraries that add to it, such as RmlUi's Lua plugin.
		lua_State* state() { return m_state.get(); }

	private:
		struct CloseState
		{
			void operator()(lua_State* state) const noexcept;
		};

		static Runtime& self(lua_State* state);
		static int callCommand(lua_State* state);
		static int callFunction(lua_State* state);
		static int print(lua_State* state);
		static int on(lua_State* state);
		static int waitFor(lua_State* state);
		static int events(lua_State* state);
		static void stopWhenLate(lua_State* state, lua_Debug* debug);

		std::optional<std::string> execute(std::string_view code, const std::string& luaChunkName);
		void openLibraries(Trust trust);
		void bindCommands(const engine::Engine& engine);
		void loadTestFramework();
		void takeEvents(lua_State* state);

		engine::Client m_client;
		std::ostream& m_output;
		std::deque<engine::Event> m_events;                 // arrived, not taken yet
		std::vector<std::unique_ptr<Function>> m_functions; // added with addFunction; Lua holds pointers to them
		std::chrono::milliseconds m_timeLimit{0};
		std::chrono::steady_clock::time_point m_deadline;
		std::unique_ptr<lua_State, CloseState> m_state; // closed first: its functions use the members above
	};
}
