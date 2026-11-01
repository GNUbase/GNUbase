-- SPDX-License-Identifier: GPL-3.0-or-later
-- Snap and grid from Lua (#87): the settings, snapping a position and shuffling parts. tests/snap_tests.cpp checks
-- every snap and grid type in detail.
local test = gnubase.test

local bar = 4 * 1720320 -- ticks; at the default 120 BPM a bar is 2 s, 96000 frames

test.case("snap settings snap positions to bars and shuffle parts", function()
	gnubase.project.new()
	gnubase.snap.set{on = true, type = "grid", grid = "bar"}
	test.equal(gnubase.snap.get().grid, "bar")
	local snapped = gnubase.snap.position{position = 150000}
	test.equal(snapped.position, 192000)
	test.equal(snapped.ticks, 2 * bar)
	test.equal(snapped.musical, true)

	local track = gnubase.track.add{type = "midi"}.track.id
	local a = gnubase.midiPart.add{track = track, position = 0, length = bar}.part.id
	local b = gnubase.midiPart.add{track = track, position = bar, length = bar}.part.id
	gnubase.event.shuffle{events = {a}, position = 150000}
	local parts = {}
	for _, part in ipairs(gnubase.project.get().midiParts) do
		parts[part.id] = part.position
	end
	test.equal(parts[b], 0)
	test.equal(parts[a], bar)
end)
