// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "engine/schema.h"
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

	// What converts the project's time: its tempo and signatures, its sample rate and frame rate, the project setup's
	// start time and bar offset (#78), and the user's frame rate.
	struct TimeContext
	{
		time::TempoMap tempo{120.0};
		time::SignatureMap signatures;
		double sampleRate = 48000.0;
		time::FrameRate frameRate = time::g_fps25;
		double startTime = 0.0; // what seconds, timecode and user frames show at the project start
		int barOffset = 0;      // added to the bars shown
		int userFramesPerSecond = 30;
	};

	// The tempo, signatures, sample rate, frame rate, start time and bar offset of a project as project.get returns
	// it. Keeps the defaults, with a warning in the log, for a tempo or signature map it can't read.
	TimeContext projectTime(const engine::Json& project);

	// A position, in seconds from the project start, as position displays show it: "1.1.1.0", "0:00:01.500",
	// "00:00:01:12" or "72000". Seconds, timecode and user frames count from the start time, bars from the bar offset.
	std::string formatTime(TimeFormat format, double seconds, const TimeContext& context);

	// A position typed in the format, in seconds, or nothing for text that isn't one or is before the project start.
	// Numbers may be separated by any other characters; those left out count from the start: "3.2" is bar 3, beat 2,
	// "1:30" a minute and a half, and for timecode "5:12" five seconds and twelve frames.
	std::optional<double> parseTime(TimeFormat format, const std::string& text, const TimeContext& context);

	// A length from one position to another, in seconds: bars, beats, sixteenths and display ticks counted from zero,
	// "1.1.0.0" for a bar and a beat, or a time in the other formats. parseLength reads one typed from a start.
	std::string formatLength(TimeFormat format, double from, double to, const TimeContext& context);
	std::optional<double> parseLength(
		TimeFormat format, const std::string& text, double from, const TimeContext& context);

	// Reads what projectTime does of a project as project.get returns it into the context. Returns false, and leaves
	// the context, if they aren't valid.
	bool readTimeContext(const engine::Json& project, TimeContext& context);

	// A ruler scale for a timeline in seconds: bars and beats where the tempo and signature maps put them, down to
	// display ticks; seconds, frames and samples in round steps. Labels leave out what is zero, e.g. "5.2" for the
	// second beat of bar 5. Nothing before the project start.
	RulerScale rulerScale(TimeFormat format, const TimeContext& context);
}
