// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/docking.h"

#include "platform/log.h"
#include "ui/keycommands.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementTabSet.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Property.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		// The events a docking window's document gets, all but focus as they bubble up.
		constexpr std::array<Rml::EventId, 7> g_events = {Rml::EventId::Change, Rml::EventId::Click,
			Rml::EventId::Dblclick, Rml::EventId::Dragend, Rml::EventId::Dragdrop, Rml::EventId::Mousedown,
			Rml::EventId::Tabchange};

		// A panel in a window of its own opens this big, centred on the primary monitor, until the user moves it.
		constexpr Size g_panelWindowSize{800, 500};

		// The nearest element from element up that has the attribute, or null.
		Rml::Element* closestWith(Rml::Element* element, const char* attribute)
		{
			while (element != nullptr && !element->HasAttribute(attribute))
				element = element->GetParentNode();
			return element;
		}

		std::string attribute(const Rml::Element& element, const char* name)
		{
			return element.GetAttribute<Rml::String>(name, "");
		}

		// The value at the path in json, or null if it isn't there.
		const Json* find(const Json& json, std::initializer_list<std::string> path)
		{
			const Json* at = &json;
			for (const std::string& key : path)
			{
				if (!at->is_object())
					return nullptr;
				const auto it = at->find(key);
				if (it == at->end())
					return nullptr;
				at = &*it;
			}
			return at;
		}

		bool contains(const std::vector<std::string>& names, const std::string& name)
		{
			return std::find(names.begin(), names.end(), name) != names.end();
		}

		std::vector<std::string> strings(const Json* list)
		{
			std::vector<std::string> result;
			if (list != nullptr && list->is_array())
				for (const Json& item : *list)
					if (item.is_string())
						result.push_back(item.get<std::string>());
			return result;
		}

		// Puts the container's children whose attribute the order names into that order, where the last of them is.
		void arrange(Rml::Element& container, const char* name, const std::vector<std::string>& order)
		{
			std::vector<Rml::Element*> children; // in the document's order
			for (int i = 0; i < container.GetNumChildren(); ++i)
				if (contains(order, attribute(*container.GetChild(i), name)))
					children.push_back(container.GetChild(i));
			std::vector<Rml::Element*> sorted;
			for (const std::string& id : order)
				std::copy_if(children.begin(), children.end(), std::back_inserter(sorted),
					[&](const Rml::Element* child) { return attribute(*child, name) == id; });
			if (sorted == children)
				return;
			Rml::Element* end = children.back()->GetNextSibling();
			for (Rml::Element* child : sorted)
			{
				Rml::ElementPtr moved = container.RemoveChild(child);
				if (end != nullptr)
					container.InsertBefore(std::move(moved), end);
				else
					container.AppendChild(std::move(moved));
			}
		}

		Json boundsJson(const Rect& bounds)
		{
			return {{"x", bounds.x}, {"y", bounds.y}, {"width", bounds.width}, {"height", bounds.height}};
		}
	}

	Docking::Docking(engine::Engine& engine, EngineBinding& binding, Shell& shell, std::filesystem::path panelWindow,
		bool hiddenWindows)
		: m_binding(binding), m_shell(shell), m_panelWindow(std::move(panelWindow)), m_hiddenWindows(hiddenWindows)
	{
		const engine::Result layout = engine::Client(engine).call("settings.get", {{"key", "ui.layout"}});
		if (layout.ok() && layout.value.at("value").is_object())
			m_layout = layout.value.at("value");

		// Another client's change; the UI's own come back the same, or while it waits for its own, which win.
		m_subscriptions.push_back(m_binding.on("settings.changed",
			[this](const engine::Event& event)
			{
				const Json& values = event.data.at("values");
				const auto layout = values.find("ui.layout");
				if (layout == values.end() || !layout->is_object() || m_saving > 0 || *layout == m_layout)
					return;
				m_layout = *layout;
				apply();
			}));
		m_shell.onCloseRequest(
			[this](Window& window)
			{
				for (auto& [id, shown] : m_shown)
					if (shown.window == window.id())
					{
						setFloating(id, false);
						return;
					}
			});
		// Windows the user moved or resized: kept for when the panel opens in a window again.
		m_shell.every(std::chrono::milliseconds(500),
			[this, alive = std::weak_ptr<bool>(m_alive)]
			{
				if (!alive.expired())
					saveBounds();
			});
	}

	Docking::~Docking()
	{
		m_shell.onCloseRequest({});
		for (auto& [name, attached] : m_windows)
			if (Rml::Element* document = attached.document.get())
				listen(*document, false);
		for (auto& [id, shown] : m_shown)
		{
			shown.content.reset();
			if (Rml::Element* document = shown.document.get())
				listen(*document, false);
		}
	}

	void Docking::listen(Rml::Element& document, bool on)
	{
		for (const Rml::EventId id : g_events)
			if (on)
				document.AddEventListener(id, this);
			else
				document.RemoveEventListener(id, this);
		if (on)
			document.AddEventListener(Rml::EventId::Focus, this, true);
		else
			document.RemoveEventListener(Rml::EventId::Focus, this, true);
	}

	void Docking::add(Panel panel)
	{
		if (this->panel(panel.id) != nullptr)
			throw std::invalid_argument("there is a panel " + panel.id + " already");
		m_panels.push_back(std::move(panel));
		for (auto& [name, attached] : m_windows)
			makeMenus(name, attached);
		apply();
	}

	void Docking::attach(const std::string& name, Rml::ElementDocument& document)
	{
		Attached& attached = m_windows[name];
		if (Rml::Element* old = attached.document.get())
			listen(*old, false);
		attached = Attached{document.GetObserverPtr(), {}, {}};
		// The first container of a set says which sections it has, before the user's order changes it.
		for (const Rml::Element* container : elements(document, "[sections]"))
		{
			std::vector<Section>& known = attached.sections[attribute(*container, "sections")];
			if (!known.empty())
				continue;
			for (int i = 0; i < container->GetNumChildren(); ++i)
				if (const Rml::Element* child = container->GetChild(i); child->HasAttribute("section"))
					known.push_back({attribute(*child, "section"), attribute(*child, "label"),
						attribute(*child, "default") == "hidden"});
		}
		for (Rml::Element* pane : elements(document, "[setup]"))
			for (const Section& known : attached.sections[attribute(*pane, "setup")])
				if (pane->QuerySelector("[setup-section=" + known.id + "]") == nullptr)
				{
					Rml::Element* item = pane->AppendChild(document.CreateElement("button"));
					item->SetAttribute("type", "toggle");
					item->SetAttribute("setup-section", known.id);
					item->SetInnerRML(known.label);
				}
		for (Rml::Element* zone : elements(document, "[zone]"))
		{
			const std::string zoneName = attribute(*zone, "zone");
			if (zoneName != "main" && zone->QuerySelector("tabset") == nullptr)
			{
				Rml::Element* tabset = zone->AppendChild(document.CreateElement("tabset"));
				tabset->SetAttribute("contextmenu", "tab-menu-" + zoneName);
			}
			if (zone->QuerySelector(".zone-frame") == nullptr)
				zone->AppendChild(document.CreateElement("div"))->SetClass("zone-frame", true);
		}
		makeMenus(name, attached);
		listen(document, true);
		document.UpdateDocument(); // for the flex directions that tell a zone's size from its height
		applyWindow(name, attached);
	}

	std::vector<Rml::Element*> Docking::elements(Rml::Element& root, const std::string& selector)
	{
		Rml::ElementList found;
		root.QuerySelectorAll(found, selector);
		return {found.begin(), found.end()};
	}

	// The zones' tab menus: Open in Separate Window, then an item to show or hide each tab. Made once per panel, since
	// a menu item mustn't go while its click is handled.
	void Docking::makeMenus(const std::string& name, Attached& attached)
	{
		auto* document = dynamic_cast<Rml::ElementDocument*>(attached.document.get());
		if (document == nullptr)
			return;
		for (const Rml::Element* zone : elements(*document, "[zone]"))
		{
			const std::string zoneName = attribute(*zone, "zone");
			if (zoneName == "main")
				continue;
			const std::string id = "tab-menu-" + zoneName;
			Rml::Element* menu = document->GetElementById(id);
			if (menu == nullptr)
			{
				menu = document->AppendChild(document->CreateElement("menu"));
				menu->SetId(id);
				Rml::Element* open = menu->AppendChild(document->CreateElement("menuitem"));
				open->SetAttribute("action", "float");
				open->SetAttribute("tabs", zoneName);
				open->SetInnerRML("Open in Separate Window");
				menu->AppendChild(document->CreateElement("menuseparator"));
			}
			for (const Panel& panel : m_panels)
				if (panel.window == name && panel.zone == zoneName &&
					menu->QuerySelector("[tab=" + panel.id + "]") == nullptr)
				{
					Rml::Element* item = menu->AppendChild(document->CreateElement("menuitem"));
					item->SetAttribute("type", "toggle");
					item->SetAttribute("tab", panel.id);
					item->SetInnerRML(panel.title);
				}
		}
	}

	void Docking::addKeyCommands(KeyCommands& keys)
	{
		struct Toggle
		{
			const char* id;
			const char* name; // the zone or strip
			const char* description;
			std::vector<const char*> keys; // the reference's (manual p. 1572)
		};
		const std::array<Toggle, 8> toggles = {{
			{"window.leftZone", "left", "Shows or hides the left zone.", {"Mod+Alt+L", "Alt+I"}},
			{"window.rightZone", "right", "Shows or hides the right zone.", {"Mod+Alt+R"}},
			{"window.lowerZone", "lower", "Shows or hides the lower zone.", {"Mod+Alt+E", "Mod+Alt+B"}},
			{"window.toolbar", "toolbar", "Shows or hides the toolbar.", {}},
			{"window.infoLine", "infoLine", "Shows or hides the info line.", {"Mod+I"}},
			{"window.overviewLine", "overviewLine", "Shows or hides the overview line.", {"Alt+O"}},
			{"window.statusLine", "statusLine", "Shows or hides the status line.", {}},
			{"window.transportBar", "transportBar", "Shows or hides the transport bar.", {"Mod+Alt+T"}},
		}};
		for (const Toggle& toggle : toggles)
		{
			keys.add({toggle.id, toggle.description,
				[this, name = std::string(toggle.name)](Window& window, Rml::Element* /*zone*/)
				{
					const std::string docked = windowIn(window);
					if (!docked.empty())
						setVisible(docked, name, !visible(docked, name));
				}});
			for (const char* binding : toggle.keys)
				keys.bind({binding, toggle.id});
		}
		const std::array<std::pair<const char*, int>, 2> steps = {{{"window.previousTab", -1}, {"window.nextTab", 1}}};
		for (const auto& [id, step] : steps)
		{
			keys.add({id,
				step < 0 ? "Shows the previous tab of the zone with the focus."
						 : "Shows the next tab of the zone with the focus.",
				[this, step = step](Window& /*window*/, Rml::Element* zone)
				{
					const Rml::Element* element = closestWith(zone, "zone");
					if (element != nullptr)
						stepTab(windowOf(*element), attribute(*element, "zone"), step);
				}});
			keys.bind({step < 0 ? "Mod+Alt+Left" : "Mod+Alt+Right", id});
		}
	}

	std::string Docking::windowIn(Window& window) const
	{
		for (const auto& [name, attached] : m_windows)
			if (const Rml::Element* document = attached.document.get();
				document != nullptr && document->GetContext() == &window.context())
				return name;
		return {};
	}

	std::string Docking::windowOf(const Rml::Element& element) const
	{
		for (const auto& [name, attached] : m_windows)
			if (attached.document.get() == element.GetOwnerDocument())
				return name;
		return {};
	}

	const Panel* Docking::panel(const std::string& id) const
	{
		const auto it = std::find_if(m_panels.begin(), m_panels.end(), [&id](const Panel& p) { return p.id == id; });
		return it == m_panels.end() ? nullptr : &*it;
	}

	Json& Docking::zone(const std::string& window, const std::string& zone)
	{
		Json& json = m_layout["windows"][window]["zones"][zone];
		if (!json.is_object())
			json = Json::object();
		return json;
	}

	Rml::Element* Docking::element(const std::string& window, const std::string& selector) const
	{
		const auto it = m_windows.find(window);
		Rml::Element* document = it == m_windows.end() ? nullptr : it->second.document.get();
		return document == nullptr ? nullptr : document->QuerySelector(selector);
	}

	bool Docking::visible(const std::string& window, const std::string& name) const
	{
		if (const Rml::Element* zone = element(window, "[zone=" + name + "]"))
		{
			const Json* value = find(m_layout, {"windows", window, "zones", name, "visible"});
			return value != nullptr && value->is_boolean() ? value->get<bool>()
														   : attribute(*zone, "default") != "hidden";
		}
		const Json* value = find(m_layout, {"windows", window, "strips", name});
		if (value != nullptr && value->is_boolean())
			return value->get<bool>();
		const Rml::Element* strip = element(window, "[strip=" + name + "]");
		return strip == nullptr || attribute(*strip, "default") != "hidden";
	}

	void Docking::setVisible(const std::string& window, const std::string& name, bool visible)
	{
		if (element(window, "[zone=" + name + "]") != nullptr)
		{
			zone(window, name)["visible"] = visible;
		}
		else
		{
			Json& strips = m_layout["windows"][window]["strips"];
			if (!strips.is_object())
				strips = Json::object();
			strips[name] = visible;
		}
		save();
		apply();
	}

	void Docking::setSize(const std::string& window, const std::string& name, double dp)
	{
		zone(window, name)["size"] = std::max(dp, 0.0);
		save();
		apply();
	}

	std::vector<std::string> Docking::tabs(const std::string& window, const std::string& name) const
	{
		std::vector<std::string> result;
		for (const std::string& id : strings(find(m_layout, {"windows", window, "zones", name, "tabs"})))
		{
			const Panel* known = panel(id);
			if (known != nullptr && known->window == window && known->zone == name && !contains(result, id))
				result.push_back(id);
		}
		for (const Panel& panel : m_panels)
			if (panel.window == window && panel.zone == name && !contains(result, panel.id))
				result.push_back(panel.id);
		return result;
	}

	std::vector<std::string> Docking::shownTabs(const std::string& window, const std::string& name) const
	{
		const std::vector<std::string> hidden = strings(find(m_layout, {"windows", window, "zones", name, "hidden"}));
		std::vector<std::string> result;
		for (const std::string& id : tabs(window, name))
		{
			const Json* floating = find(m_layout, {"panels", id, "floating"});
			if (!contains(hidden, id) && (floating == nullptr || !floating->is_boolean() || !floating->get<bool>()))
				result.push_back(id);
		}
		return result;
	}

	void Docking::showTab(const std::string& id)
	{
		if (const Panel* shown = panel(id))
		{
			zone(shown->window, shown->zone)["active"] = id;
			save();
			apply();
		}
	}

	void Docking::stepTab(const std::string& window, const std::string& name, int step)
	{
		const std::vector<std::string> shown = shownTabs(window, name);
		if (shown.empty())
			return;
		const Json* active = find(m_layout, {"windows", window, "zones", name, "active"});
		auto it = active != nullptr && active->is_string()
			? std::find(shown.begin(), shown.end(), active->get<std::string>())
			: shown.end();
		const auto count = static_cast<int>(shown.size());
		const int index = it == shown.end() ? 0 : static_cast<int>(it - shown.begin());
		zone(window, name)["active"] = shown[static_cast<size_t>((index + (step % count) + count) % count)];
		save();
		apply();
	}

	void Docking::moveTab(const std::string& id, const std::string& before)
	{
		const Panel* moved = panel(id);
		const Panel* target = panel(before);
		if (moved == nullptr || target == nullptr || id == before || moved->window != target->window ||
			moved->zone != target->zone)
			return;
		std::vector<std::string> order = tabs(moved->window, moved->zone);
		const auto to = std::find(order.begin(), order.end(), before) - order.begin();
		order.erase(std::find(order.begin(), order.end(), id));
		order.insert(order.begin() + to, id); // where before was: after it when moving right, before it when left
		zone(moved->window, moved->zone)["tabs"] = order;
		save();
		apply();
	}

	void Docking::setTabHidden(const std::string& id, bool hidden)
	{
		const Panel* tab = panel(id);
		if (tab == nullptr)
			return;
		Json& json = zone(tab->window, tab->zone);
		std::vector<std::string> list = strings(find(json, {"hidden"}));
		list.erase(std::remove(list.begin(), list.end(), id), list.end());
		if (hidden)
			list.push_back(id);
		json["hidden"] = list;
		save();
		apply();
	}

	void Docking::setFloating(const std::string& id, bool floating)
	{
		if (panel(id) == nullptr)
			return;
		Json& json = m_layout["panels"][id];
		if (!json.is_object())
			json = Json::object();
		json["floating"] = floating;
		if (const Shown& shown = m_shown[id]; shown.window != 0 && !floating)
			json["bounds"] = boundsJson(m_shell.bounds(shown.window));
		save();
		apply();
	}

	const Docking::Section* Docking::section(
		const std::string& window, const std::string& set, const std::string& id) const
	{
		const auto attached = m_windows.find(window);
		if (attached == m_windows.end())
			return nullptr;
		const auto sections = attached->second.sections.find(set);
		if (sections == attached->second.sections.end())
			return nullptr;
		const auto it = std::find_if(sections->second.begin(), sections->second.end(),
			[&id](const Section& section) { return section.id == id; });
		return it == sections->second.end() ? nullptr : &*it;
	}

	std::vector<std::string> Docking::sections(const std::string& window, const std::string& set) const
	{
		std::vector<std::string> result;
		for (const std::string& id : strings(find(m_layout, {"windows", window, "sections", set, "order"})))
			if (section(window, set, id) != nullptr && !contains(result, id))
				result.push_back(id);
		if (const auto attached = m_windows.find(window); attached != m_windows.end())
			if (const auto known = attached->second.sections.find(set); known != attached->second.sections.end())
				for (const Section& entry : known->second)
					if (!contains(result, entry.id))
						result.push_back(entry.id);
		return result;
	}

	bool Docking::sectionHidden(const std::string& window, const std::string& set, const std::string& id) const
	{
		if (const Json* hidden = find(m_layout, {"windows", window, "sections", set, "hidden"});
			hidden != nullptr && hidden->is_array())
			return contains(strings(hidden), id);
		const Section* known = section(window, set, id);
		return known != nullptr && known->hidden;
	}

	Json& Docking::sectionsJson(const std::string& window, const std::string& set)
	{
		Json& json = m_layout["windows"][window]["sections"][set];
		if (!json.is_object())
			json = Json::object();
		return json;
	}

	void Docking::moveSection(
		const std::string& window, const std::string& set, const std::string& id, const std::string& before)
	{
		std::vector<std::string> order = sections(window, set);
		const auto from = std::find(order.begin(), order.end(), id);
		const auto to = std::find(order.begin(), order.end(), before);
		if (from == order.end() || to == order.end() || id == before)
			return;
		const auto index = to - order.begin();
		order.erase(from);
		order.insert(order.begin() + index, id); // where before was, as for tabs
		sectionsJson(window, set)["order"] = order;
		save();
		apply();
	}

	void Docking::setSectionHidden(
		const std::string& window, const std::string& set, const std::string& id, bool hidden)
	{
		if (section(window, set, id) == nullptr)
			return;
		std::vector<std::string> list; // every hidden one, so that the defaults no longer count
		for (const std::string& name : sections(window, set))
			if (name == id ? hidden : sectionHidden(window, set, name))
				list.push_back(name);
		sectionsJson(window, set)["hidden"] = list;
		save();
		apply();
	}

	Window* Docking::windowOf(const std::string& panel)
	{
		const auto it = m_shown.find(panel);
		return it == m_shown.end() || it->second.window == 0 ? nullptr : m_shell.find(it->second.window);
	}

	void Docking::save()
	{
		++m_saving;
		m_binding.send("settings.set", {{"key", "ui.layout"}, {"value", m_layout}},
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired())
					return;
				--m_saving;
				if (!result.ok())
					logging::warning("The window layout couldn't be saved: {}", result.error);
			});
	}

	void Docking::saveBounds()
	{
		bool changed = false;
		for (const auto& [id, shown] : m_shown)
		{
			if (shown.window == 0 || m_shell.find(shown.window) == nullptr)
				continue;
			const Json bounds = boundsJson(m_shell.bounds(shown.window));
			Json& json = m_layout["panels"][id];
			if (json.value("bounds", Json()) != bounds)
			{
				json["bounds"] = bounds;
				changed = true;
			}
		}
		if (changed)
			save();
	}

	void Docking::apply()
	{
		applyFloating(false);
		for (auto& [name, attached] : m_windows)
			applyWindow(name, attached);
		applyFloating(true);
	}

	void Docking::applyWindow(const std::string& window, Attached& attached)
	{
		auto* document = dynamic_cast<Rml::ElementDocument*>(attached.document.get());
		if (document == nullptr)
			return;
		for (Rml::Element* zone : elements(*document, "[zone]"))
		{
			const std::string zoneName = attribute(*zone, "zone");
			if (zoneName == "main")
				continue;
			const bool shown = visible(window, zoneName);
			zone->SetClass("hidden", !shown);
			for (Rml::Element* splitter : elements(*document, "splitter[for=" + zoneName + "]"))
				splitter->SetClass("hidden", !shown);
			const Json* size = find(m_layout, {"windows", window, "zones", zoneName, "size"});
			const double dp =
				size != nullptr && size->is_number() ? size->get<double>() : zone->GetAttribute<double>("size", -1.0);
			const Rml::Element* parent = zone->GetParentNode();
			const bool column = parent != nullptr &&
				(parent->GetComputedValues().flex_direction() == Rml::Style::FlexDirection::Column ||
					parent->GetComputedValues().flex_direction() == Rml::Style::FlexDirection::ColumnReverse);
			const Rml::Property length(static_cast<float>(dp), Rml::Unit::DP);
			if (dp >= 0.0 && column)
				zone->SetProperty(Rml::PropertyId::Height, length);
			else if (dp >= 0.0)
				zone->SetProperty(Rml::PropertyId::Width, length);
			applyTabs(attached, *zone, window, zoneName);
		}
		for (Rml::Element* strip : elements(*document, "[strip]"))
			strip->SetClass("hidden", !visible(window, attribute(*strip, "strip")));
		for (const char* strip : {"toolbar", "infoLine", "overviewLine", "statusLine", "transportBar"})
			document->SetClass(std::string("no-") + strip, !visible(window, strip));
		for (Rml::Element* toggle : elements(*document, "[toggle]"))
		{
			if (visible(window, attribute(*toggle, "toggle")))
				toggle->SetAttribute("checked", "");
			else
				toggle->RemoveAttribute("checked");
		}
		for (const auto& [set, known] : attached.sections)
		{
			const std::vector<std::string> order = sections(window, set);
			for (Rml::Element* container : elements(*document, "[sections=" + set + "]"))
			{
				arrange(*container, "section", order);
				for (Rml::Element* part : elements(*container, "[section]"))
					part->SetClass("hidden", sectionHidden(window, set, attribute(*part, "section")));
			}
			for (Rml::Element* pane : elements(*document, "[setup=" + set + "]"))
			{
				arrange(*pane, "setup-section", order);
				for (Rml::Element* item : elements(*pane, "[setup-section]"))
				{
					if (sectionHidden(window, set, attribute(*item, "setup-section")))
						item->RemoveAttribute("checked");
					else
						item->SetAttribute("checked", "");
				}
			}
		}
		for (Rml::Element* item : elements(*document, "menuitem[tab]"))
		{
			const Panel* tab = panel(attribute(*item, "tab"));
			const bool shown = tab != nullptr &&
				!contains(strings(find(m_layout, {"windows", window, "zones", tab->zone, "hidden"})), tab->id);
			if (shown)
				item->SetAttribute("checked", "");
			else
				item->RemoveAttribute("checked");
		}
		document->GetContext()->RequestNextUpdate(0);
	}

	// Builds the zone's tab group again when its tabs changed, keeping what the panels that stay in it show, and
	// shows the active tab.
	void Docking::applyTabs(
		Attached& attached, Rml::Element& zone, const std::string& window, const std::string& zoneName)
	{
		auto* tabset = dynamic_cast<Rml::ElementTabSet*>(zone.QuerySelector("tabset"));
		if (tabset == nullptr)
			return;
		Rml::ElementDocument* document = zone.GetOwnerDocument();
		const std::vector<std::string> shown = shownTabs(window, zoneName);
		std::vector<std::string>& built = attached.tabs[zoneName];
		if (shown != built)
		{
			const Rml::Element* panels = tabset->QuerySelector("panels");
			for (int i = tabset->GetNumTabs() - 1; i >= 0; --i)
			{
				if (panels != nullptr && static_cast<size_t>(i) < built.size())
				{
					Shown& content = m_shown[built[static_cast<size_t>(i)]];
					Rml::Element* panel = panels->GetChild(i);
					if (content.element != nullptr && panel != nullptr && content.element->GetParentNode() == panel)
						content.parked = panel->RemoveChild(content.element);
				}
				tabset->RemoveTab(i);
			}
			for (size_t i = 0; i < shown.size(); ++i)
			{
				const Panel& info = *panel(shown[i]);
				Rml::ElementPtr tab = document->CreateElement("tab");
				tab->SetAttribute("panel", info.id);
				tab->SetInnerRML(info.title);
				tabset->SetTab(static_cast<int>(i), std::move(tab));
				Rml::ElementPtr page = document->CreateElement("panel");
				page->SetAttribute("panel", info.id);
				Rml::Element* parent = page.get();
				tabset->SetPanel(static_cast<int>(i), std::move(page));
				Shown& content = m_shown[info.id];
				if (content.parked)
				{
					parent->AppendChild(std::move(content.parked));
				}
				else
				{
					content.element = parent->AppendChild(document->CreateElement("div"));
					content.element->SetClass("panel-content", true);
					if (info.create)
						content.content = info.create(*content.element);
				}
			}
			// What no longer shows here goes: what create returned first, then its element.
			for (auto& [id, content] : m_shown)
				if (content.parked)
				{
					content.content.reset();
					content.parked.reset();
					content.element = nullptr;
				}
			built = shown;
		}
		if (shown.empty())
			return;
		const Json* active = find(m_layout, {"windows", window, "zones", zoneName, "active"});
		const auto it = active != nullptr && active->is_string()
			? std::find(shown.begin(), shown.end(), active->get<std::string>())
			: shown.end();
		tabset->SetActiveTab(it == shown.end() ? 0 : static_cast<int>(it - shown.begin()));
	}

	// Closes the windows of panels that are docked again, or opens those of panels that left their zone.
	void Docking::applyFloating(bool open)
	{
		for (const Panel& panel : m_panels)
		{
			const Json* floating = find(m_layout, {"panels", panel.id, "floating"});
			const bool alone = floating != nullptr && floating->is_boolean() && floating->get<bool>();
			Shown& shown = m_shown[panel.id];
			if (!open && !alone && shown.window != 0)
			{
				const WindowId window = shown.window;
				shown = Shown{};
				m_shell.close(window);
			}
			else if (open && alone && shown.window == 0)
			{
				WindowSpec spec;
				spec.title = panel.title;
				spec.size = g_panelWindowSize;
				spec.hidden = m_hiddenWindows;
				if (const Json* bounds = find(m_layout, {"panels", panel.id, "bounds"}); bounds != nullptr)
					spec.bounds = Rect{bounds->value("x", 0), bounds->value("y", 0),
						bounds->value("width", g_panelWindowSize.width),
						bounds->value("height", g_panelWindowSize.height)};
				try
				{
					const Window& window = m_shell.open(spec);
					Rml::ElementDocument* document = window.context().LoadDocument(m_panelWindow.generic_u8string());
					Rml::Element* content = document == nullptr ? nullptr : document->GetElementById("panel-content");
					if (content == nullptr)
					{
						m_shell.close(window.id());
						logging::error("Cannot load the panel window {}", m_panelWindow.u8string());
						continue;
					}
					document->SetTitle(panel.title);
					document->SetAttribute("panel", panel.id);
					document->Show();
					listen(*document, true);
					shown.window = window.id();
					shown.document = document->GetObserverPtr();
					shown.element = content;
					if (panel.create)
						shown.content = panel.create(*content);
				}
				catch (const std::runtime_error& error)
				{
					logging::error("Cannot open a window for the panel {}: {}", panel.id, error.what());
				}
			}
		}
	}

	void Docking::later(std::function<void()> change)
	{
		m_shell.post(
			[alive = std::weak_ptr<bool>(m_alive), change = std::move(change)]
			{
				if (!alive.expired())
					change();
			});
	}

	// From the documents' events. What changes the documents waits for the shell's next step, since elements must not
	// go while their events are handled.
	void Docking::ProcessEvent(Rml::Event& event)
	{
		Rml::Element* target = event.GetTargetElement();
		if (target == nullptr)
			return;
		std::string window = windowOf(*target);
		switch (event.GetId())
		{
		case Rml::EventId::Focus:
		{
			const Rml::Element* area = closestWith(target, "zone");
			if (Rml::ElementDocument* document = target->GetOwnerDocument())
				for (Rml::Element* other : elements(*document, "[zone]"))
					if (other != nullptr)
						other->SetClass("focused", other == area);
			break;
		}
		case Rml::EventId::Change:
			if (target->HasAttribute("toggle") && !window.empty())
				later([this, window = std::move(window), name = attribute(*target, "toggle"),
						  on = event.GetParameter("value", false)] { setVisible(window, name, on); });
			else if (const Rml::Element* pane = closestWith(target, "setup");
				pane != nullptr && target->HasAttribute("setup-section") && !window.empty())
				later([this, window = std::move(window), set = attribute(*pane, "setup"),
						  id = attribute(*target, "setup-section"), on = event.GetParameter("value", false)]
					{ setSectionHidden(window, set, id, !on); });
			else if (target->HasAttribute("tab"))
				later([this, id = attribute(*target, "tab"), on = event.GetParameter("value", false)]
					{ setTabHidden(id, !on); });
			else if (target->GetTagName() == "splitter" && target->HasAttribute("for") && !window.empty())
				zone(window, attribute(*target, "for"))["size"] = event.GetParameter("value", 0.0F); // saved on dragend
			break;
		case Rml::EventId::Dragend:
			if (target->GetTagName() == "splitter" && target->HasAttribute("for"))
				save();
			break;
		case Rml::EventId::Dblclick:
			if (const Rml::Element* splitter = closestWith(target, "for");
				splitter != nullptr && splitter->GetTagName() == "splitter" && !window.empty())
				later([this, window = std::move(window), name = attribute(*splitter, "for")]
					{ setVisible(window, name, false); });
			break;
		case Rml::EventId::Click:
			if (const Rml::Element* item = closestWith(target, "action");
				item != nullptr && attribute(*item, "action") == "float")
			{
				// The tab right-clicked, or the zone's active one.
				std::string id = m_contextTab;
				if (const Rml::Element* tab = element(window, "[zone=" + attribute(*item, "tabs") + "] tab:selected");
					id.empty() && tab != nullptr)
					id = attribute(*tab, "panel");
				later([this, id] { setFloating(id, true); });
			}
			else if (const Rml::Element* button = closestWith(target, "id");
				button != nullptr && button->GetId() == "dock")
			{
				later([this, id = attribute(*target->GetOwnerDocument(), "panel")] { setFloating(id, false); });
			}
			break;
		case Rml::EventId::Mousedown:
			if (event.GetParameter("button", 0) == 1)
			{
				const Rml::Element* tab = closestWith(target, "panel");
				m_contextTab = tab != nullptr && tab->GetTagName() == "tab" ? attribute(*tab, "panel") : std::string();
			}
			break;
		case Rml::EventId::Dragdrop:
		{
			const Rml::Element* onto = closestWith(target, "panel");
			const auto* dragged = static_cast<Rml::Element*>(event.GetParameter<void*>("drag_element", nullptr));
			if (onto != nullptr && dragged != nullptr && onto->GetTagName() == "tab" && dragged->GetTagName() == "tab")
				later([this, id = attribute(*dragged, "panel"), before = attribute(*onto, "panel")]
					{ moveTab(id, before); });
			const Rml::Element* item = closestWith(target, "setup-section");
			const Rml::Element* pane = closestWith(target, "setup");
			if (item != nullptr && pane != nullptr && dragged != nullptr && dragged->HasAttribute("setup-section") &&
				!window.empty())
				later([this, window = std::move(window), set = attribute(*pane, "setup"),
						  id = attribute(*dragged, "setup-section"), before = attribute(*item, "setup-section")]
					{ moveSection(window, set, id, before); });
			break;
		}
		case Rml::EventId::Tabchange:
		{
			const Rml::Element* area = closestWith(target, "zone");
			if (area == nullptr || window.empty())
				break;
			const std::string name = attribute(*area, "zone");
			const std::vector<std::string>& built = m_windows[window].tabs[name];
			const int index = event.GetParameter("tab_index", 0);
			if (index >= 0 && static_cast<size_t>(index) < built.size())
			{
				const Json* active = find(m_layout, {"windows", window, "zones", name, "active"});
				if (active == nullptr || *active != built[static_cast<size_t>(index)])
				{
					zone(window, name)["active"] = built[static_cast<size_t>(index)];
					save();
				}
			}
			break;
		}
		default:
			break;
		}
	}
}
