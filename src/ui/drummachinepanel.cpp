// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/drummachinepanel.h"

#include "ui/rmlui.h"
#include "ui/widgetutils.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <utility>

namespace gnubase::ui
{
	using engine::Json;

	namespace
	{
		constexpr std::array<Rml::EventId, 4> g_events = {
			Rml::EventId::Click, Rml::EventId::Change, Rml::EventId::Mousedown, Rml::EventId::Mouseup};

		constexpr int g_pads = 128;
		constexpr int g_padsPerGroup = 16;
		constexpr uint32_t g_padStride = 32; // processors::g_drumPadStride
		constexpr uint32_t g_globals = 4096; // the first global parameter's id
		// The pad parameters that show the pad's state.
		constexpr uint32_t g_instrument = 0;
		constexpr uint32_t g_mute = 3;
		constexpr uint32_t g_solo = 4;

		// The editing pages (p. 943) and the global parameters each one shows, by processor.list's names.
		struct Page
		{
			const char* id;
			const char* title;
			std::vector<const char*> parameters; // the Instrument page shows the pad's
		};

		const std::vector<Page>& pages()
		{
			static const std::vector<Page> list = {
				{"instrument", "Instrument", {}},
				{"delay", "Delay",
					{"Delay", "Delay time", "Delay sync", "Delay note", "Delay feedback", "Delay ping-pong",
						"Delay to reverb", "Delay level"}},
				{"reverb", "Reverb",
					{"Reverb", "Reverb predelay", "Reverb room size", "Reverb time", "Reverb damping", "Reverb level"}},
			};
			return list;
		}

		std::string encode(const std::string& text)
		{
			return Rml::StringUtilities::EncodeRml(text);
		}

		void setClass(Rml::Element* element, const char* name, bool set)
		{
			if (element != nullptr && element->IsClassSet(name) != set)
				element->SetClass(name, set);
		}

		void setText(Rml::Element* element, const std::string& text)
		{
			const std::string rml = encode(text);
			if (element != nullptr && element->GetInnerRML() != rml)
				element->SetInnerRML(rml);
		}

		void setChecked(Rml::Element* element, bool set)
		{
			if (element == nullptr || element->HasAttribute("checked") == set)
				return;
			if (set)
				element->SetAttribute("checked", "");
			else
				element->RemoveAttribute("checked");
		}

		Rml::Element* withAttribute(Rml::Element* element, const char* name)
		{
			for (; element != nullptr; element = element->GetParentNode())
				if (element->HasAttribute(name))
					return element;
			return nullptr;
		}

		const Json* findIn(const Json& project, const char* list, const Json& id)
		{
			const auto items = project.find(list);
			if (items == project.end() || !id.is_number_unsigned() || id.get<uint64_t>() == 0)
				return nullptr;
			for (const Json& item : *items)
				if (item.at("id") == id)
					return &item;
			return nullptr;
		}

		// A pad's parameter name without "Pad 0 ", capitalized: "Level".
		std::string label(const std::string& name)
		{
			std::string text = name.rfind("Pad 0 ", 0) == 0 ? name.substr(6) : name;
			if (!text.empty())
				text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
			return text;
		}
	}

	DrumMachinePanel::DrumMachinePanel(EngineBinding& binding, Rml::Element& element,
		std::function<void(uint64_t track, int pad)> loadSample, std::function<void()> addTrack,
		std::function<void(uint64_t track, const std::string& mode)> preset)
		: m_binding(binding), m_loadSample(std::move(loadSample)), m_addTrack(std::move(addTrack)),
		  m_preset(std::move(preset)), m_stepOwner(binding.stepOwner("drumMachine")), m_root(element.GetObserverPtr())
	{
		std::string groups;
		for (int group = 0; group < g_pads / g_padsPerGroup; ++group)
			groups += R"(<button class="drum-group" group=")" + std::to_string(group) + R"(">)" +
				std::to_string(group + 1) + "</button>";
		std::string tabs;
		for (const Page& page : pages())
			tabs += std::string(R"(<button class="drum-page-tab" page=")") + page.id + R"(" id="drum-page-)" + page.id +
				R"(">)" + page.title + "</button>";
		std::string grid;
		for (int i = 0; i < g_padsPerGroup; ++i)
			grid += R"(<div class="drum-pad"><span class="drum-pad-name"/><span class="drum-pad-note"/>)"
					R"(<span class="drum-pad-color"/></div>)";
		element.SetInnerRML(R"(<div class="drum-machine empty" id="drum-machine">)"
							R"(<div class="drum-hint"><div>Select a drum track</div>)"
							R"(<button id="drum-add-track">Add Drum Track</button></div>)"
							R"(<div class="drum-toolbar">)" +
			tabs +
			R"(<button id="drum-bypass" type="toggle" title="Bypass All Effects">Bypass FX</button>)"
			R"(<button id="drum-load-preset" title="Load Track Preset">Load Preset</button>)"
			R"(<button id="drum-save-preset" title="Save Track Preset">Save Preset</button>)"
			R"(<input type="text" id="drum-pad-name" class="drum-pad-field" title="Rename Pad"/>)"
			R"(<select id="drum-pad-color" title="Set Color"/>)"
			R"(<button id="drum-load" title="Load Sample">Load</button>)"
			R"(<button id="drum-copy" title="Copy Pad">Copy</button>)"
			R"(<button id="drum-cut" title="Cut Pad">Cut</button>)"
			R"(<button id="drum-paste" title="Paste Pad">Paste</button>)"
			R"(<button id="drum-reset" title="Reset Pad">Reset</button>)"
			R"(<span id="drum-message" class="drum-message"/></div>)"
			R"(<div class="drum-body"><div class="drum-pads"><div class="drum-groups">)" +
			groups + R"(</div><div class="drum-grid" id="drum-grid">)" + grid +
			R"(</div></div><div class="drum-page" id="drum-page-content"/></div></div>)");
		for (const Rml::EventId id : g_events)
			element.AddEventListener(id, this);
		element.AddEventListener(Rml::EventId::Blur, this, true); // blur doesn't bubble
		element.AddEventListener(m_dropEvent, this);
		m_binding.send("processor.list", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (!alive.expired() && result.ok())
					build(result.value);
			});
		show();
	}

	DrumMachinePanel::~DrumMachinePanel()
	{
		if (Rml::Element* root = m_root.get())
		{
			for (const Rml::EventId id : g_events)
				root->RemoveEventListener(id, this);
			root->RemoveEventListener(Rml::EventId::Blur, this, true);
			root->RemoveEventListener(m_dropEvent, this);
		}
	}

	// Ends the note of the pad that sounds and plays pad, unless it is negative: into the track's channel as if from
	// its MIDI input (midi.inject), as the drum editor auditions sounds, so that a drum map's pitches play their pads.
	// Without an audio device nothing sounds, which is no error to report.
	void DrumMachinePanel::sound(int pad)
	{
		const auto inject = [this](uint64_t channel, int note, bool on)
		{
			m_binding.send("midi.inject",
				{{"channel", channel}, {"drumSounds", true},
					{"messages", Json::array({{{"position", 0}, {"bytes", {on ? 0x90 : 0x80, note, on ? 127 : 0}}}})}},
				[](const engine::Result&) {});
		};
		if (const auto sounding = std::exchange(m_sounding, std::nullopt))
			inject(sounding->first, sounding->second, false);
		const Json channel = m_channel.is_object() ? m_channel.value("id", Json()) : Json();
		if (pad < 0 || !channel.is_number_unsigned())
			return;
		m_sounding = std::make_pair(channel.get<uint64_t>(), pad);
		inject(m_sounding->first, pad, true);
	}

	Rml::Element* DrumMachinePanel::find(const std::string& id) const
	{
		Rml::Element* root = m_root.get();
		return root == nullptr ? nullptr : root->QuerySelector("#" + id);
	}

	void DrumMachinePanel::build(const Json& processors)
	{
		for (const Json& processor : processors.at("processors"))
		{
			if (processor.at("id") != "drumMachine")
				continue;
			m_padNames = processor.value("padNames", std::vector<std::string>());
			for (const Json& item : processor.at("parameters"))
			{
				const auto id = item.at("id").get<uint32_t>();
				if (id >= g_padStride && id < g_globals)
					continue; // the other pads' are pad 0's
				Parameter parameter{id, item.at("name").get<std::string>(), item.at("minimum").get<double>(),
					item.at("maximum").get<double>(), item.at("default").get<double>(),
					item.value("unit", std::string()), item.value("stepped", false),
					item.value("options", std::vector<std::string>())};
				(id < g_globals ? m_padParameters : m_globalParameters).push_back(std::move(parameter));
			}
		}
		buildPage();
	}

	uint32_t DrumMachinePanel::idOf(const Parameter& parameter) const
	{
		return parameter.id < g_globals ? (static_cast<uint32_t>(m_pad) * g_padStride) + parameter.id : parameter.id;
	}

	// The page's controls: a toggle for the parameters that are on or off, a drop-down list for those with options,
	// a value field for the others.
	void DrumMachinePanel::buildPage()
	{
		Rml::Element* page = find("drum-page-content");
		if (page == nullptr)
			return;
		std::vector<const Parameter*> shown;
		if (m_page == "instrument")
			for (const Parameter& each : m_padParameters)
				shown.push_back(&each);
		for (const Page& each : pages())
			if (m_page == each.id)
				for (const char* name : each.parameters)
					for (const Parameter& global : m_globalParameters)
						if (global.name == name)
							shown.push_back(&global);
		std::string rml;
		for (const Parameter* found : shown)
		{
			const std::string id = std::to_string(idOf(*found));
			std::string attributes = R"( id="drum-parameter-)";
			attributes.append(id).append(R"(" parameter=")").append(id);
			attributes.append(R"(" title=")").append(encode(label(found->name))).append("\"");
			const std::string text = R"(<span class="drum-label">)" + encode(label(found->name)) + "</span>";
			rml += R"(<div class="drum-control">)";
			if (found->options == std::vector<std::string>{"Off", "On"})
			{
				rml.append(R"(<button type="toggle")").append(attributes).append(">");
				rml.append(encode(label(found->name))).append("</button>");
			}
			else if (!found->options.empty())
			{
				rml.append(text).append("<select").append(attributes).append("/>");
			}
			else
			{
				std::string decimals = found->maximum <= 1.0 ? "3" : "1";
				if (found->stepped)
					decimals = R"(0" step="1)";
				rml.append(text).append("<valuefield").append(attributes);
				rml.append(R"( min=")").append(widgets::format(static_cast<float>(found->minimum)));
				rml.append(R"(" max=")").append(widgets::format(static_cast<float>(found->maximum)));
				rml.append(R"(" default=")").append(widgets::format(static_cast<float>(found->defaultValue)));
				rml.append(R"(" decimals=")").append(decimals);
				rml.append(R"(" unit=")").append(encode(found->unit)).append(R"("/>)");
			}
			rml += "</div>";
		}
		page->SetInnerRML(rml);
		show();
	}

	// The pad's own name, its sample's, or the engine's for its note or for the note of the sound it goes by (a copy's,
	// #1567), as pattern lanes have them (drumPadName).
	std::string DrumMachinePanel::padName(int pad) const
	{
		if (const auto name = m_names.find(pad); name != m_names.end() && !name->second.empty())
			return name->second;
		if (const auto sound = m_sounds.find(pad); sound != m_sounds.end())
			pad = sound->second;
		return pad >= 0 && static_cast<size_t>(pad) < m_padNames.size() ? m_padNames[static_cast<size_t>(pad)]
																		: std::string();
	}

	double DrumMachinePanel::value(uint32_t id) const
	{
		// A parameter whose change is on its way keeps what the user gave it, not the engine's older value, also after
		// another drum track showed meanwhile (#1471).
		if (const Json* shown = m_channel.is_object() ? m_binding.shown(stepKey(id), m_read) : nullptr)
			return shown->get<double>();
		if (const auto set = m_values.find(id); set != m_values.end())
			return set->second;
		const uint32_t base = id < g_globals ? id % g_padStride : id;
		for (const auto* list : {&m_padParameters, &m_globalParameters})
			for (const Parameter& each : *list)
				if (each.id == base)
					return each.defaultValue;
		return 0.0;
	}

	std::string DrumMachinePanel::stepKey(uint32_t id) const
	{
		return m_stepOwner + "channel " + m_channel.value("id", Json()).dump() + " instrument " + std::to_string(id);
	}

	void DrumMachinePanel::update(uint64_t track, const Json& project, uint64_t loads, uint64_t read)
	{
		m_read = read;
		if (track == m_selected && loads == m_loads)
			return;
		m_selected = track;
		m_loads = loads;
		const Json* found = findIn(project, "tracks", track);
		const Json* channel = found == nullptr ? nullptr : findIn(project, "channels", found->value("channel", Json()));
		if (channel != nullptr && channel->value("instrument", std::string()) != "drumMachine")
			channel = nullptr;
		if ((channel != nullptr ? track : 0) != m_track)
		{
			showMessage(""); // what failed concerned the track shown before
			m_copied = -1;
		}
		m_track = channel != nullptr ? track : 0;
		m_channel = channel != nullptr ? *channel : Json();
		std::map<uint32_t, double> values;
		std::map<int, std::string> names;
		std::map<int, int> sounds;
		std::map<int, std::string> colors;
		if (channel != nullptr)
		{
			for (const Json& item : channel->value("instrumentParameters", Json::array()))
				values[item.at("parameter").get<uint32_t>()] = item.at("value").get<double>();
			for (const Json& pad : channel->value("pads", Json::array()))
			{
				std::string name = pad.value("name", std::string());
				if (const Json* clip = findIn(project, "clips", pad.value("sample", Json()));
					name.empty() && clip != nullptr)
					name = clip->value("name", std::string());
				names[pad.value("pad", 0)] = name;
				if (const int sound = pad.value("sound", -1); sound >= 0)
					sounds[pad.value("pad", 0)] = sound;
				if (std::string color = pad.value("color", std::string()); !color.empty())
					colors[pad.value("pad", 0)] = std::move(color);
			}
		}
		m_values = std::move(values);
		m_names = std::move(names);
		m_sounds = std::move(sounds);
		m_colors = std::move(colors);
		// Set Color offers the project's color set (pp. 942-943), as the project window's color menu does.
		m_colorSet = {{"", "No Color"}};
		for (const Json& color : project.value("colors", Json::array()))
			m_colorSet.emplace_back(color.value("color", std::string()), color.value("name", std::string()));
		show();
	}

	void DrumMachinePanel::show()
	{
		Rml::Element* root = find("drum-machine");
		if (root == nullptr)
			return;
		setClass(root, "empty", m_track == 0);
		m_showing = true;
		// The pads of the group shown, four rows of four, the lowest note at the bottom left (p. 942).
		if (const Rml::Element* grid = find("drum-grid"))
		{
			bool solo = false;
			for (int pad = 0; pad < g_pads && !solo; ++pad)
				solo = value((static_cast<uint32_t>(pad) * g_padStride) + g_solo) >= 0.5;
			for (int i = 0; i < grid->GetNumChildren() && i < g_padsPerGroup; ++i)
			{
				const int pad = (m_group * g_padsPerGroup) + ((3 - (i / 4)) * 4) + (i % 4);
				Rml::Element* element = grid->GetChild(i);
				const std::string number = std::to_string(pad);
				if (element->GetAttribute<Rml::String>("pad", "") != number)
					element->SetAttribute("pad", number);
				const auto parameter = [pad, this](uint32_t id)
				{ return value((static_cast<uint32_t>(pad) * g_padStride) + id); };
				setText(element->GetChild(0), padName(pad));
				setText(element->GetChild(1), number);
				setClass(element, "assigned", parameter(g_instrument) >= 0.5);
				setClass(element, "selected", pad == m_pad);
				setClass(element, "muted", parameter(g_mute) >= 0.5);
				setClass(element, "soloed", parameter(g_solo) >= 0.5);
				setClass(element, "soloed-out", solo && parameter(g_solo) < 0.5);
				// The pad's color, which is user data, in its strip.
				const auto color = m_colors.find(pad);
				const std::string shown = color != m_colors.end() ? color->second : std::string();
				if (Rml::Element* strip = element->GetChild(2);
					strip != nullptr && strip->GetAttribute<Rml::String>("color", "") != shown)
				{
					strip->SetAttribute("color", shown);
					if (shown.empty())
						strip->RemoveProperty("background-color");
					else
						strip->SetProperty("background-color", shown);
				}
				setClass(element, "colored", !shown.empty());
			}
		}
		Rml::ElementList groups;
		root->QuerySelectorAll(groups, ".drum-group");
		for (Rml::Element* group : groups)
			setClass(group, "selected", group->GetAttribute<int>("group", -1) == m_group);
		Rml::ElementList tabs;
		root->QuerySelectorAll(tabs, ".drum-page-tab");
		for (Rml::Element* tab : tabs)
			setClass(tab, "selected", tab->GetAttribute<Rml::String>("page", "") == m_page);
		if (Rml::Element* field = find("drum-pad-name"))
		{
			const std::string text = padName(m_pad);
			if (field->GetAttribute<Rml::String>("value", "") != text && !field->IsPseudoClassSet("focus"))
				field->SetAttribute("value", text);
		}
		if (Rml::Element* select = find("drum-pad-color"))
		{
			const auto color = m_colors.find(m_pad);
			widgets::fill(*select, m_colorSet, color != m_colors.end() ? color->second : std::string());
		}
		setChecked(find("drum-bypass"), value(g_globals + 1) >= 0.5);
		if (Rml::Element* paste = find("drum-paste");
			paste != nullptr && paste->HasAttribute("disabled") != (m_copied < 0))
		{
			if (m_copied < 0)
				paste->SetAttribute("disabled", "");
			else
				paste->RemoveAttribute("disabled");
		}
		if (Rml::Element* page = find("drum-page-content"))
		{
			Rml::ElementList controls;
			page->QuerySelectorAll(controls, "[parameter]");
			for (Rml::Element* control : controls)
			{
				const auto id = static_cast<uint32_t>(control->GetAttribute<int>("parameter", 0));
				const uint32_t base = id < g_globals ? id % g_padStride : id;
				const Parameter* each = nullptr;
				for (const auto* list : {&m_padParameters, &m_globalParameters})
					for (const Parameter& candidate : *list)
						if (candidate.id == base)
							each = &candidate;
				if (each == nullptr)
					continue;
				const double shown = value(id);
				if (each->options == std::vector<std::string>{"Off", "On"})
				{
					setChecked(control, shown >= 0.5);
				}
				else if (!each->options.empty())
				{
					widgets::Options options;
					for (size_t i = 0; i < each->options.size(); ++i)
						options.emplace_back(std::to_string(i), each->options[i]);
					widgets::fill(*control, options, std::to_string(std::lround(shown)));
				}
				else if (std::abs(widgets::number(*control, "value", static_cast<float>(shown) + 1.0F) - shown) > 1e-6)
				{
					control->SetAttribute("value", widgets::format(static_cast<float>(shown)));
				}
			}
		}
		m_showing = false;
	}

	void DrumMachinePanel::set(uint32_t id, double value, int gesture)
	{
		if (m_channel.is_null())
			return;
		const Json step = {{"command", "parameter.set"},
			{"parameters",
				{{"channel", m_channel.at("id")}, {"target", {{"type", "instrument"}, {"parameter", id}}},
					{"value", value}}}};
		m_binding.sendStep(gesture, stepKey(id), "Change Drum Machine", Json::array({step}), {}, value);
	}

	void DrumMachinePanel::showMessage(const std::string& text)
	{
		Rml::Element* message = find("drum-message");
		setText(message, text);
		setClass(message, "error", !text.empty());
		if (message != nullptr)
			message->SetAttribute("title", text);
	}

	void DrumMachinePanel::load(uint64_t track, int pad, const std::string& path)
	{
		m_binding.send("drum.loadPad", {{"track", track}, {"pad", pad}, {"path", path}},
			[this, alive = std::weak_ptr<bool>(m_alive), track](const engine::Result& result)
			{
				if (!alive.expired() && track == m_track)
					showMessage(result.ok() ? "" : "Loading the sample failed: " + result.error);
			});
	}

	void DrumMachinePanel::ProcessEvent(Rml::Event& event)
	{
		Rml::Element* target = event.GetTargetElement();
		if (target != nullptr && target->GetId() == "drum-pad-name")
		{
			// A new name goes out with Enter or when the field loses the focus (Rename Pad, p. 943).
			const bool done = event == Rml::EventId::Blur ||
				(event == Rml::EventId::Change && event.GetParameter<bool>("linebreak", false));
			const auto text = target->GetAttribute<Rml::String>("value", "");
			if (done && m_track != 0 && text != padName(m_pad))
				m_binding.send("drum.setPad", {{"track", m_track}, {"pad", m_pad}, {"name", text}});
			return;
		}
		switch (event.GetId())
		{
		case Rml::EventId::Mousedown:
		{
			// A pad plays while the mouse button is down on it, at velocity 127 (reference manual p. 942).
			const Rml::Element* pad = target != nullptr ? withAttribute(target, "pad") : nullptr;
			if (pad != nullptr && m_track != 0 && event.GetParameter<int>("button", 0) == 0)
				sound(std::clamp(pad->GetAttribute<int>("pad", 0), 0, g_pads - 1));
			return;
		}
		case Rml::EventId::Mouseup:
			// ponytail: a button released outside the panel ends the note with the next press there.
			sound(-1);
			return;
		case Rml::EventId::Click:
		{
			if (target != nullptr && target->GetId() == "drum-add-track" && m_addTrack)
			{
				m_addTrack(); // Add Drum Track (p. 146)
				return;
			}
			if (target == nullptr || m_track == 0)
				return;
			const std::string id = target->GetId();
			if (const Rml::Element* pad = withAttribute(target, "pad"))
			{
				m_pad = std::clamp(pad->GetAttribute<int>("pad", m_pad), 0, g_pads - 1);
				buildPage();
			}
			else if (const Rml::Element* group = withAttribute(target, "group"))
			{
				m_group = std::clamp(group->GetAttribute<int>("group", m_group), 0, (g_pads / g_padsPerGroup) - 1);
				show();
			}
			else if (const Rml::Element* page = withAttribute(target, "page"))
			{
				m_page = page->GetAttribute<Rml::String>("page", "instrument");
				buildPage();
			}
			else if (id == "drum-load" && m_loadSample)
			{
				m_loadSample(m_track, m_pad);
			}
			else if ((id == "drum-load-preset" || id == "drum-save-preset") && m_preset)
			{
				m_preset(m_track, id == "drum-load-preset" ? "load" : "save");
			}
			else if (id == "drum-copy" || id == "drum-cut")
			{
				m_copied = m_pad;
				m_cut = id == "drum-cut";
				show();
			}
			else if (id == "drum-paste" && m_copied >= 0)
			{
				m_binding.send("drum.copyPad", {{"track", m_track}, {"pad", m_copied}, {"to", m_pad}, {"move", m_cut}});
				if (m_cut)
					m_copied = -1; // a cut pad pastes once
				show();
			}
			else if (id == "drum-reset")
			{
				m_binding.send("drum.resetPad", {{"track", m_track}, {"pad", m_pad}});
			}
			return;
		}
		case Rml::EventId::Change:
		{
			if (m_showing || target == nullptr)
				return;
			if (target->GetId() == "drum-bypass")
			{
				set(g_globals + 1, event.GetParameter<bool>("value", false) ? 1.0 : 0.0, 0);
				return;
			}
			if (target->GetId() == "drum-pad-color")
			{
				// Set Color (pp. 942-943): a color of the project's set, or none.
				const auto color = m_colors.find(m_pad);
				const auto chosen = event.GetParameter<Rml::String>("value", "");
				if (m_track != 0 && chosen != (color != m_colors.end() ? color->second : std::string()))
					m_binding.send("drum.setPad", {{"track", m_track}, {"pad", m_pad}, {"color", chosen}});
				return;
			}
			const Rml::Element* control = withAttribute(target, "parameter");
			if (control == nullptr || control != target)
				return;
			const auto id = static_cast<uint32_t>(control->GetAttribute<int>("parameter", 0));
			double changed = 0.0;
			if (control->GetTagName() == "button")
				changed = event.GetParameter<bool>("value", false) ? 1.0 : 0.0;
			else if (control->GetTagName() == "select")
				changed = std::strtod(event.GetParameter<Rml::String>("value", "0").c_str(), nullptr);
			else
				changed = event.GetParameter<float>("value", 0.0F);
			set(id, changed, control->GetTagName() == "valuefield" ? event.GetParameter<int>("gesture", 0) : 0);
			show();
			return;
		}
		default:
			break;
		}
		if (event.GetType() != g_fileDropEvent || m_track == 0)
			return;
		// A file dropped on a pad loads onto it, elsewhere onto the selected pad (p. 944), the first of several.
		std::istringstream lines(event.GetParameter<Rml::String>("files", ""));
		std::string file;
		std::getline(lines, file);
		if (file.empty())
			return;
		int pad = m_pad;
		if (const Rml::Element* element = withAttribute(target, "pad"))
			pad = std::clamp(element->GetAttribute<int>("pad", m_pad), 0, g_pads - 1);
		load(m_track, pad, file);
	}
}
