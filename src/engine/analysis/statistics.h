// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/analysis/analysis.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// The audio functions (#147, docs/features/audio-functions.md): detecting silence and the statistics of audio. They
// allocate and take time in proportion to the audio, so they run on the command thread, never on audio threads.
namespace gnubase::engine::analysis
{
	struct SilenceSettings
	{
		double openThreshold = -20.0;  // dBFS a sample must reach to open
		double closeThreshold = -30.0; // dBFS that keeps it open, at most openThreshold
		double minimumOpen = 0.010;    // seconds a section stays open at least
		double minimumClosed = 0.100;  // seconds it stays closed at least after a section
		double preRoll = 0.0;          // seconds a section starts earlier
		double postRoll = 0.0;         // seconds it ends later
	};

	struct Section
	{
		int64_t start = 0; // the first frame
		int64_t end = 0;   // after the last one
	};

	// Finds the sections that aren't silent in audio fed to it in blocks of interleaved frames: a section opens at the
	// first frame where a sample of any channel reaches the open threshold and closes after the last frame whose
	// sample reaches the close threshold before 20 ms below it (so that a tone's zero crossings don't close it). Once
	// open, a section stays open for at least the minimum time open, joining what sounds within it; once closed,
	// nothing opens before the minimum time closed has passed. Pre-roll and post-roll widen the sections, which merge
	// where they then overlap.
	class SilenceDetector
	{
	public:
		SilenceDetector(double sampleRate, int channels, const SilenceSettings& settings);

		void feed(const float* frames, size_t count);
		// The sections of everything fed, by start, within it.
		std::vector<Section> finish();

	private:
		void close(int64_t limit); // ends the open section, at most at limit

		double m_sampleRate = 0.0;
		int m_channels = 1;
		SilenceSettings m_settings;
		double m_open = 0.0; // the thresholds, linear
		double m_close = 0.0;
		int64_t m_hold = 0; // the frames below the close threshold that close a section
		int64_t m_frame = 0;
		int64_t m_start = -1;      // where the open section started, or -1
		int64_t m_last = 0;        // the open section's last frame at the close threshold
		int64_t m_minimumOpen = 0; // the minimum times, in frames
		int64_t m_minimumClosed = 0;
		int64_t m_reopen = 0; // the first frame that may open a section again
		std::vector<Section> m_sections;
	};

	struct ChannelStatistics
	{
		double minSample = 0.0;      // the lowest sample
		double maxSample = 0.0;      // the highest sample
		double peak = 0.0;           // the largest absolute sample
		double dcOffset = 0.0;       // the mean of the samples
		int bitDepth = 0;            // the bits the samples need, 32 for floating point, 0 for silence
		double averageRms = 0.0;     // the RMS of every sample
		double maxRms = 0.0;         // the highest RMS of 50 ms windows, 10 ms apart; averageRms if shorter
		std::optional<double> pitch; // Hz, the strongest frequency
	};

	// The statistics of each channel of audio fed to it in blocks of interleaved frames (the Statistics window,
	// reference manual p. 609); the loudness meter (src/engine/audio/meters.h) measures loudness and true peak.
	class StatisticsMeter
	{
	public:
		StatisticsMeter(double sampleRate, int channels);

		void feed(const float* frames, size_t count);
		std::vector<ChannelStatistics> finish();

	private:
		struct Channel
		{
			ChannelStatistics statistics;
			double sum = 0.0;
			double squares = 0.0;
			std::array<double, 5> parts{}; // the squares of the last five 10 ms parts
			double part = 0.0;             // the squares of the 10 ms part so far
			PowerSpectrum spectrum;
		};

		double m_sampleRate = 0.0;
		std::vector<Channel> m_channels;
		int64_t m_frames = 0;
		int64_t m_part = 0; // frames in a 10 ms part
	};
}
