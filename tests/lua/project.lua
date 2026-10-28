-- SPDX-License-Identifier: GPL-3.0-or-later
-- Tracks, events and undo through the Lua API.
local test = gnubase.test

test.before(gnubase.project.new)

test.case("a new project has an input and an output bus and no tracks", function()
	local project = gnubase.project.get()
	test.equal(#project.tracks, 0)
	test.equal(project.busses[1].direction, "input")
	test.equal(project.busses[2].direction, "output")
end)

test.case("adding a track is one undo step", function()
	local added = gnubase.track.add{type = "audio"}
	test.equal(added.track.name, "Audio 01")
	test.equal(gnubase.project.get().tracks, {added.track})
	gnubase.history.undo()
	test.equal(gnubase.project.get().tracks, {})
	gnubase.history.redo()
	test.equal(gnubase.project.get().tracks, {added.track})
end)

test.case("adding a track announces it", function()
	gnubase.events() -- forgets what earlier commands announced
	local added = gnubase.track.add{type = "midi"}
	local event = gnubase.waitFor("track.added", 0)
	test.ok(event, "track.added")
	test.equal(event.data, added)
end)

test.case("a failed command changes nothing", function()
	test.raises(function() gnubase.track.add{type = "nonsense"} end, "parameters.type")
	test.equal(gnubase.project.get().tracks, {})
end)
