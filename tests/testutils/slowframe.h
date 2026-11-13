// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>

#include <chrono>
#include <thread>
#include <utility>

namespace gnubase::test
{
	// A slow frame, as in a slow build on a busy machine: while it exists, the first mouseup that reaches the element
	// (in the capture phase, before the element's own listeners) takes 600 ms, longer than a double click's time.
	// again() makes the next one slow too.
	class SlowFrame final : public Rml::EventListener
	{
	public:
		explicit SlowFrame(Rml::Element& element) : m_element(element)
		{
			m_element.AddEventListener(Rml::EventId::Mouseup, this, true);
		}
		~SlowFrame() override { m_element.RemoveEventListener(Rml::EventId::Mouseup, this, true); }
		SlowFrame(const SlowFrame&) = delete;
		SlowFrame& operator=(const SlowFrame&) = delete;
		SlowFrame(SlowFrame&&) = delete;
		SlowFrame& operator=(SlowFrame&&) = delete;

		void ProcessEvent(Rml::Event& /*event*/) override
		{
			if (std::exchange(m_slow, false))
				std::this_thread::sleep_for(std::chrono::milliseconds(600));
		}

		// Whether the slow mouseup is still to come.
		bool pending() const { return m_slow; }
		void again() { m_slow = true; }

	private:
		Rml::Element& m_element;
		bool m_slow = true;
	};
}
