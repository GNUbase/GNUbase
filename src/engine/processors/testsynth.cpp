// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/processors/testsynth.h"

#include "engine/processors/builtin.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gnubase::engine::processors
{
	namespace
	{
		constexpr double g_twoPi = 6.28318530717958647692;

		double ramp(double seconds, double sampleRate)
		{
			return std::max(seconds, 0.0) * sampleRate;
		}
	}

	void TestSynth::prepare(double sampleRate, uint32_t /*maxFrames*/)
	{
		m_sampleRate = sampleRate;
	}

	int TestSynth::activeVoices() const
	{
		return static_cast<int>(
			std::count_if(m_voices.begin(), m_voices.end(), [](const Voice& voice) { return voice.active; }));
	}

	void TestSynth::noteOn(uint8_t channel, uint8_t pitch, uint16_t velocity) noexcept
	{
		if (velocity == 0)
		{
			noteOff(channel, pitch);
			return;
		}
		auto voice = std::find_if(m_voices.begin(), m_voices.end(), [](const Voice& v) { return !v.active; });
		if (voice == m_voices.end())
			voice = std::min_element(
				m_voices.begin(), m_voices.end(), [](const Voice& a, const Voice& b) { return a.started < b.started; });

		const double attack = ramp(m_attack, m_sampleRate);
		*voice = {};
		voice->active = true;
		voice->pitch = pitch;
		voice->channel = channel;
		voice->started = m_notes++;
		voice->increment = 440.0 * std::pow(2.0, (pitch - 69) / 12.0) / m_sampleRate;
		voice->amplitude = m_level * velocity / 65535.0;
		voice->envelope = attack < 1.0 ? 1.0 : 0.0;
		voice->step = attack < 1.0 ? 0.0 : 1.0 / attack;
	}

	void TestSynth::noteOff(uint8_t channel, uint8_t pitch) noexcept
	{
		const double release = ramp(m_release, m_sampleRate);
		for (Voice& voice : m_voices)
		{
			if (!voice.active || voice.released || voice.pitch != pitch || voice.channel != channel)
				continue;
			voice.released = true;
			// Counted in whole samples, so the voice ends exactly when the release does.
			voice.remaining = static_cast<uint64_t>(std::llround(release));
			if (voice.remaining == 0)
				voice.active = false;
			else
				voice.step = -voice.envelope / static_cast<double>(voice.remaining);
		}
	}

	void TestSynth::render(const graph::ProcessContext& context, uint32_t begin, uint32_t end) noexcept
	{
		for (int c = 0; c < context.channels; ++c)
			std::memset(context.audio[c] + begin, 0, (end - begin) * sizeof(float));
		if (context.channels == 0)
			return;
		float* first = context.audio[0];
		for (Voice& voice : m_voices)
		{
			for (uint32_t i = begin; i < end && voice.active; ++i)
			{
				first[i] += static_cast<float>(voice.amplitude * voice.envelope * std::sin(g_twoPi * voice.phase));
				voice.phase += voice.increment;
				voice.phase -= std::floor(voice.phase);
				if (voice.released)
				{
					voice.envelope += voice.step;
					if (--voice.remaining == 0)
						voice.active = false;
				}
				else if (voice.step > 0.0)
				{
					voice.envelope = std::min(voice.envelope + voice.step, 1.0);
					if (voice.envelope >= 1.0)
						voice.step = 0.0;
				}
			}
		}
		for (int c = 1; c < context.channels; ++c)
			std::memcpy(context.audio[c] + begin, first + begin, (end - begin) * sizeof(float));
	}

	void TestSynth::process(const graph::ProcessContext& context) noexcept
	{
		splitAtEvents(
			context,
			[this](const graph::ProcessEvent& event)
			{
				switch (event.type)
				{
				case graph::ProcessEvent::Type::NoteOn:
					noteOn(event.channel, event.key, event.velocity);
					break;
				case graph::ProcessEvent::Type::NoteOff:
					noteOff(event.channel, event.key);
					break;
				case graph::ProcessEvent::Type::Parameter:
					if (event.parameter == Level)
						m_level = event.parameterValue;
					else if (event.parameter == Attack)
						m_attack = event.parameterValue;
					else if (event.parameter == Release)
						m_release = event.parameterValue;
					break;
				case graph::ProcessEvent::Type::Controller:
					break;
				}
			},
			[this, &context](uint32_t begin, uint32_t end) { render(context, begin, end); });
	}
}
