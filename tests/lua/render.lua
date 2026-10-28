-- SPDX-License-Identifier: GPL-3.0-or-later
-- Offline rendering through the dummy device: the device input plays through an audio track.
local test = gnubase.test

local function peak(samples)
	local result = 0
	for _, sample in ipairs(samples) do
		result = math.max(result, math.abs(sample))
	end
	return result
end

test.before(function()
	gnubase.project.new()
	gnubase.audio.setDevice{blockSize = 128, captureSeconds = 1, input = {processor = "sine"}}
end)

test.case("an audio track plays the device input", function()
	gnubase.track.add{type = "audio"}
	gnubase.audio.render{frames = 4800}
	local samples = gnubase.audio.capture{channel = 0, start = 0, frames = 4800}.samples
	test.near(peak(samples), 0.5, 0.001)
	test.golden("lua-audio-track-plays-input", {samples = samples, sampleRate = 48000}, 1e-5)
end)

test.case("without tracks the output is silent", function()
	gnubase.audio.render{frames = 1024}
	test.equal(peak(gnubase.audio.capture{channel = 0, start = 0, frames = 1024}.samples), 0)
end)
