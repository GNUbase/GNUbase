// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/time/timecode.h"

#include <cmath>

namespace gnubase::time
{
	namespace
	{
		// Frame labels skipped at the start of each minute except every tenth: 2 at 30 fps, 4 at 60 fps.
		int64_t droppedPerMinute(FrameRate rate)
		{
			return rate.dropFrame ? rate.framesPerSecond / 15 : 0;
		}

		// Nominal frame durations per 1000 frames.
		double speed(FrameRate rate)
		{
			return rate.pullDown ? 1001.0 : 1000.0;
		}

		std::string padded(int64_t value, size_t digits)
		{
			const std::string text = std::to_string(value);
			return std::string(digits > text.size() ? digits - text.size() : 0, '0') + text;
		}
	}

	Timecode fromFrameNumber(int64_t frame, FrameRate rate)
	{
		int64_t label = frame < 0 ? -frame : frame;
		const int64_t fps = rate.framesPerSecond;
		const int64_t dropped = droppedPerMinute(rate);
		if (dropped > 0)
		{
			const int64_t perMinute = (fps * 60) - dropped;
			const int64_t perTenMinutes = (fps * 600) - (dropped * 9);
			const int64_t inTenMinutes = label % perTenMinutes;
			label += dropped * 9 * (label / perTenMinutes);
			if (inTenMinutes > dropped)
				label += dropped * ((inTenMinutes - dropped) / perMinute);
		}
		return {frame < 0, static_cast<int>(label / (fps * 3600)), static_cast<int>((label / (fps * 60)) % 60),
			static_cast<int>((label / fps) % 60), static_cast<int>(label % fps)};
	}

	int64_t toFrameNumber(const Timecode& timecode, FrameRate rate)
	{
		const int64_t minutes = (int64_t{60} * timecode.hours) + timecode.minutes;
		const int64_t frame = (((minutes * 60) + timecode.seconds) * rate.framesPerSecond) + timecode.frames -
			(droppedPerMinute(rate) * (minutes - (minutes / 10)));
		return timecode.negative ? -frame : frame;
	}

	Timecode toTimecode(double seconds, FrameRate rate)
	{
		const double frames = std::abs(seconds) * rate.framesPerSecond * 1000.0 / speed(rate);
		// A millionth of a frame absorbs rounding at exact frame boundaries; it is far shorter than a sample.
		const auto frame = static_cast<int64_t>(std::floor(frames + 1e-6));
		return fromFrameNumber(seconds < 0.0 ? -frame : frame, rate);
	}

	double toSeconds(const Timecode& timecode, FrameRate rate)
	{
		return static_cast<double>(toFrameNumber(timecode, rate)) * speed(rate) / (rate.framesPerSecond * 1000.0);
	}

	std::string formatTimecode(const Timecode& timecode, FrameRate rate)
	{
		return (timecode.negative ? "-" : "") + padded(timecode.hours, 2) + ":" + padded(timecode.minutes, 2) + ":" +
			padded(timecode.seconds, 2) + (rate.dropFrame ? ";" : ":") + padded(timecode.frames, 2);
	}

	std::string formatSeconds(double seconds)
	{
		const int64_t milliseconds = std::llround(std::abs(seconds) * 1000.0);
		return (seconds < 0.0 && milliseconds > 0 ? "-" : "") + std::to_string(milliseconds / 3600000) + ":" +
			padded((milliseconds / 60000) % 60, 2) + ":" + padded((milliseconds / 1000) % 60, 2) + "." +
			padded(milliseconds % 1000, 3);
	}
}
