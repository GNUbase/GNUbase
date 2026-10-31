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
		// ponytail: nudges by a quarter note; the snap grid (#87) will set the step.
		constexpr int64_t g_nudge = time::g_ticksPerQuarter;

		bool held(Rml::Event& event, const char* key)
		{
			return event.GetParameter<int>(key, 0) != 0;
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
		// The selection's earliest start or latest end, in seconds.
		const auto edge = [this](bool end)
		{
			double result = end ? 0.0 : 1e300;
			for (const Shown& shown : m_events)
				if (std::binary_search(m_selection.begin(), m_selection.end(), shown.id))
					result = end ? std::max(result, shown.end) : std::min(result, shown.start);
			return result;
		};
		// Without a selection, a split cuts every event at the positions (reference manual p. 268).
		const auto split = [this](Json positions)
		{
			Json parameters = {{"positions", std::move(positions)}};
			if (!m_selection.empty())
				parameters["events"] = m_selection;
			edit("event.split", std::move(parameters));
		};
		const auto nudge = [onSelection](const char* edgeName, int64_t ticks)
		{
			return edgeName == nullptr ? onSelection("event.move", {{"ticks", ticks}})
									   : onSelection("event.resize", {{"edge", edgeName}, {"ticks", ticks}});
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
			{"edit.nudgeLeft", nudge(nullptr, -g_nudge)},
			{"edit.nudgeRight", nudge(nullptr, g_nudge)},
			{"edit.nudgeStartLeft", nudge("start", -g_nudge)},
			{"edit.nudgeStartRight", nudge("start", g_nudge)},
			{"edit.nudgeEndLeft", nudge("end", -g_nudge)},
			{"edit.nudgeEndRight", nudge("end", g_nudge)},
			{"edit.moveStartToCursor",
				[this, edge]
				{
					if (!m_selection.empty())
						edit("event.move", {{"events", m_selection}, {"delta", m_cursorSample - samples(edge(false))}});
				}},
			{"edit.moveEndToCursor",
				[this, edge]
				{
					if (!m_selection.empty())
						edit("event.move", {{"events", m_selection}, {"delta", m_cursorSample - samples(edge(true))}});
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

	void EventDisplay::areaDown(Rml::Event& event, double x, double y)
	{
		m_dragMoved = false;
		m_dragDx = 0.0;
		m_dragLanes = 0;
		const bool add = held(event, "ctrl_key") || held(event, "meta_key") || held(event, "shift_key");
		const Shown* shown = eventAt(x, y);
		if (shown == nullptr)
		{
			if (!add)
				select({});
			return;
		}
		m_dragEvent = shown->id;
		const bool selected = std::binary_search(m_selection.begin(), m_selection.end(), shown->id);
		if (add && !held(event, "alt_key"))
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
		const double left = (shown->start - m_view.start) / m_view.scale;
		const double right = (shown->end - m_view.start) / m_view.scale;
		const bool wide = right - left > 3.0 * g_edge;
		if (wide && x - left <= g_edge)
			m_drag = Drag::ResizeStart;
		else if (wide && right - x <= g_edge)
			m_drag = Drag::ResizeEnd;
		else
			m_drag = Drag::Events;
	}

	void EventDisplay::areaMove(double dx, double dy)
	{
		if (!m_dragMoved && std::abs(dx) < g_dragThreshold && std::abs(dy) < g_dragThreshold)
			return;
		m_dragMoved = true;
		if (m_drag != Drag::Events)
			return;
		m_dragDx = dx;
		m_dragLanes = static_cast<int>(std::lround(dy / (g_trackHeight * m_view.trackZoom)));
		setLanes();
	}

	void EventDisplay::areaUp(Rml::Event& event)
	{
		const Drag drag = m_drag;
		if (drag != Drag::Events && drag != Drag::ResizeStart && drag != Drag::ResizeEnd)
			return;
		const bool moved = std::exchange(m_dragMoved, false);
		const int lanes = std::exchange(m_dragLanes, 0);
		m_drag = Drag::None;
		m_dragDx = 0.0;
		setLanes(); // without the drag's preview
		const bool alt = held(event, "alt_key");
		if (!moved)
		{
			// Alt and a click splits the event there (reference manual p. 268).
			if (alt && alive())
				edit("event.split",
					{{"events", {m_dragEvent}},
						{"positions", {samples(m_view.start + (dpFrom(*m_lanes, m_dragX) * m_view.scale))}}});
			return;
		}
		const double dx = (event.GetParameter<float>("mouse_x", 0.0F) - m_dragX) / dpRatio();
		Json parameters = {{"events", m_selection}, {"delta", samples(dx * m_view.scale)}};
		if (drag != Drag::Events)
		{
			parameters["edge"] = drag == Drag::ResizeStart ? "start" : "end";
			parameters["mode"] = m_sizingMovesContents ? "moveContents" : "normal";
			edit("event.resize", std::move(parameters));
			return;
		}
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
		// Alt and a drag copies, with Shift too shared copies (reference manual pp. 270-271).
		if (alt)
		{
			parameters["shared"] = held(event, "shift_key");
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
		const double row = (y + m_view.top) / (g_trackHeight * m_view.trackZoom);
		if (row < 0.0 || eventAt(x, y) != nullptr)
			return;
		const auto lane = static_cast<size_t>(row);
		// Below the tracks, a new MIDI track (reference manual p. 198, the track list's empty area, #80).
		if (lane >= m_laneTypes.size())
		{
			m_binding.send("track.add", {{"type", "midi"}});
			return;
		}
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
