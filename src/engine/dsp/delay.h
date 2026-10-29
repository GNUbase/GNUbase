// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/dsp/dsp.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace gnubase::engine::dsp
{
	// A stereo delay with feedback: the time in milliseconds or as a note value at the transport's tempo. Ping-pong
	// sends the sum of both inputs to the left delay, whose output feeds the right one and back, so the echoes
	// alternate between the sides. Mono nodes get a plain delay; channels after the second pass unchanged.
	//
	// A new time, from automation or a tempo change, doesn't jump: the delay glides there like a tape machine, reading
	// between samples with linear interpolation while it moves, and exactly on the samples once it is there. Feedback
	// and mix glide too. Above 0 dB the loop saturates softly towards g_loopLimit, so full feedback holds the echoes
	// forever but a sustained input can't make them grow without bound.
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
		// The highest level in the feedback loop (+6 dB): it is linear up to 1, then saturates towards this.
		static constexpr float g_loopLimit = 2.0f;
		// How long the time takes to glide to a new value: 1 - 1/e of the way.
		static constexpr double g_glideSeconds = 0.05;

		struct NoteValue
		{
			const char* name;
			double quarters;
		};

		// The note values for Sync: from a 1/32 triplet to a whole note; straight, triplet and dotted values.
		static const std::array<NoteValue, 16>& noteValues();
		static constexpr int g_defaultNote = 10; // 1/4

		Delay() { update(); }

		void prepare(double sampleRate, uint32_t maxFrames);
		void reset() noexcept;
		void setParameter(uint32_t id, double value) noexcept;
		// Quarter notes per minute, for Sync.
		void setTempo(double bpm) noexcept;
		void process(float* const* audio, int channels, uint32_t begin, uint32_t end) noexcept;

		// The delay in samples it is at or gliding to.
		uint32_t delayFrames() const { return m_frames; }
		// How long the echoes last after the input went quiet, until they are 90 dB down; UINT32_MAX for endless.
		// [any thread] Computed when the parameters change.
		uint32_t tail() const { return m_tail.load(std::memory_order_relaxed); }

	private:
		void update() noexcept;
		// The line's sample `delay` samples before the one about to be written, between samples interpolated.
		float read(const float* line, double delay) const noexcept;

		double m_sampleRate = 48000.0;
		double m_timeMs = 500.0;
		bool m_sync = false;
		int m_note = g_defaultNote;
		double m_feedback = 0.3;
		double m_mix = 0.3;
		bool m_pingPong = false;
		double m_tempo = 120.0;

		uint32_t m_frames = 1;           // the delay, whole samples
		Smoothed m_delay{1.0, 1e-3};     // where it is now, in samples, gliding to m_frames
		Smoothed m_feedbackGain{0.3};    // gliding to m_feedback
		Smoothed m_wet{0.3};             // gliding to m_mix
		std::atomic<uint32_t> m_tail{0}; // read by tail() on any thread
		bool m_fresh = true;             // nothing processed since the last reset: parameters apply at once
		size_t m_length = 0;             // of each line: the longest delay plus one
		std::array<std::vector<float>, 2> m_lines;
		size_t m_write = 0;
	};
}
