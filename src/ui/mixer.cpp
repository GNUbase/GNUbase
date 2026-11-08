// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/mixer.h"

#include "ui/docking.h"
#include "ui/widgetutils.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <iomanip>
#include <locale>
#include <optional>
#include <sstream>
#include <utility>

namespace gnubase::ui
{
	using engine::Json;

	namespace
	{
		// The channel type filter's types (reference manual p. 454), as mixer.set names them, and their labels.
		constexpr std::array<std::pair<const char*, const char*>, 8> g_types = {{
			{"input", "Input Channels"},
			{"audio", "Audio Channels"},
			{"instrument", "Instrument Channels"},
			{"midi", "MIDI Channels"},
			{"group", "Group Channels"},
			{"effect", "Effect Channels"},
			{"vca", "VCA Channels"},
			{"output", "Output Channels"},
		}};

		constexpr double g_silence = -144.0;       // engine::g_minChannelVolume
		constexpr double g_sendKnobMin = -60.0;    // a send level knob's lowest level, as the fader's
		constexpr int g_sends = 8;                 // engine::g_maxSends
		constexpr const char* g_addEffect = "add"; // a send destination list's Add Effect Track to Send

		std::string encode(const std::string& text)
		{
			return Rml::StringUtilities::EncodeRml(text);
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

		// The element or its closest parent with the class, up to stop.
		Rml::Element* closest(Rml::Element* element, const char* name, const Rml::Element* stop)
		{
			for (; element != nullptr && element != stop; element = element->GetParentNode())
				if (element->IsClassSet(name))
					return element;
			return nullptr;
		}

		std::string trackType(const std::string& type)
		{
			return type == "folder" ? "group" : type;
		}

		// The fader's value as the strip shows it, e.g. "-6.0", or "-inf" for silence.
		std::string decibels(double volume)
		{
			if (volume <= g_silence)
				return "-inf";
			std::ostringstream text;
			text.imbue(std::locale::classic());
			text << std::fixed << std::setprecision(1) << volume;
			return text.str();
		}

		void setText(Rml::Element& element, const std::string& text)
		{
			const std::string rml = encode(text);
			if (element.GetInnerRML() != rml)
				element.SetInnerRML(rml);
		}

		// The dB text under a fader: what the fader shows, which is the drag's during a drag.
		void showVolume(const Rml::Element& fader)
		{
			Rml::Element* parent = fader.GetParentNode();
			if (Rml::Element* text = parent != nullptr ? parent->QuerySelector(".mixer-volume") : nullptr)
				setText(*text, decibels(widgets::number(fader, "value", 0.0F)));
		}

		void setChecked(Rml::Element& element, bool checked)
		{
			if (checked == element.HasAttribute("checked"))
				return;
			if (checked)
				element.SetAttribute("checked", "");
			else
				element.RemoveAttribute("checked");
		}

		bool endsWith(const std::string& text, const std::string& end)
		{
			return text.size() >= end.size() &&
				std::equal(end.rbegin(), end.rend(), text.rbegin(),
					[](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
		}

		// The path of a strip control's value in the channel's JSON, by dots: pre.lowCut.on, or for an EQ band
		// eq.bands.<band>.gain and for a send sends.<send>.level.
		std::vector<std::string> paramPath(const std::string& param)
		{
			std::vector<std::string> keys;
			std::istringstream text(param);
			for (std::string key; std::getline(text, key, '.');)
				keys.push_back(key);
			return keys;
		}

		// A strip control's value in the channel's JSON, or null.
		Json paramValue(const Json& channel, const std::string& param)
		{
			const Json* value = &channel;
			for (const std::string& key : paramPath(param))
			{
				if (value->is_array() && !key.empty() && std::isdigit(static_cast<unsigned char>(key[0])) != 0)
				{
					const size_t index = std::stoul(key) - 1; // EQ bands count from 1
					if (index >= value->size())
						return nullptr;
					value = &value->at(index);
				}
				else if (value->is_object() && value->contains(key))
				{
					value = &value->at(key);
				}
				else
				{
					return nullptr;
				}
			}
			return *value;
		}

		// What a strip control's gesture changes, as EngineBinding::sendStep names it: the mixer's step owner, then
		// e.g. "channel 5 volume". Each mixer has an owner of its own, so the others follow its steps (#716).
		std::string stepKey(const std::string& owner, uint64_t channel, const std::string& param)
		{
			return owner + "channel " + std::to_string(channel) + " " + param;
		}

		// The parameters of channel.set that give a strip control the value.
		Json paramChange(const std::string& param, Json value)
		{
			const std::vector<std::string> keys = paramPath(param);
			for (size_t i = keys.size(); i-- > 0;)
			{
				if (i > 0 && (keys[i - 1] == "bands" || keys[i - 1] == "sends"))
				{
					// eq.bands.<band> and sends.<send>: one band's or send's object in the array
					value[keys[i - 1] == "bands" ? "band" : "send"] = std::stoi(keys[i]);
					value = Json::array({std::move(value)});
					continue;
				}
				value = Json({{keys[i], std::move(value)}});
			}
			return value;
		}

		// The pre section, the equalizer and the strip's dynamics (reference manual pp. 473-489), and the sends
		// (pp. 552-557). Every strip has the sections, so that the docking system learns them from the first one; only
		// audio channels have controls. The mixers hide them by default, the channel settings window shows them.
		std::string sectionsRml(bool audio, bool shown)
		{
			const auto number = [](double value) { return widgets::format(static_cast<float>(value)); };
			const auto knob = [&number](
								  const std::string& param, const char* title, double min, double max, double fallback)
			{
				return R"(<knob param=")" + param + R"(" title=")" + title + R"(" min=")" + number(min) + R"(" max=")" +
					number(max) + R"(" default=")" + number(fallback) + R"(" step=")" + number((max - min) / 200.0) +
					R"("/>)";
			};
			const auto toggle = [](const std::string& param, const char* title, const std::string& label)
			{
				return R"(<button type="toggle" class="toggle" param=")" + param + R"(" title=")" + title + R"(">)" +
					label + "</button>";
			};
			const auto frequency = [&number](const std::string& param, const char* title, double min, double fallback)
			{
				return R"(<valuefield param=")" + param + R"(" title=")" + title + R"(" min=")" + number(min) +
					R"(" max="20000" default=")" + number(fallback) + R"(" step="10" decimals="0" unit="Hz"/>)";
			};
			const auto row = [](const std::string& controls)
			{ return R"(<div class="mixer-row">)" + controls + "</div>"; };
			const auto section = [audio, shown](const char* id, const char* label, const std::string& controls)
			{
				return std::string(R"(<div class="mixer-section mixer-)") + id + R"(" section=")" + id +
					R"(" label=")" + label + (shown ? R"(">)" : R"(" default="hidden">)") + (audio ? controls : "") +
					"</div>";
			};

			std::string eq = toggle("eq.bypass", "Equalizers Bypass", "Bypass");
			const std::array<double, 4> frequencies = {100.0, 500.0, 2000.0, 8000.0};
			for (int band = 1; band <= 4; ++band)
			{
				const std::string path = "eq.bands." + std::to_string(band) + ".";
				eq += row(toggle(path + "on", "EQ Band on/off", std::to_string(band)) +
					knob(path + "gain", "Gain", -24.0, 24.0, 0.0) +
					frequency(path + "frequency", "Frequency", 20.0, frequencies[static_cast<size_t>(band - 1)]) +
					knob(path + "q", "Q-Factor", 0.1, 12.0, 1.0));
			}
			std::string sends;
			for (int send = 1; send <= g_sends; ++send)
			{
				const std::string path = "sends." + std::to_string(send) + ".";
				std::string controls = toggle(path + "on", "Send on/off", std::to_string(send));
				controls += R"(<select param=")" + path + R"(destination" title="Send Destination"/>)";
				controls += toggle(path + "preFader", "Pre-Fader", "Pre");
				controls += knob(path + "level", "Send Level", g_sendKnobMin, 6.0, 0.0);
				sends += row(controls);
			}
			return section("pre", "Pre",
					   knob("pre.gain", "Pre-Gain", -24.0, 24.0, 0.0) + toggle("pre.phase", "Phase", "Ø") +
						   row(toggle("pre.lowCut.on", "Low-Cut Filter", "LC") +
							   frequency("pre.lowCut.frequency", "Low-Cut Frequency", 20.0, 20.0)) +
						   row(toggle("pre.highCut.on", "High-Cut Filter", "HC") +
							   frequency("pre.highCut.frequency", "High-Cut Frequency", 50.0, 20000.0))) +
				section("eq", "Equalizer", eq) +
				section("strip", "Strip",
					row(toggle("strip.gate.on", "Noise Gate", "Gate") +
						knob("strip.gate.threshold", "Threshold", -80.0, 0.0, -40.0) +
						knob("strip.gate.range", "Range", -80.0, 0.0, -80.0)) +
						row(toggle("strip.compressor.on", "Compressor", "Comp") +
							knob("strip.compressor.threshold", "Threshold", -60.0, 0.0, -20.0) +
							knob("strip.compressor.ratio", "Ratio", 1.0, 20.0, 4.0) +
							knob("strip.compressor.makeup", "MakeUp Gain", 0.0, 24.0, 0.0)) +
						row(toggle("strip.limiter.on", "Limiter", "Limit") +
							knob("strip.limiter.threshold", "Threshold", -30.0, 0.0, 0.0))) +
				section("sends", "Sends", sends);
		}

		// Where a channel's sends can go: no destination, or the group and effect channels other than its own; and with
		// add, Add Effect Track to Send (reference manual p. 554).
		widgets::Options sendDestinations(const std::vector<MixerChannel>& channels, uint64_t own, bool add)
		{
			widgets::Options options{{"0", "No Destination"}};
			for (const MixerChannel& channel : channels)
				if (channel.id != own && channel.channel != nullptr &&
					(channel.type == "group" || channel.type == "effect"))
					options.emplace_back(std::to_string(channel.id), channel.name);
			if (add)
				options.emplace_back(g_addEffect, "Add Effect Track to Send");
			return options;
		}

		// The link group a channel is in and its number, from 1 in the order of the groups; 0 for none.
		std::pair<const Json*, size_t> linkGroup(const Json& project, uint64_t channel, const char* key)
		{
			if (!project.contains("linkGroups"))
				return {nullptr, 0};
			size_t number = 0;
			for (const Json& group : project.at("linkGroups"))
			{
				++number;
				const Json& value = group.at(key);
				if (value.is_array() ? std::find(value.begin(), value.end(), Json(channel)) != value.end()
									 : value == channel)
					return {&group, number};
			}
			return {nullptr, 0};
		}

		// The toolbar's Deactivate All buttons (reference manual p. 447), by the state they turn off.
		constexpr std::array<std::pair<const char*, const char*>, 3> g_states = {{
			{"mute", "M"},
			{"solo", "S"},
			{"listen", "L"},
		}};

		bool held(const Rml::Event& event, const char* key)
		{
			return event.GetParameter<int>(key, 0) != 0;
		}

		constexpr std::array<Rml::EventId, 6> g_rootEvents = {Rml::EventId::Click, Rml::EventId::Dblclick,
			Rml::EventId::Change, Rml::EventId::Mousedown, Rml::EventId::Mouseup, Rml::EventId::Dragend};
	}

	Json soloClick(const Json& channel, bool exclusive, bool defeat)
	{
		if (defeat)
			return {{"soloDefeat", !channel.value("soloDefeat", false)}};
		const bool solo = !channel.value("solo", false);
		return {{"solo", solo}, {"exclusive", exclusive && solo}};
	}

	std::vector<MixerChannel> mixerChannels(const Json& project)
	{
		std::vector<MixerChannel> result;
		const auto busses = [&](const char* direction)
		{
			for (const Json& bus : project.at("busses"))
				if (bus.at("direction") == direction && bus.at("parent") == 0)
					result.push_back(
						{bus.at("id").get<uint64_t>(), 0, direction, bus.at("name").get<std::string>(), nullptr, {}});
		};
		busses("input");
		std::map<uint64_t, const Json*> channels;
		for (const Json& channel : project.at("channels"))
			channels[channel.at("id").get<uint64_t>()] = &channel;
		for (const Json& track : project.at("tracks"))
		{
			const auto channel = channels.find(track.at("channel").get<uint64_t>());
			if (channel == channels.end())
				continue;
			result.push_back(
				{channel->first, track.at("id").get<uint64_t>(), trackType(track.at("type").get<std::string>()),
					track.at("name").get<std::string>(), channel->second, track.value("picture", std::string())});
		}
		busses("output");
		return result;
	}

	Mixer::Mixer(EngineBinding& binding, Docking& docking, Rml::Element& root, std::string view, MixerKind kind,
		MixerActions actions)
		: m_binding(binding), m_stepOwner(binding.stepOwner("mixer")), m_docking(docking),
		  m_root(root.GetObserverPtr()), m_view(std::move(view)), m_kind(kind), m_actions(std::move(actions))
	{
		Rml::ElementDocument* document = root.GetOwnerDocument();
		const bool settings = m_kind == MixerKind::ChannelSettings;
		// The toolbar (reference manual pp. 444-449): what the mixer shows; in the channel settings window, which
		// channel it shows (pp. 501-503). The menus go into the body, so that no zone clips them.
		std::string rml = settings
			? R"(<div class="mixer channel-settings"><div class="toolbar mixer-toolbar">)"
			  R"(<button id="settings-previous" title="Go to Previous Mixer Channel">&lt;</button>)"
			  R"(<button id="settings-next" title="Go to Next Mixer Channel">&gt;</button>)"
			  R"(<div class="settings-name" title="Channel Name"/>)"
			  R"(<button id="settings-output" title="Go to Output"/>)"
			: R"(<div class="mixer"><div class="toolbar mixer-toolbar">)"
			  R"(<button id="mixer-filter" menu="mixer-filter-menu" title="Set Channel Type Filter">)"
			  R"(Channel Types</button>)"
			  R"(<button id="mixer-agents" menu="mixer-agents-menu" )"
			  R"(title="Set Channel Visibility Agents">Visibility</button>)";
		if (!settings)
			for (const auto& [state, label] : g_states)
			{
				std::string title = state;
				title[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(title[0])));
				rml += std::string(R"(<button class="toggle )") + state + R"(" deactivate=")" + state +
					R"(" title="Deactivate All )" + title + R"( States">)" + label + "</button>";
			}
		rml += R"(<div class="spacer"/>)";
		if (settings)
			rml += R"(<button id="settings-functions" menu="settings-functions-menu" class="icon-only" )"
				   R"(title="Functions Menu"><svg class="icon" src="../icons/general-menu.svg"/></button>)";
		if (m_kind == MixerKind::Window)
			rml += R"(<button id="mixer-left-zone" type="toggle" toggle="left" class="icon-only" )"
				   R"(title="Show/Hide Left Zone"><svg class="icon" src="../icons/layout-left-zone.svg"/></button>)";
		rml += R"(<button id="mixer-layout" menu="mixer-layout-pane" class="icon-only" title="Set up Window Layout">)"
			   R"(<svg class="icon" src="../icons/general-arrow-down.svg"/></button></div>)"
			   R"(<div class="mixer-channels"/></div>)";
		root.SetInnerRML(rml);
		m_strips = root.QuerySelector(".mixer-channels")->GetObserverPtr();
		if (Rml::Element* filter = root.QuerySelector("#mixer-filter"))
			m_filter = filter->GetObserverPtr();

		std::string filter;
		for (const auto& [type, label] : g_types)
			filter += std::string(R"(<menuitem type="toggle" channel-type=")") + type + R"(" checked="">)" + label +
				"</menuitem>";
		std::string layout =
			m_kind == MixerKind::Window ? R"(<button type="toggle" toggle="left">Left Zone</button>)" : "";
		layout += R"(<div setup-presets="channel"/><button setup-preset-save="channel">Save Preset</button>)"
				  R"(<button setup-all="channel">Show All</button><button setup-reset="channel">Reset All</button>)"
				  R"(<div setup="channel"/>)";
		// The channel settings window has the Functions menu (pp. 461, 503) instead of the filter and the agents.
		std::vector<std::pair<std::string, std::string>> menus = {
			{R"(<menu id="mixer-filter-menu">)", filter},
			{R"(<menu id="mixer-agents-menu">)", R"(<menuitem agent="showAll">Show All Channels</menuitem>)"},
		};
		if (settings)
			menus = {{R"(<menu id="settings-functions-menu">)",
				R"(<menuitem function="copy">Copy Channel Settings</menuitem>)"
				R"(<menuitem function="paste">Paste Channel Settings</menuitem>)"
				R"(<menuitem type="toggle" function="follow" checked="">)"
				R"(Follow Edit Buttons or Selection Changes</menuitem>)"}};
		menus.emplace_back(R"(<menu id="mixer-layout-pane" class="setup-pane">)", layout);
		for (const auto& [open, items] : menus)
		{
			const Rml::ElementPtr holder = document->CreateElement("div");
			holder->SetInnerRML(open + items + "</menu>");
			Rml::Element* menu = document->AppendChild(holder->RemoveChild(holder->GetFirstChild()));
			menu->AddEventListener(Rml::EventId::Click, this);
			menu->AddEventListener(Rml::EventId::Change, this);
			m_menus.push_back(menu->GetObserverPtr());
		}
		m_docking.arrangeSections(*m_menus.back().get()); // the setup pane, once the docking window knows the set
		for (const Rml::EventId id : g_rootEvents)
			root.AddEventListener(id, this);
		root.AddEventListener(Rml::EventId::Blur, this, true); // the notepad's, which doesn't bubble

		// Every change to the project is an undo step or a new project.
		for (const char* event : {"history.changed", "project.reset"})
			m_subscriptions.push_back(m_binding.on(event,
				[this, reset = std::string(event) == "project.reset"](const engine::Event&)
				{
					requestProject();
					if (reset)
						requestView();
				}));
		m_subscriptions.push_back(m_binding.on("mixer.changed",
			[this](const engine::Event& event)
			{
				if (event.data.at("mixer") != m_view)
					return;
				m_hidden.clear();
				for (const Json& id : event.data.at("hidden"))
					m_hidden.insert(id.get<uint64_t>());
				m_hiddenTypes = event.data.at("hiddenTypes").get<std::set<std::string>>();
				show();
			}));
		// Ctrl-click gives a send the default send level (reference manual p. 552).
		const auto sendLevel = [this](const Json& value)
		{
			m_defaultSendLevel = value.get<double>();
			show();
		};
		m_subscriptions.push_back(m_binding.on("settings.changed",
			[sendLevel](const engine::Event& event)
			{
				if (const auto level = event.data.at("values").find("audio.defaultSendLevel");
					level != event.data.at("values").end())
					sendLevel(*level);
			}));
		m_binding.send("settings.get", {{"key", "audio.defaultSendLevel"}},
			[sendLevel, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (!alive.expired() && result.ok())
					sendLevel(result.value.at("value"));
			});
		// Whether its channel visibility follows the track visibility (#166).
		const auto sync = [this](const Json& visibility)
		{
			m_syncs = visibility.at("sync") == m_view;
			showVisibilityList();
		};
		m_subscriptions.push_back(
			m_binding.on("trackVisibility.changed", [sync](const engine::Event& event) { sync(event.data); }));
		if (m_kind == MixerKind::Window)
			m_binding.send("trackVisibility.get", Json::object(),
				[sync, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
				{
					if (!alive.expired() && result.ok())
						sync(result.value);
				});
		if (!settings)
			requestView();
		requestProject();
	}

	Mixer::~Mixer()
	{
		if (Rml::Element* root = m_root.get())
		{
			for (const Rml::EventId id : g_rootEvents)
				root->RemoveEventListener(id, this);
			root->RemoveEventListener(Rml::EventId::Blur, this, true);
		}
		if (Rml::Element* visibility = m_visibility.get())
		{
			visibility->RemoveEventListener(Rml::EventId::Change, this);
			visibility->RemoveEventListener(Rml::EventId::Click, this);
		}
		for (const Rml::ObserverPtr<Rml::Element>& menu : m_menus)
			if (Rml::Element* element = menu.get())
			{
				element->RemoveEventListener(Rml::EventId::Click, this);
				element->RemoveEventListener(Rml::EventId::Change, this);
				if (Rml::Element* parent = element->GetParentNode())
					parent->RemoveChild(element);
			}
	}

	void Mixer::showVisibility(Rml::Element& element)
	{
		if (Rml::Element* old = m_visibility.get())
		{
			old->RemoveEventListener(Rml::EventId::Change, this);
			old->RemoveEventListener(Rml::EventId::Click, this);
		}
		m_visibility = element.GetObserverPtr();
		element.SetInnerRML(R"(<div class="toolbar mixer-visibility-toolbar"><button id="mixer-visibility-sync" )"
							R"(title="Sync Visibility of Project and Mixer: On/Off">=</button></div>)"
							R"(<div class="mixer-visibility"/>)");
		element.AddEventListener(Rml::EventId::Change, this);
		element.AddEventListener(Rml::EventId::Click, this);
		showVisibilityList();
	}

	// ponytail: reads the whole project after every change, as the event display does.
	void Mixer::requestProject()
	{
		if (m_loading)
		{
			m_stale = true;
			return;
		}
		m_loading = true;
		m_binding.send("project.get", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired())
					return;
				m_loading = false;
				if (result.ok())
				{
					m_project = result.value;
					m_channels = mixerChannels(m_project);
					show();
				}
				if (std::exchange(m_stale, false))
					requestProject();
			});
	}

	std::function<void(const engine::Result&)> Mixer::whenRefused()
	{
		return [this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
		{
			if (!alive.expired() && !result.ok())
				requestProject();
		};
	}

	void Mixer::requestView()
	{
		m_binding.send("mixer.get", {{"mixer", m_view}},
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok())
					return;
				m_hidden.clear();
				for (const Json& id : result.value.at("hidden"))
					m_hidden.insert(id.get<uint64_t>());
				m_hiddenTypes = result.value.at("hiddenTypes").get<std::set<std::string>>();
				show();
			});
	}

	void Mixer::show()
	{
		const Rml::Element* strips = m_strips.get();
		if (strips == nullptr)
			return;
		std::vector<const MixerChannel*> shown;
		std::vector<uint64_t> ids;
		const bool settings = m_kind == MixerKind::ChannelSettings;
		for (const MixerChannel& channel : m_channels)
			if (settings ? channel.track == m_track && m_track != 0
						 : m_hidden.count(channel.id) == 0 && m_hiddenTypes.count(channel.type) == 0)
			{
				shown.push_back(&channel);
				ids.push_back(channel.id);
			}
		if (ids != m_shown)
		{
			m_shown = ids;
			buildStrips(shown);
		}
		for (const MixerChannel* channel : shown)
			updateStrip(*m_stripOf.at(channel->id), *channel);
		if (settings)
			showSettingsToolbar();

		if (Rml::Element* filter = m_filter.get())
			filter->SetClass("checked", !m_hiddenTypes.empty()); // the button shows that a type is hidden
		// The Deactivate All buttons light up while any channel has their state.
		for (const auto& [state, label] : g_states)
		{
			Rml::Element* button = m_root.get() != nullptr
				? m_root.get()->QuerySelector(std::string(".mixer-toolbar button[deactivate=") + state + "]")
				: nullptr;
			if (button == nullptr)
				continue;
			bool any = false;
			for (const Json& channel : m_project.value("channels", Json::array()))
				any = any || channel.value(state, false) ||
					(std::string(state) == "solo" && channel.value("soloDefeat", false));
			button->SetClass("checked", any);
		}
		if (Rml::Element* menu = m_menus.front().get())
			for (const auto& [type, label] : g_types)
				if (Rml::Element* item = menu->QuerySelector(std::string("[channel-type=") + type + "]"))
					setChecked(*item, m_hiddenTypes.count(type) == 0);
		showVisibilityList();
		if (Rml::Context* context = strips->GetContext())
			context->RequestNextUpdate(0);
	}

	void Mixer::buildStrips(const std::vector<const MixerChannel*>& strips)
	{
		Rml::Element& container = *m_strips.get();
		while (container.GetNumChildren() > 0)
			container.RemoveChild(container.GetFirstChild());
		m_stripOf.clear();
		Rml::ElementDocument* document = container.GetOwnerDocument();
		for (const MixerChannel* channel : strips)
		{
			const bool audio = channel->channel != nullptr && channel->channel->at("format") != "none";
			const bool vca = channel->type == "vca"; // a fader and buttons (reference manual p. 515)
			std::string rml = R"(<div class="mixer-channel-name"/><div class="mixer-link"/>)"
							  R"(<div class="mixer-picture" section="picture" label="Pictures" default="hidden"/>)";
			rml += sectionsRml(audio, m_kind == MixerKind::ChannelSettings);
			rml += R"(<div class="mixer-notes" section="notes" label="Notepad" default="hidden">)";
			if (channel->channel != nullptr)
				rml += R"(<textarea class="mixer-notes-field" rows="4"/>)";
			rml += R"(</div><div class="mixer-fader" section="fader" label="Fader">)";
			if (audio) // the panner (reference manual p. 465)
				rml += R"(<knob class="mixer-pan" param="pan" title="Pan" min="-1" max="1" default="0" step="0.01"/>)";
			if (vca) // instead of the panner, the link group it controls
				rml += R"(<select class="mixer-vca-group" title="VCA Channel Controls Link Group"/>)";
			if (channel->channel != nullptr)
				rml +=
					R"(<div class="mixer-buttons"><button class="toggle mute" control="mute" title="Mute">M</button>)"
					R"(<button class="toggle solo" control="solo" title="Solo">S</button>)" +
					std::string(audio || vca
							? R"(<button class="toggle listen" control="listen" title="Listen">L</button>)"
							: "") +
					"</div>";
			// Edit Channel Settings (reference manual p. 500).
			if (channel->channel != nullptr && m_kind != MixerKind::ChannelSettings && m_actions.editChannel)
				rml += R"(<button class="mixer-edit" edit="" title="Edit Channel Settings">e</button>)";
			if (audio || vca) // from -60 dB, linear until the channel strip's dB law (#107)
				rml += R"(<fader min="-60" max="6" default="0" step="0.1"/><span class="mixer-volume"/>)";
			rml += "</div>";
			Rml::Element* strip = container.AppendChild(document->CreateElement("div"));
			strip->SetClass("mixer-channel", true);
			strip->SetAttribute("sections", "channel");
			strip->SetAttribute("channel", std::to_string(channel->id));
			strip->SetAttribute("type", channel->type);
			strip->SetInnerRML(rml);
			m_stripOf[channel->id] = strip;
		}
		m_docking.arrangeSections(container);
	}

	void Mixer::updateStrip(Rml::Element& strip, const MixerChannel& channel)
	{
		if (Rml::Element* name = strip.QuerySelector(".mixer-channel-name"))
			setText(*name, channel.name);
		if (Rml::Element* picture = strip.QuerySelector(".mixer-picture");
			picture != nullptr && attribute(*picture, "picture") != channel.picture)
		{
			picture->SetAttribute("picture", channel.picture);
			while (picture->GetNumChildren() > 0)
				picture->RemoveChild(picture->GetFirstChild());
			if (!channel.picture.empty())
			{
				Rml::Element* image = picture->AppendChild(
					strip.GetOwnerDocument()->CreateElement(endsWith(channel.picture, ".svg") ? "svg" : "img"));
				image->SetAttribute("src", channel.picture);
			}
		}
		if (channel.channel == nullptr)
			return;
		const Json& json = *channel.channel;
		// The display line (reference manual p. 458): the number and the name of the channel's link group.
		if (Rml::Element* link = strip.QuerySelector(".mixer-link"))
		{
			const auto [group, number] = linkGroup(m_project, channel.id, "channels");
			setText(*link, group != nullptr ? std::to_string(number) + " " + group->at("name").get<std::string>() : "");
			strip.SetClass("linked", group != nullptr);
		}
		if (Rml::Element* select = strip.QuerySelector(".mixer-vca-group"))
		{
			widgets::Options groups{{"0", "None"}};
			size_t number = 0;
			for (const Json& group : m_project.value("linkGroups", Json::array()))
				groups.emplace_back(std::to_string(group.at("id").get<uint64_t>()),
					std::to_string(++number) + " " + group.at("name").get<std::string>());
			const Json* controlled = linkGroup(m_project, channel.id, "vca").first;
			m_updating = true; // filling a drop-down list sends a change
			widgets::fill(*select, groups,
				controlled != nullptr ? std::to_string(controlled->at("id").get<uint64_t>()) : std::string("0"));
			m_updating = false;
		}
		if (auto* notes = dynamic_cast<Rml::ElementFormControl*>(strip.QuerySelector(".mixer-notes-field")))
		{
			Rml::Context* context = strip.GetContext();
			const bool typing = context != nullptr && context->GetFocusElement() == notes;
			if (!typing && notes->GetValue() != json.value("notes", std::string()))
				notes->SetValue(json.value("notes", std::string()));
		}
		for (const char* control : {"mute", "solo", "listen"})
			if (Rml::Element* button = strip.QuerySelector(std::string("button[control=") + control + "]"))
				button->SetClass("checked", json.at(control).get<bool>());
		if (Rml::Element* solo = strip.QuerySelector("button[control=solo]"))
			solo->SetClass("defeated", json.at("soloDefeat").get<bool>());
		Rml::ElementList controls;
		strip.QuerySelectorAll(controls, "[param]");
		m_updating = true; // filling a drop-down list sends a change
		// A send level's Ctrl-click value follows the default send level, also while its knob is ahead. The knob goes
		// up to the project's maximum volume, as the fader does, and down to the default send level below its usual
		// -60 dB, so that it gives a send the level track.add gives it.
		const std::string level = widgets::format(static_cast<float>(m_defaultSendLevel));
		const std::string lowest = widgets::format(static_cast<float>(std::min(m_defaultSendLevel, g_sendKnobMin)));
		const std::string highest = widgets::format(m_project.value("maxVolume", 6.0F));
		for (Rml::Element* control : controls)
		{
			const std::string param = attribute(*control, "param");
			if (param.rfind("sends.", 0) == 0 && endsWith(param, ".level"))
				for (const auto& [name, value] :
					{std::pair{"default", &level}, std::pair{"min", &lowest}, std::pair{"max", &highest}})
					if (attribute(*control, name) != *value)
						control->SetAttribute(name, *value);
			if (control->HasAttribute("value") && m_binding.sending(stepKey(m_stepOwner, channel.id, param)))
				continue; // the control is ahead of the engine
			const Json value = paramValue(json, param);
			if (control->GetTagName() == "select")
			{
				if (value.is_number())
					widgets::fill(*control,
						sendDestinations(m_channels, channel.id, channel.track != 0 && m_actions.addEffectTrack),
						std::to_string(value.get<uint64_t>()));
			}
			else if (value.is_boolean())
			{
				setChecked(*control, value.get<bool>());
			}
			else if (value.is_number())
			{
				const std::string text = widgets::format(value.get<float>());
				if (attribute(*control, "value") != text)
					control->SetAttribute("value", text);
			}
		}
		m_updating = false;
		if (Rml::Element* fader = strip.QuerySelector("fader"))
		{
			const double volume = json.at("volume").get<double>();
			const std::string max = widgets::format(m_project.value("maxVolume", 6.0F));
			if (attribute(*fader, "max") != max)
				fader->SetAttribute("max", max);
			// Not while the fader is ahead of the engine; a strip made again shows the engine's volume at once.
			const std::string value = widgets::format(static_cast<float>(volume));
			if (attribute(*fader, "value") != value &&
				!(fader->HasAttribute("value") && m_binding.sending(stepKey(m_stepOwner, channel.id, "volume"))))
				fader->SetAttribute("value", value);
			showVolume(*fader);
		}
	}

	void Mixer::showTrack(uint64_t track)
	{
		m_track = track;
		show();
	}

	// Previous, next and Go to Output: the tracks they go to, in their goto attribute, and the names.
	void Mixer::showSettingsToolbar()
	{
		Rml::Element* root = m_root.get();
		if (root == nullptr)
			return;
		std::vector<const MixerChannel*> tracks;
		for (const MixerChannel& channel : m_channels)
			if (channel.track != 0)
				tracks.push_back(&channel);
		const auto at = std::find_if(
			tracks.begin(), tracks.end(), [this](const MixerChannel* channel) { return channel->track == m_track; });
		const MixerChannel* shown = at != tracks.end() ? *at : nullptr;
		const auto point = [](Rml::Element* button, const MixerChannel* to)
		{
			if (button == nullptr)
				return;
			if (to != nullptr)
			{
				button->SetAttribute("goto", std::to_string(to->track));
				button->RemoveAttribute("disabled");
			}
			else
			{
				button->RemoveAttribute("goto");
				button->SetAttribute("disabled", "");
			}
		};
		point(
			root->QuerySelector("#settings-previous"), shown != nullptr && at != tracks.begin() ? *(at - 1) : nullptr);
		point(root->QuerySelector("#settings-next"),
			shown != nullptr && std::next(at) != tracks.end() ? *std::next(at) : nullptr);
		if (Rml::Element* name = root->QuerySelector(".settings-name"))
			setText(*name, shown != nullptr ? shown->name : std::string());
		// Go to Output: the bus or the channel it plays into, which it goes to if that is a track's.
		const MixerChannel* output = nullptr;
		if (shown != nullptr && shown->channel != nullptr)
		{
			const Json destination = shown->channel->value("output", Json(0));
			for (const MixerChannel& channel : m_channels)
				if (destination == channel.id)
					output = &channel;
		}
		if (Rml::Element* button = root->QuerySelector("#settings-output"))
		{
			setText(*button, output != nullptr ? output->name : std::string());
			point(button, output != nullptr && output->track != 0 ? output : nullptr);
		}
	}

	void Mixer::goTo(uint64_t track)
	{
		showTrack(track);
		if (m_follows && m_actions.wentTo) // without Follow, the selection is the project window's own
			m_actions.wentTo(track);
	}

	void Mixer::showVisibilityList()
	{
		Rml::Element* element = m_visibility.get();
		Rml::Element* list = element != nullptr ? element->QuerySelector(".mixer-visibility") : nullptr;
		if (list == nullptr)
			return;
		std::string rml;
		for (const MixerChannel& channel : m_channels)
			rml += R"(<button type="toggle" class="mixer-visibility-item" visibility=")" + std::to_string(channel.id) +
				'"' + (m_hidden.count(channel.id) == 0 ? R"( checked="")" : "") + ">" + encode(channel.name) +
				"</button>";
		if (list->GetInnerRML() != rml)
			list->SetInnerRML(rml);
		if (Rml::Element* sync = element->QuerySelector("#mixer-visibility-sync"))
			sync->SetClass("checked", m_syncs);
	}

	void Mixer::setHidden(uint64_t id, bool hidden)
	{
		std::set<uint64_t> changed = m_hidden;
		if (hidden)
			changed.insert(id);
		else
			changed.erase(id);
		sendView({{"hidden", changed}});
	}

	void Mixer::setTypeHidden(const std::string& type, bool hidden)
	{
		std::set<std::string> changed = m_hiddenTypes;
		if (hidden)
			changed.insert(type);
		else
			changed.erase(type);
		sendView({{"hiddenTypes", changed}});
	}

	void Mixer::sendView(Json parameters)
	{
		parameters["mixer"] = m_view;
		m_binding.send("mixer.set", std::move(parameters)); // mixer.changed shows it
	}

	const MixerChannel* Mixer::channelOf(Rml::Element* element) const
	{
		const Rml::Element* strip = closest(element, "mixer-channel", m_root.get());
		if (strip == nullptr)
			return nullptr;
		const uint64_t id = idAttribute(*strip, "channel");
		const auto found =
			std::find_if(m_channels.begin(), m_channels.end(), [id](const MixerChannel& c) { return c.id == id; });
		return found == m_channels.end() ? nullptr : &*found;
	}

	// The dragged channel takes the place of the one it is dropped on (reference manual p. 496): its track moves into
	// that track's folder, after it when it came from the left, before it otherwise.
	void Mixer::drop(uint64_t onto)
	{
		const auto find = [this](uint64_t id)
		{
			return std::find_if(
				m_channels.begin(), m_channels.end(), [id](const MixerChannel& c) { return c.id == id; });
		};
		const auto from = find(m_dragged);
		const auto to = find(onto);
		if (from == m_channels.end() || to == m_channels.end() || from->track == 0 || to->track == 0 || from == to)
			return;
		std::map<uint64_t, uint64_t> parents;
		std::map<uint64_t, std::vector<uint64_t>> children;
		for (const Json& track : m_project.at("tracks"))
		{
			const auto id = track.at("id").get<uint64_t>();
			parents[id] = track.at("parent").get<uint64_t>();
			if (id != from->track)
				children[parents[id]].push_back(id);
		}
		const uint64_t parent = parents[to->track];
		for (uint64_t up = parent; up != 0; up = parents[up])
			if (up == from->track)
				return; // not into itself
		const std::vector<uint64_t>& siblings = children[parent];
		const auto index =
			(std::find(siblings.begin(), siblings.end(), to->track) - siblings.begin()) + (from < to ? 1 : 0);
		m_binding.send("track.move", {{"track", from->track}, {"parent", parent}, {"index", index}});
	}

	void Mixer::ProcessEvent(Rml::Event& event)
	{
		Rml::Element* target = event.GetTargetElement();
		if (target == nullptr)
			return;
		switch (event.GetId())
		{
		case Rml::EventId::Change:
			if (target->HasAttribute("channel-type"))
			{
				setTypeHidden(attribute(*target, "channel-type"), !event.GetParameter("value", true));
			}
			else if (target->HasAttribute("visibility"))
			{
				setHidden(idAttribute(*target, "visibility"), !event.GetParameter("value", true));
			}
			else if (attribute(*target, "function") == "follow")
			{
				m_follows = event.GetParameter("value", true);
			}
			else if (target->IsClassSet("mixer-vca-group") && !m_updating)
			{
				// Assigns the VCA fader to a link group, or with None takes it from its group (reference manual p.
				// 515).
				if (const MixerChannel* channel = channelOf(target))
				{
					const uint64_t group = std::stoull(event.GetParameter<Rml::String>("value", "0"));
					const Json* controlled = linkGroup(m_project, channel->id, "vca").first;
					if (group != 0)
						m_binding.send("linkGroup.set", {{"group", group}, {"vca", channel->id}});
					else if (controlled != nullptr)
						m_binding.send("linkGroup.set", {{"group", controlled->at("id")}, {"vca", 0}});
				}
			}
			else if (target->GetTagName() == "fader")
			{
				if (const MixerChannel* channel = channelOf(target)) // a drag is one undo step
				{
					const float volume = event.GetParameter("value", 0.0F);
					showVolume(*target); // with the fader, ahead of the engine
					m_binding.sendStep(event.GetParameter("gesture", 0), stepKey(m_stepOwner, channel->id, "volume"),
						"Change Volume",
						Json::array({Json{{"command", "channel.set"},
							{"parameters", {{"channel", channel->id}, {"volume", volume}}}}}),
						whenRefused());
				}
			}
			else if (target->HasAttribute("param") && !m_updating)
			{
				if (const MixerChannel* channel = channelOf(target); channel != nullptr && channel->channel != nullptr)
				{
					const Rml::Variant* given =
						event.GetParameters().count("value") != 0 ? &event.GetParameters().at("value") : nullptr;
					if (given == nullptr)
						break;
					const std::string param = attribute(*target, "param");
					if (target->GetTagName() == "select" && given->Get<Rml::String>() == g_addEffect)
					{
						// Add Effect Track to Send; the list shows the send's destination again once the project
						// comes back.
						if (m_actions.addEffectTrack)
							m_actions.addEffectTrack(*target, channel->track, std::stoi(paramPath(param).at(1)));
						requestProject();
						break;
					}
					if (given->GetType() == Rml::Variant::BOOL || target->GetTagName() == "select")
					{
						// A toggle, or a send's destination, a channel id.
						Json parameters = paramChange(param,
							given->GetType() == Rml::Variant::BOOL ? Json(given->Get<bool>())
																   : Json(std::stoull(given->Get<Rml::String>("0"))));
						parameters["channel"] = channel->id;
						m_binding.send("channel.set", std::move(parameters));
						break;
					}
					// A knob's or a value field's drag, wheel turns or held arrow key: one undo step.
					Json parameters = paramChange(param, given->Get<double>());
					parameters["channel"] = channel->id;
					m_binding.sendStep(event.GetParameter("gesture", 0), stepKey(m_stepOwner, channel->id, param),
						"Change " + attribute(*target, "title"),
						Json::array({Json{{"command", "channel.set"}, {"parameters", std::move(parameters)}}}),
						whenRefused());
				}
			}
			break;
		case Rml::EventId::Click:
			if (m_visibility && event.GetCurrentElement() == m_visibility.get())
			{
				// Sync Visibility of Project and Mixer (reference manual p. 442): one window at a time.
				for (const Rml::Element* up = target; up != nullptr; up = up->GetParentNode())
					if (up->GetId() == "mixer-visibility-sync")
						m_binding.send("trackVisibility.set", {{"sync", m_syncs ? std::string() : m_view}});
			}
			else if (attribute(*target, "agent") == "showAll")
			{
				sendView({{"hidden", Json::array()}});
			}
			else if (attribute(*target, "function") == "copy")
			{
				if (const MixerChannel* channel = channelOf(m_stripOf.empty() ? nullptr : m_stripOf.begin()->second))
					m_binding.send("channel.copySettings", {{"channel", channel->id}});
			}
			else if (attribute(*target, "function") == "paste")
			{
				if (const MixerChannel* channel = channelOf(m_stripOf.empty() ? nullptr : m_stripOf.begin()->second))
					m_binding.send("channel.pasteSettings", {{"channels", {channel->id}}});
			}
			else if (target->HasAttribute("goto"))
			{
				goTo(idAttribute(*target, "goto"));
			}
			else if (target->HasAttribute("edit"))
			{
				if (const MixerChannel* channel = channelOf(target); channel != nullptr && m_actions.editChannel)
					m_actions.editChannel(channel->track);
			}
			else if (target->HasAttribute("deactivate"))
			{
				m_binding.send("channel.deactivateAll", {{"state", attribute(*target, "deactivate")}});
			}
			else if (target->HasAttribute("control"))
			{
				if (const MixerChannel* channel = channelOf(target); channel != nullptr && channel->channel != nullptr)
				{
					// Solo: Ctrl-click (Cmd on macOS) solos exclusively, Alt-click switches solo defeat (p. 466).
					const std::string control = attribute(*target, "control");
					Json parameters = control == "solo"
						? soloClick(*channel->channel, held(event, "ctrl_key") || held(event, "meta_key"),
							  held(event, "alt_key"))
						: Json{{control, !channel->channel->at(control).get<bool>()}};
					parameters["channel"] = channel->id;
					m_binding.send("channel.set", std::move(parameters));
				}
			}
			break;
		case Rml::EventId::Dblclick:
			if (closest(target, "mixer-picture", m_root.get()) != nullptr)
				if (const MixerChannel* channel = channelOf(target); channel != nullptr && channel->track != 0)
					if (m_actions.choosePicture)
						m_actions.choosePicture(channel->track);
			break;
		case Rml::EventId::Mousedown:
		{
			const MixerChannel* channel =
				closest(target, "mixer-channel-name", m_root.get()) != nullptr ? channelOf(target) : nullptr;
			m_dragged = channel != nullptr ? channel->id : 0;
			break;
		}
		case Rml::EventId::Mouseup:
			if (const MixerChannel* channel = channelOf(target); channel != nullptr && m_dragged != 0)
				drop(channel->id);
			m_dragged = 0;
			break;
		case Rml::EventId::Dragend:
			// The fader may show a value that came during the drag now.
			for (const Rml::Element* up = target; up != nullptr && up != m_root.get(); up = up->GetParentNode())
				if (up->GetTagName() == "fader")
				{
					showVolume(*up);
					break;
				}
			break;
		case Rml::EventId::Blur:
			if (target->IsClassSet("mixer-notes-field"))
				if (const MixerChannel* channel = channelOf(target); channel != nullptr && channel->channel != nullptr)
					if (auto* field = dynamic_cast<Rml::ElementFormControl*>(target);
						field != nullptr && field->GetValue() != channel->channel->value("notes", std::string()))
						m_binding.send("channel.set", {{"channel", channel->id}, {"notes", field->GetValue()}});
			break;
		default:
			break;
		}
	}
}
