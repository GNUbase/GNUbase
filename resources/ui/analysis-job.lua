-- SPDX-License-Identifier: GPL-3.0-or-later
-- A dialog owns one analysis job. Replacing it or closing the dialog cancels it; only its latest result is shown.
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
