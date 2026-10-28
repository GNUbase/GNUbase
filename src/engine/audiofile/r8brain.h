// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>

// The one place that includes r8brain-free-src: r8brain.cpp is compiled with the third-party code, so its headers
// stay out of GNUbase's own files and their warnings and clang-tidy checks.
namespace gnubase::engine::detail
{
	// One channel of r8b::CDSPResampler24.
	class ChannelResampler
	{
	public:
		virtual ~ChannelResampler() = default;

		// Resamples frames input samples; output points to the resampler's own buffer, valid until the next call.
		// Returns the number of output samples.
		virtual int process(double* input, int frames, double*& output) = 0;

		virtual void clear() = 0;

		// Input samples needed before the first output sample.
		virtual int inputLatency() const = 0;
	};

	std::unique_ptr<ChannelResampler> createChannelResampler(double fromRate, double toRate, int maxInputFrames);
}
