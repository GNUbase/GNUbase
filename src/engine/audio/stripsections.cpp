// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/stripsections.h"
#include "engine/document/stripparameters.h"

#include <algorithm>
#include <array>
#include <tuple>

namespace gnubase::engine
{
	namespace
	{
		using dsp::Equalizer;

		// How long an equalizer goes on running after its last band went off: its glide to a pass-through and more.
		constexpr double g_filterTailSeconds = 0.1;

		double slopeIndex(int slope)
		{
			const std::array<int, 5> slopes = {6, 12, 24, 36, 48}; // dsp::Slope's order
			return static_cast<double>(std::find(slopes.begin(), slopes.end(), slope) - slopes.begin());
		}

		dsp::FilterType filterType(EqBandType type)
		{
			switch (type)
			{
			case EqBandType::LowShelf:
				return dsp::FilterType::LowShelf;
			case EqBandType::HighShelf:
				return dsp::FilterType::HighShelf;
			case EqBandType::LowCut:
				return dsp::FilterType::LowCut;
			case EqBandType::HighCut:
				return dsp::FilterType::HighCut;
			case EqBandType::Bell:
				break;
			}
			return dsp::FilterType::Bell;
		}

		void setBand(Equalizer& core, int band, bool on, dsp::FilterType type, double frequency, double gain, double q,
			double slope) noexcept
		{
			core.setParameter(Equalizer::parameter(band, Equalizer::On), on ? 1.0 : 0.0);
			core.setParameter(Equalizer::parameter(band, Equalizer::Type), static_cast<double>(type));
			core.setParameter(Equalizer::parameter(band, Equalizer::Frequency), frequency);
			core.setParameter(Equalizer::parameter(band, Equalizer::Gain), gain);
			core.setParameter(Equalizer::parameter(band, Equalizer::Q), q);
			core.setParameter(Equalizer::parameter(band, Equalizer::Slope), slope);
		}

		bool samePre(const ChannelSections& a, const ChannelSections& b)
		{
			return std::tie(a.phase, a.lowCut, a.highCut) == std::tie(b.phase, b.lowCut, b.highCut);
		}

		bool samePost(const ChannelSections& a, const ChannelSections& b)
		{
			return std::tie(a.eqBypass, a.eq, a.gate, a.compressor, a.limiter) ==
				std::tie(b.eqBypass, b.eq, b.gate, b.compressor, b.limiter);
		}
	}

	void ParameterRamp::set(double newValue, uint32_t ramp) noexcept
	{
		if (ramp == 0)
		{
			value = target = newValue;
			length = 0;
			return;
		}
		start = value;
		target = newValue;
		length = ramp;
		done = 0;
	}

	double ParameterRamp::next() noexcept
	{
		if (length == 0)
			return value;
		const double current = value;
		++done;
		if (done >= length)
		{
			value = target;
			length = 0;
		}
		else
		{
			value = start + ((target - start) * (static_cast<double>(done) / static_cast<double>(length)));
		}
		return current;
	}

	StripSections::StripSections()
	{
		m_gain.set(1.0, 0);
		// Each equalizer has only its own bands: the cuts the first and the last, the channel EQ those between.
		for (int band = 0; band < Equalizer::g_bands; ++band)
			(band == 0 || band == Equalizer::g_bands - 1 ? m_eq : m_cuts)
				.core.setParameter(Equalizer::parameter(band, Equalizer::On), 0.0);
	}

	void StripSections::prepare(double sampleRate, uint32_t maxFrames)
	{
		if (sampleRate == m_sampleRate && maxFrames == m_maxFrames)
			return;
		m_sampleRate = sampleRate;
		m_maxFrames = maxFrames;
		m_tailFrames = static_cast<uint32_t>(sampleRate * g_filterTailSeconds);
		m_polarity.setTime(dsp::g_smoothingSeconds, sampleRate);
		m_cuts.core.prepare(sampleRate, maxFrames);
		m_eq.core.prepare(sampleRate, maxFrames);
		if (!m_freshPre)
			m_cuts.core.glideChanges();
		if (!m_freshPost)
			m_eq.core.glideChanges();
		m_gate.prepare(sampleRate, maxFrames);
		m_compressor.prepare(sampleRate, maxFrames);
		m_limiter.prepare(sampleRate, maxFrames);
		m_limiterRuns = false; // prepare cleared it
	}

	template <typename Render>
	void StripSections::play(const graph::EventBuffer& events, bool pre, uint32_t frames, const Render& render) noexcept
	{
		uint32_t at = 0;
		for (const graph::ProcessEvent& event : events)
		{
			if (event.type != graph::ProcessEvent::Type::Parameter || event.parameter < g_stripEvents ||
				isPreSection(event.parameter - g_stripEvents) != pre)
				continue;
			const uint32_t offset = std::min(event.offset, frames);
			if (offset > at)
			{
				render(at, offset);
				at = offset;
			}
			const uint32_t id = event.parameter - g_stripEvents;
			if (id == StripParameter::PreGain)
				m_gain.set(event.parameterValue, event.ramp);
			else
				setStripValue(m_values, id, event.parameterValue);
		}
		if (at < frames)
			render(at, frames);
	}

	void StripSections::applyPre() noexcept
	{
		const ChannelSections& settings = m_values;
		if (!m_freshPre && samePre(settings, m_appliedPre))
			return;
		const ChannelSections& old = m_appliedPre;
		const bool all = m_freshPre;
		m_polarity.set(settings.phase ? -1.0 : 1.0, !all);
		if (all || !(settings.lowCut == old.lowCut) || !(settings.highCut == old.highCut))
		{
			const auto cut = [this](int band, const CutFilter& filter, dsp::FilterType type)
			{ setBand(m_cuts.core, band, filter.on, type, filter.frequency, 0.0, 0.71, slopeIndex(filter.slope)); };
			cut(0, settings.lowCut, dsp::FilterType::LowCut);
			cut(Equalizer::g_bands - 1, settings.highCut, dsp::FilterType::HighCut);
			m_cuts.on = settings.lowCut.on || settings.highCut.on;
		}
		m_appliedPre = settings;
		m_freshPre = false;
		// Only the first values apply at once, even to an equalizer that hasn't processed yet.
		m_cuts.core.glideChanges();
	}

	void StripSections::applyPost() noexcept
	{
		const ChannelSections& settings = m_values;
		if (!m_freshPost && samePost(settings, m_appliedPost))
			return;
		const ChannelSections& old = m_appliedPost;
		const bool all = m_freshPost;
		if (all || settings.eqBypass != old.eqBypass || settings.eq != old.eq)
		{
			m_eq.on = false;
			for (size_t i = 0; i < settings.eq.size(); ++i)
			{
				const EqBand& band = settings.eq[i];
				const bool on = band.on && !settings.eqBypass;
				setBand(m_eq.core, static_cast<int>(i) + 1, on, filterType(band.type), band.frequency, band.gain,
					band.q, slopeIndex(12));
				m_eq.on = m_eq.on || on;
			}
		}

		// Dynamics that come on start afresh, rather than with what they held when they went off.
		const StripGate& gate = settings.gate;
		if (gate.on && (all || !old.gate.on))
			m_gate.reset();
		m_gate.setParameter(dsp::Gate::Threshold, gate.threshold);
		m_gate.setParameter(dsp::Gate::Range, gate.range);
		m_gate.setParameter(dsp::Gate::Attack, gate.attack);
		m_gate.setParameter(dsp::Gate::Release, gate.release);
		const StripCompressor& compressor = settings.compressor;
		if (compressor.on && (all || !old.compressor.on))
			m_compressor.reset();
		m_compressor.setParameter(dsp::Compressor::Threshold, compressor.threshold);
		m_compressor.setParameter(dsp::Compressor::Ratio, compressor.ratio);
		m_compressor.setParameter(dsp::Compressor::Attack, compressor.attack);
		m_compressor.setParameter(dsp::Compressor::Release, compressor.release);
		m_compressor.setParameter(dsp::Compressor::Makeup, compressor.makeup);
		m_limiter.setParameter(dsp::Limiter::Ceiling, settings.limiter.threshold);
		m_limiter.setParameter(dsp::Limiter::Release, settings.limiter.release);

		m_appliedPost = settings;
		m_freshPost = false;
		m_eq.core.glideChanges();
	}

	void StripSections::Filters::process(
		float* const* audio, int channels, uint32_t begin, uint32_t end, uint32_t tailFrames) noexcept
	{
		if (!on && tail == 0)
			return;
		// Coming back after it stopped needs no reset: a band clears the filter sections it starts to use.
		tail = on ? tailFrames : tail - std::min(tail, end - begin);
		core.process(audio, channels, begin, end);
	}

	void StripSections::processPre(
		const graph::EventBuffer& events, float* const* audio, int channels, uint32_t frames) noexcept
	{
		play(events, true, frames,
			[&](uint32_t begin, uint32_t end)
			{
				applyPre();
				if (m_gain.length != 0 || !m_polarity.settled() || m_gain.value * m_polarity.value() != 1.0)
					for (uint32_t i = begin; i < end; ++i)
					{
						const auto gain = static_cast<float>(m_gain.next() * m_polarity.next());
						for (int c = 0; c < channels; ++c)
							audio[c][i] *= gain;
					}
				m_cuts.process(audio, channels, begin, end, m_tailFrames);
			});
	}

	void StripSections::processPost(
		const graph::EventBuffer& events, bool lookAhead, float* const* audio, int channels, uint32_t frames) noexcept
	{
		play(events, false, frames,
			[&](uint32_t begin, uint32_t end)
			{
				applyPost();
				const ChannelSections& settings = m_values;
				if (settings.gate.on)
					m_gate.process(audio, channels, begin, end);
				if (settings.compressor.on)
					m_compressor.process(audio, channels, begin, end);
				m_eq.process(audio, channels, begin, end, m_tailFrames);
				// A limiter that comes on starts afresh, unless it ran bypassed to keep its look-ahead.
				const bool runs = settings.limiter.on || lookAhead;
				if (runs && !m_limiterRuns)
					m_limiter.reset();
				m_limiterRuns = runs;
				if (runs)
				{
					m_limiter.setBypass(!settings.limiter.on);
					m_limiter.process(audio, channels, begin, end);
				}
			});
	}
}
