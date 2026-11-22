// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/colormenu.h"

#include "engine/engine.h"
#include "ui/eventdisplay.h"
#include "ui/tracklist.h"
#include "ui/widgets.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::ui
{
	using engine::Json;

	ColorMenu::ColorMenu(std::function<EventDisplay*()> display, std::function<TrackList*()> tracks,
		EngineBinding& binding, Rml::Element& document)
		: m_display(std::move(display)), m_tracks(std::move(tracks)), m_binding(binding),
		  m_document(document.GetObserverPtr())
	{
		// Alt-clicks on the track list and the inspector, after the press that closes open menus.
		document.AddEventListener(Rml::EventId::Click, this);
		if (Rml::Element* button = document.GetElementById("select-color"))
		{
			m_button = button->GetObserverPtr();
			// Before the menu opens, and for Space and Enter.
			button->AddEventListener(Rml::EventId::Mousedown, this);
			button->AddEventListener(Rml::EventId::Click, this);
		}
		Rml::Element* menu = document.GetElementById("color-menu");
		if (menu == nullptr && document.GetOwnerDocument() != nullptr)
		{
			menu = document.AppendChild(document.GetOwnerDocument()->CreateElement("menu"));
			menu->SetId("color-menu");
		}
		if (menu != nullptr)
		{
			m_menu = menu->GetObserverPtr();
			menu->AddEventListener(Rml::EventId::Click, this);
		}
		if (Rml::Element* tool = document.QuerySelector("button[tool=color]"))
		{
			m_tool = tool->GetObserverPtr();
			tool->AddEventListener(Rml::EventId::Mousescroll, this);
		}
		fill();
		showToolColor();
	}

	ColorMenu::~ColorMenu()
	{
		if (m_document)
			m_document->RemoveEventListener(Rml::EventId::Click, this);
		if (m_button)
		{
			m_button->RemoveEventListener(Rml::EventId::Mousedown, this);
			m_button->RemoveEventListener(Rml::EventId::Click, this);
		}
		if (m_menu)
			m_menu->RemoveEventListener(Rml::EventId::Click, this);
		if (m_tool)
			m_tool->RemoveEventListener(Rml::EventId::Mousescroll, this);
	}

	void ColorMenu::openAt(float x, float y)
	{
		if (!m_menu)
			return;
		m_track = 0;
		fill();
		openMenu(*m_menu.get(), Rml::Vector2f(x, y));
	}

	void ColorMenu::showToolColor()
	{
		const EventDisplay* display = m_display();
		Rml::Element* strip = m_tool ? m_tool->QuerySelector(".tool-color") : nullptr;
		if (display == nullptr || strip == nullptr)
			return;
		// The color is user data (CLAUDE.md, UI conventions).
		if (const std::string color = display->toolColor(); color.empty())
			strip->RemoveProperty("background-color");
		else
			strip->SetProperty("background-color", color);
	}

	void ColorMenu::fill()
	{
		const EventDisplay* display = m_display();
		if (!m_menu || display == nullptr)
			return;
		const bool events = m_track == 0 && !display->selection().empty();
		std::string rml = std::string(R"(<menuitem color="">)") +
			(events ? "Set Event Color to Track" : "Set Track Color to Default") + "</menuitem><menuseparator/>";
		const Json& project = display->project();
		std::vector<std::string> colors;
		if (project.is_object())
			for (const Json& color : project.value("colors", Json::array()))
			{
				colors.push_back(color.at("color").get<std::string>());
				rml += R"(<menuitem color=")" + colors.back() + R"("><span class="color-swatch"/>)" +
					Rml::StringUtilities::EncodeRml(color.at("name").get<std::string>()) + "</menuitem>";
			}
		m_menu->SetInnerRML(rml);
		// The swatches show the colors, which are user data (CLAUDE.md, UI conventions).
		for (int i = 0; i < m_menu->GetNumChildren(); ++i)
		{
			const Rml::Element* item = m_menu->GetChild(i);
			const auto color = item->GetAttribute<Rml::String>("color", "");
			if (Rml::Element* swatch = item->GetNumChildren() > 0 ? item->GetChild(0) : nullptr;
				swatch != nullptr && !color.empty())
				swatch->SetProperty("background-color", color);
		}
	}

	void ColorMenu::apply(const std::string& color)
	{
		const EventDisplay* display = m_display();
		const TrackList* tracks = m_tracks();
		if (display == nullptr || tracks == nullptr)
			return;
		const Json value = color.empty() ? Json(nullptr) : Json(color);
		if (const std::vector<uint64_t>& events = display->selection(); m_track == 0 && !events.empty())
		{
			m_binding.send("event.set", {{"events", events}, {"color", value}});
			return;
		}
		Json commands = Json::array();
		for (const uint64_t track : m_track != 0 ? std::vector<uint64_t>{m_track} : tracks->selection())
			commands.push_back({{"command", "track.set"}, {"parameters", {{"track", track}, {"color", value}}}});
		if (!commands.empty())
			m_binding.send(
				"history.transaction", {{"description", "Colorize Tracks"}, {"commands", std::move(commands)}});
	}

	uint64_t ColorMenu::trackAt(Rml::Event& event) const
	{
		if (event.GetParameter<int>("button", 0) != 0 || event.GetParameter<int>("alt_key", 0) == 0)
			return 0;
		const Rml::Element* target = event.GetTargetElement();
		const Rml::Element* parent = target != nullptr ? target->GetParentNode() : nullptr;
		if (target == nullptr || parent == nullptr)
			return 0;
		// The inspector's Basics header: the track the inspector shows, the first selected one.
		if (target->IsClassSet("section-header") && parent->GetAttribute<Rml::String>("section", "") == "basics")
		{
			const TrackList* list = m_tracks();
			const std::vector<uint64_t> tracks = list != nullptr ? list->selection() : std::vector<uint64_t>{};
			return tracks.empty() ? 0 : tracks.front();
		}
		// The left area of a track in the track list: its row, or its color strip, which a theme may stretch across
		// the row, left of its name.
		Rml::Element* row = target->IsClassSet("track-color") ? target->GetParentNode() : event.GetTargetElement();
		if (row->GetTagName() != "listitem" || row->Closest("#track-list") == nullptr)
			return 0;
		if (Rml::Element* name = row->QuerySelector(".track-name");
			name == nullptr || event.GetParameter<float>("mouse_x", 0.0F) >= name->GetAbsoluteLeft())
			return 0;
		return static_cast<uint64_t>(row->GetAttribute<double>("value", 0.0));
	}

	void ColorMenu::ProcessEvent(Rml::Event& event)
	{
		const Rml::Element* target = event.GetTargetElement();
		if (event.GetCurrentElement() == m_tool.get())
		{
			// Down steps on through the color set, up back (reference manual p. 115).
			const auto delta = event.GetParameter<float>("wheel_delta_y", 0.0F);
			if (EventDisplay* display = m_display(); display != nullptr && delta != 0.0F)
				display->stepToolColor(delta > 0.0F ? 1 : -1);
			event.StopPropagation();
			return;
		}
		if (event.GetCurrentElement() == m_button.get())
		{
			m_track = 0;
			fill();
			return;
		}
		if (event.GetCurrentElement() == m_document.get())
		{
			if (const uint64_t track = trackAt(event); track != 0 && m_menu)
			{
				m_track = track;
				fill();
				openMenu(*m_menu.get(),
					Rml::Vector2f(
						event.GetParameter<float>("mouse_x", 0.0F), event.GetParameter<float>("mouse_y", 0.0F)));
			}
			return;
		}
		while (target != nullptr && target != m_menu.get() && !target->HasAttribute("color"))
			target = target->GetParentNode();
		if (target != nullptr && target != m_menu.get())
		{
			apply(target->GetAttribute<Rml::String>("color", ""));
			m_track = 0;
		}
	}
}
