// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace gnubase::time
{
	struct FrameRate
	{
		int framesPerSecond = 25; // frames counted per timecode second
		bool pullDown = false;    // runs 0.1% slow: 1000 frames take as long as 1001 nominal frames
		bool dropFrame = false;   // skips frame numbers so the count keeps up with the clock
	};

	inline constexpr FrameRate g_fps24PullDown{24, true, false}; // 23.976
	inline constexpr FrameRate g_fps24{24, false, false};
	inline constexpr FrameRate g_fps25PullDown{25, true, false}; // 24.975
	inline constexpr FrameRate g_fps25{25, false, false};
	inline constexpr FrameRate g_fps30PullDown{30, true, false};    // 29.97
	inline constexpr FrameRate g_fps30PullDownDrop{30, true, true}; // 29.97 drop-frame
	inline constexpr FrameRate g_fps30{30, false, false};
	inline constexpr FrameRate g_fps30Drop{30, false, true};
	inline constexpr FrameRate g_fps50{50, false, false};
	inline constexpr FrameRate g_fps60PullDown{60, true, false}; // 59.94
	inline constexpr FrameRate g_fps60{60, false, false};

	struct Timecode
	{
		bool negative = false;
		int hours = 0;
		int minutes = 0;
		int seconds = 0;
		int frames = 0;
	};

	// Frame numbers count frames since zero; timecodes label them, skipping labels at drop-frame rates.
	Timecode fromFrameNumber(int64_t frame, FrameRate rate);
	int64_t toFrameNumber(const Timecode& timecode, FrameRate rate);

	// A time between two frames shows the earlier frame (the later one before zero).
	Timecode toTimecode(double seconds, FrameRate rate);
	double toSeconds(const Timecode& timecode, FrameRate rate);

	// "01:02:03:12", with ';' before the frames at drop-frame rates.
	std::string formatTimecode(const Timecode& timecode, FrameRate rate);
	// "1:02:03.500"
	std::string formatSeconds(double seconds);
}
