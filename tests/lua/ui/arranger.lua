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
