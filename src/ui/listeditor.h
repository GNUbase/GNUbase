// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <map>
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
	// several parts in one history.transaction. Use it on the UI thread; the binding must outlive it.
	class ListEditor final : private Rml::EventListener
	{
	public:
		ListEditor(EngineBinding& binding, Rml::Element& root, std::string view);
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
			uint64_t id = 0;
			std::string type;      // note, or controller.add's type
			int64_t partStart = 0; // project ticks
			int64_t start = 0;
			int64_t length = 0;   // notes
			int number = 0;       // pitch, controller or program number
			double value = 0.0;   // 0-1: velocity, controller value, pressure, bend
			double release = 0.5; // notes: release velocity
			int channel = 0;
			std::vector<int> data; // SysEx
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
			Draw,
			Values,
		};

		void ProcessEvent(Rml::Event& event) override;
		void press(Rml::Event& event);
		void drag(Rml::Event& event);
		void release(Rml::Event& event);
		void click(Rml::Element& target, bool ctrl);

		void render();
		bool shown(const Row& row, const Row* first) const;
		std::string text(const Row& row, const std::string& field) const;
		// The field's value as a whole number in the list's units, if the row's type has it.
		static std::optional<int64_t> number(const Row& row, const std::string& field);
		std::optional<int64_t> parse(const std::string& field, const std::string& typed, const Row& row) const;
		// The value display's bar: 0-1, if the row's type has one.
		static std::optional<double> bar(const Row& row);

		const Row* rowOf(Rml::Element* element) const;
		bool selected(const Row& row) const;
		std::vector<const Row*> selectedRows() const;
		double ticksAt(const Rml::Event& event, Rml::Element& lane) const;
		int64_t snap(double ticks) const;
		int64_t grid() const;
		int64_t lengthQuantize() const;
		const Part* partAt(int64_t ticks) const;

		void select(std::set<std::pair<uint64_t, uint64_t>> rows);
		void setView(engine::Json parameters);
		void edit(const std::string& description, std::map<std::pair<uint64_t, std::string>, engine::Json> changes);
		void editField(const Row& edited, const std::string& field, const std::string& typed);
		void add(int64_t start, int64_t length);
		void erase(const Row& row);

		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_root;
		std::string m_name;
		std::string m_tool = "objectSelection";
		std::string m_insertType = "note";
		std::string m_focus = "none";
		std::set<std::string> m_hidden; // event types the filters bar hides
		engine::Json m_project = engine::Json::object();
		engine::Json m_view = engine::Json::object();
		std::vector<Part> m_parts;
		std::vector<Row> m_rows;
		std::set<std::pair<uint64_t, uint64_t>> m_selection; // notes and controller events: ids are unique
		int64_t m_from = 0;                                  // the event display's time axis, project ticks
		int64_t m_to = 1;
		std::string m_rowsRml; // the rows' elements as last set
		bool m_ctrl = false;   // Ctrl (Cmd) held at the last key press: an edit sets every selected event's value

		Gesture m_gesture = Gesture::None;
		std::optional<Row> m_pressed;
		double m_pressedAt = 0.0;
		std::map<std::pair<uint64_t, uint64_t>, double> m_values; // what a drag in the value display set
		std::vector<Rml::ObserverPtr<Rml::Element>> m_menus;
	};
}
