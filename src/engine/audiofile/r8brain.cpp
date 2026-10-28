// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audiofile/r8brain.h"

#include <CDSPResampler.h>

namespace gnubase::engine::detail
{
	namespace
	{
		class R8brainResampler final : public ChannelResampler
		{
		public:
			R8brainResampler(double fromRate, double toRate, int maxInputFrames)
				: m_resampler(fromRate, toRate, maxInputFrames)
			{
			}

			int process(double* input, int frames, double*& output) override
			{
				return m_resampler.process(input, frames, output);
			}

			void clear() override { m_resampler.clear(); }

			int inputLatency() const override { return m_resampler.getInLenBeforeOutPos(0); }

		private:
			r8b::CDSPResampler24 m_resampler;
		};
	}

	std::unique_ptr<ChannelResampler> createChannelResampler(double fromRate, double toRate, int maxInputFrames)
	{
		return std::make_unique<R8brainResampler>(fromRate, toRate, maxInputFrames);
	}
}
