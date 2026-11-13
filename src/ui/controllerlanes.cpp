// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/controllerlanes.h"

#include "engine/midi/chords.h"
#include "engine/midi/quantizesettings.h"
#include "platform/log.h"
#include "ui/keyeditor.h"
#include "ui/timeformat.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <map>
#include <utility>

namespace gnubase::ui
{
	using engine::Json;

	namespace
	{
		// The lanes' types as the menus list them (p. 1158), the controller events' first; controllers come after.
		constexpr std::array<std::pair<const char*, const char*>, 8> g_types = {{
			{"velocity", "Velocity"},
			{"pitchBend", "Pitchbend"},
			{"channelPressure", "Aftertouch"},
			{"polyPressure", "Poly Pressure"},
			{"program", "Program Change"},
			{"probability", "Play Probability"},
			{"velocityVariance", "Velocity Variance"},
			{"offVelocity", "Off Velocity"},
		}};

		// Controllers the menus list by name; all 128 are in the Continuous Controllers submenu.
		constexpr std::array<std::pair<int, const char*>, 5> g_namedControllers = {{
			{1, "Modulation"},
			{7, "Volume"},
			{10, "Pan"},
			{11, "Expression"},
			{64, "Sustain"},
		}};

		bool isNoteLane(const std::string& type)
		{
			return type == "velocity" || type == "offVelocity" || type == "probability" || type == "velocityVariance";
		}

		std::string laneName(const std::string& type, int number)
		{
			if (type == "controller")
			{
				for (const auto& [each, name] : g_namedControllers)
					if (each == number)
						return std::string(name) + " (CC " + std::to_string(number) + ")";
				return "CC " + std::to_string(number);
			}
			if (type == "polyPressure")
				return "Poly Pressure " + engine::midi::pitchName(number);
			for (const auto& [each, name] : g_types)
				if (type == each)
					return name;
			return type;
		}

		std::string attribute(const Rml::Element& element, const char* name)
		{
			return element.GetAttribute<Rml::String>(name, "");
		}

		uint64_t idAttribute(const Rml::Element& element, const char* name)
		{
			const std::string value = attribute(element, name);
			return value.empty() ? 0 : std::stoull(value);
		}

		Rml::Element* withAttribute(Rml::Element* element, const char* name, const Rml::Element* stop)
		{
			for (; element != nullptr && element != stop; element = element->GetParentNode())
				if (element->HasAttribute(name))
					return element;
			return nullptr;
		}

		Rml::Property percent(double value)
		{
			return {static_cast<float>(value), Rml::Unit::PERCENT};
		}

		int64_t ticksOf(const Json& track, int64_t value, const TimeContext& time)
		{
			if (track.value("timeBase", std::string("musical")) == "linear")
				return time.tempo.toTicks(value, time.sampleRate);
			return value;
		}

		// A lane type's items for a menu: the types, named controllers and every controller by sixteen.
		std::string typeItems()
		{
			std::string rml;
			const auto item = [](const std::string& type, int number, const std::string& text)
			{
				return R"(<menuitem lane-type=")" + type + R"(" lane-number=")" + std::to_string(number) + R"(">)" +
					Rml::StringUtilities::EncodeRml(text) + "</menuitem>";
			};
			for (const auto& [type, name] : g_types)
				rml += item(type, 0, name);
			rml += "<menuseparator/>";
			for (const auto& [number, name] : g_namedControllers)
				rml += item("controller", number, laneName("controller", number));
			rml += "<menuitem>Continuous Controllers<menu>";
			for (int group = 0; group < 128; group += 16)
			{
				rml += "<menuitem>CC " + std::to_string(group) + "-" + std::to_string(group + 15) + "<menu>";
				for (int number = group; number < group + 16; ++number)
					rml += item("controller", number, laneName("controller", number));
				rml += "</menu></menuitem>";
			}
			return rml + "</menu></menuitem>";
		}

		std::string actionItem(const char* action, const char* text)
		{
			return std::string(R"(<menuitem lane-action=")") + action + R"(">)" + text + "</menuitem>";
		}

		// The controller value of an event as a lane shows it, 0 to 1: a program change's program.
		double eventValue(const Json& event)
		{
			if (event.at("type") == "program")
				return event.at("number").get<double>() / 127.0;
			return event.value("value", 0.0);
		}

		double noteValue(const Json& note, const std::string& type)
		{
			if (type == "offVelocity")
				return note.at("releaseVelocity").get<double>();
			if (type == "probability")
				return note.value("probability", 1.0);
			if (type == "velocityVariance")
				return (note.value("velocityVariance", 0.0) + 1.0) / 2.0; // -1 at the bottom, 0 in the middle
			return note.at("velocity").get<double>();
		}

		// A display value as the lane's field: the velocity variance from -1 to 1.
		double fieldValue(const std::string& type, double value)
		{
			return type == "velocityVariance" ? (2.0 * value) - 1.0 : value;
		}
	}

	ControllerLanes::ControllerLanes(EngineBinding& binding, Rml::Element& root, std::string view)
		: m_binding(binding), m_root(root.GetObserverPtr()), m_name(std::move(view)), m_menuSuffix(menuIdSuffix(m_name))
	{
		root.SetInnerRML(R"(<div class="controller-lane-list"/><div class="controller-lanes-footer">)"
						 R"(<button id="controller-lane-create" menu="controller-create-menu)" +
			m_menuSuffix +
			R"(" title="Create Controller Lane">+</button>)"
			R"(<button id="controller-lane-setup" menu="controller-setup-menu)" +
			m_menuSuffix + R"(" title="Controller Lane Setup">Setup</button></div>)");
		Rml::ElementDocument* document = root.GetOwnerDocument();
		const std::array<std::pair<const char*, std::string>, 4> menus = {{
			{"controller-lane-menu",
				typeItems() + "<menuseparator/>" + actionItem("selectAll", "Select All Events on Lane") +
					actionItem("remove", "Remove This Lane") + "<menuseparator/>" +
					actionItem("applyProbability", "Apply MIDI Play Probability") +
					actionItem("resetProbability", "Reset MIDI Play Probability") +
					actionItem("applyVariance", "Apply MIDI Velocity Variance") +
					actionItem("resetVariance", "Reset MIDI Velocity Variance")},
			{"controller-create-menu",
				typeItems() + "<menuseparator/>" + actionItem("showUsed", "Show Used Controllers") +
					actionItem("velocityOnly", "Show Velocity Lane Only") +
					actionItem("toggle", "Show/Hide Controller Lanes")},
			{"controller-setup-menu", ""},
			// What a right click below a lane's name opens (p. 1261): the lanes to add, and this one to remove.
			{"controller-lane-context-menu",
				typeItems() + "<menuseparator/>" + actionItem("remove", "Remove This Lane")},
		}};
		for (const auto& [id, items] : menus)
		{
			const Rml::ElementPtr holder = document->CreateElement("div");
			holder->SetInnerRML(std::string(R"(<menu id=")") + id + m_menuSuffix + R"(">)" + items + "</menu>");
			Rml::Element* menu = document->AppendChild(holder->RemoveChild(holder->GetFirstChild()));
			menu->AddEventListener(Rml::EventId::Click, this);
			m_menus.push_back(menu->GetObserverPtr());
		}
		// The mouseup that ends a gesture goes to the document (listen), so that it ends wherever the mouse goes up.
		for (const Rml::EventId event : {Rml::EventId::Mousedown, Rml::EventId::Mousemove, Rml::EventId::Click})
			root.AddEventListener(event, this);

		// The presets are a user setting (p. 1166).
		m_subscriptions.push_back(m_binding.on("settings.changed",
			[this](const engine::Event& event)
			{
				const Json& values = event.data.at("values");
				if (const auto presets = values.find("ui.controllerLanePresets"); presets != values.end())
				{
					m_presets = *presets;
					m_presetsKnown = true;
					render();
				}
			}));
		m_binding.send("settings.get", {{"key", "ui.controllerLanePresets"}},
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok() || m_presetsKnown) // gone, or newer changes win
					return;
				m_presets = result.value.at("value");
				render();
			});
	}

	ControllerLanes::~ControllerLanes()
	{
		listen(false);
		if (Rml::Element* root = m_root.get())
			for (const Rml::EventId event : {Rml::EventId::Mousedown, Rml::EventId::Mousemove, Rml::EventId::Click})
				root->RemoveEventListener(event, this);
		for (const Rml::ObserverPtr<Rml::Element>& menu : m_menus)
			if (Rml::Element* element = menu.get())
			{
				element->RemoveEventListener(Rml::EventId::Click, this);
				if (Rml::Element* parent = element->GetParentNode())
					parent->RemoveChild(element);
			}
		if (Rml::Element* dialog = m_dialog.get())
			dialog->RemoveEventListener(Rml::EventId::Click, this);
	}

	void ControllerLanes::setTool(std::string tool, std::string lineMode)
	{
		m_tool = std::move(tool);
		m_lineMode = std::move(lineMode);
	}

	void ControllerLanes::show(const Json& project, const Json& view, int64_t from, int64_t to)
	{
		m_view = view;
		m_from = from;
		m_to = std::max(to, from + 1);
		m_parts.clear();
		m_partIndex.clear();
		m_noteSelection.clear();
		m_controllerSelection.clear();
		m_lanes.clear();
		const std::string grid = project.value("quantize", Json::object()).value("grid", std::string("1/16"));
		m_grid = engine::midi::noteValueFromString(grid).value_or(engine::midi::NoteValue{}).ticks();
		const auto midiParts = project.find("midiParts");
		if (midiParts != project.end() && m_view.contains("parts"))
		{
			// Indexed once here, not for every lane on every render.
			const TimeContext time = projectTime(project);
			std::map<uint64_t, const Json*> tracks;
			for (const Json& track : project.at("tracks"))
				tracks[track.at("id").get<uint64_t>()] = &track;
			std::map<uint64_t, const Json*> parts;
			for (const Json& part : *midiParts)
				parts[part.at("id").get<uint64_t>()] = &part;
			const auto editable = m_view.at("editable").get<std::set<uint64_t>>();
			for (const Json& id : m_view.at("parts"))
				if (const auto found = parts.find(id.get<uint64_t>()); found != parts.end())
				{
					const Json& part = *found->second;
					const Json& track = *tracks.at(part.at("track").get<uint64_t>());
					const auto position = part.at("position").get<int64_t>();
					m_partIndex[found->first] = m_parts.size();
					m_parts.push_back({found->first, ticksOf(track, position, time),
						ticksOf(track, position + part.at("length").get<int64_t>(), time),
						editable.count(found->first) != 0, part});
				}
			for (const Json& selected : m_view.value("selection", Json::array()))
				m_noteSelection.emplace(selected.at("part").get<uint64_t>(), selected.at("note").get<uint64_t>());
			for (const Json& selected : m_view.value("controllerSelection", Json::array()))
				m_controllerSelection.emplace(
					selected.at("part").get<uint64_t>(), selected.at("controller").get<uint64_t>());
			for (const Json& lane : m_view.value("lanes", Json::array()))
				m_lanes.push_back({lane.at("type").get<std::string>(), lane.value("number", 0)});
		}
		render();
	}

	const Json* ControllerLanes::partJson(uint64_t id) const
	{
		const auto found = m_partIndex.find(id);
		return found != m_partIndex.end() ? &m_parts[found->second].json : nullptr;
	}

	bool ControllerLanes::noteSelected(uint64_t part, uint64_t note) const
	{
		return m_noteSelection.count({part, note}) != 0;
	}

	int ControllerLanes::laneChannel(const Lane& lane, const Part& part, int64_t from, int64_t to) const
	{
		std::optional<int> first;
		for (const Item& item : items(lane))
			if (item.part == part.id)
			{
				if (item.start >= from && item.start <= to)
					return item.channel;
				if (!first)
					first = item.channel;
			}
		return first.value_or(0);
	}

	// The lane's bars or steps, by part and position.
	std::vector<ControllerLanes::Item> ControllerLanes::items(const Lane& lane) const
	{
		std::vector<Item> found;
		for (const Part& part : m_parts)
		{
			if (isNoteLane(lane.type))
			{
				for (const Json& note : part.json.at("notes"))
				{
					const uint64_t id = note.at("id").get<uint64_t>();
					const int64_t start = part.start + note.at("position").get<int64_t>();
					found.push_back({part.id, id, start, start, noteValue(note, lane.type), 0, part.editable,
						noteSelected(part.id, id)});
				}
				continue;
			}
			std::vector<Item> events;
			for (const Json& event : part.json.at("controllers"))
			{
				if (event.at("type") != lane.type ||
					((lane.type == "controller" || lane.type == "polyPressure") &&
						event.value("number", 0) != lane.number))
					continue;
				const uint64_t id = event.at("id").get<uint64_t>();
				events.push_back(
					{part.id, id, part.start + event.at("position").get<int64_t>(), part.end, eventValue(event),
						event.value("channel", 0), part.editable, m_controllerSelection.count({part.id, id}) != 0});
			}
			std::stable_sort(
				events.begin(), events.end(), [](const Item& a, const Item& b) { return a.start < b.start; });
			for (size_t i = 0; i + 1 < events.size(); ++i)
				events[i].end = events[i + 1].start; // a value holds until the next event (p. 1157)
			found.insert(found.end(), events.begin(), events.end());
		}
		return found;
	}

	void ControllerLanes::render()
	{
		Rml::Element* root = m_root.get();
		if (root == nullptr)
			return;
		const bool shown = m_view.value("showLanes", true);
		if (root->IsClassSet("lanes-hidden") == shown)
			root->SetClass("lanes-hidden", !shown);
		std::string rml;
		m_items.clear();
		for (size_t i = 0; i < m_lanes.size(); ++i)
		{
			const Lane& lane = m_lanes[i];
			const std::string index = std::to_string(i);
			const bool notes = isNoteLane(lane.type);
			rml += R"(<div class="controller-lane" lane=")";
			rml += index;
			rml += R"(" lane-kind=")";
			rml += lane.type;
			rml += R"("><div class="controller-lane-header" contextmenu="controller-lane-context-menu)";
			rml += m_menuSuffix;
			rml += R"("><button class="controller-lane-type" lane-menu=")";
			rml += index;
			rml += R"(" menu="controller-lane-menu)";
			rml += m_menuSuffix;
			rml += R"(">)";
			rml += Rml::StringUtilities::EncodeRml(laneName(lane.type, lane.number));
			rml += R"(</button></div><div class="controller-lane-area" lane=")";
			rml += index;
			rml += R"(">)";
			m_items.push_back(items(lane));
			for (const Item& item : m_items.back())
				rml += std::string(R"(<div class=")") + (notes ? "controller-bar" : "controller-step") +
					(item.selected ? " selected" : "") + (item.editable ? "" : " inactive") + R"(" part=")" +
					std::to_string(item.part) + (notes ? R"(" note=")" : R"(" controller=")") +
					std::to_string(item.id) + R"("/>)";
			rml += "</div></div>";
		}
		Rml::Element* list = root->QuerySelector(".controller-lane-list");
		if (list != nullptr && rml != m_rml)
		{
			list->SetInnerRML(rml);
			m_rml = rml;
		}
		placeItems();

		// The setup menu: the presets, Add Preset, and a submenu that removes one.
		if (Rml::Element* menu = m_menus.size() > 2 ? m_menus[2].get() : nullptr)
		{
			std::string presets;
			std::string removes;
			for (const Json& preset : m_presets)
			{
				const std::string name = Rml::StringUtilities::EncodeRml(preset.value("name", std::string()));
				presets.append(R"(<menuitem lane-preset=")")
					.append(name)
					.append(R"(">)")
					.append(name)
					.append("</menuitem>");
				removes.append(R"(<menuitem remove-preset=")")
					.append(name)
					.append(R"(">)")
					.append(name)
					.append("</menuitem>");
			}
			const std::string items = presets + (presets.empty() ? "" : "<menuseparator/>") +
				actionItem("addPreset", "Add Preset...") +
				(removes.empty() ? ""
								 : R"(<menuitem id="controller-remove-preset">Remove Preset<menu>)" + removes +
							"</menu></menuitem>");
			if (menu->GetInnerRML() != items)
				menu->SetInnerRML(items);
		}
		if (Rml::Context* context = root->GetContext())
			context->RequestNextUpdate(0);
	}

	void ControllerLanes::setRange(int64_t from, int64_t to)
	{
		m_from = from;
		m_to = std::max(to, from + 1);
		placeItems();
	}

	void ControllerLanes::placeItems()
	{
		Rml::Element* list = m_root.get() != nullptr ? m_root.get()->QuerySelector(".controller-lane-list") : nullptr;
		const auto range = static_cast<double>(m_to - m_from);
		for (size_t i = 0; list != nullptr && i < m_items.size(); ++i)
		{
			const Rml::Element* area = list->QuerySelector(".controller-lane-area[lane=\"" + std::to_string(i) + "\"]");
			for (int child = 0;
				area != nullptr && child < area->GetNumChildren() && static_cast<size_t>(child) < m_items[i].size();
				++child)
			{
				const Item& item = m_items[i][static_cast<size_t>(child)];
				Rml::Element* element = area->GetChild(child);
				const double left = static_cast<double>(item.start - m_from) / range * 100.0;
				const double width = static_cast<double>(item.end - item.start) / range * 100.0;
				element->SetProperty(Rml::PropertyId::Left, percent(left));
				if (item.end > item.start)
					element->SetProperty(Rml::PropertyId::Width, percent(width));
				element->SetProperty(Rml::PropertyId::Height, percent(std::clamp(item.value, 0.0, 1.0) * 100.0));
			}
		}
	}

	std::optional<size_t> ControllerLanes::laneAt(Rml::Element* target) const
	{
		const Rml::Element* area = withAttribute(target, "lane", m_root.get());
		if (area == nullptr || !area->IsClassSet("controller-lane-area"))
			return std::nullopt;
		const auto lane = static_cast<size_t>(std::stoul(attribute(*area, "lane")));
		return lane < m_lanes.size() ? std::optional<size_t>(lane) : std::nullopt;
	}

	std::pair<double, double> ControllerLanes::at(const Rml::Event& event, size_t lane) const
	{
		Rml::Element* area =
			m_root.get()->QuerySelector(".controller-lane-area[lane=\"" + std::to_string(lane) + "\"]");
		if (area == nullptr)
			return {0.0, 0.0};
		const Rml::Vector2f origin = area->GetAbsoluteOffset(Rml::BoxArea::Border);
		const Rml::Vector2f size = area->GetBox().GetSize(Rml::BoxArea::Border);
		const float x = (event.GetParameter("mouse_x", 0.0F) - origin.x) / std::max(size.x, 1.0F);
		// The bottom row of pixels is 0, the top one 1.
		const float y = (event.GetParameter("mouse_y", 0.0F) - origin.y) / std::max(size.y - 1.0F, 1.0F);
		return {static_cast<double>(m_from) + (x * static_cast<double>(m_to - m_from)),
			std::clamp(1.0 - static_cast<double>(y), 0.0, 1.0)};
	}

	int64_t ControllerLanes::grid() const
	{
		return m_grid;
	}

	int64_t ControllerLanes::snap(double ticks, bool off) const
	{
		if (off)
			return std::llround(ticks);
		const auto step = static_cast<double>(grid());
		return std::llround(std::round(ticks / step) * step);
	}

	int64_t ControllerLanes::cell(double ticks, int64_t step)
	{
		return std::llround(std::floor(ticks / static_cast<double>(step)) * static_cast<double>(step));
	}

	int64_t ControllerLanes::lengthQuantize() const
	{
		const std::string length = m_view.value("lengthQuantize", std::string("link"));
		if (length == "link")
			return grid();
		return engine::midi::noteValueFromString(length).value_or(engine::midi::NoteValue{}).ticks();
	}

	const ControllerLanes::Part* ControllerLanes::partAt(int64_t ticks) const
	{
		const Part* found = nullptr;
		for (const Part& part : m_parts)
			if (part.editable && ticks >= part.start && ticks < part.end &&
				(found == nullptr || Json(part.id) == m_view.at("activePart")))
				found = &part;
		return found;
	}

	Json ControllerLanes::laneSpec(const Lane& lane)
	{
		return {{"type", lane.type}, {"number", lane.number}};
	}

	Json ControllerLanes::noteChange(const Lane& lane, uint64_t note, double value)
	{
		static const std::map<std::string, const char*> fields = {{"velocity", "velocity"},
			{"offVelocity", "releaseVelocity"}, {"probability", "probability"},
			{"velocityVariance", "velocityVariance"}};
		return {{"id", note}, {fields.at(lane.type), fieldValue(lane.type, std::clamp(value, 0.0, 1.0))}};
	}

	Json ControllerLanes::controllerChange(const Lane& lane, uint64_t controller, double value)
	{
		value = std::clamp(value, 0.0, 1.0);
		if (lane.type == "program")
			return {{"id", controller}, {"number", std::lround(value * 127.0)}};
		return {{"id", controller}, {"value", value}};
	}

	void ControllerLanes::edit(const std::string& description, Json commands)
	{
		if (commands.empty())
			return;
		if (commands.size() == 1)
			m_binding.send(commands[0].at("command").get<std::string>(), commands[0].at("parameters"));
		else
			m_binding.send("history.transaction", {{"description", description}, {"commands", std::move(commands)}});
	}

	void ControllerLanes::setView(Json parameters)
	{
		parameters["editor"] = m_name;
		m_binding.send("editor.set", std::move(parameters));
	}

	void ControllerLanes::setLanes(const std::vector<Lane>& lanes)
	{
		Json json = Json::array();
		for (const Lane& lane : lanes)
			json.push_back(laneSpec(lane));
		setView({{"lanes", json}, {"showLanes", true}});
	}

	void ControllerLanes::ProcessEvent(Rml::Event& event)
	{
		Rml::Element* target = event.GetTargetElement();
		if (target == nullptr)
			return;
		if (event.GetCurrentElement() == m_dialog.get())
		{
			dialogEvent(event);
			return;
		}
		for (const Rml::ObserverPtr<Rml::Element>& menu : m_menus)
			if (event.GetCurrentElement() == menu.get())
			{
				Rml::Element* item = target;
				while (item != nullptr && item != menu.get() && item->GetTagName() != "menuitem")
					item = item->GetParentNode();
				if (item != nullptr && item != menu.get())
					menuItem(*menu.get(), *item);
				return;
			}
		if (event.GetId() == Rml::EventId::Mousedown && event.GetCurrentElement() == m_document.get())
		{
			// A left press anywhere in the document while a gesture goes on: the gesture's mouseup never came, as when
			// the mouse went up outside the window. It ends without its effect before anything hears the press.
			if (event.GetParameter("button", 0) == 0)
				cancel();
			return;
		}
		if (m_view.empty())
		{
			cancel();
			return;
		}
		switch (event.GetId())
		{
		case Rml::EventId::Mousedown:
			if (const Rml::Element* button = withAttribute(target, "lane-menu", m_root.get()))
			{
				m_menuLane = static_cast<size_t>(std::stoul(attribute(*button, "lane-menu")));
			}
			else if (event.GetParameter("button", 0) == 1)
			{
				// A right click below a lane's name opens that lane's context menu (p. 1261), which the document's
				// popup behavior shows; its Remove This Lane takes the lane the click was in.
				if (const Rml::Element* lane = withAttribute(target, "lane", m_root.get()))
					m_menuLane = static_cast<size_t>(std::stoul(attribute(*lane, "lane")));
			}
			else if (event.GetParameter("button", 0) == 0)
			{
				press(event);
			}
			break;
		case Rml::EventId::Mousemove:
			drag(event);
			break;
		case Rml::EventId::Mouseup:
			if (event.GetParameter("button", 0) == 0)
				release(event);
			break;
		default:
			break;
		}
	}

	void ControllerLanes::menuItem(Rml::Element& menu, Rml::Element& item)
	{
		std::vector<Lane> lanes = m_lanes;
		const bool laneMenu = &menu == m_menus[0].get();
		if (item.HasAttribute("lane-type"))
		{
			Lane lane{attribute(item, "lane-type"), std::stoi(attribute(item, "lane-number"))};
			// A poly pressure lane takes the pitch of the first selected note (p. 1179).
			if (lane.type == "polyPressure")
			{
				lane.number = 60;
				const Json selection = m_view.value("selection", Json::array());
				if (!selection.empty())
					if (const Json* part = partJson(selection.at(0).at("part").get<uint64_t>()))
						for (const Json& note : part->at("notes"))
							if (note.at("id") == selection.at(0).at("note"))
								lane.number = note.at("pitch").get<int>();
			}
			if (laneMenu && m_menuLane && *m_menuLane < lanes.size())
				lanes[*m_menuLane] = lane;
			else
				lanes.push_back(lane);
			setLanes(lanes);
			return;
		}
		if (item.HasAttribute("lane-preset"))
		{
			for (const Json& preset : m_presets)
				if (preset.value("name", std::string()) == attribute(item, "lane-preset"))
				{
					lanes.clear();
					for (const Json& lane : preset.at("lanes"))
						lanes.push_back({lane.at("type").get<std::string>(), lane.value("number", 0)});
					setLanes(lanes);
				}
			return;
		}
		if (item.HasAttribute("remove-preset"))
		{
			Json presets = Json::array();
			for (const Json& preset : m_presets)
				if (preset.value("name", std::string()) != attribute(item, "remove-preset"))
					presets.push_back(preset);
			m_binding.send("settings.set", {{"key", "ui.controllerLanePresets"}, {"value", presets}});
			return;
		}
		const std::string action = attribute(item, "lane-action");
		if (action == "velocityOnly")
		{
			setLanes({{"velocity", 0}});
		}
		else if (action == "toggle")
		{
			setView({{"showLanes", !m_view.value("showLanes", true)}});
		}
		else if (action == "addPreset")
		{
			openPresetDialog();
		}
		else if (action == "showUsed")
		{
			// Every lane that has controller events, after the lanes shown (p. 1162). SysEx and directions (#175)
			// have no controller lane.
			for (const Part& part : m_parts)
				for (const Json& event : part.json.at("controllers"))
				{
					const std::string type = event.at("type").get<std::string>();
					if (type == "sysEx" || type == "articulation")
						continue;
					const Lane lane{
						type, type == "controller" || type == "polyPressure" ? event.value("number", 0) : 0};
					if (std::none_of(lanes.begin(), lanes.end(),
							[&](const Lane& each) { return each.type == lane.type && each.number == lane.number; }))
						lanes.push_back(lane);
				}
			setLanes(lanes);
		}
		// The lane menu and a lane's context menu act on the lane their menu opened for.
		const bool oneLane = laneMenu || (m_menus.size() > 3 && &menu == m_menus[3].get());
		if (!oneLane || !m_menuLane || *m_menuLane >= m_lanes.size())
			return;
		const Lane lane = m_lanes[*m_menuLane];
		if (action == "remove")
		{
			lanes.erase(lanes.begin() + static_cast<std::ptrdiff_t>(*m_menuLane));
			Json json = Json::array();
			for (const Lane& each : lanes)
				json.push_back(laneSpec(each));
			setView({{"lanes", json}});
		}
		else if (action == "selectAll")
		{
			Json selection = Json::array();
			const bool notes = isNoteLane(lane.type);
			for (const Item& each : items(lane))
				if (each.editable)
					selection.push_back({{"part", each.part}, {notes ? "note" : "controller", each.id}});
			setView({{notes ? "selection" : "controllerSelection", selection}});
		}
		else if (action == "applyProbability" || action == "applyVariance" || action == "resetProbability" ||
			action == "resetVariance")
		{
			// The selected notes, or all of the editable parts' (pp. 1171, 1173).
			const bool probability = action == "applyProbability" || action == "resetProbability";
			const bool selected = !m_view.value("selection", Json::array()).empty();
			Json commands = Json::array();
			for (const Part& part : m_parts)
			{
				if (!part.editable)
					continue;
				Json notes = Json::array();
				for (const Json& note : part.json.at("notes"))
					if (!selected || noteSelected(part.id, note.at("id").get<uint64_t>()))
						notes.push_back(note.at("id"));
				if (notes.empty())
					continue;
				if (action.rfind("apply", 0) == 0)
				{
					commands.push_back({{"command", "note.apply"},
						{"parameters",
							{{"part", part.id}, {"notes", notes},
								{"what", probability ? "probability" : "velocityVariance"}}}});
				}
				else
				{
					Json changes = Json::array();
					for (const Json& id : notes)
						changes.push_back(
							{{"id", id}, {probability ? "probability" : "velocityVariance", probability ? 1.0 : 0.0}});
					commands.push_back(
						{{"command", "note.set"}, {"parameters", {{"part", part.id}, {"notes", changes}}}});
				}
			}
			edit(probability ? "Play probability" : "Velocity variance", std::move(commands));
		}
	}

	// Add Preset (p. 1166): the dialog names the lanes shown as a preset.
	void ControllerLanes::openPresetDialog()
	{
		const Rml::Element* root = m_root.get();
		if (m_dialog || root == nullptr || root->GetContext() == nullptr)
			return;
		const std::string path = std::filesystem::u8path(root->GetOwnerDocument()->GetSourceURL())
									 .replace_filename("controller-lane-preset.rml")
									 .generic_u8string();
		Rml::ElementDocument* dialog = root->GetContext()->LoadDocument(path);
		if (dialog == nullptr)
		{
			logging::warning("The controller lanes can't open the dialog {}", path);
			return;
		}
		dialog->AddEventListener(Rml::EventId::Click, this);
		dialog->Show(Rml::ModalFlag::Modal);
		m_dialog = dialog->GetObserverPtr();
	}

	void ControllerLanes::dialogEvent(Rml::Event& event)
	{
		const Rml::Element* element = event.GetTargetElement();
		while (element != nullptr && !element->HasAttribute("result"))
			element = element->GetParentNode();
		if (element == nullptr || attribute(*element, "result") != "ok")
			return;
		auto* field =
			dynamic_cast<Rml::ElementFormControl*>(event.GetCurrentElement()->GetElementById("lane-preset-name"));
		const std::string name = field != nullptr ? field->GetValue() : std::string();
		if (name.empty())
			return;
		Json lanes = Json::array();
		for (const Lane& lane : m_lanes)
			lanes.push_back(laneSpec(lane));
		Json presets = Json::array();
		for (const Json& preset : m_presets)
			if (preset.value("name", std::string()) != name)
				presets.push_back(preset);
		presets.push_back({{"name", name}, {"lanes", lanes}});
		m_binding.send("settings.set", {{"key", "ui.controllerLanePresets"}, {"value", presets}});
	}

	void ControllerLanes::press(Rml::Event& event)
	{
		// The document's mousedown (ProcessEvent) has normally ended a gesture whose mouseup never came already; this
		// ends one that went on while the root moved to another document.
		cancel();
		begin(event);
		if (m_gesture != Gesture::None)
			listen(true);
	}

	void ControllerLanes::cancel()
	{
		listen(false);
		m_gesture = Gesture::None;
	}

	void ControllerLanes::listen(bool document)
	{
		if (Rml::Element* listened = m_document.get())
		{
			listened->RemoveEventListener(Rml::EventId::Mouseup, this);
			listened->RemoveEventListener(Rml::EventId::Mousedown, this, true);
		}
		m_document.reset();
		if (const Rml::Element* root = m_root.get(); document && root != nullptr)
			if (Rml::ElementDocument* owner = root->GetOwnerDocument())
			{
				owner->AddEventListener(Rml::EventId::Mouseup, this);
				// In the capture phase: before the press reaches the root or anything else in the document.
				owner->AddEventListener(Rml::EventId::Mousedown, this, true);
				m_document = owner->GetObserverPtr();
			}
	}

	void ControllerLanes::begin(Rml::Event& event)
	{
		const std::optional<size_t> lane = laneAt(event.GetTargetElement());
		if (!lane)
			return;
		m_lane = *lane;
		m_pressedAt = at(event, m_lane);
		m_snapOff = event.GetParameter("ctrl_key", 0) != 0 || event.GetParameter("meta_key", 0) != 0;
		const bool shift = event.GetParameter("shift_key", 0) != 0;
		const Lane& shown = m_lanes[m_lane];
		const bool notes = isNoteLane(shown.type);
		m_pressed.reset();
		if (const Rml::Element* element =
				withAttribute(event.GetTargetElement(), notes ? "note" : "controller", m_root.get()))
			for (const Item& item : items(shown))
				if (item.editable && item.part == idAttribute(*element, "part") &&
					item.id == idAttribute(*element, notes ? "note" : "controller"))
					m_pressed = item;
		if (notes && !m_pressed)
		{
			// Bars are thin: a press in a cell of the quantize grid takes the note that starts there nearest to it.
			const int64_t from = cell(m_pressedAt.first, grid());
			for (const Item& item : items(shown))
				if (item.editable && item.start >= from && item.start < from + grid() &&
					(!m_pressed ||
						std::abs(static_cast<double>(item.start) - m_pressedAt.first) <
							std::abs(static_cast<double>(m_pressed->start) - m_pressedAt.first)))
					m_pressed = item;
		}
		m_painted.clear();
		if (m_tool == "objectSelection")
		{
			if (!m_pressed)
			{
				if (!notes && !shift && !m_snapOff)
					setView({{"controllerSelection", Json::array()}});
				return;
			}
			Item& pressed = *m_pressed;
			if (!notes && (shift || m_snapOff || !pressed.selected))
			{
				// A click selects the event, Shift or Ctrl (Cmd) and a click adds or removes it.
				Json selection = Json::array();
				if (shift || m_snapOff)
					for (const Json& each : m_view.value("controllerSelection", Json::array()))
						if (each.at("part") != pressed.part || each.at("controller") != pressed.id)
							selection.push_back(each);
				if (!(shift || m_snapOff) || !pressed.selected)
					selection.push_back({{"part", pressed.part}, {"controller", pressed.id}});
				setView({{"controllerSelection", selection}});
				if (shift || m_snapOff)
					return;
				pressed.selected = true;
			}
			m_gesture = Gesture::Value;
		}
		else if (m_tool == "draw" || (m_tool == "line" && m_lineMode == "paint"))
		{
			m_gesture = Gesture::Paint;
			m_painted.emplace_back(std::llround(m_pressedAt.first), m_pressedAt.second);
		}
		else if (m_tool == "line")
		{
			m_gesture = Gesture::Line;
		}
		else if (m_tool == "erase" && m_pressed && !notes)
		{
			edit("Erase",
				Json::array({{{"command", "controller.remove"},
					{"parameters", {{"part", m_pressed->part}, {"controllers", Json::array({m_pressed->id})}}}}}));
		}
	}

	void ControllerLanes::drag(Rml::Event& event)
	{
		if (m_gesture == Gesture::Paint)
		{
			const auto [ticks, value] = at(event, m_lane);
			m_painted.emplace_back(std::llround(ticks), value);
		}
	}

	void ControllerLanes::release(Rml::Event& event)
	{
		listen(false);
		const Gesture gesture = std::exchange(m_gesture, Gesture::None);
		if (gesture == Gesture::None || m_lane >= m_lanes.size())
			return;
		const std::pair<double, double> end = at(event, m_lane);
		if (gesture == Gesture::Value)
		{
			finishValue(m_lane, end.second);
		}
		else if (gesture == Gesture::Paint)
		{
			m_painted.emplace_back(std::llround(end.first), end.second);
			finishPaint(m_lane);
		}
		else
		{
			finishLine(m_lane, end, m_snapOff);
		}
	}

	// A drag of a value moves the pressed item's value, and with a selected one the values of all selected ones
	// of the lane, by as much as the mouse went.
	void ControllerLanes::finishValue(size_t lane, double value)
	{
		if (!m_pressed || value == m_pressedAt.second)
			return;
		const Lane& shown = m_lanes[lane];
		const double delta = value - m_pressedAt.second;
		std::map<uint64_t, Json> changes;
		for (const Item& item : items(shown))
			if (item.editable &&
				((item.part == m_pressed->part && item.id == m_pressed->id) || (m_pressed->selected && item.selected)))
				changes[item.part].push_back(isNoteLane(shown.type)
						? noteChange(shown, item.id, item.value + delta)
						: controllerChange(shown, item.id, item.value + delta));
		Json commands = Json::array();
		for (auto& [part, items] : changes)
			commands.push_back({{"command", isNoteLane(shown.type) ? "note.set" : "controller.set"},
				{"parameters", {{"part", part}, {isNoteLane(shown.type) ? "notes" : "controllers", items}}}});
		edit("Change values", std::move(commands));
	}

	// Painting (p. 1180): on a note lane the notes from the first point to the last take the value painted nearest
	// to them, and a click on a bar its value; on a controller lane an event at every step of the length quantize
	// painted over, replacing the lane's events there.
	void ControllerLanes::finishPaint(size_t lane)
	{
		const Lane& shown = m_lanes[lane];
		std::stable_sort(
			m_painted.begin(), m_painted.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		// The value painted at a position: between two points on the line from one to the other.
		const auto valueAt = [this](int64_t ticks)
		{
			const auto next = std::lower_bound(m_painted.begin(), m_painted.end(), ticks,
				[](const auto& point, int64_t at) { return point.first < at; });
			if (next == m_painted.begin())
				return next->second;
			if (next == m_painted.end())
				return m_painted.back().second;
			const auto before = std::prev(next);
			if (next->first == before->first)
				return next->second;
			const double t =
				static_cast<double>(ticks - before->first) / static_cast<double>(next->first - before->first);
			return before->second + ((next->second - before->second) * t);
		};
		Json commands = Json::array();
		if (isNoteLane(shown.type))
		{
			// The cells of the quantize grid painted over, at least the one clicked.
			const int64_t from = cell(static_cast<double>(m_painted.front().first), grid());
			const int64_t to = std::max(m_painted.back().first, from + grid() - 1);
			std::map<uint64_t, Json> changes;
			for (const Item& item : items(shown))
				if (item.editable &&
					((m_pressed && item.part == m_pressed->part && item.id == m_pressed->id) ||
						(item.start >= from && item.start <= to)))
					changes[item.part].push_back(noteChange(shown, item.id, valueAt(item.start)));
			for (auto& [part, items] : changes)
				commands.push_back({{"command", "note.set"}, {"parameters", {{"part", part}, {"notes", items}}}});
			edit("Draw values", std::move(commands));
			return;
		}
		// The part where the paint started, wherever it ended.
		const Part* part = partAt(std::llround(m_pressedAt.first));
		if (part == nullptr)
			return;
		// An event at every step from the first point to the last, or only where the mouse went with snap off.
		std::map<int64_t, double> events; // project ticks
		if (m_snapOff)
		{
			for (const auto& [ticks, value] : m_painted)
				events[ticks] = value;
		}
		else
		{
			const int64_t step = lengthQuantize();
			for (int64_t at = cell(static_cast<double>(m_painted.front().first), step); at <= m_painted.back().first;
				at += step)
				events[at] = valueAt(std::max(at, m_painted.front().first));
		}
		for (auto event = events.begin(); event != events.end();)
			event = event->first >= part->start && event->first < part->end ? std::next(event) : events.erase(event);
		if (events.empty())
			return;
		const int channel = laneChannel(shown, *part, events.begin()->first, events.rbegin()->first);
		Json removed = Json::array();
		for (const Item& item : items(shown))
			if (item.part == part->id && item.start >= events.begin()->first && item.start <= events.rbegin()->first)
				removed.push_back(item.id);
		if (!removed.empty())
			commands.push_back(
				{{"command", "controller.remove"}, {"parameters", {{"part", part->id}, {"controllers", removed}}}});
		Json added = Json::array();
		for (const auto& [ticks, value] : events)
		{
			Json event = {{"position", ticks - part->start}, {"type", shown.type}, {"channel", channel}};
			if (shown.type == "program")
				event["number"] = std::lround(std::clamp(value, 0.0, 1.0) * 127.0);
			else
				event["value"] = std::clamp(value, 0.0, 1.0);
			if (shown.type == "controller" || shown.type == "polyPressure")
				event["number"] = shown.number;
			added.push_back(event);
		}
		commands.push_back(
			{{"command", "controller.add"}, {"parameters", {{"part", part->id}, {"controllers", added}}}});
		edit("Draw controllers", std::move(commands));
	}

	// The line tool (p. 1186): controller.line in its mode from where the drag started to where it ended, in the
	// part where it started; the quantize grid is a period, the length quantize the density of the events. On a
	// note lane it changes the selected notes of the part, or all of them without a selection there. On a controller
	// lane it replaces the events of every channel in the range, as painting does, and draws on the channel of the
	// lane's events there (laneChannel).
	void ControllerLanes::finishLine(size_t lane, const std::pair<double, double>& end, bool snapOff)
	{
		const Lane& shown = m_lanes[lane];
		std::pair<int64_t, double> first{snap(m_pressedAt.first, snapOff), m_pressedAt.second};
		std::pair<int64_t, double> last{snap(end.first, snapOff), end.second};
		const Part* part = partAt(first.first);
		if (part == nullptr)
			return;
		if (last.first < first.first)
			std::swap(first, last);
		first.first = std::max(first.first, part->start);
		last.first = std::clamp(last.first, first.first, part->end - 1);
		Json spec = laneSpec(shown);
		Json commands = Json::array();
		if (!isNoteLane(shown.type))
		{
			const int channel = laneChannel(shown, *part, first.first, last.first);
			spec["channel"] = channel;
			Json others = Json::array();
			for (const Item& item : items(shown))
				if (item.part == part->id && item.channel != channel && item.start >= first.first &&
					item.start <= last.first)
					others.push_back(item.id);
			if (!others.empty())
				commands.push_back(
					{{"command", "controller.remove"}, {"parameters", {{"part", part->id}, {"controllers", others}}}});
		}
		Json parameters = {{"part", part->id}, {"lane", spec}, {"mode", m_lineMode},
			{"from", {{"position", first.first - part->start}, {"value", fieldValue(shown.type, first.second)}}},
			{"to", {{"position", last.first - part->start}, {"value", fieldValue(shown.type, last.second)}}},
			{"density", lengthQuantize()}, {"period", grid()}};
		if (isNoteLane(shown.type))
		{
			Json notes = Json::array();
			for (const Item& item : items(shown))
				if (item.part == part->id && item.selected)
					notes.push_back(item.id);
			if (!notes.empty())
				parameters["notes"] = notes;
		}
		commands.push_back({{"command", "controller.line"}, {"parameters", std::move(parameters)}});
		edit("Draw line", std::move(commands));
	}
}
