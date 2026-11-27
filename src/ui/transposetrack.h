// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/timeformat.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <functional>
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
	// with the chord events in the upper half, or the arranger track's arranger events (arranger-lane-*), which span
	// their sections and show their names. Not arranger-*: those are the arranger editor's, whose lists' rows are
	// .arranger-row, and the styles of the one would style the other.
	enum class LaneKind : uint8_t
	{
		Transpose,
		Chord,
		Scale,
		Arranger,
	};

	// The event an element gets where arranger events dragged from the lane are dropped outside it, such as on the
	// inspector's Current Arranger Chain list (reference manual p. 409): its parameter events has their ids, separated
	// by spaces, and mouse_y where the mouse is. An element that takes them stops its propagation, and the events
	// stay where they are; otherwise the drag moves them.
	inline constexpr const char* g_arrangerDrop = "arrangerdrop";

	// A global track's events on its lane, in an element of its own (<div class="transpose-row">, or "chord-row")
	// placed over the lane: a .transpose-event per event in a .transpose-events, with its value in semitones (a chord
	// event: its chord's name) as text and attribute event (its id). The draw tool adds an event at the nearest bar
	// (transpose.add, chord.add: an undefined chord) and opens it for editing, the erase tool removes the event
	// clicked, and with the other tools a drag moves an event to the nearest bar (transpose.set) and a double click
	// opens it for editing: the row's <input class="transpose-field">, .shown over it, where Enter sets the value (the
	// chord, the scale such as "D dorian") typed. A chord event made from audio is .detected. With the chord track's
	// Acoustic Feedback and a track for auditioning, a press on a chord or scale event auditions it until the button
	// goes up (chord.audition). While a drag moves an event, it follows the mouse. Arranger events differ: they are as
	// wide as their sections, the draw tool draws one from the press to the release, also over other arranger events,
	// showing it as an .arranger-lane-event.drawing while the button is down (a click draws from the grid line at or
	// before it to the next bar line), a drag moves one, both snapped as the display's drags snap with the other
	// arranger events' edges as magnets too, and the field, as wide as the event, renames one (an empty name keeps the
	// old one). A click selects an arranger event (.selected), with Ctrl (Cmd) adds or removes it, and a press beside
	// the events clears the selection; a drag of an edge (.arranger-lane-edge) resizes the event, snapped as a move
	// is, and a drag of a selected one moves all selected (#2076). An edit the engine refuses says why, as the
	// display's own edits do (EventDisplay::showRefusal). The row follows the project as the display reads it, and the
	// track's own events (transpose.changed, chord.changed, arranger.changed), and the display's view. Use it on the UI
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
		// The arranger events selected, in the order they were; the info line shows them (p. 405).
		const std::vector<uint64_t>& selection() const { return m_selection; }
		void deselect();
		// Removes the arranger events selected (Delete); whether there were any.
		bool removeSelected();
		// Told when a click selects arranger events, so that the other selections go (AppUi).
		void onSelect(std::function<void()> handler) { m_onSelect = std::move(handler); }

	private:
		struct Event
		{
			uint64_t id = 0;
			int64_t start = 0; // samples
			int64_t end = 0;   // samples, an arranger event's end
			std::string text;  // as the event shows it
			std::string value; // as the field edits it
			bool detected = false;
			int64_t position = 0; // an arranger event's start in the track's time base
		};

		// An event a drag moves or resizes, which shows so until the lane reads it where the engine put it.
		struct Moved
		{
			uint64_t event = 0;
			double shift = 0.0;    // seconds the start moves, snapped
			double endShift = 0.0; // and the end
			int64_t from = 0;      // where it was, samples
			int64_t fromEnd = 0;
			bool dragging = false; // the button is still down
			bool answered = false; // the move's answer is in
			bool group = false;    // the selected arranger events move with it
		};

		// What a drag on an arranger event changes: where it is, or one of its edges.
		enum class Edge : uint8_t
		{
			None,
			Start,
			End,
		};

		void ProcessEvent(Rml::Event& event) override;
		bool alive() const { return static_cast<bool>(m_element); }
		// Reads the project the display read last, once per answer, or the track's part of it, as project.get and
		// the track's *.changed events have it.
		void follow();
		void read(const engine::Json& section);
		void show();
		void place();
		// Whether the display shows the track's lane, and the scale lane Show Scales.
		bool shownOnLane() const;
		void press(Rml::Event& event);
		void move(Rml::Event& event);
		void release(Rml::Event& event);
		// Drops the arranger event dragged, or the selected ones in timeline order if it is one of them, where the
		// release is outside the lane (g_arrangerDrop); whether something took them.
		bool dropOutside(Rml::Event& event, uint64_t dragged);
		const Event* find(uint64_t id) const;
		// Shows an event moved (or an arranger event's edge, m_edge) as a drag dx dp across moves it, if it moves that
		// far; whether it does.
		bool moveTo(uint64_t id, double dx);
		// Ends what a drag shows once the lane shows the event drawn or moved as the engine has it.
		void settle();
		// Sends an edit; if the engine refuses it, the display says why. Then done runs with the result.
		void send(const std::string& name, engine::Json parameters,
			std::function<void(const engine::Result&)> done = nullptr);
		// The position of the bar nearest to a time in seconds, in the transpose track's time base.
		int64_t nearestBar(double seconds) const;
		double seconds(int64_t samples) const { return static_cast<double>(samples) / m_display.time().sampleRate; }
		// A time in seconds as a position in the track's time base.
		int64_t position(double seconds) const;
		// The arranger event the draw tool draws from the press at seconds to dx dp away: its start and end, seconds.
		std::pair<double, double> drawn(double from, double dx) const;
		// Where a drag dx dp across moves an event's start to, in seconds.
		double movedTo(const Event& event, double dx) const;
		// Where a drag dx dp across moves an arranger event's edge at edge seconds to, in seconds.
		double edgeTo(double edge, uint64_t event, double dx) const;
		// The edges of the arranger events but one (0: all of them), in samples, which arranger events snap to.
		std::vector<int64_t> magnets(uint64_t except) const;

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
		uint64_t m_loaded = 0; // the display's answers read (EventDisplay::answers)
		uint64_t m_track = 0;  // the transpose or chord track, 0 without one
		bool m_linear = false;
		bool m_showScales = false; // the chord track's Show Scales
		bool m_audition = false;   // a press auditions the event
		bool m_auditioning = false;
		std::vector<Event> m_list;
		std::optional<std::pair<uint64_t, float>> m_drag; // the event pressed and where, in pixels
		Edge m_edge = Edge::None;                         // the arranger event's edge pressed
		std::vector<uint64_t> m_selection;                // the arranger events selected
		std::function<void()> m_onSelect;
		std::optional<Moved> m_moved;                           // what a drag moves
		std::optional<std::pair<double, float>> m_draw;         // where the draw tool pressed, in seconds and pixels
		std::optional<std::pair<double, double>> m_drawn;       // what it draws, seconds, until m_added shows
		std::optional<uint64_t> m_added;                        // the event drawn, once answered
		std::optional<uint64_t> m_editing;                      // the event the field edits
		std::optional<std::pair<uint64_t, double>> m_lastPress; // the event pressed, and when
		std::vector<Subscription> m_subscriptions;
	};
}
