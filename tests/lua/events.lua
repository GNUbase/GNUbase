-- SPDX-License-Identifier: GPL-3.0-or-later
-- Editing parts and events from Lua (#85): the event commands, shared copies and muted parts that don't play.
-- tests/eventedit_tests.cpp checks every command in detail.
local test = gnubase.test

local quarter = 1720320 -- ticks; at the default 120 BPM a quarter note is 0.5 s, 24000 frames

local function synthPart()
	gnubase.project.new()
	gnubase.audio.setDevice{blockSize = 128, inputChannels = 0, captureSeconds = 1}
	local track = gnubase.track.add{type = "instrument"}
	gnubase.channel.set{channel = track.channel.id, instrument = "testSynth"}
	local part = gnubase.midiPart.add{track = track.track.id, position = 0, length = quarter}.part
	gnubase.note.add{part = part.id, notes = {{position = 0, length = quarter, pitch = 69, velocity = 1}}}
	return part
end

local function loudest()
	gnubase.transport.locate{position = 0}
	gnubase.transport.play()
	local captured = gnubase.audio.render{frames = 12000}.capturedFrames
	gnubase.transport.stop()
	local samples = gnubase.audio.capture{channel = 0, start = captured - 12000, frames = 12000}.samples
	return math.max(table.unpack(samples, 1, 6000))
end

test.case("a muted part doesn't play, and plays again when unmuted", function()
	local part = synthPart()
	gnubase.event.set{events = {part.id}, mute = true}
	test.equal(loudest(), 0)
	gnubase.history.undo()
	test.ok(loudest() > 0.1, "the part plays")
end)

test.case("duplicates, splits and glues parts; shared copies follow each other", function()
	local part = synthPart()
	local copies = gnubase.event.duplicate{events = {part.id}, count = 2, shared = true}.events
	test.equal(#copies, 2)
	local note = gnubase.project.get().midiParts[2].notes[1]
	gnubase.note.set{part = copies[2], notes = {{id = note.id, pitch = 72}}}
	for _, each in ipairs(gnubase.project.get().midiParts) do
		test.equal(each.notes[1].pitch, 72)
	end

	local right = gnubase.event.split{events = {part.id}, positions = {12000}}.events
	test.equal(#right, 1)
	local glued = gnubase.event.glue{events = {part.id, right[1]}}.event
	test.equal(glued, part.id)
	test.equal(#gnubase.project.get().midiParts, 3)
end)
