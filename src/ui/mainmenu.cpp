// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/mainmenu.h"

#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>

#include <stdexcept>
#include <utility>

namespace gnubase::ui
{
	namespace
	{
		// The element or its nearest ancestor with that attribute, or null.
		Rml::Element* closestWith(Rml::Element* element, const char* attribute)
		{
			while (element != nullptr && !element->HasAttribute(attribute))
				element = element->GetParentNode();
			return element;
		}
	}

	MainMenu::MainMenu(KeyCommands& keys, Window& window, Rml::ElementDocument& document)
		: m_keys(keys), m_window(window), m_document(document.GetObserverPtr())
	{
		Rml::ElementList found;
		document.QuerySelectorAll(found, "menuitem[command]");
		for (Rml::Element* element : found)
		{
			std::string command = element->GetAttribute<Rml::String>("command", "");
			const auto text = element->GetAttribute<Rml::String>("parameters", "{}");
			engine::Json parameters = engine::Json::parse(text, nullptr, false);
			if (!parameters.is_object())
				throw std::invalid_argument(
					"the parameters of the menu item for " + command + " aren't a JSON object: " + text);
			// First, so that it floats right of the item's text.
			Rml::Element* shown = element->InsertBefore(document.CreateElement("span"), element->GetFirstChild());
			shown->SetClass("menu-keys", true);
			m_items.push_back(
				{element->GetObserverPtr(), shown->GetObserverPtr(), std::move(command), std::move(parameters)});
		}
		document.AddEventListener(Rml::EventId::Click, this);
		update();
	}

	MainMenu::~MainMenu()
	{
		if (Rml::Element* document = m_document.get())
			document->RemoveEventListener(Rml::EventId::Click, this);
	}

	void MainMenu::update()
	{
		for (const Item& item : m_items)
		{
			Rml::Element* element = item.element.get();
			Rml::Element* shown = item.keys.get();
			if (element == nullptr || shown == nullptr)
				continue;
			const bool enabled = m_keys.canRun(item.command, m_window, closestWith(element, "focuszone"));
			if (enabled && element->HasAttribute("disabled"))
				element->RemoveAttribute("disabled");
			else if (!enabled && !element->HasAttribute("disabled"))
				element->SetAttribute("disabled", "");
			const std::vector<std::string> keys = m_keys.keysFor(item.command, item.parameters);
			const std::string text = keys.empty() ? std::string() : Rml::StringUtilities::EncodeRml(keys.front());
			if (shown->GetInnerRML() != text)
				shown->SetInnerRML(text);
		}
	}

	void MainMenu::ProcessEvent(Rml::Event& event)
	{
		Rml::Element* element = closestWith(event.GetTargetElement(), "command");
		if (element == nullptr || element->HasAttribute("disabled"))
			return;
		for (const Item& item : m_items)
			if (item.element.get() == element)
			{
				m_keys.run(item.command, item.parameters, m_window, closestWith(element, "focuszone"));
				return;
			}
	}
}
