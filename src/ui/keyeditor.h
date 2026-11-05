// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"

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
}

namespace gnubase::ui
{
	class ControllerLanes;
}

// The key editor (#127, reference manual pp. 1189-1220): docs/features/key-editor.md describes it.
namespace gnubase::ui
{
	// The key editor's tools (pp. 1192-1193, 1211-1215), in the toolbar's order.
	enum class KeyTool : uint8_t
	{
		ObjectSelection,
		Draw,
		Erase,
		Trim,
		Split,
		Glue,
		Line, // on the controller lanes, in its mode (#128)
	};

	// Fills an element of a MidiEditor with the key editor: its toolbar (tools, the transpose palette, step and MIDI
	// input, length quantize), the info line of the first selected note, the parts it shows by track (a click
	// activates one, as MidiEditor handles it), the note display, a piano keyboard beside a grid of 128 pitches
	// with the notes as boxes and the step input cursor, and below it the controller lanes (ControllerLanes). The
	// notes' time axis fits the parts' range into the width. A right click in the note display opens the toolbox
	// #key-editor-toolbox, the toolbar's tools at the mouse, as in the event display (reference manual p. 65): with
	// the user setting ui.toolboxOnRightClick, else with a modifier key; the key editor has no context menu yet.
	// show() gives it the project and the engine's editor view
	// whenever they change; the selection is the view's (editor.set selection), and every edit goes to the engine as
	// note.* commands, those of several parts in one history.transaction. Use it on the UI thread; the binding must
	// outlive it.
	class KeyEditor final : private Rml::EventListener
	{
	public:
		// drum: the drum editor (#129, reference manual pp. 1221-1242): the active track's drum map's sound list
		// instead of the keyboard, its rows in the map's order, notes as diamonds and the drumstick instead of the draw
		// tool.
		KeyEditor(EngineBinding& binding, Rml::Element& root, std::string view, bool drum = false);
		~KeyEditor() override;
		KeyEditor(const KeyEditor&) = delete;
		KeyEditor& operator=(const KeyEditor&) = delete;
		KeyEditor(KeyEditor&&) = delete;
		KeyEditor& operator=(KeyEditor&&) = delete;

		void show(const engine::Json& project, const engine::Json& view);

		KeyTool tool() const { return m_tool; }
		bool drum() const { return m_drum; }
		void setTool(KeyTool tool);

		// Scrolls the note display so that the pitch is in the middle; false before the display has its size. The
		// display scrolls to the middle of its notes, or to middle C, when it first has a size, unless it was
		// scrolled to a pitch before.
		bool scrollToPitch(int pitch);

	private:
		struct Note
		{
			uint64_t part = 0;
			uint64_t id = 0;
			int64_t start = 0; // project ticks
			int64_t length = 0;
			int pitch = 60;
			double velocity = 0.0;
			double releaseVelocity = 0.5;
			int channel = 0;
			bool editable = false;
		};

		struct Part
		{
			uint64_t id = 0;
			uint64_t track = 0;
			int64_t start = 0; // project ticks
			int64_t end = 0;
			bool editable = false;
		};

		// What a press in the note display started.
		enum class Gesture : uint8_t
		{
			None,
			Move,
			ResizeStart,
			ResizeEnd,
			Draw,
		};

		void ProcessEvent(Rml::Event& event) override;
		void press(Rml::Event& event);
		void drag(Rml::Event& event);
		void release(Rml::Event& event);
		void doubleClick(Rml::Event& event);
		void toolbar(Rml::Element& target);
		// The note display opens the toolbox on a right click with the user setting ui.toolboxOnRightClick, else on
		// one with a modifier key.
		void showToolbox();

		void render();
		void renderInfo();
		// The drum editor's: the active track's drum map, the rows' order from it, and the sound list and Map menu.
		const engine::Json* activeMap() const;
		void updateOrder();
		std::string soundsRml() const;
		void renderDrumMap();
		void drumMapMenu(Rml::Element& target);
		void editSound(int pitch, const std::string& field, const std::string& text);
		void place(Rml::Element& element, int64_t start, int64_t length, int pitch) const;
		std::string partsRml() const;

		// The note display's point under the mouse: project ticks and pitch.
		std::pair<double, int> at(const Rml::Event& event) const;
		int64_t snap(double ticks, bool off) const;
		int64_t delta(Gesture gesture, double ticks) const;
		int64_t grid() const;
		// The length of new notes of the pitch: with Drum-Map Link its sound's snap.
		int64_t lengthQuantize(int pitch) const;
		// The pitch rows away from pitch in the display's order, or -1 beyond its ends.
		int shifted(int pitch, int rows) const;
		const Note* note(uint64_t part, uint64_t id) const;
		const Note* noteOf(Rml::Element* element) const;
		// The editable part at the ticks: the active one if it is there, else the first.
		const Part* partAt(int64_t ticks) const;
		bool selected(const Note& note) const;
		std::vector<const Note*> selectedNotes() const;

		void select(std::vector<std::pair<uint64_t, uint64_t>> notes);
		void setView(engine::Json parameters);
		// Sends note.* commands, each {command, parameters}: one alone, several in a history.transaction.
		void edit(const std::string& description, engine::Json commands);
		// Notes of the pitch at the starts, each of the length.
		void addNotes(const Part& part, const std::vector<int64_t>& starts, int64_t length, int pitch);
		void transpose(int semitones);
		void editInfo(const std::string& field, const std::string& text);

		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_root;
		std::string m_name;
		bool m_drum = false;
		KeyTool m_tool = KeyTool::ObjectSelection;
		std::vector<int> m_order; // the pitches of the rows, from the top
		std::vector<int> m_rowOf; // each pitch's row
		std::string m_soundsRml;  // the drum sound list as last set
		std::string m_mapMenuRml; // the Map menu as last set
		engine::Json m_project = engine::Json::object();
		engine::Json m_view = engine::Json::object();
		std::vector<Part> m_parts;
		std::vector<Note> m_notes;
		std::vector<std::pair<uint64_t, uint64_t>> m_selection;
		int64_t m_from = 0; // the notes' time axis, project ticks
		int64_t m_to = 1;
		bool m_scrolled = false;
		std::string m_notesRml; // the notes' elements as last set

		Gesture m_gesture = Gesture::None;
		std::pair<double, int> m_pressedAt;
		std::optional<Note> m_pressed; // the note pressed, as it was
		bool m_snapOff = false;
		Rml::ObserverPtr<Rml::Element> m_drawn; // the note a draw drags out
		// the length quantize menu, the line tool's menu, the toolbox, then the drum editor's Map menu
		std::vector<Rml::ObserverPtr<Rml::Element>> m_menus;
		std::string m_lineMode = "line"; // the line tool's (#128)
		std::unique_ptr<ControllerLanes> m_lanes;
		bool m_toolboxOnRightClick = false; // ui.toolboxOnRightClick
		bool m_toolboxKnown = false;        // changed since it was asked for
		std::vector<Subscription> m_subscriptions;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for answers that come after it's gone
	};

	const char* keyToolName(KeyTool tool);
	std::optional<KeyTool> keyToolFromName(const std::string& name);
}
