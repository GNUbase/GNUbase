// SPDX-License-Identifier: GPL-3.0-or-later
// The project window's tools in the event display (#86, reference manual pp. 57-65): what a press in the event lanes
// does with each tool other than the object selection tool (eventediting.cpp), the selection range, and the tool
// modifiers, plus the mouse pointer per tool, the toolbox or context menu on a right click and the notification when a
// key command switches the tool (#469). Edits go to the engine as event.* and midiPart.add commands.
// docs/features/tools.md describes them.
#include "engine/time/tempomap.h"
#include "ui/eventdisplay.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Property.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		// The default tool modifiers (reference manual p. 1601) of the tools that GNUbase has, which the user setting
		// ui.toolModifiers overrides one by one.
		const std::map<std::string, std::string>& defaultModifiers()
		{
			static const std::map<std::string, std::string> modifiers = {
				{"dragDrop.copy", "alt"},
				{"dragDrop.sharedCopy", "alt+shift"},
				{"erase.deleteLaterEvents", "alt"},
				{"glue.glueAllFollowing", "alt"},
				{"range.selectFullVertical", "alt+shift"},
				{"select.setCursorPosition", "alt+shift"},
				{"select.splitEvent", "alt"},
				{"sizeObjects.disableSnapping", "ctrl"},
				{"split.splitRepeated", "alt"},
			};
			return modifiers;
		}

		Rml::Property dp(double value)
		{
			return {static_cast<float>(value), Rml::Unit::DP};
		}

		// Adds another range to the range from start to end on the lanes from first to last: from the earliest start
		// to the latest end, across the lanes between.
		RangeSelection joined(const std::vector<uint64_t>& laneTracks, const std::optional<RangeSelection>& other,
			int64_t start, int64_t end, size_t first, size_t last)
		{
			if (other)
			{
				const RangeSelection& add = *other;
				for (size_t i = 0; i < laneTracks.size(); ++i)
					if (std::find(add.tracks.begin(), add.tracks.end(), laneTracks[i]) != add.tracks.end())
					{
						first = std::min(first, i);
						last = std::max(last, i);
						start = std::min(start, add.start);
						end = std::max(end, add.end);
					}
			}
			// A track's automation tracks' lanes have its id too (#121): each track once.
			std::vector<uint64_t> tracks;
			for (size_t i = first; i <= last && i < laneTracks.size(); ++i)
				if (tracks.empty() || tracks.back() != laneTracks[i])
					tracks.push_back(laneTracks[i]);
			return {start, end, std::move(tracks)};
		}
	}

	void EventDisplay::setToolModifiers(const Json& value)
	{
		m_toolModifiers = defaultModifiers();
		if (value.is_object())
			for (const auto& [action, keys] : value.items())
				if (keys.is_string())
					m_toolModifiers[action] = keys.get<std::string>();
	}

	bool EventDisplay::modifiers(const char* action) const
	{
		const auto found = m_toolModifiers.find(action);
		return found != m_toolModifiers.end() && !found->second.empty() && found->second == m_pressed;
	}

	void EventDisplay::addToolActions()
	{
		constexpr int count = static_cast<int>(Tool::Play) + 1;
		for (int i = 0; i < count; ++i)
		{
			const auto tool = static_cast<Tool>(i);
			m_actions[std::string("tool.") + toolName(tool)] = [this, tool]
			{
				setTool(tool);
				notify(toolTitle(tool));
			};
		}
		m_actions["tool.next"] = [this]
		{
			setTool(static_cast<Tool>((static_cast<int>(m_tool) + 1) % count));
			notify(toolTitle(m_tool));
		};
		m_actions["tool.previous"] = [this]
		{
			setTool(static_cast<Tool>((static_cast<int>(m_tool) + count - 1) % count));
			notify(toolTitle(m_tool));
		};
		m_actions["tool.combineSelectionTools"] = [this]
		{
			setCombineSelectionTools(!m_combine);
			notify(m_combine ? "Combine Selection Tools On" : "Combine Selection Tools Off");
		};
	}

	void EventDisplay::setTool(Tool tool)
	{
		// Switching from a range to another tool keeps the events in it selected (reference manual p. 286).
		if (m_range && tool != Tool::RangeSelection)
		{
			selectRange();
			setRange(std::nullopt);
		}
		if (tool == Tool::RangeSelection && m_tool != tool)
			select({});
		m_tool = tool;
		showTools();
		showPointer();
	}

	void EventDisplay::setCombineSelectionTools(bool on)
	{
		m_combine = on;
		if (!on && m_range && m_tool != Tool::RangeSelection)
		{
			selectRange();
			setRange(std::nullopt);
		}
		showTools();
		showPointer();
	}

	void EventDisplay::setLineMode(LineMode mode)
	{
		m_lineMode = mode;
		showTools();
	}

	void EventDisplay::setPlayMode(PlayMode mode)
	{
		m_playMode = mode;
		showTools();
	}

	void EventDisplay::setSizingMovesContents(bool moves)
	{
		m_sizingMovesContents = moves;
		showTools();
	}

	Tool EventDisplay::toolAt(double y) const
	{
		// Combined, the object selection tool selects ranges in a lane's upper half (reference manual p. 253).
		const std::optional<size_t> lane = visibleLaneAt(y);
		if (m_combine && m_tool == Tool::ObjectSelection && lane &&
			y + m_view.top < (m_laneTops[*lane] + m_laneTops[*lane + 1]) * 0.5)
			return Tool::RangeSelection;
		return m_tool;
	}

	std::string EventDisplay::pointerAt(double x, double y) const
	{
		const Tool tool = toolAt(y);
		if (tool == Tool::ObjectSelection)
			if (const Shown* shown = eventAt(x, y);
				shown != nullptr && edgeAt(*shown, x) != 0 && fadeHandleAt(*shown, x, y) == 0) // a press drags a fade
				return "resize";
		return toolName(tool);
	}

	void EventDisplay::showPointer()
	{
		if (!alive() || !m_mouse || m_drag != Drag::None)
			return;
		const std::string pointer = pointerAt(m_mouse->first, m_mouse->second);
		if (m_area->GetAttribute<Rml::String>("pointer", "") == pointer)
			return;
		m_area->SetAttribute("pointer", pointer);
		// RmlUi takes the pointer for the element under the mouse from its style as the last update left it: style
		// it now, and update again, so that the pointer changes at once, also when the mouse doesn't move.
		m_document->GetOwnerDocument()->UpdateDocument();
		if (Rml::Context* context = m_root->GetContext())
			context->RequestNextUpdate(0);
	}

	void EventDisplay::showContextMenus()
	{
		if (!alive())
			return;
		const Rml::Element* plain = m_toolboxOnRightClick ? m_toolbox : m_contextMenu;
		const Rml::Element* modified = m_toolboxOnRightClick ? m_contextMenu : m_toolbox;
		if (plain != nullptr)
			m_area->SetAttribute("contextmenu", plain->GetId());
		else
			m_area->RemoveAttribute("contextmenu");
		if (modified != nullptr)
			m_area->SetAttribute("modifiercontextmenu", modified->GetId());
		else
			m_area->RemoveAttribute("modifiercontextmenu");
	}

	void EventDisplay::notify(const std::string& text)
	{
		if (m_notifies && m_notification != nullptr && alive())
			m_notification->SetAttribute("text", text);
	}

	bool EventDisplay::toolDown(double x, double y)
	{
		const std::optional<size_t> lane = visibleLaneAt(y);
		m_pressTool = toolAt(y);
		const Shown* shown = eventAt(x, y);
		m_dragEvent = shown == nullptr ? 0 : shown->id;
		switch (m_pressTool)
		{
		case Tool::ObjectSelection:
			setRange(std::nullopt); // with the tools combined, selecting events ends the range
			return false;
		case Tool::RangeSelection:
			if (!m_selection.empty())
				select({});
			m_rangeBase = m_pressed == "shift" ? m_range : std::nullopt; // Shift adds to the range
			m_drag = Drag::Range;
			setRangeFrom(x, y);
			return true;
		case Tool::Zoom:
		case Tool::Mute:
			m_drag = Drag::Rectangle;
			return true;
		case Tool::Comp:
			// Shift and a click selects a take (reference manual p. 228).
			if (m_pressed == "shift")
			{
				if (shown != nullptr)
					select({shown->id});
				return true;
			}
			m_drag = Drag::Comp;
			return true;
		case Tool::Draw:
			// Volume curve points on audio events (reference manual p. 399).
			if (shown != nullptr && curveDown(*shown, x, y))
				return true;
			// Empty MIDI parts on MIDI and instrument tracks, audio parts on audio tracks (reference manual p. 250).
			if (lane && shown == nullptr && takesParts(*lane))
				m_drag = Drag::DrawPart;
			return true;
		case Tool::Play:
			// Auditions or scrubs the pressed audio track (reference manual pp. 252-253); auditioning MIDI waits.
			if (lane && m_laneTypes[*lane] == "audio")
			{
				m_drag = Drag::Play;
				m_auditioned = m_laneTracks[*lane];
				m_binding.send(m_playMode == PlayMode::Scrub ? "audition.scrub" : "audition.start",
					{{"track", m_auditioned},
						{"position", std::max<int64_t>(0, samples(m_view.start + (x * m_view.scale)))}});
			}
			return true;
		case Tool::Split:
		case Tool::Glue:
		case Tool::Erase:
			if (shown != nullptr)
				m_drag = Drag::Click;
			return true;
		case Tool::Line:
			return true; // it draws on automation tracks, which the event display shows with #121
		}
		return false;
	}

	void EventDisplay::toolMove(double x, double y)
	{
		switch (m_drag)
		{
		case Drag::Rectangle:
			showRectangle(
				true, std::min(m_pressX, x), std::min(m_pressY, y), std::max(m_pressX, x), std::max(m_pressY, y));
			break;
		case Drag::Range:
			setRangeFrom(x, y);
			break;
		case Drag::DrawPart:
		case Drag::Comp:
		{
			// The part or the comp's range as it will be: snapped, on the pressed lane.
			const size_t lane = laneAt(m_pressY + m_view.top);
			if (lane >= m_laneTracks.size())
				return;
			const double height = m_laneTops[lane + 1] - m_laneTops[lane];
			const double top = m_laneTops[lane] - m_view.top;
			const auto at = [this](double dpX)
			{
				const time::Position snapped = snapAt(m_view.start + (dpX * m_view.scale), std::nullopt);
				const double seconds =
					static_cast<double>(time::toSamples(snapped, m_time.tempo, m_time.sampleRate)) / m_time.sampleRate;
				return (seconds - m_view.start) / m_view.scale;
			};
			showRectangle(true, std::min(at(m_pressX), at(x)), top, std::max(at(m_pressX), at(x)), top + height);
			break;
		}
		case Drag::VolumePoint:
			m_dragDx = x - m_pressX;
			m_dragDy = y - m_pressY;
			showFades();
			break;
		case Drag::Play:
			if (m_playMode == PlayMode::Scrub)
				m_binding.send("audition.scrub",
					{{"track", m_auditioned},
						{"position", std::max<int64_t>(0, samples(m_view.start + (x * m_view.scale)))}});
			break;
		default:
			break;
		}
	}

	void EventDisplay::toolUp(double x, double y)
	{
		showRectangle(false);
		const bool moved = m_dragMoved;
		const auto pressed = std::find_if(
			m_events.begin(), m_events.end(), [this](const Shown& shown) { return shown.id == m_dragEvent; });
		const bool onEvent = m_dragEvent != 0 && pressed != m_events.end();
		const double pressTime = m_view.start + (m_pressX * m_view.scale);
		const auto snappedSamples = [this](double seconds)
		{ return time::toSamples(snapAt(seconds, std::nullopt), m_time.tempo, m_time.sampleRate); };
		switch (m_drag)
		{
		case Drag::Rectangle:
		{
			if (!moved)
			{
				if (m_pressTool == Tool::ObjectSelection && modifiers("select.setCursorPosition"))
				{
					m_binding.send("transport.locate", {{"position", samples(pressTime)}});
				}
				else if (m_pressTool == Tool::Zoom) // Alt zooms out (reference manual p. 101)
				{
					remember();
					zoomAround(m_pressed == "alt" ? 2.0 : 0.5, pressTime, m_pressX);
				}
				else if (m_pressTool == Tool::Mute && onEvent)
				{
					edit("event.set", {{"events", {pressed->id}}, {"mute", !pressed->mute}});
				}
				break;
			}
			const double from = m_view.start + (std::min(m_pressX, x) * m_view.scale);
			const double to = m_view.start + (std::max(m_pressX, x) * m_view.scale);
			if (m_pressTool == Tool::Zoom)
			{
				// ponytail: zooms across only, the reference's standard mode; zoom track heights with #81.
				zoomTo(from, to);
				break;
			}
			const double top = std::min(m_pressY, y) + m_view.top;
			const double bottom = std::max(m_pressY, y) + m_view.top;
			std::vector<uint64_t> touched;
			for (const Shown& shown : m_events)
				if (m_laneTops[shown.lane + 1] > top && m_laneTops[shown.lane] < bottom && shown.end > from &&
					shown.start < to)
					touched.push_back(shown.id);
			if (m_pressTool == Tool::Mute)
			{
				if (!touched.empty())
					edit("event.set", {{"events", touched}, {"mute", true}});
			}
			else if (m_pressed.find("ctrl") != std::string::npos || m_pressed.find("shift") != std::string::npos)
			{
				touched.insert(touched.end(), m_selection.begin(), m_selection.end());
				select(std::move(touched));
			}
			else
			{
				select(std::move(touched));
			}
			break;
		}
		case Drag::Range:
			setRangeFrom(x, y);
			break;
		case Drag::DrawPart:
		{
			const std::optional<size_t> lane = visibleLaneAt(m_pressY);
			if (!lane)
				break;
			const auto ticks = [this](int64_t sample) { return m_time.tempo.toTicks(sample, m_time.sampleRate); };
			if (!moved)
			{
				// A click draws the bar there.
				const int64_t bar = m_time.signatures.toBarsBeats(m_time.tempo.toTicks(pressTime)).bar;
				addPart(*lane, m_time.signatures.barStart(bar), m_time.signatures.barStart(bar + 1));
				break;
			}
			const time::Position start = snapAt(pressTime, std::nullopt);
			const time::Position end = snapAt(m_view.start + (x * m_view.scale), std::nullopt);
			const auto inTicks = [this, &ticks](const time::Position& position)
			{
				return position.base == time::TimeBase::Musical
					? position.value
					: ticks(time::toSamples(position, m_time.tempo, m_time.sampleRate));
			};
			const time::Ticks a = inTicks(start);
			const time::Ticks b = inTicks(end);
			if (a != b)
				addPart(*lane, std::min(a, b), std::max(a, b));
			break;
		}
		case Drag::Play:
			m_binding.send("audition.stop", Json::object());
			break;
		case Drag::Comp:
		{
			// A click brings the take to the front; a drag on a lane of a track whose lanes show comps the range
			// (reference manual p. 228).
			const std::optional<size_t> lane = visibleLaneAt(m_pressY);
			if (!moved)
			{
				if (onEvent)
					edit("event.toFront", {{"events", {pressed->id}}});
				break;
			}
			if (!lane || m_laneTakes[*lane] < 0)
				break;
			const double releaseTime = m_view.start + (x * m_view.scale);
			const int64_t start = snappedSamples(std::min(pressTime, releaseTime));
			const int64_t end = snappedSamples(std::max(pressTime, releaseTime));
			if (end > start && start >= 0)
				edit("event.comp",
					{{"track", m_laneTracks[*lane]}, {"lane", m_laneTakes[*lane]}, {"start", start}, {"end", end}});
			break;
		}
		case Drag::VolumePoint:
			curveUp();
			break;
		case Drag::Click:
		{
			if (moved || !onEvent)
				break;
			const Shown& event = *pressed;
			// The events after it on its track, in order.
			std::vector<const Shown*> later;
			for (const Shown& shown : m_events)
				if (shown.lane == event.lane && shown.id != event.id && shown.start >= event.start)
					later.push_back(&shown);
			std::sort(later.begin(), later.end(), [](const Shown* a, const Shown* b) { return a->start < b->start; });
			if (m_pressTool == Tool::Split)
			{
				// Snapped; the tool modifier Split Repeated cuts it into pieces that long (reference manual p. 269).
				const int64_t at = snappedSamples(pressTime);
				const int64_t start = samples(event.start);
				const int64_t end = samples(event.end);
				if (at <= start || at >= end)
					break;
				Json positions = Json::array({at});
				if (modifiers("split.splitRepeated"))
					for (int64_t next = at + (at - start); next < end; next += at - start)
						positions.push_back(next);
				edit("event.split",
					{{"events", {event.id}}, {"positions", std::move(positions)},
						{"zeroCrossing", m_snap.zeroCrossing}});
			}
			else if (m_pressTool == Tool::Glue)
			{
				// To the next event, or with Glue All Following Events to all that follow (reference manual p. 269).
				if (later.empty())
					break;
				std::vector<uint64_t> events = {event.id};
				const size_t count = modifiers("glue.glueAllFollowing") ? later.size() : 1;
				for (size_t i = 0; i < count; ++i)
					events.push_back(later[i]->id);
				edit("event.glue", {{"events", events}});
			}
			else if (!eraseCurvePoint(event, m_pressX, m_pressY))
			{
				// Erase, with Delete Later Events everything after it on its track (reference manual p. 255).
				std::vector<uint64_t> events = {event.id};
				if (modifiers("erase.deleteLaterEvents"))
					for (const Shown* shown : later)
						events.push_back(shown->id);
				edit("event.remove", {{"events", events}});
			}
			break;
		}
		default:
			break;
		}
	}

	bool EventDisplay::toolDoubleClick(double x, double y)
	{
		switch (m_pressTool)
		{
		case Tool::Zoom:
			// Undoes the zoom before the double click's first click zoomed in (reference manual p. 103).
			undoZoom();
			undoZoom();
			return true;
		case Tool::RangeSelection:
			break;
		default:
			return false; // the second click is a click
		}
		// The range of the event, or of the gap between two events, on the lane (reference manual p. 285).
		const std::optional<size_t> lane = visibleLaneAt(y);
		if (!lane)
			return true;
		double start = 0.0;
		double end = 0.0;
		if (const Shown* shown = eventAt(x, y))
		{
			start = shown->start;
			end = shown->end;
		}
		else
		{
			const double at = m_view.start + (x * m_view.scale);
			std::optional<double> next;
			for (const Shown& other : m_events)
			{
				if (other.lane != *lane)
					continue;
				if (other.end <= at)
					start = std::max(start, other.end);
				if (other.start > at)
					next = std::min(next.value_or(other.start), other.start);
			}
			if (!next)
				return true;
			end = *next;
		}
		// With Shift added to the range.
		setRange(joined(
			m_laneTracks, m_pressed == "shift" ? m_range : std::nullopt, samples(start), samples(end), *lane, *lane));
		return true;
	}

	// From where the drag started to x and y, snapped (reference manual p. 287); with Select Full Vertical on every
	// track, with Shift added to the range the drag started with.
	void EventDisplay::setRangeFrom(double x, double y)
	{
		if (m_laneTracks.empty())
			return;
		const auto laneOf = [this](double dpY) { return std::min(laneAt(dpY + m_view.top), m_laneTracks.size() - 1); };
		size_t first = std::min(laneOf(m_pressY), laneOf(y));
		size_t last = std::max(laneOf(m_pressY), laneOf(y));
		if (modifiers("range.selectFullVertical"))
		{
			first = 0;
			last = m_laneTracks.size() - 1;
		}
		const auto at = [this](double dpX)
		{
			return time::toSamples(
				snapAt(m_view.start + (dpX * m_view.scale), std::nullopt), m_time.tempo, m_time.sampleRate);
		};
		setRange(joined(
			m_laneTracks, m_rangeBase, std::min(at(m_pressX), at(x)), std::max(at(m_pressX), at(x)), first, last));
	}

	void EventDisplay::setRange(std::optional<RangeSelection> range)
	{
		m_range = std::move(range);
		showRange();
	}

	void EventDisplay::selectRange()
	{
		if (!m_range)
			return;
		const RangeSelection& range = *m_range;
		const double from = static_cast<double>(range.start) / m_time.sampleRate;
		const double to = static_cast<double>(range.end) / m_time.sampleRate;
		const std::vector<uint64_t>& tracks = range.tracks;
		selectWhere(
			[this, from, to, &tracks](const Shown& shown)
			{
				if (std::find(tracks.begin(), tracks.end(), m_laneTracks[shown.lane]) == tracks.end())
					return false;
				return from == to ? shown.start <= from && from < shown.end : shown.start < to && shown.end > from;
			});
	}

	void EventDisplay::showRange()
	{
		if (!alive())
			return;
		std::string ends;
		std::string tracks;
		bool visible = false;
		if (m_range)
		{
			const RangeSelection& range = *m_range;
			ends = std::to_string(range.start) + " " + std::to_string(range.end);
			for (const uint64_t track : range.tracks)
				tracks += (tracks.empty() ? "" : " ") + std::to_string(track);
			const auto first =
				std::find(m_laneTracks.begin(), m_laneTracks.end(), range.tracks.empty() ? 0 : range.tracks.front());
			const auto last =
				std::find(m_laneTracks.begin(), m_laneTracks.end(), range.tracks.empty() ? 0 : range.tracks.back());
			if (first != m_laneTracks.end() && last != m_laneTracks.end())
			{
				const double start = static_cast<double>(range.start) / m_time.sampleRate;
				const double end = static_cast<double>(range.end) / m_time.sampleRate;
				m_rangeElement->SetProperty(Rml::PropertyId::Left, dp((start - m_view.start) / m_view.scale));
				m_rangeElement->SetProperty(Rml::PropertyId::Width, dp((end - start) / m_view.scale));
				const double height = m_laneTops[static_cast<size_t>(last - m_laneTracks.begin()) + 1] -
					m_laneTops[static_cast<size_t>(first - m_laneTracks.begin())];
				const double top = m_laneTops[static_cast<size_t>(first - m_laneTracks.begin())] - m_view.top;
				m_rangeElement->SetProperty(Rml::PropertyId::Top, dp(top));
				m_rangeElement->SetProperty(Rml::PropertyId::Height, dp(height));
				visible = true;
			}
		}
		m_rangeElement->SetClass("shown", visible);
		m_root->SetAttribute("range", ends);
		m_root->SetAttribute("range-tracks", tracks);
	}

	void EventDisplay::showRectangle(bool shown, double left, double top, double right, double bottom)
	{
		if (!alive())
			return;
		m_rectangle->SetClass("shown", shown);
		if (!shown)
			return;
		m_rectangle->SetProperty(Rml::PropertyId::Left, dp(left));
		m_rectangle->SetProperty(Rml::PropertyId::Top, dp(top));
		m_rectangle->SetProperty(Rml::PropertyId::Width, dp(right - left));
		m_rectangle->SetProperty(Rml::PropertyId::Height, dp(bottom - top));
	}
}
