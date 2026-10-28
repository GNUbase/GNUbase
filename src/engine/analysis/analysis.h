// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
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

	// The K-weighting filter of ITU-R BS.1770-4 at a sample rate: a high shelf, then a high pass.
	std::array<Biquad, 2> kWeighting(double sampleRate);

	// The integrated loudness in LUFS (ITU-R BS.1770-4) of channels that play together, each weighted like front left
	// and right: the K-weighted mean square of 400 ms blocks that overlap by 75 %, gated at -70 LUFS and then 10 LU
	// below the loudness of the blocks that passed. Nothing if no block passes, e.g. for silence or audio shorter
	// than 400 ms. The channels have the same length.
	std::optional<double> integratedLoudness(const std::vector<std::vector<float>>& channels, double sampleRate);

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
}
