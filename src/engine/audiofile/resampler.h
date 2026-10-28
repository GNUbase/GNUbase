// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/audiofile/audiofile.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gnubase::engine
{
	// Converts interleaved float audio between sample rates with the 24-bit quality of r8brain-free-src
	// (r8b::CDSPResampler24). The resampler's delay is compensated: output frame n lies at n / toRate seconds, input
	// frame 0 at 0 seconds. Construction allocates, so audio threads don't create resamplers; the disk threads use
	// them.
	class Resampler
	{
	public:
		// maxInputFrames: the most frames one process call gets.
		Resampler(double fromRate, double toRate, int channels, size_t maxInputFrames);
		~Resampler();
		Resampler(const Resampler&) = delete;
		Resampler& operator=(const Resampler&) = delete;

		// Converts frames input frames and appends the output frames to output.
		void process(const float* input, size_t frames, std::vector<float>& output);

		// After the last input: appends the remaining output, up to the length the input has at toRate.
		void finish(std::vector<float>& output);

		// Starts over as after construction.
		void reset();

		// How many input frames the resampler needs before it produces the first output frame; reading must start
		// this far before a position for the output there to be the same as when playing from the start.
		size_t inputLatency() const;

		// The length of inputFrames at another sample rate.
		static int64_t outputLength(int64_t inputFrames, double fromRate, double toRate);

	private:
		struct Channels;

		// Runs input through every channel's resampler and appends the output; returns the frames appended.
		size_t run(const float* input, size_t frames, std::vector<float>& output);

		std::unique_ptr<Channels> m_channels; // empty when the rates are equal
		double m_fromRate;
		double m_toRate;
		int m_channelCount;
		size_t m_maxInputFrames;
		std::vector<double> m_input; // one channel of the current input
		int64_t m_inputFrames = 0;
		int64_t m_outputFrames = 0;
	};

	// Writes the audio file at input to output as a 32-bit float WAV file at sampleRate and returns what was
	// written. Throws AudioFileError.
	AudioFileInfo convertSampleRate(const std::string& input, const std::string& output, double sampleRate);
}
