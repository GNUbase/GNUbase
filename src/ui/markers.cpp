// SPDX-License-Identifier: GPL-3.0-or-later
// The project's markers as the UI reads them, and the marker lines and marker events in the event display
// (ui/markers.h). The markers window is in markerswindow.cpp.
#include "ui/markers.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		// As tall as any event display gets: the marker lines' one lane.
		constexpr float g_linesHeight = 100000.0F;
		// A double click, as RmlUi has it: the second click within half a second and 3 dp.
		constexpr std::chrono::milliseconds g_doubleClickTime{500};
		constexpr float g_doubleClickDistance = 3.0F;
		// How close to a position marker's line a click selects it, in dp.
		constexpr double g_markerReach = 4.0;
		// How far down a marker track's lane a press on a cycle marker moves it rather than selecting it, in dp: its
		// upper border (reference manual p. 425).
		constexpr double g_cycleBorder = 8.0;
		// How far a press moves before it drags, in dp.
		constexpr double g_dragStart = 2.0;

		Rml::Property dp(double value)
		{
			return {static_cast<float>(value), Rml::Unit::DP};
		}

		template <typename T>
		T& create(Rml::Element& parent, Rml::Element* before, const char* tag, const char* classes)
		{
			Rml::ElementPtr element = parent.GetOwnerDocument()->CreateElement(tag);
			element->SetClassNames(classes);
			auto* created = dynamic_cast<T*>(parent.InsertBefore(std::move(element), before));
			if (created == nullptr)
				throw std::logic_error(std::string("<") + tag + "> isn't registered (ui::registerCanvas)");
			return *created;
		}

		MarkerLineMode lineMode(const Json& value)
		{
			if (value == "all")
				return MarkerLineMode::All;
			if (value == "off")
				return MarkerLineMode::Off;
			return MarkerLineMode::Active;
		}
	}

	std::vector<MarkerItem> ProjectMarkers::of(uint64_t track) const
	{
		std::vector<MarkerItem> found;
		std::copy_if(markers.begin(), markers.end(), std::back_inserter(found),
			[track](const MarkerItem& marker) { return marker.track == track; });
		return found;
	}

	int64_t ProjectMarkers::position(const Track& track, double seconds) const
	{
		seconds = clampPositionSeconds(seconds, time.sampleRate);
		return track.linear ? positionSamples(seconds, time.sampleRate) : time.tempo.toTicks(seconds);
	}

	ProjectMarkers readMarkers(const Json& project)
	{
		ProjectMarkers result;
		result.time = projectTime(project);
		result.active = project.value("activeMarkerTrack", uint64_t{0});
		std::map<uint64_t, bool> linear;
		for (const Json& track : project.at("tracks"))
		{
			ProjectMarkers::Track item;
			item.id = track.at("id").get<uint64_t>();
			item.marker = track.at("type") == "marker";
			item.linear = track.at("timeBase") == "linear";
			item.name = track.value("name", "");
			linear[item.id] = item.linear;
			result.tracks.push_back(std::move(item));
		}
		const auto seconds = [&result](int64_t position, bool isLinear)
		{
			return isLinear ? static_cast<double>(position) / result.time.sampleRate
							: result.time.tempo.toSeconds(position);
		};
		for (const Json& marker : project.at("markers"))
		{
			MarkerItem item;
			item.id = marker.at("id").get<uint64_t>();
			item.track = marker.at("track").get<uint64_t>();
			item.cycle = marker.at("type") == "cycle";
			item.number = marker.at("number").get<int>();
			item.name = marker.at("name").get<std::string>();
			const bool isLinear = linear[item.track];
			item.start = seconds(marker.at("position").get<int64_t>(), isLinear);
			item.end = item.cycle ? seconds(marker.at("end").get<int64_t>(), isLinear) : item.start;
			result.markers.push_back(std::move(item));
		}
		// In timeline order, as the engine lists them.
		std::sort(result.markers.begin(), result.markers.end(), [](const MarkerItem& a, const MarkerItem& b)
			{ return std::tie(a.start, a.cycle, a.number, a.id) < std::tie(b.start, b.cycle, b.number, b.id); });
		return result;
	}

	MarkerLines::MarkerLines(EventDisplay& display, EngineBinding& binding)
		: m_display(display), m_binding(binding), m_area(display.eventArea().GetObserverPtr())
	{
		Rml::Element& area = display.eventArea();
		const Rml::Element* events = area.QuerySelector("event-lanes.events");
		// Lines behind the events and folder tracks' data, marker events and their labels over the events.
		m_lines = &create<EventLanes>(area, area.GetFirstChild(), "event-lanes", "marker-lines");
		Rml::Element* after = events != nullptr ? events->GetNextSibling() : nullptr;
		m_events = &create<EventLanes>(area, after, "event-lanes", "marker-events");
		m_labels = &create<Rml::Element>(area, after, "div", "marker-labels");
		// The field that renames a marker, over its label.
		Rml::ElementPtr field = area.GetOwnerDocument()->CreateElement("input");
		field->SetAttribute("type", "text");
		field->SetClassNames("marker-name-field");
		m_field = dynamic_cast<Rml::ElementFormControl*>(area.InsertBefore(std::move(field), after));
		if (m_field == nullptr)
			throw std::logic_error("<input> isn't a form control");
		m_field->AddEventListener(Rml::EventId::Change, this);
		m_field->AddEventListener(Rml::EventId::Blur, this);
		area.AddEventListener(Rml::EventId::Mousedown, this);
		display.onViewChanged(
			[this, alive = std::weak_ptr<bool>(m_alive)]
			{
				if (!alive.expired())
					place();
			});

		for (const char* event : {"history.changed", "project.reset"})
			m_subscriptions.push_back(m_binding.on(event, [this](const engine::Event&) { requestProject(); }));
		m_subscriptions.push_back(m_binding.on("settings.changed",
			[this](const engine::Event& event)
			{
				const Json& values = event.data.at("values");
				if (const auto mode = values.find("ui.markerLines"); mode != values.end())
				{
					m_mode = lineMode(*mode);
					m_modeKnown = true;
					show();
				}
				if (const auto ids = values.find("ui.markerIds"); ids != values.end() && ids->is_boolean())
				{
					m_showIds = ids->get<bool>();
					m_idsKnown = true;
					show();
				}
			}));
		m_binding.send("settings.get", {{"key", "ui.markerLines"}},
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok() || m_modeKnown) // newer changes win
					return;
				m_mode = lineMode(result.value.at("value"));
				show();
			});
		m_binding.send("settings.get", {{"key", "ui.markerIds"}},
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok() || m_idsKnown || !result.value.at("value").is_boolean())
					return;
				m_showIds = result.value.at("value").get<bool>();
				show();
			});
		requestProject();
	}

	MarkerLines::~MarkerLines()
	{
		if (!alive())
			return;
		m_area->RemoveEventListener(Rml::EventId::Mousedown, this);
		m_field->RemoveEventListener(Rml::EventId::Change, this);
		m_field->RemoveEventListener(Rml::EventId::Blur, this);
		if (Rml::ElementDocument* document = m_area->GetOwnerDocument(); document != nullptr && m_drag != Drag::None)
		{
			document->RemoveEventListener(Rml::EventId::Mousemove, this);
			document->RemoveEventListener(Rml::EventId::Mouseup, this);
		}
	}

	// ponytail: reads the whole project after every change, as the event display does.
	void MarkerLines::requestProject()
	{
		if (m_loading)
		{
			m_stale = true;
			return;
		}
		m_loading = true;
		m_binding.send("project.get", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired())
					return;
				m_loading = false;
				if (result.ok())
				{
					m_markers = readMarkers(result.value);
					if (m_drag == Drag::None)
						m_preview.reset(); // a drag released shows where it went
					show();
				}
				if (std::exchange(m_stale, false))
					requestProject();
			});
	}

	void MarkerLines::show()
	{
		if (!alive())
			return;
		const ProjectMarkers& markers = shown();
		std::vector<TimeSpan> lines;
		for (const MarkerItem& marker : markers.markers)
		{
			if (m_mode == MarkerLineMode::Off || (m_mode == MarkerLineMode::Active && marker.track != markers.active))
				continue;
			lines.push_back({marker.start, 0.0});
			if (marker.cycle)
				lines.push_back({marker.end, 0.0});
		}
		std::sort(lines.begin(), lines.end(), [](const TimeSpan& a, const TimeSpan& b) { return a.start < b.start; });
		m_lines->setLanes({{g_linesHeight, std::move(lines)}});

		// Markers that went are no longer selected.
		m_selection.erase(std::remove_if(m_selection.begin(), m_selection.end(),
							  [this](uint64_t id)
							  {
								  return std::none_of(m_markers.markers.begin(), m_markers.markers.end(),
									  [id](const MarkerItem& marker) { return marker.id == id; });
							  }),
			m_selection.end());

		// A label per marker on a marker track: with ui.markerIds its number, the cycle markers' in brackets as in
		// the reference, and its name.
		std::string labels;
		for (const MarkerItem& marker : markers.markers)
		{
			std::string text;
			if (m_showIds && marker.id != 0) // a new cycle marker being dragged has no number yet
				text = marker.cycle ? "[" + std::to_string(marker.number) + "]" : std::to_string(marker.number);
			if (!marker.name.empty())
				text += (text.empty() ? "" : " ") + marker.name;
			labels += R"(<div class="marker-label)" + std::string(marker.cycle ? " cycle" : "") +
				(marker.track == markers.active ? " active" : "") + R"(" marker=")" + std::to_string(marker.id) +
				R"(">)" + Rml::StringUtilities::EncodeRml(text) + "</div>";
		}
		m_labels->SetInnerRML(labels);
		showSelection();
		m_laneTops.clear(); // rebuild the lanes with the new marker events
		place();
		if (m_renaming != 0 && !m_fieldOpen)
			openField();
	}

	void MarkerLines::setSelection(std::vector<uint64_t> markers)
	{
		m_selection = std::move(markers);
		showSelection();
	}

	void MarkerLines::pick(std::vector<uint64_t> markers)
	{
		// The project window selects events or markers.
		if (!markers.empty() && !m_display.selection().empty())
			m_display.select({});
		if (markers == m_selection)
			return;
		setSelection(std::move(markers));
		if (m_onSelect)
			m_onSelect(m_selection);
	}

	void MarkerLines::rename(uint64_t marker)
	{
		if (m_fieldOpen)
			closeField(false);
		m_renaming = marker;
		openField();
	}

	bool MarkerLines::removeSelected()
	{
		if (m_selection.empty())
			return false;
		m_binding.send("marker.remove", {{"markers", m_selection}});
		return true;
	}

	void MarkerLines::showSelection()
	{
		if (!alive())
			return;
		for (int i = 0; i < m_labels->GetNumChildren(); ++i)
		{
			Rml::Element* label = m_labels->GetChild(i);
			const auto id = label->GetAttribute<uint64_t>("marker", 0);
			label->SetClass("selected", std::find(m_selection.begin(), m_selection.end(), id) != m_selection.end());
		}
	}

	// What the mouse is on: the marker whose label it is on, a position marker's line within reach, a cycle marker's
	// start or end handle within reach, its upper border, or else the cycle marker it is in.
	MarkerLines::Hit MarkerLines::hitAt(Rml::Event& event, const LanePoint& at) const
	{
		const Rml::Vector2f mouse = {
			event.GetParameter<float>("mouse_x", 0.0F), event.GetParameter<float>("mouse_y", 0.0F)};
		for (int i = 0; i < m_labels->GetNumChildren(); ++i) // the labels let the mouse through
			if (Rml::Element* label = m_labels->GetChild(i); label->IsVisible() && label->IsPointWithinElement(mouse))
				return {label->GetAttribute<uint64_t>("marker", 0), Part::Marker};
		const double reach = g_markerReach * m_display.view().scale;
		const uint64_t track = at.track->id;
		const auto on = [track](const MarkerItem& marker, bool cycle)
		{ return marker.track == track && marker.cycle == cycle; };
		for (const MarkerItem& marker : m_markers.markers)
			if (on(marker, false) && std::abs(marker.start - at.seconds) <= reach)
				return {marker.id, Part::Marker};
		for (const MarkerItem& marker : m_markers.markers)
		{
			if (!on(marker, true))
				continue;
			if (std::abs(marker.start - at.seconds) <= reach)
				return {marker.id, Part::Start};
			if (std::abs(marker.end - at.seconds) <= reach)
				return {marker.id, Part::End};
		}
		// The last cycle marker that starts before the mouse, as a double click finds it.
		Hit inside;
		for (const MarkerItem& marker : m_markers.markers)
			if (on(marker, true) && marker.start <= at.seconds && at.seconds <= marker.end)
				inside = {marker.id, at.y < g_cycleBorder ? Part::Border : Part::Inside};
		return inside;
	}

	// A click selects the marker hit, or none; with Ctrl (Cmd on macOS) it adds or removes it. A selected marker
	// stays selected with the others, so that a drag moves them all.
	void MarkerLines::select(Rml::Event& event, uint64_t hit)
	{
		std::vector<uint64_t> selection = m_selection;
		const auto at = std::find(selection.begin(), selection.end(), hit);
		if (event.GetParameter<int>("ctrl_key", 0) != 0 || event.GetParameter<int>("meta_key", 0) != 0)
		{
			if (at != selection.end())
				selection.erase(at);
			else if (hit != 0)
				selection.push_back(hit);
		}
		else if (hit == 0 || at == selection.end())
		{
			selection.clear();
			if (hit != 0)
				selection.push_back(hit);
		}
		// The project window selects events or markers: a marker clicked deselects the events.
		if (!selection.empty() && !m_display.selection().empty())
			m_display.select({});
		if (selection == m_selection)
			return;
		setSelection(std::move(selection));
		if (m_onSelect)
			m_onSelect(m_selection);
	}

	void MarkerLines::deselect()
	{
		if (m_selection.empty())
			return;
		setSelection({});
		if (m_onSelect)
			m_onSelect(m_selection);
	}

	void MarkerLines::place()
	{
		if (!alive())
			return;
		const ProjectMarkers& markers = shown();
		const TimelineView& view = m_display.view();
		const auto& tracks = m_display.laneTracks();
		const auto& tops = m_display.laneTops();
		if (tracks != m_laneTracks || tops != m_laneTops)
		{
			m_laneTracks = tracks;
			m_laneTops = tops;
			std::vector<EventLanes::Lane> lanes;
			for (size_t i = 0; i < tracks.size(); ++i)
			{
				EventLanes::Lane lane{static_cast<float>(tops[i + 1] - tops[i]), {}};
				for (const MarkerItem& marker : markers.of(tracks[i]))
					lane.events.push_back({marker.start, marker.end - marker.start});
				std::sort(lane.events.begin(), lane.events.end(),
					[](const TimeSpan& a, const TimeSpan& b) { return a.start < b.start; });
				lanes.push_back(std::move(lane));
			}
			m_events->setLanes(std::move(lanes));
		}
		for (Rml::Element* canvas : {static_cast<Rml::Element*>(m_lines), static_cast<Rml::Element*>(m_events)})
		{
			canvas->SetAttribute("start", view.start);
			canvas->SetAttribute("scale", view.scale);
		}
		m_events->SetAttribute("scroll-top", view.top);

		std::map<uint64_t, size_t> lanes;
		for (size_t i = 0; i < tracks.size(); ++i)
			lanes.emplace(tracks[i], i);
		// The labels are the markers', in the same order.
		for (int i = 0; i < m_labels->GetNumChildren() && i < static_cast<int>(markers.markers.size()); ++i)
		{
			Rml::Element* label = m_labels->GetChild(i);
			const MarkerItem& marker = markers.markers[static_cast<size_t>(i)];
			const auto lane = lanes.find(marker.track);
			label->SetClass("hidden", lane == lanes.end());
			if (lane == lanes.end())
				continue;
			const double top = tops[lane->second] - view.top;
			label->SetProperty(Rml::PropertyId::Left, dp((marker.start - view.start) / view.scale));
			label->SetProperty(Rml::PropertyId::Top, dp(top));
		}
		// The name field over the label of the marker it renames.
		if (m_fieldOpen)
			for (const MarkerItem& marker : m_markers.markers)
				if (const auto lane = lanes.find(marker.track); marker.id == m_renaming && lane != lanes.end())
				{
					m_field->SetProperty(Rml::PropertyId::Left, dp((marker.start - view.start) / view.scale));
					m_field->SetProperty(Rml::PropertyId::Top, dp(tops[lane->second] - view.top));
				}
	}

	// Opens the name field once the marker being renamed is there.
	void MarkerLines::openField()
	{
		const auto marker = std::find_if(m_markers.markers.begin(), m_markers.markers.end(),
			[this](const MarkerItem& item) { return item.id == m_renaming; });
		if (!alive() || marker == m_markers.markers.end())
			return;
		m_fieldOpen = true;
		m_field->SetValue(marker->name);
		m_field->SetClass("shown", true);
		place();
		m_field->Focus();
	}

	void MarkerLines::closeField(bool rename)
	{
		const uint64_t marker = std::exchange(m_renaming, 0);
		if (!std::exchange(m_fieldOpen, false))
			return;
		if (rename)
			m_binding.send("marker.set", {{"marker", marker}, {"name", m_field->GetValue()}});
		m_field->SetClass("shown", false);
		m_field->Blur(); // which comes back here, closed
	}

	float MarkerLines::ratio() const
	{
		const Rml::Context* context = m_area->GetContext();
		return context == nullptr ? 1.0F : context->GetDensityIndependentPixelRatio();
	}

	std::optional<MarkerLines::LanePoint> MarkerLines::markerLaneAt(Rml::Event& event) const
	{
		const TimelineView& view = m_display.view();
		const double x = (event.GetParameter<float>("mouse_x", 0.0F) - m_area->GetAbsoluteLeft()) / ratio();
		const double y = ((event.GetParameter<float>("mouse_y", 0.0F) - m_area->GetAbsoluteTop()) / ratio()) + view.top;
		const auto& tops = m_display.laneTops();
		if (tops.empty() || y < 0.0 || y >= tops.back())
			return std::nullopt;
		const auto lane = static_cast<size_t>(std::upper_bound(tops.begin(), tops.end(), y) - tops.begin() - 1);
		const auto id = m_display.laneTracks()[lane];
		const auto track = std::find_if(m_markers.tracks.begin(), m_markers.tracks.end(),
			[id](const ProjectMarkers::Track& candidate) { return candidate.id == id && candidate.marker; });
		if (track == m_markers.tracks.end())
			return std::nullopt;
		return LanePoint{&*track, view.start + (x * view.scale), y - tops[lane]};
	}

	void MarkerLines::ProcessEvent(Rml::Event& event)
	{
		if (!alive())
			return;
		const bool onField = event.GetTargetElement() == m_field;
		switch (event.GetId())
		{
		case Rml::EventId::Mousemove:
			dragTo(event, false);
			break;
		case Rml::EventId::Mouseup:
			dragTo(event, true);
			break;
		case Rml::EventId::Change:
			if (onField && event.GetParameter<bool>("linebreak", false))
				closeField(true);
			break;
		case Rml::EventId::Blur:
			if (onField)
				closeField(false);
			break;
		case Rml::EventId::Mousedown:
			if (onField)
				event.StopPropagation(); // the display doesn't select or draw under it
			else if (event.GetParameter<int>("button", 0) == 0)
				press(event);
			break;
		default:
			break;
		}
	}

	void MarkerLines::press(Rml::Event& event)
	{
		const std::optional<LanePoint> at = markerLaneAt(event);
		if (!at)
			return;
		const ProjectMarkers::Track& track = *at->track;
		const double seconds = at->seconds;
		const bool alt = event.GetParameter<int>("alt_key", 0) != 0;
		const bool shift = event.GetParameter<int>("shift_key", 0) != 0;
		const bool ctrl = event.GetParameter<int>("ctrl_key", 0) != 0 || event.GetParameter<int>("meta_key", 0) != 0;
		// The cycle marker under the mouse: the last one that starts before it.
		const MarkerItem* cycle = nullptr;
		for (const MarkerItem& marker : m_markers.markers)
			if (marker.track == track.id && marker.cycle && marker.start <= seconds && seconds <= marker.end)
				cycle = &marker;
		// The event display stops the mouse down, so RmlUi sends no dblclick: two clicks close in time and place make
		// one.
		const auto now = std::chrono::steady_clock::now();
		const Rml::Vector2f mouse = {
			event.GetParameter<float>("mouse_x", 0.0F), event.GetParameter<float>("mouse_y", 0.0F)};
		const bool twice = now - m_lastClick < g_doubleClickTime &&
			(mouse - m_lastMouse).SquaredMagnitude() <
				g_doubleClickDistance * g_doubleClickDistance * ratio() * ratio();
		m_lastClick = twice ? std::chrono::steady_clock::time_point() : now;
		m_lastMouse = mouse;
		const Hit hit = hitAt(event, *at);

		switch (m_display.tool())
		{
		case Tool::RangeSelection:
			// A double click selects the range between the markers around it on every track (p. 431); a click and
			// a drag select ranges as on any lane.
			if (twice)
			{
				m_binding.send("marker.range",
					{{"track", track.id}, {"position", positionSamples(seconds, m_markers.time.sampleRate)}},
					[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
					{
						if (alive.expired() || !result.ok())
							return;
						m_display.setRange(RangeSelection{result.value.at("start").get<int64_t>(),
							result.value.at("end").get<int64_t>(), m_display.laneTracks()});
					});
				event.StopPropagation();
			}
			return;
		case Tool::Erase:
		{
			// The marker clicked, with Alt every marker on the track from it or the click on (p. 425).
			std::vector<uint64_t> erased;
			const MarkerItem* from = nullptr;
			for (const MarkerItem& marker : m_markers.markers)
				if (marker.id == hit.marker)
					from = &marker;
			for (const MarkerItem& marker : m_markers.markers)
				if (marker.track == track.id &&
					(marker.id == hit.marker || (alt && marker.start >= (from != nullptr ? from->start : seconds))))
					erased.push_back(marker.id);
			if (!erased.empty())
				m_binding.send("marker.remove", {{"markers", erased}});
			event.StopPropagation();
			return;
		}
		default:
			break;
		}

		if (twice)
		{
			if (cycle != nullptr && alt)
				m_display.zoomTo(cycle->start, cycle->end);
			else if (cycle != nullptr)
				m_binding.send("marker.setLocators", {{"marker", cycle->id}});
			return;
		}
		if (alt)
		{
			// Alt-click adds a position marker, but not on a cycle marker, which Alt and a double click zoom to; with
			// Shift its name opens to type.
			if (cycle == nullptr)
				m_binding.send("marker.add",
					{{"track", track.id}, {"position", m_markers.position(track, m_display.snapped(seconds))}},
					[this, shift, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
					{
						if (!alive.expired() && result.ok() && shift)
							rename(result.value.at("marker").at("id").get<uint64_t>());
					});
			return;
		}
		m_pressX = mouse.x;
		m_pressSeconds = seconds;
		m_dragMoved = false;
		m_dragTrack = track.id;
		m_dragMarker = hit.marker;
		if (ctrl && hit.marker == 0)
		{
			// Ctrl and a drag adds a cycle marker, with Shift to rename.
			m_drag = Drag::NewCycle;
			m_dragFrom = m_display.snapped(seconds);
			m_renameNew = shift;
		}
		else
		{
			select(event, hit.marker);
			const bool selected = std::find(m_selection.begin(), m_selection.end(), hit.marker) != m_selection.end();
			if (ctrl || !selected || hit.part == Part::Inside || m_display.tool() != Tool::ObjectSelection)
				return;
			const auto marker = std::find_if(m_markers.markers.begin(), m_markers.markers.end(),
				[&hit](const MarkerItem& item) { return item.id == hit.marker; });
			if (marker == m_markers.markers.end())
				return;
			m_drag = Drag::Move;
			if (hit.part == Part::Start)
				m_drag = Drag::Start;
			else if (hit.part == Part::End)
				m_drag = Drag::End;
			m_dragFrom = m_drag == Drag::End ? marker->end : marker->start;
		}
		if (Rml::ElementDocument* document = m_area->GetOwnerDocument())
		{
			document->AddEventListener(Rml::EventId::Mousemove, this);
			document->AddEventListener(Rml::EventId::Mouseup, this);
		}
		event.StopPropagation(); // the display draws no rectangle
	}

	// A drag on the marker track: shows where it puts the markers, and on the release sends it.
	void MarkerLines::dragTo(Rml::Event& event, bool release)
	{
		if (m_drag == Drag::None)
			return;
		const Drag drag = m_drag;
		if (release)
		{
			m_drag = Drag::None;
			if (Rml::ElementDocument* document = m_area->GetOwnerDocument())
			{
				document->RemoveEventListener(Rml::EventId::Mousemove, this);
				document->RemoveEventListener(Rml::EventId::Mouseup, this);
			}
		}
		// Moved onto another marker track's lane, the markers go there.
		uint64_t onto = m_dragTrack;
		if (const std::optional<LanePoint> lane = markerLaneAt(event); drag == Drag::Move && lane)
			onto = lane->track->id;
		const auto mouseX = event.GetParameter<float>("mouse_x", 0.0F);
		// Sideways, or straight onto another marker track to keep the position.
		m_dragMoved = m_dragMoved || std::abs(mouseX - m_pressX) / ratio() >= g_dragStart || onto != m_dragTrack;
		if (!m_dragMoved)
			return;
		const TimelineView& view = m_display.view();
		// A new cycle marker ends where the mouse is: from the press's own time, not its snapped one.
		const double to =
			(drag == Drag::NewCycle ? m_pressSeconds : m_dragFrom) + ((mouseX - m_pressX) / ratio() * view.scale);
		const double at = m_display.snapped(to, drag == Drag::NewCycle ? std::nullopt : std::optional(m_dragFrom));
		const auto trackOf = [this](uint64_t id)
		{
			const auto found = std::find_if(m_markers.tracks.begin(), m_markers.tracks.end(),
				[id](const ProjectMarkers::Track& track) { return track.id == id; });
			return found != m_markers.tracks.end() ? *found : ProjectMarkers::Track{};
		};

		ProjectMarkers preview = m_markers;
		Json commands = Json::array();
		bool sent = false;
		if (drag == Drag::NewCycle)
		{
			MarkerItem item;
			item.track = m_dragTrack;
			item.cycle = true;
			item.start = std::min(m_dragFrom, at);
			item.end = std::max(m_dragFrom, at);
			if (item.end > item.start)
				preview.markers.push_back(item);
			sent = release && item.end > item.start;
			if (sent)
			{
				const ProjectMarkers::Track track = trackOf(m_dragTrack);
				m_binding.send("marker.add",
					{{"type", "cycle"}, {"track", m_dragTrack}, {"position", m_markers.position(track, item.start)},
						{"end", m_markers.position(track, item.end)}},
					[this, named = m_renameNew, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
					{
						if (alive.expired())
							return;
						if (result.ok() && named)
							rename(result.value.at("marker").at("id").get<uint64_t>());
						requestProject();
					});
			}
		}
		else
		{
			const double delta = at - m_dragFrom;
			for (MarkerItem& marker : preview.markers)
			{
				const bool moved = drag == Drag::Move &&
					std::find(m_selection.begin(), m_selection.end(), marker.id) != m_selection.end();
				if (!moved && marker.id != m_dragMarker)
					continue;
				Json parameters = {{"marker", marker.id}};
				if (moved)
				{
					marker.start = std::max(0.0, marker.start + delta);
					marker.end = std::max(marker.start, marker.end + delta);
					if (onto != m_dragTrack)
					{
						marker.track = onto;
						parameters["track"] = onto;
					}
					parameters["position"] = m_markers.position(trackOf(marker.track), marker.start);
				}
				else if (drag == Drag::Start)
				{
					marker.start = std::min(at, marker.end - (1.0 / m_markers.time.sampleRate));
					parameters["position"] = m_markers.position(trackOf(marker.track), marker.start);
					parameters["end"] = m_markers.position(trackOf(marker.track), marker.end);
				}
				else
				{
					marker.end = std::max(at, marker.start + (1.0 / m_markers.time.sampleRate));
					parameters["end"] = m_markers.position(trackOf(marker.track), marker.end);
				}
				commands.push_back({{"command", "marker.set"}, {"parameters", std::move(parameters)}});
			}
			sent = release && !commands.empty();
			if (sent)
				m_binding.send("history.transaction",
					{{"description", drag == Drag::Move ? "Move Markers" : "Resize Marker"}, {"commands", commands}},
					[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result&)
					{
						if (!alive.expired())
							requestProject();
					});
		}
		std::sort(preview.markers.begin(), preview.markers.end(), [](const MarkerItem& a, const MarkerItem& b)
			{ return std::tie(a.start, a.cycle, a.number, a.id) < std::tie(b.start, b.cycle, b.number, b.id); });
		// Shown until the project with the edit comes; a release that sends nothing shows the project again.
		if (release && !sent)
			m_preview.reset();
		else
			m_preview = std::move(preview);
		show();
	}
}
