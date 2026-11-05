// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/timeformat.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace Rml
{
	class Element;
	class ElementFormControl;
}

// The transpose track in the UI (#168, reference manual pp. 165, 418): the transpose events on the transpose track's
// lane in the event display. The events are the engine's (transpose.*); docs/features/transpose.md describes them.
namespace gnubase::ui
{
	// The transpose events on the transpose track's lane, in an element of its own (<div class="transpose-row">)
	// placed over the lane: a .transpose-event per event in a .transpose-events, with its value in semitones as text
	// and attribute event (its id). The draw tool adds an event at the nearest bar (transpose.add) and opens it for
	// editing, the erase tool removes the event clicked, and with the other tools a drag moves an event to the nearest
	// bar (transpose.set) and a double click opens it for editing: the row's <input class="transpose-field">, .shown
	// over it, where Enter sets the value typed. The row follows the project and the display's view. Use it on the UI
	// thread; the display and the binding must outlive it.
	class TransposeLane final : private Rml::EventListener
	{
	public:
		TransposeLane(EventDisplay& display, EngineBinding& binding);
		~TransposeLane() override;
		TransposeLane(const TransposeLane&) = delete;
		TransposeLane& operator=(const TransposeLane&) = delete;
		TransposeLane(TransposeLane&&) = delete;
		TransposeLane& operator=(TransposeLane&&) = delete;

		Rml::Element* element() const { return m_element.get(); }
		// Opens an event for editing.
		void edit(uint64_t event);

	private:
		struct Event
		{
			uint64_t id = 0;
			int64_t start = 0; // samples
			int value = 0;
		};

		void ProcessEvent(Rml::Event& event) override;
		bool alive() const { return static_cast<bool>(m_element); }
		void requestProject();
		void show();
		void place();
		void press(Rml::Event& event);
		void release(Rml::Event& event);
		// The position of the bar nearest to a time in seconds, in the transpose track's time base.
		int64_t nearestBar(double seconds) const;
		double seconds(int64_t samples) const { return static_cast<double>(samples) / m_time.sampleRate; }

		EventDisplay& m_display;
		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_element;
		Rml::Element* m_events = nullptr;           // a .transpose-event per event
		Rml::ElementFormControl* m_field = nullptr; // .shown while an event is edited
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
		TimeContext m_time;
		uint64_t m_track = 0; // the transpose track, 0 without one
		bool m_linear = false;
		std::vector<Event> m_list;
		std::optional<std::pair<uint64_t, float>> m_drag;       // the event pressed and where, in pixels
		std::optional<uint64_t> m_editing;                      // the event the field edits
		std::optional<std::pair<uint64_t, double>> m_lastPress; // the event pressed, and when
		bool m_loading = false;
		bool m_stale = false;
		std::vector<Subscription> m_subscriptions;
	};
}
