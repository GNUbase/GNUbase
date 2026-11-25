-- SPDX-License-Identifier: GPL-3.0-or-later
-- Detect Silence's waveform and held preview (reference manual p. 604), through the engine API.
local dialog = DetectSilence

local function origin(element)
	local x, y = element.client_left, element.client_top
	while element do
		x, y = x + element.offset_left, y + element.offset_top
		element = element.offset_parent
		if element then x, y = x - element.scroll_left, y - element.scroll_top end
	end
	return x, y
end

function dialog.zoom(document)
	local zoom = tonumber(document:GetElementById("ds-zoom"):GetAttribute("value")) or 1
	document:GetElementById("ds-display").style.width = (zoom * 100) .. "%"
	document.context:Update()
	dialog.peaks(document)
end

function dialog.wheel(document, event)
	local p = event.parameters
	local zoom = document:GetElementById("ds-zoom")
	if p.ctrl_key == 1 or p.ctrl_key == true or p.meta_key == 1 or p.meta_key == true then
		local value = tonumber(zoom:GetAttribute("value")) or 1
		local view = document:GetElementById("ds-view")
		local wave = document:GetElementById("ds-wave")
		local pointer = p.mouse_x - origin(view)
		local fraction = (view.scroll_left + pointer) / wave.client_width
		value = math.max(1, math.min(16, value * (p.wheel_delta_y < 0 and 2 or 0.5)))
		zoom:SetAttribute("value", tostring(value))
		document:GetElementById("ds-display").style.width = (value * 100) .. "%"
		-- Apply the new width before the scroll range clamps the pointer's new offset.
		document.context:Update()
		view.scroll_left = fraction * wave.client_width - pointer
	else
		local view = document:GetElementById("ds-view")
		view.scroll_left = view.scroll_left + (p.wheel_delta_y + p.wheel_delta_x) * view.client_width / 8
	end
	dialog.peaks(document)
	event:StopPropagation()
end

function dialog.peaks(document)
	local source = dialog.events[dialog.index]
	local view = document:GetElementById("ds-view")
	local wave = document:GetElementById("ds-wave")
	if wave.client_width <= 0 or view.client_width <= 0 then return end
	local first = math.max(0, math.floor(source.length * view.scroll_left / wave.client_width))
	local last = math.min(source.length,
		math.ceil(source.length * (view.scroll_left + view.client_width) / wave.client_width))
	if last <= first then return end
	local ok, result = pcall(gnubase.media.peaks,
		{media = source.media, start = source.offset + first, frames = last - first, pixels = 256})
	if not ok then return end -- Analysis reports missing files too.
	local bars = {}
	local channels = #result.channels
	for c, channel in ipairs(result.channels) do
		local count = #channel.min
		for i = 1, count do
			local top = ((c - 0.5) - channel.max[i] / 65534) * 100 / channels
			local height = (channel.max[i] - channel.min[i]) * 100 / (65534 * channels)
			bars[#bars + 1] = string.format(
				'<div class="ds-peak" style="left:%.4f%%;width:%.4f%%;top:%.4f%%;height:%.4f%%"/>',
				(first + (i - 1) * (last - first) / count) * 100 / source.length,
				(last - first) * 100 / (count * source.length), top, height)
		end
	end
	dialog.channels = math.max(channels, 1)
	document:GetElementById("ds-wave").inner_rml = table.concat(bars)
	dialog.handles(document)
end

function dialog.handles(document)
	for _, name in ipairs({"open", "close"}) do
		local value = tonumber(document:GetElementById("ds-" .. name .. "-threshold"):GetAttribute("value")) or -120
		local y = (1 - 10 ^ (value / 20)) * 50 / (dialog.channels or 1)
		document:GetElementById("ds-" .. name .. "-handle").style.top = y .. "%"
	end
end

function dialog.press(document, event)
	if event.parameters.button ~= 0 then return end
	local target = event.target_element
	if target:HasAttribute("threshold") then
		dialog.drag = tostring(target:GetAttribute("threshold"))
		return
	end
	local source = dialog.events[dialog.index]
	local wave = document:GetElementById("ds-wave")
	local left = origin(wave)
	local fraction = math.max(0, math.min(1, (event.parameters.mouse_x - left) / wave.client_width))
	local parameters = {clip = source.clip, position = source.offset + math.floor(fraction * (source.length - 1)),
		["end"] = source.offset + source.length,
		level = tonumber(document:GetElementById("ds-preview-level"):GetAttribute("value")) or 1}
	if document:GetElementById("ds-mute-gaps"):HasAttribute("checked") then
		if not dialog.sections then return end
		parameters.sections = dialog.sections
	end
	local ok = pcall(gnubase.audition.start, parameters)
	dialog.playing = ok
end

function dialog.move(document, event)
	if not dialog.drag then return end
	local wave = document:GetElementById("ds-wave")
	local _, top = origin(wave)
	local amplitude = math.max(0.000001, math.min(1,
		1 - 2 * (dialog.channels or 1) * (event.parameters.mouse_y - top) / wave.client_height))
	local field = document:GetElementById("ds-" .. dialog.drag .. "-threshold")
	field:SetAttribute("value", tostring(20 * math.log(amplitude, 10)))
	dialog.changed(document, field)
end

function dialog.release()
	dialog.drag = nil
	dialog.stopPreview()
end

function dialog.stopPreview()
	if dialog.playing then pcall(gnubase.audition.stop, {}); dialog.playing = false end
end

function dialog.close()
	dialog.release()
	if dialog.peaksReady then gnubase.off("media.peaksReady", dialog.peaksReady) end
	if dialog.job then dialog.job.close() end
end
