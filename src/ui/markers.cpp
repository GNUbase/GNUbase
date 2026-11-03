// SPDX-License-Identifier: GPL-3.0-or-later
// The project's markers as the UI reads them, and the marker lines and marker events in the event display
// (ui/markers.h). The markers window is in markerswindow.cpp.
#include "ui/markers.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
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
		seconds = std::max(seconds, 0.0);
		return track.linear ? std::llround(seconds * time.sampleRate) : time.tempo.toTicks(seconds);
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
		std::vector<TimeSpan> lines;
		for (const MarkerItem& marker : m_markers.markers)
		{
			if (m_mode == MarkerLineMode::Off || (m_mode == MarkerLineMode::Active && marker.track != m_markers.active))
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
		for (const MarkerItem& marker : m_markers.markers)
		{
			std::string text;
			if (m_showIds)
				text = marker.cycle ? "[" + std::to_string(marker.number) + "]" : std::to_string(marker.number);
			if (!marker.name.empty())
				text += (text.empty() ? "" : " ") + marker.name;
			labels += R"(<div class="marker-label)" + std::string(marker.cycle ? " cycle" : "") +
				(marker.track == m_markers.active ? " active" : "") + R"(" marker=")" + std::to_string(marker.id) +
				R"(">)" + Rml::StringUtilities::EncodeRml(text) + "</div>";
		}
		m_labels->SetInnerRML(labels);
		showSelection();
		m_laneTops.clear(); // rebuild the lanes with the new marker events
		place();
	}

	void MarkerLines::setSelection(std::vector<uint64_t> markers)
	{
		m_selection = std::move(markers);
		showSelection();
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

	// A click selects the marker under the mouse, or none; with Ctrl (Cmd on macOS) it adds or removes it: the marker
	// whose label it is on, a position marker's line within reach, or else the cycle marker it is in.
	void MarkerLines::select(Rml::Event& event, const ProjectMarkers::Track& track, double seconds)
	{
		uint64_t hit = 0;
		const Rml::Vector2f mouse = {
			event.GetParameter<float>("mouse_x", 0.0F), event.GetParameter<float>("mouse_y", 0.0F)};
		for (int i = 0; i < m_labels->GetNumChildren() && hit == 0; ++i) // the labels let the mouse through
			if (Rml::Element* label = m_labels->GetChild(i); label->IsVisible() && label->IsPointWithinElement(mouse))
				hit = label->GetAttribute<uint64_t>("marker", 0);
		const double reach = g_markerReach * m_display.view().scale;
		for (const MarkerItem& marker : m_markers.markers)
			if (hit == 0 && marker.track == track.id && !marker.cycle && std::abs(marker.start - seconds) <= reach)
				hit = marker.id;
		for (const MarkerItem& marker : m_markers.markers)
			if (hit == 0 && marker.track == track.id && marker.cycle && marker.start <= seconds &&
				seconds <= marker.end)
				hit = marker.id;
		std::vector<uint64_t> selection = m_selection;
		if (event.GetParameter<int>("ctrl_key", 0) != 0 || event.GetParameter<int>("meta_key", 0) != 0)
		{
			const auto at = std::find(selection.begin(), selection.end(), hit);
			if (at != selection.end())
				selection.erase(at);
			else if (hit != 0)
				selection.push_back(hit);
		}
		else
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
				for (const MarkerItem& marker : m_markers.of(tracks[i]))
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
		for (int i = 0; i < m_labels->GetNumChildren() && i < static_cast<int>(m_markers.markers.size()); ++i)
		{
			Rml::Element* label = m_labels->GetChild(i);
			const MarkerItem& marker = m_markers.markers[static_cast<size_t>(i)];
			const auto lane = lanes.find(marker.track);
			label->SetClass("hidden", lane == lanes.end());
			if (lane == lanes.end())
				continue;
			const double top = tops[lane->second] - view.top;
			label->SetProperty(Rml::PropertyId::Left, dp((marker.start - view.start) / view.scale));
			label->SetProperty(Rml::PropertyId::Top, dp(top));
		}
	}

	float MarkerLines::ratio() const
	{
		const Rml::Context* context = m_area->GetContext();
		return context == nullptr ? 1.0F : context->GetDensityIndependentPixelRatio();
	}

	std::optional<std::pair<const ProjectMarkers::Track*, double>> MarkerLines::markerLaneAt(Rml::Event& event) const
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
		return std::make_pair(&*track, view.start + (x * view.scale));
	}

	void MarkerLines::ProcessEvent(Rml::Event& event)
	{
		if (!alive() || event.GetParameter<int>("button", 0) != 0)
			return;
		const auto at = markerLaneAt(event);
		if (!at)
			return;
		const auto [track, seconds] = *at;
		const bool alt = event.GetParameter<int>("alt_key", 0) != 0;
		// The cycle marker under the mouse: the last one that starts before it.
		const MarkerItem* cycle = nullptr;
		for (const MarkerItem& marker : m_markers.markers)
			if (marker.track == track->id && marker.cycle && marker.start <= seconds && seconds <= marker.end)
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
		if (!twice)
		{
			// Alt-click adds a position marker, but not on a cycle marker, which Alt and a double click zoom to.
			if (alt && cycle == nullptr)
				m_binding.send("marker.add", {{"track", track->id}, {"position", m_markers.position(*track, seconds)}});
			else if (!alt)
				select(event, *track, seconds);
		}
		else if (cycle != nullptr && alt)
		{
			m_display.zoomTo(cycle->start, cycle->end);
		}
		else if (cycle != nullptr)
		{
			m_binding.send("marker.setLocators", {{"marker", cycle->id}});
		}
	}
}
