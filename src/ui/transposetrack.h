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
#include <string>
#include <utility>
#include <vector>

namespace Rml
{
	class Element;
	class ElementFormControl;
}

// The transpose track, the chord track and the arranger track in the UI (#168, reference manual pp. 165, 418; #173,
// #933, pp. 1320-1331; #1393, pp. 404-405): the transpose events on the transpose track's lane, the chord events and
// the scale events (the scale lane, while Show Scales is on) on the chord track's lane and the arranger events on the
// arranger track's lane in the event display. The events are the engine's (transpose.*, chord.*, arranger.*);
// docs/features/transpose.md, chord-track.md and arranger.md describe them.
namespace gnubase::ui
{
	// Which track's events a lane shows: the transpose track's (class names transpose-*), the chord track's chord
	// events (chord-*) or its scale events (scale-*), which show in the lower half of its lane while Show Scales is on,
	// with the chord events in the upper half, or the arranger track's arranger events (arranger-*), which span their
	// sections and show their names.
	enum class LaneKind : uint8_t
	{
		Transpose,
		Chord,
		Scale,
		Arranger,
	};

	// A global track's events on its lane, in an element of its own (<div class="transpose-row">, or "chord-row")
	// placed over the lane: a .transpose-event per event in a .transpose-events, with its value in semitones (a chord
	// event: its chord's name) as text and attribute event (its id). The draw tool adds an event at the nearest bar
	// (transpose.add, chord.add: an undefined chord) and opens it for editing, the erase tool removes the event
	// clicked, and with the other tools a drag moves an event to the nearest bar (transpose.set) and a double click
	// opens it for editing: the row's <input class="transpose-field">, .shown over it, where Enter sets the value (the
	// chord, the scale such as "D dorian") typed. A chord event made from audio is .detected. With the chord track's
	// Acoustic Feedback and a track for auditioning, a press on a chord or scale event auditions it until the button
	// goes up (chord.audition). Arranger events differ: they are as wide as their sections, the draw tool draws one
	// from the press to the release (a click: to the next bar line), a drag moves one, both snapped as the display's
	// drags snap, and the field renames one. The row follows the project and the display's view. Use it on the UI
	// thread; the display and the binding must outlive it.
	class GlobalEventLane final : private Rml::EventListener
	{
	public:
		GlobalEventLane(EventDisplay& display, EngineBinding& binding, LaneKind kind = LaneKind::Transpose);
		~GlobalEventLane() override;
		GlobalEventLane(const GlobalEventLane&) = delete;
		GlobalEventLane& operator=(const GlobalEventLane&) = delete;
		GlobalEventLane(GlobalEventLane&&) = delete;
		GlobalEventLane& operator=(GlobalEventLane&&) = delete;

		Rml::Element* element() const { return m_element.get(); }
		// Opens an event for editing.
		void edit(uint64_t event);

	private:
		struct Event
		{
			uint64_t id = 0;
			int64_t start = 0; // samples
			int64_t end = 0;   // samples, an arranger event's end
			std::string text;  // as the event shows it
			std::string value; // as the field edits it
			bool detected = false;
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
		// A time in seconds as a position in the track's time base.
		int64_t position(double seconds) const;
		// Adds an arranger event the draw tool drew from the press at seconds to the release dx dp away.
		void draw(double from, double dx);

		// A class name of the lane's kind, such as "transpose-event", and a command or event, such as "chord.add".
		std::string named(const char* suffix) const;
		std::string command(const char* suffix) const;
		Event eventFrom(const engine::Json& event) const;

		EventDisplay& m_display;
		EngineBinding& m_binding;
		LaneKind m_kind;
		Rml::ObserverPtr<Rml::Element> m_element;
		Rml::Element* m_events = nullptr;           // a .transpose-event per event
		Rml::ElementFormControl* m_field = nullptr; // .shown while an event is edited
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
		TimeContext m_time;
		uint64_t m_track = 0; // the transpose or chord track, 0 without one
		bool m_linear = false;
		bool m_showScales = false; // the chord track's Show Scales
		bool m_audition = false;   // a press auditions the event
		bool m_auditioning = false;
		std::vector<Event> m_list;
		std::optional<std::pair<uint64_t, float>> m_drag;       // the event pressed and where, in pixels
		std::optional<std::pair<double, float>> m_draw;         // where the draw tool pressed, in seconds and pixels
		std::optional<uint64_t> m_editing;                      // the event the field edits
		std::optional<std::pair<uint64_t, double>> m_lastPress; // the event pressed, and when
		bool m_loading = false;
		bool m_stale = false;
		std::vector<Subscription> m_subscriptions;
	};
}
