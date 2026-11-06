// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/trackvisibility.h"

#include "platform/log.h"
#include "ui/widgets.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <utility>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		std::string encode(const std::string& text)
		{
			return Rml::StringUtilities::EncodeRml(text);
		}

		// The element or its closest parent with the attribute, up to stop.
		Rml::Element* closestWith(Rml::Element* element, const char* name, const Rml::Element* stop)
		{
			for (; element != nullptr && element != stop; element = element->GetParentNode())
				if (element->HasAttribute(name))
					return element;
			return nullptr;
		}

		std::string attribute(const Rml::Element& element, const char* name)
		{
			return element.GetAttribute<Rml::String>(name, "");
		}

		void setInner(Rml::Element* element, const std::string& rml)
		{
			if (element != nullptr && element->GetInnerRML() != rml)
				element->SetInnerRML(rml);
		}

		void removeListener(const Rml::ObserverPtr<Rml::Element>& element, Rml::EventListener* listener)
		{
			if (Rml::Element* found = element.get())
				found->RemoveEventListener(Rml::EventId::Click, listener);
		}
	}

	TrackVisibility::TrackVisibility(EngineBinding& binding, Rml::ElementDocument& document)
		: m_binding(binding), m_document(document.GetObserverPtr())
	{
		if (Rml::Element* button = document.GetElementById("track-visibility-configurations"))
			m_button = button->GetObserverPtr();
		if (Rml::Element* menu = document.GetElementById("track-visibility-configurations-menu"))
		{
			m_menu = menu->GetObserverPtr();
			menu->AddEventListener(Rml::EventId::Click, this);
		}
		for (const char* event : {"history.changed", "project.reset"})
			m_subscriptions.push_back(m_binding.on(event,
				[this, reset = std::string(event) == "project.reset"](const engine::Event&)
				{
					requestProject();
					if (reset)
						requestVisibility();
				}));
		m_subscriptions.push_back(m_binding.on("trackVisibility.changed",
			[this](const engine::Event& event)
			{
				m_visibility = event.data;
				show();
			}));
		requestProject();
		requestVisibility();
	}

	TrackVisibility::~TrackVisibility()
	{
		removeListener(m_menu, this);
		removeListener(m_panel, this);
		removeSyncMenu();
		if (auto* dialog = dynamic_cast<Rml::ElementDocument*>(m_dialog.get()))
		{
			dialog->RemoveEventListener(g_dialogCloseEvent, this);
			closeDialog(*dialog, "cancel");
		}
	}

	Rml::Element& TrackVisibility::showPanel(Rml::Element& element)
	{
		removeListener(m_panel, this);
		removeSyncMenu();
		m_panel = element.GetObserverPtr();
		element.SetInnerRML(
			R"(<div class="visibility-page track-visibility" page="track"><div class="toolbar track-visibility-toolbar">)"
			R"(<button id="track-visibility-sync" menu="track-visibility-sync-menu" )"
			R"(title="Sync Visibility of Project and Mixer: On/Off">=</button></div>)"
			R"(<div class="track-visibility-list"/></div>)"
			R"(<div class="visibility-page hidden" page="editor"/>)"
			R"(<div class="visibility-tabs"><button class="visibility-tab checked" page="track">Track</button>)"
			R"(<button class="visibility-tab" page="editor">Editor</button></div>)");
		element.AddEventListener(Rml::EventId::Click, this);
		// The menu in the document's body, where it isn't clipped by the zone.
		Rml::ElementDocument* document = element.GetOwnerDocument();
		const Rml::ElementPtr holder = document->CreateElement("div");
		std::string items;
		for (int number = 1; number <= 4; ++number)
			items += R"(<menuitem type="toggle" sync="window)" + std::to_string(number) +
				R"(">Sync Project and Mixer )" + std::to_string(number) + "</menuitem>";
		holder->SetInnerRML(R"(<menu id="track-visibility-sync-menu">)" + items + "</menu>");
		Rml::Element* menu = document->AppendChild(holder->RemoveChild(holder->GetFirstChild()));
		menu->AddEventListener(Rml::EventId::Click, this);
		m_syncMenu = menu->GetObserverPtr();
		show();
		return *element.QuerySelector(".visibility-page[page=editor]");
	}

	void TrackVisibility::removeSyncMenu()
	{
		if (Rml::Element* menu = m_syncMenu.get())
		{
			menu->RemoveEventListener(Rml::EventId::Click, this);
			if (Rml::Element* parent = menu->GetParentNode())
				parent->RemoveChild(menu);
		}
	}

	// ponytail: reads the whole project after every undo step, as the mixer does, for the tracks' names.
	void TrackVisibility::requestProject()
	{
		m_binding.send("project.get", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok())
					return;
				m_tracks = result.value.at("tracks");
				show();
			});
	}

	void TrackVisibility::requestVisibility()
	{
		m_binding.send("trackVisibility.get", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok())
					return;
				m_visibility = result.value;
				show();
			});
	}

	void TrackVisibility::show()
	{
		if (!m_visibility.is_object())
			return;
		m_hidden = m_visibility.at("hidden").get<std::set<uint64_t>>();
		const auto active = m_visibility.at("configuration").get<std::string>();
		if (Rml::Element* button = m_button.get())
			setInner(button,
				active.empty() ? std::string("Configurations")
							   : encode(active) + (m_visibility.at("modified").get<bool>() ? " *" : ""));
		std::string rml = R"(<menuitem visibility-configuration="add">Add Configuration...</menuitem>)";
		const std::string disabled = active.empty() ? R"( disabled="")" : "";
		rml += R"(<menuitem visibility-configuration="update")" + disabled + ">Update Configuration</menuitem>";
		rml += R"(<menuitem visibility-configuration="rename")" + disabled + ">Rename Configuration...</menuitem>";
		rml += R"(<menuitem visibility-configuration="delete")" + disabled + ">Delete Configuration</menuitem>";
		const Json& configurations = m_visibility.at("configurations");
		if (!configurations.empty())
			rml += "<menuseparator/>";
		for (const Json& configuration : configurations)
		{
			const auto name = configuration.at("name").get<std::string>();
			rml += R"(<menuitem type="toggle" configuration=")" + encode(name) + '"' +
				(name == active ? R"( checked="")" : "") + ">" + encode(name) + "</menuitem>";
		}
		setInner(m_menu.get(), rml);
		showPanelList();
	}

	void TrackVisibility::showPanelList()
	{
		Rml::Element* panel = m_panel.get();
		if (panel == nullptr || !m_visibility.is_object())
			return;
		std::map<uint64_t, int> levels;
		std::string rml;
		for (const Json& track : m_tracks)
		{
			const auto id = track.at("id").get<uint64_t>();
			const auto parent = levels.find(track.at("parent").get<uint64_t>());
			const int level = parent != levels.end() ? parent->second + 1 : 0;
			levels[id] = level;
			rml += R"(<button class="track-visibility-item)" + std::string(m_hidden.count(id) == 0 ? " checked" : "") +
				R"(" track=")" + std::to_string(id) + R"(" level=")" + std::to_string(level) + R"(">)" +
				encode(track.at("name").get<std::string>()) + "</button>";
		}
		setInner(panel->QuerySelector(".track-visibility-list"), rml);
		const std::string sync = m_visibility.at("sync").get<std::string>();
		if (Rml::Element* button = panel->QuerySelector("#track-visibility-sync"))
			button->SetClass("checked", !sync.empty());
		if (Rml::Element* menu = m_syncMenu.get())
			for (int i = 0; i < menu->GetNumChildren(); ++i)
			{
				Rml::Element* item = menu->GetChild(i);
				if (attribute(*item, "sync") == sync)
					item->SetAttribute("checked", "");
				else
					item->RemoveAttribute("checked");
			}
	}

	std::vector<uint64_t> TrackVisibility::inside(uint64_t track) const
	{
		std::vector<uint64_t> found = {track};
		for (const Json& item : m_tracks) // folders come before the tracks in them
			if (std::find(found.begin(), found.end(), item.at("parent").get<uint64_t>()) != found.end())
				found.push_back(item.at("id").get<uint64_t>());
		return found;
	}

	// A click hides or shows the track with the tracks inside it; shown, the folders around it show too, so that the
	// track list shows it. Shift shows only it (reference manual p. 82).
	void TrackVisibility::click(uint64_t track, bool exclusive)
	{
		std::map<uint64_t, uint64_t> parents;
		for (const Json& item : m_tracks)
			parents[item.at("id").get<uint64_t>()] = item.at("parent").get<uint64_t>();
		std::vector<uint64_t> shown = inside(track);
		for (uint64_t up = parents[track]; up != 0; up = parents[up])
			shown.push_back(up);
		std::set<uint64_t> hidden = m_hidden;
		if (exclusive)
		{
			hidden.clear();
			for (const auto& [id, parent] : parents)
				hidden.insert(id);
		}
		if (exclusive || m_hidden.count(track) != 0)
			for (const uint64_t id : shown)
				hidden.erase(id);
		else
			for (const uint64_t id : inside(track))
				hidden.insert(id);
		m_binding.send("trackVisibility.set", {{"hidden", hidden}}); // trackVisibility.changed shows it
	}

	void TrackVisibility::choose(const std::string& item)
	{
		if (item == "add" || item == "rename")
		{
			openDialog(item == "rename");
			return;
		}
		const auto active = m_visibility.at("configuration").get<std::string>();
		Json configurations = Json::array();
		for (Json configuration : m_visibility.at("configurations"))
		{
			if (configuration.at("name") == active)
			{
				if (item == "delete")
					continue;
				configuration["hidden"] = m_hidden; // update
			}
			configurations.push_back(configuration);
		}
		m_binding.send("trackVisibility.set", {{"configurations", configurations}});
	}

	void TrackVisibility::openDialog(bool rename)
	{
		const Rml::Element* document = m_document.get();
		if (m_dialog || document == nullptr || !m_visibility.is_object())
			return;
		const std::string path = std::filesystem::u8path(document->GetOwnerDocument()->GetSourceURL())
									 .replace_filename("visibility-configuration.rml")
									 .generic_u8string();
		Rml::ElementDocument* dialog = document->GetContext()->LoadDocument(path);
		if (dialog == nullptr)
		{
			logging::warning("The track visibility can't open the dialog {}", path);
			return;
		}
		m_renaming = rename;
		if (rename)
		{
			setInner(dialog->GetElementById("visibility-configuration-title"), "Rename Configuration");
			if (auto* field =
					dynamic_cast<Rml::ElementFormControl*>(dialog->GetElementById("visibility-configuration-name")))
				field->SetValue(m_visibility.at("configuration").get<std::string>());
		}
		m_dialog = dialog->GetObserverPtr();
		dialog->AddEventListener(g_dialogCloseEvent, this);
		dialog->Show(Rml::ModalFlag::Modal);
	}

	// Add Configuration keeps the tracks hidden now under the name typed, Rename Configuration renames the active one.
	void TrackVisibility::closedDialog(Rml::Event& event)
	{
		Rml::Element* dialog = m_dialog.get();
		dialog->RemoveEventListener(g_dialogCloseEvent, this);
		m_dialog.reset();
		auto* field = dynamic_cast<Rml::ElementFormControl*>(dialog->GetElementById("visibility-configuration-name"));
		const std::string name = field != nullptr ? field->GetValue() : std::string();
		if (event.GetParameter<Rml::String>("result", "") != "ok" || name.empty())
			return;
		Json configurations = m_visibility.at("configurations");
		if (m_renaming)
		{
			for (Json& configuration : configurations)
				if (configuration.at("name") == m_visibility.at("configuration"))
					configuration["name"] = name;
		}
		else
		{
			configurations.push_back({{"name", name}, {"hidden", m_hidden}});
		}
		m_binding.send("trackVisibility.set", {{"configurations", configurations}, {"configuration", name}});
	}

	void TrackVisibility::ProcessEvent(Rml::Event& event)
	{
		Rml::Element* current = event.GetCurrentElement();
		if (m_dialog && current == m_dialog.get())
		{
			if (event.GetType() == g_dialogCloseEvent)
				closedDialog(event);
			return;
		}
		Rml::Element* target = event.GetTargetElement();
		if (event.GetId() != Rml::EventId::Click || target == nullptr || !m_visibility.is_object())
			return;
		if (current == m_menu.get())
		{
			if (const Rml::Element* item = closestWith(target, "visibility-configuration", current);
				item != nullptr && !item->HasAttribute("disabled"))
				choose(attribute(*item, "visibility-configuration"));
			else if (const Rml::Element* chosen = closestWith(target, "configuration", current))
				m_binding.send("trackVisibility.set", {{"configuration", attribute(*chosen, "configuration")}});
		}
		else if (current == m_syncMenu.get())
		{
			// One mixer window at a time; its item again switches it off (reference manual p. 82).
			if (const Rml::Element* item = closestWith(target, "sync", current))
			{
				const std::string sync = attribute(*item, "sync");
				m_binding.send(
					"trackVisibility.set", {{"sync", m_visibility.at("sync") == sync ? std::string() : sync}});
			}
		}
		else if (current == m_panel.get())
		{
			if (const Rml::Element* item = closestWith(target, "track", current);
				item != nullptr && item->IsClassSet("track-visibility-item"))
			{
				click(std::stoull(attribute(*item, "track")), event.GetParameter("shift_key", 0) != 0);
			}
			else if (const Rml::Element* tab = closestWith(target, "page", current);
				tab != nullptr && tab->IsClassSet("visibility-tab"))
			{
				// The tabs at the bottom switch between the pages (p. 80).
				const std::string page = attribute(*tab, "page");
				Rml::ElementList elements;
				current->QuerySelectorAll(elements, "[page]");
				for (Rml::Element* element : elements)
				{
					const bool chosen = attribute(*element, "page") == page;
					if (element->IsClassSet("visibility-tab"))
						element->SetClass("checked", chosen);
					else if (element->IsClassSet("visibility-page"))
						element->SetClass("hidden", !chosen);
				}
			}
		}
	}
}
