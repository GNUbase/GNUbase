// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/processors/generators.h"

#include "engine/processors/builtin.h"

#include <algorithm>
#include <cmath>

namespace gnubase::engine::processors
{
	namespace
	{
		constexpr double g_twoPi = 6.28318530717958647692;

		void writeAll(const graph::ProcessContext& context, uint32_t frame, float value) noexcept
		{
			for (int c = 0; c < context.channels; ++c)
				context.audio[c][frame] = value;
		}
	}

	void Sine::prepare(double sampleRate, uint32_t /*maxFrames*/)
	{
		m_sampleRate = sampleRate;
	}

	void Sine::process(const graph::ProcessContext& context) noexcept
	{
		splitAtEvents(
			context,
			[this](const graph::ProcessEvent& event)
			{
				if (event.type != graph::ProcessEvent::Type::Parameter)
					return;
				if (event.parameter == Frequency)
					m_frequency = event.parameterValue;
				else if (event.parameter == Level)
					m_level = event.parameterValue;
			},
			[this, &context](uint32_t begin, uint32_t end)
			{
				const double increment = m_frequency / m_sampleRate;
				for (uint32_t i = begin; i < end; ++i)
				{
					writeAll(context, i, static_cast<float>(m_level * std::sin(g_twoPi * m_phase)));
					m_phase += increment;
					m_phase -= std::floor(m_phase);
				}
			});
	}

	void Noise::process(const graph::ProcessContext& context) noexcept
	{
		splitAtEvents(
			context,
			[this](const graph::ProcessEvent& event)
			{
				if (event.type == graph::ProcessEvent::Type::Parameter && event.parameter == Level)
					m_level = event.parameterValue;
			},
			[this, &context](uint32_t begin, uint32_t end)
			{
				for (uint32_t i = begin; i < end; ++i)
				{
					m_state ^= m_state << 13U;
					m_state ^= m_state >> 17U;
					m_state ^= m_state << 5U;
					const double unit = (static_cast<double>(m_state) / 2147483648.0) - 1.0; // -1 to 1
					writeAll(context, i, static_cast<float>(m_level * unit));
				}
			});
	}

	void Impulse::prepare(double sampleRate, uint32_t /*maxFrames*/)
	{
		m_sampleRate = sampleRate;
		updatePeriod();
	}

	void Impulse::updatePeriod()
	{
		m_period = std::max<uint64_t>(1, static_cast<uint64_t>(std::llround(m_interval * m_sampleRate)));
		m_untilNext = std::min(m_untilNext, m_period - 1);
	}

	void Impulse::process(const graph::ProcessContext& context) noexcept
	{
		splitAtEvents(
			context,
			[this](const graph::ProcessEvent& event)
			{
				if (event.type != graph::ProcessEvent::Type::Parameter)
					return;
				if (event.parameter == Interval)
				{
					m_interval = event.parameterValue;
					updatePeriod();
				}
				else if (event.parameter == Level)
				{
					m_level = event.parameterValue;
				}
			},
			[this, &context](uint32_t begin, uint32_t end)
			{
				for (uint32_t i = begin; i < end; ++i)
				{
					if (m_untilNext == 0)
					{
						writeAll(context, i, static_cast<float>(m_level));
						m_untilNext = m_period;
					}
					else
					{
						writeAll(context, i, 0.0f);
					}
					--m_untilNext;
				}
			});
	}

	void NoteProbe::process(const graph::ProcessContext& context) noexcept
	{
		const graph::EventBuffer& events = context.eventsIn;
		size_t next = 0;
		for (uint32_t i = 0; i < context.frames; ++i)
		{
			for (; next < events.size() && events[next].offset <= i; ++next)
			{
				const graph::ProcessEvent& event = events[next];
				const size_t index = ((event.channel % 16) * 128) + (event.key % 128);
				if (event.type == graph::ProcessEvent::Type::NoteOn && !m_on[index])
				{
					m_on[index] = true;
					++m_count;
					m_pitches += event.key;
				}
				else if (event.type == graph::ProcessEvent::Type::NoteOff && m_on[index])
				{
					m_on[index] = false;
					--m_count;
					m_pitches -= event.key;
				}
			}
			for (int c = 0; c < context.channels; ++c)
				context.audio[c][i] = static_cast<float>(c == 1 ? m_pitches : m_count);
		}
	}

	void PositionProbe::process(const graph::ProcessContext& context) noexcept
	{
		const graph::TransportBlock& transport = context.transport;
		if (!transport.playing)
		{
			for (uint32_t i = 0; i < context.frames; ++i)
				writeAll(context, i, -1.0f);
			return;
		}
		for (size_t s = 0; s < transport.segmentCount; ++s)
		{
			const graph::TransportSegment& segment = transport.segments[s];
			for (uint32_t i = 0; i < segment.frames && segment.offset + i < context.frames; ++i)
				writeAll(context, segment.offset + i, static_cast<float>(segment.position + i));
		}
	}
}
