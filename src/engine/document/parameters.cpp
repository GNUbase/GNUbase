// SPDX-License-Identifier: GPL-3.0-or-later
// Parameters (#30): parameter.list, parameter.get and parameter.set read and change the static value of every
// automatable parameter of a channel: the fader's volume, pan, panLeft and panRight, mute, the parameters of an
// instrument track's built-in instrument, the channel strip's sections and the built-in plugins in its slots. They name
// parameters by the targets automation lanes take, and give values with their display text. parameter.set is one undo
// step and changes nothing if it fails (ProjectModel::add); playback takes the values from the render snapshot like
// those of channel.set.
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/document/stripparameters.h"
#include "engine/engine.h"
#include "engine/processors/builtin.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		std::string idText(ObjectId id)
		{
			return std::to_string(id);
		}

		Json objectSchema(Json properties, const std::vector<std::string>& required = {})
		{
			return {{"type", "object"}, {"properties", std::move(properties)}, {"required", required},
				{"additionalProperties", false}};
		}

		Channel& findChannel(Project& project, const Json& parameters)
		{
			const auto id = parameters.at("channel").get<ObjectId>();
			const auto found = project.channels.find(id);
			require(found != project.channels.end(), "no channel " + idText(id));
			return found->second;
		}

		struct ChannelParameter
		{
			AutomationTarget target;
			processors::ParameterInfo info; // its range, default, unit and options
		};

		// A channel's parameters, in the order parameter.list returns them.
		std::vector<ChannelParameter> channelParameters(const Project& project, const Channel& channel)
		{
			std::vector<ChannelParameter> parameters;
			if (channel.format != ChannelFormat::None)
			{
				const auto pan = [](const char* name, double defaultValue)
				{ return processors::ParameterInfo{0, name, -1.0, 1.0, defaultValue, {}, false, {}}; };
				parameters.push_back({{AutomationTargetType::Volume, 0, 0, 0, 0},
					{0, "Volume", g_minChannelVolume, project.maxVolume, 0.0, "dB", false, {}}});
				parameters.push_back({{AutomationTargetType::Pan, 0, 0, 0, 0}, pan("Pan", 0.0)});
				parameters.push_back({{AutomationTargetType::PanLeft, 0, 0, 0, 0}, pan("Pan left", -1.0)});
				parameters.push_back({{AutomationTargetType::PanRight, 0, 0, 0, 0}, pan("Pan right", 1.0)});
				parameters.push_back(
					{{AutomationTargetType::Mute, 0, 0, 0, 0}, {0, "Mute", 0.0, 1.0, 0.0, {}, true, {"Off", "On"}}});
			}
			else if (project.trackById.at(channel.track).type == TrackType::Vca) // a VCA fader: its fader only
			{
				parameters.push_back({{AutomationTargetType::Volume, 0, 0, 0, 0},
					{0, "Volume", g_minChannelVolume, project.maxVolume, 0.0, "dB", false, {}}});
			}
			if (const processors::BuiltinInfo* instrument = processors::findBuiltin(channel.instrument))
				for (const processors::ParameterInfo& parameter : instrument->parameters)
					parameters.push_back({{AutomationTargetType::Instrument, parameter.id, 0, 0, 0}, parameter});
			if (channel.format != ChannelFormat::None)
				for (const processors::ParameterInfo& parameter : stripParameters())
					parameters.push_back({{AutomationTargetType::Strip, parameter.id, 0, 0, 0}, parameter});
			// The plugins in its slots, top to bottom: built-in ones; others report theirs once they run (#283).
			for (const ObjectId id : channel.slots)
			{
				if (const processors::BuiltinInfo* plugin = processors::findBuiltinPlugin(project.slots.at(id).plugin))
					for (const processors::ParameterInfo& parameter : plugin->parameters)
						parameters.push_back({{AutomationTargetType::Plugin, parameter.id, 0, 0, id}, parameter});
			}
			return parameters;
		}

		// The target a command names, which must be one parameter.list can return.
		AutomationTarget target(const Json& json)
		{
			require(
				json.is_object() && json.contains("type") && json.at("type").is_string(), "target.type is required");
			const std::optional<AutomationTargetType> type =
				automationTargetTypeFromName(json.at("type").get<std::string>());
			if (!type.has_value() ||
				(*type != AutomationTargetType::Volume && *type != AutomationTargetType::Pan &&
					*type != AutomationTargetType::PanLeft && *type != AutomationTargetType::PanRight &&
					*type != AutomationTargetType::Mute && *type != AutomationTargetType::Instrument &&
					*type != AutomationTargetType::Plugin && *type != AutomationTargetType::Strip))
				throw CommandError(
					"target.type must be volume, pan, panLeft, panRight, mute, instrument, plugin or strip");
			const bool plugin = *type == AutomationTargetType::Plugin;
			const bool parameter =
				*type == AutomationTargetType::Instrument || *type == AutomationTargetType::Strip || plugin;
			for (const auto& item : json.items())
				require(item.key() == "type" || (item.key() == "parameter" && parameter) ||
						(item.key() == "slot" && plugin),
					"target." + item.key() + " doesn't belong to a " + json.at("type").get<std::string>() + " target");
			AutomationTarget result;
			result.type = *type;
			if (parameter)
			{
				require(json.contains("parameter") && isParameterId(json.at("parameter")),
					"target.parameter must be a parameter id (see parameter.list)");
				result.parameter = json.at("parameter").get<uint32_t>();
			}
			if (plugin)
			{
				require(json.contains("slot") && json.at("slot").is_number_unsigned(),
					"target.slot must be a slot id (see plugin.list)");
				result.slot = json.at("slot").get<ObjectId>();
			}
			return result;
		}

		ChannelParameter findParameter(const Project& project, const Channel& channel, const AutomationTarget& target)
		{
			const std::vector<ChannelParameter> parameters = channelParameters(project, channel);
			const auto found = std::find_if(parameters.begin(), parameters.end(),
				[&target](const ChannelParameter& parameter) { return parameter.target == target; });
			if (found != parameters.end())
				return *found;
			if (target.type == AutomationTargetType::Plugin)
			{
				const auto slot = project.slots.find(target.slot);
				require(slot != project.slots.end() && slot->second.channel == channel.id,
					"channel " + idText(channel.id) + " has no slot " + idText(target.slot));
				throw CommandError(slot->second.plugin + " has no parameter " + std::to_string(target.parameter));
			}
			if (target.type == AutomationTargetType::Strip && channel.format != ChannelFormat::None)
				throw CommandError("the channel strip has no parameter " + std::to_string(target.parameter));
			if (target.type != AutomationTargetType::Instrument)
				throw CommandError("a MIDI channel has no volume, pan or channel strip sections");
			require(!channel.instrument.empty(), "channel " + idText(channel.id) + " plays no instrument");
			throw CommandError(channel.instrument + " has no parameter " + std::to_string(target.parameter));
		}

		double value(const Project& project, const Channel& channel, const ChannelParameter& parameter)
		{
			if (parameter.target.type == AutomationTargetType::Plugin)
			{
				const std::map<uint32_t, double>& values = project.slots.at(parameter.target.slot).parameters;
				const auto set = values.find(parameter.target.parameter);
				return set != values.end() ? set->second : parameter.info.defaultValue;
			}
			switch (parameter.target.type)
			{
			case AutomationTargetType::Volume:
				return channel.volume;
			case AutomationTargetType::Pan:
				return channel.pan;
			case AutomationTargetType::PanLeft:
				return channel.panLeft;
			case AutomationTargetType::PanRight:
				return channel.panRight;
			case AutomationTargetType::Mute:
				return channel.mute ? 1.0 : 0.0;
			case AutomationTargetType::Strip:
				return stripValue(channel.sections, parameter.target.parameter);
			default:
			{
				const auto set = channel.instrumentParameters.find(parameter.target.parameter);
				return set != channel.instrumentParameters.end() ? set->second : parameter.info.defaultValue;
			}
			}
		}

		void setValue(Project& project, Channel& channel, const ChannelParameter& parameter, double value)
		{
			if (parameter.target.type == AutomationTargetType::Plugin)
			{
				project.slots.at(parameter.target.slot).parameters[parameter.target.parameter] = value;
				return;
			}
			switch (parameter.target.type)
			{
			case AutomationTargetType::Volume:
				channel.volume = value;
				break;
			case AutomationTargetType::Pan:
				channel.pan = value;
				break;
			case AutomationTargetType::PanLeft:
				channel.panLeft = value;
				break;
			case AutomationTargetType::PanRight:
				channel.panRight = value;
				break;
			case AutomationTargetType::Mute:
				channel.mute = value != 0.0;
				break;
			case AutomationTargetType::Strip:
				setStripValue(channel.sections, parameter.target.parameter, value);
				break;
			default:
				channel.instrumentParameters[parameter.target.parameter] = value;
				break;
			}
		}

		Json describe(const Project& project, const Channel& channel, const ChannelParameter& parameter)
		{
			const processors::ParameterInfo& info = parameter.info;
			const double current = value(project, channel, parameter);
			return {{"target", toJson(parameter.target)}, {"name", info.name}, {"minimum", info.minimum},
				{"maximum", info.maximum}, {"default", info.defaultValue}, {"unit", info.unit},
				{"stepped", info.stepped}, {"options", info.options}, {"value", current},
				{"text", processors::valueToText(info, current)}};
		}

		// The mixer section a strip parameter is in.
		std::string sectionName(uint32_t parameter)
		{
			if (isPreSection(parameter))
				return "Pre";
			return parameter < StripParameter::GateOn ? "Equalizer" : "Strip";
		}

		Json listParameters(Project& project, CommandContext& context)
		{
			const Channel& channel = findChannel(project, context.parameters());
			Json parameters = Json::array();
			for (const ChannelParameter& parameter : channelParameters(project, channel))
				parameters.push_back(describe(project, channel, parameter));
			return {{"parameters", std::move(parameters)}};
		}

		// What a track's automation tracks can show (reference manual p. 880): its channel's parameters, and on MIDI
		// and instrument tracks pitch bend, channel pressure and the controllers, each with the lane the track has for
		// it.
		Json listAutomationTargets(Project& project, CommandContext& context)
		{
			const auto id = context.parameters().at("track").get<ObjectId>();
			const auto track = project.trackById.find(id);
			require(track != project.trackById.end(), "no track " + idText(id));
			Json targets = Json::array();
			if (track->second.channel == 0)
				return {{"targets", std::move(targets)}};
			const auto add = [&](const AutomationTarget& target, Json description, const std::string& group)
			{
				ObjectId lane = 0;
				for (const auto& [laneId, other] : project.automation)
					if (other.track == id && other.target == target)
						lane = laneId;
				description["group"] = group;
				description["lane"] = lane;
				targets.push_back(std::move(description));
			};
			const Channel& channel = project.channels.at(track->second.channel);
			for (const ChannelParameter& parameter : channelParameters(project, channel))
			{
				std::string group = "Channel";
				if (parameter.target.type == AutomationTargetType::Instrument)
					group = processors::findBuiltin(channel.instrument)->name;
				else if (parameter.target.type == AutomationTargetType::Plugin)
					group = processors::findBuiltinPlugin(project.slots.at(parameter.target.slot).plugin)->name;
				else if (parameter.target.type == AutomationTargetType::Strip)
					group = sectionName(parameter.target.parameter);
				add(parameter.target, describe(project, channel, parameter), group);
			}
			if (track->second.type != TrackType::Midi && track->second.type != TrackType::Instrument)
				return {{"targets", std::move(targets)}};
			const auto midi = [&](AutomationTargetType type, uint8_t number, const std::string& name, double value)
			{
				const AutomationTarget target{type, 0, number, 0, 0};
				add(target,
					{{"target", toJson(target)}, {"name", name}, {"minimum", 0.0}, {"maximum", 1.0}, {"default", value},
						{"unit", ""}, {"stepped", false}, {"options", Json::array()}, {"value", value},
						{"text", processors::valueToText({}, value)}},
					"MIDI");
			};
			midi(AutomationTargetType::PitchBend, 0, "Pitch Bend", 0.5);
			midi(AutomationTargetType::ChannelPressure, 0, "Channel Pressure", 0.0);
			for (int number = 0; number < 128; ++number)
				midi(AutomationTargetType::Controller, static_cast<uint8_t>(number),
					"Controller " + std::to_string(number), 0.0);
			return {{"targets", std::move(targets)}};
		}

		Json getParameter(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const Channel& channel = findChannel(project, parameters);
			return {{"parameter",
				describe(project, channel, findParameter(project, channel, target(parameters.at("target"))))}};
		}

		Json setParameter(Project& project, CommandContext& context, const ProjectModel& model)
		{
			const Json& parameters = context.parameters();
			Channel& channel = findChannel(project, parameters);
			const ChannelParameter parameter = findParameter(project, channel, target(parameters.at("target")));
			const processors::ParameterInfo& info = parameter.info;
			require(parameters.contains("value") != parameters.contains("text"), "give a value or text, not both");
			double newValue = 0.0;
			if (parameters.contains("value"))
			{
				newValue = parameters.at("value").get<double>();
				require(std::isfinite(newValue), "value must be a finite number");
				newValue = std::clamp(newValue, info.minimum, info.maximum);
				if (info.stepped)
					newValue = std::round(newValue);
			}
			else
			{
				const auto text = parameters.at("text").get<std::string>();
				const std::optional<double> parsed = processors::textToValue(info, text);
				if (!parsed.has_value())
					throw CommandError("text \"" + text + "\" isn't a value of " + info.name);
				newValue = *parsed;
			}
			setValue(project, channel, parameter, newValue);
			if (parameter.target.type == AutomationTargetType::Plugin)
				context.emit("plugin.changed", {{"slot", model.describeSlot(project, parameter.target.slot)}});
			else
				context.emit("channel.changed", {{"channel", toJson(channel)}});
			return {{"parameter", describe(project, channel, parameter)}};
		}
	}

	void ProjectModel::registerParameterCommands(Engine& engine)
	{
		const Json idSchema = {{"type", "integer"}};
		const Json targetSchema = objectSchema(
			{{"type",
				 {{"type", "string"},
					 {"enum", {"volume", "pan", "panLeft", "panRight", "mute", "instrument", "plugin", "strip"}}}},
				{"parameter", {{"type", "integer"}, {"minimum", 0}, {"maximum", UINT32_MAX}}},
				{"slot", {{"type", "integer"}, {"minimum", 0}}}},
			{"type"});
		const Json parameterResult = {
			{"type", "object"}, {"properties", {{"parameter", {{"type", "object"}}}}}, {"required", {"parameter"}}};

		add(engine,
			{"parameter.list", 1,
				"The automatable parameters of a channel: on channels with audio the fader's volume (in dB, up to the "
				"project's maxVolume), pan, panLeft and panRight (-1 to 1, see channel.set) and mute (0 off, 1 on, the "
				"channel's mute button), and on instrument tracks the parameters of their built-in instrument (see "
				"processor.list), then on channels with audio the channel strip's sections (targets of type strip: the "
				"pre section's gain, phase and low and high cut, the equalizer's bypass and bands and the gate, "
				"compressor and limiter, the values channel.set changes in pre, eq and strip; switches are 0 or 1, "
				"cut slopes and the types of EQ bands 1 and 4 stepped choices), then those of the built-in "
				"plugins in its slots, top to bottom, with targets of type plugin that name the slot. Each "
				"has its target, as automationLane.add takes it, its name, minimum, maximum, default, unit, whether "
				"it is stepped (whole numbers only), options (the names of a stepped parameter's values from the "
				"minimum on, if it has any), its static value and text, the value as it is displayed. Automation "
				"overrides the static value while it plays. MIDI channels have none: their volume and pan are MIDI "
				"controllers (channel.set midiVolume and midiPan).",
				objectSchema({{"channel", idSchema}}, {"channel"}),
				{{"type", "object"}, {"properties", {{"parameters", {{"type", "array"}}}}},
					{"required", {"parameters"}}},
				true},
			listParameters);
		add(engine,
			{"parameter.get", 1, "One parameter of a channel, named by its target, as parameter.list returns it.",
				objectSchema({{"channel", idSchema}, {"target", targetSchema}}, {"channel", "target"}), parameterResult,
				true},
			getParameter);
		add(engine,
			{"parameter.set", 1,
				"Sets the static value of a channel's parameter (see parameter.list), named by its target, from "
				"either value or text: the name of one of its options, or a number, optionally followed by its unit, "
				"in any case (\"-6 dB\"). Values beyond the range stop at its ends, and stepped parameters round to "
				"whole numbers. Volume and pan changes glide over 5 ms. A channel that gets another instrument "
				"starts with its parameters at their defaults. Returns the parameter.",
				objectSchema({{"channel", idSchema}, {"target", targetSchema}, {"value", {{"type", "number"}}},
								 {"text", {{"type", "string"}}}},
					{"channel", "target"}),
				parameterResult, false},
			[this](Project& project, CommandContext& context) { return setParameter(project, context, *this); });
		add(engine,
			{"automationLane.targets", 1,
				"What a track's automation lanes can automate, in the order automation tracks offer them: its "
				"channel's parameters as parameter.list returns them, then on MIDI and instrument tracks pitch bend, "
				"channel pressure and controllers 0 to 127 (MIDI channel 0, from 0 to 1, pitch bend's default 0.5). "
				"Each also has group (Channel, the instrument's or plugin's name, Pre, Equalizer or Strip for the "
				"channel strip's sections, or MIDI) and lane, the id of the "
				"track's automation lane for it, or 0. Tracks without a channel have none.",
				objectSchema({{"track", idSchema}}, {"track"}),
				{{"type", "object"}, {"properties", {{"targets", {{"type", "array"}}}}}, {"required", {"targets"}}},
				true},
			listAutomationTargets);
	}
}
