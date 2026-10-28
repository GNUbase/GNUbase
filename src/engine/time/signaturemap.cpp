// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/time/signaturemap.h"

#include <stdexcept>

namespace gnubase::time
{
	namespace
	{
		constexpr Ticks g_ticksPerSixteenth = g_ticksPerQuarter / 4;

		Ticks beatLength(const TimeSignature& signature)
		{
			return 4 * g_ticksPerQuarter / signature.denominator;
		}

		// Rounds toward negative infinity, so positions before bar 0 count down from it.
		Ticks floorDivide(Ticks value, Ticks divisor)
		{
			return (value / divisor) - (value % divisor < 0 ? 1 : 0);
		}
	}

	std::string formatBarsBeats(const BarsBeats& position)
	{
		return std::to_string(position.bar + 1) + "." + std::to_string(position.beat + 1) + "." +
			std::to_string(position.sixteenth + 1) + "." + std::to_string(position.tick);
	}

	SignatureMap::SignatureMap(TimeSignature signature) : SignatureMap(std::vector<SignatureChange>{{0, signature}})
	{
	}

	std::vector<SignatureChange> SignatureMap::changes() const
	{
		std::vector<SignatureChange> changes;
		changes.reserve(m_sections.size());
		for (const Section& section : m_sections)
			changes.push_back({section.bar, section.signature});
		return changes;
	}

	SignatureMap::SignatureMap(const std::vector<SignatureChange>& changes)
	{
		if (changes.empty() || changes.front().bar != 0)
			throw std::invalid_argument("a signature map starts with a change at bar 0");
		m_sections.reserve(changes.size());
		for (const SignatureChange& change : changes)
		{
			const int denominator = change.signature.denominator;
			const bool powerOfTwo = denominator >= 1 && denominator <= 16 && (denominator & (denominator - 1)) == 0;
			if (change.signature.numerator < 1 || !powerOfTwo)
				throw std::invalid_argument("invalid time signature");
			Ticks start = 0;
			if (!m_sections.empty())
			{
				const Section& previous = m_sections.back();
				if (change.bar <= previous.bar)
					throw std::invalid_argument("signature changes must be in increasing order");
				start = previous.start + ((change.bar - previous.bar) * previous.barLength);
			}
			m_sections.push_back(
				{change.bar, start, change.signature.numerator * beatLength(change.signature), change.signature});
		}
	}

	Ticks SignatureMap::barStart(int64_t bar) const
	{
		return toTicks(BarsBeats{bar});
	}

	TimeSignature SignatureMap::signatureAt(Ticks tick) const
	{
		return detail::lastAtOrBefore(m_sections, &Section::start, tick).signature;
	}

	BarsBeats SignatureMap::toBarsBeats(Ticks tick, int displayTicksPerSixteenth) const
	{
		const Section& section = detail::lastAtOrBefore(m_sections, &Section::start, tick);
		const Ticks bars = floorDivide(tick - section.start, section.barLength);
		const Ticks inBar = tick - section.start - (bars * section.barLength);
		const Ticks beat = beatLength(section.signature);
		return {section.bar + bars, static_cast<int>(inBar / beat),
			static_cast<int>((inBar % beat) / g_ticksPerSixteenth),
			static_cast<int>((inBar % g_ticksPerSixteenth) / (g_ticksPerSixteenth / displayTicksPerSixteenth))};
	}

	Ticks SignatureMap::toTicks(const BarsBeats& position, int displayTicksPerSixteenth) const
	{
		const Section& section = detail::lastAtOrBefore(m_sections, &Section::bar, position.bar);
		return section.start + ((position.bar - section.bar) * section.barLength) +
			(position.beat * beatLength(section.signature)) + (position.sixteenth * g_ticksPerSixteenth) +
			(position.tick * (g_ticksPerSixteenth / displayTicksPerSixteenth));
	}
}
