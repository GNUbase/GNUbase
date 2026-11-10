// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/workspacesmenu.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>

#include <string>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		std::string encode(const std::string& text)
		{
			return Rml::StringUtilities::EncodeRml(text);
		}
	}

	WorkspacesMenu::WorkspacesMenu(EngineBinding& binding, Rml::Element& menu)
		: m_binding(binding), m_menu(menu.GetObserverPtr()), m_fixed(menu.GetNumChildren())
	{
		menu.AddEventListener(Rml::EventId::Click, this);
		m_subscriptions.push_back(m_binding.on("workspace.changed",
			[this](const engine::Event& event)
			{
				m_workspaces = event.data;
				show();
			}));
		m_subscriptions.push_back(m_binding.on("project.reset", [this](const engine::Event&) { request(); }));
		// The global workspaces as another client, such as another GNUbase process, changed them: the active one may
		// be gone or back.
		m_subscriptions.push_back(m_binding.on("settings.changed",
			[this](const engine::Event& event)
			{
				if (event.data.at("values").contains("ui.workspaces"))
					request();
			}));
		request();
	}

	WorkspacesMenu::~WorkspacesMenu()
	{
		if (Rml::Element* menu = m_menu.get())
			menu->RemoveEventListener(Rml::EventId::Click, this);
	}

	bool WorkspacesMenu::hasActive() const
	{
		const auto active = m_workspaces.find("active");
		return active != m_workspaces.end() && !active->value("type", std::string()).empty();
	}

	void WorkspacesMenu::request()
	{
		m_binding.send("workspace.get", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok())
					return;
				m_workspaces = result.value;
				show();
			});
	}

	void WorkspacesMenu::show()
	{
		Rml::Element* menu = m_menu.get();
		if (menu == nullptr || !m_workspaces.contains("active"))
			return;
		const Json& active = m_workspaces.at("active");
		const auto item = [&active](const std::string& type, const std::string& name, const std::string& text)
		{
			const bool checked = active.value("type", std::string()) == type &&
				(type.empty() || active.value("name", std::string()) == name);
			return R"(<menuitem type="toggle" workspace-type=")" + type + R"(" workspace=")" + encode(name) + '"' +
				(checked ? R"( checked="")" : "") + ">" + text + "</menuitem>";
		};
		std::string rml = "<menuseparator/>" + item("", "", "No Workspace");
		for (const char* type : {"global", "project"})
			for (const Json& workspace : m_workspaces.value(type, Json::array()))
			{
				const auto name = workspace.value("name", std::string());
				rml += item(type, name,
					std::string(R"(<span class="workspace-type">)") + (type[0] == 'g' ? "G" : "P") + "</span> " +
						encode(name));
			}
		while (menu->GetNumChildren() > m_fixed)
			menu->RemoveChild(menu->GetLastChild());
		// Parsed into a holder, then moved over, so that the menu's own items stay as they are.
		const Rml::ElementPtr holder = menu->GetOwnerDocument()->CreateElement("div");
		holder->SetInnerRML(rml);
		while (holder->HasChildNodes())
			menu->AppendChild(holder->RemoveChild(holder->GetFirstChild()));
	}

	void WorkspacesMenu::ProcessEvent(Rml::Event& event)
	{
		for (const Rml::Element* element = event.GetTargetElement(); element != nullptr && element != m_menu.get();
			element = element->GetParentNode())
			if (element->HasAttribute("workspace-type"))
			{
				m_binding.send("workspace.activate",
					{{"type", element->GetAttribute<Rml::String>("workspace-type", "")},
						{"name", element->GetAttribute<Rml::String>("workspace", "")}});
				return;
			}
	}
}
