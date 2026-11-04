-- SPDX-License-Identifier: GPL-3.0-or-later
-- gnubase.ui.command (docs/features/ui-lua.md): runs a key command in the main window, as documents' buttons do, and
-- refuses a command that doesn't exist or parameters it doesn't take at the call, as KeyCommands::check does (#669).
local test = gnubase.test
local ui = gnubase.ui

test.before(gnubase.project.new)

test.case("gnubase.ui.command runs UI commands and engine commands with their parameters", function()
	-- A UI command: the transport panel opens in a window of its own, and closes again.
	ui.command{command = "window.transportPanel"}
	test.equal(#ui.windows().windows, 2)
	ui.command{command = "window.transportPanel", parameters = {}}
	test.equal(#ui.windows().windows, 1)

	-- An engine command with its parameters, sent before the next command of this script runs.
	gnubase.transport.set{left = 48000, right = 96000}
	ui.command{command = "transport.locate", parameters = {target = "rightLocator"}}
	test.equal(gnubase.transport.get().position, 96000)
end)

test.case("gnubase.ui.command refuses a command that doesn't exist and parameters the command doesn't take", function()
	gnubase.transport.set{left = 48000, right = 96000}
	local position = gnubase.transport.get().position
	test.raises(function() ui.command{command = "transport.ply"} end, "there is no command transport.ply")
	test.raises(function() ui.command{command = "window.transportPanl"} end, "there is no command window.transportPanl")
	test.raises(function() ui.command{command = "transport.locate", parameters = {target = 5}} end, "target")
	test.raises(function() ui.command{command = "transport.locate", parameters = {traget = "rightLocator"}} end,
		"traget")
	test.raises(function() ui.command{command = "transport.locate", parameters = {1, 2}} end, "JSON object")
	-- A UI command takes no parameters; they aren't dropped.
	test.raises(function() ui.command{command = "window.transportPanel", parameters = {size = 2}} end,
		"window.transportPanel takes no parameters")
	-- The call itself: a command by name, and nothing else.
	test.raises(function() ui.command{} end, "ui.command needs a command")
	test.raises(function() ui.command{command = 5} end, "ui.command needs a command")
	test.raises(function() ui.command{command = "window.transportPanel", parameter = {}} end, "not parameter")

	-- None of them ran: no window opened, and the cursor didn't move.
	test.equal(#ui.windows().windows, 1)
	test.equal(gnubase.transport.get().position, position)
end)
