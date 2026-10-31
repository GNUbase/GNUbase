// SPDX-License-Identifier: GPL-3.0-or-later
// Editing parts and events in the event display (#85, reference manual pp. 245-277): the selection, the mouse
// gestures of the object selection tool and the edit commands, all sent to the engine as event.* commands.
// docs/features/parts-and-events.md describes them.
#include "engine/time/tempomap.h"
#include "platform/log.h"
#include "ui/eventdisplay.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		constexpr double g_edge = 6.0;          // dp at an event's left and right edge that resize it
		constexpr double g_dragThreshold = 3.0; // dp the mouse moves before a press becomes a drag
		constexpr double g_magnet = 8.0;        // dp from which events, markers and the cursor attract
		constexpr double g_minGridWidth = 12.0; // dp: adapt to zoom's finest note value is at least this wide

		bool held(Rml::Event& event, const char* key)
		{
			return event.GetParameter<int>(key, 0) != 0;
		}

		// The modifiers held, as the user setting ui.toolModifiers writes them: ctrl (Ctrl, Cmd on macOS), alt and
		// shift, joined with +.
		std::string modifiersOf(Rml::Event& event)
		{
			std::string keys;
			const auto add = [&keys](bool on, const char* key)
			{
				if (on)
					keys += (keys.empty() ? "" : "+") + std::string(key);
			};
			add(held(event, "ctrl_key") || held(event, "meta_key"), "ctrl");
			add(held(event, "alt_key"), "alt");
			add(held(event, "shift_key"), "shift");
			return keys;
		}

		bool inside(const Rml::Element* element, const Rml::Element* ancestor)
		{
			for (; element != nullptr; element = element->GetParentNode())
				if (element == ancestor)
					return true;
			return false;
		}
	}

	int64_t EventDisplay::samples(double seconds) const
	{
		return std::llround(seconds * m_time.sampleRate);
	}

	engine::SnapGrid EventDisplay::snapGrid() const
	{
		engine::SnapGrid grid;
		grid.tempo = m_time.tempo;
		grid.signatures = m_time.signatures;
		grid.sampleRate = m_time.sampleRate;
		grid.quantize = m_quantize;
		grid.frameRate = m_format == TimeFormat::UserFrames ? time::FrameRate{m_time.userFramesPerSecond, false, false}
															: m_time.frameRate;
		// Adapt to zoom (reference manual p. 107): the finest note value, down to a 64th, at least g_minGridWidth wide
		// at the tempo at the left edge; zoomed out, bars.
		const double quarter = 60.0 / m_time.tempo.bpmAt(m_time.tempo.toTicks(m_view.start)); // seconds
		for (time::Ticks step = 2 * time::g_ticksPerQuarter; step >= time::g_ticksPerQuarter / 16; step /= 2)
			if (static_cast<double>(step) / static_cast<double>(time::g_ticksPerQuarter) * quarter / m_view.scale >=
				g_minGridWidth)
				grid.zoomStep = step;
		return grid;
	}

	time::Position EventDisplay::snapAt(double seconds, std::optional<double> from) const
	{
		engine::SnapTarget target;
		target.position = samples(seconds);
		if (from)
			target.from = samples(*from);
		target.cursor = m_cursorSample;
		target.range = samples(g_magnet * m_view.scale);
		for (const Shown& shown : m_events)
		{
			if (std::binary_search(m_selection.begin(), m_selection.end(), shown.id))
				continue;
			target.magnets.push_back(samples(shown.start));
			target.magnets.push_back(samples(shown.end));
			if (shown.snapPoint)
				target.magnets.push_back(samples(*shown.snapPoint));
		}
		target.magnets.insert(target.magnets.end(), m_markers.begin(), m_markers.end());
		return engine::snap(m_snap, snapGrid(), target);
	}

	std::pair<Json, double> EventDisplay::shiftTo(const time::Position& to, double from) const
	{
		const bool musical = to.base == time::TimeBase::Musical &&
			std::none_of(m_events.begin(), m_events.end(),
				[this](const Shown& shown)
				{
					return m_laneLinear[shown.lane] &&
						std::binary_search(m_selection.begin(), m_selection.end(), shown.id);
				});
		if (musical)
			return {{{"ticks", to.value - m_time.tempo.toTicks(from)}}, m_time.tempo.toSeconds(to.value) - from};
		const int64_t sample = time::toSamples(to, m_time.tempo, m_time.sampleRate);
		return {{{"delta", sample - samples(from)}}, (static_cast<double>(sample) / m_time.sampleRate) - from};
	}

	void EventDisplay::select(std::vector<uint64_t> events)
	{
		std::sort(events.begin(), events.end());
		events.erase(std::unique(events.begin(), events.end()), events.end());
		m_selection = std::move(events);
		setLanes();
		showSelection();
	}

	void EventDisplay::selectWhere(const std::function<bool(const Shown&)>& selected)
	{
		std::vector<uint64_t> events;
		for (const Shown& shown : m_events)
			if (selected(shown))
				events.push_back(shown.id);
		select(std::move(events));
	}

	void EventDisplay::showSelection()
	{
		if (!alive())
			return;
		std::string ids;
		for (const uint64_t id : m_selection)
			ids += (ids.empty() ? "" : " ") + std::to_string(id);
		m_root->SetAttribute("selection", ids);
	}

	void EventDisplay::edit(const std::string& command, Json parameters, bool selectResult)
	{
		m_binding.send(command, std::move(parameters),
			[this, selectResult, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired())
					return;
				if (!result.ok())
					logging::warning("An edit of events failed: {}", result.error);
				else if (selectResult)
					select(result.value.at("events").get<std::vector<uint64_t>>());
			});
	}

	void EventDisplay::repeat(int count, bool shared)
	{
		if (!m_selection.empty())
			edit("event.duplicate", {{"events", m_selection}, {"count", count}, {"shared", shared}}, true);
	}

	void EventDisplay::addEditActions()
	{
		// An event command for the selection, if there is one.
		const auto onSelection = [this](const char* command, const Json& parameters, bool selectResult = false)
		{
			return [this, command, parameters, selectResult]
			{
				if (m_selection.empty())
					return;
				Json withEvents = parameters;
				withEvents["events"] = m_selection;
				edit(command, std::move(withEvents), selectResult);
			};
		};
		// The selection's earliest start or latest end, in seconds; none while the display doesn't show it yet.
		const auto edge = [this](bool end)
		{
			std::optional<double> result;
			for (const Shown& shown : m_events)
				if (std::binary_search(m_selection.begin(), m_selection.end(), shown.id))
				{
					const double at = end ? shown.end : shown.start;
					if (!result)
						result = at;
					else
						result = end ? std::max(*result, at) : std::min(*result, at);
				}
			return result;
		};
		// Without a selection, a split cuts every event at the positions (reference manual p. 268).
		const auto split = [this](Json positions)
		{
			Json parameters = {{"positions", std::move(positions)}, {"zeroCrossing", m_snap.zeroCrossing}};
			if (!m_selection.empty())
				parameters["events"] = m_selection;
			edit("event.split", std::move(parameters));
		};
		// Nudging moves by a grid step at the selection's start (reference manual p. 107), snap on or off.
		const auto nudge = [this, edge](const char* edgeName, int direction)
		{
			return [this, edge, edgeName, direction]
			{
				if (m_selection.empty())
					return;
				// A selection the display doesn't show yet takes the step at the project's start.
				const time::Position step =
					engine::gridStep(m_snap.grid, snapGrid(), samples(edge(false).value_or(0.0)));
				Json parameters = {{"events", m_selection},
					{step.base == time::TimeBase::Musical ? "ticks" : "delta", direction * step.value}};
				if (edgeName != nullptr)
					parameters["edge"] = edgeName;
				edit(edgeName == nullptr ? "event.move" : "event.resize", std::move(parameters));
			};
		};
		const std::map<std::string, std::function<void()>> actions = {
			{"edit.delete", onSelection("event.remove", Json::object())},
			{"edit.mute", onSelection("event.set", {{"mute", true}})},
			{"edit.unmute", onSelection("event.set", {{"mute", false}})},
			{"edit.lock", onSelection("event.set", {{"lock", true}})},
			{"edit.unlock", onSelection("event.set", {{"lock", false}})},
			{"edit.splitAtCursor", [this, split] { split(Json::array({m_cursorSample})); }},
			{"edit.splitLoop", [this, split] { split(Json::array({m_left, m_right})); }},
			{"edit.glue", onSelection("event.glue", Json::object())},
			{"edit.duplicate", [this] { repeat(1, false); }},
			{"edit.repeat",
				[this]
				{
					if (!m_selection.empty())
						openDialog("repeat-events.rml");
				}},
			{"edit.fillLoop", onSelection("event.fillLoop", Json::object())},
			{"edit.toRealCopy", onSelection("event.toRealCopy", Json::object())},
			{"edit.nudgeLeft", nudge(nullptr, -1)},
			{"edit.nudgeRight", nudge(nullptr, 1)},
			{"edit.nudgeStartLeft", nudge("start", -1)},
			{"edit.nudgeStartRight", nudge("start", 1)},
			{"edit.nudgeEndLeft", nudge("end", -1)},
			{"edit.nudgeEndRight", nudge("end", 1)},
			{"edit.snap", [this] { setSnap({{"on", !m_snap.on}}); }},
			// Snap Point to Cursor (reference manual p. 105): the selected audio events that the cursor is in.
			{"edit.snapPointToCursor",
				[this]
				{
					std::vector<uint64_t> events;
					for (const Shown& shown : m_events)
						if (m_laneTypes[shown.lane] == "audio" && shown.start <= m_cursor && m_cursor < shown.end &&
							std::binary_search(m_selection.begin(), m_selection.end(), shown.id))
							events.push_back(shown.id);
					if (!events.empty())
						edit("event.setSnapPoint", {{"events", events}, {"position", m_cursorSample}});
				}},
			{"edit.moveStartToCursor",
				[this, edge]
				{
					if (const std::optional<double> at = edge(false))
						edit("event.move", {{"events", m_selection}, {"delta", m_cursorSample - samples(*at)}});
				}},
			{"edit.moveEndToCursor",
				[this, edge]
				{
					if (const std::optional<double> at = edge(true))
						edit("event.move", {{"events", m_selection}, {"delta", m_cursorSample - samples(*at)}});
				}},
			// The select submenu (reference manual pp. 254-255); without track selection (#81), on every track.
			{"edit.selectAll", [this] { selectWhere([](const Shown&) { return true; }); }},
			{"edit.selectNone", [this] { select({}); }},
			{"edit.invertSelection",
				[this]
				{
					selectWhere([this](const Shown& shown)
						{ return !std::binary_search(m_selection.begin(), m_selection.end(), shown.id); });
				}},
			{"edit.selectInLoop",
				[this]
				{
					const double left = static_cast<double>(m_left) / m_time.sampleRate;
					const double right = static_cast<double>(m_right) / m_time.sampleRate;
					selectWhere([left, right](const Shown& shown) { return shown.end > left && shown.start < right; });
				}},
			{"edit.selectFromStartToCursor",
				[this] { selectWhere([this](const Shown& shown) { return shown.end <= m_cursor; }); }},
			{"edit.selectFromCursorToEnd",
				[this] { selectWhere([this](const Shown& shown) { return shown.start >= m_cursor; }); }},
			{"edit.selectUnderCursor",
				[this]
				{
					selectWhere([this](const Shown& shown) { return shown.start <= m_cursor && shown.end > m_cursor; });
				}},
		};
		m_actions.insert(actions.begin(), actions.end());
	}

	const EventDisplay::Shown* EventDisplay::eventAt(double x, double y) const
	{
		const double row = (y + m_view.top) / (g_trackHeight * m_view.trackZoom);
		if (row < 0.0)
			return nullptr;
		const auto lane = static_cast<size_t>(row);
		const double time = m_view.start + (x * m_view.scale);
		const Shown* found = nullptr; // the last one is drawn on top
		for (const Shown& shown : m_events)
			if (shown.lane == lane && time >= shown.start && time < shown.end)
				found = &shown;
		return found;
	}

	std::optional<size_t> EventDisplay::laneAt(double y) const
	{
		const double row = (y + m_view.top) / (g_trackHeight * m_view.trackZoom);
		if (row < 0.0 || row >= static_cast<double>(m_laneTracks.size()))
			return std::nullopt;
		return static_cast<size_t>(row);
	}

	void EventDisplay::areaDown(Rml::Event& event, double x, double y)
	{
		m_dragMoved = false;
		m_dragDx = 0.0;
		m_dragLanes = 0;
		m_pressX = x;
		m_pressY = y;
		m_pressed = modifiersOf(event);
		if (toolDown(x, y))
			return;
		const bool add = held(event, "ctrl_key") || held(event, "meta_key") || held(event, "shift_key");
		const Shown* shown = eventAt(x, y);
		if (shown == nullptr)
		{
			// A rectangle selects what it touches (with Ctrl or Shift, adds it); a click selects nothing.
			if (!add)
				select({});
			m_drag = Drag::Rectangle;
			return;
		}
		m_dragEvent = shown->id;
		const bool selected = std::binary_search(m_selection.begin(), m_selection.end(), shown->id);
		const double left = (shown->start - m_view.start) / m_view.scale;
		const double right = (shown->end - m_view.start) / m_view.scale;
		const bool wide = right - left > 3.0 * g_edge;
		const bool atStart = wide && x - left <= g_edge;
		const bool atEnd = wide && !atStart && right - x <= g_edge;
		// An edge with the modifier Disable Snapping resizes rather than adding to the selection.
		if (add && !held(event, "alt_key") && !((atStart || atEnd) && modifiers("sizeObjects.disableSnapping")))
		{
			// Ctrl or Shift and a click adds an event to the selection or takes it out.
			std::vector<uint64_t> events = m_selection;
			if (selected)
				events.erase(std::find(events.begin(), events.end(), shown->id));
			else
				events.push_back(shown->id);
			select(std::move(events));
			return;
		}
		if (!selected)
			select({shown->id});
		if (atStart)
			m_drag = Drag::ResizeStart;
		else if (atEnd)
			m_drag = Drag::ResizeEnd;
		else
			m_drag = Drag::Events;
	}

	void EventDisplay::areaMove(double dx, double dy)
	{
		if (!m_dragMoved && std::abs(dx) < g_dragThreshold && std::abs(dy) < g_dragThreshold)
			return;
		m_dragMoved = true;
		if (m_drag != Drag::Events && m_drag != Drag::ResizeStart && m_drag != Drag::ResizeEnd)
			toolMove(m_pressX + dx, m_pressY + dy);
		if (m_drag != Drag::Events)
			return;
		m_dragDx = moveShift(dx).second / m_view.scale;
		m_dragLanes = static_cast<int>(std::lround(dy / (g_trackHeight * m_view.trackZoom)));
		setLanes();
	}

	// Where a drag dx dp across moves the selection: the pressed event's snap point, or its start, snaps (reference
	// manual p. 105), and the others keep their distance to it. Shuffle moves them unsnapped, as event.shuffle
	// places them.
	std::pair<Json, double> EventDisplay::moveShift(double dx) const
	{
		const auto pressed = std::find_if(
			m_events.begin(), m_events.end(), [this](const Shown& shown) { return shown.id == m_dragEvent; });
		const double from = pressed == m_events.end() ? 0.0 : pressed->snapPoint.value_or(pressed->start);
		return shiftTo(snapAt(from + (dx * m_view.scale), from), from);
	}

	void EventDisplay::areaUp(Rml::Event& event)
	{
		const Drag drag = m_drag;
		if (!alive())
			return;
		m_pressed = modifiersOf(event); // what is held now decides, e.g. whether a drag copies
		if (drag == Drag::Rectangle || drag == Drag::Range || drag == Drag::DrawPart || drag == Drag::Play ||
			drag == Drag::Click)
		{
			toolUp(dpFrom(*m_lanes, event.GetParameter<float>("mouse_x", 0.0F)),
				(event.GetParameter<float>("mouse_y", 0.0F) - m_lanes->GetAbsoluteTop()) / dpRatio());
			m_dragMoved = false;
			m_drag = Drag::None;
			return;
		}
		if (drag != Drag::Events && drag != Drag::ResizeStart && drag != Drag::ResizeEnd)
			return;
		const bool moved = std::exchange(m_dragMoved, false);
		const int lanes = std::exchange(m_dragLanes, 0);
		m_drag = Drag::None;
		m_dragDx = 0.0;
		setLanes(); // without the drag's preview
		if (!moved)
		{
			const double at = m_view.start + (dpFrom(*m_lanes, m_dragX) * m_view.scale);
			// The tool modifier Set Cursor Position moves the project cursor there.
			if (modifiers("select.setCursorPosition"))
			{
				m_binding.send("transport.locate", {{"position", samples(at)}});
				return;
			}
			// The tool modifier Split Event (Alt) and a click splits the event there (reference manual p. 268),
			// snapped.
			if (modifiers("select.splitEvent"))
			{
				const time::Position snapped = snapAt(at, std::nullopt);
				edit("event.split",
					{{"events", {m_dragEvent}},
						{"positions", {time::toSamples(snapped, m_time.tempo, m_time.sampleRate)}},
						{"zeroCrossing", m_snap.zeroCrossing}});
			}
			return;
		}
		const double dx = (event.GetParameter<float>("mouse_x", 0.0F) - m_dragX) / dpRatio();
		if (drag != Drag::Events)
		{
			// The pressed event's edge snaps, unless the tool modifier Disable Snapping is held; the selection's
			// edges move as far.
			const auto pressed = std::find_if(
				m_events.begin(), m_events.end(), [this](const Shown& shown) { return shown.id == m_dragEvent; });
			if (pressed == m_events.end())
				return;
			const double from = drag == Drag::ResizeStart ? pressed->start : pressed->end;
			const double to = from + (dx * m_view.scale);
			Json parameters =
				shiftTo(modifiers("sizeObjects.disableSnapping") ? time::Position{time::TimeBase::Linear, samples(to)}
																 : snapAt(to, from),
					from)
					.first;
			parameters["events"] = m_selection;
			parameters["edge"] = drag == Drag::ResizeStart ? "start" : "end";
			parameters["mode"] = m_sizingMovesContents ? "moveContents" : "normal";
			parameters["zeroCrossing"] = m_snap.zeroCrossing;
			edit("event.resize", std::move(parameters));
			return;
		}
		const bool shared = modifiers("dragDrop.sharedCopy");
		const bool copy = shared || modifiers("dragDrop.copy");
		// Shuffle (reference manual p. 106): the selection trades places with the events it is dragged among.
		if (m_snap.on && m_snap.type == engine::SnapType::Shuffle && !copy && lanes == 0)
		{
			double start = 1e300;
			for (const Shown& shown : m_events)
				if (std::binary_search(m_selection.begin(), m_selection.end(), shown.id))
					start = std::min(start, shown.start);
			edit("event.shuffle", {{"events", m_selection}, {"position", samples(start + (dx * m_view.scale))}});
			return;
		}
		Json parameters = moveShift(dx).first;
		parameters["events"] = m_selection;
		// Onto another track when the selection is on one track.
		// ponytail: a selection on several tracks moves only along them; one transaction per track would move it.
		if (lanes != 0)
		{
			size_t lane = 0;
			bool oneLane = true;
			bool first = true;
			for (const Shown& shown : m_events)
				if (std::binary_search(m_selection.begin(), m_selection.end(), shown.id))
				{
					oneLane = oneLane && (first || shown.lane == lane);
					lane = shown.lane;
					first = false;
				}
			const auto target = static_cast<int64_t>(lane) + lanes;
			if (oneLane && target >= 0 && target < static_cast<int64_t>(m_laneTracks.size()))
				parameters["track"] = m_laneTracks[static_cast<size_t>(target)];
		}
		// The tool modifiers Copy (Alt) and Shared Copy (Alt and Shift) copy (reference manual pp. 270-271).
		if (copy)
		{
			parameters["shared"] = shared;
			edit("event.copy", std::move(parameters), true);
		}
		else
		{
			edit("event.move", std::move(parameters));
		}
	}

	void EventDisplay::doubleClick(Rml::Event& event)
	{
		if (!alive() || !inside(event.GetTargetElement(), m_area))
			return;
		const double x = dpFrom(*m_lanes, event.GetParameter<float>("mouse_x", 0.0F));
		const double y = (event.GetParameter<float>("mouse_y", 0.0F) - m_lanes->GetAbsoluteTop()) / dpRatio();
		m_pressed = modifiersOf(event);
		if (m_pressTool != Tool::ObjectSelection)
		{
			if (toolDoubleClick(x, y))
				m_drag = Drag::None; // the second press's release does nothing more
			return;
		}
		const std::optional<size_t> found = laneAt(y);
		if (!found || eventAt(x, y) != nullptr)
			return;
		const size_t lane = *found;
		if (m_laneTypes[lane] != "midi" && m_laneTypes[lane] != "instrument")
			return;
		// A one-bar part where the click is (reference manual p. 251), or one between the locators.
		const double seconds = m_view.start + (x * m_view.scale);
		const int64_t sample = samples(seconds);
		time::Ticks start = 0;
		time::Ticks end = 0;
		if (m_left < m_right && sample >= m_left && sample < m_right)
		{
			start = m_time.tempo.toTicks(m_left, m_time.sampleRate);
			end = m_time.tempo.toTicks(m_right, m_time.sampleRate);
		}
		else
		{
			const int64_t bar = m_time.signatures.toBarsBeats(m_time.tempo.toTicks(seconds)).bar;
			start = m_time.signatures.barStart(bar);
			end = m_time.signatures.barStart(bar + 1);
		}
		m_drag = Drag::None;
		addPart(lane, start, end);
	}

	void EventDisplay::addPart(size_t lane, time::Ticks start, time::Ticks end)
	{
		int64_t position = start;
		int64_t length = end - start;
		if (m_laneLinear[lane])
		{
			position = m_time.tempo.toSamples(start, m_time.sampleRate);
			length = m_time.tempo.toSamples(end, m_time.sampleRate) - position;
		}
		m_binding.send("midiPart.add", {{"track", m_laneTracks[lane]}, {"position", position}, {"length", length}},
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (!alive.expired() && result.ok())
					select({result.value.at("part").at("id").get<uint64_t>()});
			});
	}
}
