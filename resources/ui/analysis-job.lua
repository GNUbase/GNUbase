-- SPDX-License-Identifier: GPL-3.0-or-later
-- A dialog owns one analysis job. Replacing it or closing the dialog cancels it; only its latest result is shown.
-- Capture pool clips as whole-clip sources, or the selected project events with their optional editor range.
function analysisSources(document)
	local wanted, sources = {}, {}
	local pool = tostring(document:GetAttribute("clips") or "")
	for id in (pool ~= "" and pool or tostring(document:GetAttribute("events") or "")):gmatch("%d+") do
		wanted[tonumber(id)] = true
	end
	local project = gnubase.project.get()
	local clips, media = {}, {}
	for _, item in ipairs(project.media) do media[item.id] = item end
	for _, clip in ipairs(project.clips) do
		clips[clip.id] = clip
		if pool ~= "" and wanted[clip.id] then
			sources[#sources + 1] = {clip = clip.id, media = clip.media, name = clip.name,
				offset = 0, length = media[clip.media].length}
		end
	end
	if pool == "" then
		for _, event in ipairs(project.audioEvents) do
			if wanted[event.id] then
				event.name, event.media = clips[event.clip].name, clips[event.clip].media
				local start, finish = tonumber(document:GetAttribute("start")), tonumber(document:GetAttribute("end"))
				if start and finish then event.offset, event.length = start, finish - start end
				sources[#sources + 1] = event
			end
		end
	end
	return sources
end

function createAnalysisJob(message)
	local pending, complete
	local function cancel()
		local previous = pending
		pending = nil
		if previous then pcall(gnubase.job.cancel, {job = previous}) end
	end
	local function finish(job)
		if job.id ~= pending or job.state == "running" then return end
		pending = nil
		message("")
		complete(job.state == "done", job.state == "done" and job.result or job.error or "Cancelled")
	end
	local function progress(event)
		if event.data.job == pending then
			message(string.format("Analyzing %d%%", math.floor(event.data.progress * 100)))
		end
	end
	local function finished(event) finish(event.data.job) end
	gnubase.on("job.progress", progress)
	gnubase.on("job.finished", finished)
	return {
		cancel = cancel,
		start = function(command, parameters, done)
			cancel()
			complete = done
			message("Analyzing 0%")
			local ok, result = pcall(command, parameters)
			if not ok then message(""); done(false, result); return end
			pending = result.job.id
			-- A fast job can finish before start returns and before its id is known here.
			finish(gnubase.job.get{job = pending}.job)
		end,
		close = function()
			gnubase.off("job.progress", progress)
			gnubase.off("job.finished", finished)
			cancel()
		end
	}
end
