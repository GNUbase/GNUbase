// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/cursors.h"
#include "ui/enginebinding.h"

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
	class Context;
	class Element;
}

namespace gnubase::ui
{
	class ControllerLanes;
}

namespace gnubase::time
{
	class TempoMap;
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
		Mute, // on the Articulations lane (#1684); notes have no mute yet (#679)
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

		// read is EngineBinding::handed() when the answer that brought project was handed on, or 0 if unknown, which
		// counts as older than every edit that changed points: the expression editor asks whether its last edit that
		// changed points is in the project (#1795, #1831).
		void show(const engine::Json& project, const engine::Json& view, uint64_t read = 0);

		KeyTool tool() const { return m_tool; }
		bool drum() const { return m_drum; }
		// Runs when the drum editor's Map menu chooses Drum Map Setup.
		void setDrumMapSetup(std::function<void()> open) { m_drumMapSetup = std::move(open); }
		void setTool(KeyTool tool);

		// The notes' time axis, project ticks across the grid, instead of the parts' range: the in-place editor's
		// (#131, ui/inplaceeditor.h) is the project window's timeline, linear in seconds on the tempo map, which must
		// outlive the editor or the next call. Without a range it fits the parts' range, linear in ticks.
		void setTimeRange(std::optional<std::pair<int64_t, int64_t>> range, const time::TempoMap* tempo = nullptr);

		// After the update of a window's context, if the editor is in it: builds the notes' expression lines again
		// when a pixel has other ticks than they were built with, as after the editor's first layout and when its
		// window or zone changes its width (#1671). Lines that an update laid out with their own ticks only for a
		// quarter more or fewer (#1716), others for any other ticks (#1768). True when it built them, so that the
		// window updates again (Shell::afterUpdate). The first scroll to the notes waits for that layout too, and
		// is true when it scrolled. It places the expression editor again for a grid of another width, and is true
		// then.
		bool fitLines(const Rml::Context& updated);

		// Scrolls the note display so that the pitch is in the middle; false before the display has its size. The
		// display scrolls to the middle of its notes, or to middle C, when it first has a size, unless it was
		// scrolled to a pitch before.
		bool scrollToPitch(int pitch);

		// The editor's element, its focus zone keyEditor: the key commands keyEditor.* act on the editor whose element
		// is the zone they were pressed in (app::AppUi::addMidiFunctions, #679).
		const Rml::Element* zone() const { return m_root.get(); }
		// Transposes the selected notes, all or none (the transpose palette, keyEditor.transposeUp and the others).
		void transpose(int semitones);
		// Deletes the selected notes (keyEditor.delete).
		void removeSelected();

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
			std::vector<std::string> articulations;           // its attributes (#1112)
			engine::Json expressions = engine::Json::array(); // its expression points (#176), by position
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
			Expression, // the draw or erase tool in the expression editor (pp. 1310-1311)
		};

		void ProcessEvent(Rml::Event& event) override;
		void press(Rml::Event& event);
		void drag(Rml::Event& event);
		// The selected notes where a move or resize has taken them, at the time and pitch now under m_draggedTo: after
		// every mouse move, when the display shows the project again during the gesture (#1789) and when an in-place
		// editor's range changes during it (#1808).
		void placeDragged();
		void release(Rml::Event& event);
		void doubleClick(Rml::Event& event);
		// The drag on the keyboard's left part (p. 1261): to the right or left it zooms the pitches vertically, up or
		// down it scrolls the note display.
		void zoomScroll(Rml::Event& event);
		// Scales the pitch rows' height that the style sheet gives them by the zoom, and with them the keyboard; at 1
		// the rows have the style sheet's height itself. Without a length to scale the zoom stays 1.
		void setPitchZoom(float zoom);
		// The button went up outside the editor (dragend): the draws, the drumstick's deletion and the keyboard's drag
		// end as in it, a move over another in-place editor's grid drops the notes there, the others end without their
		// effect.
		void releaseOutside(Rml::Event& event);
		// Ends the gesture without its effect: the drawn note goes, the notes go back where they are.
		void cancelGesture();
		// During a gesture the grid and the keyboard's left part are RmlUi drag elements (.in-gesture), whose drag
		// events come wherever the mouse is, also outside the window, and whose dragend comes at the release.
		void dragDuringGesture();
		// The grid of another in-place editor under the element, and its track: where a move's release drops the
		// selected notes (p. 1261); null when the element isn't in one or this editor isn't an in-place one.
		std::pair<Rml::Element*, uint64_t> foreignInPlaceGrid(Rml::Element* hovered) const;
		// A move released over that grid: the selected notes and controller events go to the target track's part at
		// the drop's time, as note.move, which keeps their distances to the pressed note and everything they have;
		// one undo step. A part that gives only controller events sends a note.move with no notes.
		void moveToInPlace(const Rml::Event& event, Rml::Element& grid, uint64_t track);
		// What a press on a note's box starts: a resize near its ends, else a move.
		Gesture edgeGesture(Rml::Element* target, float mouseX) const;
		// The note display's pointer for where the mouse is (docs/features/pointers.md), over the element at the x in
		// the window, with the modifiers held: resize where a press resizes a note, else the tool's.
		void showPointer(Rml::Element& target, float mouseX, HeldModifiers keys);
		// Starts drawing a note at the snapped position under the press, as the draw tool's press does.
		void startDraw(double ticks);
		// Takes the drawn note out of the grid, into m_removedDrawn.
		void removeDrawn();
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
		// Plays the pitch at the MIDI velocity on the active track's instrument, as a pressed key of the keyboard
		// does (p. 1204), after ending the pitch played before; without one only ends it.
		void sound(std::optional<int> pitch, int velocity = 100);
		void toolbar(Rml::Element& target);
		// The note display opens the toolbox on a right click with the user setting ui.toolboxOnRightClick, else on
		// one with a modifier key.
		void showToolbox();

		void render();
		// Places the parts' boxes, the notes, the step input cursor and the expression editor on the time axis m_from
		// to m_to.
		void placeItems();
		// The expression editor alone, as its gesture changes it.
		void placeExpression();
		// Where a time lies across the grid, 0 at its left edge and 1 at its right (axisFraction).
		double fraction(int64_t ticks) const;
		// The ticks of a pixel of the note display, or 0 before its layout.
		int64_t ticksPerPixel() const;
		// Whether the notes' expression lines or the expression editor show, whose lines fitLines fits.
		bool linesShown() const;
		// fitLines on the note display's box as it is: laidOut when its window's update has just laid it out, which
		// keeps the lines; not when placeItems() places what render() built, maybe for a box of an earlier layout.
		bool refitLines(bool laidOut);
		void renderInfo();
		// Per-note expression (#1512, pp. 1303-1313): the steps of a note's points of the type, as lines at their
		// values in percent of a span of ticks that starts offset ticks before the note. Steps narrower than minimum
		// ticks join the next ones.
		static std::string stepsRml(const engine::Json& points, const std::string& type, int64_t offset, int64_t span,
			int64_t length, int64_t minimum);
		// The expression editor: open on the selected notes, from the first one's start to the last one's end, it
		// shows their points of the view's expressionType.
		void renderExpression();
		std::pair<int64_t, int64_t> expressionSpan() const;
		// Whether the note expression setup of a part's track hides an expression (#2120, p. 1301).
		bool expressionHidden(uint64_t part, const std::string& type) const;
		// The draw tool's points or the erase tool's sweep at the mouse in the expression editor.
		void paint(const Rml::Event& event);
		// Ends its gesture: the selected notes it reached get the points it drew in the range it swept, as
		// note.replaceExpression, which keeps the others as the engine has them (#1795).
		void endExpression();
		// A note's points as the gesture in the expression editor changes them: the swept range's points of the
		// type replaced by those drawn there.
		engine::Json edited(const Note& note) const;
		// The points the gesture drew on a note, {position, value} each from the note's start: one where the value
		// changes, not one per mouse position (#1655).
		engine::Json drawn(const Note& note) const;
		void scrollToNotes(); // the first scroll, to the middle of the notes, until one succeeds (m_scrolled)
		// The drum editor's: the active track's drum map, the rows' order from it, and the sound list and Map menu.
		const engine::Json* activeMap() const;
		engine::Json activeTrack() const; // as project.get lists it, or an empty object
		// The names of the active track's drum sounds, by pitch: its drum map's, else with the GM Default name list
		// (#1493, p. 1239) General MIDI's, else empty.
		std::vector<std::string> soundNames() const;
		// The drum editor shows its notes as diamonds, unless Show Note Length is on.
		bool diamonds() const { return m_drum && !m_view.value("showNoteLength", false); }
		void updateOrder();
		// Whether the drum sound list shows the pitch: Show Drum Sounds with Events and in Use by Instrument hide
		// sounds, and their notes.
		bool listed(int pitch) const { return m_rowOf.at(static_cast<size_t>(pitch)) >= 0; }
		std::string soundsRml() const;
		void renderDrumMap();
		void drumMapMenu(Rml::Element& target);
		void editSound(int pitch, const std::string& field, const std::string& text);
		void place(Rml::Element& element, int64_t start, int64_t length, int pitch) const;
		std::string partsRml() const;
		void noteColors(const std::string& colors, const engine::Json& part, const engine::Json& track,
			const std::map<uint64_t, const engine::Json*>& maps);

		// The note display's point under the mouse: project ticks and pitch.
		std::pair<double, int> at(const Rml::Event& event) const;
		std::pair<double, int> at(float mouseX, float mouseY) const; // the mouse's point in the window, in pixels
		// With a pitch, the drum editor snaps it to its sound's snap while the grid type is drumMap.
		int64_t snap(double ticks, bool off, int pitch = -1) const;
		// How far a gesture moves the pressed note's edge, snapped; a move rows away in the display's order snaps to
		// the snap of the pitch it moves to.
		int64_t delta(Gesture gesture, double ticks, int rows = 0) const;
		int64_t grid(int pitch = -1) const;
		int64_t quantizeGrid() const;                                 // the project's quantize grid (quantize.set)
		static int64_t soundSnap(const engine::Json& map, int pitch); // a drum sound's snap, in ticks
		// The length of new notes of the pitch: with Drum-Map Link its sound's snap.
		int64_t lengthQuantize(int pitch) const;
		// Indicate Transpositions (#1766, reference manual pp. 419, 1260): the semitones the view's transpositions give
		// a part's notes at the ticks, 0 while it is off and in the drum editor. The note display shows each note at
		// its pitch shown, which a drawn note's pitch has under the mouse; edits move the pitch by the rows dragged.
		int transposition(uint64_t part, int64_t ticks) const;
		int shownPitch(const Note& note) const;
		// The pitch rows away from pitch in the display's order, or -1 beyond its ends.
		int shifted(int pitch, int rows) const;
		const Note* note(uint64_t part, uint64_t id) const;
		const Note* noteOf(Rml::Element* element) const;
		// The editable part at the ticks: the active one if it is there, else the first.
		const Part* partAt(int64_t ticks) const;
		bool selected(const Note& note) const;
		std::vector<const Note*> selectedNotes() const;

		void select(std::vector<std::pair<uint64_t, uint64_t>> notes);
		// The view's selected notes, parts and notes.
		std::vector<std::pair<uint64_t, uint64_t>> viewSelection() const;
		// The notes the display shows selected as editor.set takes them: those that stay selected keep the view's
		// attributes selected of them on the Articulations lane (#1762).
		engine::Json selectionJson() const;
		// Shows the controller lanes for the view, with the notes the display shows selected: while a click's
		// selection is unanswered, the click's, so that a lane edit acts on those (#1897).
		void showLanes();
		void setView(engine::Json parameters);
		// Sends note.* commands, each {command, parameters}: one alone, several in a history.transaction. done, unless
		// empty, runs with the result. The description is the engine's name for the commands' undo step, such as
		// "Move notes", so that a gesture's step has one name whether it changes one part or several (#2014).
		void edit(const std::string& description, engine::Json commands,
			std::function<void(const engine::Result&)> done = {});
		// Whether the project shown may lack an edit that the expression editor sent: one is out, or the last one that
		// changed points was answered after the read of the project shown. An edit that changed nothing isn't missing.
		bool expressionBehind() const;
		// Notes of the pitch at the starts, each of the length.
		// At the view's insert velocity unless one is given.
		void addNotes(const Part& part, const std::vector<int64_t>& starts, int64_t length, int pitch,
			std::optional<double> velocity = std::nullopt);
		void editInfo(const std::string& field, const std::string& text);
		// The articulation map of a note's track, or null.
		const engine::Json* articulationMap(const Note& note) const;
		// The info line's pop-up menu of the attributes (#1580, p. 1290): an attribute chosen goes to every selected
		// note of a track with the first selected note's map in place of its group's, or goes from them where the first
		// selected note has it.
		void chooseAttribute(const std::string& name);

		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_root;
		std::string m_name;
		std::string m_menuSuffix; // menuIdSuffix's
		bool m_drum = false;
		std::function<void()> m_drumMapSetup;
		KeyTool m_tool = KeyTool::ObjectSelection;
		std::vector<int> m_order;          // the pitches of the rows, from the top
		std::vector<int> m_rowOf;          // each pitch's row, -1 for one the drum sound list doesn't show
		std::string m_soundsRml;           // the drum sound list as last set
		engine::Json m_keyNames;           // the view's soundNames that the key editor's keys show
		std::string m_mapMenuRml;          // the Map menu as last set
		std::string m_articulationMenuRml; // the info line's attributes menu as last set
		engine::Json m_project = engine::Json::object();
		engine::Json m_view = engine::Json::object();
		std::vector<Part> m_parts;
		std::vector<Note> m_notes;
		std::vector<std::pair<uint64_t, uint64_t>> m_selection;
		int m_selectionsOut = 0; // select()'s editor.set commands not answered yet: show() keeps m_selection then
		int64_t m_from = 0;      // the notes' time axis, project ticks
		int64_t m_to = 1;
		std::optional<std::pair<int64_t, int64_t>> m_range; // setTimeRange's
		const time::TempoMap* m_tempo = nullptr;            // and its tempo map
		bool m_scrolled = false;
		std::string m_notesRml; // the notes' elements as last set
		// The notes' colors that the Event Colors menu chooses (pp. 1133-1134), by part and note: none for the style
		// sheets' color.
		std::map<std::pair<uint64_t, uint64_t>, std::string> m_noteColors;
		std::string m_expressionRml;                                    // the expression editor's lines as last set
		std::set<std::pair<uint64_t, std::string>> m_hiddenExpressions; // by part, as its track's setup hides them
		std::string m_expressionItems;                                  // Parameter Selection's items as last set

		Gesture m_gesture = Gesture::None;
		std::pair<double, int> m_pressedAt;
		float m_pressedX = 0.0F;                            // the mouse's x at the press, in pixels
		float m_pressedY = 0.0F;                            // the mouse's y at the press, in pixels
		std::optional<Note> m_pressed;                      // the note pressed, as it was
		std::optional<std::pair<float, float>> m_draggedTo; // the mouse at a move's or resize's last move, in pixels
		bool m_snapOff = false;
		bool m_dragged = false;     // a drag event came since the press: the mouse leaving the window keeps the gesture
		float m_pitchZoom = 1.0F;   // the pitch rows' scale, which the keyboard's left part drags (p. 1261)
		float m_zoomStart = 1.0F;   // m_pitchZoom at the press
		float m_scrollStart = 0.0F; // the note display's scroll top at the press, in pixels
		bool m_zoomDrag = false;    // the drag went past the threshold
		Rml::ObserverPtr<Rml::Element> m_drawn; // the note a draw drags out
		// The drawn note taken out of the grid, kept until the next one goes: the mouse event that ends the draw may
		// have it as its target, which the listeners after this one still read.
		Rml::ElementPtr m_removedDrawn;
		int64_t m_drawnLength = 0;                          // its length, at m_pressedAt
		double m_drawnFrom = 0.0;                           // the mouse's ticks at the press that started it
		std::optional<std::pair<uint64_t, int>> m_sounding; // the channel and pitch of the key pressed
		Rml::ObserverPtr<Rml::Element> m_document;          // ends the key's pitch on a release anywhere in it
		// the length quantize menu, the line tool's menu, the toolbox, then the drum editor's Map, Pitch Visibility
		// Options, Grid Type and Names menus
		std::vector<Rml::ObserverPtr<Rml::Element>> m_menus;
		std::string m_lineMode = "line"; // the line tool's (#128)
		std::unique_ptr<ControllerLanes> m_lanes;
		float m_mouseX = 0.0F;         // the last mouse move's x in the window, for showPointer while the mouse rests
		bool m_expressionOpen = false; // the expression editor (p. 1308)
		std::pair<uint64_t, uint64_t> m_expressionNote; // the note double-clicked to open it: part, id
		float m_expressionWide = 0.0F;       // the grid's width placeExpression placed it for, which fitLines follows
		std::map<int64_t, double> m_painted; // the draw tool's points in it, project ticks
		int64_t m_paintFrom = 0;             // the range it swept, project ticks
		int64_t m_paintTo = -1;
		uint64_t m_read = 0;                // show()'s read
		uint64_t m_expressionEdits = 0;     // the expression editor's edits sent
		uint64_t m_expressionAnswered = 0;  // and answered
		uint64_t m_expressionChanged = 0;   // EngineBinding::handed() at the last answer that changed points, 0 before
		int64_t m_linesPixel = -1;          // the ticks per pixel the notes' expression lines were built with
		bool m_linesKept = false;           // an update laid the notes' lines out with those ticks (refitLines)
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
