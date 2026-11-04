// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"
#include "engine/dsp/dsp.h"
#include "engine/dsp/dynamics.h"
#include "engine/dsp/equalizer.h"

#include <cstdint>

namespace gnubase::engine
{
	// The channel strip's sections before the fader (#107, docs/features/channel-strip.md), which a channel keeps
	// across graph rebuilds: the pre section (gain and phase, then the low and the high cut) before the plugin slots,
	// and after them the gate, the compressor, the equalizer and the limiter, on the DSP cores of the built-in
	// effects (#114). Each graph's channel strip hands it the channel's settings; it applies them on the audio thread,
	// where the cores glide to them. Sections that are off cost nothing and add no latency.
	class StripSections
	{
	public:
		StripSections();

		// Off the audio thread: the first time and when the sample rate or the block size changes, which only
		// happens while no graph processes.
		void prepare(double sampleRate, uint32_t maxFrames);

		// Audio thread: the sections before the plugin slots, and those after them.
		void processPre(const ChannelSections& settings, float* const* audio, int channels, uint32_t frames) noexcept;
		void processPost(const ChannelSections& settings, float* const* audio, int channels, uint32_t frames) noexcept;

		// The delay processPost adds with these settings: the limiter's look-ahead while it is on. After prepare.
		uint32_t latency(const ChannelSections& settings) const;

	private:
		// An equalizer that runs while a band is on and for a while after the last one went off, so it glides out.
		struct Filters
		{
			dsp::Equalizer core;
			uint32_t tail = 0; // frames it still runs for with every band off
			bool on = false;

			void process(float* const* audio, int channels, uint32_t frames, uint32_t tailFrames) noexcept;
		};

		void apply(const ChannelSections& settings) noexcept;

		double m_sampleRate = 0.0;
		uint32_t m_maxFrames = 0;
		uint32_t m_tailFrames = 0;
		dsp::Smoothed m_gain{1.0}; // the pre gain, negative with the phase inverted: it glides through zero
		Filters m_cuts;            // bands 0 and 5: the low and the high cut
		Filters m_eq;              // bands 1 to 4
		dsp::Gate m_gate;
		dsp::Compressor m_compressor;
		dsp::Limiter m_limiter;
		ChannelSections m_applied;
		bool m_fresh = true; // nothing applied yet: the settings apply at once
	};
}
