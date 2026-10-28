// SPDX-License-Identifier: GPL-3.0-or-later
#include "scripting/reference.h"

#include <algorithm>
#include <sstream>

namespace gnubase::scripting
{
	using engine::Json;

	namespace
	{
		constexpr const char* g_introduction = R"(# Lua API

Generated from the engine's command registry; don't edit it by hand. The tests check that it is up to date: run them
with `GNUBASE_UPDATE_DOCS=1` to write it again.

Every engine command is a function of the global table `gnubase`: `track.add` is `gnubase.track.add`. Pass the
parameters as a table and get the result as a table. A failed command raises an error with the reason, which `pcall`
catches. Commands run one after the other on the script's thread, so coroutines can call them as well.

```lua
local version = gnubase.engine.version().version
local ok, problem = pcall(gnubase.track.add, {type = "nonsense"})
print(version, ok, problem)
```

## Events

- `gnubase.on(name, handler)`: calls `handler(event)` for every event with that name, or `"*"` for all, while the
  script runs commands or waits.
- `gnubase.waitFor(name, seconds)`: takes the next event with that name, waiting up to `seconds` (default 10);
  `nil` on timeout.
- `gnubase.events()`: takes every event that arrived and hasn't been taken.

An event is a table with `name`, `data` (what changed) and `request` (the command that caused it).

## Running scripts

- `gnubase --headless --script file.lua` runs a trusted script: every standard library except `os.exit`. A script
  that raises an error ends GNUbase with exit code 4.
- The MCP tool `script_run` runs untrusted code: no `io`, `os`, `debug`, `package`, `require`, `dofile` or
  `loadfile`, `load` takes source code only, and Lua code may run for at most 60 seconds. It returns what the script
  printed.

## Commands
)";

		std::string typeOf(const Json& schema)
		{
			if (const auto choices = schema.find("enum"); choices != schema.end())
			{
				std::string text;
				for (const Json& choice : *choices)
					text += (text.empty() ? "" : " | ") + choice.dump();
				return "one of " + text;
			}
			const Json type = schema.value("type", Json());
			if (type.is_array())
			{
				std::string text;
				for (const Json& name : type)
					text += (text.empty() ? "" : " or ") + name.get<std::string>();
				return text;
			}
			if (!type.is_string())
				return "any";
			if (type == "array" && schema.contains("items"))
				return "array of " + typeOf(schema.at("items"));
			return type.get<std::string>();
		}

		void listProperties(std::ostream& out, const Json& schema, const char* heading)
		{
			const Json properties = schema.value("properties", Json::object());
			if (properties.empty())
				return;
			const Json required = schema.value("required", Json::array());
			out << '\n' << heading << ":\n\n";
			for (const auto& [name, property] : properties.items())
			{
				const bool isRequired = std::find(required.begin(), required.end(), name) != required.end();
				out << "- `" << name << "`: " << typeOf(property) << (isRequired ? ", required" : "");
				if (const auto description = property.find("description"); description != property.end())
					out << ". " << description->get<std::string>();
				out << '\n';
			}
		}
	}

	std::string reference(const engine::Engine& engine)
	{
		std::ostringstream out;
		out << g_introduction;
		std::string area;
		for (const engine::CommandInfo& info : engine.commands())
		{
			const std::string commandArea = info.name.substr(0, info.name.find('.'));
			if (commandArea != area)
			{
				area = commandArea;
				out << "\n### " << area << '\n';
			}
			out << "\n#### `gnubase." << info.name << "`\n\n" << info.description << '\n';
			if (info.query)
				out << "\nOnly reads; changes nothing.\n";
			listProperties(out, info.parameters, "Parameters");
			listProperties(out, info.result, "Returns");
		}
		return out.str();
	}
}
