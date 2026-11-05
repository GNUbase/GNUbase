// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/meters.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace gnubase::engine
{
	namespace
	{
		constexpr double g_publishSeconds = 0.1; // how often the level meters publish
		constexpr double g_absoluteGate = -70.0; // LUFS
		constexpr double g_pi = 3.14159265358979323846;
		constexpr double g_nothing = std::numeric_limits<double>::quiet_NaN();

		double toLoudness(double meanSquare)
		{
			return -0.691 + (10.0 * std::log10(meanSquare));
		}

		std::optional<double> value(double published)
		{
			return std::isnan(published) ? std::nullopt : std::optional<double>(published);
		}

		// The modified Bessel function of the first kind of order 0, for the Kaiser window.
		double besselI0(double x)
		{
			double sum = 1.0;
			double term = 1.0;
			for (int k = 1; k < 50; ++k)
			{
				term *= (x / (2.0 * k)) * (x / (2.0 * k));
				sum += term;
			}
			return sum;
		}

		// Raises a held value for the one writer, against the reader that takes it (exchange): lock-free, and no
		// value goes unseen.
		void raise(std::atomic<float>& held, float value) noexcept
		{
			float current = held.load(std::memory_order_relaxed);
			while (value > current && !held.compare_exchange_weak(current, value, std::memory_order_relaxed))
			{
			}
		}

		constexpr float g_aes17 = 1.41421356f; // AES17's +3.01 dB on the RMS level
	}

	void LevelMeter::process(const float* const* audio, int channels, uint32_t frames, double sampleRate) noexcept
	{
		channels = std::clamp(channels, 0, g_maxMeterChannels);
		if (const uint32_t resets = m_resets.load(std::memory_order_acquire); resets != m_resetsSeen)
		{
			m_resetsSeen = resets;
			m_peakMax.fill(0.0f);
			m_rmsMax.fill(0.0f);
		}
		// Channels that come back, e.g. a mono channel's second side, start without what they measured before.
		for (int c = m_channels.load(std::memory_order_relaxed); c < channels; ++c)
		{
			const auto index = static_cast<size_t>(c);
			m_windowPeak[index] = 0.0f;
			m_sliceSquares[index] = 0.0;
			m_squares[index] = {};
			m_peakMax[index] = 0.0f;
			m_rmsMax[index] = 0.0f;
			m_peak[index].store(0.0f, std::memory_order_relaxed);
			m_framePeak[index].store(0.0f, std::memory_order_relaxed);
			m_rms[index].store(0.0f, std::memory_order_relaxed);
			m_publishedPeakMax[index].store(0.0f, std::memory_order_relaxed);
			m_publishedRmsMax[index].store(0.0f, std::memory_order_relaxed);
		}
		m_channels.store(channels, std::memory_order_relaxed);
		// The RMS window follows its resolution and the sample rate: a new one starts empty.
		if (const int resolution = m_rmsResolution.load(std::memory_order_relaxed);
			resolution != m_resolution || sampleRate != m_sampleRate)
		{
			m_resolution = resolution;
			m_sampleRate = sampleRate;
			m_rmsFrames = static_cast<uint32_t>(std::max(1L, std::lround(resolution * sampleRate / 1000.0)));
			m_slices = static_cast<int>(std::min<uint32_t>(g_slices, m_rmsFrames));
			m_squares = {};
			m_sliceSquares = {};
			m_sliceLengths = {};
			m_sliceIndex = 0;
			m_sliceFrames = 0;
			m_squaresFrames = 0;
			m_windowLength = static_cast<uint32_t>(std::max(1L, std::lround(g_publishSeconds * sampleRate)));
			m_windowFrames = std::min(m_windowFrames, m_windowLength - 1);
		}
		std::array<float, g_maxMeterChannels> blockPeak{};
		for (uint32_t done = 0; done < frames;)
		{
			// Up to where the current slice or the 100 ms peak window ends. The slices share the window's frames out
			// evenly, so that the ring holds exactly the window once it is full.
			const auto slice = static_cast<uint64_t>(m_sliceIndex);
			const auto sliceLength =
				static_cast<uint32_t>((((slice + 1) * m_rmsFrames) / static_cast<uint64_t>(m_slices)) -
					((slice * m_rmsFrames) / static_cast<uint64_t>(m_slices)));
			const uint32_t count =
				std::min({frames - done, sliceLength - m_sliceFrames, m_windowLength - m_windowFrames});
			for (size_t c = 0; c < static_cast<size_t>(channels); ++c)
			{
				float peak = blockPeak[c];
				double squares = m_sliceSquares[c];
				for (uint32_t i = done; i < done + count; ++i)
				{
					const float x = audio[c][i];
					peak = std::max(peak, std::abs(x));
					squares += static_cast<double>(x) * x;
				}
				blockPeak[c] = peak;
				m_windowPeak[c] = std::max(m_windowPeak[c], peak);
				m_sliceSquares[c] = squares;
			}
			done += count;
			m_sliceFrames += count;
			m_windowFrames += count;
			if (m_sliceFrames == sliceLength)
				endSlice(channels);
			if (m_windowFrames == m_windowLength)
				endWindow(channels);
		}
		for (size_t c = 0; c < static_cast<size_t>(channels); ++c)
		{
			m_peakMax[c] = std::max(m_peakMax[c], blockPeak[c]);
			m_publishedPeakMax[c].store(m_peakMax[c], std::memory_order_relaxed);
			raise(m_framePeak[c], blockPeak[c]);
		}
	}

	void LevelMeter::endSlice(int channels) noexcept
	{
		// The slice replaces the oldest in the ring, which the RMS level averages.
		const auto slot = static_cast<size_t>(m_sliceIndex);
		m_squaresFrames += m_sliceFrames - m_sliceLengths[slot];
		m_sliceLengths[slot] = m_sliceFrames;
		m_sliceIndex = (m_sliceIndex + 1) % m_slices;
		m_sliceFrames = 0;
		for (size_t c = 0; c < static_cast<size_t>(channels); ++c)
		{
			m_squares[c][slot] = m_sliceSquares[c];
			m_sliceSquares[c] = 0.0;
			double sum = 0.0;
			for (size_t s = 0; s < static_cast<size_t>(m_slices); ++s)
				sum += m_squares[c][s];
			const auto rms = static_cast<float>(std::sqrt(sum / std::max<uint32_t>(m_squaresFrames, 1)));
			m_rmsMax[c] = std::max(m_rmsMax[c], rms);
			m_rms[c].store(rms, std::memory_order_relaxed);
			m_publishedRmsMax[c].store(m_rmsMax[c], std::memory_order_relaxed);
		}
	}

	void LevelMeter::endWindow(int channels) noexcept
	{
		m_windowFrames = 0;
		for (size_t c = 0; c < static_cast<size_t>(channels); ++c)
		{
			m_peak[c].store(m_windowPeak[c], std::memory_order_relaxed);
			m_windowPeak[c] = 0.0f;
		}
	}

	LevelReading LevelMeter::read() const noexcept
	{
		const float rmsGain = m_aes17.load(std::memory_order_relaxed) ? g_aes17 : 1.0f;
		LevelReading reading;
		reading.channels = m_channels.load(std::memory_order_relaxed);
		for (size_t c = 0; c < static_cast<size_t>(reading.channels); ++c)
		{
			reading.peak[c] = m_peak[c].load(std::memory_order_relaxed);
			reading.rms[c] = m_rms[c].load(std::memory_order_relaxed) * rmsGain;
			reading.peakMax[c] = m_publishedPeakMax[c].load(std::memory_order_relaxed);
			reading.rmsMax[c] = m_publishedRmsMax[c].load(std::memory_order_relaxed) * rmsGain;
		}
		return reading;
	}

	LevelReading LevelMeter::readFrame() noexcept
	{
		LevelReading reading = read();
		for (size_t c = 0; c < static_cast<size_t>(reading.channels); ++c)
			reading.peak[c] = m_framePeak[c].exchange(0.0f, std::memory_order_relaxed);
		return reading;
	}

	void LevelMeter::setRms(int milliseconds, bool aes17) noexcept
	{
		m_rmsResolution.store(std::clamp(milliseconds, 1, 1000), std::memory_order_relaxed);
		m_aes17.store(aes17, std::memory_order_relaxed);
	}

	void LevelMeter::reset() noexcept
	{
		// The audio thread clears its own maxima at its next block; until then reads show none.
		for (size_t c = 0; c < m_publishedPeakMax.size(); ++c)
		{
			m_publishedPeakMax[c].store(0.0f, std::memory_order_relaxed);
			m_publishedRmsMax[c].store(0.0f, std::memory_order_relaxed);
		}
		m_resets.fetch_add(1, std::memory_order_release);
	}

	void LoudnessMeter::Histogram::add(double meanSquare) noexcept
	{
		if (!(meanSquare > 0.0))
			return;
		const double loudness = toLoudness(meanSquare);
		if (loudness < g_absoluteGate)
			return;
		const auto bin = std::min(static_cast<int>((loudness - g_absoluteGate) * 10.0), g_bins - 1);
		energy[static_cast<size_t>(bin)] += meanSquare;
		++count[static_cast<size_t>(bin)];
	}

	void LoudnessMeter::Histogram::clear() noexcept
	{
		energy.fill(0.0);
		count.fill(0);
	}

	std::pair<double, uint64_t> LoudnessMeter::Histogram::above(double loudness) const noexcept
	{
		double sum = 0.0;
		uint64_t blocks = 0;
		for (int bin = 0; bin < g_bins; ++bin)
			if (g_absoluteGate + ((bin + 0.5) / 10.0) > loudness)
			{
				sum += energy[static_cast<size_t>(bin)];
				blocks += count[static_cast<size_t>(bin)];
			}
		return {blocks > 0 ? sum / static_cast<double>(blocks) : 0.0, blocks};
	}

	LoudnessMeter::LoudnessMeter()
		: m_momentary(g_nothing), m_shortTerm(g_nothing), m_publishedMomentaryMax(g_nothing),
		  m_publishedShortTermMax(g_nothing), m_integrated(g_nothing), m_range(g_nothing)
	{
		// The true peak interpolator: a Kaiser-windowed sinc for 4x oversampling, centred on a tap so that its phases
		// fall on the samples and 1/4, 1/2 and 3/4 between them; each phase scaled to a gain of 1.
		constexpr double beta = 7.0;
		const double center = g_taps / 2.0;
		for (int n = 0; n < g_taps; ++n)
		{
			const double x = (n - center) / 4.0;
			const double sinc = x == 0.0 ? 1.0 : std::sin(g_pi * x) / (g_pi * x);
			const double r = (n - center) / center;
			m_interpolator[static_cast<size_t>(n)] = sinc * besselI0(beta * std::sqrt(std::max(0.0, 1.0 - (r * r))));
		}
		for (int phase = 0; phase < 4; ++phase)
		{
			double sum = 0.0;
			for (int n = phase; n < g_taps; n += 4)
				sum += m_interpolator[static_cast<size_t>(n)];
			for (int n = phase; n < g_taps; n += 4)
				m_interpolator[static_cast<size_t>(n)] /= sum;
		}
		prepare(48000.0);
	}

	void LoudnessMeter::prepare(double sampleRate, bool keepMeasurement)
	{
		const bool keep = keepMeasurement && sampleRate == m_sampleRate;
		m_sampleRate = sampleRate;
		m_filters = analysis::kWeighting(sampleRate);
		m_stepLength = static_cast<uint32_t>(std::max(1L, std::lround(g_publishSeconds * sampleRate)));
		m_filterState = {};
		m_history = {};
		m_stepFrames = 0;
		m_stepEnergy = 0.0;
		m_stepCount = 0;
		m_stepIndex = 0;
		m_windowTruePeak.fill(0.0f);
		m_momentary.store(g_nothing);
		m_shortTerm.store(g_nothing);
		// Blocks start again where playback goes on, so none spans the restart.
		m_playingSteps = 0;
		m_wasMeasuring = false;
		m_freshSamples = 0;
		for (std::atomic<float>& peak : m_frameTruePeak)
			peak.store(0.0f, std::memory_order_relaxed);
		if (!keep)
			clear();
		publish();
	}

	void LoudnessMeter::clear() noexcept
	{
		m_blocks.clear();
		m_shortTerms.clear();
		m_momentaryMax.reset();
		m_shortTermMax.reset();
		m_truePeakMax.fill(0.0f);
		m_measuredSteps = 0;
		m_playingSteps = 0;
	}

	void LoudnessMeter::process(const float* const* audio, int channels, uint32_t frames, bool measuring) noexcept
	{
		channels = std::clamp(channels, 0, g_maxMeterChannels);
		if (const uint32_t resets = m_resets.load(std::memory_order_acquire); resets != m_resetsSeen)
		{
			m_resetsSeen = resets;
			clear();
		}
		if (channels != m_channels)
		{
			// Channels that come back start without what they measured before.
			for (size_t c = static_cast<size_t>(std::max(m_channels, 0)); c < static_cast<size_t>(channels); ++c)
			{
				m_windowTruePeak[c] = 0.0f;
				m_truePeakMax[c] = 0.0f;
				m_truePeak[c].store(0.0f, std::memory_order_relaxed);
				m_frameTruePeak[c].store(0.0f, std::memory_order_relaxed);
				m_publishedTruePeakMax[c].store(0.0f, std::memory_order_relaxed);
			}
			m_channels = channels;
			m_filterState = {};
			m_history = {};
			m_publishedChannels.store(channels, std::memory_order_relaxed);
		}
		if (measuring && !m_wasMeasuring)
		{
			// Playback starts: a fresh step, so no block or true peak holds audio from while the transport stood.
			m_stepEnergy = 0.0;
			m_stepFrames = 0;
			m_windowTruePeak.fill(0.0f);
			m_freshSamples = g_taps / 4;
		}
		m_wasMeasuring = measuring;
		const analysis::Biquad& shelf = m_filters[0];
		const analysis::Biquad& highPass = m_filters[1];
		constexpr int length = g_taps / 4;
		for (uint32_t i = 0; i < frames; ++i)
		{
			double energy = 0.0;
			for (size_t c = 0; c < static_cast<size_t>(channels); ++c)
			{
				const float sample = audio[c][i];
				// K-weighting: the shelf, then the high pass, transposed direct form II.
				std::array<double, 4>& z = m_filterState[c];
				const double x = sample;
				const double y1 = (shelf.b0 * x) + z[0];
				z[0] = (shelf.b1 * x) - (shelf.a1 * y1) + z[1];
				z[1] = (shelf.b2 * x) - (shelf.a2 * y1);
				const double y2 = (highPass.b0 * y1) + z[2];
				z[2] = (highPass.b1 * y1) - (highPass.a1 * y2) + z[3];
				z[3] = (highPass.b2 * y1) - (highPass.a2 * y2);
				energy += y2 * y2; // each channel weighted 1, as front left, right and center are

				// True peak: the sample and the three points between it and the one before, interpolated.
				std::array<float, g_taps / 2>& history = m_history[c];
				history[static_cast<size_t>(m_historyIndex)] = sample;
				history[static_cast<size_t>(m_historyIndex) + static_cast<size_t>(length)] = sample;
				float peak = std::max(m_windowTruePeak[c], std::abs(sample));
				const float* newest = history.data() + m_historyIndex + length; // newest[-k] is k samples ago
				// Right after playback starts, the points between the samples would still read audio from before.
				for (int phase = 0; phase < 4 && m_freshSamples == 0; ++phase)
				{
					double interpolated = 0.0;
					for (int k = 0; k < length; ++k)
						interpolated +=
							m_interpolator[static_cast<size_t>(phase) + (4 * static_cast<size_t>(k))] * newest[-k];
					peak = std::max(peak, static_cast<float>(std::abs(interpolated)));
				}
				m_windowTruePeak[c] = peak;
			}
			m_historyIndex = (m_historyIndex + 1) % length;
			if (m_freshSamples > 0)
				--m_freshSamples;
			m_stepEnergy += energy;
			if (++m_stepFrames == m_stepLength)
				step(measuring);
		}
		for (size_t c = 0; c < static_cast<size_t>(channels); ++c)
			raise(m_frameTruePeak[c], m_windowTruePeak[c]);
	}

	void LoudnessMeter::step(bool measuring) noexcept
	{
		const double meanSquare = m_stepEnergy / m_stepLength;
		m_stepEnergy = 0.0;
		m_stepFrames = 0;
		m_steps[static_cast<size_t>(m_stepIndex)] = meanSquare;
		m_stepIndex = (m_stepIndex + 1) % g_steps;
		m_stepCount = std::min(m_stepCount + 1, g_steps);
		// The mean square of the last steps.
		const auto last = [this](int steps)
		{
			double sum = 0.0;
			for (int k = 1; k <= steps; ++k)
				sum += m_steps[static_cast<size_t>((m_stepIndex - k + g_steps) % g_steps)];
			return sum / steps;
		};
		const double momentary = m_stepCount >= 4 ? last(4) : 0.0;
		const double shortTerm = m_stepCount >= g_steps ? last(g_steps) : 0.0;
		m_momentary.store(momentary > 0.0 ? toLoudness(momentary) : g_nothing, std::memory_order_relaxed);
		m_shortTerm.store(shortTerm > 0.0 ? toLoudness(shortTerm) : g_nothing, std::memory_order_relaxed);

		m_playingSteps = measuring ? m_playingSteps + 1 : 0;
		if (measuring)
		{
			++m_measuredSteps;
			// Blocks of 400 ms every 100 ms, and of 3 s every second, from where playback started: none holds audio
			// from while the transport stood.
			if (m_playingSteps >= 4 && momentary > 0.0)
			{
				m_blocks.add(momentary);
				m_momentaryMax = std::max(m_momentaryMax.value_or(-1e9), toLoudness(momentary));
			}
			if (m_playingSteps >= g_steps && shortTerm > 0.0)
			{
				m_shortTermMax = std::max(m_shortTermMax.value_or(-1e9), toLoudness(shortTerm));
				if ((m_playingSteps - g_steps) % 10 == 0)
					m_shortTerms.add(shortTerm);
			}
			for (size_t c = 0; c < static_cast<size_t>(m_channels); ++c)
				m_truePeakMax[c] = std::max(m_truePeakMax[c], m_windowTruePeak[c]);
		}
		publish();
		for (size_t c = 0; c < static_cast<size_t>(m_channels); ++c)
			raise(m_frameTruePeak[c], m_windowTruePeak[c]);
		m_windowTruePeak.fill(0.0f);
	}

	void LoudnessMeter::publish() noexcept
	{
		// Integrated: the blocks above the absolute gate, then those above 10 LU below their loudness.
		double integrated = g_nothing;
		if (const auto [meanSquare, blocks] = m_blocks.above(g_absoluteGate); blocks > 0)
			if (const auto [gated, count] = m_blocks.above(toLoudness(meanSquare) - 10.0); count > 0)
				integrated = toLoudness(gated);
		// Loudness range: the short-term blocks above 20 LU below their loudness, from the 10th to the 95th
		// percentile.
		double range = g_nothing;
		if (const auto [meanSquare, blocks] = m_shortTerms.above(g_absoluteGate); blocks > 0)
		{
			const double gate = toLoudness(meanSquare) - 20.0;
			uint64_t total = 0;
			for (int bin = 0; bin < Histogram::g_bins; ++bin)
				if (g_absoluteGate + ((bin + 0.5) / 10.0) > gate)
					total += m_shortTerms.count[static_cast<size_t>(bin)];
			std::optional<double> low;
			std::optional<double> high;
			uint64_t seen = 0;
			for (int bin = 0; bin < Histogram::g_bins && total > 0; ++bin)
			{
				const double center = g_absoluteGate + ((bin + 0.5) / 10.0);
				if (center <= gate)
					continue;
				seen += m_shortTerms.count[static_cast<size_t>(bin)];
				if (!low && static_cast<double>(seen) >= 0.10 * static_cast<double>(total))
					low = center;
				if (!high && static_cast<double>(seen) >= 0.95 * static_cast<double>(total))
					high = center;
			}
			if (low && high)
				range = *high - *low;
		}
		m_integrated.store(integrated, std::memory_order_relaxed);
		m_range.store(range, std::memory_order_relaxed);
		m_publishedMomentaryMax.store(m_momentaryMax.value_or(g_nothing), std::memory_order_relaxed);
		m_publishedShortTermMax.store(m_shortTermMax.value_or(g_nothing), std::memory_order_relaxed);
		m_seconds.store(static_cast<double>(m_measuredSteps) * g_publishSeconds, std::memory_order_relaxed);
		for (size_t c = 0; c < static_cast<size_t>(g_maxMeterChannels); ++c)
		{
			m_truePeak[c].store(m_windowTruePeak[c], std::memory_order_relaxed);
			m_publishedTruePeakMax[c].store(m_truePeakMax[c], std::memory_order_relaxed);
		}
	}

	LoudnessReading LoudnessMeter::read() const noexcept
	{
		LoudnessReading reading;
		reading.momentary = value(m_momentary.load(std::memory_order_relaxed));
		reading.shortTerm = value(m_shortTerm.load(std::memory_order_relaxed));
		reading.momentaryMax = value(m_publishedMomentaryMax.load(std::memory_order_relaxed));
		reading.shortTermMax = value(m_publishedShortTermMax.load(std::memory_order_relaxed));
		reading.integrated = value(m_integrated.load(std::memory_order_relaxed));
		reading.range = value(m_range.load(std::memory_order_relaxed));
		reading.seconds = m_seconds.load(std::memory_order_relaxed);
		reading.channels = m_publishedChannels.load(std::memory_order_relaxed);
		for (size_t c = 0; c < static_cast<size_t>(reading.channels); ++c)
		{
			reading.truePeak[c] = m_truePeak[c].load(std::memory_order_relaxed);
			reading.truePeakMax[c] = m_publishedTruePeakMax[c].load(std::memory_order_relaxed);
		}
		return reading;
	}

	LoudnessReading LoudnessMeter::readFrame() noexcept
	{
		LoudnessReading reading = read();
		for (size_t c = 0; c < static_cast<size_t>(reading.channels); ++c)
			reading.truePeak[c] = m_frameTruePeak[c].exchange(0.0f, std::memory_order_relaxed);
		return reading;
	}

	void LoudnessMeter::reset() noexcept
	{
		// The audio thread starts over at its next block; until then reads show nothing measured.
		m_integrated.store(g_nothing, std::memory_order_relaxed);
		m_range.store(g_nothing, std::memory_order_relaxed);
		m_publishedMomentaryMax.store(g_nothing, std::memory_order_relaxed);
		m_publishedShortTermMax.store(g_nothing, std::memory_order_relaxed);
		m_seconds.store(0.0, std::memory_order_relaxed);
		for (std::atomic<float>& peak : m_publishedTruePeakMax)
			peak.store(0.0f, std::memory_order_relaxed);
		m_resets.fetch_add(1, std::memory_order_release);
	}
}
