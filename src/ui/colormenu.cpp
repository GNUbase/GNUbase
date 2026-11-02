// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/colormenu.h"

#include "engine/engine.h"
#include "ui/eventdisplay.h"
#include "ui/tracklist.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>

#include <cstdint>
#include <string>
#include <vector>

namespace gnubase::ui
{
	using engine::Json;

	ColorMenu::ColorMenu(EventDisplay& display, TrackList& tracks, EngineBinding& binding, Rml::Element& document)
		: m_display(display), m_tracks(tracks), m_binding(binding)
	{
		if (Rml::Element* button = document.GetElementById("select-color"))
		{
			m_button = button->GetObserverPtr();
			// Before the menu opens, and for Space and Enter.
			button->AddEventListener(Rml::EventId::Mousedown, this);
			button->AddEventListener(Rml::EventId::Click, this);
		}
		if (Rml::Element* menu = document.GetElementById("color-menu"))
		{
			m_menu = menu->GetObserverPtr();
			menu->AddEventListener(Rml::EventId::Click, this);
		}
		fill();
	}

	ColorMenu::~ColorMenu()
	{
		if (m_button)
		{
			m_button->RemoveEventListener(Rml::EventId::Mousedown, this);
			m_button->RemoveEventListener(Rml::EventId::Click, this);
		}
		if (m_menu)
			m_menu->RemoveEventListener(Rml::EventId::Click, this);
	}

	void ColorMenu::fill()
	{
		if (!m_menu)
			return;
		const bool events = !m_display.selection().empty();
		std::string rml = std::string(R"(<menuitem color="">)") +
			(events ? "Set Event Color to Track" : "Set Track Color to Default") + "</menuitem><menuseparator/>";
		const Json& project = m_display.project();
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
			Rml::Element* item = m_menu->GetChild(i);
			const auto color = item->GetAttribute<Rml::String>("color", "");
			if (Rml::Element* swatch = item->GetNumChildren() > 0 ? item->GetChild(0) : nullptr;
				swatch != nullptr && !color.empty())
				swatch->SetProperty("background-color", color);
		}
	}

	void ColorMenu::apply(const std::string& color)
	{
		const Json value = color.empty() ? Json(nullptr) : Json(color);
		if (const std::vector<uint64_t>& events = m_display.selection(); !events.empty())
		{
			m_binding.send("event.set", {{"events", events}, {"color", value}});
			return;
		}
		Json commands = Json::array();
		for (const uint64_t track : m_tracks.selection())
			commands.push_back({{"command", "track.set"}, {"parameters", {{"track", track}, {"color", value}}}});
		if (!commands.empty())
			m_binding.send(
				"history.transaction", {{"description", "Colorize Tracks"}, {"commands", std::move(commands)}});
	}

	void ColorMenu::ProcessEvent(Rml::Event& event)
	{
		Rml::Element* target = event.GetTargetElement();
		if (event.GetCurrentElement() == m_button.get())
		{
			fill();
			return;
		}
		while (target != nullptr && target != m_menu.get() && !target->HasAttribute("color"))
			target = target->GetParentNode();
		if (target != nullptr && target != m_menu.get())
			apply(target->GetAttribute<Rml::String>("color", ""));
	}
}
