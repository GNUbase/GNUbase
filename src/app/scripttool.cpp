// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/scripttool.h"

#include "scripting/runtime.h"

#include <chrono>
#include <sstream>
#include <stdexcept>
#include <string>

namespace gnubase::app
{
	namespace
	{
		constexpr std::chrono::seconds g_timeLimit{60};
	}

	void addScriptTool(mcp::Server& server, engine::Engine& engine)
	{
		using engine::Json;
		server.addTool({"script_run",
			"Runs Lua code with GNUbase's API: every engine command as gnubase.<area>.<name>(parameters), e.g. "
			"gnubase.track.add{type = \"audio\"}, and events through gnubase.on, gnubase.waitFor and gnubase.events "
			"(see docs/lua.md). Test cases registered with gnubase.test.case run when the code ends. Runs in a sandbox "
			"without files, processes or bytecode, for at most 60 s of Lua code. "
			"Returns what the script printed; an error comes back with its traceback and what was printed until then.",
			Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["code"],
				"properties": {"code": {"type": "string"}, "name": {"type": "string"}}})"),
			Json::parse(R"({"type": "object", "properties": {"output": {"type": "string"}}, "required": ["output"]})"),
			false, [&engine](const Json& arguments)
			{
				std::ostringstream output;
				scripting::Runtime lua(engine, scripting::Trust::Untrusted, output);
				lua.setTimeLimit(g_timeLimit);
				const std::optional<std::string> error =
					lua.run(arguments.at("code").get<std::string>(), arguments.value("name", std::string("script")));
				if (error)
					throw std::runtime_error(output.str().empty() ? *error : output.str() + "\n" + *error);
				return Json{{"output", output.str()}};
			}});
	}
}
