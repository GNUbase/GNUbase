-- SPDX-License-Identifier: GPL-3.0-or-later
-- Measuring the captured output: levels, loudness and spectrum, and a null test against a recording.
local test = gnubase.test

local function render(frequency, level, seconds)
	gnubase.project.new()
	local track = gnubase.track.add{type = "audio"}
	gnubase.channel.set{channel = track.channel.id, monitor = true}
	gnubase.audio.setDevice{
		captureSeconds = 10,
		input = {processor = "sine", parameters = {Frequency = frequency, Level = level}},
	}
	gnubase.audio.render{frames = math.floor(seconds * 48000)}
end

test.case("a 1 kHz sine at -20 dBFS in both channels", function()
	render(1000, 0.1, 5)
	local result = gnubase.audio.analyze()
	test.near(result.loudness, -20, 0.1)
	test.equal(#result.channels, 2)
	for _, channel in ipairs(result.channels) do
		test.near(channel.peakDb, -20, 0.01)
		test.near(channel.rmsDb, -23.01, 0.01)
		test.near(channel.spectrum.peaks[1].frequency, 1000, 0.5)
	end
end)

test.case("a recording cancels against the capture it came from", function()
	render(440, 0.5, 1)
	local path = (os.getenv("GNUBASE_TEST_OUTPUT_DIR") or ".") .. "/lua-analysis-reference.wav"
	test.cleanup(function() os.remove(path) end)
	test.equal(gnubase.audio.record{path = path}.frames, 48000)
	local result = gnubase.audio.compare{reference = path}
	test.equal(result.peak, 0)
	test.equal(result.peakDb, nil)
end)
