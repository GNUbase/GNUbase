// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/analysis/analysis.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstddef>
#include <cstdint>

namespace gnubase::engine::analysis
{
	namespace
	{
		constexpr double g_pi = 3.14159265358979323846;
		constexpr size_t g_fftSize = 8192;
		constexpr size_t g_minFftSize = 64;
		constexpr size_t g_lobe = 4;            // bins on each side of a peak: the Blackman-Harris window's main lobe
		constexpr double g_peakRange = 80.0;    // dB below the strongest peak that still count; leakage is below 92 dB
		constexpr double g_gate = -70.0;        // LUFS, BS.1770's absolute gate
		constexpr double g_relativeGate = 10.0; // LU below the loudness of the blocks above the absolute gate
		constexpr double g_loudnessOffset = -0.691; // makes a 1 kHz sine in one channel read like its RMS level

		double toLoudness(double meanSquare)
		{
			return g_loudnessOffset + (10.0 * std::log10(meanSquare));
		}

		// The symmetric 4-term Blackman-Harris window: sidelobes 92 dB down.
		std::vector<double> blackmanHarris(size_t length)
		{
			std::vector<double> window(length, 1.0);
			if (length < 2)
				return window;
			const auto denominator = static_cast<double>(length - 1);
			for (size_t i = 0; i < length; ++i)
			{
				const double x = 2.0 * g_pi * static_cast<double>(i) / denominator;
				window[i] =
					0.35875 - (0.48829 * std::cos(x)) + (0.14128 * std::cos(2.0 * x)) - (0.01168 * std::cos(3.0 * x));
			}
			return window;
		}

		// An iterative radix-2 FFT in place; the size is a power of two.
		class Fft
		{
		public:
			explicit Fft(size_t size) : m_twiddles(size / 2)
			{
				for (size_t k = 0; k < m_twiddles.size(); ++k)
					m_twiddles[k] = std::polar(1.0, -2.0 * g_pi * static_cast<double>(k) / static_cast<double>(size));
			}

			void transform(std::vector<std::complex<double>>& data) const
			{
				const size_t size = data.size();
				for (size_t i = 1, j = 0; i < size; ++i)
				{
					size_t bit = size >> 1;
					for (; (j & bit) != 0; bit >>= 1)
						j ^= bit;
					j ^= bit;
					if (i < j)
						std::swap(data[i], data[j]);
				}
				for (size_t length = 2; length <= size; length <<= 1)
				{
					const size_t half = length / 2;
					const size_t stride = size / length;
					for (size_t start = 0; start < size; start += length)
					{
						for (size_t k = 0; k < half; ++k)
						{
							const std::complex<double> even = data[start + k];
							const std::complex<double> odd = data[start + k + half] * m_twiddles[k * stride];
							data[start + k] = even + odd;
							data[start + k + half] = even - odd;
						}
					}
				}
			}

		private:
			std::vector<std::complex<double>> m_twiddles;
		};

		double powerDecibels(double power)
		{
			return 10.0 * std::log10(power);
		}

		// A biquad that filters samples one by one, in transposed direct form II, which keeps rounding low for the
		// K-weighting's low corner frequency.
		class BiquadFilter
		{
		public:
			explicit BiquadFilter(const Biquad& coefficients) : m_c(coefficients) {}

			double process(double x)
			{
				const double y = (m_c.b0 * x) + m_z1;
				m_z1 = (m_c.b1 * x) - (m_c.a1 * y) + m_z2;
				m_z2 = (m_c.b2 * x) - (m_c.a2 * y);
				return y;
			}

		private:
			Biquad m_c;
			double m_z1 = 0.0;
			double m_z2 = 0.0;
		};

		// The integrated loudness in LUFS of the K-weighted squares of every channel added up per 100 ms step of step
		// frames.
		std::optional<double> integratedLoudness(const std::vector<double>& stepSquares, int64_t step)
		{
			if (stepSquares.size() < 4 || step <= 0)
				return std::nullopt;
			const size_t blocks = stepSquares.size() - 3;
			const double blockLength = 4.0 * static_cast<double>(step);
			std::vector<double> meanSquares(blocks);
			for (size_t j = 0; j < blocks; ++j)
				meanSquares[j] =
					(stepSquares[j] + stepSquares[j + 1] + stepSquares[j + 2] + stepSquares[j + 3]) / blockLength;

			// The mean square of the blocks louder than the threshold, or nothing if there are none.
			const auto gated = [&meanSquares](double threshold) -> std::optional<double>
			{
				double sum = 0.0;
				size_t count = 0;
				for (const double meanSquare : meanSquares)
				{
					if (meanSquare > 0.0 && toLoudness(meanSquare) > threshold)
					{
						sum += meanSquare;
						++count;
					}
				}
				if (count == 0)
					return std::nullopt;
				return sum / static_cast<double>(count);
			};
			const std::optional<double> aboveGate = gated(g_gate);
			if (!aboveGate)
				return std::nullopt;
			const std::optional<double> aboveRelativeGate = gated(toLoudness(*aboveGate) - g_relativeGate);
			if (!aboveRelativeGate)
				return std::nullopt;
			return toLoudness(*aboveRelativeGate);
		}
	}

	Levels levels(const std::vector<float>& samples)
	{
		Levels result;
		double squares = 0.0;
		for (const float sample : samples)
		{
			const double value = sample;
			result.peak = std::max(result.peak, std::abs(value));
			squares += value * value;
		}
		if (!samples.empty())
			result.rms = std::sqrt(squares / static_cast<double>(samples.size()));
		return result;
	}

	std::optional<double> decibels(double level)
	{
		if (!(level > 0.0))
			return std::nullopt;
		return 20.0 * std::log10(level);
	}

	std::array<Biquad, 2> kWeighting(double sampleRate)
	{
		// The filters of BS.1770's 48 kHz coefficients, made for any sample rate with the bilinear transform.
		Biquad shelf;
		{
			const double frequency = 1681.974450955533;
			const double gain = 3.999843853973347; // dB
			const double q = 0.7071752369554196;
			const double k = std::tan(g_pi * frequency / sampleRate);
			const double vh = std::pow(10.0, gain / 20.0);
			const double vb = std::pow(vh, 0.4996667741545416);
			const double a0 = 1.0 + (k / q) + (k * k);
			shelf.b0 = (vh + (vb * k / q) + (k * k)) / a0;
			shelf.b1 = 2.0 * ((k * k) - vh) / a0;
			shelf.b2 = (vh - (vb * k / q) + (k * k)) / a0;
			shelf.a1 = 2.0 * ((k * k) - 1.0) / a0;
			shelf.a2 = (1.0 - (k / q) + (k * k)) / a0;
		}
		Biquad highPass;
		{
			const double frequency = 38.13547087602444;
			const double q = 0.5003270373238773;
			const double k = std::tan(g_pi * frequency / sampleRate);
			const double a0 = 1.0 + (k / q) + (k * k);
			highPass.b0 = 1.0;
			highPass.b1 = -2.0;
			highPass.b2 = 1.0;
			highPass.a1 = 2.0 * ((k * k) - 1.0) / a0;
			highPass.a2 = (1.0 - (k / q) + (k * k)) / a0;
		}
		return {shelf, highPass};
	}

	std::optional<double> integratedLoudness(const std::vector<std::vector<float>>& channels, double sampleRate)
	{
		// A block is four steps of 100 ms; the sum of the K-weighted squares of every channel per step.
		const auto step = static_cast<size_t>(std::lround(0.1 * sampleRate));
		if (channels.empty() || step == 0)
			return std::nullopt;
		size_t frames = channels.front().size();
		for (const std::vector<float>& channel : channels)
			frames = std::min(frames, channel.size());
		const size_t steps = frames / step;

		std::vector<double> stepSquares(steps, 0.0);
		const auto [shelfCoefficients, highPassCoefficients] = kWeighting(sampleRate);
		for (const std::vector<float>& channel : channels)
		{
			BiquadFilter shelf(shelfCoefficients);
			BiquadFilter highPass(highPassCoefficients);
			for (size_t i = 0; i < steps * step; ++i)
			{
				const double y = highPass.process(shelf.process(channel[i]));
				stepSquares[i / step] += y * y;
			}
		}
		return integratedLoudness(stepSquares, static_cast<int64_t>(step));
	}

	void PowerSpectrum::feed(const float* samples, size_t count, size_t stride)
	{
		for (size_t i = 0; i < count; ++i)
		{
			m_pending.push_back(samples[i * stride]);
			if (m_pending.size() == g_fftSize)
			{
				transformWindow(g_fftSize, g_fftSize);
				m_pending.erase(m_pending.begin(), m_pending.begin() + (g_fftSize / 2));
			}
		}
	}

	void PowerSpectrum::transformWindow(size_t length, size_t size)
	{
		static const Fft fullFft(g_fftSize);
		static const std::vector<double> fullWindow = blackmanHarris(g_fftSize);
		const bool full = size == g_fftSize && length == g_fftSize;
		const std::vector<double> window = full ? std::vector<double>() : blackmanHarris(length);
		const std::vector<double>& w = full ? fullWindow : window;
		m_windowPower = 0.0;
		for (const double value : w)
			m_windowPower += value * value;

		std::vector<std::complex<double>> data(size);
		for (size_t i = 0; i < length; ++i)
			data[i] = w[i] * static_cast<double>(m_pending[i]);
		if (full)
			fullFft.transform(data);
		else
			Fft(size).transform(data);
		m_power.resize((size / 2) + 1, 0.0);
		for (size_t k = 0; k < m_power.size(); ++k)
			m_power[k] += std::norm(data[k]) * (k == 0 || k == size / 2 ? 1.0 : 2.0);
		++m_windows;
	}

	PowerBins PowerSpectrum::finish()
	{
		PowerBins bins;
		bins.size = g_fftSize;
		size_t length = g_fftSize;
		if (m_windows == 0) // shorter than a window: one zero-padded window
		{
			length = m_pending.size();
			bins.size = g_minFftSize;
			while (bins.size < length)
				bins.size <<= 1;
			if (length > 0)
				transformWindow(length, bins.size);
		}
		bins.power = m_power;
		bins.power.resize((bins.size / 2) + 1, 0.0);
		if (m_windows == 0)
			return bins;
		const double scale = 1.0 / (static_cast<double>(bins.size) * m_windowPower * static_cast<double>(m_windows));
		for (double& bin : bins.power)
			bin *= scale;
		double sum = 0.0;
		for (const double w : blackmanHarris(length))
			sum += w;
		bins.noiseBandwidth = static_cast<double>(length) * m_windowPower / (sum * sum);
		return bins;
	}

	Spectrum spectrum(const std::vector<float>& samples, double sampleRate, size_t maxPeaks)
	{
		PowerSpectrum power;
		power.feed(samples.data(), samples.size());
		return summarize(power.finish(), sampleRate, maxPeaks);
	}

	SpectrumCurve spectrumCurve(const PowerBins& bins, double sampleRate)
	{
		const std::vector<double>& power = bins.power;
		const double binWidth = sampleRate / static_cast<double>(bins.size);
		const double scale = 2.0 * bins.noiseBandwidth;
		const double halfStep = std::pow(2.0, 1.0 / 48.0);
		SpectrumCurve curve;
		for (int i = 0;; ++i)
		{
			const double frequency = 20.0 * std::pow(2.0, i / 24.0);
			if (frequency > sampleRate / 2.0)
				break;
			const auto low = static_cast<size_t>(std::ceil(frequency / halfStep / binWidth));
			const size_t high =
				std::min(power.size() - 1, static_cast<size_t>(std::floor(frequency * halfStep / binWidth)));
			double level = 0.0;
			if (low <= high)
			{
				level = *std::max_element(power.begin() + static_cast<std::ptrdiff_t>(low),
					power.begin() + static_cast<std::ptrdiff_t>(high) + 1);
			}
			else
			{
				const double at = frequency / binWidth;
				const auto below = std::min(power.size() - 1, static_cast<size_t>(at));
				const size_t above = std::min(power.size() - 1, below + 1);
				const double fraction = at - static_cast<double>(below);
				level = (power[below] * (1.0 - fraction)) + (power[above] * fraction);
			}
			curve.frequencies.push_back(frequency);
			curve.levels.push_back(level > 0.0 ? std::optional<double>(powerDecibels(level * scale)) : std::nullopt);
		}
		return curve;
	}

	Spectrum summarize(const PowerBins& bins, double sampleRate, size_t maxPeaks)
	{
		const std::vector<double>& power = bins.power;
		const size_t size = bins.size;
		const double binWidth = sampleRate / static_cast<double>(size);
		Spectrum result;

		const double nyquist = sampleRate / 2.0;
		for (int octave = -5; std::ldexp(1000.0, octave) <= nyquist; ++octave) // 31.25 Hz, 62.5 Hz, ...
		{
			const double center = std::ldexp(1000.0, octave);
			const double low = center / std::sqrt(2.0);
			const double high = center * std::sqrt(2.0);
			double sum = 0.0;
			for (size_t k = 0; k < power.size(); ++k)
			{
				const double frequency = static_cast<double>(k) * binWidth;
				if (frequency >= low && frequency < high)
					sum += power[k];
			}
			Band band;
			band.frequency = center;
			if (sum > 0.0)
				band.level = powerDecibels(sum);
			result.bands.push_back(band);
		}

		// Local maxima, strongest first. A peak's power is its main lobe's, its frequency between bins from a
		// parabola through the decibels of three bins.
		std::vector<size_t> candidates;
		for (size_t k = 1; k + 1 < power.size(); ++k)
		{
			if (power[k] > 0.0 && power[k] > power[k - 1] && power[k] >= power[k + 1])
				candidates.push_back(k);
		}
		std::sort(candidates.begin(), candidates.end(), [&power](size_t a, size_t b) { return power[a] > power[b]; });
		std::vector<size_t> taken;
		for (const size_t k : candidates)
		{
			if (result.peaks.size() >= maxPeaks ||
				powerDecibels(power[k]) < powerDecibels(power[candidates.front()]) - g_peakRange)
				break;
			const bool near = std::any_of(taken.begin(), taken.end(),
				[k](size_t other) { return (k > other ? k - other : other - k) <= 2 * g_lobe; });
			if (near)
				continue;
			taken.push_back(k);

			const auto decibelsAt = [&power](size_t bin) { return powerDecibels(std::max(power[bin], 1e-300)); };
			const double left = decibelsAt(k - 1);
			const double middle = decibelsAt(k);
			const double right = decibelsAt(k + 1);
			const double curvature = left - (2.0 * middle) + right;
			const double offset = curvature != 0.0 ? 0.5 * (left - right) / curvature : 0.0;
			double lobePower = 0.0;
			for (size_t bin = std::max<size_t>(1, k - std::min(k, g_lobe));
				bin <= std::min(power.size() - 1, k + g_lobe); ++bin)
				lobePower += power[bin];
			result.peaks.push_back({(static_cast<double>(k) + offset) * binWidth, powerDecibels(2.0 * lobePower)});
		}
		return result;
	}
}
