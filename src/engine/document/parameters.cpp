// SPDX-License-Identifier: GPL-3.0-or-later
// Parameters (#30): parameter.list, parameter.get and parameter.set read and change the static value of every
// automatable parameter of a channel: the fader's volume, pan, panLeft and panRight, and the parameters of an
// instrument track's built-in instrument (plugins: M05). They name parameters by the targets automation lanes take,
// and give values with their display text. parameter.set is one undo step and changes nothing if it fails
// (ProjectModel::add); playback takes the values from the render snapshot like those of channel.set.
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
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
				parameters.push_back({{AutomationTargetType::Volume, 0, 0, 0},
					{0, "Volume", g_minChannelVolume, project.maxVolume, 0.0, "dB", false, {}}});
				parameters.push_back({{AutomationTargetType::Pan, 0, 0, 0}, pan("Pan", 0.0)});
				parameters.push_back({{AutomationTargetType::PanLeft, 0, 0, 0}, pan("Pan left", -1.0)});
				parameters.push_back({{AutomationTargetType::PanRight, 0, 0, 0}, pan("Pan right", 1.0)});
			}
			if (const processors::BuiltinInfo* instrument = processors::findBuiltin(channel.instrument))
				for (const processors::ParameterInfo& parameter : instrument->parameters)
					parameters.push_back({{AutomationTargetType::Instrument, parameter.id, 0, 0}, parameter});
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
					*type != AutomationTargetType::Instrument))
				throw CommandError("target.type must be volume, pan, panLeft, panRight or instrument");
			const bool parameter = *type == AutomationTargetType::Instrument;
			for (const auto& item : json.items())
				require(item.key() == "type" || (item.key() == "parameter" && parameter),
					"target." + item.key() + " doesn't belong to a " + json.at("type").get<std::string>() + " target");
			AutomationTarget result;
			result.type = *type;
			if (parameter)
			{
				require(json.contains("parameter") && isParameterId(json.at("parameter")),
					"target.parameter must be a parameter id (see parameter.list)");
				result.parameter = json.at("parameter").get<uint32_t>();
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
			if (target.type != AutomationTargetType::Instrument)
				throw CommandError("a MIDI channel has no volume or pan");
			require(!channel.instrument.empty(), "channel " + idText(channel.id) + " plays no instrument");
			throw CommandError(channel.instrument + " has no parameter " + std::to_string(target.parameter));
		}

		double value(const Channel& channel, const ChannelParameter& parameter)
		{
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
			default:
			{
				const auto set = channel.instrumentParameters.find(parameter.target.parameter);
				return set != channel.instrumentParameters.end() ? set->second : parameter.info.defaultValue;
			}
			}
		}

		void setValue(Channel& channel, const ChannelParameter& parameter, double value)
		{
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
			default:
				channel.instrumentParameters[parameter.target.parameter] = value;
				break;
			}
		}

		Json describe(const Channel& channel, const ChannelParameter& parameter)
		{
			const processors::ParameterInfo& info = parameter.info;
			const double current = value(channel, parameter);
			return {{"target", toJson(parameter.target)}, {"name", info.name}, {"minimum", info.minimum},
				{"maximum", info.maximum}, {"default", info.defaultValue}, {"unit", info.unit},
				{"stepped", info.stepped}, {"options", info.options}, {"value", current},
				{"text", processors::valueToText(info, current)}};
		}

		Json listParameters(Project& project, CommandContext& context)
		{
			const Channel& channel = findChannel(project, context.parameters());
			Json parameters = Json::array();
			for (const ChannelParameter& parameter : channelParameters(project, channel))
				parameters.push_back(describe(channel, parameter));
			return {{"parameters", std::move(parameters)}};
		}

		Json getParameter(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const Channel& channel = findChannel(project, parameters);
			return {{"parameter", describe(channel, findParameter(project, channel, target(parameters.at("target"))))}};
		}

		Json setParameter(Project& project, CommandContext& context)
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
			setValue(channel, parameter, newValue);
			context.emit("channel.changed", {{"channel", toJson(channel)}});
			return {{"parameter", describe(channel, parameter)}};
		}
	}

	void ProjectModel::registerParameterCommands(Engine& engine)
	{
		const Json idSchema = {{"type", "integer"}};
		const Json targetSchema = objectSchema(
			{{"type", {{"type", "string"}, {"enum", {"volume", "pan", "panLeft", "panRight", "instrument"}}}},
				{"parameter", {{"type", "integer"}, {"minimum", 0}, {"maximum", UINT32_MAX}}}},
			{"type"});
		const Json parameterResult = {
			{"type", "object"}, {"properties", {{"parameter", {{"type", "object"}}}}}, {"required", {"parameter"}}};

		add(engine,
			{"parameter.list", 1,
				"The automatable parameters of a channel: on channels with audio the fader's volume (in dB, up to the "
				"project's maxVolume), pan, panLeft and panRight (-1 to 1, see channel.set), and on instrument "
				"tracks the parameters of their built-in instrument (see processor.list; plugins come later). Each "
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
			setParameter);
	}
}
