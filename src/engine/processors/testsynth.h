// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/graph/processor.h"

#include <array>
#include <cstdint>

namespace gnubase::engine::processors
{
	inline constexpr int g_testSynthVoices = 16;

	// A simple polyphonic sine synth for tests: velocity sets each voice's amplitude, attack and release are linear
	// ramps. When all voices are busy, a new note takes the oldest one. A note-on with velocity 0 is a note-off.
	class TestSynth : public graph::Processor
	{
	public:
		enum Parameter : uint32_t
		{
			Level,   // linear amplitude of a note at full velocity
			Attack,  // seconds
			Release, // seconds
		};

		void process(const graph::ProcessContext& context) noexcept override;

		// Voices that are sounding, including releasing ones.
		int activeVoices() const;

	protected:
		void prepare(double sampleRate, uint32_t maxFrames) override;

	private:
		struct Voice
		{
			bool active = false;
			bool released = false;
			uint8_t pitch = 0;
			uint8_t channel = 0;
			uint64_t started = 0; // note-on count when it started, to find the oldest voice
			double phase = 0.0;   // in cycles, 0 to 1
			double increment = 0.0;
			double amplitude = 0.0;
			double envelope = 0.0;  // 0 to 1
			double step = 0.0;      // envelope change per sample: rising in the attack, falling in the release
			uint64_t remaining = 0; // samples left in the release
		};

		void noteOn(uint8_t channel, uint8_t pitch, uint16_t velocity) noexcept;
		void noteOff(uint8_t channel, uint8_t pitch) noexcept;
		void render(const graph::ProcessContext& context, uint32_t begin, uint32_t end) noexcept;

		double m_sampleRate = 48000.0;
		double m_level = 0.25;
		double m_attack = 0.005;
		double m_release = 0.05;
		uint64_t m_notes = 0;
		std::array<Voice, g_testSynthVoices> m_voices{};
	};
}
