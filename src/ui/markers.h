// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/canvas.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/timeformat.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>
#include <RmlUi/Core/Types.h>

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
	class ElementDocument;
	class ElementFormControl;
}

// Markers in the UI (#91, reference manual pp. 422-431): marker events on the marker tracks' lanes and marker lines
// across the event display, and the markers window. The markers are the engine's (marker.* commands);
// docs/features/markers.md describes both.
namespace gnubase::ui
{
	// A marker as the UI shows it, at its time in seconds from the project start.
	struct MarkerItem
	{
		uint64_t id = 0;
		uint64_t track = 0;
		bool cycle = false;
		int number = 0;
		std::string name;
		double start = 0.0;
		double end = 0.0; // a cycle marker's; a position marker's is its start
	};

	// The project's markers as project.get returns them: its time, every track's id and whether it is a marker track
	// in the project's order, each marker track's name, and the markers in timeline order.
	struct ProjectMarkers
	{
		struct Track
		{
			uint64_t id = 0;
			bool marker = false;
			bool linear = false; // a marker track's time base
			std::string name;
		};

		TimeContext time;
		std::vector<Track> tracks;
		uint64_t active = 0; // the active marker track, 0 if there is none
		std::vector<MarkerItem> markers;

		// The markers of a marker track, in timeline order.
		std::vector<MarkerItem> of(uint64_t track) const;
		// A time in seconds as a position in the track's time base, as marker commands take it.
		int64_t position(const Track& track, double seconds) const;
	};

	ProjectMarkers readMarkers(const engine::Json& project);

	// Which markers the event display shows as lines across every track: the user setting ui.markerLines.
	enum class MarkerLineMode : uint8_t
	{
		Active, // the active marker track's
		All,    // every marker track's
		Off,
	};

	// Draws the markers into an event display (ui/eventdisplay.h) and follows the project through the binding: marker
	// lines behind the events of every track (<event-lanes class="marker-lines">), the marker events on the marker
	// tracks' lanes over them (<event-lanes class="marker-events">: position markers as lines, cycle markers as spans)
	// with a label each (.marker-label: with the user setting ui.markerIds the number, in brackets for cycle markers,
	// and the name). On a marker track's lane (reference manual pp. 424-425, 431), a click selects the marker under
	// the mouse (its label gets .selected; Ctrl adds or removes it) and a drag moves the selected markers, cycle
	// markers by their label or upper border, onto another marker track too; a drag at a cycle marker's start or end
	// resizes it; Ctrl and a drag adds a cycle marker, Alt-click a position marker (with Shift each opens its name in
	// .marker-name-field to type); a double click on a cycle marker sets the locators to it, and with Alt zooms to it.
	// The erase tool deletes the marker clicked, with Alt every later one on its track; with the range tool a double
	// click selects the range between the markers around it on every track. Moves and new markers snap. Use it on the
	// UI thread; the display, the engine and the binding must outlive it.
	class MarkerLines final : private Rml::EventListener
	{
	public:
		MarkerLines(EventDisplay& display, EngineBinding& binding);
		~MarkerLines() override;
		MarkerLines(const MarkerLines&) = delete;
		MarkerLines& operator=(const MarkerLines&) = delete;
		MarkerLines(MarkerLines&&) = delete;
		MarkerLines& operator=(MarkerLines&&) = delete;

		const ProjectMarkers& markers() const { return m_markers; }
		MarkerLineMode mode() const { return m_mode; }
		bool showsIds() const { return m_showIds; }
		// Whether the project is being read again: each answer drops the selected markers it no longer has.
		bool loading() const { return m_loading; }

		// The ids of the selected markers. setSelection selects others; onSelect is told of the user's clicks and of
		// deselect, which AppUi calls when an event is selected: the project window selects events or markers.
		const std::vector<uint64_t>& selection() const { return m_selection; }
		void setSelection(std::vector<uint64_t> markers);
		void deselect();
		void onSelect(std::function<void(const std::vector<uint64_t>&)> handler) { m_onSelect = std::move(handler); }
		// Selects markers as a click does, telling onSelect, e.g. the track list's Locate menu.
		void pick(std::vector<uint64_t> markers);
		// Opens the marker's name to type, once the project has the marker: Enter renames it (marker.set).
		void rename(uint64_t marker);
		// Removes the selected markers (Delete); false if none is selected.
		bool removeSelected();

	private:
		// What a press on a marker track's lane is on.
		enum class Part : uint8_t
		{
			None,
			Marker, // a position marker or a label: a drag moves it
			Border, // a cycle marker's upper border: a drag moves it
			Start,  // a cycle marker's start or end handle: a drag resizes it
			End,
			Inside, // a cycle marker below its border: selects it
		};
		struct Hit
		{
			uint64_t marker = 0;
			Part part = Part::None;
		};
		struct LanePoint
		{
			const ProjectMarkers::Track* track = nullptr;
			double seconds = 0.0;
			double y = 0.0; // dp from the lane's top
		};
		enum class Drag : uint8_t
		{
			None,
			Move,
			Start,
			End,
			NewCycle,
		};

		void ProcessEvent(Rml::Event& event) override;
		void press(Rml::Event& event);
		void dragTo(Rml::Event& event, bool release);
		bool alive() const { return static_cast<bool>(m_area); }
		float ratio() const; // dp
		void requestProject();
		// The markers shown: those of the project, or while dragging where the drag puts them.
		const ProjectMarkers& shown() const { return m_preview ? *m_preview : m_markers; }
		void show();
		void showSelection();
		void place();
		void openField();
		void closeField(bool rename);
		Hit hitAt(Rml::Event& event, const LanePoint& at) const;
		void select(Rml::Event& event, uint64_t hit);
		// The marker track's lane under the mouse, the time and the height there, if it is over one.
		std::optional<LanePoint> markerLaneAt(Rml::Event& event) const;

		EventDisplay& m_display;
		EngineBinding& m_binding;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for the binding's answers after it went
		Rml::ObserverPtr<Rml::Element> m_area;
		EventLanes* m_lines = nullptr;
		EventLanes* m_events = nullptr;
		Rml::Element* m_labels = nullptr;
		Rml::ElementFormControl* m_field = nullptr; // the name of the marker being renamed
		ProjectMarkers m_markers;
		std::optional<ProjectMarkers> m_preview;
		Drag m_drag = Drag::None;
		uint64_t m_dragMarker = 0;   // the marker pressed
		uint64_t m_dragTrack = 0;    // the track it was pressed on
		double m_dragFrom = 0.0;     // seconds: the marker's start, end, or the snapped press for a new cycle marker
		double m_pressSeconds = 0.0; // seconds: the press, unsnapped
		float m_pressX = 0.0F;       // the window's pixels
		bool m_dragMoved = false;    // further than a click
		bool m_renameNew = false;    // a new cycle marker opens its name
		uint64_t m_renaming = 0;     // the marker whose name opens, or is open
		bool m_fieldOpen = false;
		MarkerLineMode m_mode = MarkerLineMode::Active;
		bool m_showIds = true;
		bool m_idsKnown = false; // changed since it was asked for
		std::vector<uint64_t> m_selection;
		std::function<void(const std::vector<uint64_t>&)> m_onSelect;
		std::vector<uint64_t> m_laneTracks;
		std::vector<double> m_laneTops;
		bool m_modeKnown = false; // changed since it was asked for
		bool m_loading = false;
		bool m_stale = false;
		std::optional<double> m_lastClick; // RmlUi's time of the last click, for double clicks
		Rml::Vector2f m_lastMouse;
		std::vector<Subscription> m_subscriptions;
	};

	// The markers window (reference manual pp. 426-431): fills a document with the controls of
	// resources/ui/markers.rml and follows the project through the binding. It lists the active marker track's
	// markers in #marker-list, a <listview> with a table body #marker-rows: a row (value: the marker's id) with
	// the locate arrow (the row of the marker at or last before the project cursor has .at-cursor; a click in it
	// locates there), then the columns in the user's order (the user setting ui.markerColumns): the id number, the
	// type, and the position, end, length and description in text fields, times in the transport's primary time
	// format. The header row #marker-head sorts by a column on a click, the position second, moves a column when
	// dragged and resizes one when the line after it is dragged. A description or length typed into one of several
	// selected rows changes them all. #marker-track picks the active marker track; #add-position-marker,
	// #add-cycle-marker and #remove-marker add and remove markers; #marker-functions-menu moves the selected markers
	// to the cursor (#move-to-cursor) and reassigns the ids (#reassign-position-ids, #reassign-cycle-ids). Enter in
	// the list locates to the selected marker. The toggles #marker-auto-scroll and, in #marker-settings-menu,
	// #show-marker-ids, #cycle-follows and #sync-selection turn their user settings on and off. Use it on the UI
	// thread; the engine and the binding must outlive it, and it shows nothing more once its document goes.
	class MarkersWindow final : private Rml::EventListener
	{
	public:
		MarkersWindow(engine::Engine& engine, EngineBinding& binding, Rml::ElementDocument& document);
		~MarkersWindow() override;
		MarkersWindow(const MarkersWindow&) = delete;
		MarkersWindow& operator=(const MarkersWindow&) = delete;
		MarkersWindow(MarkersWindow&&) = delete;
		MarkersWindow& operator=(MarkersWindow&&) = delete;

		// The markers listed: the active marker track's, in the order the list shows them.
		const std::vector<MarkerItem>& markers() const { return m_listed; }
		// The ids of the selected markers. setSelection selects those of the markers listed; onSelect is told of the
		// user's changes.
		std::vector<uint64_t> selection() const;
		void setSelection(const std::vector<uint64_t>& markers);
		void onSelect(std::function<void(const std::vector<uint64_t>&)> handler) { m_onSelect = std::move(handler); }
		// Whether the selection is the project window's too: the user setting ui.markersSyncSelection, but not while
		// recording.
		bool syncsSelection() const { return m_flags.at("ui.markersSyncSelection") && !m_recording; }
		// The project window's selection, which the window takes whenever it starts syncing.
		void syncFrom(std::function<std::vector<uint64_t>()> source);

	private:
		// The user setting ui.markerColumns.
		struct Columns
		{
			static Columns read(const engine::Json& value);
			engine::Json json() const;

			std::vector<std::string> order = {"number", "type", "position", "end", "length", "description"};
			engine::Json widths = engine::Json::object(); // by column, in dp
			std::string sort = "position";
			bool descending = false;
		};

		struct HeaderDrag
		{
			std::string column;
			bool resize = false;
			bool moved = false;
			float x = 0.0F;     // where it started, in the window's pixels
			float width = 0.0F; // the column's then, in dp
		};

		void ProcessEvent(Rml::Event& event) override;
		bool alive() const { return static_cast<bool>(m_document); }
		void resync();
		void applySetting(const std::string& key, const engine::Json& value);
		void requestProject();
		void showHead();
		void show();
		void showCursor();
		void scrollToArrow();
		void click(Rml::Element* target);
		std::optional<HeaderDrag> grab(Rml::Element* target, float x) const;
		float dpRatio() const;
		void header(Rml::Event& event);
		void saveColumns();
		void commit(Rml::Element& field);
		void setCursor(int64_t sample);

		engine::Engine& m_engine;
		EngineBinding& m_binding;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
		Rml::ObserverPtr<Rml::Element> m_document;
		Rml::Element* m_list = nullptr;
		Rml::Element* m_head = nullptr;
		Rml::Element* m_rows = nullptr;
		Rml::Element* m_tracks = nullptr;
		Rml::Element* m_functions = nullptr;
		ProjectMarkers m_markers;
		std::vector<MarkerItem> m_listed;
		int64_t m_cursorSample = 0; // showCursor converts it at the project's current sample rate
		uint64_t m_arrow = 0;       // the marker at the cursor
		bool m_recording = false;
		TimeFormat m_format = TimeFormat::BarsBeats;
		Columns m_columns;
		std::map<std::string, bool> m_flags = {{"ui.markersAutoScroll", true}, {"ui.markersSyncSelection", true},
			{"ui.markerIds", true}, {"transport.cycleFollowsMarkers", false}};
		std::set<std::string> m_settingsKnown; // changed since they were asked for
		std::optional<HeaderDrag> m_drag;
		std::function<void(const std::vector<uint64_t>&)> m_onSelect;
		std::function<std::vector<uint64_t>()> m_syncFrom;
		bool m_loaded = false;     // the project has been read once
		bool m_rebuilding = false; // the rows are being replaced
		bool m_loading = false;
		bool m_stale = false;
		std::vector<Subscription> m_subscriptions;
		std::optional<Subscription> m_position; // while playing
	};
}
