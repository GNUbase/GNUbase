// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/snap.h"
#include "engine/engine.h"
#include "engine/midi/quantizesettings.h"
#include "ui/canvas.h"
#include "ui/enginebinding.h"
#include "ui/eventwaveforms.h"
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

// The event display (#84): the project's events on a timeline under a ruler and an overview line, with the project
// cursor, a cross-hair cursor, zoom and scrolling. docs/features/event-display.md describes it.
namespace gnubase::ui
{
	// A track's height before vertical zoom, in dp, until it gets one of its own (setTrackHeight), and the limits.
	inline constexpr double g_trackHeight = 40.0;
	inline constexpr double g_minTrackHeight = 20.0;
	inline constexpr double g_maxTrackHeight = 400.0;
	// The project setup's length until the project is read, in seconds (#78).
	inline constexpr double g_defaultProjectLength = 600.0;

	// The part of the timeline the event display shows.
	struct TimelineView
	{
		double start = 0.0;     // seconds from the project start at the left edge
		double scale = 0.01;    // seconds per dp
		double top = 0.0;       // dp scrolled down
		double trackZoom = 1.0; // vertical zoom: track heights times this
	};

	// How auto-scroll keeps the project cursor in view while playing (reference manual p. 316).
	enum class AutoScrollMode : uint8_t
	{
		Page,       // the view turns a page when the cursor leaves it
		Stationary, // the cursor stays in the middle and the timeline moves
	};

	// The project window's tools (#86, reference manual pp. 57-65), in the toolbar's order: what a press in the event
	// lanes does. docs/features/tools.md describes them.
	enum class Tool : uint8_t
	{
		ObjectSelection,
		RangeSelection,
		Draw,
		Erase,
		Split,
		Glue,
		Mute,
		Zoom,
		Line,
		Play,
	};
	// The tool's name, as the display's attribute tool and the toolbar's buttons name it: objectSelection, split, ...
	const char* toolName(Tool tool);
	std::optional<Tool> toolFromName(const std::string& name);

	// The line tool's modes (reference manual p. 873), for automation tracks (#121).
	enum class LineMode : uint8_t
	{
		Line,
		Parabola,
		Sine,
		Triangle,
		Square,
	};

	// The play tool's modes (reference manual pp. 252-253): auditioning the pressed track, or scrubbing it.
	enum class PlayMode : uint8_t
	{
		Play,
		Scrub,
	};

	// A selection range (reference manual p. 285): from start to end in project samples, on these tracks (by id, in
	// the project's order). A click makes one of no length.
	struct RangeSelection
	{
		int64_t start = 0;
		int64_t end = 0;
		std::vector<uint64_t> tracks;
	};

	// Fills an element of a document with the event display and follows the project through the engine binding: an
	// overview line, the ruler, a lane of events per track, the project cursor and the cross-hair cursor, classed for
	// the style sheets. While playing it reads the playback position once a frame. Use it on the UI thread; the
	// engine and the binding must outlive it. If the element goes, e.g. when live reload loads its document again,
	// the display shows nothing more.
	//
	// The document may have the display's menus and buttons, which the display then drives (eventdisplaymenus.cpp,
	// resources/ui/project.rml): a ruler time format menu #ruler-format-menu, which a right click in the ruler opens
	// too; a horizontal zoom presets menu #zoom-presets-menu, whose Add and Organize items open the dialogs
	// zoom-preset.rml and zoom-presets.rml next to the document; an auto-scroll toggle button #auto-scroll, with the
	// state class .suspended while editing suspends it; and an auto-scroll settings menu #auto-scroll-menu. Its third
	// dialog, repeat-events.rml next to the document too, opens with edit.repeat.
	class EventDisplay final : private Rml::EventListener
	{
	public:
		EventDisplay(engine::Engine& engine, EngineBinding& binding, Rml::Element& root);
		~EventDisplay() override;
		EventDisplay(const EventDisplay&) = delete;
		EventDisplay& operator=(const EventDisplay&) = delete;
		EventDisplay(EventDisplay&&) = delete;
		EventDisplay& operator=(EventDisplay&&) = delete;

		const TimelineView& view() const { return m_view; }
		// The project cursor, in seconds.
		double cursor() const { return m_cursor; }
		// The seconds that the event lanes show across.
		double visibleLength() const;
		// From the project start to the end of the project setup's length or of its last event, if that is later.
		double projectLength() const;

		// Zoom (reference manual pp. 101-104). Every zoom is a step in the zoom history; scrolling isn't.
		void zoomIn(); // one step, around the project cursor, or onto it when it is out of view
		void zoomOut();
		void zoomInVertically();
		void zoomOutVertically();
		void zoomFull();
		void zoomTo(double from, double to); // seconds, e.g. a cycle marker's range
		bool undoZoom();
		bool redoZoom();

		// Scrolling: the seconds at the left edge, the dp scrolled down.
		void scrollTo(double start);
		void scrollVertically(double top);

		// The lanes, which the track list (ui/tracklist.h) shows beside them: a lane per track that isn't hidden, in
		// the project's order. Heights are per track, before vertical zoom; lane tops count from the top of the first
		// lane, in dp, with vertical zoom, and the last one is the bottom of the last lane.
		const std::vector<uint64_t>& laneTracks() const { return m_laneTracks; }
		const std::vector<double>& laneTops() const { return m_laneTops; }
		double trackHeight(uint64_t track) const;
		void setTrackHeight(uint64_t track, double height);
		void setTrackHeights(double height); // every track's
		void setHiddenTracks(std::set<uint64_t> tracks);
		// The project as project.get last returned it, or null before the first answer; loads() counts the answers.
		const engine::Json& project() const { return m_project; }
		uint64_t loads() const { return m_loads; }
		// Folder tracks' data blocks (reference manual p. 226, docs/features/folder-tracks.md): per lane, the events
		// and parts of the tracks inside a folder's lane, by the user setting ui.folderData (always, never, or
		// hideWhenExpanded: while the folder is folded); empty for the other lanes.
		const std::vector<EventLanes::Lane>& folderData() const { return m_folderData; }
		// Called after the lanes or the view changed.
		void onLayout(std::function<void()> listener) { m_layoutListener = std::move(listener); }

		// Auto-scroll (reference manual pp. 316-317). Suspended when editing: a click in the event display while
		// playing suspends it until playback stops or auto-scroll is switched on again.
		void setAutoScroll(bool on);
		bool autoScroll() const { return m_autoScroll; }
		void setAutoScrollMode(AutoScrollMode mode);
		AutoScrollMode autoScrollMode() const { return m_autoScrollMode; }
		void setSuspendAutoScrollWhenEditing(bool suspend);
		bool suspendAutoScrollWhenEditing() const { return m_suspendWhenEditing; }
		bool autoScrollSuspended() const { return m_suspended; }

		// The ruler's time format, and with it the snap grid's (snap.set's format).
		void setRulerFormat(TimeFormat format);
		TimeFormat rulerFormat() const { return m_format; }
		void setCrossHair(bool shown);

		// Horizontal zoom presets: user settings for every project (ui.zoomPresets), each a name and the seconds the
		// view shows. Applying one keeps the left edge.
		void addZoomPreset(const std::string& name);
		bool applyZoomPreset(const std::string& name);
		void removeZoomPreset(const std::string& name);
		void renameZoomPreset(const std::string& from, const std::string& to);
		const engine::Json& zoomPresets() const { return m_presets; }

		// Editing parts and events (#85, eventediting.cpp, docs/features/parts-and-events.md). The selection is the
		// display's, not the project's: audio event and MIDI part ids, ascending, also in the root's attribute
		// selection. Edits go to the engine as event.* commands; a click selects, a drag moves once the press is held
		// for the drag delay, Alt and a drag copies (with Shift too, shared copies), Ctrl (Cmd) while dragging keeps
		// a move or copy across or up and down, a drag at an event's left or right edge resizes it, Alt and a click
		// splits there, and a double click on an empty MIDI or instrument lane adds a one-bar part, or one between
		// the locators when the click is between them; one below the tracks adds a MIDI track.
		const std::vector<uint64_t>& selection() const { return m_selection; }
		void select(std::vector<uint64_t> events);
		// Selects the first event on the track's lane, by start; with none there or the track not shown, the
		// selection stays (Up and Down while an event is selected, reference manual p. 218, ui/tracklist.h).
		void selectFirstOn(uint64_t track);
		// Sizing moves contents instead of normal sizing when resizing with the mouse (reference manual p. 265).
		void setSizingMovesContents(bool moves);
		bool sizingMovesContents() const { return m_sizingMovesContents; }
		// The selection count times one after the other; shared copies of MIDI parts with shared.
		void repeat(int count, bool shared);

		// Snap and grid (#87, docs/features/snap.md): the project's settings (snap.get), which moving, copying,
		// resizing and splitting with the mouse follow, and the nudges' step. The document's toolbar may have a snap
		// toggle button #snap, menus #snap-type-menu and #grid-type-menu (the grid types of the ruler's time format),
		// whose buttons #snap-type and #grid-type show the choice, and a toggle button #snap-zero-crossing.
		const engine::SnapSettings& snap() const { return m_snap; }

		// The tools (#86, eventtools.cpp, docs/features/tools.md), also in the root's attribute tool. The object
		// selection tool edits as above; the others send the same event.* commands (split, glue, erase, mute), zoom,
		// draw MIDI parts, audition or scrub the pressed audio track, or select a range. With the selection tools
		// combined, the object selection tool selects ranges in a lane's upper half. Their tool modifiers are the user
		// setting ui.toolModifiers. The document's toolbar may have a button per tool, button[tool=<name>], whose
		// attribute tool-menu names a menu of its modes that a second click opens (#object-selection-menu with the
		// sizing modes, #line-menu with the line modes, #play-menu with the play modes), and a toggle button
		// #combine-selection-tools.
		void setTool(Tool tool);
		Tool tool() const { return m_tool; }
		void setCombineSelectionTools(bool on);
		bool combineSelectionTools() const { return m_combine; }
		void setLineMode(LineMode mode);
		LineMode lineMode() const { return m_lineMode; }
		void setPlayMode(PlayMode mode);
		PlayMode playMode() const { return m_playMode; }
		// The selection range, also in the root's attributes range ("start end") and range-tracks (ids); none
		// without the range selection tool. Choosing another tool selects the events in the range instead.
		const std::optional<RangeSelection>& range() const { return m_range; }
		void setRange(std::optional<RangeSelection> range);
		// Importing files (#92, fileimport.cpp, docs/features/file-import.md). MIDI files (.mid, .midi, .smf) are
		// imported with midiFile.import. Audio files open the Import Options dialog (import-options.rml next to the
		// document), which imports them with audioFile.import at sample (default: the project cursor), one after
		// another onto track if it is an audio track, else onto new tracks. Files dropped on the display go to the
		// track of the lane under the mouse, at the time there. With clipsOnly, audio files only go into the pool
		// (#93), without tracks or events.
		void importFiles(const std::vector<std::string>& files, std::optional<uint64_t> track = std::nullopt,
			std::optional<int64_t> sample = std::nullopt, bool clipsOnly = false);
		// File > Import (#480, reference manual p. 360): at the project cursor, onto the first of the selected tracks
		// (the track list's, top to bottom) if it is an audio track: the first file, or all of them one after another
		// with Import Files on One Track, and the others onto new tracks. Without one, onto new tracks; with Split
		// Channels, every channel onto a new track of its own.
		void importToSelection(const std::vector<std::string>& files, const std::vector<uint64_t>& selection);

		// The commands that key commands bind to, by id: zoom.in, zoom.out, zoom.inVertically, zoom.outVertically,
		// zoom.full, zoom.undo, zoom.redo and view.autoScroll (on or off); locate.nudgeCursorLeft and
		// locate.nudgeCursorRight move the project cursor by a grid step; and the edits of parts and events:
		// edit.delete, edit.mute, edit.unmute, edit.lock, edit.unlock, edit.splitAtCursor, edit.splitLoop, edit.glue,
		// edit.duplicate, edit.repeat (opens the Repeat Events dialog), edit.fillLoop, edit.toRealCopy, edit.nudgeLeft,
		// edit.nudgeRight, edit.nudgeStartLeft, edit.nudgeStartRight, edit.nudgeEndLeft, edit.nudgeEndRight,
		// edit.moveStartToCursor, edit.moveEndToCursor, edit.selectAll, edit.selectNone, edit.invertSelection,
		// edit.selectInLoop, edit.selectFromStartToCursor, edit.selectFromCursorToEnd, edit.selectUnderCursor, and
		// Left and Right's edit.selectPrevious and edit.selectNext; and edit.locatorsToSelection sets the locators
		// around the selected events. The Fades menu (#138, docs/features/fades.md): audio.fadeInToRange,
		// audio.fadeOutFromRange and audio.adjustFadesToRange on the selection range, audio.crossfade (crossfades the
		// selected audio events, or opens the crossfade editor for a crossfade they have), audio.removeFadeIn,
		// audio.removeFadeOut, audio.removeFades and audio.fadeEditor (the fade dialog) on the selected audio events.
		// With the range selection tool,
		// edit.delete deletes the range; edit.cut, edit.copy, edit.cutTime, edit.deleteTime, edit.insertSilence,
		// edit.splitRange and edit.cropRange edit it, edit.paste and edit.pasteTime paste at its start (without a
		// range at the project cursor) and edit.globalCopy copies the locator range on every track.
		// edit.snap (snap on or off) and edit.snapPointToCursor (the selected audio events' snap points); and the
		// tools: tool.objectSelection, tool.rangeSelection, tool.draw, tool.erase, tool.split, tool.glue, tool.mute,
		// tool.zoom, tool.line, tool.play, tool.next, tool.previous and tool.combineSelectionTools (on or off).
		const std::map<std::string, std::function<void()>>& actions() const { return m_actions; }

		// Runs listener after every change of the view, for what others draw on the timeline, such as marker lines
		// (ui/markers.h).
		void onViewChanged(std::function<void()> listener) { m_viewListeners.push_back(std::move(listener)); }
		// The element around the event lanes, which they fill.
		Rml::Element& eventArea() const { return *m_area; }

	private:
		enum class Drag : uint8_t
		{
			None,
			Ruler,         // zooms with the distance dragged down or up
			OverviewMove,  // scrolls
			OverviewStart, // the rectangle's left edge
			OverviewEnd,   // its right edge
			Events,        // moves or copies the selection
			ResizeStart,   // the start of the selection
			ResizeEnd,     // its end
			Rectangle,     // a rectangle that selects, mutes or zooms
			Range,         // a selection range
			DrawPart,      // a new MIDI part
			Play,          // auditions or scrubs until the mouse is released
			Click,         // splits, glues or erases where it is released, unless it moved
			FadeIn,        // the fade in handle of the pressed audio event
			FadeOut,       // its fade out handle
		};

		// An audio event or a MIDI part as the display shows it.
		struct Shown
		{
			uint64_t id = 0;
			size_t lane = 0;
			double start = 0.0; // seconds
			double end = 0.0;
			bool mute = false;
			std::optional<double> snapPoint; // audio events, inside them, in seconds
		};

		void ProcessEvent(Rml::Event& event) override;
		void mouseDown(Rml::Event& event, Rml::Element& target);
		void mouseMove(Rml::Event& event);
		void wheel(Rml::Event& event);

		// Elements live while the root does.
		bool alive() const { return static_cast<bool>(m_root); }
		float dpRatio() const;
		double width() const;  // of the event lanes, in dp
		double height() const; // of the event lanes, in dp
		double overviewLength() const;
		// x in the window's pixels as dp from the left of an element.
		double dpFrom(Rml::Element& element, float x) const;

		void requestProject();
		void loadProject(const engine::Json& project);
		void setFolderMode(const std::string& mode); // ui.folderData
		void setLanes();
		void followTransport(const engine::Json& status);
		void setCursor(int64_t sample);
		void follow();

		// Zoom steps go into the history first.
		void remember();
		void zoomAround(double factor, double time, double x);
		void zoomVertically(double factor);
		void setView(TimelineView view);
		void apply();
		void savePresets();

		// The document's menus, buttons and dialogs for the display (eventdisplaymenus.cpp).
		struct CycleMarker
		{
			std::string name;
			int64_t start = 0; // samples
			int64_t end = 0;
		};
		void findControls();
		void requestMarkers();
		void click(Rml::Element* target);
		void dialogEvent(Rml::Event& event);
		void openDialog(const char* file);
		void showFormat();
		void showRuler();
		void takeSnap(const engine::Json& settings); // from snap.changed or project.get
		void followSnapFormat();                     // the ruler shows the snap settings' format
		void showPresets();
		void showAutoScroll();
		void showSnap();
		void setSnap(const engine::Json& settings); // sends snap.set

		// Importing files (fileimport.cpp).
		struct Import
		{
			std::vector<std::string> files; // audio files, waiting for the Import Options dialog
			std::optional<uint64_t> track;
			int64_t sample = 0;
			bool clipsOnly = false;
			bool selected = false; // track is the selected track, which takes the first file, not all of them
		};
		void startImport(const std::vector<std::string>& files, Import import);
		void dropFiles(Rml::Event& event);
		void openImportOptions(bool saved);
		void importAudio(Rml::Element& dialog);

		// Editing parts and events (eventediting.cpp).
		void addEditActions();
		const Shown* eventAt(double x, double y) const; // dp in the event lanes
		size_t laneAt(double y) const; // dp from the top of the first lane; 0 above it, the lane count below the last
		void areaDown(Rml::Event& event, double x, double y);
		void areaMove(double dx, double dy, bool constrained); // constrained: restricts() says so
		// Whether the keys are exactly the tool modifier of Copy or Shared Copy.
		bool copyKeys(const std::string& keys) const;
		// Whether the keys held restrict a drag of events: Ctrl (Cmd), unless they are a copy's tool modifier.
		bool restricts(Rml::Event& event) const;
		void areaUp(Rml::Event& event);
		// A drag dx and dy dp from the press, across or up and down only when restricted.
		static std::pair<double, double> restricted(double dx, double dy, bool constrained);
		// How many lanes a drag dy dp down moves the selection: none unless every selected event lands on a lane of
		// a track of its type.
		int laneShift(double dy) const;
		// Sends the move or copy of the selection: the shift's parameters (event.move's delta or ticks), lanes down.
		void moveSelection(engine::Json shift, int lanes, bool copy, bool shared);
		void doubleClick(Rml::Event& event);
		void showSelection();
		// Sends an event.* command for the selection; with selectResult, the events it returns become the selection.
		void edit(const std::string& command, engine::Json parameters, bool selectResult = false);
		void selectWhere(const std::function<bool(const Shown&)>& selected);
		int64_t samples(double seconds) const;
		// Snapping (eventediting.cpp): where a point at seconds goes, from where it was for grid relative; the
		// selection's edges and snap points don't attract.
		engine::SnapGrid snapGrid() const;
		time::Position snapAt(double seconds, std::optional<double> from) const;
		// The distance from seconds to a snapped position as event.move and event.resize take it: ticks when the grid
		// is musical and every selected event is on a musical track, else samples. Also in seconds.
		std::pair<engine::Json, double> shiftTo(const time::Position& to, double from) const;
		std::pair<engine::Json, double> moveShift(double dx) const; // a drag of the selection, dx dp across
		// Left and Right (reference manual p. 253): the event before (-1) or after (1) the selected ones on the lane.
		void selectNeighbour(int direction);
		// Adds a MIDI part on the lane from start to end, ticks, and selects it.
		void addPart(size_t lane, time::Ticks start, time::Ticks end);
		// The lane at y dp down the event lanes, if there is one there.
		std::optional<size_t> visibleLaneAt(double y) const;

		// Fades and crossfades (eventfades.cpp): an audio event's fades and crossfade as project.get has them, its
		// media file's sample rate and its length in clip frames.
		struct AudioFades
		{
			double rate = 0.0;
			int64_t length = 0;
			engine::Fade in;
			engine::Fade out;
			engine::Crossfade crossfade;
		};
		void addFadeActions();
		void readFades(const engine::Json& event, double rate);
		// Starts dragging a fade handle when x and y dp in the event lanes are at one of the pressed event's.
		bool fadeDown(const Shown& shown, double x, double y);
		void fadeUp();
		// The fade length in seconds that a drag of a handle gives an event's fade in or fade out.
		double draggedFade(uint64_t event, bool in) const;
		void showFades(); // the fade lines and the selected audio events' handles
		// A double click on an event at seconds: in a crossfade's zone, opens the crossfade editor.
		void openCrossfadeAt(const Shown& shown, double seconds);
		// The selected audio events, by id.
		std::vector<uint64_t> selectedAudio() const;
		// Opens a dialog of the display's document folder whose Lua reads the attributes when it shows.
		void openFadeDialog(const char* file, const std::vector<std::pair<std::string, std::string>>& attributes);

		// The tools (eventtools.cpp).
		void addToolActions();
		void showTools();
		// Whether the pressed modifiers are the ones the user setting ui.toolModifiers gives the action.
		bool modifiers(const char* action) const;
		// What a press at x and y dp in the event lanes does with the tool, unless the object selection tool takes it.
		bool toolDown(double x, double y);
		void toolMove(double x, double y);
		void toolUp(double x, double y);
		bool toolDoubleClick(double x, double y); // whether it took the double click
		void setRangeFrom(double x, double y);    // from the drag's start
		void selectRange();                       // the events in it, when another tool takes over
		void showRange();
		// The rectangle that a tool drags, dp in the event lanes; hidden when not shown.
		void showRectangle(bool shown, double left = 0.0, double top = 0.0, double right = 0.0, double bottom = 0.0);
		void setToolModifiers(const engine::Json& value);

		engine::Engine& m_engine;
		EngineBinding& m_binding;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for the binding's answers after it went
		Rml::ObserverPtr<Rml::Element> m_root;
		Rml::Element* m_document = nullptr;
		Rml::Element* m_overview = nullptr;
		EventLanes* m_overviewEvents = nullptr;
		Rml::Element* m_overviewView = nullptr;
		Ruler* m_ruler = nullptr;
		Rml::Element* m_area = nullptr;
		EventLanes* m_lanes = nullptr;
		std::unique_ptr<EventWaveforms> m_waveforms; // over the lanes
		Rml::Element* m_crossVertical = nullptr;
		Rml::Element* m_crossHorizontal = nullptr;
		Rml::Element* m_cursorLine = nullptr;

		TimeContext m_time;
		std::vector<EventLanes::Lane> m_tracks; // events in seconds, heights before zoom
		std::vector<uint64_t> m_laneTracks;     // the track of each lane
		std::vector<double> m_laneTops = {0.0};
		std::map<uint64_t, double> m_heights; // set by setTrackHeight, before zoom
		std::set<uint64_t> m_hidden;
		engine::Json m_project;
		uint64_t m_loads = 0;
		std::function<void()> m_layoutListener;
		std::vector<std::string> m_laneTypes; // and its type
		std::vector<bool> m_laneLinear;       // and whether its time base is linear
		std::vector<Shown> m_events;
		std::vector<uint64_t> m_selection;
		EventLanes* m_mutedLanes = nullptr;     // muted events over the others
		EventLanes* m_selectedLanes = nullptr;  // selected events over those
		EventLanes* m_folderLanes = nullptr;    // folders' data blocks, under the events
		FadeLines* m_fadeLines = nullptr;       // over the waveforms
		std::map<uint64_t, AudioFades> m_fades; // by audio event
		std::vector<EventLanes::Lane> m_folderData;
		std::string m_folderMode = "always"; // ui.folderData
		int64_t m_left = 0;                  // the locators, in samples
		int64_t m_right = 0;
		bool m_sizingMovesContents = false;
		uint64_t m_dragEvent = 0; // pressed on
		bool m_dragMoved = false;
		bool m_dragStarted = false; // moving events, after the drag delay
		bool m_dragToggles = false; // a click with Ctrl or Shift takes the pressed event out of the selection
		double m_pressTime = 0.0;   // RmlUi's clock, in seconds
		double m_dragDx = 0.0;      // dp, while moving, snapped
		int m_dragLanes = 0;
		double m_dragLaneY = 0.0; // where a drag of events started, in dp from the top of the first lane
		double m_end = 0.0;       // of the last event
		double m_length = g_defaultProjectLength; // the project setup's
		bool m_loading = false;
		bool m_stale = false;
		int64_t m_cursorSample = 0;
		double m_cursor = 0.0;
		bool m_playing = false;

		TimelineView m_view;
		std::vector<TimelineView> m_undo;
		std::vector<TimelineView> m_redo;
		bool m_autoScroll = false;
		AutoScrollMode m_autoScrollMode = AutoScrollMode::Page;
		bool m_suspendWhenEditing = false;
		bool m_suspended = false;
		TimeFormat m_format = TimeFormat::BarsBeats;
		bool m_crossHair = true;
		engine::Json m_presets = engine::Json::array();
		bool m_presetsKnown = false; // changed since they were asked for

		Drag m_drag = Drag::None;
		float m_dragX = 0.0F; // where it started, in the window's pixels
		float m_dragY = 0.0F;
		TimelineView m_dragView;
		double m_dragLength = 0.0; // the overview's, in seconds
		double m_dragTime = 0.0;   // under the mouse when it started
		double m_dragAt = 0.0;     // dp from the left of the event lanes
		bool m_dragZoomed = false;

		Rml::Element* m_formatMenu = nullptr; // the document's, null if it has none
		Rml::Element* m_presetsMenu = nullptr;
		Rml::Element* m_autoScrollButton = nullptr;
		Rml::Element* m_autoScrollMenu = nullptr;
		Tool m_tool = Tool::ObjectSelection;
		Tool m_pressTool = Tool::ObjectSelection; // the tool of the press now, upper or lower half when combined
		bool m_combine = false;
		LineMode m_lineMode = LineMode::Line;
		PlayMode m_playMode = PlayMode::Play;
		uint64_t m_auditioned = 0; // the track the play tool auditions or scrubs while pressed
		std::optional<RangeSelection> m_range;
		std::optional<RangeSelection> m_rangeBase; // what a drag with Shift adds to
		double m_pressX = 0.0;                     // dp in the event lanes
		double m_pressY = 0.0;
		std::string m_pressed; // the modifiers held at the press: ctrl, alt and shift joined with +
		std::map<std::string, std::string> m_toolModifiers;
		bool m_modifiersKnown = false; // changed since they were asked for
		Rml::Element* m_rangeElement = nullptr;
		Rml::Element* m_rectangle = nullptr;
		std::vector<Rml::Element*> m_toolButtons; // the document's
		Rml::Element* m_combineButton = nullptr;
		Rml::Element* m_objectMenu = nullptr;
		Rml::Element* m_lineMenu = nullptr;
		Rml::Element* m_playMenu = nullptr;
		std::vector<CycleMarker> m_cycleMarkers;
		std::vector<int64_t> m_markers; // every marker's position and end, in samples, for snapping
		engine::SnapSettings m_snap;
		int m_formatsSent = 0; // snap.set {format} from the ruler that the engine hasn't answered yet
		engine::midi::QuantizeSettings m_quantize;
		Rml::Element* m_snapButton = nullptr;
		Rml::Element* m_snapTypeButton = nullptr;
		Rml::Element* m_snapTypeMenu = nullptr;
		Rml::Element* m_gridTypeButton = nullptr;
		Rml::Element* m_gridTypeMenu = nullptr;
		Rml::Element* m_zeroCrossingButton = nullptr;
		Rml::ObserverPtr<Rml::Element> m_dialog; // the dialog document open now
		engine::Json m_organized;                // the presets that the organize dialog lists, by index
		Import m_import;
		bool m_importCopy = true; // the import options chosen last
		bool m_importConvert = true;
		bool m_importSplit = false;
		bool m_importOneTrack = false;

		std::map<std::string, std::function<void()>> m_actions;
		std::vector<std::function<void()>> m_viewListeners;
		std::vector<Subscription> m_subscriptions;
		std::optional<Subscription> m_position; // while playing
	};
}
