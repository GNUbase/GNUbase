-- SPDX-License-Identifier: GPL-3.0-or-later
-- Sending MIDI into a track and receiving what its instrument played. tests/mcptools_tests.cpp does the same as an
-- MCP client.
local test = gnubase.test

test.case("MIDI sent into an instrument track comes back with its frame positions and plays the test synth", function()
	gnubase.project.new()
	gnubase.audio.setDevice{blockSize = 128, inputChannels = 0, captureSeconds = 1}
	local synth = gnubase.track.add{type = "instrument"}.channel.id
	gnubase.channel.set{channel = synth, instrument = "testSynth"}

	-- A4 (440 Hz) from frame 1000 to 12000.
	gnubase.midi.inject{channel = synth, messages = {
		{position = 1000, bytes = {0x90, 69, 127}},
		{position = 12000, bytes = {0x80, 69, 0}},
	}}
	gnubase.audio.render{frames = 24000}

	local captured = gnubase.midi.capture()
	test.equal(captured.captured, {
		{channel = synth, position = 1000, bytes = {0x90, 69, 127}},
		{channel = synth, position = 12000, bytes = {0x80, 69, 0}},
	})
	test.equal(captured.dropped, 0)

	local playing = gnubase.audio.analyze{channels = {0}, start = 1000, frames = 11000, peaks = 1}
	test.near(playing.channels[1].spectrum.peaks[1].frequency, 440, 2)
	local after = gnubase.audio.analyze{channels = {0}, start = 16000, frames = 8000}
	test.ok(after.channels[1].peak < 1e-4, "silent after the note")
end)
