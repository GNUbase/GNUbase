// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/graph/processor.h"

#include <cstdint>

namespace gnubase::engine::processors
{
	// Gain in dB and, on stereo nodes, balance: panning to one side lowers the other side and leaves this one alone,
	// so the center is unity gain. Other channel counts get the gain only.
	class GainPan : public graph::Processor
	{
	public:
		enum Parameter : uint32_t
		{
			Gain, // dB
			Pan,  // -1 left, 0 center, 1 right
		};

		void process(const graph::ProcessContext& context) noexcept override;

	private:
		void update();

		double m_gain = 0.0;
		double m_pan = 0.0;
		float m_left = 1.0f; // linear factors, from gain and pan
		float m_right = 1.0f;
		float m_other = 1.0f; // for nodes that are not stereo
	};
}
