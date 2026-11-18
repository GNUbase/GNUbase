// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"
#include "engine/dsp/dsp.h"
#include "engine/dsp/dynamics.h"
#include "engine/dsp/equalizer.h"
#include "engine/graph/events.h"

#include <cstdint>

namespace gnubase::engine
{
	// Parameter events for the strip's sections have this plus their StripParameter id as their parameter. They
	// travel with the fader's events, which ignores them.
	inline constexpr uint32_t g_stripEvents = 256;

	// A value that glides in a straight line over the frames a parameter event's ramp gives, frame by frame.
	struct ParameterRamp
	{
		double value = 0.0; // the value of the next frame
		double start = 0.0;
		double target = 0.0;
		uint32_t length = 0; // frames of the ramp, 0 while it doesn't move
		uint32_t done = 0;

		void set(double newValue, uint32_t ramp) noexcept; // ramp 0 jumps
		double next() noexcept;                            // the value for this frame; moves on by one frame
	};

	// The channel strip's sections before the fader (#107, docs/features/channel-strip.md), which a channel keeps
	// across graph rebuilds: the pre section (gain and phase, then the low and the high cut) before the plugin slots,
	// and after them the gate, the compressor, the equalizer and the limiter, on the DSP cores of the built-in
	// effects (#114). Their settings arrive as parameter events (g_stripEvents), the channel's static values and its
	// automation (#658), which apply at their samples: the pre gain glides along their ramps, the other values change
	// where the events are and the cores glide to them. Sections that are off cost nothing and add no latency.
	class StripSections
	{
	public:
		StripSections();

		// Off the audio thread: the first time and when the sample rate or the block size changes, which only
		// happens while no graph processes.
		void prepare(double sampleRate, uint32_t maxFrames);

		// Audio thread: the sections before the plugin slots, and those after them, with the block's events, sorted
		// by offset; each takes the events of its own parameters. The pre gain's value is a linear gain. lookAhead:
		// the limiter's look-ahead counts in the channel's latency (see latency), so the limiter runs only while it
		// is set, bypassed while it is off: it never delays a channel by more than the latency the graph reports.
		void processPre(const graph::EventBuffer& events, float* const* audio, int channels, uint32_t frames) noexcept;
		void processPost(const graph::EventBuffer& events, bool lookAhead, float* const* audio, int channels,
			uint32_t frames) noexcept;

		// The delay processPost adds while the limiter's look-ahead counts (processPost's lookAhead). After prepare.
		uint32_t latency(bool limiter) const { return limiter ? m_limiter.latency() : 0; }

	private:
		// An equalizer that runs while a band is on and for a while after the last one went off, so it glides out.
		struct Filters
		{
			dsp::Equalizer core;
			uint32_t tail = 0; // frames it still runs for with every band off
			bool on = false;

			void process(float* const* audio, int channels, uint32_t begin, uint32_t end, uint32_t tailFrames) noexcept;
		};

		// Plays the events of the pre section's parameters or of the others, rendering the frames between them.
		template <typename Render>
		void play(const graph::EventBuffer& events, bool pre, uint32_t frames, const Render& render) noexcept;
		void applyPre() noexcept;
		void applyPost() noexcept;

		double m_sampleRate = 0.0;
		uint32_t m_maxFrames = 0;
		uint32_t m_tailFrames = 0;
		ChannelSections m_values;      // what the events set; the pre gain is in m_gain
		ParameterRamp m_gain;          // the pre gain, linear
		dsp::Smoothed m_polarity{1.0}; // -1 with the phase inverted: it glides through zero
		Filters m_cuts;                // bands 0 and 5: the low and the high cut
		Filters m_eq;                  // bands 1 to 4
		dsp::Gate m_gate;
		dsp::Compressor m_compressor;
		dsp::Limiter m_limiter;
		bool m_limiterRuns = false;
		ChannelSections m_appliedPre; // the values the cores have
		ChannelSections m_appliedPost;
		bool m_freshPre = true; // nothing applied yet: the values apply at once
		bool m_freshPost = true;
	};
}
