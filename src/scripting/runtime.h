// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

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
	// print writes to the output given to the constructor. Use a runtime on one thread, never an audio thread.
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

	private:
		struct CloseState
		{
			void operator()(lua_State* state) const noexcept;
		};

		static Runtime& self(lua_State* state);
		static int callCommand(lua_State* state);
		static int print(lua_State* state);
		static int on(lua_State* state);
		static int waitFor(lua_State* state);
		static int events(lua_State* state);

		std::optional<std::string> execute(std::string_view code, const std::string& luaChunkName);
		void openLibraries(Trust trust);
		void bindCommands(const engine::Engine& engine);
		void takeEvents(lua_State* state);

		engine::Client m_client;
		std::ostream& m_output;
		std::deque<engine::Event> m_events;             // arrived, not taken yet
		std::unique_ptr<lua_State, CloseState> m_state; // closed first: its functions use the members above
	};
}
