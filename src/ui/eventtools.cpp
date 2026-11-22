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
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

		// How long a press waits for the actions that wait for Snap to Zero Crossing (EventDisplay::runWaitingActions).
		constexpr std::chrono::seconds g_waitingActionsTimeout{2};
		// How far across a press in the selection range moves before it drags the range's contents, in dp.
		constexpr double g_rangeDragStart = 3.0;
		constexpr double g_rangeEdge = 6.0; // dp just inside the selection range's edges where a press drags them

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

	const std::map<std::string, std::string>& EventDisplay::defaultToolModifiers()
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

	void EventDisplay::setToolModifiers(const Json& value)
	{
		m_toolModifiers = defaultToolModifiers();
		if (value.is_object())
			for (const auto& [action, keys] : value.items())
				if (keys.is_string())
					m_toolModifiers[action] = keys.get<std::string>();
	}

	bool EventDisplay::modifiers(const char* action, const std::string& keys) const
	{
		const auto found = m_toolModifiers.find(action);
		return found != m_toolModifiers.end() && !found->second.empty() && found->second == keys;
	}

	void EventDisplay::addToolActions()
	{
		constexpr int count = static_cast<int>(Tool::Color) + 1;
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

	std::string EventDisplay::toolColor() const
	{
		if (!m_toolColor.empty() || !m_project.is_object() || m_project.value("colors", Json::array()).empty())
			return m_toolColor;
		return m_project.at("colors").front().at("color").get<std::string>();
	}

	void EventDisplay::setToolColor(std::string color)
	{
		m_toolColor = std::move(color);
		showToolColor();
	}

	void EventDisplay::stepToolColor(int steps)
	{
		const Json colors = m_project.is_object() ? m_project.value("colors", Json::array()) : Json::array();
		if (colors.empty())
			return;
		const auto count = static_cast<int>(colors.size());
		const auto found = std::find_if(colors.begin(), colors.end(),
			[current = toolColor()](const Json& color) { return color.at("color") == current; });
		const int index =
			found == colors.end() ? 0 : (((static_cast<int>(found - colors.begin()) + steps) % count) + count) % count;
		setToolColor(colors[static_cast<size_t>(index)].at("color").get<std::string>());
	}

	void EventDisplay::showToolColor()
	{
		if (alive())
			m_root->SetAttribute("tool-color", toolColor());
		if (m_toolColorListener)
			m_toolColorListener();
	}

	void EventDisplay::colorClick(uint64_t event)
	{
		const bool selected = std::binary_search(m_selection.begin(), m_selection.end(), event);
		if (m_pressed == "alt" && m_project.is_object())
		{
			// The event's own color, or else its track's; a track with the default color has none to take.
			const auto byId = [](const Json& list, const Json& id)
			{
				const auto found =
					std::find_if(list.begin(), list.end(), [&id](const Json& object) { return object.at("id") == id; });
				return found == list.end() ? Json() : *found;
			};
			Json color;
			for (const char* list : {"audioEvents", "midiParts", "audioParts"})
				if (const Json found = byId(m_project.value(list, Json::array()), event); found.is_object())
				{
					const Json track = byId(m_project.at("tracks"), found.at("track"));
					color = found.at("color").is_string() || !track.is_object() ? found.at("color") : track.at("color");
				}
			if (color.is_string())
				setToolColor(color.get<std::string>());
		}
		else if (m_pressed == "ctrl")
		{
			if (!selected)
				select({event});
			if (m_chooseColor)
				m_chooseColor(m_lanes->GetAbsoluteLeft() + static_cast<float>(m_pressX * dpRatio()),
					m_lanes->GetAbsoluteTop() + static_cast<float>(m_pressY * dpRatio()));
		}
		else if (const std::string color = toolColor(); !color.empty() && m_pressed.empty())
		{
			edit("event.set", {{"events", selected ? m_selection : std::vector<uint64_t>{event}}, {"color", color}});
		}
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

	void EventDisplay::setWarpMode(WarpMode mode)
	{
		m_warpMode = mode;
		showTools();
	}

	void EventDisplay::setSizingMovesContents(bool moves)
	{
		m_sizingMovesContents = moves;
		showTools();
	}

	Tool EventDisplay::toolAt(double y) const
	{
		// Combined, the object selection tool selects ranges in the upper half of a lane at least two rows high
		// (reference manual p. 253).
		const std::optional<size_t> lane = visibleLaneAt(y);
		if (m_combine && m_tool == Tool::ObjectSelection && lane &&
			m_laneTops[*lane + 1] - m_laneTops[*lane] >= 2 * g_trackRow && !inLowerHalf(*lane, y))
			return Tool::RangeSelection;
		return m_tool;
	}

	void EventDisplay::showPointer()
	{
		if (!alive() || !m_mouse || m_drag != Drag::None)
			return;
		const std::string pointer = pointerAt(m_mouse->first, m_mouse->second, m_held);
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
			{
				// A press in the range on one of its tracks drags its contents (reference manual p. 291). With Shift a
				// press adds to the range, but a drag in it selects a new range (reference manual p. 288): a Shift
				// double click there still adds the event's range.
				const int64_t at = samples(m_view.start + (x * m_view.scale));
				const bool inRange = lane && m_range && m_range->start <= at && at < m_range->end &&
					std::find(m_range->tracks.begin(), m_range->tracks.end(), m_laneTracks[*lane]) !=
						m_range->tracks.end();
				const bool shift = m_pressed.find("shift") != std::string::npos;
				if (const RangeEdge edge = shift ? RangeEdge::None : rangeEdgeAt(x, y); edge != RangeEdge::None)
				{
					m_dragRange = m_range;
					m_rangeEdge = edge;
					m_drag = Drag::RangeEdge;
					return true;
				}
				if (inRange && !shift)
				{
					m_dragRange = m_range;
					m_drag = Drag::RangeMove;
					return true;
				}
				m_rangeBase = m_pressed == "shift" ? m_range : std::nullopt;
				m_rangeAnew = m_pressed == "shift" && inRange;
				m_drag = Drag::Range;
				setRangeFrom(x, y);
				return true;
			}
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
			// Volume curve points on audio events (reference manual p. 399); off a point, the event volume handle
			// (p. 401) as with the object selection tool.
			if (shown != nullptr &&
				((!curvePointAt(*shown, x, y) && volumeDown(*shown, x, y)) || curveDown(*shown, x, y)))
				return true;
			// Empty MIDI parts on MIDI and instrument tracks, audio parts on audio tracks (reference manual p. 250).
			if (lane && shown == nullptr && takesParts(*lane))
				m_drag = Drag::DrawPart;
			return true;
		case Tool::Play:
			// Auditions or scrubs the pressed audio track, also on its automation tracks, whose track's own lane is the
			// track's first (reference manual pp. 252-253); auditioning MIDI waits.
			if (lane &&
				m_laneTypes[static_cast<size_t>(
					std::find(m_laneTracks.begin(), m_laneTracks.end(), m_laneTracks[*lane]) - m_laneTracks.begin())] ==
					"audio")
			{
				m_auditioned = m_laneTracks[*lane];
				if (m_playMode == PlayMode::Play)
				{
					m_drag = Drag::Play;
					m_binding.send("audition.start",
						{{"track", m_auditioned},
							{"position", std::max<int64_t>(0, samples(m_view.start + (x * m_view.scale)))}});
					return true;
				}
				// At an audio event's lower left or right corner it resizes the event as the object selection tool
				// does, scrubbing along (reference manual p. 268).
				if (const int edge = scrubEdgeAt(x, y); edge != 0 && shown != nullptr)
				{
					if (!std::binary_search(m_selection.begin(), m_selection.end(), shown->id))
						select({shown->id});
					m_drag = edge < 0 ? Drag::ResizeStart : Drag::ResizeEnd;
					m_resizeFrom = edge < 0 ? shown->start : shown->end;
					m_resizeScrubs = true;
					const AudioFades& fades = m_fades.at(shown->id);
					scrubEdge(fades.clip, fades.offset, fades.length);
					return true;
				}
				m_drag = Drag::Play;
				scrubAt(x);
			}
			return true;
		case Tool::Split:
		case Tool::Glue:
		case Tool::Erase:
		case Tool::Color:
			if (shown != nullptr)
				m_drag = Drag::Click;
			return true;
		case Tool::Line:
			return true; // it draws on automation tracks, which the event display shows with #121
		case Tool::TimeWarp:
			return timeWarpDown();
		}
		return false;
	}

	int EventDisplay::scrubEdgeAt(double x, double y) const
	{
		const std::optional<size_t> lane = visibleLaneAt(y);
		const Shown* shown = eventAt(x, y);
		if (toolAt(y) != Tool::Play || m_playMode != PlayMode::Scrub || !lane || m_laneTypes[*lane] != "audio" ||
			shown == nullptr || m_fades.count(shown->id) == 0 || !inLowerHalf(*lane, y))
			return 0;
		return edgeAt(*shown, x);
	}

	void EventDisplay::scrubAt(double x)
	{
		m_binding.send("audition.scrub",
			{{"track", m_auditioned}, {"position", std::max<int64_t>(0, samples(m_view.start + (x * m_view.scale)))}});
	}

	void EventDisplay::scrubEdge(uint64_t clip, int64_t offset, int64_t length)
	{
		m_binding.send(
			"audition.scrub", {{"clip", clip}, {"position", m_drag == Drag::ResizeStart ? offset : offset + length}});
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
			if (m_rangeAnew)
				m_rangeBase = std::nullopt;
			setRangeFrom(x, y);
			break;
		case Drag::RangeMove:
			if (m_dragRange)
			{
				RangeSelection to = movedRange(*m_dragRange, x, y);
				m_dragStarted =
					m_dragStarted || std::abs(x - m_pressX) >= g_rangeDragStart || to.tracks != m_dragRange->tracks;
				setRange(std::move(to));
			}
			break;
		case Drag::RangeEdge:
			if (m_dragRange)
				setRange(resizedRange(*m_dragRange, x, y));
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
				scrubAt(x);
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
					m_clickRedo = m_redo; // for a double click with Alt, which redoes
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
			const double top = std::min(m_pressY, y) + m_view.top;
			const double bottom = std::max(m_pressY, y) + m_view.top;
			if (m_pressTool == Tool::Zoom)
			{
				// The rectangle's stretch of time across; with Zoom Tool Standard Mode: Horizontal Zooming Only off,
				// its tracks down too (reference manual pp. 101, 1602), in one step of the zoom history.
				const size_t steps = m_undo.size();
				zoomTo(from, to);
				if (!m_zoomHorizontalOnly && bottom > top)
				{
					if (m_undo.size() == steps)
						remember();
					const double zoom = m_view.trackZoom;
					zoomVertically(height() / (bottom - top));
					scrollVertically(top * m_view.trackZoom / zoom);
				}
				break;
			}
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
			zeroCrossRange();
			break;
		case Drag::RangeMove:
		{
			// Released, its contents go where the range is, or with the tool modifier Copy (Alt) a copy of them; a
			// click selects a range of no length there. A drag that came back to the press leaves the range where it
			// was, and one that came back across only keeps its position.
			if (!moved || !m_dragRange || !m_dragStarted)
			{
				m_rangeBase = std::nullopt;
				setRangeFrom(x, y);
				zeroCrossRange();
				break;
			}
			const RangeSelection from = *m_dragRange;
			RangeSelection to = movedRange(from, x, y);
			if (std::abs(x - m_pressX) < g_rangeDragStart)
			{
				to.start = from.start;
				to.end = from.end;
			}
			setRange(to);
			if (to.start == from.start && to.tracks == from.tracks)
				break;
			Json parameters = {{"start", from.start}, {"end", from.end}, {"tracks", from.tracks},
				{"position", to.start}, {"copy", modifiers("dragDrop.copy")}};
			// Snapped to a musical grid line, it goes to the line's tick: a part on a bar lands on another bar's own
			// tick, which can lie up to half a sample from the sample the range shows (#1785). Not with Grid Relative,
			// whose tick keeps the range start's distance from its line, less than a sample, so it would land off the
			// line and, at the project start, off tick 0.
			if (to.start != from.start && m_snap.type != engine::SnapType::GridRelative)
			{
				const double start = static_cast<double>(from.start) / m_time.sampleRate;
				const time::Position snapped = snapAt(start + ((x - m_pressX) * m_view.scale), start);
				if (snapped.base == time::TimeBase::Musical && snapped.value >= 0 &&
					(to.start != 0 || snapped.value == 0) &&
					time::toSamples(snapped, m_time.tempo, m_time.sampleRate) == to.start)
				{
					parameters.erase("position");
					parameters["positionTicks"] = snapped.value;
				}
			}
			if (to.tracks != from.tracks)
			{
				// Where the range's top track goes: range.move names the tracks from the top.
				const std::vector<uint64_t> order = shownTracks();
				size_t top = 0;
				for (size_t i = 1; i < from.tracks.size(); ++i)
					if (std::find(order.begin(), order.end(), from.tracks[i]) <
						std::find(order.begin(), order.end(), from.tracks[top]))
						top = i;
				parameters["track"] = to.tracks[top];
			}
			// Refused, e.g. audio onto a MIDI track, the range goes back to its contents, unless it moved since.
			const RangeSelection shown = m_range.value_or(to);
			m_binding.send("range.move", std::move(parameters),
				[this, from, shown, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
				{
					if (alive.expired() || result.ok())
						return;
					showRefusal(result.error);
					if (m_range && m_range->start == shown.start && m_range->end == shown.end &&
						m_range->tracks == shown.tracks)
						setRange(from);
				});
			break;
		}
		case Drag::RangeEdge:
			// A click selects a range of no length there, as anywhere.
			if (!moved || !m_dragRange)
			{
				m_rangeBase = std::nullopt;
				setRangeFrom(x, y);
			}
			else
			{
				setRange(resizedRange(*m_dragRange, x, y));
			}
			zeroCrossRange();
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
		case Drag::TimeWarp:
			timeWarpUp(x);
			break;
		case Drag::Click:
		{
			if (moved || !onEvent)
				break;
			const Shown& event = *pressed;
			if (m_pressTool == Tool::Color)
			{
				colorClick(event.id);
				break;
			}
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
			// Undoes the zoom before the double click's first click zoomed in; with Alt, which zoomed out, redoes the
			// zoom undone before it (reference manual p. 104). The first click's zoom leaves no trace in the history.
			undoZoom();
			m_redo = m_clickRedo;
			if (m_pressed == "alt")
				redoZoom();
			else
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
		zeroCrossRange();
		return true;
	}

	EventDisplay::RangeEdge EventDisplay::rangeEdgeAt(double x, double y) const
	{
		if (!m_range || m_range->tracks.empty())
			return RangeEdge::None;
		const RangeSelection& range = *m_range;
		// Its box: from its top track's first lane to its bottom track's last.
		const auto first =
			std::find_first_of(m_laneTracks.begin(), m_laneTracks.end(), range.tracks.begin(), range.tracks.end());
		const auto last =
			std::find_first_of(m_laneTracks.rbegin(), m_laneTracks.rend(), range.tracks.begin(), range.tracks.end());
		if (first == m_laneTracks.end())
			return RangeEdge::None;
		const double top = m_laneTops[static_cast<size_t>(first - m_laneTracks.begin())] - m_view.top;
		const double bottom =
			m_laneTops[m_laneTracks.size() - static_cast<size_t>(last - m_laneTracks.rbegin())] - m_view.top;
		const auto across = [this](int64_t sample)
		{ return ((static_cast<double>(sample) / m_time.sampleRate) - m_view.start) / m_view.scale; };
		const double left = across(range.start);
		const double right = across(range.end);
		if (x < left || x > right || y < top || y > bottom)
			return RangeEdge::None;
		// As an event's, only where it's wide or high enough to have edges.
		if (right - left > 3.0 * g_rangeEdge)
		{
			if (x - left <= g_rangeEdge)
				return RangeEdge::Start;
			if (right - x <= g_rangeEdge)
				return RangeEdge::End;
		}
		if (bottom - top > 3.0 * g_rangeEdge)
		{
			if (y - top <= g_rangeEdge)
				return RangeEdge::Top;
			if (bottom - y <= g_rangeEdge)
				return RangeEdge::Bottom;
		}
		return RangeEdge::None;
	}

	std::vector<uint64_t> EventDisplay::shownTracks() const
	{
		std::vector<uint64_t> order;
		for (const uint64_t track : m_laneTracks)
			if (std::find(order.begin(), order.end(), track) == order.end())
				order.push_back(track);
		return order;
	}

	RangeSelection EventDisplay::movedRange(const RangeSelection& from, double x, double y) const
	{
		// Across with the mouse, its start snapped.
		const double start = static_cast<double>(from.start) / m_time.sampleRate;
		const int64_t to = std::max<int64_t>(0, samples(snapped(start + ((x - m_pressX) * m_view.scale), start)));
		RangeSelection range{to, to + (from.end - from.start), from.tracks};
		if (m_laneTracks.empty())
			return range;
		// Down or up by as many tracks as the mouse, as far as there are tracks for all of its own (p. 291).
		const std::vector<uint64_t> order = shownTracks();
		const auto index = [&order](uint64_t track)
		{ return static_cast<std::ptrdiff_t>(std::find(order.begin(), order.end(), track) - order.begin()); };
		const auto trackAt = [this](double dpY)
		{ return m_laneTracks[std::min(laneAt(dpY + m_view.top), m_laneTracks.size() - 1)]; };
		const auto size = static_cast<std::ptrdiff_t>(order.size());
		std::ptrdiff_t top = size;
		std::ptrdiff_t bottom = -1;
		for (const uint64_t track : from.tracks)
		{
			top = std::min(top, index(track));
			bottom = std::max(bottom, index(track));
		}
		if (bottom >= size) // a track the lanes don't show
			return range;
		const std::ptrdiff_t by = std::clamp(index(trackAt(y)) - index(trackAt(m_pressY)), -top, size - 1 - bottom);
		for (uint64_t& track : range.tracks)
			track = order[static_cast<size_t>(index(track) + by)];
		return range;
	}

	RangeSelection EventDisplay::resizedRange(const RangeSelection& from, double x, double y) const
	{
		RangeSelection range = from;
		if (m_rangeEdge == RangeEdge::Start || m_rangeEdge == RangeEdge::End)
		{
			// The edge follows the mouse across, snapped; past the other edge it becomes that one (p. 288).
			const bool start = m_rangeEdge == RangeEdge::Start;
			const double edge = static_cast<double>(start ? from.start : from.end) / m_time.sampleRate;
			const int64_t to = std::max<int64_t>(0, samples(snapped(edge + ((x - m_pressX) * m_view.scale), edge)));
			const int64_t other = start ? from.end : from.start;
			range.start = std::min(to, other);
			range.end = std::max(to, other);
			return range;
		}
		// The top or the bottom goes to the track under the mouse, not past the other edge: the tracks it passes
		// come in or go.
		const std::vector<uint64_t> order = shownTracks();
		const auto index = [&order](uint64_t track)
		{ return static_cast<std::ptrdiff_t>(std::find(order.begin(), order.end(), track) - order.begin()); };
		const auto size = static_cast<std::ptrdiff_t>(order.size());
		std::ptrdiff_t top = size;
		std::ptrdiff_t bottom = -1;
		for (const uint64_t track : from.tracks)
		{
			top = std::min(top, index(track));
			bottom = std::max(bottom, index(track));
		}
		if (m_laneTracks.empty() || bottom >= size)
			return range;
		const std::ptrdiff_t mouse = index(m_laneTracks[std::min(laneAt(y + m_view.top), m_laneTracks.size() - 1)]);
		range.tracks.clear();
		for (std::ptrdiff_t i = 0; i < size; ++i)
		{
			const uint64_t track = order[static_cast<size_t>(i)];
			const bool in = std::find(from.tracks.begin(), from.tracks.end(), track) != from.tracks.end();
			if (m_rangeEdge == RangeEdge::Top ? i >= std::min(mouse, bottom) && (in || i < top)
											  : i <= std::max(mouse, top) && (in || i > bottom))
				range.tracks.push_back(track);
		}
		return range;
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
		// With Group Editing, on every track of the folder (p. 274).
		if (range)
			range->tracks = withEditGroupTracks(range->tracks);
		m_range = std::move(range);
		if (m_range && !m_selectedPoints.empty()) // as selecting events does, so that Delete edits the range
			selectPoints({});
		showRange();
	}

	void EventDisplay::zeroCrossRange()
	{
		if (!m_snap.zeroCrossing || !m_range)
			return;
		const RangeSelection range = *m_range;
		m_binding.sendAwaited("snap.zeroCrossings", {{"positions", {range.start, range.end}}, {"tracks", range.tracks}},
			[this, range, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired())
					return;
				// Unless the range changed meanwhile. The binding then runs the actions that waited.
				if (result.ok() && m_range && m_range->start == range.start && m_range->end == range.end &&
					m_range->tracks == range.tracks)
				{
					const Json& positions = result.value.at("positions");
					RangeSelection moved = range;
					moved.start = positions.at(0).get<int64_t>();
					moved.end = std::max(moved.start, positions.at(1).get<int64_t>());
					setRange(std::move(moved));
				}
			});
	}

	void EventDisplay::runWaitingActions()
	{
		if (!m_binding.holding())
			return;
		// Handing on the answers runs the actions; the press doesn't wait for what they send. After the timeout they
		// run on the range as it is, not on the one that the press starts.
		if (!m_binding.awaitHeld(g_waitingActionsTimeout))
			m_binding.releaseHeld();
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
