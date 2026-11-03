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
		constexpr std::array<std::pair<const char*, const char*>, 7> g_types = {{
			{"input", "Input Channels"},
			{"audio", "Audio Channels"},
			{"instrument", "Instrument Channels"},
			{"midi", "MIDI Channels"},
			{"group", "Group Channels"},
			{"effect", "Effect Channels"},
			{"output", "Output Channels"},
		}};

		constexpr double g_silence = -144.0; // engine::g_minChannelVolume

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
		// eq.bands.<band>.gain.
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

		// The parameters of channel.set that give a strip control the value.
		Json paramChange(const std::string& param, Json value)
		{
			const std::vector<std::string> keys = paramPath(param);
			for (size_t i = keys.size(); i-- > 0;)
			{
				if (i > 0 && keys[i - 1] == "bands")
				{
					value["band"] = std::stoi(keys[i]); // eq.bands.<band>: one band's object in the array
					value = Json::array({std::move(value)});
					continue;
				}
				value = Json({{keys[i], std::move(value)}});
			}
			return value;
		}

		// The pre section, the equalizer and the strip's dynamics (reference manual pp. 473-489). Every strip has the
		// sections, so that the docking system learns them from the first one; only audio channels have controls.
		std::string sectionsRml(bool audio)
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
			const auto section = [audio](const char* id, const char* label, const std::string& controls)
			{
				return std::string(R"(<div class="mixer-section mixer-)") + id + R"(" section=")" + id +
					R"(" label=")" + label + R"(" default="hidden">)" + (audio ? controls : "") + "</div>";
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
							knob("strip.limiter.threshold", "Threshold", -30.0, 0.0, 0.0)));
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

		constexpr std::array<Rml::EventId, 5> g_rootEvents = {Rml::EventId::Click, Rml::EventId::Dblclick,
			Rml::EventId::Change, Rml::EventId::Mousedown, Rml::EventId::Mouseup};
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

	Mixer::Mixer(EngineBinding& binding, Docking& docking, Rml::Element& root, std::string view, bool window,
		std::function<void(uint64_t track)> choosePicture)
		: m_binding(binding), m_docking(docking), m_root(root.GetObserverPtr()), m_view(std::move(view)),
		  m_window(window), m_choosePicture(std::move(choosePicture))
	{
		Rml::ElementDocument* document = root.GetOwnerDocument();
		// The toolbar (reference manual pp. 444-449): what the mixer shows. The menus go into the body, so that no
		// zone clips them.
		std::string rml = R"(<div class="mixer"><div class="toolbar mixer-toolbar">)"
						  R"(<button id="mixer-filter" menu="mixer-filter-menu" title="Set Channel Type Filter">)"
						  R"(Channel Types</button>)"
						  R"(<button id="mixer-agents" menu="mixer-agents-menu" title="Set Channel Visibility Agents">)"
						  R"(Visibility</button>)";
		for (const auto& [state, label] : g_states)
		{
			std::string title = state;
			title[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(title[0])));
			rml += std::string(R"(<button class="toggle )") + state + R"(" deactivate=")" + state +
				R"(" title="Deactivate All )" + title + R"( States">)" + label + "</button>";
		}
		rml += R"(<div class="spacer"/>)";
		if (m_window)
			rml += R"(<button id="mixer-left-zone" type="toggle" toggle="left" class="icon-only" )"
				   R"(title="Show/Hide Left Zone"><svg class="icon" src="../icons/layout-left-zone.svg"/></button>)";
		rml += R"(<button id="mixer-layout" menu="mixer-layout-pane" class="icon-only" title="Set up Window Layout">)"
			   R"(<svg class="icon" src="../icons/general-arrow-down.svg"/></button></div>)"
			   R"(<div class="mixer-channels"/></div>)";
		root.SetInnerRML(rml);
		m_strips = root.QuerySelector(".mixer-channels")->GetObserverPtr();
		m_filter = root.QuerySelector("#mixer-filter")->GetObserverPtr();

		std::string filter;
		for (const auto& [type, label] : g_types)
			filter += std::string(R"(<menuitem type="toggle" channel-type=")") + type + R"(" checked="">)" + label +
				"</menuitem>";
		std::string layout = m_window ? R"(<button type="toggle" toggle="left">Left Zone</button>)" : "";
		layout += R"(<div setup="channel"/>)";
		const std::array<std::pair<std::string, std::string>, 3> menus = {{
			{R"(<menu id="mixer-filter-menu">)", filter},
			{R"(<menu id="mixer-agents-menu">)", R"(<menuitem agent="showAll">Show All Channels</menuitem>)"},
			{R"(<menu id="mixer-layout-pane" class="setup-pane">)", layout},
		}};
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
			visibility->RemoveEventListener(Rml::EventId::Change, this);
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
			old->RemoveEventListener(Rml::EventId::Change, this);
		m_visibility = element.GetObserverPtr();
		element.SetInnerRML(R"(<div class="mixer-visibility"/>)");
		element.AddEventListener(Rml::EventId::Change, this);
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
		for (const MixerChannel& channel : m_channels)
			if (m_hidden.count(channel.id) == 0 && m_hiddenTypes.count(channel.type) == 0)
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
			std::string rml = R"(<div class="mixer-channel-name"/>)"
							  R"(<div class="mixer-picture" section="picture" label="Pictures" default="hidden"/>)";
			rml += sectionsRml(audio);
			rml += R"(<div class="mixer-notes" section="notes" label="Notepad" default="hidden">)";
			if (channel->channel != nullptr)
				rml += R"(<textarea class="mixer-notes-field" rows="4"/>)";
			rml += R"(</div><div class="mixer-fader" section="fader" label="Fader">)";
			if (audio) // the panner (reference manual p. 465)
				rml += R"(<knob class="mixer-pan" param="pan" title="Pan" min="-1" max="1" default="0" step="0.01"/>)";
			if (channel->channel != nullptr)
				rml +=
					R"(<div class="mixer-buttons"><button class="toggle mute" control="mute" title="Mute">M</button>)"
					R"(<button class="toggle solo" control="solo" title="Solo">S</button>)" +
					std::string(
						audio ? R"(<button class="toggle listen" control="listen" title="Listen">L</button>)" : "") +
					"</div>";
			if (audio) // from -60 dB, linear until the channel strip's dB law (#107)
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
		for (Rml::Element* control : controls)
		{
			const Json value = paramValue(json, attribute(*control, "param"));
			if (value.is_boolean())
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
		if (Rml::Element* fader = strip.QuerySelector("fader"))
		{
			const double volume = json.at("volume").get<double>();
			const std::string max = widgets::format(m_project.value("maxVolume", 6.0F));
			if (attribute(*fader, "max") != max)
				fader->SetAttribute("max", max);
			const std::string value = widgets::format(static_cast<float>(volume));
			if (attribute(*fader, "value") != value)
				fader->SetAttribute("value", value);
			if (Rml::Element* text = strip.QuerySelector(".mixer-volume"))
				setText(*text, decibels(volume));
		}
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
			else if (target->GetTagName() == "fader")
			{
				if (const MixerChannel* channel = channelOf(target))
					m_binding.send(
						"channel.set", {{"channel", channel->id}, {"volume", event.GetParameter("value", 0.0F)}});
			}
			else if (target->HasAttribute("param"))
			{
				if (const MixerChannel* channel = channelOf(target); channel != nullptr && channel->channel != nullptr)
				{
					const Rml::Variant* given =
						event.GetParameters().count("value") != 0 ? &event.GetParameters().at("value") : nullptr;
					if (given == nullptr)
						break;
					Json value =
						given->GetType() == Rml::Variant::BOOL ? Json(given->Get<bool>()) : Json(given->Get<double>());
					Json parameters = paramChange(attribute(*target, "param"), std::move(value));
					parameters["channel"] = channel->id;
					m_binding.send("channel.set", std::move(parameters));
				}
			}
			break;
		case Rml::EventId::Click:
			if (attribute(*target, "agent") == "showAll")
				sendView({{"hidden", Json::array()}});
			else if (target->HasAttribute("deactivate"))
				m_binding.send("channel.deactivateAll", {{"state", attribute(*target, "deactivate")}});
			else if (target->HasAttribute("control"))
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
			break;
		case Rml::EventId::Dblclick:
			if (closest(target, "mixer-picture", m_root.get()) != nullptr)
				if (const MixerChannel* channel = channelOf(target); channel != nullptr && channel->track != 0)
					m_choosePicture(channel->track);
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
