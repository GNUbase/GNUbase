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
	// notes' time axis fits the parts' range into the width. show() gives it the project and the engine's editor view
	// whenever they change; the selection is the view's (editor.set selection), and every edit goes to the engine as
	// note.* commands, those of several parts in one history.transaction. Use it on the UI thread; the binding must
	// outlive it.
	class KeyEditor final : private Rml::EventListener
	{
	public:
		KeyEditor(EngineBinding& binding, Rml::Element& root, std::string view);
		~KeyEditor() override;
		KeyEditor(const KeyEditor&) = delete;
		KeyEditor& operator=(const KeyEditor&) = delete;
		KeyEditor(KeyEditor&&) = delete;
		KeyEditor& operator=(KeyEditor&&) = delete;

		void show(const engine::Json& project, const engine::Json& view);

		KeyTool tool() const { return m_tool; }
		void setTool(KeyTool tool);

		// Scrolls the note display so that the pitch is in the middle; false before the display has its size. The
		// display scrolls to the middle of its notes, or to middle C, when it first has a size.
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

		void render();
		void renderInfo();
		void place(Rml::Element& element, int64_t start, int64_t length, int pitch) const;
		std::string partsRml() const;

		// The note display's point under the mouse: project ticks and pitch.
		std::pair<double, int> at(const Rml::Event& event) const;
		int64_t snap(double ticks, bool off) const;
		int64_t delta(Gesture gesture, double ticks) const;
		int64_t grid() const;
		int64_t lengthQuantize() const;
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
		void addNote(const Part& part, int64_t start, int64_t length, int pitch);
		void transpose(int semitones);
		void editInfo(const std::string& field, const std::string& text);

		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_root;
		std::string m_name;
		KeyTool m_tool = KeyTool::ObjectSelection;
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
		std::vector<Rml::ObserverPtr<Rml::Element>> m_menus;
		std::string m_lineMode = "line"; // the line tool's (#128)
		std::unique_ptr<ControllerLanes> m_lanes;
	};

	const char* keyToolName(KeyTool tool);
	std::optional<KeyTool> keyToolFromName(const std::string& name);
}
