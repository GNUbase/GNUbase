// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"
#include "engine/processors/builtin.h"

#include <cstdint>
#include <vector>

// The channel strip's section parameters as parameters (#658, docs/features/channel-strip.md): what automation lanes
// of type strip and parameter.list name by their ids, with their ranges, units and options.
namespace gnubase::engine
{
	// The ids of the section parameters. Switches are 0 or 1, slopes and band types stepped choices (see
	// stripParameters' options); the others are in the unit of the ChannelSections field they stand for.
	struct StripParameter
	{
		enum Id : uint8_t
		{
			PreGain = 0,
			Phase = 1,
			LowCutOn = 2,
			LowCutFrequency = 3,
			LowCutSlope = 4,
			HighCutOn = 5,
			HighCutFrequency = 6,
			HighCutSlope = 7,
			EqBypass = 10,
			// EQ band n (1 to 4): 10 * n + one of these. Only bands 1 and 4 have a type.
			BandOn = 1,
			BandType = 2,
			BandFrequency = 3,
			BandGain = 4,
			BandQ = 5,
			GateOn = 50,
			GateThreshold = 51,
			GateRange = 52,
			GateAttack = 53,
			GateRelease = 54,
			CompressorOn = 60,
			CompressorThreshold = 61,
			CompressorRatio = 62,
			CompressorAttack = 63,
			CompressorRelease = 64,
			CompressorMakeup = 65,
			LimiterOn = 70,
			LimiterThreshold = 71,
			LimiterRelease = 72,
		};

		// The id of an EQ band's (1 to 4) parameter.
		static constexpr uint32_t band(int number, Id parameter)
		{
			return (10 * static_cast<uint32_t>(number)) + parameter;
		}
	};

	// Every section parameter of an audio channel, in the order the mixer shows them: the pre section, the equalizer,
	// then the gate, the compressor and the limiter. Each one's id is its ParameterInfo::id. A band type's values are
	// bell, shelf and cut.
	const std::vector<processors::ParameterInfo>& stripParameters();

	// The parameter with an id, or null.
	const processors::ParameterInfo* findStripParameter(uint32_t id);

	// Whether a parameter is in the pre section, before the plugin slots (the others come after them).
	inline bool isPreSection(uint32_t id)
	{
		return id < StripParameter::EqBypass;
	}

	// A parameter's value in sections, in its unit.
	double stripValue(const ChannelSections& sections, uint32_t id) noexcept;

	// Sets a parameter in sections to a value in its range; real-time safe. Unknown ids change nothing.
	void setStripValue(ChannelSections& sections, uint32_t id, double value) noexcept;
}
