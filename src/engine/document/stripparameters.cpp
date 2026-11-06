// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/stripparameters.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace gnubase::engine
{
	namespace
	{
		using processors::ParameterInfo;

		constexpr std::array<int, 5> g_slopes = {6, 12, 24, 36, 48}; // dB per octave

		ParameterInfo number(
			uint32_t id, std::string name, double minimum, double maximum, double defaultValue, std::string unit = {})
		{
			return {id, std::move(name), minimum, maximum, defaultValue, std::move(unit), false, {}};
		}

		ParameterInfo choice(uint32_t id, std::string name, double defaultValue, std::vector<std::string> options)
		{
			const auto maximum = static_cast<double>(options.size() - 1);
			return {id, std::move(name), 0.0, maximum, defaultValue, {}, true, std::move(options)};
		}

		ParameterInfo onOff(uint32_t id, std::string name)
		{
			return choice(id, std::move(name), 0.0, {"Off", "On"});
		}

		ParameterInfo slope(uint32_t id, std::string name)
		{
			return choice(id, std::move(name), 1.0, {"6 dB/oct", "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct"});
		}

		std::vector<ParameterInfo> makeParameters()
		{
			using P = StripParameter;
			const ChannelSections defaults;
			std::vector<ParameterInfo> list = {number(P::PreGain, "Pre Gain", -24.0, 24.0, 0.0, "dB"),
				onOff(P::Phase, "Phase"), onOff(P::LowCutOn, "Low Cut"),
				number(P::LowCutFrequency, "Low Cut Frequency", 20.0, 20000.0, defaults.lowCut.frequency, "Hz"),
				slope(P::LowCutSlope, "Low Cut Slope"), onOff(P::HighCutOn, "High Cut"),
				number(P::HighCutFrequency, "High Cut Frequency", 50.0, 20000.0, defaults.highCut.frequency, "Hz"),
				slope(P::HighCutSlope, "High Cut Slope"), onOff(P::EqBypass, "EQ Bypass")};
			for (int band = 1; band <= 4; ++band)
			{
				const EqBand& settings = defaults.eq[static_cast<size_t>(band - 1)];
				const std::string prefix = "EQ " + std::to_string(band) + " ";
				list.push_back(onOff(P::band(band, P::BandOn), prefix + "On"));
				if (band == 1 || band == 4)
					list.push_back(choice(P::band(band, P::BandType), prefix + "Type", 1.0,
						{"Bell", band == 1 ? "Low Shelf" : "High Shelf", band == 1 ? "Low Cut" : "High Cut"}));
				list.push_back(number(
					P::band(band, P::BandFrequency), prefix + "Frequency", 20.0, 20000.0, settings.frequency, "Hz"));
				list.push_back(number(P::band(band, P::BandGain), prefix + "Gain", -24.0, 24.0, 0.0, "dB"));
				list.push_back(number(P::band(band, P::BandQ), prefix + "Q", 0.1, 12.0, settings.q));
			}
			const StripGate& gate = defaults.gate;
			const StripCompressor& compressor = defaults.compressor;
			const StripLimiter& limiter = defaults.limiter;
			list.insert(list.end(),
				{onOff(P::GateOn, "Gate"), number(P::GateThreshold, "Gate Threshold", -80.0, 0.0, gate.threshold, "dB"),
					number(P::GateRange, "Gate Range", -80.0, 0.0, gate.range, "dB"),
					number(P::GateAttack, "Gate Attack", 0.01, 100.0, gate.attack, "ms"),
					number(P::GateRelease, "Gate Release", 1.0, 4000.0, gate.release, "ms"),
					onOff(P::CompressorOn, "Compressor"),
					number(P::CompressorThreshold, "Compressor Threshold", -60.0, 0.0, compressor.threshold, "dB"),
					number(P::CompressorRatio, "Compressor Ratio", 1.0, 20.0, compressor.ratio),
					number(P::CompressorAttack, "Compressor Attack", 0.1, 100.0, compressor.attack, "ms"),
					number(P::CompressorRelease, "Compressor Release", 10.0, 2000.0, compressor.release, "ms"),
					number(P::CompressorMakeup, "Compressor Makeup", 0.0, 24.0, compressor.makeup, "dB"),
					onOff(P::LimiterOn, "Limiter"),
					number(P::LimiterThreshold, "Limiter Threshold", -30.0, 0.0, limiter.threshold, "dB"),
					number(P::LimiterRelease, "Limiter Release", 1.0, 1000.0, limiter.release, "ms")});
			return list;
		}

		// A band type's value: 0 bell, 1 shelf, 2 cut.
		double typeValue(EqBandType type)
		{
			switch (type)
			{
			case EqBandType::LowShelf:
			case EqBandType::HighShelf:
				return 1.0;
			case EqBandType::LowCut:
			case EqBandType::HighCut:
				return 2.0;
			case EqBandType::Bell:
				break;
			}
			return 0.0;
		}

		EqBandType bandType(int band, double value)
		{
			const long index = std::lround(value);
			if (index == 1)
				return band == 1 ? EqBandType::LowShelf : EqBandType::HighShelf;
			if (index == 2)
				return band == 1 ? EqBandType::LowCut : EqBandType::HighCut;
			return EqBandType::Bell;
		}

		double slopeValue(int slope)
		{
			return static_cast<double>(std::find(g_slopes.begin(), g_slopes.end(), slope) - g_slopes.begin());
		}

		int slopeOf(double value)
		{
			return g_slopes[static_cast<size_t>(std::clamp(std::lround(value), 0L, 4L))];
		}

		// The band (1 to 4) of an EQ band's parameter, or 0.
		uint32_t bandOf(uint32_t id)
		{
			const uint32_t number = id / 10;
			const uint32_t parameter = id % 10;
			return number >= 1 && number <= 4 && parameter >= StripParameter::BandOn &&
					parameter <= StripParameter::BandQ
				? number
				: 0;
		}
	}

	const std::vector<processors::ParameterInfo>& stripParameters()
	{
		static const std::vector<processors::ParameterInfo> parameters = makeParameters();
		return parameters;
	}

	const processors::ParameterInfo* findStripParameter(uint32_t id)
	{
		const std::vector<processors::ParameterInfo>& list = stripParameters();
		const auto found = std::find_if(
			list.begin(), list.end(), [id](const processors::ParameterInfo& parameter) { return parameter.id == id; });
		return found != list.end() ? &*found : nullptr;
	}

	double stripValue(const ChannelSections& sections, uint32_t id) noexcept
	{
		using P = StripParameter;
		const auto flag = [](bool on) { return on ? 1.0 : 0.0; };
		switch (id)
		{
		case P::PreGain:
			return sections.gain;
		case P::Phase:
			return flag(sections.phase);
		case P::LowCutOn:
			return flag(sections.lowCut.on);
		case P::LowCutFrequency:
			return sections.lowCut.frequency;
		case P::LowCutSlope:
			return slopeValue(sections.lowCut.slope);
		case P::HighCutOn:
			return flag(sections.highCut.on);
		case P::HighCutFrequency:
			return sections.highCut.frequency;
		case P::HighCutSlope:
			return slopeValue(sections.highCut.slope);
		case P::EqBypass:
			return flag(sections.eqBypass);
		case P::GateOn:
			return flag(sections.gate.on);
		case P::GateThreshold:
			return sections.gate.threshold;
		case P::GateRange:
			return sections.gate.range;
		case P::GateAttack:
			return sections.gate.attack;
		case P::GateRelease:
			return sections.gate.release;
		case P::CompressorOn:
			return flag(sections.compressor.on);
		case P::CompressorThreshold:
			return sections.compressor.threshold;
		case P::CompressorRatio:
			return sections.compressor.ratio;
		case P::CompressorAttack:
			return sections.compressor.attack;
		case P::CompressorRelease:
			return sections.compressor.release;
		case P::CompressorMakeup:
			return sections.compressor.makeup;
		case P::LimiterOn:
			return flag(sections.limiter.on);
		case P::LimiterThreshold:
			return sections.limiter.threshold;
		case P::LimiterRelease:
			return sections.limiter.release;
		default:
			break;
		}
		const uint32_t number = bandOf(id);
		if (number == 0)
			return 0.0;
		const EqBand& band = sections.eq[number - 1];
		switch (id % 10)
		{
		case P::BandOn:
			return flag(band.on);
		case P::BandType:
			return typeValue(band.type);
		case P::BandFrequency:
			return band.frequency;
		case P::BandGain:
			return band.gain;
		default:
			return band.q;
		}
	}

	void setStripValue(ChannelSections& sections, uint32_t id, double value) noexcept
	{
		using P = StripParameter;
		const bool on = value >= 0.5;
		switch (id)
		{
		case P::PreGain:
			sections.gain = value;
			return;
		case P::Phase:
			sections.phase = on;
			return;
		case P::LowCutOn:
			sections.lowCut.on = on;
			return;
		case P::LowCutFrequency:
			sections.lowCut.frequency = value;
			return;
		case P::LowCutSlope:
			sections.lowCut.slope = slopeOf(value);
			return;
		case P::HighCutOn:
			sections.highCut.on = on;
			return;
		case P::HighCutFrequency:
			sections.highCut.frequency = value;
			return;
		case P::HighCutSlope:
			sections.highCut.slope = slopeOf(value);
			return;
		case P::EqBypass:
			sections.eqBypass = on;
			return;
		case P::GateOn:
			sections.gate.on = on;
			return;
		case P::GateThreshold:
			sections.gate.threshold = value;
			return;
		case P::GateRange:
			sections.gate.range = value;
			return;
		case P::GateAttack:
			sections.gate.attack = value;
			return;
		case P::GateRelease:
			sections.gate.release = value;
			return;
		case P::CompressorOn:
			sections.compressor.on = on;
			return;
		case P::CompressorThreshold:
			sections.compressor.threshold = value;
			return;
		case P::CompressorRatio:
			sections.compressor.ratio = value;
			return;
		case P::CompressorAttack:
			sections.compressor.attack = value;
			return;
		case P::CompressorRelease:
			sections.compressor.release = value;
			return;
		case P::CompressorMakeup:
			sections.compressor.makeup = value;
			return;
		case P::LimiterOn:
			sections.limiter.on = on;
			return;
		case P::LimiterThreshold:
			sections.limiter.threshold = value;
			return;
		case P::LimiterRelease:
			sections.limiter.release = value;
			return;
		default:
			break;
		}
		const uint32_t number = bandOf(id);
		if (number == 0)
			return;
		EqBand& band = sections.eq[number - 1];
		switch (id % 10)
		{
		case P::BandOn:
			band.on = on;
			break;
		case P::BandType:
			if (number == 1 || number == 4) // bands 2 and 3 are bells
				band.type = bandType(static_cast<int>(number), value);
			break;
		case P::BandFrequency:
			band.frequency = value;
			break;
		case P::BandGain:
			band.gain = value;
			break;
		default:
			band.q = value;
			break;
		}
	}
}
