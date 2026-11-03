// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/stripsections.h"

#include <algorithm>
#include <array>

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
	}

	StripSections::StripSections()
	{
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
		m_gain.setTime(dsp::g_smoothingSeconds, sampleRate);
		m_cuts.core.prepare(sampleRate, maxFrames);
		m_eq.core.prepare(sampleRate, maxFrames);
		m_gate.prepare(sampleRate, maxFrames);
		m_compressor.prepare(sampleRate, maxFrames);
		m_limiter.prepare(sampleRate, maxFrames);
	}

	uint32_t StripSections::latency(const ChannelSections& settings) const
	{
		return settings.limiter.on ? m_limiter.latency() : 0;
	}

	void StripSections::apply(const ChannelSections& settings) noexcept
	{
		if (!m_fresh && settings == m_applied)
			return;
		const ChannelSections& old = m_applied;
		const bool all = m_fresh;
		m_gain.set(dsp::dbToGain(settings.gain) * (settings.phase ? -1.0 : 1.0), !all);

		if (all || !(settings.lowCut == old.lowCut) || !(settings.highCut == old.highCut))
		{
			const auto cut = [this](int band, const CutFilter& filter, dsp::FilterType type)
			{ setBand(m_cuts.core, band, filter.on, type, filter.frequency, 0.0, 0.71, slopeIndex(filter.slope)); };
			cut(0, settings.lowCut, dsp::FilterType::LowCut);
			cut(Equalizer::g_bands - 1, settings.highCut, dsp::FilterType::HighCut);
			m_cuts.on = settings.lowCut.on || settings.highCut.on;
		}
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
		if (settings.limiter.on && (all || !old.limiter.on))
			m_limiter.reset();
		m_limiter.setParameter(dsp::Limiter::Ceiling, settings.limiter.threshold);
		m_limiter.setParameter(dsp::Limiter::Release, settings.limiter.release);

		m_applied = settings;
		m_fresh = false;
	}

	void StripSections::Filters::process(
		float* const* audio, int channels, uint32_t frames, uint32_t tailFrames) noexcept
	{
		if (!on && tail == 0)
			return;
		if (!on)
			tail -= std::min(tail, frames);
		else if (tail == 0)
			core.reset(); // coming back after it stopped: what its filters held then is long gone
		if (on)
			tail = tailFrames;
		core.process(audio, channels, 0, frames);
	}

	void StripSections::processPre(
		const ChannelSections& settings, float* const* audio, int channels, uint32_t frames) noexcept
	{
		apply(settings);
		if (!m_gain.settled() || m_gain.value() != 1.0)
			for (uint32_t i = 0; i < frames; ++i)
			{
				const auto gain = static_cast<float>(m_gain.next());
				for (int c = 0; c < channels; ++c)
					audio[c][i] *= gain;
			}
		m_cuts.process(audio, channels, frames, m_tailFrames);
	}

	void StripSections::processPost(
		const ChannelSections& settings, float* const* audio, int channels, uint32_t frames) noexcept
	{
		apply(settings);
		if (settings.gate.on)
			m_gate.process(audio, channels, 0, frames);
		if (settings.compressor.on)
			m_compressor.process(audio, channels, 0, frames);
		m_eq.process(audio, channels, frames, m_tailFrames);
		if (settings.limiter.on)
			m_limiter.process(audio, channels, 0, frames);
	}
}
