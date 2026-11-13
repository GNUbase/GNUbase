-- SPDX-License-Identifier: GPL-3.0-or-later
-- The arranger editor (docs/features/arranger.md, #167): window.arranger opens it in a window of its own, its lists
-- follow the engine, and its buttons change the chain through the engine API.
local test = gnubase.test
local ui = gnubase.ui

test.before(gnubase.project.new)

-- The window that isn't the main one.
local function arrangerWindow()
	for _, window in ipairs(ui.windows().windows) do
		if not window.main then return window.id end
	end
end

-- The arranger window once the UI has opened it: window.arranger opens it in the UI's next step, which a wait steps.
local function openedWindow()
	for _ = 1, 50 do
		local window = arrangerWindow()
		if window then return window end
		pcall(ui.wait, {selector = "#no-such-element", seconds = 0.05})
	end
end

local function steps()
	return gnubase.arranger.get().chains[1].steps
end

test.case("the arranger editor builds a chain from the events and plays its steps", function()
	test.cleanup(function()
		if #ui.windows().windows > 1 then ui.command{command = "window.arranger"} end
	end)
	gnubase.track.add{type = "arranger", timeBase = "linear"}
	local a = gnubase.arranger.add{position = 0, ["end"] = 48000}.event
	local b = gnubase.arranger.add{position = 48000, ["end"] = 96000}.event
	ui.command{command = "window.arranger"}
	local window = openedWindow()
	test.ok(window)
	ui.wait{window = window, selector = "#event-" .. b.id}

	-- Append from the events list: B, then A.
	ui.click{window = window, selector = "#event-" .. b.id .. " .append"}
	ui.wait{window = window, selector = "#step-0"}
	ui.click{window = window, selector = "#event-" .. a.id .. " .append"}
	ui.wait{window = window, selector = "#step-1"}
	test.equal(#steps(), 2)
	test.equal(steps()[1].event, b.id)
	test.equal(steps()[2].event, a.id)

	-- Repeats and removing a step.
	ui.click{window = window, selector = "#step-1 .more"}
	test.equal(steps()[2].repeats, 2)
	ui.wait{window = window, selector = "#step-1 .repeats", text = "2"}
	ui.click{window = window, selector = "#step-0 .remove"}
	test.equal(#steps(), 1)
	test.equal(steps()[1].event, a.id)

	-- Arranger mode, then the step's arrow plays it from its event: the cursor goes there.
	ui.click{window = window, selector = "#arranger-mode"}
	test.ok(gnubase.arranger.get().mode)
	ui.wait{window = window, selector = "#arranger-mode.checked"}
	gnubase.transport.locate{position = 70000}
	ui.click{window = window, selector = "#step-0 .locate"}
	test.equal(gnubase.transport.get().position, 0)
	test.equal(gnubase.transport.get().arranger.step, 0)
	ui.wait{window = window, selector = "#step-0.current"}

	-- A new chain becomes the active one, empty.
	ui.click{window = window, selector = "#new-chain"}
	local arranger = gnubase.arranger.get()
	test.equal(#arranger.chains, 2)
	test.equal(arranger.chain, arranger.chains[2].id)
	ui.wait{window = window, selector = "#step-0", visible = false}

	-- window.arranger closes it again.
	ui.command{command = "window.arranger"}
	pcall(ui.wait, {selector = "#no-such-element", seconds = 0.1})
	test.equal(arrangerWindow(), nil)
end)

test.case("the editor disables its step buttons outside arranger mode and shows every locate arrow", function()
	test.cleanup(function()
		if #ui.windows().windows > 1 then ui.command{command = "window.arranger"} end
	end)
	gnubase.track.add{type = "arranger", timeBase = "linear"}
	local a = gnubase.arranger.add{position = 0, ["end"] = 48000}.event
	local b = gnubase.arranger.add{position = 48000, ["end"] = 96000}.event
	gnubase.arranger.setChain{steps = {{event = b.id}, {event = a.id}}} -- two steps, but arranger mode stays off
	ui.command{command = "window.arranger"}
	local window = openedWindow()
	test.ok(window)
	ui.wait{window = window, selector = "#step-1"}

	-- A toolbar button's disabled state, polled until it settles: a refresh that follows an engine event lands a UI
	-- step later. Reading the attribute through ui.dom doesn't need the button to be unclipped in the narrow window.
	local function disabled(id, want)
		for _ = 1, 50 do
			local element = ui.dom{window = window, selector = "#" .. id}.elements[1]
			local isDisabled = element ~= nil and element.attributes ~= nil and element.attributes.disabled ~= nil
			if isDisabled == want then return true end
			pcall(ui.wait, {window = window, selector = "#no-such-element", seconds = 0.05})
		end
		return false
	end

	-- Outside arranger mode the chain step buttons call arranger.goToStep, which refuses: they are disabled (#880).
	for _, id in ipairs({"first-step", "previous-step", "next-step", "first-repeat", "last-repeat"}) do
		test.ok(disabled(id, true), id .. " is disabled outside arranger mode")
	end
	-- The locate arrow shows on every row, not just the one playing, so each row's locate button is a real target (#880).
	for _, row in ipairs({"#step-0", "#step-1"}) do
		local arrow = ui.dom{window = window, selector = row .. " .locate-arrow"}.elements[1]
		test.ok(arrow ~= nil and arrow.visible, "the locate arrow shows on " .. row)
	end

	-- Arranger mode on: the step buttons that only need a chain work again.
	ui.click{window = window, selector = "#arranger-mode"}
	ui.wait{window = window, selector = "#arranger-mode.checked"}
	for _, id in ipairs({"first-step", "previous-step", "next-step"}) do
		test.ok(disabled(id, false), id .. " works in arranger mode")
	end
	-- First and Last Repeat also need a step of the chain playing: goToStep puts one there, and then they work.
	gnubase.arranger.goToStep{step = 0}
	ui.wait{window = window, selector = "#step-0.current"}
	test.ok(disabled("first-repeat", false), "first-repeat works with a step playing")
	test.ok(disabled("last-repeat", false), "last-repeat works with a step playing")
end)
