// SPDX-License-Identifier: GPL-3.0-or-later
// Editing parts and events in the event display (#85, reference manual pp. 245-277): the selection, the mouse
// gestures of the object selection tool and the edit commands, all sent to the engine as event.* commands.
// docs/features/parts-and-events.md describes them.
#include "engine/time/tempomap.h"
#include "platform/log.h"
#include "ui/eventdisplay.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/SystemInterface.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <initializer_list>
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
		// Seconds a press on events is held before a drag moves them (reference manual p. 256), until the
		// preferences have the setting.
		constexpr double g_dragDelay = 0.2;
		constexpr double g_magnet = 8.0;        // dp from which events, markers and the cursor attract
		constexpr double g_minGridWidth = 12.0; // dp: adapt to zoom's finest note value is at least this wide

		bool held(Rml::Event& event, const char* key)
		{
			return event.GetParameter<int>(key, 0) != 0;
		}

		// Modifiers as the user setting ui.toolModifiers writes them: ctrl (Ctrl, Cmd on macOS), alt and shift,
		// joined with +.
		std::string joinedKeys(bool ctrl, bool alt, bool shift)
		{
			std::string keys;
			const auto add = [&keys](bool on, const char* key)
			{
				if (on)
					keys += (keys.empty() ? "" : "+") + std::string(key);
			};
			add(ctrl, "ctrl");
			add(alt, "alt");
			add(shift, "shift");
			return keys;
		}

		// The modifier a key is for joinedKeys: 1 Ctrl (also Cmd, Meta and Win), 2 Alt, 3 Shift, 0 for other keys.
		size_t modifierRole(int key)
		{
			switch (key)
			{
			case Rml::Input::KI_LCONTROL:
			case Rml::Input::KI_RCONTROL:
			case Rml::Input::KI_LMETA:
			case Rml::Input::KI_RMETA:
			case Rml::Input::KI_LWIN:
			case Rml::Input::KI_RWIN:
				return 1;
			case Rml::Input::KI_LMENU:
			case Rml::Input::KI_RMENU:
				return 2;
			case Rml::Input::KI_LSHIFT:
			case Rml::Input::KI_RSHIFT:
				return 3;
			default:
				return 0;
			}
		}

		// The modifiers held.
		std::string modifiersOf(Rml::Event& event)
		{
			return joinedKeys(
				held(event, "ctrl_key") || held(event, "meta_key"), held(event, "alt_key"), held(event, "shift_key"));
		}

		// The modifiers without ctrl.
		std::string withoutCtrl(const std::string& keys)
		{
			if (keys == "ctrl")
				return {};
			return keys.rfind("ctrl+", 0) == 0 ? keys.substr(5) : keys;
		}

		// RmlUi's clock, in seconds.
		double now()
		{
			Rml::SystemInterface* system = Rml::GetSystemInterface();
			return system != nullptr ? system->GetElapsedTime() : 0.0;
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

	std::optional<int64_t> EventDisplay::selectionStart() const
	{
		if (m_range)
			return m_time.tempo.toTicks(m_range->start, m_time.sampleRate);
		std::optional<double> start;
		for (const Shown& shown : m_events)
			if (std::binary_search(m_selection.begin(), m_selection.end(), shown.id))
				start = std::min(start.value_or(shown.start), shown.start);
		if (!start)
			return std::nullopt;
		return m_time.tempo.toTicks(*start);
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
		if (!m_selection.empty())
			m_selectedPoints.clear(); // events or automation points (#121); setLanes shows it
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
					engine::gridStep(m_snap.grid(), snapGrid(), samples(edge(false).value_or(0.0)));
				Json parameters = {{"events", m_selection},
					{step.base == time::TimeBase::Musical ? "ticks" : "delta", direction * step.value}};
				if (edgeName != nullptr)
					parameters["edge"] = edgeName;
				edit(edgeName == nullptr ? "event.move" : "event.resize", std::move(parameters));
			};
		};
		// Nudge Cursor Left and Right: a grid step from the cursor (reference manual p. 300), never before the start.
		const auto nudgeCursor = [this](int direction)
		{
			return [this, direction]
			{
				const engine::SnapGrid grid = snapGrid();
				const time::Position step = engine::gridStep(m_snap.grid(), grid, m_cursorSample);
				int64_t to = m_cursorSample + (direction * step.value);
				if (step.base == time::TimeBase::Musical)
					to = grid.tempo.toSamples(
						std::max<time::Ticks>(
							0, grid.tempo.toTicks(m_cursorSample, grid.sampleRate) + (direction * step.value)),
						grid.sampleRate);
				// The next nudge goes on from here, even before transport.changed says so.
				setCursor(std::max<int64_t>(0, to));
				m_binding.send("transport.locate", {{"position", m_cursorSample}});
			};
		};
		// A range command for the selection range, if there is one (reference manual pp. 287-294), with a length
		// unless it splits.
		const auto onRange = [this](const char* command, const Json& parameters, bool needsLength = true)
		{
			return [this, command, parameters, needsLength]
			{
				if (!m_range || (needsLength && m_range->end <= m_range->start))
					return;
				Json withRange = parameters;
				withRange["start"] = m_range->start;
				withRange["end"] = m_range->end;
				withRange["tracks"] = m_range->tracks;
				edit(command, std::move(withRange));
			};
		};
		// An action that only edits the selection range: menus and keys disable it while there is none (canRun).
		const auto rangeAction =
			[this, onRange](const char* action, const char* command, const Json& parameters, bool needsLength = true)
		{
			m_rangeActions[action] = needsLength;
			return std::make_pair(
				std::string(action), std::function<void()>(onRange(command, parameters, needsLength)));
		};
		// Paste goes to the range's start and first track, or to the project cursor on the tracks it was copied from,
		// where it selects what it pasted.
		const auto paste = [this](bool time)
		{
			return [this, time]
			{
				Json parameters = {{"position", m_range ? m_range->start : m_cursorSample}, {"time", time}};
				if (m_range && !m_range->tracks.empty())
					parameters["track"] = m_range->tracks.front();
				edit("range.paste", std::move(parameters), !m_range);
			};
		};
		const std::map<std::string, std::function<void()>> actions = {
			{"edit.delete",
				[this, range = onRange("range.delete", Json::object()),
					events = onSelection("event.remove", Json::object())]
				{
					if (m_range)
						range();
					else
						events();
				}},
			rangeAction("edit.cut", "range.cut", Json::object()),
			rangeAction("edit.copy", "range.copy", Json::object()),
			{"edit.paste", paste(false)},
			rangeAction("edit.cutTime", "range.cut", {{"time", true}}),
			rangeAction("edit.deleteTime", "range.delete", {{"time", true}}),
			{"edit.pasteTime", paste(true)},
			rangeAction("edit.insertSilence", "range.insertSilence", Json::object()),
			rangeAction("edit.splitRange", "range.split", Json::object(), false),
			rangeAction("edit.cropRange", "range.crop", Json::object()),
			{"edit.globalCopy", [this] { edit("range.copy", Json::object()); }},
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
			// Audio parts (reference manual p. 250): the selected audio events into a part, the selected audio parts
			// back into events.
			{"audio.eventsToPart",
				[this]
				{
					if (const std::vector<uint64_t> events = selectedAudio(); !events.empty())
						edit("event.toPart", {{"events", events}}, true);
				}},
			{"audio.dissolvePart",
				[this]
				{
					std::vector<uint64_t> parts;
					for (const Shown& shown : m_events)
						if (m_laneTypes[shown.lane] == "audio" && m_fades.count(shown.id) == 0 &&
							std::binary_search(m_selection.begin(), m_selection.end(), shown.id))
							parts.push_back(shown.id);
					if (!parts.empty())
						edit("event.dissolvePart", {{"events", parts}}, true);
				}},

			{"locate.nudgeCursorLeft", nudgeCursor(-1)},
			{"locate.nudgeCursorRight", nudgeCursor(1)},
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
			{"edit.locatorsToSelection",
				[this]
				{
					bool any = false;
					double start = 0.0;
					double end = 0.0;
					for (const Shown& shown : m_events)
						if (std::binary_search(m_selection.begin(), m_selection.end(), shown.id))
						{
							start = any ? std::min(start, shown.start) : shown.start;
							end = any ? std::max(end, shown.end) : shown.end;
							any = true;
						}
					if (any)
						m_binding.send("transport.set", {{"left", samples(start)}, {"right", samples(end)}});
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
			{"edit.selectPrevious", [this] { selectNeighbour(-1); }},
			{"edit.selectNext", [this] { selectNeighbour(1); }},
		};
		m_actions.insert(actions.begin(), actions.end());
	}

	bool EventDisplay::canRun(const std::string& action) const
	{
		const auto found = m_rangeActions.find(action);
		return found == m_rangeActions.end() || (m_range && (!found->second || m_range->end > m_range->start));
	}

	void EventDisplay::selectNeighbour(int direction)
	{
		// From the last selected event in time going right, the first going left.
		const auto key = [](const Shown& shown) { return std::make_pair(shown.start, shown.id); };
		const auto before = [direction, &key](const Shown& a, const Shown& b)
		{ return direction < 0 ? key(a) < key(b) : key(a) > key(b); };
		const Shown* from = nullptr;
		for (const Shown& shown : m_events)
			if (std::binary_search(m_selection.begin(), m_selection.end(), shown.id) &&
				(from == nullptr || before(shown, *from)))
				from = &shown;
		if (from == nullptr)
			return;
		const Shown* to = nullptr;
		for (const Shown& shown : m_events)
			if (shown.lane == from->lane && before(shown, *from) && (to == nullptr || before(*to, shown)))
				to = &shown;
		if (to != nullptr)
			select({to->id});
	}

	void EventDisplay::selectFirstOn(uint64_t track)
	{
		const auto lane = std::find(m_laneTracks.begin(), m_laneTracks.end(), track);
		if (lane == m_laneTracks.end())
			return;
		const auto index = static_cast<size_t>(lane - m_laneTracks.begin());
		const Shown* first = nullptr;
		for (const Shown& shown : m_events)
			if (shown.lane == index &&
				(first == nullptr || std::make_pair(shown.start, shown.id) < std::make_pair(first->start, first->id)))
				first = &shown;
		if (first != nullptr)
			select({first->id});
	}

	size_t EventDisplay::laneAt(double y) const
	{
		if (y < 0.0)
			return 0;
		return static_cast<size_t>(std::upper_bound(m_laneTops.begin(), m_laneTops.end(), y) - m_laneTops.begin()) - 1;
	}

	const EventDisplay::Shown* EventDisplay::eventAt(double x, double y) const
	{
		if (y + m_view.top < 0.0)
			return nullptr;
		const size_t lane = laneAt(y + m_view.top);
		const double time = m_view.start + (x * m_view.scale);
		const Shown* found = nullptr; // the last one is drawn on top
		for (const Shown& shown : m_events)
			if (shown.lane == lane && time >= shown.start && time < shown.end)
				found = &shown;
		return found;
	}

	std::optional<size_t> EventDisplay::visibleLaneAt(double y) const
	{
		const size_t lane = laneAt(y + m_view.top);
		if (y + m_view.top < 0.0 || lane >= m_laneTracks.size())
			return std::nullopt;
		return lane;
	}

	int EventDisplay::edgeAt(const Shown& shown, double x) const
	{
		const double left = (shown.start - m_view.start) / m_view.scale;
		const double right = (shown.end - m_view.start) / m_view.scale;
		if (right - left <= 3.0 * g_edge) // too narrow to have edges
			return 0;
		if (x - left <= g_edge)
			return -1;
		return right - x <= g_edge ? 1 : 0;
	}

	void EventDisplay::areaRightDown(Rml::Event& event, double x, double y)
	{
		const bool modified =
			held(event, "ctrl_key") || held(event, "meta_key") || held(event, "shift_key") || held(event, "alt_key");
		if (modified != m_toolboxOnRightClick) // the toolbox, which chooses a tool and leaves the selection
			return;
		// Where a press selects a range, the context menu acts on the range: an event selected too would show beside
		// it.
		if (toolAt(y) == Tool::RangeSelection)
			return;
		const Shown* shown = eventAt(x, y);
		if (shown == nullptr)
			return;
		// As a press there does, it ends a range (with the tools combined), which the menu would act on instead.
		setRange(std::nullopt);
		if (!std::binary_search(m_selection.begin(), m_selection.end(), shown->id))
			select({shown->id});
	}

	void EventDisplay::areaDown(Rml::Event& event, double x, double y)
	{
		m_dragMoved = false;
		m_dragStarted = false;
		m_dragToggles = false;
		m_pressTime = now();
		m_dragDx = 0.0;
		m_dragLanes = 0;
		m_dragLaneY = y + m_view.top;
		m_pressX = x;
		m_pressY = y;
		m_pressed = modifiersOf(event);
		if (automationDown(x, y))
			return;
		if (toolDown(x, y))
			return;
		const bool add = held(event, "ctrl_key") || held(event, "meta_key") || held(event, "shift_key");
		const Shown* shown = eventAt(x, y);
		// The tool modifier Split Event (Alt) and a drag on an empty spot draws a part (reference manual p. 250).
		if (shown == nullptr && modifiers("select.splitEvent"))
		{
			if (const std::optional<size_t> lane = visibleLaneAt(y); lane && takesParts(*lane))
			{
				m_drag = Drag::DrawPart;
				return;
			}
		}
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
		const int edge = edgeAt(*shown, x);
		const bool atStart = edge < 0;
		const bool atEnd = edge > 0;
		// The tool modifiers for a press on an event (Set Cursor Position, Split Event, Copy, Shared Copy, and Disable
		// Snapping at an edge) act on it rather than adding it to the selection, whatever keys they are set to; so
		// does a copy's with Ctrl, a restricted copy.
		const bool toolModifier = modifiers("select.setCursorPosition") || modifiers("select.splitEvent") ||
			copyKeys(m_pressed) || copyKeys(withoutCtrl(m_pressed)) ||
			((atStart || atEnd) && modifiers("sizeObjects.disableSnapping"));
		// A fade handle (reference manual p. 383), or the event volume handle (p. 401).
		if (!add && !toolModifier && (fadeDown(*shown, x, y) || volumeDown(*shown, x, y)))
			return;
		if (add && !toolModifier)
		{
			// Ctrl or Shift and a click adds an event to the selection or, released without moving, takes it out; a
			// drag moves the selection with it.
			if (selected)
			{
				m_dragToggles = true;
			}
			else
			{
				std::vector<uint64_t> events = m_selection;
				events.push_back(shown->id);
				select(std::move(events));
			}
			m_drag = Drag::Events;
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

	std::string EventDisplay::pointerAt(double x, double y, const std::string& keys) const
	{
		const std::optional<size_t> lane = visibleLaneAt(y);
		if (lane && automationAt(*lane) != nullptr) // a press edits points with the tool chosen, combined or not
			return toolName(m_tool);
		const Tool tool = toolAt(y);
		if (tool != Tool::ObjectSelection)
			return toolName(tool);
		// What a press with the object selection tool and the keys does (areaDown, areaUp).
		const bool split = modifiers("select.splitEvent", keys);
		const Shown* shown = eventAt(x, y);
		if (shown == nullptr) // with the tool modifier Split Event, draws a part (reference manual p. 250)
			return split && lane && takesParts(*lane) ? toolName(Tool::Draw) : toolName(tool);
		const int edge = edgeAt(*shown, x);
		const bool add = keys.find("ctrl") != std::string::npos || keys.find("shift") != std::string::npos;
		const bool toolModifier = modifiers("select.setCursorPosition", keys) || split || copyKeys(keys) ||
			copyKeys(withoutCtrl(keys)) || (edge != 0 && modifiers("sizeObjects.disableSnapping", keys));
		if (add && !toolModifier) // adds the event to the selection or takes it out
			return toolName(tool);
		if (edge != 0) // resizes it, unless the press drags a fade
			return toolModifier || fadeHandleAt(*shown, x, y) == 0 ? "resize" : toolName(tool);
		// A click with the tool modifier Split Event splits the event there (reference manual p. 268).
		return split ? toolName(Tool::Split) : toolName(tool);
	}

	void EventDisplay::heldKeys(Rml::Event& event)
	{
		std::array<bool, 4> roles = {false, held(event, "ctrl_key") || held(event, "meta_key"), held(event, "alt_key"),
			held(event, "shift_key")};
		// A modifier key going down or up itself, whether or not the key event's modifiers say so yet. One going up
		// leaves its modifier held while another key of it is down: the other Shift key, or Cmd for Ctrl.
		const bool keyEvent = event.GetId() == Rml::EventId::Keydown || event.GetId() == Rml::EventId::Keyup;
		const int key = keyEvent ? event.GetParameter<int>("key_identifier", 0) : 0;
		if (const size_t role = modifierRole(key); role != 0)
		{
			if (event.GetId() == Rml::EventId::Keydown)
				m_modifierKeys.insert(key);
			else
				m_modifierKeys.erase(key);
			roles.at(role) = std::any_of(m_modifierKeys.begin(), m_modifierKeys.end(),
				[role](int other) { return modifierRole(other) == role; });
		}
		// A key that went up where the display didn't see it, e.g. in another window: the modifiers say it's not held.
		for (auto pressed = m_modifierKeys.begin(); pressed != m_modifierKeys.end();)
			pressed = roles.at(modifierRole(*pressed)) ? std::next(pressed) : m_modifierKeys.erase(pressed);
		std::string keys = joinedKeys(roles[1], roles[2], roles[3]);
		if (keys == m_held)
			return;
		m_held = std::move(keys);
		showPointer();
	}

	void EventDisplay::areaMove(double dx, double dy, bool constrained)
	{
		if (!m_dragMoved && std::abs(dx) < g_dragThreshold && std::abs(dy) < g_dragThreshold)
			return;
		m_dragMoved = true;
		if (m_drag == Drag::Automation)
		{
			automationMove(m_pressX + dx, m_pressY + dy);
			return;
		}
		if (m_drag == Drag::FadeIn || m_drag == Drag::FadeOut || m_drag == Drag::Volume)
		{
			m_dragDx = dx;
			m_dragDy = dy;
			showFades();
			return;
		}
		if (m_drag != Drag::Events && m_drag != Drag::ResizeStart && m_drag != Drag::ResizeEnd)
			toolMove(m_pressX + dx, m_pressY + dy);
		if (m_drag != Drag::Events)
			return;
		// The drag delay (reference manual p. 256): events move once the press is held that long.
		if (!m_dragStarted && now() - m_pressTime < g_dragDelay)
			return;
		m_dragStarted = true;
		const std::pair<double, double> shift = restricted(dx, dy, constrained);
		m_dragDx = constrained && shift.first == 0.0 ? 0.0 : moveShift(shift.first).second / m_view.scale;
		m_dragLanes = laneShift(shift.second);
		setLanes();
	}

	// Ctrl (Cmd) keeps a drag across or up and down, whichever it went further (reference manual pp. 256, 271).
	std::pair<double, double> EventDisplay::restricted(double dx, double dy, bool constrained)
	{
		if (!constrained)
			return {dx, dy};
		return std::abs(dx) >= std::abs(dy) ? std::make_pair(dx, 0.0) : std::make_pair(0.0, dy);
	}

	// Events only go onto tracks of their type (reference manual p. 256): audio events onto audio tracks, MIDI parts
	// onto MIDI and instrument tracks.
	int EventDisplay::laneShift(double dy) const
	{
		if (m_laneTracks.empty())
			return 0;
		const auto lane = [this](double y) { return static_cast<int>(std::min(laneAt(y), m_laneTracks.size() - 1)); };
		const int lanes = lane(m_dragLaneY + dy) - lane(m_dragLaneY);
		if (lanes == 0)
			return 0;
		for (const Shown& shown : m_events)
		{
			if (!std::binary_search(m_selection.begin(), m_selection.end(), shown.id))
				continue;
			const int64_t target = static_cast<int64_t>(shown.lane) + lanes;
			if (target < 0 || target >= static_cast<int64_t>(m_laneTracks.size()))
				return 0;
			const std::string& to = m_laneTypes[static_cast<size_t>(target)];
			if (m_laneTypes[shown.lane] == "audio" ? to != "audio" : (to != "midi" && to != "instrument"))
				return 0;
		}
		return lanes;
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
		if (drag == Drag::Automation)
		{
			automationUp(dpFrom(*m_lanes, event.GetParameter<float>("mouse_x", 0.0F)),
				(event.GetParameter<float>("mouse_y", 0.0F) - m_lanes->GetAbsoluteTop()) / dpRatio());
			m_dragMoved = false;
			m_drag = Drag::None;
			return;
		}
		if (drag == Drag::Rectangle || drag == Drag::Range || drag == Drag::DrawPart || drag == Drag::Comp ||
			drag == Drag::Play || drag == Drag::Click || drag == Drag::VolumePoint)
		{
			toolUp(dpFrom(*m_lanes, event.GetParameter<float>("mouse_x", 0.0F)),
				(event.GetParameter<float>("mouse_y", 0.0F) - m_lanes->GetAbsoluteTop()) / dpRatio());
			m_dragMoved = false;
			m_drag = Drag::None;
			return;
		}
		if (drag == Drag::FadeIn || drag == Drag::FadeOut || drag == Drag::Volume)
		{
			if (m_dragMoved)
			{
				if (drag == Drag::Volume)
					volumeUp();
				else
					fadeUp();
			}
			m_drag = Drag::None;
			return;
		}
		if (drag != Drag::Events && drag != Drag::ResizeStart && drag != Drag::ResizeEnd)
			return;
		const bool moved = std::exchange(m_dragMoved, false);
		const bool started = std::exchange(m_dragStarted, false) || now() - m_pressTime >= g_dragDelay;
		const bool toggles = std::exchange(m_dragToggles, false);
		m_drag = Drag::None;
		m_dragDx = 0.0;
		m_dragLanes = 0;
		setLanes(); // without the drag's preview
		// A click with Ctrl or Shift on a selected event, also one that moved a little before the drag delay.
		if (toggles && (!moved || (drag == Drag::Events && !started)))
		{
			std::vector<uint64_t> events = m_selection;
			events.erase(std::remove(events.begin(), events.end(), m_dragEvent), events.end());
			select(std::move(events));
			return;
		}
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
		const double dy = (event.GetParameter<float>("mouse_y", 0.0F) - m_dragY) / dpRatio();
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
		// Released before the drag delay: neither a move nor a click.
		if (!started)
			return;
		const bool constrained = restricts(event);
		if (constrained)
			m_pressed = withoutCtrl(m_pressed);
		const std::pair<double, double> shift = restricted(dx, dy, constrained);
		const int lanes = laneShift(shift.second);
		const bool shared = modifiers("dragDrop.sharedCopy");
		const bool copy = shared || modifiers("dragDrop.copy");
		if (constrained && shift.first == 0.0)
		{
			// Up or down only: onto other tracks at the same time, or nowhere.
			if (lanes != 0 || copy)
				moveSelection({{"delta", 0}}, lanes, copy, shared);
			return;
		}
		// Shuffle (reference manual p. 106): the selection trades places with the events it is dragged among.
		if (m_snap.on && m_snap.type == engine::SnapType::Shuffle && !copy && lanes == 0)
		{
			double start = 1e300;
			for (const Shown& shown : m_events)
				if (std::binary_search(m_selection.begin(), m_selection.end(), shown.id))
					start = std::min(start, shown.start);
			edit("event.shuffle",
				{{"events", m_selection}, {"position", samples(start + (shift.first * m_view.scale))}});
			return;
		}
		moveSelection(moveShift(shift.first).first, lanes, copy, shared);
	}

	bool EventDisplay::copyKeys(const std::string& keys) const
	{
		const auto copies = {"dragDrop.copy", "dragDrop.sharedCopy"};
		return std::any_of(copies.begin(), copies.end(),
			[this, &keys](const char* action)
			{
				const auto found = m_toolModifiers.find(action);
				return found != m_toolModifiers.end() && !found->second.empty() && found->second == keys;
			});
	}

	// Ctrl (Cmd) restricts a drag of events, unless the keys held are a copy's tool modifier.
	bool EventDisplay::restricts(Rml::Event& event) const
	{
		return (held(event, "ctrl_key") || held(event, "meta_key")) && !copyKeys(modifiersOf(event));
	}

	void EventDisplay::moveSelection(Json shift, int lanes, bool copy, bool shared)
	{
		// The tool modifiers Copy (Alt) and Shared Copy (Alt and Shift) copy (reference manual pp. 270-271).
		const char* command = copy ? "event.copy" : "event.move";
		if (copy)
			shift["shared"] = shared;
		// Onto other tracks, each event as many lanes down as the drag went: a command per track, in one undo step.
		std::map<size_t, std::vector<uint64_t>> byLane;
		if (lanes != 0)
			for (const Shown& shown : m_events)
				if (std::binary_search(m_selection.begin(), m_selection.end(), shown.id))
					byLane[shown.lane].push_back(shown.id);
		const auto track = [this, lanes](size_t lane)
		{ return m_laneTracks[static_cast<size_t>(static_cast<int64_t>(lane) + lanes)]; };
		if (byLane.size() <= 1)
		{
			shift["events"] = m_selection;
			if (!byLane.empty())
				shift["track"] = track(byLane.begin()->first);
			edit(command, std::move(shift), copy);
			return;
		}
		Json commands = Json::array();
		for (const auto& [lane, events] : byLane)
		{
			Json parameters = shift;
			parameters["events"] = events;
			parameters["track"] = track(lane);
			commands.push_back({{"command", command}, {"parameters", std::move(parameters)}});
		}
		m_binding.send("history.transaction",
			{{"description", copy ? "Copy" : "Move"}, {"commands", std::move(commands)}},
			[this, copy, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired())
					return;
				if (!result.ok())
				{
					logging::warning("An edit of events failed: {}", result.error);
					return;
				}
				if (!copy)
					return;
				std::vector<uint64_t> copies;
				for (const Json& done : result.value.at("results"))
					for (const Json& id : done.at("events"))
						copies.push_back(id.get<uint64_t>());
				select(std::move(copies));
			});
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
		if (const Shown* shown = eventAt(x, y); shown != nullptr)
		{
			// Outside a crossfade, an audio event opens in the sample editor (reference manual p. 613), an audio part
			// in the audio part editor (p. 692).
			if (!openCrossfadeAt(*shown, m_view.start + (x * m_view.scale)))
			{
				for (const Json& audio : m_project.value("audioEvents", Json::array()))
					if (audio.at("id") == shown->id)
						m_binding.send("sampleEditor.open", {{"event", shown->id}});
				for (const Json& part : m_project.value("audioParts", Json::array()))
					if (part.at("id") == shown->id)
						m_binding.send("editor.open", {{"editor", "window"}, {"parts", {shown->id}}});
			}
			return;
		}
		if (y + m_view.top < 0.0)
			return;
		const std::optional<size_t> found = visibleLaneAt(y);
		// Below the tracks, a new MIDI track (reference manual p. 198, the track list's empty area, #80).
		if (!found)
		{
			m_binding.send("track.add", {{"type", "midi"}});
			return;
		}
		const size_t lane = *found;
		if (!takesParts(lane))
			return;
		// A one-bar part where the click is (reference manual p. 251), or one between the locators; on audio tracks
		// only that (p. 250).
		const double seconds = m_view.start + (x * m_view.scale);
		const int64_t sample = samples(seconds);
		time::Ticks start = 0;
		time::Ticks end = 0;
		if (m_left < m_right && sample >= m_left && sample < m_right)
		{
			start = m_time.tempo.toTicks(m_left, m_time.sampleRate);
			end = m_time.tempo.toTicks(m_right, m_time.sampleRate);
		}
		else if (m_laneTypes[lane] == "audio")
		{
			return;
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

	bool EventDisplay::takesParts(size_t lane) const
	{
		const std::string& type = m_laneTypes[lane];
		return type == "audio" || type == "midi" || type == "instrument";
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
		// On a lane of a track whose lanes show, on that lane (#99).
		m_binding.send(m_laneTypes[lane] == "audio" ? "audioPart.add" : "midiPart.add",
			{{"track", m_laneTracks[lane]}, {"position", position}, {"length", length},
				{"lane", std::max(m_laneTakes[lane], 0)}},
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (!alive.expired() && result.ok())
					select({result.value.at("part").at("id").get<uint64_t>()});
			});
	}
}
