// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/docking.h"

#include "platform/log.h"
#include "ui/displays.h"
#include "ui/keycommands.h"
#include "ui/livereload.h"
#include "ui/widgets.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementTabSet.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		// The events a docking window's document gets, all but focus as they bubble up.
		constexpr std::array<Rml::EventId, 8> g_events = {Rml::EventId::Change, Rml::EventId::Click,
			Rml::EventId::Dblclick, Rml::EventId::Dragend, Rml::EventId::Dragdrop, Rml::EventId::Keydown,
			Rml::EventId::Mousedown, Rml::EventId::Tabchange};

		// The order in which Tab activates the zones (p. 100): the project zone, the Channel zone, then the left, lower
		// and right zones.
		constexpr std::array<const char*, 5> g_zoneOrder = {"main", "channel", "left", "lower", "right"};

		// A panel in a window of its own opens this big, centred on the primary monitor, until the user moves it.
		constexpr Size g_panelWindowSize{800, 500};
		constexpr double g_mostSize = 100000.0; // dp a zone or resizable element can have, whatever ui.layout says

		// The nearest element from element up that has the attribute, or null.
		Rml::Element* closestWith(Rml::Element* element, const char* attribute)
		{
			while (element != nullptr && !element->HasAttribute(attribute))
				element = element->GetParentNode();
			return element;
		}

		// Whether Tab belongs to the element, as RmlUi or the widget handles it, rather than to the zones: a text
		// field or a select box, where it moves between controls (#60), and an open menu, which keeps its keys.
		bool keepsTab(const Rml::Element& element)
		{
			const Rml::String& tag = element.GetTagName();
			if (tag == "textarea" || tag == "select")
				return true;
			if (tag == "input")
			{
				const auto type = element.GetAttribute<Rml::String>("type", "text");
				return type != "checkbox" && type != "radio" && type != "range" && type != "button" && type != "submit";
			}
			for (const Rml::Element* above = &element; above != nullptr; above = above->GetParentNode())
				if (above->GetTagName() == "menu")
					return true;
			return false;
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

		// Puts the container's children whose attribute the order names into that order, in the places they take, so
		// that the other children, such as a toolbar's spacer, stay where they are.
		void arrange(Rml::Element& container, const char* name, const std::vector<std::string>& order)
		{
			std::vector<Rml::Element*> places; // in the document's order
			for (int i = 0; i < container.GetNumChildren(); ++i)
				if (contains(order, attribute(*container.GetChild(i), name)))
					places.push_back(container.GetChild(i));
			std::vector<Rml::Element*> sorted;
			for (const std::string& id : order)
				std::copy_if(places.begin(), places.end(), std::back_inserter(sorted),
					[&](const Rml::Element* child) { return attribute(*child, name) == id; });
			for (std::size_t i = 0; i < sorted.size(); ++i)
			{
				if (places[i] == sorted[i])
					continue;
				// Swaps the child in this place with the one that belongs here, which is in a later place.
				const auto there =
					std::find(places.begin() + static_cast<std::ptrdiff_t>(i) + 1, places.end(), sorted[i]);
				Rml::Element* here = places[i];
				Rml::Element* after = (*there)->GetNextSibling();
				container.InsertBefore(container.RemoveChild(*there), here);
				if (after != nullptr)
					container.InsertBefore(container.RemoveChild(here), after);
				else
					container.AppendChild(container.RemoveChild(here));
				std::swap(places[i], *there);
			}
		}

		bool inside(const Rml::Element* element, const Rml::Element& ancestor)
		{
			while (element != nullptr && element != &ancestor)
				element = element->GetParentNode();
			return element != nullptr;
		}

		Json boundsJson(const Rect& bounds)
		{
			return {{"x", bounds.x}, {"y", bounds.y}, {"width", bounds.width}, {"height", bounds.height}};
		}

		// The bounds in a window's or a panel's entry of ui.layout, on a connected monitor, or none.
		std::optional<Rect> savedBounds(const Json* entry, Size size, const std::vector<Display>& displays)
		{
			const Json* bounds = entry != nullptr ? find(*entry, {"bounds"}) : nullptr;
			if (bounds == nullptr || !bounds->is_object())
				return std::nullopt;
			const Json* monitor = find(*entry, {"monitor"});
			const Rect rect{bounds->value("x", 0), bounds->value("y", 0), bounds->value("width", size.width),
				bounds->value("height", size.height)};
			return restore(rect,
				monitor != nullptr && monitor->is_string() ? monitor->get<std::string>() : std::string(), displays);
		}

		// Puts the bounds and their monitor's name into a window's or a panel's entry of ui.layout; whether that
		// changed it.
		bool keepBounds(Json& entry, const Rect& bounds, const std::vector<Display>& displays)
		{
			if (!entry.is_object())
				entry = Json::object();
			const Display* display = displayOf(bounds, displays);
			const Json monitor = display != nullptr ? display->name : std::string();
			const Json rect = boundsJson(bounds);
			if (entry.value("bounds", Json()) == rect && entry.value("monitor", Json()) == monitor)
				return false;
			entry["bounds"] = rect;
			entry["monitor"] = monitor;
			return true;
		}
	}

	Docking::Docking(engine::Engine& engine, EngineBinding& binding, Shell& shell, std::filesystem::path panelWindow,
		bool hiddenWindows)
		: m_binding(binding), m_shell(shell), m_panelWindow(std::move(panelWindow)), m_hiddenWindows(hiddenWindows),
		  m_reloadEvent(g_documentReloadEvent), m_closeEvent(g_dialogCloseEvent)
	{
		const engine::Result layout = engine::Client(engine).call("settings.get", {{"key", "ui.layout"}});
		if (layout.ok() && layout.value.at("value").is_object())
			m_layout = layout.value.at("value");
		const engine::Result presets = engine::Client(engine).call("settings.get", {{"key", "ui.setupPresets"}});
		if (presets.ok() && presets.value.at("value").is_object())
			m_presets = presets.value.at("value");

		// Another client's change, or a workspace's (workspace.activate); the docking system's own come back as they
		// were sent.
		m_subscriptions.push_back(m_binding.on("settings.changed",
			[this](const engine::Event& event)
			{
				const Json& values = event.data.at("values");
				if (const auto presets = values.find("ui.setupPresets");
					presets != values.end() && presets->is_object() && *presets != m_presets)
				{
					m_presets = *presets;
					apply(); // the setup panes' lists of presets
				}
				const auto layout = values.find("ui.layout");
				if (layout == values.end() || !layout->is_object() || m_saves.count(event.request) != 0)
					return;
				if (*layout != m_layout)
				{
					m_layout = *layout;
					apply();
				}
				applyWindows(); // also a workspace's layout that is the same, whose windows the user closed since
				// A layout of its own that is on its way may reach the engine after this one: this one comes last.
				if (!m_saves.empty())
					save();
			}));
		m_shell.onCloseRequest(
			[this](Window& window)
			{
				later([this] { saveWindows(); }); // once the shell closed it
				for (auto& [id, shown] : m_shown)
					if (shown.window == window.id())
					{
						setFloating(id, false);
						return;
					}
			});
		// In every step, after the step tasks and right before the shell draws: the window's size, the UI scale, a
		// label the engine binding changed or the shown sections may have changed the room.
		m_shell.beforeDraw(
			[this, alive = std::weak_ptr<bool>(m_alive)]
			{
				if (!alive.expired())
					fitSections();
			});
		// Windows the user moved, resized, opened or closed: kept for when they open again, and for workspaces.
		m_shell.every(std::chrono::milliseconds(500),
			[this, alive = std::weak_ptr<bool>(m_alive)]
			{
				if (!alive.expired())
					saveWindows();
			});
	}

	Docking::~Docking()
	{
		m_shell.onCloseRequest({});
		if (Rml::Element* dialog = m_presetDialog.get())
			dialog->RemoveEventListener(m_closeEvent, this);
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
		{
			document.AddEventListener(Rml::EventId::Focus, this, true);
			document.AddEventListener(m_reloadEvent, this);
		}
		else
		{
			document.RemoveEventListener(Rml::EventId::Focus, this, true);
			document.RemoveEventListener(m_reloadEvent, this);
		}
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
		attached = Attached{document.GetObserverPtr(), {}, {}, {}};
		for (Rml::Element* container : elements(document, "[fit=room]"))
			attached.fits.push_back(container->GetObserverPtr());
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

	std::vector<Rml::Element*> Docking::elementsWith(Rml::Element& root, const char* name, const std::string& value)
	{
		std::vector<Rml::Element*> result;
		if (root.HasAttribute(name) && (value.empty() || attribute(root, name) == value))
			result.push_back(&root);
		const std::vector<Rml::Element*> found =
			elements(root, "[" + std::string(name) + (value.empty() ? "]" : "=" + value + "]"));
		result.insert(result.end(), found.begin(), found.end());
		return result;
	}

	// The first container of a set says which sections it has, before the user's order changes it.
	bool Docking::learnSections(Attached& attached, Rml::Element& root)
	{
		auto* document = dynamic_cast<Rml::ElementDocument*>(attached.document.get());
		if (document == nullptr)
			return false;
		bool learned = false;
		for (const Rml::Element* container : elementsWith(root, "sections", {}))
		{
			std::vector<Section>& known = attached.sections[attribute(*container, "sections")];
			if (!known.empty())
				continue;
			for (int i = 0; i < container->GetNumChildren(); ++i)
				if (const Rml::Element* child = container->GetChild(i); child->HasAttribute("section"))
					known.push_back({attribute(*child, "section"), attribute(*child, "label"),
						attribute(*child, "default") == "hidden"});
			learned = learned || !known.empty();
		}
		for (Rml::Element* pane : elements(*document, "[setup]"))
			for (const Section& known : attached.sections[attribute(*pane, "setup")])
				if (pane->QuerySelector("[setup-section=" + known.id + "]") == nullptr)
				{
					Rml::Element* item = pane->AppendChild(document->CreateElement("button"));
					item->SetAttribute("type", "toggle");
					item->SetAttribute("setup-section", known.id);
					item->SetInnerRML(known.label);
				}
		return learned;
	}

	void Docking::arrangeSections(Rml::Element& element)
	{
		Rml::ElementDocument* document = element.GetOwnerDocument();
		const Rml::Element* up = &element; // in the document, not just made for it
		while (up != nullptr && up != document)
			up = up->GetParentNode();
		const auto attached = m_windows.find(windowOf(element));
		if (document == nullptr || up == nullptr || attached == m_windows.end())
			return;
		if (learnSections(attached->second, element))
			applySections(attached->first, attached->second, *document); // the set's panes and every container of it
		else
			applySections(attached->first, attached->second, element);
		document->GetContext()->RequestNextUpdate(0);
	}

	void Docking::fitSections()
	{
		for (auto& [name, attached] : m_windows)
		{
			auto* document = dynamic_cast<Rml::ElementDocument*>(attached.document.get());
			if (document == nullptr || attached.fits.empty() || !document->IsVisible())
				continue;
			// The room as it is now, which the last drawing may not have had yet. ponytail: a layout pass per step,
			// which costs little while nothing changed; listen for the changes if it shows in a profile.
			document->UpdateDocument();
			bool changed = false;
			for (const Rml::ObserverPtr<Rml::Element>& container : attached.fits)
				if (Rml::Element* element = container.get())
					changed = fit(*element) || changed;
			if (changed)
				document->GetContext()->RequestNextUpdate(0);
		}
	}

	bool Docking::fit(Rml::Element& container)
	{
		// What a child takes of the container's width, with its margins; a section without room is laid out out of the
		// flow, so this is what it would take.
		const auto width = [](Rml::Element& child) { return child.GetBox().GetSize(Rml::BoxArea::Margin).x; };
		std::vector<Rml::Element*> sections; // the shown ones before the spacer, which can go
		float room = container.GetBox().GetSize(Rml::BoxArea::Content).x;
		bool beforeSpacer = true;
		bool changed = false;
		for (int i = 0; i < container.GetNumChildren(); ++i)
		{
			Rml::Element* child = container.GetChild(i);
			const bool section = child->HasAttribute("section");
			if (child->IsClassSet("spacer"))
				beforeSpacer = false;
			else if (section && beforeSpacer && !child->IsClassSet("hidden"))
				sections.push_back(child);
			else if (child->GetComputedValues().display() != Rml::Style::Display::None)
				room -= width(*child);
			if (section && (child->IsClassSet("hidden") || !beforeSpacer) && child->IsClassSet("no-room"))
			{
				child->SetClass("no-room", false);
				changed = true;
			}
		}
		// The last go first: a section shows while it and those before it fit, within rounding.
		bool fits = true;
		for (Rml::Element* section : sections)
		{
			room -= width(*section);
			fits = fits && room > -0.01F;
			if (section->IsClassSet("no-room") == fits)
			{
				section->SetClass("no-room", !fits);
				changed = true;
			}
		}
		return changed;
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
		const std::array<Toggle, 10> toggles = {{
			{"window.menuBar", "menuBar", "Shows or hides the menu bar.", {}},
			{"window.channelZone", "channel", "Shows or hides the Channel zone.", {"Mod+Alt+C"}},
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

	Json& Docking::size(const std::string& window, const std::string& name)
	{
		// A zone's, also before the window is attached.
		if (element(window, "[resizable=" + name + "]") == nullptr)
			return zone(window, name)["size"];
		Json& sizes = m_layout["windows"][window]["sizes"];
		if (!sizes.is_object())
			sizes = Json::object();
		return sizes[name];
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
		size(window, name) = std::max(dp, 0.0);
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
		const auto count = static_cast<int>(shown.size());
		const auto index = static_cast<int>(activeTab(shown, window, name));
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
			keepBounds(json, m_shell.bounds(shown.window), m_shell.displays());
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

	void Docking::resetSections(const std::string& window, const std::string& set)
	{
		if (const Json* sets = find(m_layout, {"windows", window, "sections"});
			sets == nullptr || !sets->is_object() || !sets->contains(set))
			return;
		m_layout["windows"][window]["sections"].erase(set);
		save();
		apply();
	}

	void Docking::savePreset(const std::string& window, const std::string& set, const std::string& name)
	{
		if (name.empty())
			return;
		Json hidden = Json::array();
		for (const std::string& id : sections(window, set))
			if (sectionHidden(window, set, id))
				hidden.push_back(id);
		Json list = Json::array();
		for (const Json& preset : m_presets.value(set, Json::array()))
			if (preset.value("name", std::string()) != name)
				list.push_back(preset);
		list.push_back({{"name", name}, {"order", sections(window, set)}, {"hidden", hidden}});
		m_presets[set] = list;
		m_binding.send("settings.set", {{"key", "ui.setupPresets"}, {"value", m_presets}});
		apply();
	}

	void Docking::recallPreset(const std::string& window, const std::string& set, const std::string& name)
	{
		for (const Json& preset : m_presets.value(set, Json::array()))
			if (preset.value("name", std::string()) == name)
			{
				Json& json = sectionsJson(window, set);
				json["order"] = preset.value("order", Json::array());
				json["hidden"] = preset.value("hidden", Json::array());
				save();
				apply();
				return;
			}
	}

	void Docking::openPresetDialog(Rml::Context& context, const std::string& window, const std::string& set)
	{
		// Open for another pane, maybe in another window: it moves to this one.
		if (Rml::Element* open = m_presetDialog.get())
		{
			open->RemoveEventListener(m_closeEvent, this);
			m_presetDialog.reset();
			open->GetOwnerDocument()->Close();
		}
		const std::string path =
			std::filesystem::path(m_panelWindow).replace_filename("setup-preset.rml").generic_u8string();
		Rml::ElementDocument* dialog = context.LoadDocument(path);
		if (dialog == nullptr)
		{
			logging::warning("The docking system can't open the dialog {}", path);
			return;
		}
		dialog->AddEventListener(m_closeEvent, this);
		dialog->Show(Rml::ModalFlag::Modal);
		m_presetDialog = dialog->GetObserverPtr();
		m_presetWindow = window;
		m_presetSet = set;
	}

	void Docking::presetDialogClosed(Rml::Event& event)
	{
		Rml::ElementDocument* dialog = event.GetTargetElement()->GetOwnerDocument();
		dialog->RemoveEventListener(m_closeEvent, this);
		m_presetDialog.reset();
		if (event.GetParameter<Rml::String>("result", "") != "ok")
			return;
		const auto* field = dynamic_cast<Rml::ElementFormControl*>(dialog->GetElementById("setup-preset-name"));
		if (field != nullptr)
			later([this, window = m_presetWindow, set = m_presetSet, name = field->GetValue()]
				{ savePreset(window, set, name); });
	}

	void Docking::open(const std::string& id)
	{
		const Panel* shown = panel(id);
		if (shown == nullptr)
			return;
		if (const Json* floating = find(m_layout, {"panels", id, "floating"});
			floating != nullptr && floating->is_boolean() && floating->get<bool>())
		{
			// Its window may be behind another one or minimized.
			if (const Window* window = windowOf(id))
				m_shell.raise(window->id());
			return;
		}
		Json& json = zone(shown->window, shown->zone);
		if (std::vector<std::string> hidden = strings(find(json, {"hidden"})); contains(hidden, id))
		{
			hidden.erase(std::remove(hidden.begin(), hidden.end(), id), hidden.end());
			json["hidden"] = hidden;
		}
		json["active"] = id;
		json["visible"] = true;
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
		m_saves.insert(m_binding.send("settings.set", {{"key", "ui.layout"}, {"value", m_layout}},
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired())
					return;
				m_saves.erase(result.request);
				if (!result.ok())
					logging::warning("The window layout couldn't be saved: {}", result.error);
			}));
	}

	void Docking::saveWindows()
	{
		const std::vector<Display> displays = m_shell.displays();
		bool changed = false;
		for (const auto& [id, shown] : m_shown)
			if (shown.window != 0 && m_shell.find(shown.window) != nullptr)
				changed = keepBounds(m_layout["panels"][id], m_shell.bounds(shown.window), displays) || changed;
		for (AppWindow& window : m_appWindows)
		{
			const WindowId id = window.current();
			const bool open = id != 0 && m_shell.find(id) != nullptr;
			const bool opened = open != window.open;
			bool maximized = open && m_shell.maximized(id);
			// A maximized window keeps its normal bounds, unless it opened maximized.
			const Rect bounds =
				open && (!maximized || opened || window.bounds.width <= 0) ? m_shell.bounds(id) : window.bounds;
			// A window that was maximized when it closed opens maximized again.
			if (open && opened && !maximized && savedMaximized(window.name))
			{
				m_shell.setMaximized(id, true);
				maximized = true;
			}
			const bool moved = open && !(bounds == window.bounds);
			const bool zoomed = open && maximized != window.maximized;
			window.open = open;
			window.bounds = bounds;
			if (open)
				window.maximized = maximized;
			if (!opened && !moved && !zoomed)
				continue;
			Json& entry = m_layout["windows"][window.name];
			if (!entry.is_object())
				entry = Json::object();
			if (window.setOpen && entry.value("open", false) != open)
			{
				entry["open"] = open;
				changed = true;
			}
			if (!open)
				continue;
			changed = keepBounds(entry, bounds, displays) || changed;
			if (maximized != savedMaximized(window.name))
			{
				if (maximized)
					entry["maximized"] = true;
				else
					entry.erase("maximized");
				changed = true;
			}
		}
		if (changed)
			save();
	}

	void Docking::addWindow(
		const std::string& name, std::function<WindowId()> current, std::function<void(bool open)> setOpen)
	{
		AppWindow window{name, std::move(current), std::move(setOpen), false, {}, false};
		const WindowId id = window.current();
		window.open = id != 0 && m_shell.find(id) != nullptr;
		if (window.open)
		{
			if (!window.setOpen) // the main window, where it was
				if (const std::optional<Rect> saved = bounds(name))
					m_shell.setBounds(id, *saved);
			window.bounds = m_shell.bounds(id);
			if (!window.setOpen && savedMaximized(name))
				m_shell.setMaximized(id, true);
			window.maximized = m_shell.maximized(id);
		}
		// ui.layout from the last session can have it open while it is closed now, or the other way round: corrected
		// at once, so that a workspace added now has it as it is.
		if (window.setOpen)
			if (const Json* open = find(m_layout, {"windows", name, "open"});
				(open != nullptr && open->is_boolean() && open->get<bool>()) != window.open)
			{
				Json& entry = m_layout["windows"][name];
				if (!entry.is_object())
					entry = Json::object();
				entry["open"] = window.open;
				save();
			}
		m_appWindows.push_back(std::move(window));
	}

	std::optional<Rect> Docking::bounds(const std::string& name) const
	{
		return savedBounds(find(m_layout, {"windows", name}), g_panelWindowSize, m_shell.displays());
	}

	void Docking::applyWindows()
	{
		for (AppWindow& window : m_appWindows)
		{
			const WindowId id = window.current();
			const bool open = id != 0 && m_shell.find(id) != nullptr;
			const Json* wanted = find(m_layout, {"windows", window.name, "open"});
			const bool opens = wanted != nullptr && wanted->is_boolean() && wanted->get<bool>();
			if (window.setOpen && open != opens)
			{
				window.setOpen(opens); // which opens it at its bounds from ui.layout
				continue;
			}
			if (!open)
				continue;
			const bool maximized = savedMaximized(window.name);
			if (const std::optional<Rect> saved = bounds(window.name))
			{
				m_shell.setMaximized(id, false); // the saved bounds are the normal ones
				m_shell.setBounds(id, *saved);
				window.bounds = m_shell.bounds(id);
			}
			m_shell.setMaximized(id, maximized);
			window.maximized = maximized;
		}
	}

	bool Docking::savedMaximized(const std::string& name) const
	{
		const Json* maximized = find(m_layout, {"windows", name, "maximized"});
		return maximized != nullptr && maximized->is_boolean() && maximized->get<bool>();
	}

	void Docking::applySize(Rml::Element& element, const Json* saved)
	{
		const double dp =
			saved != nullptr && saved->is_number() ? saved->get<double>() : element.GetAttribute<double>("size", -1.0);
		if (!(dp >= 0.0)) // NaN too
			return;
		const Rml::Element* parent = element.GetParentNode();
		const bool column = parent != nullptr &&
			(parent->GetComputedValues().flex_direction() == Rml::Style::FlexDirection::Column ||
				parent->GetComputedValues().flex_direction() == Rml::Style::FlexDirection::ColumnReverse);
		const Rml::Property length(static_cast<float>(std::min(dp, g_mostSize)), Rml::Unit::DP);
		if (column)
			element.SetProperty(Rml::PropertyId::Height, length);
		else
			element.SetProperty(Rml::PropertyId::Width, length);
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
			// The keyboard focus leaves a zone that hides, for the main zone, so that the zone's keys no longer apply.
			if (const Rml::Element* focus = document->GetContext()->GetFocusElement(); !shown && inside(focus, *zone))
				if (Rml::Element* main = document->QuerySelector("[zone=main]"))
					main->Focus();
			for (Rml::Element* splitter : elements(*document, "splitter[for=" + zoneName + "]"))
				splitter->SetClass("hidden", !shown);
			applySize(*zone, find(m_layout, {"windows", window, "zones", zoneName, "size"}));
			applyTabs(attached, *zone, window, zoneName);
		}
		for (Rml::Element* sized : elements(*document, "[resizable]"))
			applySize(*sized, find(m_layout, {"windows", window, "sizes", attribute(*sized, "resizable")}));
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
		learnSections(attached, *document); // including from panels that showed for the first time
		applySections(window, attached, *document);
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

	// The containers of sections in root, root too, and its setup panes.
	void Docking::applySections(const std::string& window, const Attached& attached, Rml::Element& root)
	{
		for (const auto& [set, known] : attached.sections)
		{
			const std::vector<std::string> order = sections(window, set);
			for (Rml::Element* container : elementsWith(root, "sections", set))
			{
				arrange(*container, "section", order);
				for (Rml::Element* part : elements(*container, "[section]"))
					part->SetClass("hidden", sectionHidden(window, set, attribute(*part, "section")));
			}
			for (Rml::Element* pane : elementsWith(root, "setup", set))
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
		// Save Preset has nothing to save while the window doesn't know the set's sections, such as a mixer window
		// without channels.
		for (Rml::Element* save : elementsWith(root, "setup-preset-save", {}))
		{
			if (sections(window, attribute(*save, "setup-preset-save")).empty())
				save->SetAttribute("disabled", "");
			else
				save->RemoveAttribute("disabled");
		}
		// The presets' buttons, made again only when the presets changed, since a button mustn't go while its click is
		// handled.
		for (Rml::Element* list : elementsWith(root, "setup-presets", {}))
		{
			std::vector<std::string> names;
			for (const Json& preset : m_presets.value(attribute(*list, "setup-presets"), Json::array()))
				names.push_back(preset.value("name", std::string()));
			std::vector<std::string> built;
			for (const Rml::Element* item : elements(*list, "[setup-preset]"))
				built.push_back(attribute(*item, "setup-preset"));
			if (names == built)
				continue;
			while (list->HasChildNodes())
				list->RemoveChild(list->GetFirstChild());
			for (const std::string& name : names)
			{
				Rml::Element* item = list->AppendChild(list->GetOwnerDocument()->CreateElement("button"));
				item->SetAttribute("setup-preset", name);
				item->SetInnerRML(Rml::StringUtilities::EncodeRml(name));
			}
		}
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
		if (!shown.empty())
			tabset->SetActiveTab(static_cast<int>(activeTab(shown, window, zoneName)));
	}

	size_t Docking::activeTab(
		const std::vector<std::string>& shown, const std::string& window, const std::string& name) const
	{
		const Json* active = find(m_layout, {"windows", window, "zones", name, "active"});
		const auto it = active != nullptr && active->is_string()
			? std::find(shown.begin(), shown.end(), active->get<std::string>())
			: shown.end();
		return it == shown.end() ? 0 : static_cast<size_t>(it - shown.begin());
	}

	bool Docking::showing(const std::string& id) const
	{
		const Panel* info = panel(id);
		const auto shown = m_shown.find(id);
		if (info == nullptr || shown == m_shown.end() || shown->second.element == nullptr)
			return false;
		if (shown->second.window != 0)
			return true;
		const std::vector<std::string> tabs = shownTabs(info->window, info->zone);
		return visible(info->window, info->zone) && !tabs.empty() &&
			tabs[activeTab(tabs, info->window, info->zone)] == id;
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
				spec.bounds = savedBounds(find(m_layout, {"panels", panel.id}), g_panelWindowSize, m_shell.displays());
				try
				{
					const Window& window = m_shell.open(spec);
					Rml::ElementDocument* document = window.context().LoadDocument(m_panelWindow.generic_u8string());
					if (document == nullptr || document->GetElementById("panel-content") == nullptr)
					{
						m_shell.close(window.id());
						logging::error("Cannot load the panel window {}", m_panelWindow.u8string());
						continue;
					}
					document->Show();
					shown.window = window.id();
					fillPanelWindow(panel, shown, *document);
				}
				catch (const std::runtime_error& error)
				{
					logging::error("Cannot open a window for the panel {}: {}", panel.id, error.what());
				}
			}
		}
	}

	bool Docking::fillPanelWindow(const Panel& panel, Shown& shown, Rml::ElementDocument& document)
	{
		document.SetTitle(panel.title);
		document.SetAttribute("panel", panel.id);
		listen(document, true);
		shown.document = document.GetObserverPtr();
		Rml::Element* content = document.GetElementById("panel-content");
		shown.element = content;
		if (content != nullptr && panel.create)
			shown.content = panel.create(*content);
		return content != nullptr;
	}

	void Docking::reload(Rml::ElementDocument& old, Rml::ElementDocument& loaded)
	{
		const auto attached =
			std::find_if(m_windows.begin(), m_windows.end(), [&old](const std::pair<const std::string, Attached>& entry)
				{ return entry.second.document.get() == &old; });
		if (attached != m_windows.end())
		{
			// What the panels docked there filled goes first, what create returned before its element.
			for (auto& [id, shown] : m_shown)
				if (shown.window == 0 && shown.element != nullptr && shown.element->GetOwnerDocument() == &old)
				{
					shown.content.reset();
					shown.element = nullptr;
				}
			attach(attached->first, loaded);
			return;
		}
		for (auto& [id, shown] : m_shown)
			if (shown.document.get() == &old)
			{
				listen(old, false);
				shown.content.reset();
				shown.element = nullptr;
				const Panel* info = panel(id);
				if (info != nullptr && !fillPanelWindow(*info, shown, loaded))
					logging::warning("{} has no #panel-content now; the panel {} shows nothing until it docks",
						m_panelWindow.u8string(), id);
				return;
			}
	}

	bool Docking::focusZone(Rml::Element& from, int step)
	{
		Rml::ElementDocument* document = from.GetOwnerDocument();
		if (document == nullptr)
			return false;
		const auto rank = [](const Rml::Element* zone)
		{
			const std::string name = attribute(*zone, "zone");
			return std::find(g_zoneOrder.begin(), g_zoneOrder.end(), name) - g_zoneOrder.begin();
		};
		std::vector<Rml::Element*> zones = elements(*document, "[zone]");
		zones.erase(std::remove_if(zones.begin(), zones.end(),
						[&rank](const Rml::Element* zone)
						{
							return zone->IsClassSet("hidden") ||
								rank(zone) == static_cast<std::ptrdiff_t>(g_zoneOrder.size());
						}),
			zones.end());
		if (zones.empty())
			return false;
		std::sort(zones.begin(), zones.end(),
			[&rank](const Rml::Element* a, const Rml::Element* b) { return rank(a) < rank(b); });
		// Outside the zones, e.g. on the toolbar, which belongs to the focus zone project, as in the project zone.
		const auto at = std::find(zones.begin(), zones.end(), closestWith(&from, "zone"));
		const auto count = static_cast<std::ptrdiff_t>(zones.size());
		const std::ptrdiff_t index = at == zones.end() ? 0 : at - zones.begin();
		return zones[static_cast<size_t>((((index + step) % count) + count) % count)]->Focus(true);
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
	// go while their events are handled. A document loaded again is the exception: its panels' contents must go while
	// it is still there, and they and the new document are off the event's path.
	void Docking::ProcessEvent(Rml::Event& event)
	{
		Rml::Element* target = event.GetTargetElement();
		if (target == nullptr)
			return;
		if (event.GetType() == m_reloadEvent)
		{
			// Only another document of the same context takes its place, whatever pointer the event has.
			auto* old = dynamic_cast<Rml::ElementDocument*>(target);
			Rml::Context* context = target->GetContext();
			const void* named = event.GetParameter<void*>("document", nullptr);
			for (int i = 0; old != nullptr && context != nullptr && i < context->GetNumDocuments(); ++i)
				if (Rml::ElementDocument* loaded = context->GetDocument(i); loaded != old && loaded == named)
				{
					reload(*old, *loaded);
					break;
				}
			return;
		}
		if (event.GetType() == m_closeEvent)
		{
			if (target->GetOwnerDocument() == m_presetDialog.get())
				presetDialogClosed(event);
			return;
		}
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
				size(window, attribute(*target, "for")) = event.GetParameter("value", 0.0F); // saved on dragend
			break;
		case Rml::EventId::Dragend:
			if (target->GetTagName() == "splitter" && target->HasAttribute("for"))
				save();
			break;
		case Rml::EventId::Dblclick:
			// A zone's splitter hides it; an element that isn't a zone, such as the track list, stays.
			if (const Rml::Element* splitter = closestWith(target, "for"); splitter != nullptr &&
				splitter->GetTagName() == "splitter" && !window.empty() &&
				element(window, "[zone=" + attribute(*splitter, "for") + "]") != nullptr)
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
			else if (const Rml::Element* reset = closestWith(target, "setup-reset");
				reset != nullptr && !window.empty())
			{
				later([this, window = std::move(window), set = attribute(*reset, "setup-reset")]
					{ resetSections(window, set); });
			}
			else if (const Rml::Element* list = closestWith(target, "setup-presets");
				list != nullptr && !window.empty())
			{
				if (const Rml::Element* preset = closestWith(target, "setup-preset"))
					later([this, window = std::move(window), set = attribute(*list, "setup-presets"),
							  name = attribute(*preset, "setup-preset")] { recallPreset(window, set, name); });
			}
			else if (const Rml::Element* saving = closestWith(target, "setup-preset-save");
				saving != nullptr && !window.empty() && !saving->HasAttribute("disabled"))
			{
				later(
					[this, document = target->GetOwnerDocument()->GetObserverPtr(), window = std::move(window),
						set = attribute(*saving, "setup-preset-save")]
					{
						if (const Rml::Element* pane = document.get(); pane != nullptr && pane->GetContext() != nullptr)
							openPresetDialog(*pane->GetContext(), window, set);
					});
			}
			else if (const Rml::Element* all = closestWith(target, "setup-all"); all != nullptr && !window.empty())
			{
				later(
					[this, window = std::move(window), set = attribute(*all, "setup-all")]
					{
						for (const std::string& id : sections(window, set))
							setSectionHidden(window, set, id, false);
					});
			}
			else if (const Rml::Element* button = closestWith(target, "id");
				button != nullptr && button->GetId() == "dock")
			{
				later([this, id = attribute(*target->GetOwnerDocument(), "panel")] { setFloating(id, false); });
			}
			else if (const Rml::Element* hider = closestWith(target, "hide"); hider != nullptr && !window.empty())
			{
				later([this, window = std::move(window), name = attribute(*hider, "hide")]
					{ setVisible(window, name, false); });
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
		case Rml::EventId::Keydown:
			// Tab and Shift+Tab move between the zones, before the document moves between its controls.
			if (event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN) == Rml::Input::KI_TAB &&
				!event.GetParameter("ctrl_key", false) && !event.GetParameter("alt_key", false) &&
				!event.GetParameter("meta_key", false) && !keepsTab(*target) &&
				focusZone(*target, event.GetParameter("shift_key", false) ? -1 : 1))
				event.StopPropagation();
			break;
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
