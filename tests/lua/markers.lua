-- SPDX-License-Identifier: GPL-3.0-or-later
-- Markers and the navigation commands key commands call, through the Lua API.
local test = gnubase.test

-- At 120 BPM and 48 kHz a quarter note is 1720320 ticks and 24000 samples.
local quarterTicks = 1720320
local quarterSamples = 24000

local function cursor()
	return gnubase.transport.get().position
end

test.before(function()
	gnubase.project.new()
	gnubase.transport.locate{position = 0}
	gnubase.track.add{type = "marker"}
	gnubase.marker.add{position = 2 * quarterTicks, name = "Intro"}
	gnubase.marker.add{type = "cycle", position = 4 * quarterTicks, ["end"] = 8 * quarterTicks, name = "Verse"}
	gnubase.marker.add{position = 12 * quarterTicks, name = "Outro"}
end)

test.case("locating to the next and previous marker", function()
	test.equal(gnubase.marker.locateNext().marker.name, "Intro")
	test.equal(cursor(), 2 * quarterSamples)
	test.equal(gnubase.marker.locateNext().position, 4 * quarterSamples)
	test.equal(gnubase.marker.locateNext().position, 8 * quarterSamples)
	test.equal(gnubase.marker.locateNext().marker.name, "Outro")
	test.equal(gnubase.marker.locateNext().marker, nil) -- no marker after the last one
	test.equal(cursor(), 12 * quarterSamples)
	test.equal(gnubase.marker.locatePrevious().position, 8 * quarterSamples)
	test.equal(cursor(), 8 * quarterSamples)
end)

test.case("locating to marker N and setting the locators to cycle marker N", function()
	test.equal(gnubase.marker.locate{number = 2}.marker.name, "Outro")
	test.equal(cursor(), 12 * quarterSamples)
	gnubase.marker.setLocators{number = 1}
	local transport = gnubase.transport.get()
	test.equal(transport.left, 4 * quarterSamples)
	test.equal(transport.right, 8 * quarterSamples)
	test.raises(function() gnubase.marker.locate{number = 3} end, "3")
end)

test.case("moving a marker announces it and is one undo step", function()
	local intro = gnubase.marker.list().markers[1]
	gnubase.events() -- forgets what earlier commands announced
	gnubase.marker.set{marker = intro.id, position = quarterTicks, name = "Start"}
	local event = gnubase.waitFor("marker.changed", 0)
	test.ok(event, "marker.changed")
	test.equal(event.data.marker.name, "Start")
	test.equal(gnubase.marker.locate{number = 1}.position, quarterSamples)
	gnubase.history.undo()
	test.equal(gnubase.marker.list().markers[1].name, "Intro")
	test.equal(gnubase.marker.locate{number = 1}.position, 2 * quarterSamples)
end)
