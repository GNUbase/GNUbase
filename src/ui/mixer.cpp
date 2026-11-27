// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/mixer.h"

#include "engine/analysis/analysis.h"
#include "engine/document/channelformat.h"
#include "ui/docking.h"
#include "ui/widgets.h"
#include "ui/widgetutils.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/SystemInterface.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
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
		constexpr float g_meterEnd = 0.001f;       // -60 dB, where a channel meter's bar ends
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
			Rml::Element* strip = fader.Closest(".mixer-fader");
			if (Rml::Element* text = strip != nullptr ? strip->QuerySelector(".mixer-volume") : nullptr)
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

		// WCAG 2.2's relative luminance of a color as "#rrggbb" (upper or lower case), nothing for other text.
		std::optional<double> relativeLuminance(const std::string& color)
		{
			if (color.size() != 7 || color[0] != '#')
				return std::nullopt;
			const auto digit = [](char c)
			{
				const int lower = std::tolower(static_cast<unsigned char>(c));
				if (lower >= '0' && lower <= '9')
					return lower - '0';
				return lower >= 'a' && lower <= 'f' ? lower - 'a' + 10 : -1;
			};
			const std::array<double, 3> weights = {0.2126, 0.7152, 0.0722};
			double luminance = 0.0;
			for (size_t i = 0; i < weights.size(); ++i)
			{
				const int high = digit(color.at(1 + (2 * i)));
				const int low = digit(color.at(2 + (2 * i)));
				if (high < 0 || low < 0)
					return std::nullopt;
				const double channel = ((high * 16) + low) / 255.0;
				luminance +=
					weights.at(i) * (channel <= 0.04045 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4));
			}
			return luminance;
		}

		// Whether a color is bright: black text over it has more contrast than white text.
		bool bright(const std::string& color)
		{
			return relativeLuminance(color).value_or(0.0) > 0.179; // where black and white text have the same contrast
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

		// Whether a channel's routing gives it a panner (engine::pannerFor): not mono into mono, nor on destinations
		// wider than stereo, nor without one.
		bool hasPanner(const Json& project, const Json& channel)
		{
			const Json output = channel.value("output", Json(0));
			std::optional<engine::ChannelFormat> destination;
			for (const char* list : {"busses", "channels"})
				for (const Json& each : project.value(list, Json::array()))
					if (each.at("id") == output)
						destination = engine::formatFromName(each.at("format").get<std::string>());
			const std::optional<engine::ChannelFormat> format =
				engine::formatFromName(channel.at("format").get<std::string>());
			return format && destination && engine::pannerFor(*format, *destination) != engine::Panner::None;
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
			const auto choice = [](const std::string& param, const char* title)
			{ return R"(<select param=")" + param + R"(" title=")" + title + R"("/>)"; };
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
					knob(path + "q", "Q-Factor", 0.1, 12.0, 1.0) +
					(band == 1 || band == 4 ? choice(path + "type", "EQ Type") : std::string()));
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
							   choice("pre.lowCut.slope", "Low-Cut Slope") +
							   frequency("pre.lowCut.frequency", "Low-Cut Frequency", 20.0, 20.0)) +
						   row(toggle("pre.highCut.on", "High-Cut Filter", "HC") +
							   choice("pre.highCut.slope", "High-Cut Slope") +
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

		// The choices of a section's drop-down list (reference manual pp. 474 and 480): a cut filter's slope, or the
		// type of EQ band 1 or 4.
		widgets::Options sectionChoices(const std::string& param)
		{
			if (endsWith(param, ".slope"))
				return {{"6", "6 dB"}, {"12", "12 dB"}, {"24", "24 dB"}, {"36", "36 dB"}, {"48", "48 dB"}};
			if (param == "eq.bands.1.type")
				return {{"bell", "Parametric"}, {"lowShelf", "Low Shelf"}, {"lowCut", "Low Cut"}};
			return {{"bell", "Parametric"}, {"highShelf", "High Shelf"}, {"highCut", "High Cut"}};
		}

		// Where a channel plays: its output and its sends' destinations.
		std::vector<Json> routing(const Json& channel)
		{
			std::vector<Json> to{channel.value("output", Json())};
			for (const Json& send : channel.value("sends", Json::array()))
				to.push_back(send.value("destination", Json()));
			return to;
		}

		// Where a channel's sends can go: no destination, or the group and effect channels other than its own whose
		// audio doesn't come back to it; and with add, Add Effect Track to Send (reference manual p. 554).
		widgets::Options sendDestinations(
			const Json& project, const std::vector<MixerChannel>& channels, uint64_t own, bool add)
		{
			widgets::Options options{{"0", "No Destination"}};
			for (const MixerChannel& channel : channels)
				if (channel.id != own && channel.channel != nullptr &&
					(channel.type == "group" || channel.type == "effect") && !routesTo(project, channel.id, own))
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

		constexpr std::array<Rml::EventId, 5> g_rootEvents = {Rml::EventId::Click, Rml::EventId::Dblclick,
			Rml::EventId::Change, Rml::EventId::Mousedown, Rml::EventId::Dragend};
	}

	Json soloClick(const Json& channel, bool exclusive, bool defeat)
	{
		if (defeat)
			return {{"soloDefeat", !channel.value("soloDefeat", false)}};
		const bool solo = !channel.value("solo", false);
		return {{"solo", solo}, {"exclusive", exclusive && solo}};
	}

	bool anyChannelHas(const Json& project, const std::string& state)
	{
		const Json channels = project.value("channels", Json::array());
		return std::any_of(channels.begin(), channels.end(), [&state](const Json& channel)
			{ return channel.value(state, false) || (state == "solo" && channel.value("soloDefeat", false)); });
	}

	widgets::Options outputChoices(const Json& project, const std::string& type, uint64_t channel)
	{
		widgets::Options options;
		if (type == "midi")
		{
			options.emplace_back("0", "Not Connected");
			for (const Json& track : project.at("tracks"))
				if (track.at("type") == "instrument")
					options.emplace_back(std::to_string(track.at("channel").get<uint64_t>()), track.at("name"));
			return options;
		}
		options.emplace_back("0", "No Bus");
		for (const Json& bus : project.at("busses"))
			if (bus.at("direction") == "output")
				options.emplace_back(std::to_string(bus.at("id").get<uint64_t>()), bus.at("name"));
		for (const Json& track : project.at("tracks"))
			if ((track.at("type") == "group" || track.at("type") == "effect" || track.at("type") == "folder") &&
				track.at("channel") != 0 && track.at("channel") != channel &&
				!routesTo(project, track.at("channel").get<uint64_t>(), channel))
				options.emplace_back(std::to_string(track.at("channel").get<uint64_t>()), track.at("name"));
		return options;
	}

	widgets::Options inputChoices(const Json& project)
	{
		widgets::Options options{{"0", "No Bus"}};
		for (const Json& bus : project.at("busses"))
			if (bus.at("direction") == "input")
				options.emplace_back(std::to_string(bus.at("id").get<uint64_t>()), bus.at("name"));
		return options;
	}

	bool routesTo(const Json& project, uint64_t from, uint64_t to)
	{
		const auto channels = project.find("channels");
		if (channels == project.end())
			return false;
		std::vector<uint64_t> pending{from};
		std::set<uint64_t> seen;
		while (!pending.empty())
		{
			const uint64_t next = pending.back();
			pending.pop_back();
			if (next == to)
				return true;
			if (!seen.insert(next).second)
				continue;
			const auto channel = std::find_if(
				channels->begin(), channels->end(), [next](const Json& each) { return each.at("id") == next; });
			if (channel != channels->end())
				for (const Json& id : routing(*channel))
					if (id.is_number_integer())
						pending.push_back(id.get<uint64_t>());
		}
		return false;
	}

	void SoloHold::pressed(Rml::Element* target)
	{
		m_button = {};
		for (Rml::Element* up = target; up != nullptr; up = up->GetParentNode())
			if (up->HasAttribute("control"))
			{
				if (up->GetAttribute<Rml::String>("control", "") == "solo")
				{
					m_button = up->GetObserverPtr();
					m_at = Rml::GetSystemInterface()->GetElapsedTime();
				}
				return;
			}
	}

	bool SoloHold::held(const Rml::Element& button) const
	{
		return m_button.get() == &button && Rml::GetSystemInterface()->GetElapsedTime() - m_at >= g_seconds;
	}

	std::vector<MixerChannel> mixerChannels(const Json& project)
	{
		std::vector<MixerChannel> result;
		const auto busses = [&](const char* direction)
		{
			for (const Json& bus : project.at("busses"))
				if (bus.at("direction") == direction && bus.at("parent") == 0)
					result.push_back({bus.at("id").get<uint64_t>(), 0, direction, bus.at("name").get<std::string>(),
						nullptr, {}, {}});
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
			const Json color = track.value("color", Json());
			result.push_back({channel->first, track.at("id").get<uint64_t>(),
				trackType(track.at("type").get<std::string>()), track.at("name").get<std::string>(), channel->second,
				track.value("picture", std::string()),
				color.is_string() && relativeLuminance(color.get<std::string>()).has_value() ? color.get<std::string>()
																							 : std::string()});
		}
		busses("output");
		return result;
	}

	Mixer::Mixer(EngineBinding& binding, MeterReader& meters, Docking& docking, Rml::Element& root, std::string view,
		MixerKind kind, MixerActions actions)
		: m_binding(binding), m_stepOwner(binding.stepOwner("mixer")), m_docking(docking),
		  m_root(root.GetObserverPtr()), m_view(std::move(view)), m_kind(kind), m_actions(std::move(actions)),
		  m_meterReader(meters), m_metered(meters.show())
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
			  R"(title="Set Channel Visibility Agents">Visibility</button>)"
			  R"(<button id="mixer-find" menu="mixer-find-menu" class="icon-only" title="Find Track/Channel">)"
			  R"(<svg class="icon" src="../icons/general-search.svg"/></button>)";
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
		// A mixer window's left zone (reference manual p. 439) and its menu bar (p. 55): GNUbase's own addition to
		// the pane, for platforms whose title bar has no Show Menu Bar item.
		std::string layout = m_kind == MixerKind::Window ? R"(<button type="toggle" toggle="menuBar">Menu Bar</button>)"
														   R"(<button type="toggle" toggle="left">Left Zone</button>)"
														 : "";
		layout += R"(<div setup-presets="channel"/><button setup-preset-save="channel">Save Preset</button>)"
				  R"(<button setup-all="channel">Show All</button><button setup-reset="channel">Reset All</button>)"
				  R"(<div setup="channel"/>)";
		// The channel settings window has the Functions menu (pp. 461, 503) instead of the filter and the agents.
		std::vector<std::pair<std::string, std::string>> menus = {
			{R"(<menu id="mixer-filter-menu">)", filter},
			{R"(<menu id="mixer-agents-menu">)",
				R"(<menuitem agent="showAll">Show All Channels</menuitem><menuseparator/>)"
				R"(<menuitem agent="undo">Undo Visibility Change</menuitem>)"
				R"(<menuitem agent="redo">Redo Visibility Change</menuitem>)"},
			{R"(<menu id="mixer-find-menu">)", ""}, // showFindMenu fills it
		};
		if (settings)
			menus = {{R"(<menu id="settings-functions-menu">)",
				R"(<menuitem function="copy">Copy Channel Settings</menuitem>)"
				R"(<menuitem function="paste">Paste Channel Settings</menuitem>)"
				R"(<menuitem type="toggle" function="follow" checked="">)"
				R"(Follow Edit Buttons or Selection Changes</menuitem>)"}};
		menus.emplace_back(R"(<menu id="mixer-link-menu">)", ""); // showLinkMenu fills it
		// The channel meters' Global Meter Settings (reference manual p. 467).
		menus.emplace_back(R"(<menu id="mixer-meter-menu">)",
			R"(<menuitem type="toggle" meter="holdPeaks" checked="">Meter Peak - Hold Peaks</menuitem>)"
			R"(<menuitem type="toggle" meter="holdForever">Meter Peak - Hold Forever</menuitem><menuseparator/>)"
			R"(<menuitem meter-position="input">Meter Position - Input</menuitem>)"
			R"(<menuitem meter-position="postFader">Meter Position - Post-Fader</menuitem>)"
			R"(<menuitem meter-position="postPanner" checked="">Meter Position - Post-Panner</menuitem>)"
			R"(<menuseparator/><menuitem meter="reset">Reset Meters</menuitem>)");
		menus.emplace_back(R"(<menu id="mixer-layout-pane" class="setup-pane">)", layout);
		for (const auto& [open, items] : menus)
		{
			const Rml::ElementPtr holder = document->CreateElement("div");
			holder->SetInnerRML(open + items + "</menu>");
			Rml::Element* menu = document->AppendChild(holder->RemoveChild(holder->GetFirstChild()));
			menu->AddEventListener(Rml::EventId::Click, this);
			menu->AddEventListener(Rml::EventId::Change, this);
			m_menus.push_back(menu->GetObserverPtr());
			if (menu->GetId() == "mixer-meter-menu")
				m_meterMenu = menu->GetObserverPtr();
		}
		m_docking.arrangeSections(*m_menus.back().get()); // the setup pane, once the docking window knows the set
		for (const Rml::EventId id : g_rootEvents)
			root.AddEventListener(id, this);
		root.AddEventListener(Rml::EventId::Blur, this, true);    // the notepad's, which doesn't bubble
		root.AddEventListener(Rml::EventId::Keydown, this, true); // before the notepad takes Escape
		// Every mouseup ends a channel name's drag, also one outside the mixer.
		if (Rml::Context* context = root.GetContext())
		{
			m_window = context->GetRootElement()->GetObserverPtr();
			m_window->AddEventListener(Rml::EventId::Mouseup, this);
		}

		// Every change to the project is an undo step or a new project. A step that changed only channels, such as each
		// step of a fader drag, needs no read: their channel.changed brings them (#1881).
		for (const char* event : {"history.changed", "project.reset"})
			m_subscriptions.push_back(m_binding.on(event,
				[this, reset = std::string(event) == "project.reset"](const engine::Event& changed)
				{
					const std::vector<engine::Event> channels = m_channelSteps.settle(changed);
					if (changedOnly(changed, {"channel.changed"}))
					{
						// New routing changes which destinations the other strips' sends can take, and a new routing
						// or format which strips routed into a channel have a panner.
						bool rerouted = false;
						for (const engine::Event& each : channels)
						{
							Json before;
							if (const Json* channel = takeChannel(m_project, each, &before))
							{
								rerouted = rerouted || routing(before) != routing(*channel) ||
									before.value("format", Json()) != channel->value("format", Json());
								showChannel(channel->at("id").get<uint64_t>());
							}
						}
						if (rerouted)
							show();
						return;
					}
					requestProject();
					if (!reset)
						return;
					// A note being typed belongs to the project that went, as the inspector's edits do
					// (Inspector::dropEdits).
					m_dropping = true;
					leaveNotes();
					m_dropping = false;
					requestView();
				}));
		m_subscriptions.push_back(
			m_binding.on("channel.changed", [this](const engine::Event& event) { m_channelSteps.keep(event); }));
		m_subscriptions.push_back(m_binding.on("mixer.changed",
			[this](const engine::Event& event)
			{
				if (event.data.at("mixer") != m_view || m_viewSends > 0)
					return;
				m_hidden.clear();
				for (const Json& id : event.data.at("hidden"))
					m_hidden.insert(id.get<uint64_t>());
				m_hiddenTypes = event.data.at("hiddenTypes").get<std::set<std::string>>();
				show();
			}));
		m_subscriptions.push_back(m_binding.on("settings.changed",
			[this](const engine::Event& event)
			{
				for (const auto& [key, value] : event.data.at("values").items())
					setting(key, value);
			}));
		for (const char* key : {"audio.defaultSendLevel", "meter.holdPeaks", "meter.holdForever", "meter.peakHoldTime",
				 "meter.position", "meter.fallback"})
			m_binding.send("settings.get", {{"key", key}},
				[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
				{
					if (!alive.expired() && result.ok())
						setting(result.value.at("key").get<std::string>(), result.value.at("value"));
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
		m_subscriptions.push_back(m_binding.watch<std::vector<std::vector<std::string>>>(
			m_root.get(), [this] { return readMeters(); },
			[this](const std::vector<std::vector<std::string>>& shown) { showMeters(shown); }));
		if (!settings)
			requestView();
		requestProject();
	}

	Mixer::~Mixer()
	{
		leaveNotes(); // e.g. its window closing with F3
		if (auto* dialog = dynamic_cast<Rml::ElementDocument*>(m_linkDialog.get()))
		{
			// Nothing changes the group any more: the dialog goes too.
			dialog->RemoveEventListener(g_dialogCloseEvent, this);
			dialog->RemoveEventListener(Rml::EventId::Change, this);
			closeDialog(*dialog, "cancel");
		}
		if (Rml::Element* window = m_window.get())
			window->RemoveEventListener(Rml::EventId::Mouseup, this);
		if (Rml::Element* root = m_root.get())
		{
			for (const Rml::EventId id : g_rootEvents)
				root->RemoveEventListener(id, this);
			root->RemoveEventListener(Rml::EventId::Blur, this, true);
			root->RemoveEventListener(Rml::EventId::Keydown, this, true);
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

	// ponytail: reads the whole project after every change but the channels' (#1881), as the event display does.
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

	std::function<void(const engine::Result&)> Mixer::whenAnswered(std::string what, uint64_t channel)
	{
		return
			[this, alive = std::weak_ptr<bool>(m_alive), what = std::move(what), channel](const engine::Result& result)
		{
			if (alive.expired())
				return;
			if (!result.ok())
				requestProject();
			else if (!m_binding.sending(what))
				showChannel(channel);
		};
	}

	void Mixer::requestView()
	{
		m_binding.send("mixer.get", {{"mixer", m_view}},
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				// A newer mixer.set still out would be undone; its answer reads the view again.
				if (alive.expired() || !result.ok() || m_viewSends > 0)
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

		// Every strip has the display line while the project has a link group (reference manual p. 458).
		m_strips->SetClass("link-groups", !m_project.value("linkGroups", Json::array()).empty());
		if (Rml::Element* filter = m_filter.get())
			filter->SetClass("checked", !m_hiddenTypes.empty()); // the button shows that a type is hidden
		showStates();
		if (Rml::Element* menu = m_menus.front().get())
			for (const auto& [type, label] : g_types)
				if (Rml::Element* item = menu->QuerySelector(std::string("[channel-type=") + type + "]"))
					setChecked(*item, m_hiddenTypes.count(type) == 0);
		showVisibilityList();
		scrollToRevealed();
		if (Rml::Context* context = strips->GetContext())
			context->RequestNextUpdate(0);
	}

	void Mixer::showStates()
	{
		// The Deactivate All buttons light up while any channel has their state.
		for (const auto& [state, label] : g_states)
		{
			Rml::Element* button = m_root.get() != nullptr
				? m_root.get()->QuerySelector(std::string(".mixer-toolbar button[deactivate=") + state + "]")
				: nullptr;
			if (button == nullptr)
				continue;
			button->SetClass("checked", anyChannelHas(m_project, state));
		}
	}

	void Mixer::showChannel(uint64_t id)
	{
		// What depends on one channel's JSON: its strip, the Deactivate All buttons and Go to Output; the other strips
		// take only names and types from it, which are the tracks' (mixerChannels).
		const auto strip = m_stripOf.find(id);
		const auto channel = std::find_if(
			m_channels.begin(), m_channels.end(), [id](const MixerChannel& each) { return each.id == id; });
		if (strip != m_stripOf.end() && channel != m_channels.end())
			updateStrip(*strip->second, *channel);
		showStates();
		if (m_kind == MixerKind::ChannelSettings)
			showSettingsToolbar();
		if (const Rml::Element* strips = m_strips.get(); strips != nullptr && strips->GetContext() != nullptr)
			strips->GetContext()->RequestNextUpdate(0);
	}

	void Mixer::buildStrips(const std::vector<const MixerChannel*>& strips)
	{
		leaveNotes();
		Rml::Element& container = *m_strips.get();
		while (container.GetNumChildren() > 0)
			container.RemoveChild(container.GetFirstChild());
		m_stripOf.clear();
		m_metered->clear();
		Rml::ElementDocument* document = container.GetOwnerDocument();
		for (const MixerChannel* channel : strips)
		{
			const bool audio = channel->channel != nullptr && channel->channel->at("format") != "none";
			const bool vca = channel->type == "vca"; // a fader and buttons (reference manual p. 515)
			// The display line opens its menu, and a double click types its group's name (p. 459).
			std::string rml = R"(<div class="mixer-channel-name"/><textdisplay class="mixer-link")" +
				std::string(channel->channel != nullptr ? R"( menu="mixer-link-menu" title="Link Settings")" : "") +
				R"(/><div class="mixer-picture" section="picture" label="Pictures" default="hidden"/>)";
			// The Routing section, only in the mixers (reference manual p. 471): an audio track's input bus, and where
			// the channel plays.
			if (m_kind != MixerKind::ChannelSettings)
				rml += std::string(R"(<div class="mixer-section mixer-routing" section="routing" label="Routing" )"
								   R"(default="hidden">)") +
					(channel->type == "audio"
							? R"(<div class="mixer-row"><select param="input" title="Input Routing"/></div>)"
							: "") +
					(channel->channel != nullptr && !vca
							? R"(<div class="mixer-row"><select param="output" title="Output Routing"/></div>)"
							: "") +
					"</div>";
			rml += sectionsRml(audio, m_kind == MixerKind::ChannelSettings);
			rml += R"(<div class="mixer-notes" section="notes" label="Notepad" default="hidden">)";
			if (channel->channel != nullptr)
				rml += R"(<textarea class="mixer-notes-field" rows="4"/>)";
			rml += R"(</div><div class="mixer-fader" section="fader" label="Fader">)";
			// The panner (reference manual p. 465): pan bypass, and the pan control or, while the strip is .combined,
			// the combined panner's left and right controls.
			if (channel->type == "midi") // MIDI pan, CC 10
				rml += R"(<div class="mixer-panner"><knob class="mixer-pan" param="midiPan" title="Pan" min="0" )"
					   R"(max="127" default="64" step="1"/></div>)";
			if (audio)
				rml += R"(<div class="mixer-panner"><button type="toggle" class="toggle" param="panBypass" )"
					   R"(title="Pan Bypass">B</button>)"
					   R"(<knob class="mixer-pan" param="pan" title="Pan" min="-1" max="1" default="0" step="0.01"/>)"
					   R"(<knob class="mixer-pan mixer-pan-side" param="panLeft" title="Pan Left" min="-1" max="1" )"
					   R"(default="-1" step="0.01"/>)"
					   R"(<knob class="mixer-pan mixer-pan-side" param="panRight" title="Pan Right" min="-1" max="1" )"
					   R"(default="1" step="0.01"/></div>)";
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
			// The fader, from -60 dB, linear until the channel strip's dB law (#107), and the meter of an audio or MIDI
			// channel or of a VCA fader's channels summed (p. 516), whose bars come once the engine measured it
			// (showMeters). MIDI channels have no fader here yet.
			const bool midi = channel->type == "midi";
			if (audio || vca || midi)
			{
				rml += std::string(R"(<div class="mixer-level">)") +
					(midi ? "" : R"(<fader min="-60" max="6" default="0" step="0.1"/>)") +
					R"(<div class="mixer-meter" contextmenu="mixer-meter-menu"/></div>)"
					R"(<div class="mixer-readouts"><span class="mixer-volume"/>)"
					R"(<span class="mixer-peak" title="Meter Peak Level"/></div>)";
				m_metered->push_back(channel->id);
			}
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
		showMeters(readMeters()); // the watch shows only what changes
	}

	std::vector<std::vector<std::string>> Mixer::readMeters()
	{
		const auto now = std::chrono::steady_clock::now();
		// The bars fall back from the peaks by meter.fallback (p. 1614), and below their scale to silence.
		const auto fall = static_cast<float>(
			std::pow(10.0, -m_fallback * std::chrono::duration<double>(now - m_metersRead).count() / 20.0));
		m_metersRead = now;
		std::vector<std::vector<std::string>> shown;
		for (const uint64_t id : *m_metered)
		{
			std::vector<std::string> values;
			const engine::LevelReading* level = m_meterReader.level(id);
			std::vector<HeldPeak>& held = m_held[id];
			float largest = 0.0f;
			for (size_t c = 0; level != nullptr && c < static_cast<size_t>(level->channels); ++c)
			{
				// Without Hold Forever the mark holds a peak for the peak hold time (p. 1614); below the maximum, a
				// reset cleared, it starts again.
				held.resize(std::max(held.size(), c + 1));
				HeldPeak& mark = held[c];
				if (level->peak[c] >= mark.level || mark.level > level->peakMax[c] || now - mark.at > m_peakHoldTime)
				{
					mark.level = level->peak[c];
					mark.at = now;
				}
				mark.bar = std::max(level->peak[c], mark.bar * fall);
				if (mark.bar < g_meterEnd)
					mark.bar = level->peak[c];
				values.push_back(meterValue(mark.bar));
				values.push_back(m_holdPeaks ? meterValue(m_holdForever ? level->peakMax[c] : mark.level) : "");
				largest = std::max(largest, level->peakMax[c]);
			}
			const std::optional<double> peak = engine::analysis::decibels(largest);
			values.push_back(peak ? decibels(*peak) : "-inf");
			shown.push_back(std::move(values));
		}
		return shown;
	}

	void Mixer::showMeters(const std::vector<std::vector<std::string>>& shown)
	{
		if (!m_strips)
			return;
		for (size_t i = 0; i < shown.size() && i < m_metered->size(); ++i)
		{
			const auto strip = m_stripOf.find((*m_metered)[i]);
			Rml::Element* bars = strip != m_stripOf.end() ? strip->second->QuerySelector(".mixer-meter") : nullptr;
			Rml::Element* peak = strip != m_stripOf.end() ? strip->second->QuerySelector(".mixer-peak") : nullptr;
			if (bars == nullptr || peak == nullptr)
				continue;
			const std::vector<std::string>& values = shown[i];
			const auto channels = static_cast<int>(values.size() / 2);
			if (bars->GetNumChildren() != channels)
			{
				std::string rml;
				for (int c = 0; c < channels; ++c)
					rml += R"(<meter min="-60" max="6"/>)";
				bars->SetInnerRML(rml);
			}
			for (int c = 0; c < channels; ++c)
			{
				Rml::Element* meter = bars->GetChild(c);
				meter->SetAttribute("value", values[2 * static_cast<size_t>(c)]);
				if (const std::string& mark = values[(2 * static_cast<size_t>(c)) + 1]; mark.empty())
					meter->RemoveAttribute("peak");
				else
					meter->SetAttribute("peak", mark);
			}
			setText(*peak, values.back());
		}
	}

	void Mixer::setting(const std::string& key, const Json& value)
	{
		if (key == "audio.defaultSendLevel") // what a Ctrl-click gives a send (reference manual p. 552)
		{
			m_defaultSendLevel = value.get<double>();
			show();
			return;
		}
		if (key == "meter.position")
		{
			// The engine measures there (meter.get): the menu only shows where.
			Rml::ElementList items;
			if (m_meterMenu)
				m_meterMenu->QuerySelectorAll(items, "[meter-position]");
			for (Rml::Element* item : items)
				setChecked(*item, attribute(*item, "meter-position") == value.get<std::string>());
			return;
		}
		if (key == "meter.holdPeaks")
			m_holdPeaks = value.get<bool>();
		else if (key == "meter.holdForever")
			m_holdForever = value.get<bool>();
		else if (key == "meter.peakHoldTime")
			m_peakHoldTime = std::chrono::milliseconds(value.get<int64_t>());
		else if (key == "meter.fallback")
			m_fallback = value.get<double>();
		else
			return;
		for (const auto& [item, on] : {std::pair{"holdPeaks", m_holdPeaks}, std::pair{"holdForever", m_holdForever}})
			if (Rml::Element* toggle =
					m_meterMenu ? m_meterMenu->QuerySelector(std::string("[meter=") + item + "]") : nullptr)
				setChecked(*toggle, on);
	}

	void Mixer::updateStrip(Rml::Element& strip, const MixerChannel& channel)
	{
		if (Rml::Element* name = strip.QuerySelector(".mixer-channel-name"))
		{
			setText(*name, channel.name);
			// The track's color (reference manual p. 1621) is user data, which the style sheets don't know; they
			// style the text over it by the classes.
			if (name->GetAttribute<Rml::String>("color", "") != channel.color)
			{
				if (channel.color.empty())
				{
					name->RemoveAttribute("color");
					name->RemoveProperty("background-color");
				}
				else
				{
					name->SetAttribute("color", channel.color);
					name->SetProperty("background-color", channel.color);
				}
				name->SetClass("colored", !channel.color.empty());
				name->SetClass("bright", bright(channel.color));
			}
		}
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
		if (Rml::Element* link = strip.QuerySelector(".mixer-link"); link != nullptr && !link->IsClassSet("editing"))
		{
			const auto [group, number] = linkGroup(m_project, channel.id, "channels");
			const std::string text =
				group != nullptr ? std::to_string(number) + " " + group->at("name").get<std::string>() : "";
			if (attribute(*link, "value") != text)
				link->SetAttribute("value", text);
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
		strip.SetClass("combined", json.value("panner", std::string()) == "combined");
		strip.SetClass("no-panner", json.at("format") != "none" && !hasPanner(m_project, json));
		Rml::ElementList controls;
		strip.QuerySelectorAll(controls, "[param]");
		m_updating = true; // filling a drop-down list sends a change
		// A send level's Ctrl-click value follows the default send level, also while its knob is ahead. The knob goes
		// up to the project's maximum volume, as the fader does, and down to the default send level below its usual
		// -60 dB, so that it gives a send the level track.add gives it.
		const std::string level = widgets::format(static_cast<float>(m_defaultSendLevel));
		const std::string lowest = widgets::format(static_cast<float>(std::min(m_defaultSendLevel, g_sendKnobMin)));
		const std::string highest = widgets::format(m_project.value("maxVolume", 6.0F));
		const widgets::Options destinations =
			sendDestinations(m_project, m_channels, channel.id, channel.track != 0 && m_actions.addEffectTrack);
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
			Json value = paramValue(json, param);
			if (param == "midiPan" && value.is_null())
				value = 64; // no MIDI pan, as before the first change: the knob shows its default, the center
			if (param == "input" || param == "output")
			{
				if (value.is_number())
					widgets::fill(*control,
						param == "input" ? inputChoices(m_project) : outputChoices(m_project, channel.type, channel.id),
						std::to_string(value.get<uint64_t>()));
			}
			else if (control->GetTagName() == "select" && param.rfind("sends.", 0) != 0)
			{
				if (!value.is_null())
					widgets::fill(*control, sectionChoices(param),
						value.is_string() ? value.get<std::string>() : std::to_string(value.get<int>()));
			}
			else if (control->GetTagName() == "select")
			{
				if (value.is_number())
					widgets::fill(*control, destinations, std::to_string(value.get<uint64_t>()));
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
		// MIDI and VCA channels have no settings to copy or paste, which the engine refuses.
		const bool settings = shown != nullptr && shown->channel != nullptr && shown->channel->at("format") != "none" &&
			shown->type != "vca";
		Rml::Element* functions = m_menus.front().get();
		for (const char* function : {"copy", "paste"})
			if (Rml::Element* item = functions != nullptr
					? functions->QuerySelector(std::string("menuitem[function=") + function + "]")
					: nullptr)
			{
				if (settings)
					item->RemoveAttribute("disabled");
				else
					item->SetAttribute("disabled", "");
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
		if (hidden)
			m_hidden.insert(id);
		else
			m_hidden.erase(id);
		sendView({{"hidden", m_hidden}});
	}

	void Mixer::setTypeHidden(const std::string& type, bool hidden)
	{
		if (hidden)
			m_hiddenTypes.insert(type);
		else
			m_hiddenTypes.erase(type);
		sendView({{"hiddenTypes", m_hiddenTypes}});
	}

	void Mixer::leaveNotes()
	{
		Rml::Context* context = m_root ? m_root->GetContext() : nullptr;
		if (Rml::Element* focus = context != nullptr ? context->GetFocusElement() : nullptr;
			focus != nullptr && focus->IsClassSet("mixer-notes-field") && m_root->Contains(focus))
			focus->Blur();
	}

	void Mixer::sendView(Json parameters)
	{
		parameters["mixer"] = m_view;
		// The sets changed at once, so that the next change made before the answer keeps this one; their mixer.changed
		// would bring them back as they were for a moment, so once the last is answered the mixer reads the view and
		// shows it (not at once: a visibility check box's change would remake the list it is in).
		++m_viewSends;
		m_binding.send("mixer.set", std::move(parameters),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result&)
			{
				if (!alive.expired() && --m_viewSends == 0)
					requestView();
			});
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

	void Mixer::showFindMenu()
	{
		const auto menu = std::find_if(m_menus.begin(), m_menus.end(),
			[](const Rml::ObserverPtr<Rml::Element>& each) { return each && each->GetId() == "mixer-find-menu"; });
		if (menu == m_menus.end())
			return;
		std::string rml = R"(<input type="text" class="mixer-find-search" placeholder="Search"/>)";
		for (const MixerChannel& channel : m_channels)
			if (m_hiddenTypes.count(channel.type) == 0)
				rml +=
					R"(<menuitem find=")" + std::to_string(channel.id) + R"(">)" + encode(channel.name) + "</menuitem>";
		(*menu)->SetInnerRML(rml);
	}

	void Mixer::find(const std::string& text)
	{
		const auto lower = [](std::string name)
		{
			std::transform(name.begin(), name.end(), name.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return name;
		};
		const auto menu = std::find_if(m_menus.begin(), m_menus.end(),
			[](const Rml::ObserverPtr<Rml::Element>& each) { return each && each->GetId() == "mixer-find-menu"; });
		if (menu == m_menus.end())
			return;
		for (int i = 0; i < (*menu)->GetNumChildren(); ++i)
			if (Rml::Element* item = (*menu)->GetChild(i); item->HasAttribute("find"))
			{
				// Matched against the names, not the items' RML, where & < > are entities.
				const uint64_t id = idAttribute(*item, "find");
				const auto channel = std::find_if(
					m_channels.begin(), m_channels.end(), [id](const MixerChannel& each) { return each.id == id; });
				item->SetClass("hidden",
					channel == m_channels.end() || lower(channel->name).find(lower(text)) == std::string::npos);
			}
	}

	void Mixer::reveal(uint64_t channel)
	{
		if (m_hidden.count(channel) != 0)
			setHidden(channel, false);
		m_revealing = channel;
		scrollToRevealed();
	}

	void Mixer::scrollToRevealed()
	{
		const auto strip = m_stripOf.find(m_revealing);
		if (strip == m_stripOf.end())
			return;
		if (Rml::ElementDocument* document = strip->second->GetOwnerDocument())
			document->UpdateDocument(); // lays out a strip just made
		strip->second->ScrollIntoView({Rml::ScrollAlignment::Nearest, Rml::ScrollAlignment::Nearest,
			Rml::ScrollBehavior::Instant, Rml::ScrollParentage::Closest});
		m_revealing = 0;
	}

	void Mixer::showLinkMenu(uint64_t channel)
	{
		const auto menu = std::find_if(m_menus.begin(), m_menus.end(),
			[](const Rml::ObserverPtr<Rml::Element>& each) { return each && each->GetId() == "mixer-link-menu"; });
		if (menu == m_menus.end())
			return;
		m_linkChannel = channel;
		const Json* group = linkGroup(m_project, channel, "channels").first;
		const auto item = [](const std::string& link, const std::string& label, bool checked)
		{
			return R"(<menuitem type="toggle" link=")" + link + '"' + (checked ? R"( checked="")" : "") + ">" +
				encode(label) + "</menuitem>";
		};
		// Unlink Selected Channels unlinks the line's group, as the mixer has no channel selection yet.
		std::string rml = group != nullptr ? R"(<menuitem link="unlink">Unlink Selected Channels</menuitem>)"
											 R"(<menuitem link="settings">Edit Link Group Settings</menuitem>)"
										   : "";
		rml += "<menuitem>Included in Link Group: " +
			encode(group != nullptr ? group->at("name").get<std::string>() : "<None>") + "<menu>" +
			item("0", "None", group == nullptr);
		size_t number = 0;
		for (const Json& each : m_project.value("linkGroups", Json::array()))
			if (++number; each.at("vca") != channel) // a VCA fader can't be in the group it controls
				rml += item(each.at("id").dump(), std::to_string(number) + " " + each.at("name").get<std::string>(),
					group != nullptr && each.at("id") == group->at("id"));
		rml += "</menu></menuitem>";
		if (group != nullptr)
		{
			const Json& channels = group->at("channels");
			rml += "<menuitem>Linked Channels<menu>";
			for (const MixerChannel& linked : m_channels)
				if (std::find(channels.begin(), channels.end(), Json(linked.id)) != channels.end())
					rml += R"(<menuitem disabled="">)" + encode(linked.name) + "</menuitem>";
			rml += "</menu></menuitem>";
		}
		(*menu)->SetInnerRML(rml);
	}

	// Included in Link Group (pp. 460-461) moves the channel into the group chosen, or with None out of its group,
	// which goes with its last channel; Unlink Selected Channels removes the group.
	void Mixer::link(const std::string& item)
	{
		const Json* group = linkGroup(m_project, m_linkChannel, "channels").first;
		if (item == "settings")
		{
			if (group != nullptr)
				openLinkSettings(group->at("id").get<uint64_t>());
			return;
		}
		if (item == "unlink" || item == "0")
		{
			if (group == nullptr)
				return;
			std::vector<uint64_t> channels = group->at("channels").get<std::vector<uint64_t>>();
			channels.erase(std::remove(channels.begin(), channels.end(), m_linkChannel), channels.end());
			if (item == "unlink" || channels.empty())
				m_binding.send("linkGroup.remove", {{"group", group->at("id")}});
			else
				m_binding.send("linkGroup.set", {{"group", group->at("id")}, {"channels", channels}});
			return;
		}
		for (const Json& each : m_project.value("linkGroups", Json::array()))
			if (each.at("id").dump() == item && (group == nullptr || each.at("id") != group->at("id")))
			{
				std::vector<uint64_t> channels = each.at("channels").get<std::vector<uint64_t>>();
				channels.push_back(m_linkChannel);
				m_binding.send("linkGroup.set", {{"group", each.at("id")}, {"channels", channels}});
			}
	}

	void Mixer::openLinkSettings(uint64_t id)
	{
		const Json groups = m_project.value("linkGroups", Json::array());
		const auto group =
			std::find_if(groups.begin(), groups.end(), [id](const Json& each) { return each.at("id") == id; });
		if (m_linkDialog || group == groups.end() || !m_root || m_root->GetContext() == nullptr)
			return;
		const std::string path = std::filesystem::u8path(m_root->GetOwnerDocument()->GetSourceURL())
									 .replace_filename("link-group-settings.rml")
									 .generic_u8string();
		Rml::ElementDocument* dialog = m_root->GetContext()->LoadDocument(path);
		if (dialog == nullptr)
			return;
		m_linkDialog = dialog->GetObserverPtr();
		m_linkDialogGroup = id;
		const auto field = [dialog](const std::string& link) { return dialog->QuerySelector("[link=" + link + "]"); };
		if (auto* name = dynamic_cast<Rml::ElementFormControl*>(field("name")))
			name->SetValue(group->at("name").get<std::string>());
		// The VCA faders that can control the group: not those in it (p. 515).
		const Json& channels = group->at("channels");
		widgets::Options faders;
		for (const MixerChannel& channel : m_channels)
			if (channel.type == "vca" &&
				std::find(channels.begin(), channels.end(), Json(channel.id)) == channels.end())
				faders.emplace_back(std::to_string(channel.id), channel.name);
		const auto vca = group->at("vca").get<uint64_t>();
		std::string fader = vca != 0 ? std::to_string(vca) : std::string();
		if (fader.empty() && !faders.empty())
			fader = faders.front().first;
		if (Rml::Element* select = field("vcaFader"))
			widgets::fill(*select, faders, fader);
		for (const std::string link : {"vca", "volume", "pan", "eq", "dynamics", "sends", "mute", "record"})
			if (Rml::Element* box = field(link))
			{
				if (link == "vca" ? vca != 0 : group->value(link, false))
					box->SetAttribute("checked", "");
				else
					box->RemoveAttribute("checked");
				if (link == "vca" && faders.empty())
					box->SetAttribute("disabled", "");
			}
		showLinkSettings(false);
		dialog->AddEventListener(g_dialogCloseEvent, this);
		dialog->AddEventListener(Rml::EventId::Change, this);
		dialog->Show(Rml::ModalFlag::Modal);
	}

	void Mixer::showLinkSettings(bool vcaChanged)
	{
		Rml::Element* dialog = m_linkDialog.get();
		const Rml::Element* vca = dialog != nullptr ? dialog->QuerySelector("[link=vca]") : nullptr;
		if (vca == nullptr)
			return;
		const bool on = vca->HasAttribute("checked");
		const auto enable = [](Rml::Element* element, bool enabled)
		{
			if (element == nullptr)
				return;
			if (enabled)
				element->RemoveAttribute("disabled");
			else
				element->SetAttribute("disabled", "");
		};
		enable(dialog->QuerySelector("[link=vcaFader]"), on);
		Rml::Element* volume = dialog->QuerySelector("[link=volume]");
		enable(volume, !on);
		if (volume != nullptr && vcaChanged)
		{
			if (on)
				volume->RemoveAttribute("checked");
			else
				volume->SetAttribute("checked", "");
		}
	}

	void Mixer::linkSettingsEvent(Rml::Event& event)
	{
		Rml::Element* dialog = m_linkDialog.get();
		if (event.GetType() != g_dialogCloseEvent)
		{
			if (const Rml::Element* target = event.GetTargetElement();
				target != nullptr && attribute(*target, "link") == "vca")
				showLinkSettings(true);
			return;
		}
		dialog->RemoveEventListener(g_dialogCloseEvent, this);
		dialog->RemoveEventListener(Rml::EventId::Change, this);
		m_linkDialog.reset();
		if (event.GetParameter<Rml::String>("result", "") != "ok")
			return;
		const auto field = [dialog](const std::string& link) { return dialog->QuerySelector("[link=" + link + "]"); };
		const auto checked = [&field](const std::string& link)
		{
			const Rml::Element* box = field(link);
			return box != nullptr && box->HasAttribute("checked");
		};
		Json parameters = {{"group", m_linkDialogGroup}};
		if (const auto* name = dynamic_cast<Rml::ElementFormControl*>(field("name"));
			name != nullptr && !name->GetValue().empty())
			parameters["name"] = name->GetValue();
		const auto* fader = dynamic_cast<Rml::ElementFormControl*>(field("vcaFader"));
		const bool vca = checked("vca") && fader != nullptr && !fader->GetValue().empty();
		parameters["vca"] = vca ? std::stoull(fader->GetValue()) : 0;
		for (const char* link : {"pan", "eq", "dynamics", "sends", "mute", "record"})
			parameters[link] = checked(link);
		if (!vca)
			parameters["volume"] = checked("volume");
		m_binding.send("linkGroup.set", std::move(parameters));
	}

	void Mixer::ProcessEvent(Rml::Event& event)
	{
		if (m_linkDialog && event.GetCurrentElement() == m_linkDialog.get())
		{
			linkSettingsEvent(event);
			return;
		}
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
			else if (target->IsClassSet("mixer-find-search"))
			{
				find(event.GetParameter<Rml::String>("value", ""));
			}
			else if (attribute(*target, "function") == "follow")
			{
				m_follows = event.GetParameter("value", true);
			}
			else if (target->HasAttribute("meter") && attribute(*target, "meter") != "reset")
			{
				m_binding.send("settings.set",
					{{"key", "meter." + attribute(*target, "meter")}, {"value", event.GetParameter("value", true)}});
			}
			else if (target->IsClassSet("mixer-link"))
			{
				// The name typed into the display line (p. 459), which starts with the group's number and the name.
				const MixerChannel* channel = channelOf(target);
				const auto [group, number] = channel != nullptr ? linkGroup(m_project, channel->id, "channels")
																: std::pair<const Json*, size_t>{nullptr, 0};
				auto name = event.GetParameter<Rml::String>("value", "");
				if (const std::string prefix = std::to_string(number) + " "; name.rfind(prefix, 0) == 0)
					name.erase(0, prefix.size());
				if (group != nullptr && !name.empty() && name != group->at("name"))
					m_binding.send("linkGroup.set", {{"group", group->at("id")}, {"name", name}});
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
					const std::string what = stepKey(m_stepOwner, channel->id, "volume");
					m_binding.sendStep(event.GetParameter("gesture", 0), what, "Change Volume",
						Json::array({Json{{"command", "channel.set"},
							{"parameters", {{"channel", channel->id}, {"volume", volume}}}}}),
						whenAnswered(what, channel->id));
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
						// A toggle, an EQ band's type, or a send's destination, a channel id, or a cut's slope.
						Json value;
						if (given->GetType() == Rml::Variant::BOOL)
							value = given->Get<bool>();
						else if (endsWith(param, ".type"))
							value = given->Get<Rml::String>();
						else
							value = std::stoull(given->Get<Rml::String>("0"));
						Json parameters = paramChange(param, std::move(value));
						parameters["channel"] = channel->id;
						m_binding.send("channel.set", std::move(parameters));
						break;
					}
					// A knob's or a value field's drag, wheel turns or held arrow key: one undo step. MIDI pan is a
					// MIDI value.
					Json parameters = paramChange(param,
						param == "midiPan" ? Json(std::lround(given->Get<double>())) : Json(given->Get<double>()));
					parameters["channel"] = channel->id;
					const std::string what = stepKey(m_stepOwner, channel->id, param);
					m_binding.sendStep(event.GetParameter("gesture", 0), what, "Change " + attribute(*target, "title"),
						Json::array({Json{{"command", "channel.set"}, {"parameters", std::move(parameters)}}}),
						whenAnswered(what, channel->id));
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
				m_hidden.clear();
				sendView({{"hidden", Json::array()}});
			}
			else if (const std::string agent = attribute(*target, "agent"); agent == "undo" || agent == "redo")
			{
				m_binding.send(agent == "undo" ? "mixer.undoVisibility" : "mixer.redoVisibility", {{"mixer", m_view}});
			}
			else if (target->HasAttribute("link"))
			{
				link(attribute(*target, "link"));
			}
			else if (target->HasAttribute("find"))
			{
				reveal(idAttribute(*target, "find"));
			}
			else if (attribute(*target, "function") == "copy" && !target->HasAttribute("disabled"))
			{
				if (const MixerChannel* channel = channelOf(m_stripOf.empty() ? nullptr : m_stripOf.begin()->second))
					m_binding.send("channel.copySettings", {{"channel", channel->id}});
			}
			else if (attribute(*target, "function") == "paste" && !target->HasAttribute("disabled"))
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
			else if (target->HasAttribute("meter-position"))
			{
				m_binding.send(
					"settings.set", {{"key", "meter.position"}, {"value", attribute(*target, "meter-position")}});
			}
			else if (target->IsClassSet("mixer-peak") || attribute(*target, "meter") == "reset")
			{
				m_binding.send("meter.reset", {{"meters", "channels"}}); // the peak level (p. 468)
			}
			else if (target->HasAttribute("deactivate"))
			{
				m_binding.send("channel.deactivateAll", {{"state", attribute(*target, "deactivate")}});
			}
			else if (target->HasAttribute("control"))
			{
				if (const MixerChannel* channel = channelOf(target); channel != nullptr && channel->channel != nullptr)
				{
					// Solo: Ctrl-click (Cmd on macOS) solos exclusively, Alt-click and click and hold switch solo
					// defeat (p. 466).
					const std::string control = attribute(*target, "control");
					Json parameters = control == "solo"
						? soloClick(*channel->channel, held(event, "ctrl_key") || held(event, "meta_key"),
							  held(event, "alt_key") || m_soloHold.held(*target))
						: Json{{control, !channel->channel->at(control).get<bool>()}};
					parameters["channel"] = channel->id;
					m_binding.send("channel.set", std::move(parameters));
				}
			}
			break;
		case Rml::EventId::Dblclick:
			// A double click with a modifier key on the display line opens the Link Group Settings dialog (p. 459)
			// instead of typing the group's name, the text display's default action.
			if (Rml::Element* line = closest(target, "mixer-link", m_root.get()); line != nullptr &&
				(held(event, "ctrl_key") || held(event, "meta_key") || held(event, "alt_key") ||
					held(event, "shift_key")))
			{
				if (const MixerChannel* channel = channelOf(line))
					if (const Json* group = linkGroup(m_project, channel->id, "channels").first)
					{
						event.StopPropagation();
						openLinkSettings(group->at("id").get<uint64_t>());
					}
				break;
			}
			if (closest(target, "mixer-picture", m_root.get()) != nullptr)
				if (const MixerChannel* channel = channelOf(target); channel != nullptr && channel->track != 0)
					if (m_actions.choosePicture)
						m_actions.choosePicture(*target, channel->track);
			break;
		case Rml::EventId::Mousedown:
		{
			const MixerChannel* channel =
				closest(target, "mixer-channel-name", m_root.get()) != nullptr ? channelOf(target) : nullptr;
			m_dragged = channel != nullptr ? channel->id : 0;
			m_soloHold.pressed(target);
			if (target->GetId() == "mixer-find" ||
				(target->GetParentNode() != nullptr && target->GetParentNode()->GetId() == "mixer-find"))
				showFindMenu(); // before the popup behavior opens it on the click
			// The press fills the display line's menu, which the popup behavior opens on the click, as the list
			// editor's menus do.
			if (Rml::Element* line = closest(target, "mixer-link", m_root.get());
				line != nullptr && event.GetParameter("button", 0) == 0)
				if (const MixerChannel* pressed = channelOf(line); pressed != nullptr && pressed->channel != nullptr)
					showLinkMenu(pressed->id);
			break;
		}
		case Rml::EventId::Mouseup: // anywhere in the window
			if (const MixerChannel* channel = m_root && m_root->Contains(target) ? channelOf(target) : nullptr;
				channel != nullptr && m_dragged != 0)
				drop(channel->id);
			m_dragged = 0;
			break;
		case Rml::EventId::Keydown:
			// Escape closes the notepad (reference manual p. 498), keeping what was typed.
			if (target->IsClassSet("mixer-notes-field") &&
				event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN) == Rml::Input::KI_ESCAPE)
			{
				target->Blur();
				event.StopPropagation();
			}
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
			if (target->IsClassSet("mixer-notes-field") && !m_dropping)
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
