// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
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
#include <string>
#include <vector>

namespace Rml
{
	class Element;
}

// The event display (#84): the project's events on a timeline under a ruler and an overview line, with the project
// cursor, a cross-hair cursor, zoom and scrolling. docs/features/event-display.md describes it.
namespace gnubase::ui
{
	// A track's height before vertical zoom, in dp; tracks get heights of their own with the track list (#81).
	inline constexpr double g_trackHeight = 40.0;
	// What zoom full and the overview line show at least, in seconds, until projects have a length (#78).
	inline constexpr double g_minimumProjectLength = 60.0;

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
	// state class .suspended while editing suspends it; and an auto-scroll settings menu #auto-scroll-menu.
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
		// From the project start to the end of its last event, at least g_minimumProjectLength.
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

		// Auto-scroll (reference manual pp. 316-317). Suspended when editing: a click in the event display while
		// playing suspends it until playback stops or auto-scroll is switched on again.
		void setAutoScroll(bool on);
		bool autoScroll() const { return m_autoScroll; }
		void setAutoScrollMode(AutoScrollMode mode);
		AutoScrollMode autoScrollMode() const { return m_autoScrollMode; }
		void setSuspendAutoScrollWhenEditing(bool suspend);
		bool suspendAutoScrollWhenEditing() const { return m_suspendWhenEditing; }
		bool autoScrollSuspended() const { return m_suspended; }

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

		// The commands that key commands bind to, by id: zoom.in, zoom.out, zoom.inVertically, zoom.outVertically,
		// zoom.full, zoom.undo, zoom.redo and view.autoScroll (on or off).
		const std::map<std::string, std::function<void()>>& actions() const { return m_actions; }

	private:
		enum class Drag : uint8_t
		{
			None,
			Ruler,         // zooms with the distance dragged down or up
			OverviewMove,  // scrolls
			OverviewStart, // the rectangle's left edge
			OverviewEnd,   // its right edge
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
		void showPresets();
		void showAutoScroll();

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
		double m_end = 0.0;                     // of the last event
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
		std::vector<CycleMarker> m_cycleMarkers;
		Rml::ObserverPtr<Rml::Element> m_dialog; // the dialog document open now
		engine::Json m_organized;                // the presets that the organize dialog lists, by index

		std::map<std::string, std::function<void()>> m_actions;
		std::vector<Subscription> m_subscriptions;
		std::optional<Subscription> m_position; // while playing
	};
}
