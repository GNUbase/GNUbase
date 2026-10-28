// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/graph/processor.h"

#include <cstdint>

// Signal generators. They replace their node's input and write the same signal to every channel.
namespace gnubase::engine::processors
{
	class Sine : public graph::Processor
	{
	public:
		enum Parameter : uint8_t
		{
			Frequency, // Hz
			Level,     // linear amplitude
		};

		void process(const graph::ProcessContext& context) noexcept override;

	protected:
		void prepare(double sampleRate, uint32_t maxFrames) override;

	private:
		double m_sampleRate = 48000.0;
		double m_frequency = 440.0;
		double m_level = 0.5;
		double m_phase = 0.0; // in cycles, 0 to 1
	};

	// White noise from a fixed seed, so every instance produces the same samples.
	class Noise : public graph::Processor
	{
	public:
		enum Parameter : uint8_t
		{
			Level, // linear amplitude
		};

		void process(const graph::ProcessContext& context) noexcept override;

	private:
		double m_level = 0.5;
		uint32_t m_state = 0x12345678; // xorshift32
	};

	// One sample at the level, starting with the first sample processed and repeating at the interval.
	class Impulse : public graph::Processor
	{
	public:
		enum Parameter : uint8_t
		{
			Interval, // seconds
			Level,    // linear amplitude
		};

		void process(const graph::ProcessContext& context) noexcept override;

	protected:
		void prepare(double sampleRate, uint32_t maxFrames) override;

	private:
		void updatePeriod();

		double m_sampleRate = 48000.0;
		double m_interval = 1.0;
		double m_level = 1.0;
		uint64_t m_period = 48000; // samples between impulses
		uint64_t m_untilNext = 0;  // samples until the next impulse
	};
}
