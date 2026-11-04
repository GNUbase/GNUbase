// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

// Where sounds start in audio (#143): the transient detection behind hitpoints (docs/features/hitpoints.md). It
// allocates and takes time in proportion to the audio, so it runs on the command thread, never on audio threads.
namespace gnubase::engine::analysis
{
	struct Onset
	{
		int64_t frame = 0; // where the sound starts
		double peak = 0.0; // dBFS: its largest sample in the 20 ms from there
		double rise = 0.0; // dB: how much its level rose within 5 ms, the most of either band
	};

	// Finds onsets in audio fed to it in blocks of interleaved frames. The peak level across channels is followed by
	// two envelopes, one of the samples and one of their differences (the high frequencies, so that a hi-hat stands
	// out over the decay of a kick drum), which jump up with the signal and fall by 50 ms time constants. Where either
	// rises by 6 dB or more within 5 ms, from at least -60 dBFS, a sound starts: at the first frame there that is
	// more than 1 dB above where the envelope was before. Another one can start 20 ms later.
	class OnsetDetector
	{
	public:
		OnsetDetector(double sampleRate, int channels);

		void feed(const float* frames, size_t count);
		// The onsets of everything fed, by frame.
		std::vector<Onset> finish();

	private:
		struct Band
		{
			double envelope = 0.0;
			std::vector<double> history; // the envelope of the last m_delay + 1 frames, by frame modulo its size
		};

		int m_channels = 1;
		double m_release = 0.0;    // the envelopes' factor per frame
		int64_t m_delay = 0;       // the frames a rise is measured over
		int64_t m_window = 0;      // the frames an onset's peak and rise are measured over
		std::vector<float> m_last; // each channel's sample before
		std::array<Band, 2> m_bands;
		int64_t m_frame = 0;
		int64_t m_open = -1;         // until this frame, the last onset is still measured
		std::vector<Onset> m_onsets; // peak still linear
	};
}
