// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/dsp/dsp.h"

#include <array>
#include <cstdint>
#include <vector>

namespace gnubase::engine::dsp
{
	// A stereo delay with feedback: the time in milliseconds or as a note value at the transport's tempo. Ping-pong
	// sends the sum of both inputs to the left delay, whose output feeds the right one and back, so the echoes
	// alternate between the sides. Mono nodes get a plain delay; channels after the second pass unchanged.
	class Delay
	{
	public:
		enum Parameter : uint8_t
		{
			Time,     // ms, 1 to 5000
			Sync,     // 0 off, 1 on: the time is the note value at the tempo
			Note,     // index into noteValues()
			Feedback, // %, 0 to 100
			Mix,      // %, 0 to 100: 0 dry only, 100 echoes only
			PingPong, // 0 off, 1 on
		};

		static constexpr double g_maxSeconds = 5.0;

		struct NoteValue
		{
			const char* name;
			double quarters;
		};

		// The note values for Sync: from a 1/32 triplet to a whole note; straight, triplet and dotted values.
		static const std::array<NoteValue, 16>& noteValues();
		static constexpr int g_defaultNote = 10; // 1/4

		void prepare(double sampleRate, uint32_t maxFrames);
		void reset() noexcept;
		void setParameter(uint32_t id, double value) noexcept;
		// Quarter notes per minute, for Sync.
		void setTempo(double bpm) noexcept;
		void process(float* const* audio, int channels, uint32_t begin, uint32_t end) noexcept;

		// The delay in samples.
		uint32_t delayFrames() const { return m_frames; }
		// How long the echoes last after the input went quiet, until they are 90 dB down; UINT32_MAX for endless.
		uint32_t tail() const;

	private:
		void update() noexcept;

		double m_sampleRate = 48000.0;
		double m_timeMs = 500.0;
		bool m_sync = false;
		int m_note = g_defaultNote;
		double m_feedback = 0.3;
		double m_mix = 0.3;
		bool m_pingPong = false;
		double m_tempo = 120.0;

		uint32_t m_frames = 1; // the delay
		size_t m_length = 0;   // of each line: the longest delay plus one
		std::array<std::vector<float>, 2> m_lines;
		size_t m_write = 0;
	};
}
