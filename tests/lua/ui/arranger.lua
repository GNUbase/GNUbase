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
	-- The rows lie below one another: no style for the arranger track's lane (.arranger-lane-row) places them.
	local first = ui.dom{window = window, selector = "#step-0", depth = 0}.elements[1].box
	local second = ui.dom{window = window, selector = "#step-1", depth = 0}.elements[1].box
	test.ok(second.y >= first.y + first.height - 1)

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

test.case("the editor renames the active chain", function()
	test.cleanup(function()
		if #ui.windows().windows > 1 then ui.command{command = "window.arranger"} end
	end)
	gnubase.track.add{type = "arranger", timeBase = "linear"}
	ui.command{command = "window.arranger"}
	local window = openedWindow()
	test.ok(window)
	ui.wait{window = window, selector = "#chain-name"}
	test.equal(ui.dom{window = window, selector = "#chain-name"}.elements[1].value, "Chain 1")

	-- Rename Arranger Chain (reference manual p. 406): Enter renames it, and the chain list shows the new name.
	ui.click{window = window, selector = "#chain-name"}
	ui.key{window = window, keys = "Mod+A"}
	ui.text{window = window, text = "Verse Mix"}
	ui.key{window = window, keys = "Enter"}
	-- The field's change and arranger.changed land in later UI steps, which a wait steps.
	local function settled(read, want)
		local value
		for _ = 1, 50 do
			value = read()
			if value == want then break end
			pcall(ui.wait, {window = window, selector = "#no-such-element", seconds = 0.05})
		end
		return value
	end
	test.equal(settled(function() return gnubase.arranger.get().chains[1].name end, "Verse Mix"), "Verse Mix")
	test.equal(settled(function()
		return ui.dom{window = window, selector = "#arranger-chain"}.elements[1].options[1].text
	end, "Verse Mix"), "Verse Mix")

	-- An empty name keeps the old one.
	ui.key{window = window, keys = "Mod+A"}
	ui.key{window = window, keys = "Backspace"}
	ui.key{window = window, keys = "Enter"}
	test.equal(settled(function()
		return ui.dom{window = window, selector = "#chain-name"}.elements[1].value
	end, "Verse Mix"), "Verse Mix")
	test.equal(gnubase.arranger.get().chains[1].name, "Verse Mix")
	-- Refreshing the drop-down lists replaces their options instead of adding to them.
	test.equal(#ui.dom{window = window, selector = "#arranger-chain"}.elements[1].options, 1)
	test.equal(#ui.dom{window = window, selector = "#jump-mode"}.elements[1].options, 6)
end)

test.case("the editor renames a step's arranger event with Alt and a double click", function()
	test.cleanup(function()
		if #ui.windows().windows > 1 then ui.command{command = "window.arranger"} end
	end)
	gnubase.track.add{type = "arranger", timeBase = "linear"}
	local a = gnubase.arranger.add{position = 0, ["end"] = 48000}.event
	gnubase.arranger.setChain{steps = {{event = a.id}}}
	-- The last case's cleanup closes its editor in a later UI step: once it has, window.arranger opens a new one.
	for _ = 1, 50 do
		if arrangerWindow() == nil then break end
		pcall(ui.wait, {selector = "#no-such-element", seconds = 0.05})
	end
	ui.command{command = "window.arranger"}
	local window = openedWindow()
	test.ok(window)
	ui.wait{window = window, selector = "#step-0 .name"}

	-- A plain double click doesn't open the name (reference manual p. 405: Alt and a double click).
	ui.click{window = window, selector = "#step-0 .name", count = 2}
	test.equal(ui.dom{window = window, selector = "#step-0 .name.editing"}.elements[1], nil)
	ui.click{window = window, selector = "#step-0 .name", count = 2, modifiers = {"alt"}}
	ui.wait{window = window, selector = "#step-0 .name.editing"}
	-- A transport change refreshes the editor, but leaves the name being typed alone.
	gnubase.transport.locate{position = 1000}
	pcall(ui.wait, {window = window, selector = "#no-such-element", seconds = 0.1})
	ui.wait{window = window, selector = "#step-0 .name.editing"}
	ui.key{window = window, keys = "Mod+A"}
	ui.text{window = window, text = "Intro"}
	ui.key{window = window, keys = "Enter"}
	for _ = 1, 50 do
		if gnubase.arranger.get().events[1].name == "Intro" then break end
		pcall(ui.wait, {window = window, selector = "#no-such-element", seconds = 0.05})
	end
	test.equal(gnubase.arranger.get().events[1].name, "Intro")
	ui.wait{window = window, selector = "#event-" .. a.id .. " .name", text = "Intro"}
end)

test.case("the editor selects steps and removes the selected ones", function()
	test.cleanup(function()
		if #ui.windows().windows > 1 then ui.command{command = "window.arranger"} end
	end)
	gnubase.track.add{type = "arranger", timeBase = "linear"}
	local a = gnubase.arranger.add{position = 0, ["end"] = 48000}.event
	local b = gnubase.arranger.add{position = 48000, ["end"] = 96000}.event
	gnubase.arranger.setChain{steps = {{event = a.id}, {event = b.id}, {event = a.id}, {event = b.id}}}
	gnubase.arranger.setMode{mode = true}
	for _ = 1, 50 do
		if arrangerWindow() == nil then break end
		pcall(ui.wait, {selector = "#no-such-element", seconds = 0.05})
	end
	ui.command{command = "window.arranger"}
	local window = openedWindow()
	test.ok(window)
	ui.wait{window = window, selector = "#step-3 .name"}
	local function selected()
		local rows = ""
		for step = 0, 3 do
			rows = rows .. (ui.dom{window = window, selector = "#step-" .. step .. ".selected"}.elements[1] and "x" or "-")
		end
		return rows
	end

	-- Reference manual p. 410: a click selects a step, Shift up to another one, Ctrl (Cmd) adds or removes one; the
	-- arrows and buttons don't select.
	ui.click{window = window, selector = "#step-1 .repeats"}
	test.equal(selected(), "-x--")
	ui.click{window = window, selector = "#step-3 .repeats", modifiers = {"shift"}}
	test.equal(selected(), "-xxx")
	ui.click{window = window, selector = "#step-2 .repeats", modifiers = {"ctrl"}}
	test.equal(selected(), "-x-x")
	ui.click{window = window, selector = "#step-0 .locate"}
	ui.wait{window = window, selector = "#step-0.current"}
	test.equal(selected(), "-x-x")
	-- A chain edit selects none, as the indices may name other steps now.
	ui.click{window = window, selector = "#step-0 .more"}
	ui.wait{window = window, selector = "#step-0 .repeats", text = "2"}
	test.equal(selected(), "----")
	ui.click{window = window, selector = "#step-1 .repeats"}
	ui.click{window = window, selector = "#step-3 .repeats", modifiers = {"ctrl"}}
	test.equal(selected(), "-x-x")

	-- Remove Selected on the context menu removes them.
	ui.click{window = window, selector = "#step-0 .repeats", button = "right"}
	ui.wait{window = window, selector = "#chain-step-menu.open"}
	ui.click{window = window, selector = "#chain-step-menu .remove-selected"}
	for _ = 1, 50 do
		if #steps() == 2 then break end
		pcall(ui.wait, {window = window, selector = "#no-such-element", seconds = 0.05})
	end
	test.equal(#steps(), 2)
	test.equal(steps()[1].event, a.id)
	test.equal(steps()[1].repeats, 2)
	test.equal(steps()[2].event, a.id)
end)

test.case("the editor moves and copies steps dragged and takes events dropped", function()
	test.cleanup(function()
		if #ui.windows().windows > 1 then ui.command{command = "window.arranger"} end
	end)
	gnubase.track.add{type = "arranger", timeBase = "linear"}
	local a = gnubase.arranger.add{position = 0, ["end"] = 48000}.event
	local b = gnubase.arranger.add{position = 48000, ["end"] = 96000}.event
	local c = gnubase.arranger.add{position = 96000, ["end"] = 144000}.event
	gnubase.arranger.setChain{steps = {{event = a.id}, {event = b.id}, {event = c.id}}}
	for _ = 1, 50 do
		if arrangerWindow() == nil then break end
		pcall(ui.wait, {selector = "#no-such-element", seconds = 0.05})
	end
	ui.command{command = "window.arranger"}
	local window = openedWindow()
	test.ok(window)
	-- The names of the chain's steps, and those the list shows once it shows them.
	local function order()
		local names = {[a.id] = "A", [b.id] = "B", [c.id] = "C"}
		local result = ""
		for _, step in ipairs(steps()) do result = result .. names[step.event] end
		return result
	end
	local function shown()
		local result = ""
		for _, element in ipairs(ui.dom{window = window, selector = "#chain-steps .name", depth = 0}.elements) do
			result = result .. (element.attributes.value or "")
		end
		return result
	end
	local function becomes(names)
		for _ = 1, 50 do
			if order() == names and shown() == names then break end
			pcall(ui.wait, {window = window, selector = "#no-such-element", seconds = 0.05})
		end
		test.equal(order(), names)
		test.equal(shown(), names)
	end
	becomes("ABC")
	-- A point in an element, a fraction of the way down.
	local function at(selector, fraction)
		local box = ui.dom{window = window, selector = selector, depth = 0}.elements[1].box
		return {x = math.floor(box.x + box.width / 2), y = math.floor(box.y + box.height * fraction)}
	end
	local function drag(from, to, modifiers)
		ui.drag{window = window, from = from, to = to, modifiers = modifiers} -- an empty table would be an object
	end

	-- Reference manual pp. 409-410: on a step's lower half the step dragged goes after it, on its upper half before it,
	-- and with Alt it is copied.
	drag(at("#step-0 .repeats", 0.5), at("#step-2 .repeats", 0.75))
	becomes("BCA")
	drag(at("#step-2 .repeats", 0.5), at("#step-0 .repeats", 0.25), {"alt"})
	becomes("ABCA")
	-- The selected steps move together when one of them is dragged; a press on one keeps the others selected.
	ui.click{window = window, selector = "#step-0 .repeats"}
	ui.click{window = window, selector = "#step-2 .repeats", modifiers = {"ctrl"}}
	drag(at("#step-2 .repeats", 0.5), at("#step-3 .repeats", 0.75))
	becomes("BAAC")
	-- An event dropped from the Arranger Events list goes in where it is dropped.
	drag(at("#event-" .. c.id .. " .name", 0.5), at("#step-1 .repeats", 0.25))
	becomes("BCAAC")
	-- A release on the step pressed is a click: it selects that step alone.
	ui.click{window = window, selector = "#step-0 .repeats"}
	ui.click{window = window, selector = "#step-1 .repeats", modifiers = {"ctrl"}}
	ui.click{window = window, selector = "#step-1 .repeats"}
	test.equal(#ui.dom{window = window, selector = "#chain-steps .selected", depth = 0}.elements, 1)
	test.equal(order(), "BCAAC")
end)

test.case("the editor flattens with the flatten options it keeps", function()
	test.cleanup(function()
		if #ui.windows().windows > 1 then ui.command{command = "window.arranger"} end
	end)
	-- A MIDI part as long as A, a second at 120 bpm, which the chain plays twice.
	gnubase.track.add{type = "arranger", timeBase = "linear"}
	local midi = gnubase.track.add{type = "midi"}.track
	gnubase.midiPart.add{track = midi.id, position = 0, length = 960}
	local a = gnubase.arranger.add{position = 0, ["end"] = 48000}.event
	gnubase.arranger.setChain{steps = {{event = a.id, repeats = 2}}}
	ui.command{command = "window.arranger"}
	local window = openedWindow()
	test.ok(window)
	ui.wait{window = window, selector = "#step-0"}

	local function attribute(selector, name)
		local element = ui.dom{window = window, selector = selector, depth = 0}.elements[1]
		return element ~= nil and element.attributes ~= nil and element.attributes[name] ~= nil
	end

	-- Flatten (with Options & Preferences) shows the options in place of the lists (p. 408); Rename Arranger Events
	-- needs Keep Arranger Track.
	ui.click{window = window, selector = "#flatten-with-options"}
	ui.wait{window = window, selector = "#flatten-keep-track"}
	ui.wait{window = window, selector = "#chain-steps", visible = false}
	test.ok(attribute("#flatten-rename", "disabled"))
	ui.click{window = window, selector = "#flatten-keep-track"}
	test.ok(not attribute("#flatten-rename", "disabled"))
	ui.click{window = window, selector = "#flatten-rename"}
	ui.click{window = window, selector = "#flatten-real-copies"}

	-- Go Back keeps them (p. 410).
	ui.click{window = window, selector = "#flatten-back"}
	ui.wait{window = window, selector = "#step-0"}
	ui.click{window = window, selector = "#flatten-with-options"}
	ui.wait{window = window, selector = "#flatten-keep-track"}
	for _, id in ipairs({"#flatten-keep-track", "#flatten-rename", "#flatten-real-copies"}) do
		test.ok(attribute(id, "checked"), id .. " stays checked")
	end
	test.ok(not attribute("#flatten-dont-split", "checked"))

	-- So does closing the editor: opened again, it shows them checked, with Rename Arranger Events enabled.
	ui.command{command = "window.arranger"}
	pcall(ui.wait, {selector = "#no-such-element", seconds = 0.1})
	test.equal(arrangerWindow(), nil)
	ui.command{command = "window.arranger"}
	window = openedWindow()
	test.ok(window)
	ui.wait{window = window, selector = "#step-0"}
	ui.click{window = window, selector = "#flatten-with-options"}
	ui.wait{window = window, selector = "#flatten-keep-track"}
	for _, id in ipairs({"#flatten-keep-track", "#flatten-rename", "#flatten-real-copies"}) do
		test.ok(attribute(id, "checked"), id .. " stays checked after closing the editor")
	end
	test.ok(not attribute("#flatten-dont-split", "checked"))
	test.ok(not attribute("#flatten-rename", "disabled"))

	-- Flatten: the arranger track stays with A 1 and A 2, and the parts are real copies, none shared.
	ui.click{window = window, selector = "#flatten-now"}
	local events = gnubase.arranger.get().events
	test.equal(#events, 2)
	test.equal(events[1].name, "A 1")
	test.equal(events[2].name, "A 2")
	local parts = gnubase.project.get().midiParts
	test.equal(#parts, 2)
	for _, part in ipairs(parts) do test.equal(part.shared, 0) end
	ui.wait{window = window, selector = "#flatten-keep-track", visible = false}
end)
