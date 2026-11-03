// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/analysis/analysis.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <utility>

// Level meters and the loudness meter (reference manual pp. 533-540, docs/features/metering.md). The audio thread
// measures, without allocating or locking, and publishes through atomics every 100 ms; any thread reads.
namespace gnubase::engine
{
	inline constexpr int g_maxMeterChannels = 8;

	// A meter's reading per channel, in linear sample values: 0 for silence.
	struct LevelReading
	{
		int channels = 0;
		std::array<float, g_maxMeterChannels> peak{};    // the largest sample of the last 100 ms
		std::array<float, g_maxMeterChannels> rms{};     // over the last 300 ms
		std::array<float, g_maxMeterChannels> peakMax{}; // since the last reset
		std::array<float, g_maxMeterChannels> rmsMax{};
	};

	// The peak and RMS levels of a channel strip's output or the main mix.
	class LevelMeter
	{
	public:
		// Audio thread: measures a block of up to g_maxMeterChannels channels at the sample rate.
		void process(const float* const* audio, int channels, uint32_t frames, double sampleRate) noexcept;

		// Any thread.
		LevelReading read() const noexcept;
		void reset() noexcept; // the maxima

	private:
		// Audio thread.
		// The sums of squares of the last three 100 ms windows, a ring, and of the current one.
		std::array<std::array<double, 3>, g_maxMeterChannels> m_squares{};
		std::array<double, g_maxMeterChannels> m_windowSquares{};
		int m_squaresIndex = 0;
		uint32_t m_squaresFrames = 0; // in the three windows
		std::array<uint32_t, 3> m_windowLengths{};
		std::array<float, g_maxMeterChannels> m_windowPeak{};
		std::array<float, g_maxMeterChannels> m_peakMax{};
		std::array<float, g_maxMeterChannels> m_rmsMax{};
		uint32_t m_windowFrames = 0;
		uint32_t m_resetsSeen = 0;

		std::atomic<int> m_channels{0};
		std::array<std::atomic<float>, g_maxMeterChannels> m_peak{};
		std::array<std::atomic<float>, g_maxMeterChannels> m_rms{};
		std::array<std::atomic<float>, g_maxMeterChannels> m_publishedPeakMax{};
		std::array<std::atomic<float>, g_maxMeterChannels> m_publishedRmsMax{};
		std::atomic<uint32_t> m_resets{0};
	};

	// The loudness meter's reading (EBU R 128, ITU-R BS.1770-4, EBU Tech 3341 and 3342). Loudness in LUFS, nothing
	// until there is a value or for silence; true peak as a linear level.
	struct LoudnessReading
	{
		std::optional<double> momentary;    // the last 400 ms
		std::optional<double> shortTerm;    // the last 3 s
		std::optional<double> momentaryMax; // the largest momentary loudness while measuring
		std::optional<double> shortTermMax;
		std::optional<double> integrated; // gated, while measuring
		std::optional<double> range;      // the loudness range in LU, while measuring
		int channels = 0;
		std::array<float, g_maxMeterChannels> truePeak{};    // the last 100 ms, 4x oversampled
		std::array<float, g_maxMeterChannels> truePeakMax{}; // while measuring
		double seconds = 0.0;                                // how long it measured
	};

	// The loudness and true peak of the main mix. Momentary and short-term loudness and the true peak always follow
	// the signal; the integrated loudness, the loudness range, the maxima and the time accumulate only while the
	// transport plays.
	class LoudnessMeter
	{
	public:
		LoudnessMeter();

		// Not while the audio thread processes: the sample rate. Starts over.
		void prepare(double sampleRate);

		// Audio thread: a block of the main mix's channels.
		void process(const float* const* audio, int channels, uint32_t frames, bool measuring) noexcept;

		// Any thread.
		LoudnessReading read() const noexcept;
		void reset() noexcept; // everything but momentary and short-term loudness

	private:
		// Gated loudness of blocks: the energy and count of the blocks per 0.1 LU from -70 LUFS up.
		struct Histogram
		{
			static constexpr int g_bins = 800;
			std::array<double, g_bins> energy{};
			std::array<uint64_t, g_bins> count{};

			void add(double meanSquare) noexcept;
			void clear() noexcept;
			// The mean square of the blocks louder than a loudness, and how many there are.
			std::pair<double, uint64_t> above(double loudness) const noexcept;
		};

		static constexpr int g_taps = 96;  // of the true peak interpolator, g_taps / 4 per phase
		static constexpr int g_steps = 30; // 100 ms steps in the short-term window

		void step(bool measuring) noexcept;
		void clear() noexcept;
		void publish() noexcept;

		// Audio thread.
		std::array<analysis::Biquad, 2> m_filters;
		std::array<std::array<double, 4>, g_maxMeterChannels> m_filterState{}; // per channel: z1, z2 of each
		std::array<double, g_taps> m_interpolator{};
		// Per channel the last g_taps / 4 input samples, twice in a row, so they read without wrapping.
		std::array<std::array<float, g_taps / 2>, g_maxMeterChannels> m_history{};
		int m_historyIndex = 0;
		uint32_t m_stepLength = 4800;
		uint32_t m_stepFrames = 0;
		double m_stepEnergy = 0.0;
		std::array<double, g_steps> m_steps{}; // the mean squares of the last steps, a ring
		int m_stepCount = 0;                   // how many steps are in the ring, up to g_steps
		int m_stepIndex = 0;                   // where the next one goes
		Histogram m_blocks;                    // 400 ms blocks every 100 ms, for the integrated loudness
		Histogram m_shortTerms;                // 3 s blocks every second, for the loudness range
		std::optional<double> m_momentaryMax;
		std::optional<double> m_shortTermMax;
		std::array<float, g_maxMeterChannels> m_windowTruePeak{};
		std::array<float, g_maxMeterChannels> m_truePeakMax{};
		uint64_t m_measuredSteps = 0; // since the last reset
		uint64_t m_playingSteps = 0;  // since playback last started
		uint32_t m_resetsSeen = 0;
		int m_channels = 0;

		// Published; NaN for nothing.
		std::atomic<double> m_momentary;
		std::atomic<double> m_shortTerm;
		std::atomic<double> m_publishedMomentaryMax;
		std::atomic<double> m_publishedShortTermMax;
		std::atomic<double> m_integrated;
		std::atomic<double> m_range;
		std::atomic<double> m_seconds{0.0};
		std::atomic<int> m_publishedChannels{0};
		std::array<std::atomic<float>, g_maxMeterChannels> m_truePeak{};
		std::array<std::atomic<float>, g_maxMeterChannels> m_publishedTruePeakMax{};
		std::atomic<uint32_t> m_resets{0};
	};
}
