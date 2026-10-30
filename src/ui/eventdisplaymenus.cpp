// SPDX-License-Identifier: GPL-3.0-or-later
// The event display's menus, buttons and dialogs in its document (ui/eventdisplay.h): the ruler's time format menu,
// the horizontal zoom presets menu with its dialogs, and the auto-scroll button with its settings menu.
#include "ui/eventdisplay.h"

#include "platform/log.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>

#include <filesystem>
#include <map>
#include <string>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		// The checked attribute gives a widget the state class .checked.
		void check(Rml::Element& element, bool on)
		{
			if (on)
				element.SetAttribute("checked", "");
			else
				element.RemoveAttribute("checked");
		}

		// The element or its nearest ancestor with the tag, or null.
		Rml::Element* closest(Rml::Element* element, const std::string& tag)
		{
			while (element != nullptr && element->GetTagName() != tag)
				element = element->GetParentNode();
			return element;
		}

		// The result that a click on the element closes a dialog with: its own, or its nearest ancestor's.
		std::string resultOf(Rml::Element* element)
		{
			while (element != nullptr && !element->HasAttribute("result"))
				element = element->GetParentNode();
			return element != nullptr && !element->HasAttribute("disabled")
				? element->GetAttribute<Rml::String>("result", "")
				: std::string();
		}

		std::string valueOf(Rml::Element* field)
		{
			auto* control = dynamic_cast<Rml::ElementFormControl*>(field);
			return control != nullptr ? control->GetValue() : std::string();
		}

		// An element for a menu, with the text and the attribute that says what it stands for.
		Rml::ElementPtr menuElement(Rml::Element& menu, const char* tag, const char* attribute,
			const std::string& value, const std::string& text)
		{
			Rml::ElementPtr element = menu.GetOwnerDocument()->CreateElement(tag);
			element->SetAttribute(attribute, value);
			element->SetInnerRML(Rml::StringUtilities::EncodeRml(text));
			return element;
		}
	}

	void EventDisplay::findControls()
	{
		m_formatMenu = m_document->GetElementById("ruler-format-menu");
		m_presetsMenu = m_document->GetElementById("zoom-presets-menu");
		m_autoScrollButton = m_document->GetElementById("auto-scroll");
		m_autoScrollMenu = m_document->GetElementById("auto-scroll-menu");
		if (m_formatMenu != nullptr)
			m_ruler->SetAttribute("contextmenu", m_formatMenu->GetId()); // a right click in the ruler opens it
	}

	// The cycle markers of the active marker track, for the zoom presets menu.
	void EventDisplay::requestMarkers()
	{
		if (m_presetsMenu == nullptr)
			return;
		m_binding.send("marker.list", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok())
					return;
				m_cycleMarkers.clear();
				for (const Json& marker : result.value.at("markers"))
				{
					if (marker.at("type") != "cycle")
						continue;
					std::string name = marker.at("name").get<std::string>();
					if (name.empty())
						name = "Cycle Marker " + std::to_string(marker.at("number").get<int64_t>());
					m_cycleMarkers.push_back({std::move(name), marker.at("positionSamples").get<int64_t>(),
						marker.at("endSamples").get<int64_t>()});
				}
				showPresets();
			});
	}

	void EventDisplay::click(Rml::Element* target)
	{
		const Rml::Element* item = closest(target, "menuitem");
		if (item == nullptr || item->HasAttribute("disabled"))
			return;
		const Rml::Element* menu = item->GetParentNode();
		const std::string& id = item->GetId();
		if (menu == m_formatMenu)
		{
			if (const auto format = timeFormatFromName(item->GetAttribute<Rml::String>("format", "")))
				setRulerFormat(*format);
		}
		else if (menu == m_presetsMenu)
		{
			const int marker = item->GetAttribute<int>("cycle-marker", -1);
			if (item->HasAttribute("preset"))
				applyZoomPreset(item->GetAttribute<Rml::String>("preset", ""));
			else if (marker >= 0 && marker < static_cast<int>(m_cycleMarkers.size()))
				zoomTo(static_cast<double>(m_cycleMarkers[static_cast<size_t>(marker)].start) / m_time.sampleRate,
					static_cast<double>(m_cycleMarkers[static_cast<size_t>(marker)].end) / m_time.sampleRate);
			else if (id == "zoom-full")
				zoomFull();
			else if (id == "zoom-preset-add")
				openDialog("zoom-preset.rml");
			else if (id == "zoom-presets-organize")
				openDialog("zoom-presets.rml");
		}
		else if (menu == m_autoScrollMenu)
		{
			if (id == "page-scroll")
				setAutoScrollMode(AutoScrollMode::Page);
			else if (id == "stationary-cursor")
				setAutoScrollMode(AutoScrollMode::Stationary);
			else if (id == "suspend-auto-scroll")
				setSuspendAutoScrollWhenEditing(!m_suspendWhenEditing);
		}
	}

	// Opens a dialog from the folder of the display's document, modal.
	void EventDisplay::openDialog(const char* file)
	{
		if (m_dialog)
			return;
		const std::string path = std::filesystem::u8path(m_root->GetOwnerDocument()->GetSourceURL())
									 .replace_filename(file)
									 .generic_u8string();
		Rml::ElementDocument* dialog = m_root->GetContext()->LoadDocument(path);
		if (dialog == nullptr)
		{
			logging::warning("The event display can't open the dialog {}", path);
			return;
		}
		if (Rml::Element* list = dialog->GetElementById("zoom-presets-list"))
		{
			// The organize dialog: a row per preset, with its name to edit and a button that deletes it.
			m_organized = m_presets;
			std::string rows;
			for (size_t i = 0; i < m_organized.size(); ++i)
				rows += R"(<div class="preset" index=")" + std::to_string(i) + R"("><input type="text" value=")" +
					Rml::StringUtilities::EncodeRml(m_organized[i].at("name").get<std::string>()) +
					R"("/><button>Delete</button></div>)";
			list->SetInnerRML(rows);
		}
		dialog->AddEventListener(Rml::EventId::Click, this);
		dialog->Show(Rml::ModalFlag::Modal);
		m_dialog = dialog->GetObserverPtr();
	}

	// A click in a dialog, before a button with a result closes it: OK, also when Enter clicks it, takes what the
	// dialog says.
	void EventDisplay::dialogEvent(Rml::Event& event)
	{
		Rml::Element* dialog = event.GetCurrentElement();
		Rml::Element* target = event.GetTargetElement();
		if (resultOf(target) != "ok")
		{
			// A row's Delete button marks the row deleted, for the dialog's style sheet to hide it.
			if (const Rml::Element* button = closest(target, "button"))
				if (Rml::Element* row = button->GetParentNode(); row != nullptr && row->IsClassSet("preset"))
					row->SetClass("deleted", true);
			return;
		}
		if (Rml::Element* name = dialog->GetElementById("zoom-preset-name"))
		{
			if (const std::string text = valueOf(name); !text.empty())
				addZoomPreset(text);
		}
		else if (Rml::Element* list = dialog->GetElementById("zoom-presets-list"))
		{
			// The presets that weren't deleted, renamed as typed, from the presets the dialog listed.
			Json presets = Json::array();
			Rml::ElementList rows;
			list->QuerySelectorAll(rows, ".preset");
			for (Rml::Element* row : rows)
			{
				const int index = row->GetAttribute<int>("index", -1);
				if (row->IsClassSet("deleted") || index < 0 || index >= static_cast<int>(m_organized.size()))
					continue;
				Json preset = m_organized[static_cast<size_t>(index)];
				if (const std::string text = valueOf(row->QuerySelector("input")); !text.empty())
					preset["name"] = text;
				presets.push_back(std::move(preset));
			}
			m_presets = std::move(presets);
			savePresets();
		}
	}

	void EventDisplay::showFormat()
	{
		if (!alive() || m_formatMenu == nullptr)
			return;
		for (int i = 0; i < m_formatMenu->GetNumChildren(); ++i)
			if (Rml::Element* item = m_formatMenu->GetChild(i); item->HasAttribute("format"))
				check(*item, item->GetAttribute<Rml::String>("format", "") == timeFormatName(m_format));
	}

	// The presets before Zoom Full, the cycle markers after it.
	void EventDisplay::showPresets()
	{
		if (!alive() || m_presetsMenu == nullptr)
			return;
		for (int i = m_presetsMenu->GetNumChildren() - 1; i >= 0; --i)
			if (Rml::Element* child = m_presetsMenu->GetChild(i);
				child->HasAttribute("preset") || child->HasAttribute("cycle-marker"))
				m_presetsMenu->RemoveChild(child);
		Rml::Element* full = m_presetsMenu->QuerySelector("#zoom-full");
		if (full == nullptr)
			return;
		for (const Json& preset : m_presets)
		{
			const auto name = preset.at("name").get<std::string>();
			m_presetsMenu->InsertBefore(menuElement(*m_presetsMenu, "menuitem", "preset", name, name), full);
		}
		Rml::Element* next = full->GetNextSibling();
		if (!m_cycleMarkers.empty())
			m_presetsMenu->InsertBefore(menuElement(*m_presetsMenu, "menuseparator", "cycle-marker", "", ""), next);
		for (size_t i = 0; i < m_cycleMarkers.size(); ++i)
			m_presetsMenu->InsertBefore(
				menuElement(*m_presetsMenu, "menuitem", "cycle-marker", std::to_string(i), m_cycleMarkers[i].name),
				next);
	}

	void EventDisplay::showAutoScroll()
	{
		if (!alive())
			return;
		if (m_autoScrollButton != nullptr)
		{
			check(*m_autoScrollButton, m_autoScroll);
			m_autoScrollButton->SetClass("suspended", m_suspended);
		}
		if (m_autoScrollMenu == nullptr)
			return;
		const std::map<std::string, bool> items = {{"page-scroll", m_autoScrollMode == AutoScrollMode::Page},
			{"stationary-cursor", m_autoScrollMode == AutoScrollMode::Stationary},
			{"suspend-auto-scroll", m_suspendWhenEditing}};
		for (const auto& [id, on] : items)
			if (Rml::Element* item = m_autoScrollMenu->QuerySelector("#" + id))
				check(*item, on);
	}
}
