-- SPDX-License-Identifier: GPL-3.0-or-later
-- User scripts and the script console (#209, docs/features/scripts.md): each Lua file in the user's script folder is
-- the key command script.<name>, which keys, menus and macros run as one undo step; the script console lists, runs and
-- reloads them, shows what they print and their errors, and runs the Lua typed into its prompt.
local test = gnubase.test
local ui = gnubase.ui

test.before(gnubase.project.new)

local folder = ui.scripts().folder

-- Steps the UI, which follows the engine's events in its steps, e.g. settings.changed.
local function step()
	pcall(ui.wait, {selector = "#no-such-element", seconds = 0.05})
end

-- Writes a user script and reloads the scripts; the test case's end removes it again.
local function write(name, code)
	local path = folder .. "/" .. name .. ".lua"
	local file = assert(io.open(path, "w"))
	file:write(code)
	file:close()
	test.cleanup(function()
		os.remove(path)
		ui.scripts()
	end)
	ui.scripts()
end

-- The script's row in the console: its index in ui.scripts().
local function row(name)
	for index, script in ipairs(ui.scripts().scripts) do
		if script.name == name then return "#script-" .. index end
	end
end

local function trackCount()
	return #gnubase.project.get().tracks
end

-- The console's window once the UI opened it, in its next step.
local function openConsole()
	ui.command{command = "window.scriptConsole"}
	for _ = 1, 50 do
		for _, window in ipairs(ui.windows().windows) do
			if not window.main then
				test.cleanup(function()
					if #ui.windows().windows > 1 then ui.command{command = "window.scriptConsole"} end
					step()
				end)
				return window.id
			end
		end
		step()
	end
	error("the script console didn't open")
end

test.case("the script folder starts with the example scripts, each a key command", function()
	test.ok(folder ~= "")
	local commands = {}
	for _, script in ipairs(ui.scripts().scripts) do
		commands[script.name] = script.command
		test.ok(script.file:find(script.name, 1, true), script.file)
	end
	test.equal(commands["add-audio-tracks"], "script.add-audio-tracks")
	test.equal(commands["list-tracks"], "script.list-tracks")
	ui.command{command = "script.add-audio-tracks"}
	test.equal(trackCount(), 4)
	gnubase.history.undo()
	test.equal(trackCount(), 0)
end)

test.case("a user script bound to a key command runs and is undone in one step", function()
	write("three-tracks", [[
for _ = 1, 3 do gnubase.track.add{type = "audio"} end
gnubase.track.set{track = gnubase.project.get().tracks[1].id, name = "Lead"}
]])
	gnubase.settings.set{key = "ui.keyCommands", value = {{command = "script.three-tracks", keys = {"Mod+Shift+K"}}}}
	test.cleanup(function() gnubase.settings.reset{key = "ui.keyCommands"} end)
	step()
	local steps = #gnubase.history.get().steps

	ui.key{keys = "Mod+Shift+K"}
	test.equal(trackCount(), 3)
	local history = gnubase.history.get()
	test.equal(#history.steps, steps + 1)
	test.equal(history.steps[#history.steps].action, "three-tracks")

	gnubase.history.undo()
	test.equal(trackCount(), 0)
	gnubase.history.redo()
	test.equal(trackCount(), 3)
	test.equal(gnubase.project.get().tracks[1].name, "Lead")
end)

test.case("a macro runs scripts like any other command", function()
	write("one-track", 'gnubase.track.add{type = "midi"}')
	gnubase.settings.set{key = "ui.macros",
		value = {{name = "Two Tracks", commands = {{command = "script.one-track"}, {command = "script.one-track"}}}}}
	test.cleanup(function() gnubase.settings.reset{key = "ui.macros"} end)
	step()
	ui.command{command = "macro.Two Tracks"}
	test.equal(trackCount(), 2)
end)

test.case("the script console lists, runs and reloads the scripts and runs what is typed into its prompt", function()
	write("hello", 'print("hello from a script")')
	local window = openConsole()
	ui.wait{window = window, selector = row("hello"), text = "hello"}

	-- Run shows what the script printed.
	ui.click{window = window, selector = row("hello") .. " .run"}
	ui.wait{window = window, selector = "#script-output", text = "hello from a script"}

	-- Reload finds a new script.
	local path = folder .. "/second.lua"
	local file = assert(io.open(path, "w"))
	file:write('gnubase.track.add{type = "midi"}')
	file:close()
	test.cleanup(function() os.remove(path) end)
	ui.click{window = window, selector = "#script-reload"}
	ui.wait{window = window, selector = row("second"), text = "second"}

	-- The prompt runs Lua as one undo step and keeps its globals for the next line.
	ui.click{window = window, selector = "#script-prompt"}
	ui.text{window = window, text = 'count = 2; for _ = 1, count do gnubase.track.add{type = "audio"} end'}
	ui.key{window = window, keys = "Enter"}
	test.equal(trackCount(), 2)
	ui.text{window = window, text = 'print("tracks: " .. #gnubase.project.get().tracks * count)'}
	ui.key{window = window, keys = "Enter"}
	ui.wait{window = window, selector = "#script-output", text = "tracks: 4"}
	local history = gnubase.history.get()
	test.equal(history.steps[#history.steps].action, "Script Console")
	gnubase.history.undo()
	test.equal(trackCount(), 0)

	-- Errors show too, and Clear empties the output.
	ui.text{window = window, text = 'error("typed wrong")'}
	ui.key{window = window, keys = "Enter"}
	ui.wait{window = window, selector = "#script-output", text = "typed wrong"}
	ui.click{window = window, selector = "#script-clear"}
	step()
	test.equal(ui.dom{window = window, selector = "#script-output", depth = 0}.elements[1].text, nil)
end)

test.case("a script that fails is one undo step too, and its error shows in the console", function()
	write("fails", 'gnubase.track.add{type = "audio"}\ngnubase.track.add{type = "audio"}\nerror("stopped here")')
	ui.command{command = "script.fails"}
	test.equal(trackCount(), 2)
	gnubase.history.undo()
	test.equal(trackCount(), 0)
	local window = openConsole()
	ui.wait{window = window, selector = "#script-output", text = "stopped here"}
end)
