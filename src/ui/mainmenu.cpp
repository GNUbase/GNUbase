// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/mainmenu.h"

#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>

#include <optional>
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
		// Every item is checked before any changes, so that a menu refused leaves the document as it was.
		Rml::ElementList found;
		document.QuerySelectorAll(found, "menuitem[command]");
		for (Rml::Element* element : found)
		{
			auto command = element->GetAttribute<Rml::String>("command", "");
			const auto text = element->GetAttribute<Rml::String>("parameters", "{}");
			engine::Json parameters = engine::Json::parse(text, nullptr, false);
			if (!parameters.is_object())
			{
				std::string message = "the parameters of the menu item for ";
				message.append(command).append(" aren't a JSON object: ").append(text);
				throw std::invalid_argument(message);
			}
			m_keys.check(command, parameters);
			m_items.push_back({element->GetObserverPtr(), {}, std::move(command), std::move(parameters)});
		}
		for (Item& item : m_items)
		{
			// First, so that it floats right of the item's text.
			Rml::Element* element = item.element.get();
			Rml::Element* shown = element->InsertBefore(document.CreateElement("span"), element->GetFirstChild());
			shown->SetClass("menu-keys", true);
			item.keys = shown->GetObserverPtr();
		}
		// In the bubble phase, after the listeners of the elements below the document, and of the document's added
		// before the menu; the document's bubble phase click listeners added later run after it (see update()).
		document.AddEventListener(Rml::EventId::Click, this);
		update();
	}

	MainMenu::~MainMenu()
	{
		if (Rml::Element* document = m_document.get())
			document->RemoveEventListener(Rml::EventId::Click, this);
		// A menu built again on the same document adds spans of its own.
		for (const Item& item : m_items)
		{
			Rml::Element* shown = item.keys.get();
			if (shown != nullptr && shown->GetParentNode() != nullptr)
				shown->GetParentNode()->RemoveChild(shown);
		}
	}

	void MainMenu::enable(Rml::Element& element, bool enabled)
	{
		if (enabled && element.HasAttribute("disabled"))
			element.RemoveAttribute("disabled");
		else if (!enabled && !element.HasAttribute("disabled"))
			element.SetAttribute("disabled", "");
	}

	void MainMenu::update()
	{
		const uint64_t revision = m_keys.bindingsRevision();
		const bool keysChanged = m_keysRevision != revision;
		m_keysRevision = revision;
		// The items of a menu come one after the other: they share their parent's zone. Items of one command in one
		// zone get the same answer, as the enabled callbacks get the window and the zone, not the parameters.
		const Rml::Element* parent = nullptr;
		Rml::Element* parentZone = nullptr;
		const std::string* asked = nullptr;
		Rml::Element* askedZone = nullptr;
		bool enabled = false;
		for (Item& item : m_items)
		{
			Rml::Element* element = item.element.get();
			Rml::Element* shown = item.keys.get();
			if (element == nullptr || shown == nullptr)
				continue;
			Rml::Element* zone = element;
			if (!element->HasAttribute("focuszone"))
			{
				if (element->GetParentNode() != parent)
				{
					parent = element->GetParentNode();
					parentZone = closestWith(element->GetParentNode(), "focuszone");
				}
				zone = parentZone;
			}
			if (asked == nullptr || *asked != item.command || askedZone != zone)
			{
				enabled = m_keys.canRun(item.command, m_window, zone);
				asked = &item.command;
				askedZone = zone;
			}
			enable(*element, enabled);
			// Only where checkWhen says: the window menu's panel items are the docking's (ui/docking.h).
			if (const std::optional<bool> checked = m_keys.checked(item.command, item.parameters))
			{
				if (*checked && !element->HasAttribute("checked"))
					element->SetAttribute("checked", "");
				else if (!*checked && element->HasAttribute("checked"))
					element->RemoveAttribute("checked");
			}
			if (!keysChanged)
				continue;
			const std::vector<std::string> keys = m_keys.keysFor(item.command, item.parameters);
			std::string text = keys.empty() ? std::string() : Rml::StringUtilities::EncodeRml(keys.front());
			if (text != item.shownKeys)
			{
				shown->SetInnerRML(text);
				item.shownKeys = std::move(text);
			}
		}
	}

	void MainMenu::ProcessEvent(Rml::Event& event)
	{
		// The clicked item follows what the click's listeners changed before it runs (#1120). The other items, and
		// what the click's default actions and the item's own command change, follow after the input (update).
		Rml::Element* element = closestWith(event.GetTargetElement(), "command");
		if (element == nullptr)
			return;
		for (const Item& item : m_items)
			if (item.element.get() == element)
			{
				Rml::Element* zone = closestWith(element, "focuszone");
				const bool enabled = m_keys.canRun(item.command, m_window, zone);
				enable(*element, enabled);
				if (enabled)
					m_keys.run(item.command, item.parameters, m_window, zone);
				return;
			}
	}
}
