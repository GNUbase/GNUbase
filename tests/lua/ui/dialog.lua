-- SPDX-License-Identifier: GPL-3.0-or-later
-- A UI test in Lua (#208). It drives GNUbase's UI with gnubase.ui, the MCP UI tools as functions, and reads the
-- engine with gnubase; the documents' own Lua (RmlUi's Lua plugin) reaches the engine through gnubase as well.
local test = gnubase.test
local ui = gnubase.ui

test.before(gnubase.project.new)

-- The main window's RmlUi context.
local main = rmlui.contexts[1]

-- Shows a document from tests/data/ui in the main window until the test case ends.
local function show(name)
	local document = main:LoadDocument("tests/data/ui/" .. name)
	test.ok(document, name)
	document:Show()
	test.cleanup(function() document:Close() end)
	return document
end

test.case("gnubase.ui has a function for every MCP UI tool", function()
	for _, name in ipairs{"windows", "dom", "screenshot", "wait", "move", "click", "drag", "wheel", "key", "text"} do
		test.equal(type(ui[name]), "function", name)
	end
	local windows = ui.windows().windows
	test.equal(#windows, 1)
	test.ok(windows[1].main)
end)

test.case("a dialog renames a track: simulated input, the dialog's Lua and the engine", function()
	local track = gnubase.track.add{type = "audio"}.track
	local document = show("lua-track.rml")
	document:GetElementById("rename"):SetAttribute("track", tostring(track.id))

	ui.click{selector = "#rename"} -- the button's Lua opens the dialog
	local field = ui.wait{selector = "#name"}.elements[1]
	test.equal(field.value, "Audio 01") -- the dialog's data model has the name from the engine

	ui.click{selector = "#name"}
	ui.key{keys = "Ctrl+A"}
	ui.text{text = "Bass"}
	ui.click{selector = "#ok"}
	ui.wait{selector = "#name", visible = false}
	test.equal(gnubase.project.get().tracks[1].name, "Bass")
end)

test.case("the functions check their parameters and fail like the MCP tools", function()
	show("automation.rml")
	test.raises(function() ui.click{selector = "#open", count = 9} end, "count")
	test.raises(function() ui.click{selector = "#nothing"} end, "no element matches #nothing")
	test.raises(function() ui.wait{selector = "#hidden", seconds = 0.05} end, "no visible element matches #hidden")
	test.equal(ui.dom{selector = "#search", depth = 0}.elements[1].tag, "input")

	-- A screenshot is a PNG file's bytes.
	local png = ui.screenshot{selector = "#open"}
	test.equal(png:sub(1, 4), "\x89PNG")
end)
