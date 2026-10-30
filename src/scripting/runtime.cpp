// SPDX-License-Identifier: GPL-3.0-or-later
#include "scripting/runtime.h"

#include "platform/rt/realtime.h"

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iterator>
#include <ostream>
#include <sstream>
#include <stdexcept>

namespace gnubase::scripting
{
	using engine::Json;

	namespace
	{
		constexpr int g_maxDepth = 100;
		constexpr size_t g_maxQueuedEvents = 10000;
		constexpr std::chrono::minutes g_callTimeout{10}; // long enough for an offline render
		constexpr const char* g_handlersKey = "gnubase.handlers";
		constexpr const char* g_finishTestsKey = "gnubase.test.finish";

		// testing.lua, the test framework, as bytes: CMake generates them, because MSVC limits the length of string
		// literals.
		constexpr unsigned char g_testFramework[] = {
#include "testing.lua.inc"
		};

		// Lua is compiled as C++, so lua_error throws and the destructors of C++ frames run.
		void push(lua_State* state, const Json& value)
		{
			luaL_checkstack(state, 3, "the value is nested too deep");
			switch (value.type())
			{
			case Json::value_t::boolean:
				lua_pushboolean(state, value.get<bool>() ? 1 : 0);
				break;
			case Json::value_t::number_integer:
			case Json::value_t::number_unsigned:
				lua_pushinteger(state, value.get<lua_Integer>());
				break;
			case Json::value_t::number_float:
				lua_pushnumber(state, value.get<double>());
				break;
			case Json::value_t::string:
			{
				const auto& text = value.get_ref<const std::string&>();
				lua_pushlstring(state, text.data(), text.size());
				break;
			}
			case Json::value_t::array:
			{
				lua_createtable(state, static_cast<int>(value.size()), 0);
				lua_Integer index = 1;
				for (const Json& element : value)
				{
					push(state, element);
					lua_rawseti(state, -2, index++);
				}
				break;
			}
			case Json::value_t::object:
				lua_createtable(state, 0, static_cast<int>(value.size()));
				for (const auto& [key, element] : value.items())
				{
					push(state, element);
					lua_setfield(state, -2, key.c_str());
				}
				break;
			default: // null, and the binary type JSON text never has
				lua_pushnil(state);
				break;
			}
		}

		// A sequence 1..n and nothing else is an array; any other table is an object with string keys.
		Json toJson(lua_State* state, int index, int depth)
		{
			if (depth > g_maxDepth)
				throw std::runtime_error("the table is nested too deep, or contains itself");
			index = lua_absindex(state, index);
			switch (lua_type(state, index))
			{
			case LUA_TNIL:
				return nullptr;
			case LUA_TBOOLEAN:
				return lua_toboolean(state, index) != 0;
			case LUA_TNUMBER:
				return lua_isinteger(state, index) != 0 ? Json(lua_tointeger(state, index))
														: Json(lua_tonumber(state, index));
			case LUA_TSTRING:
			{
				size_t length = 0;
				const char* text = lua_tolstring(state, index, &length);
				return std::string(text, length);
			}
			case LUA_TTABLE:
			{
				luaL_checkstack(state, 3, "the table is nested too deep");
				const auto length = static_cast<lua_Integer>(lua_rawlen(state, index));
				lua_Integer keys = 0;
				lua_pushnil(state);
				while (lua_next(state, index) != 0)
				{
					++keys;
					lua_pop(state, 1);
				}
				if (length > 0 && keys == length)
				{
					Json array = Json::array();
					for (lua_Integer i = 1; i <= length; ++i)
					{
						lua_rawgeti(state, index, i);
						array.push_back(toJson(state, -1, depth + 1));
						lua_pop(state, 1);
					}
					return array;
				}
				Json object = Json::object();
				lua_pushnil(state);
				while (lua_next(state, index) != 0)
				{
					if (lua_type(state, -2) != LUA_TSTRING)
						throw std::runtime_error("table keys must be strings, except in lists");
					object[lua_tostring(state, -2)] = toJson(state, -1, depth + 1);
					lua_pop(state, 1);
				}
				return object;
			}
			default:
				throw std::runtime_error(std::string("a ") + luaL_typename(state, index) + " can't be sent to GNUbase");
			}
		}

		void pushEvent(lua_State* state, const engine::Event& event)
		{
			lua_createtable(state, 0, 3);
			lua_pushlstring(state, event.name.data(), event.name.size());
			lua_setfield(state, -2, "name");
			push(state, event.data);
			lua_setfield(state, -2, "data");
			lua_pushinteger(state, static_cast<lua_Integer>(event.request));
			lua_setfield(state, -2, "request");
		}

		// Calls the handlers registered with gnubase.on for the event's name and for "*".
		void dispatch(lua_State* state, const engine::Event& event)
		{
			lua_getfield(state, LUA_REGISTRYINDEX, g_handlersKey);
			for (const std::string& name : {event.name, std::string("*")})
			{
				if (lua_getfield(state, -1, name.c_str()) == LUA_TTABLE)
				{
					const auto count = static_cast<lua_Integer>(lua_rawlen(state, -1));
					for (lua_Integer i = 1; i <= count; ++i)
					{
						lua_rawgeti(state, -1, i);
						pushEvent(state, event);
						lua_call(state, 1, 0); // errors in handlers stop the script, like any other error
					}
				}
				lua_pop(state, 1);
			}
			lua_pop(state, 1);
		}

		// The message handler for run: adds a traceback to the error.
		int traceback(lua_State* state)
		{
			const char* message = lua_tostring(state, 1);
			luaL_traceback(state, state, message != nullptr ? message : "(the error is not a string)", 1);
			return 1;
		}
	}

	void Runtime::CloseState::operator()(lua_State* state) const noexcept
	{
		lua_close(state);
	}

	Runtime::Runtime(engine::Engine& engine, Trust trust, std::ostream& output)
		: m_client(engine, "lua"), m_output(output), m_state(luaL_newstate())
	{
		rt::assertNotRealtime("scripting::Runtime");
		if (!m_state)
			throw std::runtime_error("Lua has no memory");
		*static_cast<Runtime**>(lua_getextraspace(m_state.get())) = this; // for hooks; coroutines get a copy
		openLibraries(trust);
		bindCommands(engine);
		loadTestFramework();
	}

	Runtime::~Runtime() = default;

	void Runtime::openLibraries(Trust trust)
	{
		lua_State* state = m_state.get();
		if (trust == Trust::Trusted)
		{
			luaL_openlibs(state);
			lua_getglobal(state, LUA_OSLIBNAME);
			lua_pushnil(state);
			lua_setfield(state, -2, "exit");
			lua_pop(state, 1);
		}
		else
		{
			const std::pair<const char*, lua_CFunction> libraries[] = {{LUA_GNAME, luaopen_base},
				{LUA_COLIBNAME, luaopen_coroutine}, {LUA_TABLIBNAME, luaopen_table}, {LUA_STRLIBNAME, luaopen_string},
				{LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8}};
			for (const auto& [name, open] : libraries)
			{
				luaL_requiref(state, name, open, 1);
				lua_pop(state, 1);
			}
			// No files, and source code only: binary chunks can break out of the sandbox.
			const char* restrict = R"(
				dofile, loadfile = nil, nil
				local rawload = load
				load = function(chunk, name, _, ...) return rawload(chunk, name, "t", ...) end
			)";
			if (luaL_dostring(state, restrict) != LUA_OK)
				throw std::runtime_error(lua_tostring(state, -1));
		}

		lua_pushlightuserdata(state, this);
		lua_pushcclosure(state, &Runtime::print, 1);
		lua_setglobal(state, "print");
	}

	void Runtime::bindCommands(const engine::Engine& engine)
	{
		lua_State* state = m_state.get();
		lua_newtable(state);
		lua_setfield(state, LUA_REGISTRYINDEX, g_handlersKey);

		lua_newtable(state); // gnubase
		const std::pair<const char*, lua_CFunction> helpers[] = {
			{"on", &Runtime::on}, {"waitFor", &Runtime::waitFor}, {"events", &Runtime::events}};
		for (const auto& [name, function] : helpers)
		{
			lua_pushlightuserdata(state, this);
			lua_pushcclosure(state, function, 1);
			lua_setfield(state, -2, name);
		}
		for (const engine::CommandInfo& info : engine.commands())
		{
			// "track.add" becomes gnubase.track.add; a name without area sits in gnubase itself.
			const size_t dot = info.name.find('.');
			const bool hasArea = dot != std::string::npos;
			if (hasArea)
			{
				const std::string area = info.name.substr(0, dot);
				if (lua_getfield(state, -1, area.c_str()) != LUA_TTABLE)
				{
					lua_pop(state, 1);
					lua_newtable(state);
					lua_pushvalue(state, -1);
					lua_setfield(state, -3, area.c_str());
				}
			}
			lua_pushlightuserdata(state, this);
			lua_pushlstring(state, info.name.data(), info.name.size());
			lua_pushcclosure(state, &Runtime::callCommand, 2);
			lua_setfield(state, -2, hasArea ? info.name.c_str() + dot + 1 : info.name.c_str());
			if (hasArea)
				lua_pop(state, 1);
		}
		lua_setglobal(state, "gnubase");
	}

	void Runtime::loadTestFramework()
	{
		lua_State* state = m_state.get();
		lua_getglobal(state, "gnubase");
		if (lua_getfield(state, -1, "test") != LUA_TNIL)
			throw std::logic_error("a command area is called test, but gnubase.test is the test framework");
		lua_pop(state, 1);
		// The chunk gets the message handler for tracebacks and returns gnubase.test and the function that runs the
		// test cases when a script ends.
		const auto* source = reinterpret_cast<const char*>(g_testFramework);
		if (luaL_loadbufferx(state, source, sizeof(g_testFramework), "=gnubase.test", "t") != LUA_OK)
			throw std::runtime_error(lua_tostring(state, -1));
		lua_pushcfunction(state, traceback);
		if (lua_pcall(state, 1, 2, 0) != LUA_OK)
			throw std::runtime_error(lua_tostring(state, -1));
		lua_setfield(state, LUA_REGISTRYINDEX, g_finishTestsKey);
		lua_setfield(state, -2, "test");
		lua_pop(state, 1);
	}

	std::optional<std::string> Runtime::run(std::string_view code, const std::string& chunkName)
	{
		return execute(code, "=" + chunkName); // shown as is in messages
	}

	std::optional<std::string> Runtime::execute(std::string_view code, const std::string& luaChunkName)
	{
		rt::assertNotRealtime("scripting::Runtime::run");
		lua_State* state = m_state.get();
		const int top = lua_gettop(state);
		if (m_timeLimit.count() > 0)
		{
			m_deadline = std::chrono::steady_clock::now() + m_timeLimit;
			lua_sethook(state, &Runtime::stopWhenLate, LUA_MASKCOUNT, 10000);
		}
		const auto message = [state]() -> std::string
		{
			const char* text = lua_tostring(state, -1);
			return text != nullptr ? text : "(the error is not a string)";
		};
		lua_pushcfunction(state, traceback);
		int status = luaL_loadbufferx(state, code.data(), code.size(), luaChunkName.c_str(), "t");
		if (status == LUA_OK)
			status = lua_pcall(state, 0, 0, top + 1);
		std::optional<std::string> error;
		if (status != LUA_OK)
			error = message();
		lua_settop(state, top);

		// The test cases the script registered run now, or are dropped if it failed. They print their report as they
		// run, so the error only sums it up, without a traceback.
		lua_getfield(state, LUA_REGISTRYINDEX, g_finishTestsKey);
		lua_pushboolean(state, error ? 0 : 1);
		if (lua_pcall(state, 1, 0, 0) != LUA_OK && !error)
			error = message();
		lua_settop(state, top);
		return error;
	}

	std::optional<std::string> Runtime::runFile(const std::filesystem::path& file)
	{
		std::ifstream in(file, std::ios::binary);
		if (!in)
			return "can't open " + file.u8string();
		std::string code{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
		if (!code.empty() && code.front() == '#')
			code.insert(0, "--");                    // keeps the line numbers
		return execute(code, "@" + file.u8string()); // a long path keeps its end in messages
	}

	void Runtime::stopWhenLate(lua_State* state, lua_Debug* /*debug*/)
	{
		const Runtime& runtime = **static_cast<Runtime**>(lua_getextraspace(state));
		if (std::chrono::steady_clock::now() > runtime.m_deadline)
			luaL_error(state, "the script ran longer than %d ms", static_cast<int>(runtime.m_timeLimit.count()));
	}

	Runtime& Runtime::self(lua_State* state)
	{
		return *static_cast<Runtime*>(lua_touserdata(state, lua_upvalueindex(1)));
	}

	int Runtime::callCommand(lua_State* state)
	{
		Runtime& runtime = self(state);
		const std::string command = lua_tostring(state, lua_upvalueindex(2));
		std::string error;
		try
		{
			const Json parameters = lua_isnoneornil(state, 1) ? Json::object() : toJson(state, 1, 0);
			const engine::Result result = runtime.m_client.call(command, parameters, g_callTimeout);
			if (result.ok())
			{
				runtime.takeEvents(state);
				push(state, result.value);
				return 1;
			}
			error = result.error;
		}
		catch (const std::exception& exception)
		{
			error = exception.what();
		}
		runtime.takeEvents(state); // a failed command has no events, but others may have arrived
		return luaL_error(state, "%s", error.c_str());
	}

	int Runtime::print(lua_State* state)
	{
		std::ostream& output = self(state).m_output;
		const int count = lua_gettop(state);
		for (int i = 1; i <= count; ++i)
		{
			size_t length = 0;
			const char* text = luaL_tolstring(state, i, &length);
			if (i > 1)
				output << '\t';
			output.write(text, static_cast<std::streamsize>(length));
			lua_pop(state, 1);
		}
		output << '\n';
		output.flush();
		return 0;
	}

	int Runtime::on(lua_State* state)
	{
		luaL_checkstring(state, 1);
		luaL_checktype(state, 2, LUA_TFUNCTION);
		lua_getfield(state, LUA_REGISTRYINDEX, g_handlersKey);
		if (lua_getfield(state, -1, lua_tostring(state, 1)) != LUA_TTABLE)
		{
			lua_pop(state, 1);
			lua_newtable(state);
			lua_pushvalue(state, -1);
			lua_setfield(state, -3, lua_tostring(state, 1));
		}
		lua_pushvalue(state, 2);
		lua_rawseti(state, -2, static_cast<lua_Integer>(lua_rawlen(state, -2)) + 1);
		return 0;
	}

	int Runtime::waitFor(lua_State* state)
	{
		Runtime& runtime = self(state);
		const std::string name = luaL_checkstring(state, 1);
		const double seconds = luaL_optnumber(state, 2, 10.0);
		const auto deadline = std::chrono::steady_clock::now() +
			std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
		runtime.takeEvents(state);
		for (;;)
		{
			const auto event = std::find_if(runtime.m_events.begin(), runtime.m_events.end(),
				[&name](const engine::Event& candidate) { return candidate.name == name; });
			if (event != runtime.m_events.end())
			{
				pushEvent(state, *event);
				runtime.m_events.erase(event);
				return 1;
			}
			const auto now = std::chrono::steady_clock::now();
			if (now >= deadline)
			{
				lua_pushnil(state);
				return 1;
			}
			const std::optional<engine::Message> message =
				runtime.m_client.wait(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
			if (message && std::holds_alternative<engine::Event>(*message))
			{
				const auto& arrived = std::get<engine::Event>(*message);
				runtime.m_events.push_back(arrived);
				dispatch(state, arrived);
			}
		}
	}

	int Runtime::events(lua_State* state)
	{
		Runtime& runtime = self(state);
		runtime.takeEvents(state);
		lua_createtable(state, static_cast<int>(runtime.m_events.size()), 0);
		lua_Integer index = 1;
		for (const engine::Event& event : runtime.m_events)
		{
			pushEvent(state, event);
			lua_rawseti(state, -2, index++);
		}
		runtime.m_events.clear();
		return 1;
	}

	void Runtime::takeEvents(lua_State* state)
	{
		while (std::optional<engine::Message> message = m_client.poll())
		{
			if (!std::holds_alternative<engine::Event>(*message))
				continue;
			const auto& event = std::get<engine::Event>(*message);
			m_events.push_back(event);
			if (m_events.size() > g_maxQueuedEvents)
				m_events.pop_front();
			dispatch(state, event);
		}
	}
}
