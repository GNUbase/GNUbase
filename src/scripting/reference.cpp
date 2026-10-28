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

## Tests

`gnubase.test` is a small test framework. A script registers test cases, which run when the script ends, in order.
The report is printed in TAP format: `ok 1 - name`, or `not ok 1 - name` with the failure on lines starting with `#`.
If a test case fails, the script fails, so `gnubase --headless --script` exits with code 4.

```lua
local test = gnubase.test

test.before(gnubase.project.new) -- every test case starts with a new project

test.case("adding a track is one undo step", function()
	local added = gnubase.track.add{type = "audio"}
	test.equal(gnubase.project.get().tracks, {added.track})
	gnubase.history.undo()
	test.equal(gnubase.project.get().tracks, {})
end)
```

A failure names the file and line of the failing call: `tests/lua/project.lua:9: at [1].name: expected "Audio 01",
got "Audio 02"`. Errors without a position, like those of commands and `assert`, get the line in the test case that
caused them.

Assertions take an optional message last, which goes in front of the failure:

- `test.ok(value)`: anything but `false` and `nil`.
- `test.equal(actual, expected)`, `test.notEqual(actual, unexpected)`: tables are compared by their contents, and a
  failure names the first difference.
- `test.near(actual, expected, tolerance)`: numbers, or lists of numbers, within the tolerance.
- `test.matches(text, pattern)`: text that matches a Lua pattern.
- `test.raises(fn, text)`: `fn` raises an error whose message contains `text` (plain text, optional). Returns the
  message.
- `test.fail(message)`.

Fixtures:

- `test.before(fn)`, `test.after(fn)`: run before and after every test case of the script. A failing before hook skips
  the test case; after hooks run even when it fails.
- `test.fixture(setup)`: returns a function that gives the fixture's value. `setup` runs the first time a test case
  asks for it, and can return a teardown function after the value, which runs when the test case ends.
- `test.cleanup(fn)`: runs `fn` when the test case ends. Cleanups and teardowns run last first, before the after hooks.

Golden files, in trusted scripts only:

- `test.golden(name, audio, tolerance, options)`: compares `audio`, a table `{samples = {...}, channels = 1,
  sampleRate = 48000}` (interleaved samples; the defaults shown), with the golden file `<name>.wav`: 32-bit float WAV,
  like the C++ tests' golden files. A mismatch fails and writes the actual audio and the difference to the output
  folder. `options` can set `goldenDir`, `outputDir` and `update` (write the golden file instead of comparing). They
  default to the environment variables `GNUBASE_GOLDEN_DIR` (else `tests/golden`), `GNUBASE_TEST_OUTPUT_DIR` (else the
  current folder) and `GNUBASE_UPDATE_GOLDEN=1`.
- `test.readWav(path)` returns such audio from a 32-bit float WAV file; `test.writeWav(path, audio)` writes one.

Every `tests/lua/*.lua` is a ctest test with the label `lua`, which runs `gnubase --headless --script` in the source
folder. AI agents run test cases with the MCP tool `script_run`, whose output has the report.

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
