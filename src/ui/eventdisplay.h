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
	// A row of track height (reference manual p. 253), the least a track has, in dp with vertical zoom.
	inline constexpr double g_trackRow = g_minTrackHeight;
	// The least height of a track whose in-place editor is open (#131), before vertical zoom, in dp.
	inline constexpr double g_inPlaceHeight = 240.0;
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
		Comp, // takes on lanes (#99)
		Draw,
		Erase,
		Split,
		Glue,
		Mute,
		Zoom,
		Line,
		Play,
		TimeWarp, // #159
		Color,    // gives events the tool's color (reference manual pp. 114-115)
	};
	// The tool's name, as the display's attribute tool and the toolbar's buttons name it: objectSelection, split, ...
	const char* toolName(Tool tool);
	std::optional<Tool> toolFromName(const std::string& name);
	// The tool's name as the toolbar's tooltips show it: Object Selection, Split, ...
	const char* toolTitle(Tool tool);

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

	// The time warp tool's modes (reference manual p. 1465): the grid with musical tracks keeping their time, the
	// grid with them following, or warp markers on audio events.
	enum class WarpMode : uint8_t
	{
		Grid,
		GridFollow,
		Free,
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
		// A track's height: its own, at least g_inPlaceHeight while its in-place editor is open. Its lanes (#99) have
		// its own height.
		double trackHeight(uint64_t track) const;
		double ownHeight(uint64_t track) const;
		void setTrackHeight(uint64_t track, double height);
		void setTrackHeights(const std::vector<uint64_t>& tracks, double height); // those tracks' (clamped)
		// The heights tracks have of their own, by track, and replacing them all (the project's, trackLayout.get).
		const std::map<uint64_t, double>& ownTrackHeights() const { return m_heights; }
		void setOwnTrackHeights(std::map<uint64_t, double> heights);
		void setHiddenTracks(std::set<uint64_t> tracks);
		// The tracks whose in-place editors are open (#131, ui/inplaceeditor.h): they are at least g_inPlaceHeight
		// high, for the editor over their lane.
		void setInPlaceTracks(std::set<uint64_t> tracks);
		const std::set<uint64_t>& inPlaceTracks() const { return m_inPlace; }
		// Whether an open in-place editor shows its toolbar and info line, which the triangle in its track's row of
		// the track list switches (reference manual p. 1259); hidden again once the editor closes.
		bool inPlaceToolbarShown(uint64_t track) const { return m_inPlaceToolbars.count(track) != 0; }
		const std::set<uint64_t>& inPlaceToolbars() const { return m_inPlaceToolbars; }
		void showInPlaceToolbar(uint64_t track, bool shown);
		// The quantize panel's cutting positions (#1579, reference manual p. 378): a .cutting-position line over the
		// event lanes where Slice would cut, at each of the project samples, by position. Each line shows that is
		// at least g_cutDistance dp after the one shown before it (#1668), so that a flam zoomed out keeps the others.
		void setCuttingPositions(std::vector<int64_t> positions);
		const std::vector<int64_t>& cuttingPositions() const { return m_cuttingPositions; }
		// The project as project.get last returned it, with the channels that channel.changed brought since, or null
		// before the first answer; loads() counts the answers and those channels, answers() only the answers, for the
		// views that show no channel (#1881), and read() is when the binding handed the last of them on
		// (EngineBinding::handed), a channel only while no answer is out, which may be older than it.
		const engine::Json& project() const { return m_project; }
		// Where each audio event of the project plays, in seconds, as the project window shows it: from its position
		// in its track's time base to where its warp puts its end (playedLength), or else as long as its clip frames
		// at its media's rate. An event in an audio part by its own span, also where the part cuts it off.
		struct AudioEventSpan
		{
			uint64_t track = 0;
			double start = 0.0;
			double end = 0.0;
		};
		const std::map<uint64_t, AudioEventSpan>& audioEventSpans() const { return m_audioSpans; }
		uint64_t loads() const { return m_loads; }
		uint64_t answers() const { return m_answers; }
		uint64_t read() const { return m_read; }
		// The tempo map and signatures of project(), read once per answer.
		const TimeContext& time() const { return m_time; }
		// The track whose lane is at a point of the window (pixels, as mouse_x and mouse_y), with the time there in
		// ticks, for something dropped there such as a chord pad (docs/features/chord-pads.md); none outside the
		// lanes.
		std::optional<std::pair<uint64_t, int64_t>> trackAt(float x, float y) const;
		// Creates an event of a sample-editor region on the audio lane under the release, in window pixels.
		void dropRegion(const engine::Json& region, float x, float y);
		// Folder tracks' data blocks (reference manual p. 226, docs/features/folder-tracks.md): per lane, the events
		// and parts of the tracks inside a folder's lane, by the user setting ui.folderData (always, never, or
		// hideWhenExpanded: while the folder is folded); empty for the other lanes.
		const std::vector<EventLanes::Lane>& folderData() const { return m_folderData; }
		// Called after the lanes, the view or a channel (channel.changed, #1881) changed.
		void onLayout(std::function<void()> listener) { m_layoutListener = std::move(listener); }
		// Called for a double click below the tracks instead of adding a MIDI track, e.g. by the track list, which
		// adds a track like the selected one (docs/features/add-track.md).
		void onAddTrack(std::function<void()> listener) { m_addTrack = std::move(listener); }
		// Called with the part for a double click on a MIDI part or an audio part instead of opening it in the editor
		// window, e.g. by the app, which opens it where the preference editing.doubleClickOpensEditorIn says.
		void onOpenEditor(std::function<void(uint64_t)> listener) { m_openEditor = std::move(listener); }
		// Called with the error when the engine refuses an import of audio or MIDI files (audioFile.import,
		// midiFile.import), e.g. by the app, which shows it in a dialog (#1976); otherwise it is only logged.
		void onImportFailed(std::function<void(const std::string&)> listener) { m_importFailed = std::move(listener); }
		// The selected tracks, top to bottom, e.g. the track list's, which the app gives: Paste and Paste Relative to
		// Cursor without a range paste onto the first of them (range.paste selectedTrack, reference manual p. 270).
		void useTrackSelection(std::function<std::vector<uint64_t>()> selection)
		{
			m_trackSelection = std::move(selection);
		}

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
		// the locators when the click is between them; one below the tracks adds a MIDI track (see onAddTrack).
		const std::vector<uint64_t>& selection() const { return m_selection; }
		void select(std::vector<uint64_t> events);
		// Where the selection starts, in ticks: the selection range's start, else the first selected event's; none
		// without either (Tap Tempo's tempo event, reference manual p. 1458).
		std::optional<int64_t> selectionStart() const;
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
		// Where a point dragged to seconds snaps to, in seconds, from where it was (for grid relative), as the
		// display's own drags snap: e.g. markers on the marker track (ui/markers.h). The magnets (samples) attract as
		// the events' edges do, e.g. the other arranger events' edges for one on the arranger track's lane.
		double snapped(double seconds, std::optional<double> from = std::nullopt,
			const std::vector<time::Samples>& magnets = {}) const;
		// The grid line at or before seconds, in seconds: where a click draws an arranger event from when snapping
		// would put it after the click (ui/transposetrack.h).
		double gridLineBefore(double seconds) const;
		// Why the engine refused an edit of events, such as a glue of parts that follow global transpose differently,
		// in the notification #edit-refused for a moment (#1367), or of the global tracks' events
		// (ui/transposetrack.h); logged too.
		void showRefusal(const std::string& error);

		// The default tool modifiers (reference manual p. 1601) of the tools that GNUbase has, by action, which the
		// user setting ui.toolModifiers overrides one by one.
		static const std::map<std::string, std::string>& defaultToolModifiers();

		// The tools (#86, eventtools.cpp, docs/features/tools.md), also in the root's attribute tool. The object
		// selection tool edits as above; the others send the same event.* commands (split, glue, erase, mute), zoom,
		// draw MIDI parts, audition or scrub the pressed audio track, or select a range. With the selection tools
		// combined, the object selection tool selects ranges in a lane's upper half. Their tool modifiers are the user
		// setting ui.toolModifiers. The document's toolbar may have a button per tool, button[tool=<name>], whose
		// attribute tool-menu names a menu of its modes that a second click opens (#object-selection-menu with the
		// sizing modes, #line-menu with the line modes, #play-menu with the play modes, #time-warp-toolbar-menu with
		// the time warp modes), and a toggle button #combine-selection-tools. Menu items with a tool attribute choose
		// it too, and those of #time-warp-menu, the time warp modes, choose the time warp tool and the mode. A right
		// click in the event lanes opens the toolbox #toolbox or the context menu #event-display-menu: the toolbox with
		// the user setting ui.toolboxOnRightClick and the context menu with a modifier key, or the other way round
		// while it is off. A tool chosen with its command (actions()) shows its name in the notification
		// #tool-notification, unless the user setting ui.toolSwitchNotification is off. Over the lanes, the attribute
		// pointer of the event area names the mouse pointer for the style sheets: the tool's name, the range selection
		// tool's in a lane's upper half with the selection tools combined, or resize at an event's edge that a drag
		// resizes; with the modifier keys held, what a press with them does, such as split with the tool modifier Split
		// Event.
		void setTool(Tool tool);
		Tool tool() const { return m_tool; }
		// The color tool's color, "#rrggbb", also in the root's attribute tool-color: the one chosen last, at first
		// the color set's first. A click of the color tool gives it to the event, or to the selection if the event is
		// in it; with Alt the tool takes the event's color, its own or else its track's; with Ctrl/Cmd the event is
		// selected and onChooseColor called (reference manual pp. 114-115).
		void setToolColor(std::string color);
		std::string toolColor() const;
		// Steps the tool's color through the color set, around at its ends, as the mouse wheel over the tool's button
		// does; a color outside the set steps to its first.
		void stepToolColor(int steps);
		// Called when the tool's color may have changed: chosen, or the project read again.
		void onToolColor(std::function<void()> listener) { m_toolColorListener = std::move(listener); }
		// Called for a Ctrl/Cmd-click of the color tool on an event with where it is in the window (pixels): the app
		// opens the Choose Event Color pane there (ui::ColorMenu).
		void onChooseColor(std::function<void(float x, float y)> listener) { m_chooseColor = std::move(listener); }
		// Whether the modifier keys of a mouse event are those the user setting ui.toolModifiers gives the action, e.g.
		// erase.deleteLaterEvents.
		bool toolModifier(const char* action, Rml::Event& event) const;
		void setCombineSelectionTools(bool on);
		bool combineSelectionTools() const { return m_combine; }
		void setLineMode(LineMode mode);
		LineMode lineMode() const { return m_lineMode; }
		void setPlayMode(PlayMode mode);
		PlayMode playMode() const { return m_playMode; }
		void setWarpMode(WarpMode mode);
		WarpMode warpMode() const { return m_warpMode; }
		// The selection range, also in the root's attributes range ("start end") and range-tracks (ids); none
		// without the range selection tool. Choosing another tool selects the events in the range instead.
		const std::optional<RangeSelection>& range() const { return m_range; }
		// A drag in it on one of its tracks, without Shift, moves its contents (range.move), across and onto other
		// tracks, with the tool modifier dragDrop.copy copies them, and the range goes with them (reference manual
		// p. 291); a drag just inside one of its edges moves that edge (p. 288); a click there without a drag selects
		// a range of no length, as anywhere.
		void setRange(std::optional<RangeSelection> range);
		// The lanes' tracks, each once: a track's automation tracks' lanes have its id too (#121).
		std::vector<uint64_t> shownTracks() const;
		// Importing files (#92, fileimport.cpp, docs/features/file-import.md). MIDI files (.mid, .midi, .smf) are
		// imported with midiFile.import. Audio files open the Import Options dialog (import-options.rml next to the
		// document), or with the user setting editing.onImportAudio at useSettings are imported at once with the
		// options in the settings (#477), with audioFile.import at sample (default: the project cursor), one after
		// another onto track if it is an audio track, else onto new tracks. Files dropped on the display go to the
		// track of the lane under the mouse, at the time there. With clipsOnly, audio files only go into the pool
		// (#93), without tracks or events.
		void importFiles(const std::vector<std::string>& files, std::optional<uint64_t> track = std::nullopt,
			std::optional<int64_t> sample = std::nullopt, bool clipsOnly = false);
		// File > Import (#480, reference manual p. 360): at the project cursor, onto the first of the selected tracks
		// (the track list's, top to bottom) if it is an audio track: the first file, or all of them one after another
		// with Import Files on One Track, and the others onto new tracks. Without one, onto new tracks; with Split
		// Channels, every channel of a file with several onto a new track of its own.
		void importToSelection(const std::vector<std::string>& files, const std::vector<uint64_t>& selection);
		// File > Import > MIDI File (#478, reference manual pp. 364-365): imports MIDI files with the options of the
		// user settings midi.import*, at the left locator with midi.importToLeftLocator, else at the project start.
		// Into a new project (newProject), the file's tempo always replaces the project's. MIDI files dropped on the
		// event lanes go where they are dropped instead (elsewhere on the display, such as the track list, to the
		// project cursor), and with midi.importSinglePart into one part, on the MIDI or instrument track whose lane
		// or track list row they are dropped on if any.
		void importMidi(const std::vector<std::string>& files, bool newProject = false);

		// The commands that key commands bind to, by id: zoom.in, zoom.out, zoom.inVertically, zoom.outVertically,
		// zoom.full, zoom.undo, zoom.redo and view.autoScroll (on or off); locate.nudgeCursorLeft and
		// locate.nudgeCursorRight move the project cursor by a grid step; and the edits of parts and events:
		// edit.delete, edit.mute, edit.unmute, edit.lock, edit.unlock, edit.splitAtCursor, edit.splitLoop, edit.glue,
		// edit.duplicate, edit.repeat (opens the Repeat Events dialog), edit.fillLoop, edit.toRealCopy, edit.nudgeLeft,
		// edit.nudgeRight, edit.nudgeStartLeft, edit.nudgeStartRight, edit.nudgeEndLeft, edit.nudgeEndRight,
		// edit.nudgeTopUp, edit.nudgeTopDown, edit.nudgeBottomUp, edit.nudgeBottomDown,
		// edit.moveStartToCursor, edit.moveEndToCursor, edit.selectAll, edit.selectNone, edit.invertSelection,
		// edit.selectInLoop, edit.selectFromStartToCursor, edit.selectFromCursorToEnd, edit.selectUnderCursor, and
		// Left and Right's edit.selectPrevious and edit.selectNext; and edit.locatorsToSelection sets the locators
		// around the selected events. The Fades menu (#138, docs/features/fades.md): audio.fadeInToRange,
		// audio.fadeOutFromRange and audio.adjustFadesToRange on the selection range, audio.crossfade (crossfades the
		// selected audio events, or opens the crossfade editor for a crossfade they have), audio.removeFadeIn,
		// audio.removeFadeOut, audio.removeFades, audio.fadeEditor (the fade dialog), and audio.applyStandardFadeIn
		// and audio.applyStandardFadeOut (the default fades) on the selected audio events.
		// audio.detectSilence, audio.spectrumAnalyzer and audio.statistics (#147, docs/features/audio-functions.md)
		// open the Detect Silence dialog, the Spectrum Analyzer and the Statistics window for them.
		// audio.removeVolumeCurve (#139, docs/features/volume-curves.md) resets the volume curves in the selection
		// range, else removes the selected audio events'.
		// With the range selection tool,
		// edit.delete deletes the range; edit.cut, edit.copy, edit.cutTime, edit.deleteTime, edit.insertSilence,
		// edit.splitRange and edit.cropRange edit it, edit.paste and edit.pasteTime paste at its start (without a range
		// at the project cursor), edit.pasteAtOrigin where it was copied from (range.paste origin), edit.pasteRelative
		// as far from the cursor as it was copied (range.paste relative) and edit.globalCopy copies the locator range
		// on every track; without a range edit.cut and edit.copy take the selected events (#1910); edit.duplicate
		// copies its contents to its end, where the range goes; edit.nudgeLeft and edit.nudgeRight move the range by a
		// grid step without its contents, the start and end nudges its edges, and edit.nudgeTopUp, edit.nudgeTopDown,
		// edit.nudgeBottomUp and edit.nudgeBottomDown take a track in or out at its top or bottom (reference manual pp.
		// 288-291). With editing.rangeToolSplitsPartlySelectedEvents, edit.mute and edit.unmute set what lies in a
		// range with a length, cut at its edges (range.set), and edit.moveStartToCursor and edit.moveEndToCursor move
		// it to the cursor with the range (range.move); off, they take the selected events. edit.snap (snap on or off)
		// and edit.snapPointToCursor (the selected audio events' snap points); and the tools: tool.objectSelection,
		// tool.rangeSelection, tool.draw, tool.erase, tool.split, tool.glue, tool.mute, tool.zoom, tool.line,
		// tool.play, tool.next, tool.previous and tool.combineSelectionTools (on or off).
		const std::map<std::string, std::function<void()>>& actions() const { return m_actions; }
		// Whether the action can do something now, for menus and keys to disable it: the actions that only edit the
		// selection range (edit.cutTime, edit.deleteTime, edit.insertSilence, edit.cropRange, edit.splitRange and the
		// top and bottom nudges) need a range, all but edit.splitRange and the nudges one with a length; edit.cut and
		// edit.copy a range with a length or, without a range, selected events; edit.paste, edit.pasteTime,
		// edit.pasteAtOrigin and edit.pasteRelative need something copied, a rangeClipboard.changed since the project
		// opened. The others always can.
		bool canRun(const std::string& action) const;

		// Automation tracks (#121, automationtracks.cpp, docs/features/automation-tracks.md): lanes of their own below
		// their track's lane, each showing one of the track's automation lanes (automationLane.*) as a curve with its
		// points, or the static value line while it has no points or doesn't read. Which ones show is view state, like
		// folding. laneAutomation() has the automation lane of each display lane, 0 for a track's own lane; a track's
		// automation lanes have its id in laneTracks() too. Showing a track's automation shows its used lanes (with
		// points), or else its first lane, or else its first parameter (automationLane.targets) without a lane: an
		// unadded lane, which the project gets with the first points a tool draws, in their undo step (#1918).
		const std::vector<uint64_t>& laneAutomation() const { return m_laneAutomation; }
		// Lanes (#99, docs/features/comping.md): a track whose lanes show has a display lane per lane of its events
		// and parts below its own, before its automation tracks, each as high as the track. The track's own lane shows
		// them all, audio events where none in front of them covers them. laneTakes() has the lane of its track each
		// display lane shows, -1 for a track's own lane and automation tracks; the lanes have the track's id in
		// laneTracks(). Which tracks show their lanes is view state, like folding.
		const std::vector<int>& laneTakes() const { return m_laneTakes; }
		bool lanesShown(uint64_t track) const { return m_lanesShown.count(track) != 0; }
		void showLanes(uint64_t track, bool shown);
		bool automationShown(uint64_t track) const { return m_openAutomation.count(track) != 0; }
		void showAutomation(uint64_t track, bool shown);
		void showUsedAutomation(); // every track's lanes with points
		void hideAllAutomation();
		// Shows one more automation track below the track's last one: the next parameter in automationLane.targets'
		// order that none shows, with its lane, or unadded if the track has none for it.
		void appendAutomation(uint64_t track);
		// Shows the plugin's lanes on its track and hides the track's others (the lane automates the plugin).
		void showOnlyPlugin(uint64_t lane);
		// The parameter selector: the automation track that shows the lane shows the target's lane instead, or the
		// target unadded if the track has none. No undo step: a lane it showed stays in the project.
		void setAutomationTarget(uint64_t lane, const engine::Json& target);
		void removeUnusedAutomation(uint64_t track); // the track's lanes without points, and its automation tracks
		void removeAutomationLane(uint64_t lane);    // Remove Parameter: the lane goes and its automation track closes
		// An unadded lane, as project.get has lanes (with no points and read off), or null for any other id. Their ids
		// come after the project's, so the automation tracks' lane attributes tell them apart.
		const engine::Json* unaddedLane(uint64_t lane) const;
		// automationLane.targets for a track whose automation shows, or null until it is in.
		const engine::Json* automationTargets(uint64_t track) const;
		// The selected automation points, by lane, also in the root's attribute automation-selection ("lane:point"
		// pairs). Selecting events deselects them, and the other way round.
		const std::map<uint64_t, std::set<uint64_t>>& selectedPoints() const { return m_selectedPoints; }
		void selectPoints(std::map<uint64_t, std::set<uint64_t>> points);

		// Runs listener after every change of the view, for what others draw on the timeline, such as marker lines
		// (ui/markers.h).
		void onViewChanged(std::function<void()> listener) { m_viewListeners.push_back(std::move(listener)); }
		// Runs listener after the selection changed and after every project update, which can change what is selected,
		// such as the quantize panel's Hitpoint Tracks (ui/quantizepanel.h).
		void onSelectionShown(std::function<void()> listener) { m_selectionListeners.push_back(std::move(listener)); }
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
			RangeMove,     // the selection range's contents, or a copy of them, from m_dragRange
			RangeEdge,     // m_rangeEdge of the selection range, from m_dragRange
			DrawPart,      // a new MIDI part
			Comp,          // brings what it is released on to the front, or comps the range dragged on a lane
			Play,          // auditions or scrubs until the mouse is released
			Click,         // splits, glues or erases where it is released, unless it moved
			FadeIn,        // the fade in handle of the pressed audio event
			FadeOut,       // its fade out handle
			Volume,        // its event volume control
			VolumePoint,   // a point of its volume curve, with the draw tool
			Automation,    // on an automation track (automationtracks.cpp)
			TimeWarp,      // the time warp tool's, on release (eventtimewarp.cpp)
			Repeat,        // repeats the selection to the right, m_repeatCount times (#2009)
		};

		// An automation lane of the project as the display shows it: points in seconds, values from 0 (bottom) to 1.
		struct AutomationView
		{
			uint64_t id = 0;
			uint64_t track = 0;
			bool read = true;
			bool linear = false;            // its track's time base
			engine::Json target;            // as automationLane.add takes it
			engine::Json info;              // its entry of automationLane.targets, null until it is in
			std::vector<uint64_t> ids;      // the points', by position
			std::vector<CurvePoint> points; // in seconds and display values
		};
		// What a press on an automation track does while dragging.
		enum class AutomationGesture : uint8_t
		{
			Add,    // a click adds a point there; a drag selects the points in a rectangle
			Move,   // moves the selected points
			Draw,   // the draw tool's freehand curve
			Line,   // the line tool
			Erase,  // removes the point under the mouse
			Editor, // a smart control of the automation event editor
			None,
		};
		enum class EditorControl : uint8_t
		{
			MoveVertically,
			ScaleVertically,
			TiltLeft,
			TiltRight,
			Stretch,
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
			// An audio event's entry on its track's lane, which shows it only where none in front covers it (#99): if
			// it is muted, m_mutedPieces has what shows.
			bool onTrackLane = false;
			bool audio = false; // audio events keep their length in samples when moved by ticks
		};

		void ProcessEvent(Rml::Event& event) override;
		void mouseDown(Rml::Event& event, Rml::Element& target);
		void mouseMove(Rml::Event& event);
		void wheel(Rml::Event& event);

		// Elements live while the root does.
		bool alive() const { return static_cast<bool>(m_root); }
		// Whether the focus is in the project window's own focus zone, not in the lower zone, an in-place editor or
		// another zone: only there do Cut and Copy without a range take the selected events (#1910).
		bool projectHasFocus() const;
		float dpRatio() const;
		double width() const;  // of the event lanes, in dp
		double height() const; // of the event lanes, in dp
		double overviewLength() const;
		// x in the window's pixels as dp from the left of an element.
		double dpFrom(Rml::Element& element, float x) const;

		void requestProject();
		// Puts a channel-only step's channel into the project, as its channel.changed brought it (#1881).
		void takeChannel(const engine::Event& channelChanged);
		// Shows m_project, with event.previewResize's answer over it while an edge is dragged.
		void showProject();
		void loadProject(const engine::Json& project);
		void setFolderMode(const std::string& mode); // ui.folderData
		// The event display's preferences (#1313): takes one, and shows them all.
		void takeDisplayPreference(const std::string& key, const engine::Json& value);
		void showDisplayPreferences();
		// The names and MIDI part contents for the lanes as high as they are now, as the preferences say.
		void showNamesAndContents(const std::vector<EventLanes::Lane>& lanes);
		void setTreatMutedAudioLikeDeleted(bool on); // editing.treatMutedAudioLikeDeleted
		void setLanes();
		void followTransport(const engine::Json& status, engine::RequestId request = 0);
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
		void readMarkerMagnets(const engine::Json& project); // m_markers, from a project.get answer
		void click(Rml::Element* target, bool control);      // control: with Ctrl (Cmd on macOS) held
		void dialogEvent(Rml::Event& event);
		void openDialog(const char* file);
		void showFormat();
		void showRuler();
		void takeSnap(const engine::Json& settings); // from snap.changed or project.get
		void followSnapFormat();                     // the ruler shows the snap settings' format
		void showPresets();
		void showAutoScroll();
		void showSnap();
		void showRootKey(const engine::Json& project);
		void showStates(const engine::Json& project);
		void setSnap(const engine::Json& settings); // sends snap.set; one with a format counts in m_formatsSent

		// Importing files (fileimport.cpp).
		struct Import
		{
			std::vector<std::string> files; // audio files, waiting for the Import Options dialog
			std::optional<uint64_t> track;
			int64_t sample = 0;
			bool clipsOnly = false;
			bool selected = false;             // track is the selected one, which takes the first file, not all of them
			int lane = 0;                      // the lane of track the files go on
			bool dropped = false;              // on the display, at sample
			std::optional<uint64_t> midiTrack; // the MIDI or instrument track they were dropped on
		};
		// The import options (#477): the Import Options dialog's, and the user settings editing.importAudio* that it
		// starts from and keeps its choices in.
		struct ImportOptions
		{
			bool copy = true;
			bool convert = true;
			bool split = false;
			bool oneTrack = false;
			std::string nameFormat = "dash";
		};
		void startImport(const std::vector<std::string>& files, Import import);
		void sendMidiImport(const std::vector<std::string>& files, const Import& import, bool newProject);
		// The done of an import's command: tells onImportFailed's listener why it failed, or logs it without one.
		std::function<void(const engine::Result&)> importDone();
		void dropFiles(Rml::Event& event);
		void openImportOptions(Import import, const ImportOptions& options, bool saved);
		void importAudio(Rml::Element& dialog);
		void sendImport(const Import& import, const ImportOptions& options, bool saved);
		// Import Files on One Track: for several files, unless they only go into the pool or were dropped on a track,
		// which takes them all.
		static bool oneTrackApplies(const Import& import);
		// From settings.list's result: the options in the user settings, and whether editing.onImportAudio says to
		// import with them without the dialog.
		static ImportOptions importSettings(const engine::Json& list);
		static bool usesSettings(const engine::Json& list);

		// Editing parts and events (eventediting.cpp).
		void addEditActions();
		const Shown* eventAt(double x, double y) const; // dp in the event lanes
		size_t laneAt(double y) const; // dp from the top of the first lane; 0 above it, the lane count below the last
		void areaDown(Rml::Event& event, double x, double y);
		// A right click that opens the context menu selects the event there, unless it's selected already.
		void areaRightDown(Rml::Event& event, double x, double y);
		void areaMove(Rml::Event& event, double dx, double dy);
		// A Ctrl/Shift press left for a click: drags the pressed event once the drag starts.
		void narrowToPressed();
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
		// Resizing the selection (#732): event.resize's parameters for a drag of the pressed event's edge dx dp across;
		// the preview of them that the display shows while the edge is dragged; the release, which sends them; and
		// Escape, which cancels the drag if it is one of edges.
		engine::Json resizeParameters(double dx, Drag drag) const;
		void previewResize(engine::Json parameters);
		void askResizePreview();
		void endResize(); // drops the preview and the answers still on their way
		void resizeUp(engine::Json parameters);
		bool cancelResize();
		void resizeProjectChanged(); // a project.get answer came in
		void resizeKeysChanged();    // the modifier keys held changed
		// Sends the move or copy of the selection: the shift's parameters (event.move's delta or ticks), lanes down.
		void moveSelection(engine::Json shift, int lanes, bool copy, bool shared);
		void doubleClick(Rml::Event& event);
		void showSelection();
		// Sends an event.* command for the selection; with selectResult, the events it returns become the selection.
		void edit(const std::string& command, engine::Json parameters, bool selectResult = false);
		void finishSliceJob(const engine::Json& job); // reports a Close Gaps job's failure once

		// Adds range.paste's selectedTrack, the first of useTrackSelection's tracks, and with snap its snap.
		void pasteOntoSelectedTrack(engine::Json& parameters, bool snap) const;
		void selectWhere(const std::function<bool(const Shown&)>& selected);
		// Group Editing of folder tracks (#1579, reference manual p. 274): each track's outermost folder with Group
		// Editing on, for the tracks that have one; the events and parts with those of their folder's other tracks
		// that have the same start, end and lane; and a range's tracks with the other tracks of their folders, in the
		// project's order.
		std::map<uint64_t, uint64_t> editGroups() const;
		std::vector<uint64_t> withEditGroups(std::vector<uint64_t> events) const;
		std::vector<uint64_t> withEditGroupTracks(const std::vector<uint64_t>& tracks) const;
		int64_t samples(double seconds) const;
		// Snapping (eventediting.cpp): where a point at seconds goes, from where it was for grid relative; the
		// selection's edges and snap points don't attract.
		engine::SnapGrid snapGrid() const;
		time::Position snapAt(
			double seconds, std::optional<double> from, const std::vector<time::Samples>& magnets = {}) const;
		// The distance from seconds to a snapped position as event.move and event.resize take it: ticks when the grid
		// is musical and every selected event is on a musical track, else samples. Also in seconds.
		std::pair<engine::Json, double> shiftTo(const time::Position& to, double from, bool moving = false) const;
		int64_t clampMove(time::TimeBase base, int64_t shift) const;
		std::pair<engine::Json, double> moveShift(double dx) const; // a drag of the selection, dx dp across
		// Left and Right (reference manual p. 253): the event before (-1) or after (1) the selected ones on the lane.
		void selectNeighbour(int direction);
		// Adds a part on the lane from start to end, ticks, and selects it: an audio part on an audio track, a MIDI
		// part on a MIDI or instrument track.
		void addPart(size_t lane, time::Ticks start, time::Ticks end);
		// Whether the lane's track takes empty parts: audio, MIDI and instrument tracks.
		bool takesParts(size_t lane) const;
		// The lane at y dp down the event lanes, if there is one there.
		std::optional<size_t> visibleLaneAt(double y) const;
		// Whether y dp down the event lanes is in the lower half of the lane.
		bool inLowerHalf(size_t lane, double y) const;
		// The edge of the event at x dp across the lanes that a press there resizes: -1 its start, 1 its end, 0 none.
		int edgeAt(const Shown& shown, double x) const;
		// Whether a press with the keys at x, y dp on the event repeats the selection by dragging (#2009, reference
		// manual p. 271), on lanes at least two rows high: the middle of its right border, above the lower corner,
		// without keys or with Shift; or the lower right corner of the last selected event with Alt, or Alt and Shift.
		bool repeatHandleAt(const Shown& shown, double x, double y, const std::string& keys) const;
		// The selection from its first start to its last end, in seconds; nothing if none of it shows.
		std::optional<TimeSpan> selectionBlock() const;
		// How many copies a repeat drag dx dp to the right makes: the snapped distance in the selection's lengths.
		int repeatCount(double dx) const;
		// The edge of the audio event at x, y dp that a press of the play tool in scrub mode resizes, scrubbing along:
		// as edgeAt, but only at the event's lower left or right corner, the lower half of its lane, so that the rest
		// of the edge scrubs the track (reference manual p. 268); 0 for none.
		int scrubEdgeAt(double x, double y) const;
		// Sends audition.scrub for the auditioned track at x dp across.
		void scrubAt(double x);
		// The play tool's resize in scrub mode: scrubs the clip to the frame the audio event, at offset and length
		// clip frames, plays at the dragged edge.
		void scrubEdge(uint64_t clip, int64_t offset, int64_t length);

		// Fades and crossfades (eventfades.cpp): an audio event's fades and crossfade as project.get has them, its
		// media file's sample rate, the frames its fades may take together (fadeRoom), the clip frames a second it is
		// drawn at (its waveform's rate, which a warped event's warp changes), its offset and length in clip frames,
		// and its volume and volume curve.
		struct AudioFades
		{
			double rate = 0.0;
			int64_t fadeRoom = 0; // the frames its fades may take together: on a warped event what it plays for
			double timeRate = 0.0;
			int64_t length = 0; // in clip frames
			engine::Fade in;
			engine::Fade out;
			engine::Crossfade crossfade;
			int64_t offset = 0;
			uint64_t clip = 0;
			double volume = 0.0; // dB
			std::vector<engine::VolumePoint> curve;
		};
		void addFadeActions();
		// Apply Standard Fade In or Out (reference manual p. 385): the user setting editing.defaultFadeIn or
		// editing.defaultFadeOut on the selected audio events, as one undo step.
		void applyStandardFade(bool in);
		void readFades(const engine::Json& event, double rate, double timeRate);
		// The fade handle at x and y dp in the event lanes, of a selected event (only those have handles): -1 its
		// fade in's, 1 its fade out's, 0 none.
		int fadeHandleAt(const Shown& shown, double x, double y) const;
		// Starts dragging a fade handle when x and y dp in the event lanes are at one of the pressed event's.
		bool fadeDown(const Shown& shown, double x, double y);
		void fadeUp();
		// The fade length in seconds that a drag of a handle gives an event's fade in or fade out.
		double draggedFade(uint64_t event, bool in) const;
		void showFades(); // the fade lines and the selected audio events' handles
		// A double click on an event at seconds: in a crossfade's zone, opens the crossfade editor.
		bool openCrossfadeAt(const Shown& shown, double seconds); // whether there is one
		// The crossfade editor of a crossfade that plays between two of the selected audio events, or into or out of
		// the only selected one; whether there is one.
		bool openSelectedCrossfade();
		// The selected audio events, by id.
		std::vector<uint64_t> selectedAudio() const;
		// The selected MIDI parts, by id.
		std::vector<uint64_t> selectedMidi() const;
		// A click on one of the Tempo Detection dialog's buttons: tempo.detect with what they say.
		void detectTempo(const std::string& button);
		// Opens a dialog of the display's document folder whose Lua reads the attributes when it shows.
		void openFadeDialog(const char* file, const std::vector<std::pair<std::string, std::string>>& attributes);

		// Event volume and volume curves (eventvolume.cpp). The volume lines go from silence at the bottom of a lane
		// to g_volumeLineTop at its top; volumeFraction is how far up the event's lane y dp in the event lanes is.
		void addVolumeActions();
		double volumeFraction(const Shown& shown, double y) const;
		// The event volume control at the bottom left of a selected audio event, at faderOf seconds on events wide
		// enough for one: dragging it gives every selected audio event the volume in dB that draggedVolume says.
		std::optional<double> faderOf(const Shown& shown) const;
		bool volumeDown(const Shown& shown, double x, double y);
		// A move of the control dy dp from the press, fine with Shift (reference manual p. 402).
		void volumeMove(double dy, bool fine);
		void volumeUp();
		// .volume-readout beside the dragged control: the pressed event's volume and its change since the press.
		void showVolumeReadout();
		// The mouse wheel lines over x and y dp in the event lanes, with editing.wheelEventVolumeAndFades: the volume
		// of the audio event there, with Shift its fades (p. 382). Whether there was one.
		bool wheelVolume(double x, double y, double lines, bool fades);
		double draggedVolume(uint64_t event) const;
		// The draw tool on an audio event: drags the curve point at x and y dp, or a new one there; with Ctrl draws
		// points along the drag and where it is let go (drawCurveTo).
		bool curveDown(const Shown& shown, double x, double y);
		void curveMove(double x, double y);
		void drawCurveTo(double x, double y, bool release);
		void curveUp(double x, double y);
		std::vector<engine::VolumePoint> draggedCurve(uint64_t event) const;
		std::optional<size_t> curvePointAt(const Shown& shown, double x, double y) const;
		bool eraseCurvePoint(const Shown& shown, double x, double y);     // the erase tool, on a point
		void showVolume(FadeLines::Lane& lane, const Shown& shown) const; // its line and volume control

		// The time warp tool, Process Tempo, Process Bars, Set Definition from Tempo and copying warp markers
		// (eventtimewarp.cpp, #159).
		void addTimeWarpActions();
		bool timeWarpDown();
		void timeWarpUp(double x);

		// The tools (eventtools.cpp).
		void addToolActions();
		void showTools();
		// The color tool's color in the root's attribute, and onToolColor.
		void showToolColor();
		// A click of the color tool on that event, with the modifier keys pressed.
		void colorClick(uint64_t event);
		// The tool a press y dp down the event lanes uses: the range selection tool in a lane's upper half when the
		// selection tools are combined, else the tool.
		Tool toolAt(double y) const;
		// The mouse pointer over the lanes at x and y dp for a press with the modifier keys held (as modifiersOf
		// joins them), as the event area's attribute pointer names it; showPointer shows the one where the mouse is,
		// with the keys held now, unless a drag goes on. heldKeys follows the modifier keys going down and up.
		std::string pointerAt(double x, double y, const std::string& keys) const;
		void showPointer();
		void heldKeys(Rml::Event& event);
		// The menus that a right click in the lanes opens, without and with a modifier key.
		void showContextMenus();
		// The notification for a tool chosen with a key command.
		void notify(const std::string& text);
		// Whether the pressed modifiers, or the keys, are the ones the user setting ui.toolModifiers gives the action.
		bool modifiers(const char* action) const { return modifiers(action, m_pressed); }
		bool modifiers(const char* action, const std::string& keys) const;
		// What a press at x and y dp in the event lanes does with the tool, unless the object selection tool takes it.
		bool toolDown(double x, double y);
		void toolMove(double x, double y);
		void toolUp(double x, double y);
		bool toolDoubleClick(double x, double y); // whether it took the double click
		// The selection range's edges, of which a press just inside one drags it, with the range tool and no Shift.
		enum class RangeEdge : uint8_t
		{
			None,
			Start,
			End,
			Top,
			Bottom,
		};
		RangeEdge rangeEdgeAt(double x, double y) const;
		// Where a RangeMove drag to x and y dp has the range from, and a RangeEdge drag.
		RangeSelection movedRange(const RangeSelection& from, double x, double y) const;
		RangeSelection resizedRange(const RangeSelection& from, double x, double y) const;
		void setRangeFrom(double x, double y); // from the drag's start
		void selectRange();                    // the events in it, when another tool takes over
		void showRange();
		// Moves the range's edges to zero crossings of the audio on its tracks (reference manual p. 285), with Snap to
		// Zero Crossing on: snap.zeroCrossings answers later, and actions wait until it has (EngineBinding::sendAwaited
		// and afterAwaited).
		void zeroCrossRange();
		// Runs the actions that wait for zeroCrossRange's answers before a press, which may start another range, so
		// that they edit the range they were run on (#1086): the binding hands the answers on until nothing is held,
		// for at most a few seconds (EngineBinding::awaitHeld); actions still waiting then run on the range as it is.
		void runWaitingActions();
		// The rectangle that a tool drags, dp in the event lanes; hidden when not shown.
		void showRectangle(bool shown, double left = 0.0, double top = 0.0, double right = 0.0, double bottom = 0.0);
		void setToolModifiers(const engine::Json& value);

		// Automation tracks (automationtracks.cpp).
		void addAutomationActions();
		void readAutomation(const engine::Json& project); // m_automation, and asks for the targets it needs
		// Asks again for the targets of the tracks with open lanes whose track, channel, plugins or lanes changed in
		// the project, e.g. a channel's volume, which a volume lane without points shows.
		void requestChangedTargets();
		// Asks for the track's automationLane.targets again; then runs when they are in.
		void requestTargets(uint64_t track, std::function<void()> then = {});
		// A lane for an automation track that shows the target on the track without a lane yet, its id.
		uint64_t addUnaddedLane(uint64_t track, const engine::Json& target);
		void reload();              // the lanes again, from the project as it was last read
		void showAutomationLanes(); // the curves, after the lanes changed
		void placeAutomation();     // where they are, after the view changed
		void placePartMarks(); // the events' marks: locked, shared copy, no global transpose; after the view changed
		// The grid lines (#412, reference manual p. 109): the grid in use near the view, whether snap is on or not.
		void showGrid();
		void placeCuttingPositions(); // setCuttingPositions' lines, after the view changed
		// The automation lane of a display lane, or null for a track's own lane.
		const AutomationView* automationAt(size_t lane) const;
		double automationValue(size_t lane, double y) const; // y dp in the event lanes, as a display value
		double automationY(size_t lane, double value) const; // and back
		// The point of the lane within reach of x and y dp in the event lanes, as an index into its points.
		std::optional<size_t> pointAt(const AutomationView& lane, size_t index, double x, double y) const;
		bool automationDown(double x, double y);
		void automationMove(double x, double y);
		void automationUp(double x, double y);
		// The automation event editor's box around the selected points, dp in the event lanes (left, top, right,
		// bottom), and its display lane: when two or more points of one lane with a ramp curve are selected.
		struct EditorBox
		{
			size_t lane = 0;
			double left = 0.0;
			double top = 0.0;
			double right = 0.0;
			double bottom = 0.0;
		};
		std::optional<EditorBox> editorBox() const;
		// The smart control of the automation event editor at x and y dp, if it shows there.
		std::optional<EditorControl> editorControlAt(double x, double y) const;
		// The selected points of the pressed lane as a drag to x and y dp moves them or the editor changes them, by id.
		std::vector<std::pair<uint64_t, CurvePoint>> transformed(double x, double y) const;
		// The other points of the lane that moved points replace: those where a continuous selection lands
		// (reference manual pp. 877-878).
		std::set<uint64_t> covered(
			const AutomationView& lane, const std::vector<std::pair<uint64_t, CurvePoint>>& moved) const;
		// Shows these points on the display lane's curve while dragging.
		void preview(size_t lane, std::vector<CurvePoint> points);
		// The lane's points with those from start to end, seconds, replaced by these.
		static std::vector<CurvePoint> replaced(
			const AutomationView& lane, double start, double end, const std::vector<CurvePoint>& points);
		// What the line tool draws from its press to x and y dp, in seconds and display values.
		std::vector<CurvePoint> linePoints(double x, double y) const;
		// A track's lane, in the time base of its track: ticks or samples.
		int64_t pointPosition(const AutomationView& lane, double seconds) const;
		// A display value in the target's unit.
		static double targetValue(const engine::Json& info, double value);
		static double displayValue(const engine::Json& info, double value);
		// Sends the commands as one undo step; read turns on reading the lane first, as tools do on a click.
		void automationEdit(const std::string& description, const AutomationView& lane, engine::Json commands);
		// Replaces the lane's points from start to end, seconds, with these (seconds and display values).
		void replacePoints(const AutomationView& lane, double start, double end, const std::vector<CurvePoint>& points);
		void showPointSelection();
		std::vector<uint64_t> openLanes(uint64_t track) const; // the lanes a track shows, that still exist

		engine::Engine& m_engine;
		EngineBinding& m_binding;
		const std::string m_wheelSteps; // what the wheel's steps over events change, for EngineBinding::sendStep
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for the binding's answers after it went
		Rml::ObserverPtr<Rml::Element> m_root;
		Rml::Element* m_document = nullptr;
		Rml::Element* m_overview = nullptr;
		EventLanes* m_overviewEvents = nullptr;
		Rml::Element* m_overviewView = nullptr;
		Ruler* m_ruler = nullptr;
		Rml::Element* m_area = nullptr;
		EventLanes* m_lanes = nullptr;
		EventLanes* m_gridLines = nullptr;  // behind everything, one lane across the area
		EventLanes* m_snapPoints = nullptr; // a line at each audio event's snap point (p. 105), over the waveforms
		std::unique_ptr<EventWaveforms> m_waveforms; // over the lanes
		PartContents* m_contents = nullptr;          // MIDI parts' notes and controllers, over the lanes (#1313)
		EventNames* m_names = nullptr;               // the names of events and parts, over what they hold (#1313)
		// By lane, as m_tracks: the names of the events and parts each shows, and its MIDI parts' contents, in seconds.
		std::vector<EventNames::Lane> m_trackNames;
		std::vector<PartContents::Lane> m_trackContents;
		// What the names and contents were last shown for: the lanes' heights; empty when they must be shown again.
		std::vector<float> m_contentsHeights;
		// The event display's preferences (reference manual pp. 1603-1604, 1607; #1313), as the user settings have
		// them.
		struct DisplayPreferences
		{
			bool names = true;                  // ui.showEventNames
			bool hideTruncated = false;         // ui.hideTruncatedEventNames
			bool highlightBorders = false;      // ui.highlightSelectedEventBorders
			int eventOpacity = 100;             // ui.eventOpacity, percent
			int dataHeight = 20;                // ui.smallestTrackHeightForData, dp
			int nameHeight = 28;                // ui.smallestTrackHeightForName, dp
			int waveformBrightness = 50;        // ui.waveformBrightness, percent
			std::string partDataMode = "lines"; // ui.partDataMode
			bool controllers = true;            // ui.showControllers
			int noteBrightness = 50;            // ui.noteBrightness, percent
		};
		DisplayPreferences m_display;
		std::set<std::string> m_displayKnown; // those settings.changed gave, which an earlier settings.get doesn't undo
		size_t m_displayAsked = 0;            // the settings.get answers still to come when the display opens
		Rml::Element* m_crossVertical = nullptr;
		Rml::Element* m_crossHorizontal = nullptr;
		Rml::Element* m_cursorLine = nullptr;
		// The chord pads' step input cursor (reference manual p. 1369): a line with a handle in the ruler, while step
		// input is on, at the step position in ticks (chordPads.get).
		Rml::Element* m_stepCursor = nullptr;
		bool m_stepInput = false;
		int64_t m_stepTicks = 0;
		void followChordPads(const engine::Json& pads);
		void placeStepCursor();

		TimeContext m_time;
		std::vector<EventLanes::Lane> m_tracks; // events in seconds, heights before zoom
		std::vector<uint64_t> m_laneTracks;     // the track of each lane
		std::set<uint64_t> m_inPlace;           // setInPlaceTracks'
		std::set<uint64_t> m_inPlaceToolbars;   // showInPlaceToolbar's, of those
		std::vector<double> m_laneTops = {0.0};
		std::map<uint64_t, double> m_heights; // set by setTrackHeight, before zoom
		std::set<uint64_t> m_hidden;
		engine::Json m_project;
		uint64_t m_loads = 0;
		uint64_t m_answers = 0;
		uint64_t m_read = 0;
		ChannelSteps m_channelSteps; // channel.changed until their step's history.changed (#1881)
		std::function<void()> m_layoutListener;
		// useTrackSelection
		std::function<std::vector<uint64_t>()> m_trackSelection;
		std::function<void()> m_addTrack;                       // onAddTrack
		std::function<void(uint64_t)> m_openEditor;             // onOpenEditor
		std::function<void(const std::string&)> m_importFailed; // onImportFailed
		std::function<void()> m_toolColorListener;              // onToolColor
		std::function<void(float, float)> m_chooseColor;        // onChooseColor
		std::string m_toolColor;                                // empty: the color set's first
		std::vector<std::string> m_laneTypes;                   // and its type
		std::vector<bool> m_laneLinear;                         // and whether its time base is linear
		std::vector<Shown> m_events;
		std::vector<std::pair<size_t, TimeSpan>> m_mutedPieces; // by lane: what shows of covered muted events
		std::vector<uint64_t> m_selection;
		EventLanes* m_mutedLanes = nullptr;    // muted events over the others
		EventLanes* m_selectedLanes = nullptr; // selected events over those
		EventLanes* m_folderLanes = nullptr;   // folders' data blocks, under the events
		FadeLines* m_fadeLines = nullptr;      // over the waveforms
		// The .part-mark icons (attribute part, its id) in a .part-marks-of at the upper right corner of each MIDI part
		// and audio event shown: .independent if it is excluded from global transpose (#168, #1368, reference manual
		// p. 421), .locked if it is locked (#384, p. 276), .shared if it is a shared copy (p. 272).
		Rml::Element* m_partMarks = nullptr;
		std::set<uint64_t> m_independent;
		std::set<uint64_t> m_locked;
		std::set<uint64_t> m_shared;
		std::map<uint64_t, AudioEventSpan> m_audioSpans;
		std::string m_partMarksRml;     // what m_partMarks holds
		Rml::Element* m_cuts = nullptr; // the .cutting-position lines
		Rml::Element* m_volumeReadout = nullptr;
		std::vector<int64_t> m_cuttingPositions;
		size_t m_cutsShown = 0;                 // lines in m_cuts
		std::map<uint64_t, AudioFades> m_fades; // by audio event
		std::vector<EventLanes::Lane> m_folderData;
		std::string m_folderMode = "always"; // ui.folderData
		int64_t m_left = 0;                  // the locators, in samples
		int64_t m_right = 0;
		bool m_sizingMovesContents = false;
		double m_resizeFrom = 0.0;      // the pressed event's edge when a drag of edges started, in seconds
		int m_repeatCount = 0;          // copies a Drag::Repeat makes, which the selected events show
		bool m_repeatShared = false;    // and whether they are shared copies
		engine::Json m_resizeWanted;    // event.previewResize's parameters for where the edge is now, null for none
		engine::Json m_resizeAsked;     // those of the last one sent
		bool m_resizeAsking = false;    // its answer is on its way
		engine::Json m_resizePreview;   // the answer shown, null for none
		double m_resizeDx = 0.0;        // dp the edge was dragged
		engine::Json m_releasedPreview; // the last release's, until the project shows its resize
		bool m_resizeLanded = false;    // a release's event.resize is done; the next project shows it
		uint64_t m_resizeGesture = 0;   // counts drags of edges, so that late answers are dropped
		uint64_t m_dragEvent = 0;       // pressed on
		bool m_dragMoved = false;
		double m_dragDy = 0.0;             // dp, dragging the event volume or a curve point
		double m_volumeDy = 0.0;           // dp from the press to the mouse, dragging the event volume
		std::optional<size_t> m_dragPoint; // the curve point dragged, none for a new one
		std::vector<std::pair<double, double>> m_drawnCurve; // with Ctrl: where the drag went, in dp (#670)
		// Moving events, after the drag delay; a RangeMove drag, once across or on other tracks.
		bool m_dragStarted = false;
		bool m_dragToggles = false;         // a Ctrl/Shift click takes the pressed event or point out of the selection
		bool m_clickSelectsPoint = false;   // a click on a point of a larger selection selects only that point
		double m_pressTime = 0.0;           // RmlUi's clock, in seconds
		double m_dragDx = 0.0;              // dp, while moving, snapped
		std::optional<int64_t> m_dragTicks; // a musical move's offset, for each event's preview
		int m_dragLanes = 0;
		// A Ctrl/Shift press whose keys are also a drag's tool modifier, e.g. Copy (#474): a click adds the pressed
		// event to the selection or takes it out, a drag narrows the selection to it first (#1018).
		bool m_clickToggles = false;
		double m_dragLaneY = 0.0; // where a drag of events started, in dp from the top of the first lane
		double m_end = 0.0;       // of the last event
		double m_length = g_defaultProjectLength; // the project setup's
		bool m_loading = false;
		bool m_stale = false;
		int64_t m_cursorSample = 0;
		engine::RequestId m_nudged = 0; // the newest Nudge Cursor's transport.locate
		double m_cursor = 0.0;
		bool m_playing = false;

		TimelineView m_view;
		std::vector<TimelineView> m_undo;
		std::vector<TimelineView> m_redo;
		std::vector<TimelineView> m_clickRedo; // m_redo before the zoom tool's last click, for Alt's double click
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
		WarpMode m_warpMode = WarpMode::Grid;
		uint64_t m_warpSource = 0; // the audio event Copy Warp Markers from Selected Event copied from
		PlayMode m_playMode = PlayMode::Play;
		uint64_t m_auditioned = 0;   // the track the play tool auditions or scrubs while pressed
		bool m_resizeScrubs = false; // the play tool's resize drag, which scrubs m_auditioned
		std::optional<RangeSelection> m_range;
		std::optional<RangeSelection> m_rangeBase; // what a drag with Shift adds to
		bool m_rangeAnew = false;                  // a Shift press in m_rangeBase, which a drag replaces
		std::optional<RangeSelection> m_dragRange; // the range a RangeMove or RangeEdge drag changes, as it was
		RangeEdge m_rangeEdge = RangeEdge::None;   // the edge a RangeEdge drag moves
		double m_pressX = 0.0;                     // dp in the event lanes
		double m_pressY = 0.0;
		std::string m_pressed; // the modifiers held at the press: ctrl, alt and shift joined with +
		std::map<std::string, std::string> m_toolModifiers;
		bool m_modifiersKnown = false; // changed since they were asked for
		Rml::Element* m_rangeElement = nullptr;
		Rml::Element* m_rectangle = nullptr;
		std::vector<Rml::Element*> m_toolButtons;  // the document's
		std::vector<Rml::Element*> m_stateButtons; // the toolbar's Deactivate All buttons (button[deactivate])
		Rml::Element* m_combineButton = nullptr;
		Rml::Element* m_objectMenu = nullptr;
		Rml::Element* m_lineMenu = nullptr;
		Rml::Element* m_timeWarpMenu = nullptr;
		Rml::Element* m_timeWarpToolbarMenu = nullptr;
		Rml::Element* m_playMenu = nullptr;
		Rml::Element* m_toolbox = nullptr;
		Rml::Element* m_contextMenu = nullptr;
		Rml::Element* m_notification = nullptr;
		Rml::Element* m_refusal = nullptr;  // #edit-refused
		std::set<uint64_t> m_sliceJobs;     // Close Gaps jobs started by this display
		bool m_toolboxOnRightClick = false; // ui.toolboxOnRightClick
		bool m_toolboxKnown = false;        // changed since it was asked for
		bool m_notifies = true;             // ui.toolSwitchNotification
		bool m_notifiesKnown = false;
		bool m_zoomHorizontalOnly = true; // ui.zoomToolHorizontalOnly
		bool m_zoomHorizontalOnlyKnown = false;
		bool m_treatMutedAudioLikeDeleted = true; // editing.treatMutedAudioLikeDeleted
		bool m_treatMutedKnown = false;
		bool m_wheelVolume = false; // editing.wheelEventVolumeAndFades
		bool m_wheelVolumeKnown = false;
		bool m_rangeSplits = false; // editing.rangeToolSplitsPartlySelectedEvents
		bool m_rangeSplitsKnown = false;
		std::optional<std::pair<double, double>> m_mouse; // over the lanes, in dp, while the mouse is there
		std::string m_held;           // the modifier keys held, as modifiersOf joins them, for the pointer
		std::set<int> m_modifierKeys; // the modifier keys seen going down and not up (Rml::Input::KeyIdentifier)
		std::vector<CycleMarker> m_cycleMarkers;
		std::vector<int64_t> m_markers; // every marker track's markers' positions and ends, in samples, for snapping
		engine::SnapSettings m_snap;
		int m_formatsSent = 0; // snap.set {format} from the ruler or the grid type menu that isn't answered yet
		engine::midi::QuantizeSettings m_quantize;
		Rml::Element* m_snapButton = nullptr;
		Rml::Element* m_snapTypeButton = nullptr;
		Rml::Element* m_snapTypeMenu = nullptr;
		Rml::Element* m_gridTypeButton = nullptr;
		Rml::Element* m_gridTypeMenu = nullptr;
		Rml::Element* m_zeroCrossingButton = nullptr;
		Rml::Element* m_rootKeyButton = nullptr; // the Project Root Key (#168)
		Rml::Element* m_rootKeyMenu = nullptr;
		Rml::ObserverPtr<Rml::Element> m_dialog; // the dialog document open now
		bool m_dialogForObjects = false;         // whether it is for objects of the project, which project.reset closes
		// What the Tempo Detection dialog analyzes, and how its buttons changed the result.
		struct TempoDetection
		{
			uint64_t event = 0; // an audio event or a MIDI part
			double factor = 1.0;
			bool offbeat = false;
			bool smooth = false;
		};
		TempoDetection m_detection;
		engine::Json m_organized;      // the presets that the organize dialog lists, by index
		Import m_import;               // the files the Import Options dialog imports
		ImportOptions m_importOptions; // the options it showed when it opened
		bool m_importSaved = false;    // the project had a file then

		std::map<uint64_t, std::vector<uint64_t>> m_openAutomation; // the lanes each track shows, in order
		std::map<uint64_t, AutomationView> m_automation;            // every lane of the project, and the unadded ones
		std::map<uint64_t, engine::Json> m_unadded;                 // the unadded lanes that show, by id
		uint64_t m_nextUnadded = uint64_t{1} << 52;                 // above the project's ids, exact as a double
		std::map<uint64_t, engine::Json> m_targets;                 // automationLane.targets, by track
		std::map<uint64_t, std::string> m_targetsKeys; // by track, what its targets depended on when last asked
		std::vector<uint64_t> m_laneAutomation;        // each display lane's automation lane, or 0
		std::vector<int> m_laneTakes;                  // each display lane's track lane, or -1
		std::set<uint64_t> m_lanesShown;               // the tracks whose lanes show
		std::map<uint64_t, std::set<uint64_t>> m_selectedPoints;
		Rml::Element* m_automationArea = nullptr; // the curves, a .automation-lane each
		Rml::Element* m_editor = nullptr;         // .automation-editor around the selected points
		AutomationGesture m_gesture = AutomationGesture::None;
		size_t m_gestureLane = 0; // the display lane pressed
		EditorControl m_control = EditorControl::MoveVertically;
		std::vector<CurvePoint> m_drawn;                     // the draw tool's path, seconds and display values
		std::map<uint64_t, std::set<uint64_t>> m_pointsBase; // the selection a rectangle with Ctrl or Shift adds to

		std::map<std::string, std::function<void()>> m_actions;
		std::map<std::string, bool> m_rangeActions; // the actions that need a range (canRun), whether with a length
		bool m_copied = false;                      // whether the range clipboard has something to paste (canRun)
		std::vector<std::function<void()>> m_viewListeners;
		std::vector<std::function<void()>> m_selectionListeners; // onSelectionShown
		std::vector<Subscription> m_subscriptions;
		std::optional<Subscription> m_position; // while playing
	};
}
