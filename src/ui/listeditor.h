// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"
#include "ui/timeformat.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Rml
{
	class Element;
}

// The list editor (#130, reference manual pp. 1243-1258): docs/features/list-editor.md describes it.
namespace gnubase::ui
{
	// Fills an element of a MidiEditor with the list editor: its toolbar (tools, Insert Event Type, Length Quantize,
	// Insert Velocity), the filters bar, and a row per event of the editor's parts in playback order: the event list's
	// columns, the event in the event display at its time and its value as a bar in the value display. show() gives it
	// the project and the engine's editor view whenever they change; the selection is the view's (editor.set selection
	// and controllerSelection), and every edit goes to the engine as note.* and controller.* commands, those of
	// several parts in one history.transaction. The auditioning column points at the event at the project cursor, which
	// playbackPosition (Engine::playbackPosition) gives while playing. Use it on the UI thread; the binding must
	// outlive it.
	class ListEditor final : private Rml::EventListener
	{
	public:
		ListEditor(EngineBinding& binding, Rml::Element& root, std::string view,
			std::function<int64_t()> playbackPosition = {});
		~ListEditor() override;
		ListEditor(const ListEditor&) = delete;
		ListEditor& operator=(const ListEditor&) = delete;
		ListEditor(ListEditor&&) = delete;
		ListEditor& operator=(ListEditor&&) = delete;

		void show(const engine::Json& project, const engine::Json& view);

	private:
		// An event of the list: a note or a controller event, with its values in MIDI 1.0's units as the list shows
		// them.
		struct Row
		{
			uint64_t part = 0;
			uint64_t track = 0; // the part's
			uint64_t id = 0;
			std::string type;      // note, or controller.add's type
			int64_t partStart = 0; // project ticks
			int64_t start = 0;
			int64_t length = 0;   // notes
			int number = 0;       // pitch, controller or program number
			double value = 0.0;   // 0-1: velocity, controller value, pressure, bend
			double release = 0.5; // notes: release velocity
			int channel = 0;
			std::vector<int> data;                  // SysEx
			std::string articulation;               // a direction's name (#1112)
			bool reset = false;                     // a direction that is a reset
			std::vector<std::string> articulations; // a note's attributes
			bool unknown = false; // attributes or a direction that the track's articulation map doesn't have (#1580)
			bool editable = false;

			bool isNote() const { return type == "note"; }
			std::pair<uint64_t, uint64_t> key() const { return {part, id}; }
		};

		struct Part
		{
			uint64_t id = 0;
			int64_t start = 0; // project ticks
			int64_t end = 0;
			bool editable = false;
		};

		enum class Gesture : uint8_t
		{
			None,
			Move,
			Resize, // a note's end
			Draw,
			Values,
		};

		void ProcessEvent(Rml::Event& event) override;
		void press(Rml::Event& event);
		void begin(Rml::Event& event);
		// Ends the gesture without its effect. Returns whether the value display showed values of it.
		bool cancel();
		// Hears the mouseup, the mouseout and, in the capture phase, the mousedown in the root's document while a
		// gesture goes on, or stops hearing them.
		void listen(bool document);
		// Makes the body a drag element while a gesture goes on, or stops doing so.
		void dragBody(bool on);
		void drag(Rml::Event& event);
		void release(Rml::Event& event);
		void click(Rml::Element& target, bool ctrl);

		void render();
		// The auditioning column's arrow (p. 1251): at the last row shown that starts at the cursor or before it.
		// Returns whether it moved.
		bool showCursor();
		// A transport.changed or transport.get status: the cursor, and while playing the playback position.
		void followTransport(const engine::Json& status);
		bool shown(const Row& row, const Row* first) const;
		std::string text(const Row& row, const std::string& field) const;
		// The field's value as a whole number in the list's units, if the row's type has it.
		static std::optional<int64_t> number(const Row& row, const std::string& field);
		// A value typed for the field: spaces around it don't count, and a pitch name can be in either case.
		std::optional<int64_t> parse(const std::string& field, const std::string& text, const Row& row) const;
		// The value display's bar: 0-1, if the row's type has one.
		static std::optional<double> bar(const Row& row);

		const Row* rowOf(Rml::Element* element) const;
		// The Attributes column's pop-up menu (#1580, p. 1291), which a right click on a note's or a direction's
		// Attributes opens: the map's attributes, one chosen toggling on the note in place of its group's, or the
		// map's directions, one chosen renaming the direction.
		void openArticulationMenu(const Row& row);
		void chooseArticulation(const std::string& name);
		bool selected(const Row& row) const;
		std::vector<const Row*> selectedRows() const;
		double ticksAt(const Rml::Event& event, Rml::Element& lane) const;
		double ticksAt(const Rml::Event& event, float left, float width) const;
		int64_t snap(double ticks) const;
		int64_t grid() const;
		int64_t lengthQuantize() const;
		const Part* partAt(int64_t ticks) const;

		// The view's selected notes and controller events.
		std::set<std::pair<uint64_t, uint64_t>> viewSelection() const;
		// show: whether the rows show the selection at once, or when editor.changed confirms it.
		void select(std::set<std::pair<uint64_t, uint64_t>> rows, bool show = true);
		void setView(engine::Json parameters);
		void edit(std::map<std::pair<uint64_t, std::string>, engine::Json> changes);
		void editField(const Row& edited, const std::string& field, const std::string& typed);
		void add(int64_t start, int64_t length);
		void erase(const Row& row);
		// The row's event as the project has it.
		engine::Json eventJson(const Row& row) const;
		// Deletes the selected events: Delete and Backspace (pp. 1255, 1258).
		void eraseSelected();

		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_root;
		std::string m_name;
		std::string m_tool = "objectSelection";
		std::string m_insertType = "note";
		std::string m_focus = "none";
		std::set<std::string> m_hidden; // event types the filters bar hides
		engine::Json m_project = engine::Json::object();
		engine::Json m_view = engine::Json::object();
		TimeContext m_time; // the project's, read once per show
		std::vector<Part> m_parts;
		std::vector<Row> m_rows;
		std::set<std::pair<uint64_t, uint64_t>> m_selection; // notes and controller events: ids are unique
		int m_selectionsOut = 0; // select()'s editor.set commands not answered yet: show() keeps m_selection then
		int64_t m_from = 0;      // the event display's time axis, project ticks
		int64_t m_to = 1;
		std::string m_rowsRml; // the rows' elements as last set
		bool m_ctrl = false;   // Ctrl (Cmd) held at the last key press: an edit sets every selected event's value

		Gesture m_gesture = Gesture::None;
		std::optional<Row> m_pressed;
		double m_pressedAt = 0.0;
		// The pressed event lane's left edge and width: a move ends wherever the mouse is, and a Focus filter can
		// rebuild the rows while it goes on.
		float m_laneLeft = 0.0F;
		float m_laneWidth = 1.0F;
		bool m_copy = false; // Alt (Opt) held at a move's press: it copies the events
		std::map<std::pair<uint64_t, uint64_t>, double> m_values; // what a drag in the value display set
		bool m_dragged = false; // a drag event came since the press: the mouse leaving the window keeps the gesture
		// The document that a gesture hears the mouseup in, wherever in it the mouse goes up.
		Rml::ObserverPtr<Rml::Element> m_document;
		std::vector<Rml::ObserverPtr<Rml::Element>> m_menus;
		std::optional<std::pair<uint64_t, uint64_t>> m_menuRow; // the part and note or direction of the menu
		bool m_menuNote = false;
		std::function<int64_t()> m_playbackPosition;
		int64_t m_cursor = 0;              // the project cursor, samples
		std::vector<time::Ticks> m_starts; // the rows shown's starts, in their order
		int m_atCursor = -1;               // the row shown that the arrow is at, -1 for none
		std::vector<Subscription> m_subscriptions;
		std::optional<Subscription> m_position; // while playing
		bool m_doubleClicked = false;           // the auditioning column's dblclick, until the click that ends it
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for answers that come after it went
	};
}
