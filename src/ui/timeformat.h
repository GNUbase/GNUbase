// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "engine/time/signaturemap.h"
#include "engine/time/tempomap.h"
#include "engine/time/timecode.h"
#include "ui/canvas.h"

#include <cstdint>
#include <optional>
#include <string>

// Time formats for rulers and position displays (#84, reference manual pp. 69-70, 317): positions on a timeline in
// seconds from the project start, read as bars and beats, seconds, timecode or samples, the way the engine converts
// them (engine/time).
namespace gnubase::ui
{
	enum class TimeFormat : uint8_t
	{
		BarsBeats,
		Seconds,
		Timecode,   // at the project's frame rate
		Samples,    // at the project's sample rate
		UserFrames, // timecode at a frame rate of the user's
	};

	// "barsBeats", "seconds", "timecode", "samples" and "userFrames".
	std::string timeFormatName(TimeFormat format);
	std::optional<TimeFormat> timeFormatFromName(const std::string& name);

	// What converts the project's time: its tempo and signatures, its sample rate and frame rate, and the user's frame
	// rate.
	struct TimeContext
	{
		time::TempoMap tempo{120.0};
		time::SignatureMap signatures;
		double sampleRate = 48000.0;
		time::FrameRate frameRate = time::g_fps25; // the project's frame rate comes with the project setup (#78)
		int userFramesPerSecond = 30;
	};

	// The tempo, signatures and sample rate of a project as project.get returns it. Keeps the defaults, with a warning
	// in the log, for a tempo or signature map it can't read.
	TimeContext projectTime(const engine::Json& project);

	// A position, in seconds from the project start, as position displays show it: "1.1.1.0", "0:00:01.500",
	// "00:00:01:12" or "72000".
	std::string formatTime(TimeFormat format, double seconds, const TimeContext& context);

	// A ruler scale for a timeline in seconds: bars and beats where the tempo and signature maps put them, down to
	// display ticks; seconds, frames and samples in round steps. Labels leave out what is zero, e.g. "5.2" for the
	// second beat of bar 5. Nothing before the project start.
	RulerScale rulerScale(TimeFormat format, const TimeContext& context);
}
