// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/dsp/dsp.h"

#include <cstdint>
#include <vector>

// Dynamics processors. Their channels are linked: one detector follows the loudest channel, and every channel gets
// the same gain, so the stereo image stays where it is.
namespace gnubase::engine::dsp
{
	// Lets signals above the threshold through and attenuates the rest by the range. Hold keeps the gate open for a
	// while after the level fell below the threshold, which also bridges the zero crossings of low notes.
	class Gate
	{
	public:
		enum Parameter : uint8_t
		{
			Threshold, // dB, -80 to 0
			Range,     // dB, -80 to 0: the gain while closed
			Attack,    // ms, 0.01 to 100: opening
			Hold,      // ms, 0 to 2000
			Release,   // ms, 1 to 4000: closing
		};

		void prepare(double sampleRate, uint32_t maxFrames);
		void reset() noexcept;
		void setParameter(uint32_t id, double value) noexcept;
		void process(float* const* audio, int channels, uint32_t begin, uint32_t end) noexcept;

		// The gain applied to the last sample, linear: for tests and meters.
		double gain() const { return m_gain; }

	private:
		void update() noexcept;

		double m_sampleRate = 48000.0;
		double m_thresholdDb = -40.0;
		double m_rangeDb = -80.0;
		double m_attackMs = 0.1;
		double m_holdMs = 20.0;
		double m_releaseMs = 100.0;

		double m_threshold = 0.01; // linear
		double m_range = 1e-4;     // linear
		double m_attack = 0.0;     // smoothing coefficients
		double m_release = 0.0;
		uint64_t m_holdFrames = 0;

		double m_gain = 0.0; // starts closed
		uint64_t m_holding = 0;
	};

	// A feed-forward compressor with a soft knee. The gain computer is the one of Giannoulis, Massberg and Reiss,
	// "Digital Dynamic Range Compressor Design" (2012): below the knee nothing happens, above it the level rises by
	// 1/ratio, and within it a quadratic joins the two. Attack and release smooth the gain reduction in dB.
	class Compressor
	{
	public:
		enum Parameter : uint8_t
		{
			Threshold, // dB, -60 to 0
			Ratio,     // 1 to 20
			Knee,      // dB, 0 to 24: the width of the soft knee
			Attack,    // ms, 0.1 to 100
			Release,   // ms, 10 to 2000
			Makeup,    // dB, 0 to 24
			Detection, // 0 peak, 1 RMS (averaged over 10 ms)
		};

		enum class DetectionMode : uint8_t
		{
			Peak,
			Rms,
		};

		// The static curve: the output level for an input level, both in dB, without makeup gain.
		static double curve(double inputDb, double thresholdDb, double ratio, double kneeDb);

		void prepare(double sampleRate, uint32_t maxFrames);
		void reset() noexcept;
		void setParameter(uint32_t id, double value) noexcept;
		void process(float* const* audio, int channels, uint32_t begin, uint32_t end) noexcept;

		// The gain reduction applied to the last sample, in dB (0 or less): for tests and meters.
		double gainReduction() const { return m_reduction; }

	private:
		void update() noexcept;

		double m_sampleRate = 48000.0;
		double m_thresholdDb = -20.0;
		double m_ratio = 4.0;
		double m_kneeDb = 6.0;
		double m_attackMs = 10.0;
		double m_releaseMs = 100.0;
		double m_makeupDb = 0.0;
		DetectionMode m_detection = DetectionMode::Peak;

		double m_attack = 0.0;
		double m_release = 0.0;
		double m_rmsCoefficient = 0.0;

		double m_meanSquare = 0.0;
		double m_reduction = 0.0; // dB
	};

	// A look-ahead peak limiter: no sample leaves it above the ceiling. It delays the signal by 1 ms and lowers the
	// gain over that time before a peak arrives, so there is no overshoot and no clipping: the gain is the minimum of
	// the required gains over the look-ahead window, released smoothly, and then averaged over the window, which keeps
	// it at or below every required gain within reach. A final clamp catches the last rounding error.
	class Limiter
	{
	public:
		enum Parameter : uint8_t
		{
			Input,   // dB, 0 to 24: gain before limiting
			Ceiling, // dB, -30 to 0: the highest output level
			Release, // ms, 1 to 1000
		};

		static constexpr double g_lookaheadSeconds = 0.001;

		void prepare(double sampleRate, uint32_t maxFrames);
		void reset() noexcept;
		void setParameter(uint32_t id, double value) noexcept;
		void process(float* const* audio, int channels, uint32_t begin, uint32_t end) noexcept;

		// The delay in samples, from prepare on.
		uint32_t latency() const { return m_lookahead; }

	private:
		void update() noexcept;

		double m_sampleRate = 48000.0;
		double m_inputDb = 0.0;
		double m_ceilingDb = -0.3;
		double m_releaseMs = 50.0;
		double m_input = 1.0; // linear
		double m_ceiling = 1.0;
		double m_release = 0.0;

		uint32_t m_lookahead = 0; // L samples
		uint32_t m_window = 1;    // L + 1
		uint32_t m_position = 0;  // where the delay lines are read and written next

		std::vector<float> m_delay;    // g_maxChannels lines of L samples
		std::vector<double> m_average; // the last L + 1 released gains, at frame % (L + 1)
		double m_sum = 0.0;            // their sum
		// The sliding minimum of the required gains over L + 1 samples: a ring of candidates, increasing from front
		// to back, each with the sample index it leaves the window at.
		std::vector<double> m_minValues;
		std::vector<uint64_t> m_minExpiry;
		uint32_t m_minFront = 0;
		uint32_t m_minSize = 0;
		uint64_t m_frame = 0;
		double m_released = 1.0;
	};
}
