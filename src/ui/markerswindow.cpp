// SPDX-License-Identifier: GPL-3.0-or-later
// The markers window (ui/markers.h): the active marker track's markers in a table, to locate, edit, add and remove.
#include "ui/markers.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/ScrollTypes.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <sstream>
#include <string>
#include <tuple>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		constexpr std::array<Rml::EventId, 6> g_events = {Rml::EventId::Click, Rml::EventId::Change,
			Rml::EventId::Keydown, Rml::EventId::Mousedown, Rml::EventId::Mousemove, Rml::EventId::Mouseup};

		// The columns after the locate column, in their usual order, and their headers.
		constexpr std::array<const char*, 6> g_columns = {"number", "type", "position", "end", "length", "description"};
		constexpr std::array<const char*, 6> g_headers = {"ID", "Type", "Position", "End", "Length", "Description"};

		// The user settings the window follows.
		constexpr std::array<const char*, 6> g_settings = {"ui.timeFormats", "ui.markerColumns", "ui.markersAutoScroll",
			"ui.markersSyncSelection", "ui.markerIds", "transport.cycleFollowsMarkers"};

		// The menu's and toolbar's toggles, by the setting they turn on and off.
		constexpr std::array<std::pair<const char*, const char*>, 4> g_toggles = {{
			{"marker-auto-scroll", "ui.markersAutoScroll"},
			{"sync-selection", "ui.markersSyncSelection"},
			{"show-marker-ids", "ui.markerIds"},
			{"cycle-follows", "transport.cycleFollowsMarkers"},
		}};

		// Columns are at least this wide, in dp; a drag closer than this to a header's edge resizes, and one this far
		// moves a column.
		constexpr float g_minWidth = 16.0F;
		constexpr float g_maxWidth = 2000.0F;
		constexpr float g_reach = 4.0F;

		// The element or its nearest ancestor with the tag, or null.
		Rml::Element* closest(Rml::Element* element, const std::string& tag)
		{
			while (element != nullptr && element->GetTagName() != tag)
				element = element->GetParentNode();
			return element;
		}

		bool inside(const Rml::Element* element, const Rml::Element* ancestor)
		{
			for (; element != nullptr; element = element->GetParentNode())
				if (element == ancestor)
					return true;
			return false;
		}

		// The marker id of the row the element is in, 0 outside a row.
		uint64_t rowMarker(Rml::Element* element)
		{
			const Rml::Element* row = closest(element, "tr");
			return row == nullptr ? 0 : row->GetAttribute<uint64_t>("value", 0);
		}

		std::string text(const std::string& value)
		{
			return Rml::StringUtilities::EncodeRml(value);
		}

		bool isColumn(const std::string& name)
		{
			return std::find(g_columns.begin(), g_columns.end(), name) != g_columns.end();
		}

		std::string lower(std::string value)
		{
			std::transform(value.begin(), value.end(), value.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return value;
		}

		// The order of two markers in a column, as -1, 0 or 1.
		int compare(const std::string& column, const MarkerItem& a, const MarkerItem& b)
		{
			const auto order = [](const auto& x, const auto& y)
			{
				if (x < y)
					return -1;
				return y < x ? 1 : 0;
			};
			if (column == "number")
				return order(a.number, b.number);
			if (column == "type")
				return order(a.cycle, b.cycle); // position markers first
			if (column == "end")
				return order(a.end, b.end);
			if (column == "length")
				return order(a.end - a.start, b.end - b.start);
			if (column == "description")
				return order(lower(a.name), lower(b.name));
			return order(a.start, b.start);
		}

		void setChecked(Rml::Element* element, bool checked)
		{
			if (element == nullptr)
				return;
			if (checked)
				element->SetAttribute("checked", "");
			else
				element->RemoveAttribute("checked");
		}
	}

	MarkersWindow::Columns MarkersWindow::Columns::read(const Json& value)
	{
		Columns columns;
		if (!value.is_object())
			return columns;
		if (const auto order = value.find("order"); order != value.end() && order->is_array())
		{
			std::vector<std::string> named;
			for (const Json& column : *order)
				if (column.is_string() && isColumn(column.get<std::string>()) &&
					std::find(named.begin(), named.end(), column.get<std::string>()) == named.end())
					named.push_back(column.get<std::string>());
			for (const std::string& column : columns.order)
				if (std::find(named.begin(), named.end(), column) == named.end())
					named.push_back(column);
			columns.order = std::move(named);
		}
		if (const auto widths = value.find("widths"); widths != value.end() && widths->is_object())
			for (const auto& [column, width] : widths->items())
				if (isColumn(column) && width.is_number() && std::isfinite(width.get<double>()))
					columns.widths[column] = std::clamp(width.get<float>(), g_minWidth, g_maxWidth);
		if (const auto sort = value.find("sort");
			sort != value.end() && sort->is_string() && isColumn(sort->get<std::string>()))
			columns.sort = sort->get<std::string>();
		const auto descending = value.find("descending");
		columns.descending = descending != value.end() && descending->is_boolean() && descending->get<bool>();
		return columns;
	}

	Json MarkersWindow::Columns::json() const
	{
		return {{"order", order}, {"widths", widths}, {"sort", sort}, {"descending", descending}};
	}

	MarkersWindow::MarkersWindow(engine::Engine& engine, EngineBinding& binding, Rml::ElementDocument& document)
		: m_engine(engine), m_binding(binding), m_document(document.GetObserverPtr()),
		  m_list(document.GetElementById("marker-list")), m_head(document.GetElementById("marker-head")),
		  m_rows(document.GetElementById("marker-rows")), m_tracks(document.GetElementById("marker-track")),
		  m_functions(document.GetElementById("marker-functions-menu"))
	{
		for (const Rml::EventId id : g_events)
			document.AddEventListener(id, this);
		document.AddEventListener(Rml::EventId::Blur, this, true); // blur doesn't bubble
		for (const char* event : {"history.changed", "project.reset"})
			m_subscriptions.push_back(m_binding.on(event, [this](const engine::Event&) { requestProject(); }));
		m_subscriptions.push_back(m_binding.on("transport.changed",
			[this](const engine::Event& event)
			{
				if (std::exchange(m_recording, event.data.value("recording", false)) && !m_recording)
					resync();
				setCursor(event.data.at("position").get<int64_t>());
				if (!event.data.at("playing").get<bool>())
					m_position.reset();
				else if (!m_position)
					m_position = m_binding.watch<int64_t>([this] { return m_engine.playbackPosition(); },
						[this](const int64_t& sample) { setCursor(sample); });
			}));
		m_subscriptions.push_back(m_binding.on("settings.changed",
			[this](const engine::Event& event)
			{
				const Json& values = event.data.at("values");
				for (const char* key : g_settings)
					if (const auto value = values.find(key); value != values.end())
					{
						m_settingsKnown.insert(key);
						applySetting(key, *value);
					}
			}));
		for (const char* key : g_settings)
			m_binding.send("settings.get", {{"key", key}},
				[this, key = std::string(key), alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
				{
					// Newer changes win.
					if (alive.expired() || !result.ok() || m_settingsKnown.count(key) != 0)
						return;
					applySetting(key, result.value.at("value"));
				});
		m_binding.send("transport.get", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok())
					return;
				m_recording = result.value.value("recording", false);
				setCursor(result.value.at("position").get<int64_t>());
			});
		showHead();
		requestProject();
	}

	MarkersWindow::~MarkersWindow()
	{
		if (!alive())
			return;
		for (const Rml::EventId id : g_events)
			m_document->RemoveEventListener(id, this);
		m_document->RemoveEventListener(Rml::EventId::Blur, this, true);
	}

	std::vector<uint64_t> MarkersWindow::selection() const
	{
		std::vector<uint64_t> ids;
		if (!alive() || m_list == nullptr)
			return ids;
		std::istringstream values(m_list->GetAttribute<Rml::String>("value", ""));
		for (std::string value; std::getline(values, value, ',');)
			if (!value.empty())
				ids.push_back(std::stoull(value));
		return ids;
	}

	void MarkersWindow::setSelection(const std::vector<uint64_t>& markers)
	{
		if (!alive() || m_list == nullptr)
			return;
		std::string value;
		for (const MarkerItem& marker : m_listed)
			if (std::find(markers.begin(), markers.end(), marker.id) != markers.end())
				value += (value.empty() ? "" : ",") + std::to_string(marker.id);
		m_list->SetAttribute("value", value);
	}

	void MarkersWindow::syncFrom(std::function<std::vector<uint64_t>()> source)
	{
		m_syncFrom = std::move(source);
		resync();
	}

	// With Sync Selection on again (opened, after recording, the setting turned on), the window takes the project
	// window's selection.
	void MarkersWindow::resync()
	{
		if (m_loaded && m_syncFrom && syncsSelection())
			setSelection(m_syncFrom());
	}

	void MarkersWindow::applySetting(const std::string& key, const Json& value)
	{
		if (key == "ui.timeFormats")
		{
			const auto primary = value.is_object() ? value.find("primary") : value.end();
			const std::optional<TimeFormat> format = primary != value.end() && primary->is_string()
				? timeFormatFromName(primary->get<std::string>())
				: std::nullopt;
			m_format = format.value_or(TimeFormat::BarsBeats);
			show();
			return;
		}
		if (key == "ui.markerColumns")
		{
			Columns columns = Columns::read(value);
			if (columns.json() == m_columns.json())
				return; // as shown: the answer to the window's own change
			m_columns = std::move(columns);
			showHead();
			show();
			return;
		}
		const auto flag = m_flags.find(key);
		if (flag == m_flags.end() || !value.is_boolean())
			return;
		const bool was = std::exchange(flag->second, value.get<bool>());
		if (key == "ui.markersAutoScroll" && flag->second)
			scrollToArrow();
		if (key == "ui.markersSyncSelection" && flag->second && !was)
			resync();
		if (!alive())
			return;
		for (const auto& [id, setting] : g_toggles)
			if (key == setting)
				setChecked(m_document->GetElementById(id), flag->second);
	}

	// ponytail: reads the whole project after every change, as the event display does.
	void MarkersWindow::requestProject()
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
					if (!std::exchange(m_loaded, true))
						resync();
				}
				if (std::exchange(m_stale, false))
					requestProject();
			});
	}

	// The header: the locate column, then the others in their order, the one the list is sorted by with .sorted (and
	// .descending), at the widths the user gave them.
	void MarkersWindow::showHead()
	{
		if (!alive() || m_head == nullptr)
			return;
		std::string cells = R"(<td class="locate"/>)";
		for (const std::string& column : m_columns.order)
		{
			const auto index =
				static_cast<size_t>(std::find(g_columns.begin(), g_columns.end(), column) - g_columns.begin());
			std::string classes = column;
			if (column == m_columns.sort)
				classes += m_columns.descending ? " sorted descending" : " sorted";
			cells += R"(<td class=")";
			cells += classes;
			cells += R"(" column=")";
			cells += column;
			cells += R"(">)";
			cells += g_headers[index];
			cells += "</td>";
		}
		m_head->SetInnerRML(cells);
		for (int i = 0; i < m_head->GetNumChildren(); ++i)
		{
			Rml::Element* cell = m_head->GetChild(i);
			const auto width = m_columns.widths.find(cell->GetAttribute<Rml::String>("column", ""));
			if (width != m_columns.widths.end())
				cell->SetProperty(Rml::PropertyId::Width, Rml::Property(width->get<float>(), Rml::Unit::DP));
		}
	}

	void MarkersWindow::show()
	{
		m_listed = m_markers.of(m_markers.active);
		std::stable_sort(m_listed.begin(), m_listed.end(),
			[this](const MarkerItem& a, const MarkerItem& b)
			{
				const int order = compare(m_columns.sort, a, b);
				if (order != 0)
					return m_columns.descending ? order > 0 : order < 0;
				// The position second, then as in the timeline.
				return std::tie(a.start, a.cycle, a.number, a.id) < std::tie(b.start, b.cycle, b.number, b.id);
			});
		if (!alive() || m_rows == nullptr)
			return;
		if (auto* tracks = dynamic_cast<Rml::ElementFormControlSelect*>(m_tracks))
		{
			tracks->RemoveAll();
			for (const ProjectMarkers::Track& track : m_markers.tracks)
				if (track.marker)
					tracks->Add(text(track.name), std::to_string(track.id));
			tracks->SetValue(std::to_string(m_markers.active));
		}

		// The rows of the markers that are still there stay selected.
		const std::vector<uint64_t> selected = selection();
		std::string value;
		std::string rows;
		const TimeContext& context = m_markers.time;
		const auto field = [](const std::string& column, const std::string& content)
		{ return R"(<td class=")" + column + R"("><input type="text" value=")" + text(content) + R"("/></td>)"; };
		for (const MarkerItem& marker : m_listed)
		{
			const std::string id = std::to_string(marker.id);
			const bool isSelected = std::find(selected.begin(), selected.end(), marker.id) != selected.end();
			if (isSelected)
				value += (value.empty() ? "" : ",") + id;
			rows += R"(<tr value=")" + id + R"(" class="marker-row)" + (isSelected ? " selected" : "") + R"(">)";
			rows += R"(<td class="locate"><svg class="icon locate-arrow" src="../icons/transport-play.svg"/></td>)";
			for (const std::string& column : m_columns.order)
			{
				if (column == "number")
					rows += field(column, std::to_string(marker.number));
				else if (column == "type")
					rows += R"(<td class="type">)" + std::string(marker.cycle ? "Cycle" : "Position") + "</td>";
				else if (column == "position")
					rows += field(column, formatTime(m_format, marker.start, context));
				else if (column == "end" && marker.cycle)
					rows += field(column, formatTime(m_format, marker.end, context));
				else if (column == "length" && marker.cycle)
					rows += field(column, formatLength(m_format, marker.start, marker.end, context));
				else if (column == "description")
					rows += field(column, marker.name);
				else
					rows += R"(<td class=")" + column + R"("/>)";
			}
			rows += "</tr>";
		}
		m_rebuilding = true;
		m_rows->SetInnerRML(rows);
		m_rebuilding = false;
		if (m_list != nullptr)
			m_list->SetAttribute("value", value);
		showCursor();
	}

	void MarkersWindow::setCursor(int64_t sample)
	{
		m_cursorSample = sample;
		showCursor();
	}

	// The arrow at the marker at the cursor, or the last one before it; with Auto-Scroll, the list scrolls to it when
	// it moves to another marker.
	void MarkersWindow::showCursor()
	{
		if (!alive() || m_rows == nullptr)
			return;
		const double cursor = static_cast<double>(m_cursorSample) / m_markers.time.sampleRate;
		uint64_t at = 0;
		double atStart = 0.0;
		for (const MarkerItem& marker : m_listed)
			if (marker.start <= cursor + 1e-9 && (at == 0 || marker.start >= atStart))
			{
				at = marker.id;
				atStart = marker.start;
			}
		for (int i = 0; i < m_rows->GetNumChildren(); ++i)
		{
			Rml::Element* row = m_rows->GetChild(i);
			row->SetClass("at-cursor", at != 0 && row->GetAttribute<uint64_t>("value", 0) == at);
		}
		if (std::exchange(m_arrow, at) != at && m_flags.at("ui.markersAutoScroll"))
			scrollToArrow();
	}

	void MarkersWindow::scrollToArrow()
	{
		if (!alive() || m_rows == nullptr)
			return;
		m_rows->GetOwnerDocument()->UpdateDocument(); // rows just made have no place yet
		for (int i = 0; i < m_rows->GetNumChildren(); ++i)
			if (Rml::Element* row = m_rows->GetChild(i); row->IsClassSet("at-cursor"))
				row->ScrollIntoView({Rml::ScrollAlignment::Nearest, Rml::ScrollAlignment::Nearest,
					Rml::ScrollBehavior::Instant, Rml::ScrollParentage::Closest});
	}

	void MarkersWindow::ProcessEvent(Rml::Event& event)
	{
		if (!alive())
			return;
		Rml::Element* target = event.GetTargetElement();
		switch (event.GetId())
		{
		case Rml::EventId::Click:
			click(target);
			break;
		case Rml::EventId::Change:
			if (target != nullptr && target == m_list)
			{
				if (m_onSelect)
					m_onSelect(selection());
			}
			else if (target != nullptr && target == m_tracks)
			{
				const auto track = event.GetParameter<Rml::String>("value", "");
				if (!track.empty() && track != std::to_string(m_markers.active))
					m_binding.send("marker.activateTrack", {{"track", std::stoull(track)}});
			}
			else if (target != nullptr && target->GetTagName() == "input" &&
				event.GetParameter<bool>("linebreak", false))
			{
				commit(*target);
			}
			break;
		case Rml::EventId::Blur:
			if (target != nullptr && target->GetTagName() == "input" && inside(target, m_rows))
				commit(*target);
			break;
		case Rml::EventId::Keydown:
		{
			// Enter in the list locates to the selected marker (reference manual p. 428).
			const auto key = static_cast<Rml::Input::KeyIdentifier>(event.GetParameter<int>("key_identifier", 0));
			const std::vector<uint64_t> selected = selection();
			if (key == Rml::Input::KI_RETURN && target != nullptr && target->GetTagName() != "input" &&
				inside(target, m_list) && !selected.empty())
			{
				m_binding.send("marker.locate", {{"marker", selected.front()}});
				event.StopPropagation();
			}
			break;
		}
		case Rml::EventId::Mousedown:
		case Rml::EventId::Mousemove:
		case Rml::EventId::Mouseup:
			header(event);
			break;
		default:
			break;
		}
	}

	// Dragging a header moves its column, dragging the line between two headers resizes the column on its left, and a
	// click sorts by the column, a second one the other way round (reference manual p. 429).
	void MarkersWindow::header(Rml::Event& event)
	{
		const Rml::Context* context = m_document->GetContext();
		const float ratio = context == nullptr ? 1.0F : context->GetDensityIndependentPixelRatio();
		const auto x = event.GetParameter<float>("mouse_x", 0.0F);
		if (event.GetId() == Rml::EventId::Mousedown)
		{
			Rml::Element* cell = closest(event.GetTargetElement(), "td");
			if (event.GetParameter<int>("button", 0) != 0 || cell == nullptr || cell->GetParentNode() != m_head ||
				m_head == nullptr)
				return;
			const float left = cell->GetAbsoluteLeft();
			const float right = left + cell->GetOffsetWidth();
			HeaderDrag drag;
			drag.x = x;
			if (right - x <= g_reach * ratio)
			{
				drag.resize = true;
			}
			else if (x - left <= g_reach * ratio)
			{
				drag.resize = true;
				cell = cell->GetPreviousSibling();
			}
			if (cell == nullptr || !cell->HasAttribute("column"))
				return;
			drag.column = cell->GetAttribute<Rml::String>("column", "");
			drag.width = cell->GetBox().GetSize(Rml::BoxArea::Content).x / ratio;
			m_drag = drag;
			event.StopPropagation();
			return;
		}
		if (!m_drag)
			return;
		const float distance = (x - m_drag->x) / ratio;
		if (m_drag->resize)
		{
			const float width = std::clamp(m_drag->width + distance, g_minWidth, g_maxWidth);
			m_columns.widths[m_drag->column] = width;
			for (int i = 0; i < m_head->GetNumChildren(); ++i)
				if (Rml::Element* cell = m_head->GetChild(i);
					cell->GetAttribute<Rml::String>("column", "") == m_drag->column)
					cell->SetProperty(Rml::PropertyId::Width, Rml::Property(width, Rml::Unit::DP));
		}
		else if (std::abs(distance) > g_reach)
		{
			m_drag->moved = true;
		}
		if (event.GetId() != Rml::EventId::Mouseup)
			return;
		const HeaderDrag drag = *m_drag;
		m_drag.reset();
		if (drag.resize)
		{
			saveColumns();
			return;
		}
		if (!drag.moved)
		{
			m_columns.descending = m_columns.sort == drag.column && !m_columns.descending;
			m_columns.sort = drag.column;
			saveColumns();
			return;
		}
		// Dropped on a header: before it on its left half, after it on its right half.
		for (int i = 0; i < m_head->GetNumChildren(); ++i)
		{
			Rml::Element* cell = m_head->GetChild(i);
			const auto column = cell->GetAttribute<Rml::String>("column", "");
			const float left = cell->GetAbsoluteLeft();
			if (column.empty() || x < left || x >= left + cell->GetOffsetWidth() || column == drag.column)
				continue;
			std::vector<std::string>& order = m_columns.order;
			order.erase(std::find(order.begin(), order.end(), drag.column));
			auto to = std::find(order.begin(), order.end(), column);
			if (x >= left + (cell->GetOffsetWidth() / 2.0F))
				++to;
			order.insert(to, drag.column);
			saveColumns();
			return;
		}
	}

	void MarkersWindow::saveColumns()
	{
		m_settingsKnown.insert("ui.markerColumns");
		showHead();
		show();
		m_binding.send("settings.set", {{"key", "ui.markerColumns"}, {"value", m_columns.json()}});
	}

	void MarkersWindow::click(Rml::Element* target)
	{
		if (target == nullptr)
			return;
		const auto on = [target](const char* id)
		{
			for (const Rml::Element* element = target; element != nullptr; element = element->GetParentNode())
				if (element->GetId() == id)
					return !element->HasAttribute("disabled");
			return false;
		};
		const std::vector<uint64_t> selected = selection();
		for (const auto& [id, setting] : g_toggles)
			if (on(id))
			{
				// The toggle turns itself; the setting, once changed, turns it to the setting.
				m_binding.send("settings.set", {{"key", setting}, {"value", !m_flags.at(setting)}});
				return;
			}
		if (on("add-position-marker"))
		{
			m_binding.send("marker.add");
		}
		else if (on("add-cycle-marker"))
		{
			m_binding.send("marker.add", {{"type", "cycle"}});
		}
		else if (on("remove-marker"))
		{
			if (!selected.empty())
				m_binding.send("marker.remove", {{"markers", selected}});
		}
		else if (on("move-to-cursor"))
		{
			// One undo step for them all; a cycle marker moves as a whole.
			Json commands = Json::array();
			for (const uint64_t id : selected)
				commands.push_back(
					{{"command", "marker.set"}, {"parameters", {{"marker", id}, {"position", "cursor"}}}});
			if (!commands.empty())
				m_binding.send(
					"history.transaction", {{"description", "Move Markers to Cursor"}, {"commands", commands}});
		}
		else if (on("reassign-position-ids") || on("reassign-cycle-ids"))
		{
			if (m_markers.active != 0)
				m_binding.send("marker.renumber",
					{{"track", m_markers.active}, {"type", on("reassign-cycle-ids") ? "cycle" : "position"}});
		}
		else if (const Rml::Element* cell = closest(target, "td");
			cell != nullptr && cell->IsClassSet("locate") && inside(cell, m_rows))
		{
			if (const uint64_t marker = rowMarker(target); marker != 0)
				m_binding.send("marker.locate", {{"marker", marker}});
		}
	}

	// A text field of a row: the id number, the description, the position, the end or the length, which marker.set
	// changes. A description or length typed into a row of a selection of several changes all of them (the length the
	// cycle markers'), in one undo step. If what is typed isn't valid or the engine refuses it, the fields show the
	// markers as they are again.
	void MarkersWindow::commit(Rml::Element& field)
	{
		if (m_rebuilding)
			return; // the rows are replaced: the field loses the focus without an edit
		const uint64_t id = rowMarker(&field);
		const auto marker =
			std::find_if(m_listed.begin(), m_listed.end(), [id](const MarkerItem& each) { return each.id == id; });
		const Rml::Element* cell = field.GetParentNode();
		auto* control = dynamic_cast<Rml::ElementFormControl*>(&field);
		if (marker == m_listed.end() || cell == nullptr || control == nullptr)
			return;
		const std::string value = control->GetValue();
		const TimeContext& context = m_markers.time;
		const auto track = std::find_if(m_markers.tracks.begin(), m_markers.tracks.end(),
			[&marker](const ProjectMarkers::Track& each) { return each.id == marker->track; });
		if (track == m_markers.tracks.end())
			return;

		// The markers it changes: the row's, or the selected ones if the row is one of several selected.
		std::vector<const MarkerItem*> markers = {&*marker};
		const std::vector<uint64_t> selected = selection();
		const bool many = cell->IsClassSet("description") || cell->IsClassSet("length");
		if (many && selected.size() > 1 && std::find(selected.begin(), selected.end(), id) != selected.end())
		{
			markers.clear();
			for (const MarkerItem& each : m_listed)
				if (std::find(selected.begin(), selected.end(), each.id) != selected.end())
					markers.push_back(&each);
		}

		Json changes = Json::array();
		const auto change = [&changes](const MarkerItem& item, Json parameters)
		{
			parameters["marker"] = item.id;
			changes.push_back(std::move(parameters));
		};
		if (cell->IsClassSet("description"))
		{
			if (value == marker->name)
				return;
			for (const MarkerItem* each : markers)
				if (each->name != value)
					change(*each, {{"name", value}});
		}
		else if (cell->IsClassSet("number"))
		{
			int number = 0;
			std::istringstream read(value);
			if (!(read >> number) || !read.eof())
			{
				show();
				return;
			}
			if (number != marker->number)
				change(*marker, {{"number", number}});
		}
		else if (cell->IsClassSet("position") || cell->IsClassSet("end"))
		{
			const bool end = cell->IsClassSet("end");
			if (value == formatTime(m_format, end ? marker->end : marker->start, context))
				return;
			const std::optional<double> seconds = parseTime(m_format, value, context);
			if (!seconds || (end && *seconds <= marker->start))
			{
				show();
				return;
			}
			change(*marker, {{end ? "end" : "position", m_markers.position(*track, *seconds)}});
		}
		else if (cell->IsClassSet("length"))
		{
			if (value == formatLength(m_format, marker->start, marker->end, context))
				return;
			for (const MarkerItem* each : markers)
			{
				const std::optional<double> length = parseLength(m_format, value, each->start, context);
				if (!length || *length <= 0.0)
				{
					show();
					return;
				}
				if (each->cycle)
					change(*each, {{"end", m_markers.position(*track, each->start + *length)}});
			}
		}
		if (changes.empty())
			return;
		const auto refused = [this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
		{
			if (!alive.expired() && !result.ok())
				show();
		};
		if (changes.size() == 1)
		{
			m_binding.send("marker.set", changes.front(), refused);
			return;
		}
		Json commands = Json::array();
		for (Json& parameters : changes)
			commands.push_back({{"command", "marker.set"}, {"parameters", std::move(parameters)}});
		m_binding.send("history.transaction",
			{{"description", cell->IsClassSet("length") ? "Change Marker Lengths" : "Rename Markers"},
				{"commands", commands}},
			refused);
	}
}
