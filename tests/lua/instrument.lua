-- SPDX-License-Identifier: GPL-3.0-or-later
-- The goal of milestone M02 from Lua: create a project, add tracks, play MIDI into an instrument and get the rendered
-- audio back. tests/mcptools_tests.cpp does the same as an MCP client.
local test = gnubase.test

local quarter = 1720320 -- ticks; at the default 120 BPM a quarter note is 0.5 s, 24000 frames

-- The strongest frequencies of an analysed channel, lowest first.
local function frequencies(channel)
	local result = {}
	for _, peak in ipairs(channel.spectrum.peaks) do
		result[#result + 1] = peak.frequency
	end
	table.sort(result)
	return result
end

test.case("MIDI from an instrument track and a MIDI track plays the test synth", function()
	gnubase.project.new()
	gnubase.audio.setDevice{blockSize = 128, inputChannels = 0, captureSeconds = 1}

	-- An instrument track playing the test synth, and a MIDI track that plays into it.
	local instrument = gnubase.track.add{type = "instrument"}
	gnubase.channel.set{channel = instrument.channel.id, instrument = "testSynth"}
	local midi = gnubase.track.add{type = "midi"}
	gnubase.channel.set{channel = midi.channel.id, output = instrument.channel.id}

	-- A4 (440 Hz) on the instrument track and E5 (659.3 Hz) on the MIDI track, both for the first 0.25 s.
	for track, pitch in pairs{[instrument] = 69, [midi] = 76} do
		local part = gnubase.midiPart.add{track = track.track.id, position = 0, length = quarter}
		local note = {position = 0, length = quarter // 2, pitch = pitch, velocity = 1}
		gnubase.note.add{part = part.part.id, notes = {note}}
	end
	gnubase.transport.play()
	gnubase.audio.render{frames = 24000}

	-- The rendered audio comes back as samples, and the analysis finds both notes.
	local samples = gnubase.audio.capture{channel = 0, start = 0, frames = 12000}.samples
	test.equal(#samples, 12000)
	test.ok(math.max(table.unpack(samples, 1, 1000)) > 0.1, "the notes sound")
	local playing = gnubase.audio.analyze{channels = {0}, start = 0, frames = 12000, peaks = 2}
	test.near(frequencies(playing.channels[1]), {440, 659.26}, 2)

	-- The notes end at 0.25 s; after the synth's release the output is silent.
	local after = gnubase.audio.analyze{channels = {0}, start = 16000, frames = 8000}
	test.ok(after.channels[1].peak < 1e-4, "silent after the notes")
end)
