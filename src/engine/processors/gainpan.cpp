// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/processors/gainpan.h"

#include "engine/processors/builtin.h"

#include <algorithm>
#include <cmath>

namespace gnubase::engine::processors
{
	void GainPan::update()
	{
		const double gain = std::pow(10.0, m_gain / 20.0);
		const double pan = std::clamp(m_pan, -1.0, 1.0);
		m_left = static_cast<float>(gain * (pan > 0.0 ? 1.0 - pan : 1.0));
		m_right = static_cast<float>(gain * (pan < 0.0 ? 1.0 + pan : 1.0));
		m_other = static_cast<float>(gain);
	}

	void GainPan::process(const graph::ProcessContext& context) noexcept
	{
		splitAtEvents(
			context,
			[this](const graph::ProcessEvent& event)
			{
				if (event.type != graph::ProcessEvent::Type::Parameter)
					return;
				if (event.parameter == Gain)
					m_gain = event.parameterValue;
				else if (event.parameter == Pan)
					m_pan = event.parameterValue;
				else
					return;
				update();
			},
			[this, &context](uint32_t begin, uint32_t end)
			{
				const bool stereo = context.channels == 2;
				for (int c = 0; c < context.channels; ++c)
				{
					float factor = m_other;
					if (stereo)
						factor = c == 0 ? m_left : m_right;
					float* samples = context.audio[c];
					for (uint32_t i = begin; i < end; ++i)
						samples[i] *= factor;
				}
			});
	}
}
