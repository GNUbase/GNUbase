// SPDX-License-Identifier: GPL-3.0-or-later
// The markers window (ui/markers.h): the active marker track's markers in a table, to locate, edit, add and remove.
#include "ui/markers.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <array>
#include <sstream>
#include <string>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		constexpr std::array<Rml::EventId, 3> g_events = {
			Rml::EventId::Click, Rml::EventId::Change, Rml::EventId::Keydown};

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
	}

	MarkersWindow::MarkersWindow(engine::Engine& engine, EngineBinding& binding, Rml::ElementDocument& document)
		: m_engine(engine), m_binding(binding), m_document(document.GetObserverPtr()),
		  m_list(document.GetElementById("marker-list")), m_rows(document.GetElementById("marker-rows")),
		  m_tracks(document.GetElementById("marker-track")),
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
				setCursor(event.data.at("position").get<int64_t>());
				if (!event.data.at("playing").get<bool>())
					m_position.reset();
				else if (!m_position)
					m_position = m_binding.watch<int64_t>([this] { return m_engine.playbackPosition(); },
						[this](const int64_t& sample) { setCursor(sample); });
			}));
		m_binding.send("transport.get", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (!alive.expired() && result.ok())
					setCursor(result.value.at("position").get<int64_t>());
			});
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
				}
				if (std::exchange(m_stale, false))
					requestProject();
			});
	}

	void MarkersWindow::show()
	{
		m_listed = m_markers.of(m_markers.active);
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
		for (const MarkerItem& marker : m_listed)
		{
			const std::string id = std::to_string(marker.id);
			const bool isSelected = std::find(selected.begin(), selected.end(), marker.id) != selected.end();
			if (isSelected)
				value += (value.empty() ? "" : ",") + id;
			rows += R"(<tr value=")" + id + R"(" class="marker-row)" + (isSelected ? " selected" : "") + R"(">)";
			rows += R"(<td class="locate"><svg class="icon locate-arrow" src="../icons/transport-play.svg"/></td>)";
			rows += R"(<td class="number"><input type="text" value=")" + std::to_string(marker.number) + R"("/></td>)";
			rows += R"(<td class="type">)" + std::string(marker.cycle ? "Cycle" : "Position") + "</td>";
			rows += R"(<td class="position">)" + formatTime(TimeFormat::BarsBeats, marker.start, context) + "</td>";
			rows += R"(<td class="end">)" +
				(marker.cycle ? formatTime(TimeFormat::BarsBeats, marker.end, context) : std::string()) + "</td>";
			// ponytail: lengths in seconds, as formatTime writes positions; bars and beats lengths if users miss them.
			rows += R"(<td class="length">)" +
				(marker.cycle ? formatTime(TimeFormat::Seconds, marker.end - marker.start, context) : std::string()) +
				"</td>";
			rows += R"(<td class="description"><input type="text" value=")" + text(marker.name) + R"("/></td>)";
			rows += "</tr>";
		}
		m_rows->SetInnerRML(rows);
		if (m_list != nullptr)
			m_list->SetAttribute("value", value);
		showCursor();
	}

	void MarkersWindow::setCursor(int64_t sample)
	{
		m_cursorSample = sample;
		showCursor();
	}

	// The arrow at the marker at the cursor, or the last one before it.
	void MarkersWindow::showCursor()
	{
		if (!alive() || m_rows == nullptr)
			return;
		const double cursor = static_cast<double>(m_cursorSample) / m_markers.time.sampleRate;
		uint64_t at = 0;
		for (const MarkerItem& marker : m_listed)
			if (marker.start <= cursor + 1e-9)
				at = marker.id;
		for (int i = 0; i < m_rows->GetNumChildren(); ++i)
		{
			Rml::Element* row = m_rows->GetChild(i);
			row->SetClass("at-cursor", at != 0 && row->GetAttribute<uint64_t>("value", 0) == at);
		}
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
			if (target != nullptr && target == m_tracks)
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
		default:
			break;
		}
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
		else if (const Rml::Element* cell = closest(target, "td"); cell != nullptr && cell->IsClassSet("locate"))
		{
			if (const uint64_t marker = rowMarker(target); marker != 0)
				m_binding.send("marker.locate", {{"marker", marker}});
		}
	}

	// A text field of a row: the id number or the description, which marker.set changes. If the engine refuses, the
	// field shows the marker as it is again.
	void MarkersWindow::commit(Rml::Element& field)
	{
		const uint64_t id = rowMarker(&field);
		const auto marker =
			std::find_if(m_listed.begin(), m_listed.end(), [id](const MarkerItem& each) { return each.id == id; });
		const Rml::Element* cell = field.GetParentNode();
		auto* control = dynamic_cast<Rml::ElementFormControl*>(&field);
		if (marker == m_listed.end() || cell == nullptr || control == nullptr)
			return;
		const std::string value = control->GetValue();
		Json change = {{"marker", id}};
		if (cell->IsClassSet("description") && value != marker->name)
		{
			change["name"] = value;
		}
		else if (cell->IsClassSet("number") && value != std::to_string(marker->number))
		{
			int number = 0;
			std::istringstream read(value);
			if (!(read >> number) || !read.eof())
			{
				show();
				return;
			}
			change["number"] = number;
		}
		else
		{
			return;
		}
		m_binding.send("marker.set", change,
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (!alive.expired() && !result.ok())
					show();
			});
	}
}
