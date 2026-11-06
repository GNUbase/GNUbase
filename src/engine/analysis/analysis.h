// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// Measurements of audio for tests and AI agents (audio.analyze, audio.compare): levels, loudness and spectra. They
// allocate and take time in proportion to the audio, so they run on the command thread, never on audio threads.
namespace gnubase::engine::analysis
{
	struct Levels
	{
		double peak = 0.0; // the largest absolute sample
		double rms = 0.0;  // the root mean square
	};

	Levels levels(const std::vector<float>& samples);

	// 20 log10(level), i.e. dBFS for sample levels. Nothing for silence.
	std::optional<double> decibels(double level);

	// A second-order IIR filter section: y[n] = b0 x[n] + b1 x[n-1] + b2 x[n-2] - a1 y[n-1] - a2 y[n-2].
	struct Biquad
	{
		double b0 = 1.0;
		double b1 = 0.0;
		double b2 = 0.0;
		double a1 = 0.0;
		double a2 = 0.0;
	};

	// A biquad that filters samples one by one, in transposed direct form II, which keeps rounding low for the
	// K-weighting's low corner frequency.
	class BiquadFilter
	{
	public:
		BiquadFilter() = default;
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

	// The K-weighting filter of ITU-R BS.1770-4 at a sample rate: a high shelf, then a high pass.
	std::array<Biquad, 2> kWeighting(double sampleRate);

	// The loudness in LUFS of a K-weighted mean square, of the channels together. Nothing for silence.
	std::optional<double> loudness(double meanSquare);

	// The integrated loudness in LUFS (ITU-R BS.1770-4) of channels that play together, each weighted like front left
	// and right: the K-weighted mean square of 400 ms blocks that overlap by 75 %, gated at -70 LUFS and then 10 LU
	// below the loudness of the blocks that passed. Nothing if no block passes, e.g. for silence or audio shorter
	// than 400 ms. The channels have the same length.
	std::optional<double> integratedLoudness(const std::vector<std::vector<float>>& channels, double sampleRate);
	// The same of the K-weighted squares of every channel added up per 100 ms step of step frames.
	std::optional<double> integratedLoudness(const std::vector<double>& stepSquares, int64_t step);

	struct SpectralPeak
	{
		double frequency = 0.0; // Hz
		double level = 0.0;     // dBFS of a sine with that amplitude, so a full-scale sine is 0 dBFS
	};

	struct Band
	{
		double frequency = 0.0;      // the octave band's center in Hz: 1000 * 2^n
		std::optional<double> level; // dBFS of the power in the band, like an RMS level; nothing if it has none
	};

	struct Spectrum
	{
		std::vector<SpectralPeak> peaks; // strongest first, at most 80 dB below the strongest
		std::vector<Band> bands;         // from 31.25 Hz up to the Nyquist frequency: up to 16 kHz at 48 kHz
	};

	// A summary of the average power spectrum: the strongest frequencies, at most maxPeaks, and octave band levels.
	// Welch's method with 4-term Blackman-Harris windows of 8192 samples that overlap by half; shorter audio is one
	// zero-padded window. Peaks closer than about 8 bins (47 Hz at 48 kHz) count as one.
	Spectrum spectrum(const std::vector<float>& samples, double sampleRate, size_t maxPeaks);

	// An average power spectrum: the one-sided power per bin of an FFT of size, scaled so that the bins add up to the
	// mean square, and the window's equivalent noise bandwidth in bins.
	struct PowerBins
	{
		std::vector<double> power;
		size_t size = 0;
		double noiseBandwidth = 1.0;
	};

	// The average power spectrum of audio fed to it in blocks, as spectrum measures it.
	class PowerSpectrum
	{
	public:
		// count samples, stride apart: one channel of interleaved frames.
		void feed(const float* samples, size_t count, size_t stride = 1);
		PowerBins finish();

	private:
		void transformWindow(size_t length, size_t size);

		std::vector<float> m_pending; // the samples of the window being filled, the second half of the last first
		std::vector<double> m_power;
		size_t m_windows = 0;
		double m_windowPower = 0.0;
	};

	// spectrum's summary of an average power spectrum.
	Spectrum summarize(const PowerBins& bins, double sampleRate, size_t maxPeaks);

	struct SpectrumCurve
	{
		std::vector<double> frequencies;           // Hz, from 20 Hz up to the Nyquist frequency, 1/24 octave apart
		std::vector<std::optional<double>> levels; // dBFS; nothing for silence
	};

	// An average power spectrum to draw: at each frequency the level of the strongest bin within 1/48 octave of it, or
	// between the bins next to it where they are farther apart, scaled so that a sine reads its peak level.
	SpectrumCurve spectrumCurve(const PowerBins& bins, double sampleRate);
}
