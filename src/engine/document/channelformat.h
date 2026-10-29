// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Channel formats (#269): which speaker each audio channel of a bus or mixer channel feeds, and how audio converts
// from one format to another where a channel feeds a destination of a different format.
namespace gnubase::engine
{
	enum class Speaker : uint8_t
	{
		M,   // mono
		L,   // front left
		R,   // front right
		C,   // front center
		Lfe, // low-frequency effects
		Ls,  // surround left (behind the listener)
		Rs,  // surround right
		Cs,  // surround center
		Sl,  // side left
		Sr,  // side right
	};

	// A speaker arrangement with a fixed speaker order, Microsoft's (front, then LFE, surround and side channels): 5.1
	// is L R C LFE Ls Rs. None is a MIDI channel's: no audio. The 3D formats come with the surround panner (#188).
	enum class ChannelFormat : uint8_t
	{
		None,
		Mono,
		Stereo,
		Lrc,       // L R C
		Lrs,       // L R Cs
		LrcLfe,    // L R C LFE
		LrsLfe,    // L R LFE Cs
		Lrcs,      // L R C Cs
		LrcsLfe,   // L R C LFE Cs
		Quadro,    // L R Ls Rs
		QuadroLfe, // L R LFE Ls Rs
		Surround50,
		Surround51,
		Cine60,  // L R C Ls Rs Cs
		Music60, // L R Ls Rs Sl Sr
		Surround70,
		Surround71, // L R C LFE Ls Rs Sl Sr
	};

	// The names commands and project files use: "mono", "stereo", "LRC", ..., "5.1", "7.1"; MIDI channels' "none".
	const char* formatName(ChannelFormat format);
	std::optional<ChannelFormat> formatFromName(const std::string& name);
	std::vector<std::string> formatNames(); // every format with audio, for schemas

	const std::vector<Speaker>& speakers(ChannelFormat format);
	int channelCount(ChannelFormat format);

	// The format for a number of audio channels: mono, stereo, LRC, quadro, 5.0, 5.1, 7.0 and 7.1; None for any
	// other number.
	ChannelFormat defaultFormat(int channels);

	// The most audio channels a channel format has.
	inline constexpr int g_maxFormatChannels = 8;

	// The gain of each source channel in each destination channel, gains[to * channelCount(from) + from], for audio
	// that a channel in the format from sends to a destination in the format to. Speakers that both formats have
	// connect one to one. The others fold into the destination's speakers by the reference's downmix tables, chained
	// where a format is further away: side channels into the front and surround channels on their side at -3.01 dB (7.1
	// to 5.1), center and LFE into L and R at -3.01 dB and surround channels into the front channel on their side at
	// -6.02 dB (5.1 to stereo); a surround center splits into Ls and Rs at -3.01 dB, and Ls and Rs merge into it at
	// -3.01 dB. L and R merge into mono at -3.01 dB each, and mono goes to C, or to L and R at centerGain where there
	// is no C, the gain of the pan law at the center. Same formats give the identity; None gives no gains.
	std::vector<float> conversionGains(ChannelFormat from, ChannelFormat to, double centerGain);
}
