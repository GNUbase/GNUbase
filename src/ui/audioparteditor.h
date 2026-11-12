// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"
#include "ui/eventwaveforms.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Rml
{
	class Element;
}

// The audio part editor (#146, reference manual pp. 692-702): docs/features/audio-part-editor.md describes it.
namespace gnubase::ui
{
	// The audio part editor's tools (pp. 694-695), in the toolbar's order.
	enum class AudioPartTool : uint8_t
	{
		ObjectSelection,
		Erase,
		Split,
		Glue,
		Mute,
		Play,
		Scrub,
	};

	// Fills an element of a MidiEditor with the audio part editor: its toolbar's tools, the info line of the first
	// selected audio event, the parts it shows by track (a click activates one, as MidiEditor handles it) and the part
	// display, the events of the parts on their lanes with their waveforms, cut at their part's edges. The time axis
	// fits the parts' range into the width. show() gives it the project and the engine's editor view whenever they
	// change; the selection is the view's (editor.set eventSelection), and every edit goes to the engine as event.*
	// commands, several in one history.transaction. Use it on the UI thread; the binding must outlive it.
	class AudioPartEditor final : private Rml::EventListener
	{
	public:
		AudioPartEditor(EngineBinding& binding, Rml::Element& root, std::string view);
		~AudioPartEditor() override;
		AudioPartEditor(const AudioPartEditor&) = delete;
		AudioPartEditor& operator=(const AudioPartEditor&) = delete;
		AudioPartEditor(AudioPartEditor&&) = delete;
		AudioPartEditor& operator=(AudioPartEditor&&) = delete;

		void show(const engine::Json& project, const engine::Json& view);

		AudioPartTool tool() const { return m_tool; }
		// The project as show() last had it.
		const engine::Json& project() const { return m_project; }
		void setTool(AudioPartTool tool);

	private:
		struct Event
		{
			uint64_t id = 0;
			uint64_t part = 0;
			uint64_t track = 0;
			double start = 0.0; // seconds, the event's own
			double end = 0.0;
			double shownStart = 0.0; // what its part lets play of it, or all of it if its part hides it entirely
			double shownEnd = 0.0;
			int lane = 0;
			double volume = 0.0; // dB
			bool mute = false;
			bool editable = false;
			bool outside = false; // moved out of its part, which hides it entirely
			uint64_t clip = 0;
			int64_t offset = 0; // in the clip, its samples
			int64_t length = 0;
		};

		// A part the editor shows: its time in seconds and track, and whether the editing mode reaches it.
		struct Part
		{
			double start = 0.0;
			double end = 0.0;
			uint64_t track = 0;
			bool editable = false;
		};

		// What a press in the part display started.
		enum class Gesture : uint8_t
		{
			None,
			Move,         // in time and across lanes
			MoveVertical, // with Ctrl: across lanes only, or a click that toggles the event's selection (p. 699)
			Audition,     // the play or scrub tool
		};

		void ProcessEvent(Rml::Event& event) override;
		void press(Rml::Event& event);
		void begin(Rml::Event& event);
		// Ends a gesture without its effect, as when its mouseup never came; whether the events need placing again.
		bool cancel();
		// While a gesture goes on, the document's mouseups and (in the capture phase) presses.
		void listen(bool document);
		void drag(Rml::Event& event);
		// Shows the events a move moves m_moved further, also after a render during the drag.
		void preview();
		bool moving() const; // a press started a move that goes on
		// The project or view shown changed: a move follows its event, or ends if that went.
		void updateMove();
		void updateMoved();
		// The pressed event's move for the mouse over seconds.
		double movedBy(double seconds) const;
		// Works a dragged move out again on the layout as it is now.
		void moveAgain();
		void release(Rml::Event& event);
		void doubleClick(Rml::Event& event);

		void render();
		void renderEvents(Rml::Element& events);
		void renderInfo();
		std::string partsRml() const;
		void place(Rml::Element& element, double start, double end, int lane) const;
		int lanes() const;
		float dpRatio() const;

		// The part display's point under the mouse: seconds and lane.
		std::pair<double, int> at(const Rml::Event& event) const;
		std::pair<double, int> at(Rml::Vector2f mouse) const;
		// To the project's quantize grid.
		double snap(double seconds) const;
		// Whether the mouse has moved past the drag threshold since the press: until then a press is a click.
		bool dragged(const Rml::Event& event);
		int64_t samples(double seconds) const;
		const Event* eventOf(Rml::Element* element) const;
		bool selected(const Event& event) const;
		// Whether next continues event in its clip, as after a split: event.glue makes them one event.
		bool continues(const Event& event, const Event& next) const;
		std::vector<const Event*> selectedEvents() const; // by start

		void select(std::vector<uint64_t> events);
		// Sends event.* commands, each {command, parameters}: one alone, several in a history.transaction.
		void edit(const std::string& description, engine::Json commands);
		void editInfo(const std::string& field, const std::string& text);

		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_root;
		std::string m_name;
		AudioPartTool m_tool = AudioPartTool::ObjectSelection;
		engine::Json m_project = engine::Json::object();
		engine::Json m_view = engine::Json::object();
		std::vector<Event> m_events;
		std::map<uint64_t, Part> m_parts; // by id
		std::vector<EventWaveforms::Event> m_waves;
		std::vector<uint64_t> m_selection;
		int m_selecting = 0; // editor.set commands of select() that the engine hasn't answered
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for answers that come after it's gone
		double m_from = 0.0;                                          // the time axis, seconds
		double m_to = 1.0;
		std::string m_shownEvents; // what the events' elements were last made from
		std::unique_ptr<EventWaveforms> m_waveforms;

		Gesture m_gesture = Gesture::None;
		std::pair<double, int> m_pressedAt; // the seconds and lane under the press
		double m_pressedOffset =
			0.0; // seconds from the pressed event's start to the press, kept as the display changes
		Rml::Vector2f m_pressedMouse; // px
		bool m_dragged = false;
		bool m_laying = false;                     // moveAgain() lays the document out
		Rml::Vector2f m_draggedTo;                 // px, the mouse's last place in a dragged move
		std::pair<double, int> m_moved;            // a dragged move's seconds and lanes
		std::optional<Event> m_pressed;            // the event pressed, as the project shown has it
		uint64_t m_auditioned = 0;                 // the track the play or scrub tool auditions
		Rml::ObserverPtr<Rml::Element> m_document; // whose resize fits the waveforms to the display again
		Rml::ObserverPtr<Rml::Element> m_listened; // the document listen() hears a gesture's mouse in
	};

	const char* audioPartToolName(AudioPartTool tool);
	std::optional<AudioPartTool> audioPartToolFromName(const std::string& name);
}
