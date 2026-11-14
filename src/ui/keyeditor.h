// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"

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
	// notes' time axis fits the parts' range into the width. A drag on the keyboard's left part zooms the pitches
	// vertically and scrolls the note display (reference manual p. 1261). A right click in the note display opens
	// the toolbox #key-editor-toolbox, the toolbar's tools at the mouse, as in the event display (p. 65): with
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
		// Runs when the drum editor's Map menu chooses Drum Map Setup.
		void setDrumMapSetup(std::function<void()> open) { m_drumMapSetup = std::move(open); }
		void setTool(KeyTool tool);

		// The notes' time axis, project ticks across the grid, instead of the parts' range: the in-place editor's
		// (#131, ui/inplaceeditor.h) is the project window's timeline. Without one it fits the parts' range.
		void setTimeRange(std::optional<std::pair<int64_t, int64_t>> range);

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
			std::vector<std::string> articulations; // its attributes (#1112)
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
			Erase,      // the drumstick's press on a note
			ZoomScroll, // the press on the keyboard's left part (p. 1261)
		};

		void ProcessEvent(Rml::Event& event) override;
		void press(Rml::Event& event);
		void drag(Rml::Event& event);
		void release(Rml::Event& event);
		void doubleClick(Rml::Event& event);
		// The drag on the keyboard's left part (p. 1261): to the right or left it zooms the pitches vertically, up or
		// down it scrolls the note display.
		void zoomScroll(Rml::Event& event);
		// Scales the pitch rows' height that the style sheet gives them by the zoom, and with them the keyboard; at 1
		// the rows have the style sheet's height itself. Without a length to scale the zoom stays 1.
		void setPitchZoom(float zoom);
		// The grid of another in-place editor under the event's target, and its track: where a move's release drops
		// the selected notes (p. 1261); null when the target isn't in one or this editor isn't an in-place one.
		std::pair<Rml::Element*, uint64_t> foreignInPlaceGrid(const Rml::Event& event) const;
		// A move released over that grid: the selected notes go to the target track's part at the drop's time, as
		// note.move, which keeps their distances to the pressed one and everything they have; one undo step.
		void moveToInPlace(const Rml::Event& event, Rml::Element& grid, uint64_t track);
		// What a press on a note's box starts: a resize near its ends, else a move.
		Gesture edgeGesture(Rml::Element* target, const Rml::Event& event) const;
		// Starts drawing a note at the snapped position under the press, as the draw tool's press does.
		void startDraw(double ticks);
		// Where a note drawn from the ticks pressed starts: the grid's step it falls in, unsnapped with Snap off.
		int64_t drawStart(double ticks, int pitch) const;
		// The drumstick's note as a drag changes it (p. 1241): where it starts, its length, pitch and velocity, and how
		// many times it repeats at the grid's steps.
		struct DrumDraw
		{
			int64_t start = 0;
			int64_t length = 0;
			int pitch = 0;
			double velocity = 0.0;
			int64_t repeats = 1;
		};
		DrumDraw drumDraw(const Rml::Event& event) const;
		// Ends the drumstick's draw: removes the drawn note and the velocity and inserts the draw's notes.
		void endDrumDraw(const DrumDraw& draw);
		// Plays the pitch on the active track's instrument, as a pressed key of the keyboard does (p. 1204), after
		// ending the pitch played before; without one only ends it.
		void sound(std::optional<int> pitch);
		void toolbar(Rml::Element& target);
		// The note display opens the toolbox on a right click with the user setting ui.toolboxOnRightClick, else on
		// one with a modifier key.
		void showToolbox();

		void render();
		// Places the parts' boxes, the notes and the step input cursor on the time axis m_from to m_to.
		void placeItems();
		void renderInfo();
		void scrollToNotes(); // the first scroll, to the middle of the notes, until one succeeds (m_scrolled)
		// The drum editor's: the active track's drum map, the rows' order from it, and the sound list and Map menu.
		const engine::Json* activeMap() const;
		// The drum editor shows its notes as diamonds, unless Show Note Length is on.
		bool diamonds() const { return m_drum && !m_view.value("showNoteLength", false); }
		void updateOrder();
		std::string soundsRml() const;
		void renderDrumMap();
		void drumMapMenu(Rml::Element& target);
		void editSound(int pitch, const std::string& field, const std::string& text);
		void place(Rml::Element& element, int64_t start, int64_t length, int pitch) const;
		std::string partsRml() const;

		// The note display's point under the mouse: project ticks and pitch.
		std::pair<double, int> at(const Rml::Event& event) const;
		// With a pitch, the drum editor snaps it to its sound's snap while the grid type is drumMap.
		int64_t snap(double ticks, bool off, int pitch = -1) const;
		// How far a gesture moves the pressed note's edge, snapped; a move rows away in the display's order snaps to
		// the snap of the pitch it moves to.
		int64_t delta(Gesture gesture, double ticks, int rows = 0) const;
		int64_t grid(int pitch = -1) const;
		static int64_t soundSnap(const engine::Json& map, int pitch); // a drum sound's snap, in ticks
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
		// At the view's insert velocity unless one is given.
		void addNotes(const Part& part, const std::vector<int64_t>& starts, int64_t length, int pitch,
			std::optional<double> velocity = std::nullopt);
		void transpose(int semitones);
		void editInfo(const std::string& field, const std::string& text);

		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_root;
		std::string m_name;
		std::string m_menuSuffix; // menuIdSuffix's
		bool m_drum = false;
		std::function<void()> m_drumMapSetup;
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
		std::optional<std::pair<int64_t, int64_t>> m_range; // setTimeRange's
		bool m_scrolled = false;
		std::string m_notesRml; // the notes' elements as last set

		Gesture m_gesture = Gesture::None;
		std::pair<double, int> m_pressedAt;
		float m_pressedX = 0.0F;       // the mouse's x at the press, in pixels
		float m_pressedY = 0.0F;       // the mouse's y at the press, in pixels
		std::optional<Note> m_pressed; // the note pressed, as it was
		bool m_snapOff = false;
		float m_pitchZoom = 1.0F;               // the pitch rows' scale, which the keyboard's left part drags (p. 1261)
		float m_zoomStart = 1.0F;               // m_pitchZoom at the press
		float m_scrollStart = 0.0F;             // the note display's scroll top at the press, in pixels
		bool m_zoomDrag = false;                // the drag went past the threshold
		Rml::ObserverPtr<Rml::Element> m_drawn; // the note a draw drags out
		int64_t m_drawnLength = 0;              // its length, at m_pressedAt
		double m_drawnFrom = 0.0;               // the mouse's ticks at the press that started it
		DrumDraw m_drumDrawn;                   // what the drumstick's draw shows, for the mouse leaving the window
		std::optional<std::pair<uint64_t, int>> m_sounding; // the channel and pitch of the key pressed
		Rml::ObserverPtr<Rml::Element> m_document;          // ends the key's pitch on a release anywhere in it
		// the length quantize menu, the line tool's menu, the toolbox, then the drum editor's Map, Pitch Visibility
		// Options and Grid Type menus
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
	// What the ids of an editor view's menus end with, so that several editors share a document: nothing for lower
	// and window, -inPlace-<track> for an in-place editor (#131), whose menus are in the project window's document
	// with the lower zone's and each other's.
	std::string menuIdSuffix(const std::string& view);
}
