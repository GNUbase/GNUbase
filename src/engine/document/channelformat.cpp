// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/channelformat.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace gnubase::engine
{
	namespace
	{
		struct FormatInfo
		{
			ChannelFormat format;
			const char* name;
			std::vector<Speaker> speakers;
		};

		const std::vector<FormatInfo>& formats()
		{
			using S = Speaker;
			static const std::vector<FormatInfo> table = {
				{ChannelFormat::None, "none", {}},
				{ChannelFormat::Mono, "mono", {S::M}},
				{ChannelFormat::Stereo, "stereo", {S::L, S::R}},
				{ChannelFormat::Lrc, "LRC", {S::L, S::R, S::C}},
				{ChannelFormat::Lrs, "LRS", {S::L, S::R, S::Cs}},
				{ChannelFormat::LrcLfe, "LRC+LFE", {S::L, S::R, S::C, S::Lfe}},
				{ChannelFormat::LrsLfe, "LRS+LFE", {S::L, S::R, S::Lfe, S::Cs}},
				{ChannelFormat::Lrcs, "LRCS", {S::L, S::R, S::C, S::Cs}},
				{ChannelFormat::LrcsLfe, "LRCS+LFE", {S::L, S::R, S::C, S::Lfe, S::Cs}},
				{ChannelFormat::Quadro, "quadro", {S::L, S::R, S::Ls, S::Rs}},
				{ChannelFormat::QuadroLfe, "quadro+LFE", {S::L, S::R, S::Lfe, S::Ls, S::Rs}},
				{ChannelFormat::Surround50, "5.0", {S::L, S::R, S::C, S::Ls, S::Rs}},
				{ChannelFormat::Surround51, "5.1", {S::L, S::R, S::C, S::Lfe, S::Ls, S::Rs}},
				{ChannelFormat::Cine60, "6.0 cine", {S::L, S::R, S::C, S::Ls, S::Rs, S::Cs}},
				{ChannelFormat::Music60, "6.0 music", {S::L, S::R, S::Ls, S::Rs, S::Sl, S::Sr}},
				{ChannelFormat::Surround70, "7.0", {S::L, S::R, S::C, S::Ls, S::Rs, S::Sl, S::Sr}},
				{ChannelFormat::Surround71, "7.1", {S::L, S::R, S::C, S::Lfe, S::Ls, S::Rs, S::Sl, S::Sr}},
			};
			return table;
		}

		const FormatInfo& info(ChannelFormat format)
		{
			return formats()[static_cast<size_t>(format)];
		}

		struct PanLawInfo
		{
			PanLaw law;
			const char* name;
			double center; // dB
		};

		constexpr PanLawInfo g_panLaws[] = {{PanLaw::Off, "0dB", 0.0}, {PanLaw::Minus3, "-3dB", -3.0},
			{PanLaw::Minus4_5, "-4.5dB", -4.5}, {PanLaw::Minus6, "-6dB", -6.0},
			{PanLaw::EqualPower, "equalPower", 0.0}};

		bool has(const std::vector<Speaker>& speakers, Speaker speaker)
		{
			return std::find(speakers.begin(), speakers.end(), speaker) != speakers.end();
		}
	}

	const char* formatName(ChannelFormat format)
	{
		return info(format).name;
	}

	std::optional<ChannelFormat> formatFromName(const std::string& name)
	{
		for (const FormatInfo& entry : formats())
			if (name == entry.name)
				return entry.format;
		return std::nullopt;
	}

	std::vector<std::string> formatNames()
	{
		std::vector<std::string> names;
		for (const FormatInfo& entry : formats())
			if (entry.format != ChannelFormat::None)
				names.emplace_back(entry.name);
		return names;
	}

	const std::vector<Speaker>& speakers(ChannelFormat format)
	{
		return info(format).speakers;
	}

	int channelCount(ChannelFormat format)
	{
		return static_cast<int>(speakers(format).size());
	}

	ChannelFormat defaultFormat(int channels)
	{
		constexpr ChannelFormat byCount[] = {ChannelFormat::None, ChannelFormat::Mono, ChannelFormat::Stereo,
			ChannelFormat::Lrc, ChannelFormat::Quadro, ChannelFormat::Surround50, ChannelFormat::Surround51,
			ChannelFormat::Surround70, ChannelFormat::Surround71};
		return channels >= 0 && channels <= g_maxFormatChannels ? byCount[channels] : ChannelFormat::None;
	}

	std::vector<float> conversionGains(ChannelFormat from, ChannelFormat to, double centerGain)
	{
		const std::vector<Speaker>& source = speakers(from);
		const std::vector<Speaker>& target = speakers(to);
		std::vector<float> gains(source.size() * target.size(), 0.0f);
		if (gains.empty())
			return gains;

		// What each speaker carries, as gains of the source channels. Speakers the destination lacks fold into
		// others, in this order, until only the destination's are left: a fold only adds speakers that come later
		// in it or that the destination has.
		const double minus3 = std::sqrt(0.5); // -3.01 dB
		std::map<Speaker, std::vector<double>> mix;
		for (size_t c = 0; c < source.size(); ++c)
		{
			mix[source[c]].assign(source.size(), 0.0);
			mix[source[c]][c] = 1.0;
		}
		const auto fold = [&](Speaker speaker, std::initializer_list<std::pair<Speaker, double>> into)
		{
			const auto found = mix.find(speaker);
			if (found == mix.end() || has(target, speaker))
				return;
			const std::vector<double> carried = std::move(found->second);
			mix.erase(found);
			for (const auto& [other, gain] : into)
			{
				std::vector<double>& sum = mix[other];
				sum.resize(source.size(), 0.0);
				for (size_t c = 0; c < source.size(); ++c)
					sum[c] += carried[c] * gain;
			}
		};
		using S = Speaker;
		fold(S::Sl, {{S::L, minus3}, {S::Ls, minus3}});
		fold(S::Sr, {{S::R, minus3}, {S::Rs, minus3}});
		fold(S::Cs, {{S::Ls, minus3}, {S::Rs, minus3}});
		if (has(target, S::Cs))
		{
			fold(S::Ls, {{S::Cs, minus3}});
			fold(S::Rs, {{S::Cs, minus3}});
		}
		fold(S::Ls, {{S::L, 0.5}}); // -6.02 dB
		fold(S::Rs, {{S::R, 0.5}});
		fold(S::Lfe, {{S::L, minus3}, {S::R, minus3}});
		if (has(target, S::M))
			fold(S::C, {{S::M, 1.0}});
		fold(S::C, {{S::L, minus3}, {S::R, minus3}});
		if (has(target, S::C))
			fold(S::M, {{S::C, 1.0}});
		fold(S::M, {{S::L, centerGain}, {S::R, centerGain}});
		fold(S::L, {{S::M, minus3}});
		fold(S::R, {{S::M, minus3}});

		for (size_t t = 0; t < target.size(); ++t)
			if (const auto found = mix.find(target[t]); found != mix.end())
				for (size_t c = 0; c < source.size(); ++c)
					gains[(t * source.size()) + c] = static_cast<float>(found->second[c]);
		return gains;
	}

	const char* panLawName(PanLaw law)
	{
		return g_panLaws[static_cast<size_t>(law)].name;
	}

	std::optional<PanLaw> panLawFromName(const std::string& name)
	{
		for (const PanLawInfo& entry : g_panLaws)
			if (name == entry.name)
				return entry.law;
		return std::nullopt;
	}

	std::vector<std::string> panLawNames()
	{
		std::vector<std::string> names;
		for (const PanLawInfo& entry : g_panLaws)
			names.emplace_back(entry.name);
		return names;
	}

	std::pair<double, double> panGains(PanLaw law, double pan)
	{
		pan = std::clamp(pan, -1.0, 1.0);
		if (law == PanLaw::Off)
			return {std::min(1.0, 1.0 - pan), std::min(1.0, 1.0 + pan)};
		constexpr double quarterPi = 0.78539816339744830962;
		double left = std::sin((1.0 - pan) * quarterPi);
		double right = std::sin((1.0 + pan) * quarterPi);
		if (law != PanLaw::EqualPower)
		{
			// sin(pi / 4) is -3.0103 dB: the power that makes it the law's attenuation.
			const double power = g_panLaws[static_cast<size_t>(law)].center / (20.0 * std::log10(std::sqrt(0.5)));
			left = std::pow(left, power);
			right = std::pow(right, power);
		}
		return {left, right};
	}

	Panner pannerFor(ChannelFormat channel, ChannelFormat destination)
	{
		if (channel == ChannelFormat::Mono && destination == ChannelFormat::Stereo)
			return Panner::Mono;
		if (channel == ChannelFormat::Stereo &&
			(destination == ChannelFormat::Stereo || destination == ChannelFormat::Mono))
			return Panner::Stereo;
		return Panner::None;
	}
}
