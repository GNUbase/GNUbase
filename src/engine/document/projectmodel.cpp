// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/projectmodel.h"

#include "engine/audiofile/audiofile.h"
#include "engine/audiofile/resampler.h"
#include "engine/dawproject/dawproject.h"
#include "engine/document/channelsettings.h"
#include "engine/document/editors.h"
#include "engine/document/hitpoints.h"
#include "engine/document/linkgroups.h"
#include "engine/document/mixerviews.h"
#include "engine/document/sampleeditor.h"
#include "engine/document/serialize.h"
#include "engine/document/volumecurves.h"
#include "engine/dsp/timestretch.h"
#include "engine/graph/graph.h"
#include "engine/jobs/jobs.h"
#include "engine/media/backup.h"
#include "engine/midi/controllerlanes.h"
#include "engine/midi/functioncommands.h"
#include "engine/midi/quantizecommands.h"
#include "engine/processors/builtin.h"
#include "platform/log.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		// Names in commands and events.

		struct TrackTypeName
		{
			TrackType type;
			const char* name;
			const char* defaultName; // new tracks are called "<defaultName> 01", "<defaultName> 02" and so on
		};

		constexpr TrackTypeName g_trackTypes[] = {
			{TrackType::Audio, "audio", "Audio"},
			{TrackType::Instrument, "instrument", "Instrument"},
			{TrackType::Midi, "midi", "MIDI"},
			{TrackType::Folder, "folder", "Folder"},
			{TrackType::Group, "group", "Group"},
			{TrackType::Effect, "effect", "FX"},
			{TrackType::Marker, "marker", "Marker"},
			{TrackType::Ruler, "ruler", "Ruler"},
			{TrackType::Tempo, "tempo", "Tempo"},
			{TrackType::Signature, "signature", "Signature"},
			{TrackType::Vca, "vca", "VCA"},
		};

		const TrackTypeName& trackTypeEntry(TrackType type)
		{
			for (const TrackTypeName& entry : g_trackTypes)
				if (entry.type == type)
					return entry;
			return g_trackTypes[0]; // not reached: every type is in the table
		}

		// The schema only lets names from the table through.
		TrackType trackType(const std::string& name)
		{
			for (const TrackTypeName& entry : g_trackTypes)
				if (entry.name == name)
					return entry.type;
			throw CommandError("unknown track type " + name);
		}

		// What media.add reads from the file unless all of them are given.
		constexpr std::array<const char*, 3> g_mediaProperties{"sampleRate", "channels", "length"};

		Json graphToJson(const graph::GraphDesc& graph)
		{
			Json nodes = Json::array();
			for (const graph::NodeDesc& node : graph.nodes)
				nodes.push_back({{"kind", graph::toString(node.kind)}, {"object", node.object},
					{"format", formatName(node.format)}, {"channels", channelCount(node.format)},
					{"output", formatName(node.output == ChannelFormat::None ? node.format : node.output)},
					{"audioInputs", Json::array()}, {"eventInputs", Json::array()}});
			// Inputs in ascending order, the order a node sums them in.
			for (const auto& [connections, key] :
				{std::pair{&graph.audio, "audioInputs"}, std::pair{&graph.events, "eventInputs"}})
			{
				std::vector<std::vector<int>> inputs(graph.nodes.size());
				for (const graph::Connection& connection : *connections)
					inputs[static_cast<size_t>(connection.to)].push_back(connection.from);
				for (size_t i = 0; i < inputs.size(); ++i)
				{
					std::sort(inputs[i].begin(), inputs[i].end());
					nodes[i][key] = inputs[i];
				}
			}
			return {{"nodes", std::move(nodes)}, {"order", graph::processingOrder(graph)}};
		}

		// Schemas.

		Json objectSchema(Json properties, const std::vector<std::string>& required = {})
		{
			return {{"type", "object"}, {"properties", std::move(properties)}, {"required", required},
				{"additionalProperties", false}};
		}

		Json resultSchema(const std::vector<std::string>& objects)
		{
			Json properties = Json::object();
			for (const std::string& name : objects)
				properties[name] = {{"type", "object"}};
			return {{"type", "object"}, {"properties", properties}, {"required", objects}};
		}

		Json enumSchema(const std::vector<std::string>& values)
		{
			return {{"type", "string"}, {"enum", values}};
		}

		Json arraySchema(Json items)
		{
			return {{"type", "array"}, {"items", std::move(items)}};
		}

		// An object schema that also allows null.
		Json nullable(Json schema)
		{
			schema["type"] = {"object", "null"};
			return schema;
		}

		// Lookups and checks. They throw CommandError, which fails the command with the message.

		std::string idText(ObjectId id)
		{
			return std::to_string(id);
		}

		template <typename Map> auto& find(Map& objects, ObjectId id, const char* what)
		{
			const auto found = objects.find(id);
			if (found == objects.end())
				throw CommandError(std::string("no ") + what + " " + idText(id));
			return found->second;
		}

		Bus& findBus(Project& project, ObjectId id)
		{
			const auto found = std::find_if(
				project.busses.begin(), project.busses.end(), [id](const Bus& bus) { return bus.id == id; });
			if (found == project.busses.end())
				throw CommandError("no bus " + idText(id));
			return *found;
		}

		const Bus* busOrNull(const Project& project, ObjectId id)
		{
			const auto found = std::find_if(
				project.busses.begin(), project.busses.end(), [id](const Bus& bus) { return bus.id == id; });
			return found == project.busses.end() ? nullptr : &*found;
		}

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		void requireRange(int64_t value, int64_t min, int64_t max, const std::string& name)
		{
			require(
				value >= min && value <= max, name + " must be " + std::to_string(min) + " to " + std::to_string(max));
		}

		// The format a command gives by name, or by its number of channels (see defaultFormat); both must agree.
		ChannelFormat channelFormat(const Json& parameters, ChannelFormat fallback)
		{
			std::optional<ChannelFormat> format;
			if (parameters.contains("format"))
			{
				const std::string name = parameters.at("format").get<std::string>();
				format = formatFromName(name);
				require(format.has_value() && *format != ChannelFormat::None, "unknown format " + name);
			}
			if (parameters.contains("channels"))
			{
				const int channels = parameters.at("channels").get<int>();
				requireRange(channels, 1, g_maxFormatChannels, "channels");
				if (!format)
					return defaultFormat(channels);
				require(channelCount(*format) == channels,
					std::string("format ") + formatName(*format) + " doesn't have " + std::to_string(channels) +
						" channels");
			}
			return format.value_or(fallback);
		}

		std::string color(const Json& parameters, const std::string& fallback)
		{
			if (!parameters.contains("color") || parameters.at("color").is_null())
				return parameters.contains("color") ? std::string() : fallback;
			const std::string value = parameters.at("color").get<std::string>();
			require(isColor(value), "color must be #rrggbb");
			return value;
		}

		// The tracks a track sits among: the top level or the folder's children.
		std::vector<ObjectId>& siblings(Project& project, ObjectId parent)
		{
			return parent == 0 ? project.tracks : project.trackById.at(parent).children;
		}

		ObjectId folderParent(Project& project, const Json& parameters)
		{
			const auto parent = parameters.value("parent", ObjectId{0});
			if (parent != 0)
				require(find(project.trackById, parent, "track").type == TrackType::Folder,
					"parent " + idText(parent) + " is not a folder track");
			return parent;
		}

		size_t insertIndex(const Json& parameters, const std::vector<ObjectId>& siblings)
		{
			const auto index = parameters.value("index", static_cast<int64_t>(siblings.size()));
			requireRange(index, 0, static_cast<int64_t>(siblings.size()), "index");
			return static_cast<size_t>(index);
		}

		// The track and every track inside it.
		std::vector<ObjectId> subtree(const Project& project, ObjectId id)
		{
			std::vector<ObjectId> ids{id};
			for (size_t i = 0; i < ids.size(); ++i)
				for (const ObjectId child : project.trackById.at(ids[i]).children)
					ids.push_back(child);
			return ids;
		}

		// Whether routing a channel to output would lead back to it, through outputs or sends.
		bool loops(const Project& project, ObjectId channel, ObjectId output)
		{
			return routingLoops(project, channel, output);
		}

		// Where the audio of the tracks in a folder goes unless routed elsewhere (reference manual p. 222): the group
		// channel of the nearest folder around them that has one, else the main mix.
		ObjectId folderOutput(const Project& project, ObjectId parent)
		{
			for (; parent != 0; parent = project.trackById.at(parent).parent)
				if (const ObjectId channel = project.trackById.at(parent).channel; channel != 0)
					return channel;
			return mainMix(project);
		}

		// The folder output of the track and every track inside it that has an audio channel.
		std::map<ObjectId, ObjectId> folderOutputs(const Project& project, ObjectId root)
		{
			std::map<ObjectId, ObjectId> outputs;
			for (const ObjectId id : subtree(project, root))
			{
				const Track& track = project.trackById.at(id);
				if (track.channel != 0 && track.type != TrackType::Midi)
					outputs[id] = folderOutput(project, track.parent);
			}
			return outputs;
		}

		// After tracks moved or a group channel went on or off: routes the channels of the tracks whose folder
		// output changed from before to the new one, unless that would make a loop. Only channels that still follow
		// the folder, i.e. play through the old folder output, move: one routed elsewhere by hand or disconnected
		// keeps its routing.
		void reroute(Project& project, const std::map<ObjectId, ObjectId>& before, CommandContext& context)
		{
			for (const auto& [id, output] : before)
			{
				const Track& track = project.trackById.at(id);
				const ObjectId now = folderOutput(project, track.parent);
				if (track.channel == 0 || now == output || loops(project, track.channel, now))
					continue;
				Channel& channel = project.channels.at(track.channel);
				if (channel.output != output)
					continue;
				channel.output = now;
				context.emit("channel.changed", {{"channel", toJson(channel)}});
			}
		}

		// The color the tracks in a folder take: with the user setting editing.applyFolderColor the folder's own
		// (p. 221), else none (empty).
		std::string folderColor(const Project& project, ObjectId folder, CommandContext& context)
		{
			if (folder == 0 ||
				!context.call("settings.get", {{"key", "editing.applyFolderColor"}}).at("value").get<bool>())
				return {};
			return project.trackById.at(folder).color;
		}

		// The color editing.autoTrackColor gives a track added at index among the siblings in parent (reference
		// manual p. 1621): none (default), the color of the track above it (previous), the color after that one in
		// the project's color set (previousPlusOne), the color last given to events or parts (lastApplied), or a
		// random color of the set (random).
		std::string autoTrackColor(const Project& project, ObjectId parent, size_t index, CommandContext& context)
		{
			const std::string mode =
				context.call("settings.get", {{"key", "editing.autoTrackColor"}}).at("value").get<std::string>();
			const std::vector<ProjectColor>& colors = project.colors;
			if (mode == "lastApplied")
				return project.lastAppliedColor;
			if (mode == "random")
			{
				static std::mt19937 random{std::random_device{}()}; // only the command thread adds tracks
				return colors[std::uniform_int_distribution<size_t>(0, colors.size() - 1)(random)].color;
			}
			if (mode != "previous" && mode != "previousPlusOne")
				return {};
			// The track above: the one before it among its siblings, or the last track inside that one if it is a
			// folder, or else the folder it is in.
			const std::vector<ObjectId>& list = parent == 0 ? project.tracks : project.trackById.at(parent).children;
			ObjectId above = parent;
			if (index > 0)
			{
				above = list[index - 1];
				while (!project.trackById.at(above).children.empty())
					above = project.trackById.at(above).children.back();
			}
			std::string previous = above == 0 ? std::string() : project.trackById.at(above).color;
			if (mode == "previous")
				return previous;
			const auto found = std::find_if(colors.begin(), colors.end(),
				[&previous](const ProjectColor& color) { return color.color == previous; });
			return found == colors.end() || found + 1 == colors.end() ? colors.front().color : (found + 1)->color;
		}

		// Gives a folder color to the tracks inside root, and to root itself with paintRoot.
		void paint(Project& project, ObjectId root, const std::string& color, bool paintRoot, CommandContext& context)
		{
			if (color.empty())
				return;
			for (const ObjectId id : subtree(project, root))
			{
				Track& track = project.trackById.at(id);
				if ((id == root && !paintRoot) || track.color == color)
					continue;
				track.color = color;
				context.emit("track.changed", {{"track", toJson(track)}});
			}
		}

		// Moves a track, with what it contains, to index among the tracks of the folder parent (0: the top level),
		// counted without it. Its audio and that of the tracks inside it follow the folders' group channels.
		void moveInto(Project& project, ObjectId id, ObjectId parent, int64_t index, CommandContext& context)
		{
			Track& track = find(project.trackById, id, "track");
			require(track.type != TrackType::Marker || parent == 0, "marker tracks can't be inside a folder");
			require(track.type != TrackType::Tempo || parent == 0, "the tempo track can't be inside a folder");
			require(track.type != TrackType::Signature || parent == 0, "the signature track can't be inside a folder");
			const std::vector<ObjectId> moved = subtree(project, id);
			require(std::find(moved.begin(), moved.end(), parent) == moved.end(),
				"can't move a folder into itself or a folder inside it");
			const std::map<ObjectId, ObjectId> outputs = folderOutputs(project, id);

			std::vector<ObjectId>& from = siblings(project, track.parent);
			from.erase(std::find(from.begin(), from.end(), id));
			std::vector<ObjectId>& to = siblings(project, parent);
			requireRange(index, 0, static_cast<int64_t>(to.size()), "index");
			to.insert(to.begin() + index, id);
			const bool changed = track.parent != parent;
			track.parent = parent;
			context.emit("track.moved", {{"track", id}, {"parent", parent}, {"index", index}});
			reroute(project, outputs, context);
			if (changed)
				paint(project, id, folderColor(project, parent, context), true, context);
		}

		void checkOutput(const Project& project, const Channel& channel, ObjectId output)
		{
			if (output == 0)
				return;
			if (project.trackById.at(channel.track).type == TrackType::Midi)
			{
				const auto target = project.channels.find(output);
				require(target != project.channels.end() &&
						project.trackById.at(target->second.track).type == TrackType::Instrument,
					"a MIDI channel's output must be an instrument channel");
				return;
			}
			if (const Bus* bus = busOrNull(project, output))
			{
				require(bus->direction == BusDirection::Output, "bus " + idText(output) + " is an input bus");
				return;
			}
			require(isSendDestination(project, output), "output must be an output bus, a group or an effect channel");
			require(!loops(project, channel.id, output),
				"routing channel " + idText(channel.id) + " to " + idText(output) + " would make a loop");
		}

		// Time.

		time::Position position(const Track& track, const Json& value, const std::string& name)
		{
			const int64_t position = value.get<int64_t>();
			require(position >= 0, name + " must not be negative");
			return {track.timeBase, position};
		}

		// A MIDI part's end, in the time base of its position.
		time::Position partEnd(const MidiPart& part)
		{
			return {part.position.base, part.position.value + part.length};
		}

		int64_t rescale(int64_t value, double factor)
		{
			return std::llround(static_cast<double>(value) * factor);
		}

		void rebaseTrack(Project& project, Track& track, time::TimeBase base, CommandContext& context)
		{
			for (auto& [id, event] : project.audioEvents)
				if (event.track == track.id)
				{
					event.position = time::rebase(event.position, base, project.tempo, project.sampleRate);
					context.emit("audioEvent.changed", {{"event", toJson(event)}});
				}
			for (auto& [id, part] : project.audioParts)
				if (part.track == track.id)
				{
					const time::Position end = time::rebase({part.position.base, part.position.value + part.length},
						base, project.tempo, project.sampleRate);
					part.position = time::rebase(part.position, base, project.tempo, project.sampleRate);
					part.length = end.value - part.position.value;
					context.emit("audioPart.changed", {{"part", toJson(part)}});
				}
			for (auto& [id, part] : project.midiParts)
				if (part.track == track.id)
				{
					const time::Position end = time::rebase(partEnd(part), base, project.tempo, project.sampleRate);
					part.position = time::rebase(part.position, base, project.tempo, project.sampleRate);
					part.length = end.value - part.position.value;
					context.emit("midiPart.changed", {{"part", toJson(part)}});
				}
			for (auto& [id, marker] : project.markers)
				if (marker.track == track.id)
				{
					marker.position = time::rebase(marker.position, base, project.tempo, project.sampleRate);
					marker.end = time::rebase(marker.end, base, project.tempo, project.sampleRate);
					keepMinimumLength(project, marker); // rounding may have left it shorter than a sample
					context.emit("marker.changed", {{"marker", toJson(marker)}});
				}
			for (auto& [id, lane] : project.automation)
				if (lane.track == track.id && !lane.points.empty())
				{
					for (auto& [pointId, point] : lane.points)
						point.position = time::rebase(point.position, base, project.tempo, project.sampleRate);
					context.emit("automationLane.changed", {{"lane", toJson(lane)}});
				}
			track.timeBase = base;
		}

		// Linear positions are in samples, so they keep their time when the sample rate changes.
		void changeSampleRate(Project& project, double sampleRate, CommandContext& context)
		{
			const double factor = sampleRate / project.sampleRate;
			for (auto& [id, event] : project.audioEvents)
				if (event.position.base == time::TimeBase::Linear)
				{
					event.position.value = rescale(event.position.value, factor);
					context.emit("audioEvent.changed", {{"event", toJson(event)}});
				}
			for (auto& [id, part] : project.midiParts)
				if (part.position.base == time::TimeBase::Linear)
				{
					part.position.value = rescale(part.position.value, factor);
					part.length = rescale(part.length, factor);
					context.emit("midiPart.changed", {{"part", toJson(part)}});
				}
			for (auto& [id, part] : project.audioParts)
				if (part.position.base == time::TimeBase::Linear)
				{
					part.position.value = rescale(part.position.value, factor);
					part.length = rescale(part.length, factor);
					context.emit("audioPart.changed", {{"part", toJson(part)}});
				}
			for (auto& [id, lane] : project.automation)
			{
				bool changed = false;
				for (auto& [pointId, point] : lane.points)
					if (point.position.base == time::TimeBase::Linear)
					{
						point.position.value = rescale(point.position.value, factor);
						changed = true;
					}
				if (changed)
					context.emit("automationLane.changed", {{"lane", toJson(lane)}});
			}
			project.sampleRate = sampleRate;
			// At a lower rate, a short cycle marker may become shorter than a sample, in either time base.
			for (auto& [id, marker] : project.markers)
			{
				const bool linear = marker.position.base == time::TimeBase::Linear;
				if (linear)
				{
					marker.position.value = rescale(marker.position.value, factor);
					marker.end.value = rescale(marker.end.value, factor);
				}
				if (keepMinimumLength(project, marker) || linear)
					context.emit("marker.changed", {{"marker", toJson(marker)}});
			}
		}

		int64_t countTracks(const Project& project, TrackType type)
		{
			return std::count_if(project.trackById.begin(), project.trackById.end(),
				[type](const auto& entry) { return entry.second.type == type; });
		}

		// The first marker track is called "Markers", the next ones "Marker 01", "Marker 02" and so on.
		std::string defaultTrackName(const Project& project, TrackType type)
		{
			const auto count = countTracks(project, type);
			if (type == TrackType::Marker && count == 0)
				return "Markers";
			if (type == TrackType::Tempo)
				return "Tempo";
			if (type == TrackType::Signature)
				return "Signature";
			const std::string number = std::to_string(type == TrackType::Marker ? count : count + 1);
			return trackTypeEntry(type).defaultName + std::string(number.size() < 2 ? " 0" : " ") + number;
		}

		void activateMarkerTrack(Project& project, ObjectId track, CommandContext& context)
		{
			if (project.activeMarkerTrack == track)
				return;
			project.activeMarkerTrack = track;
			context.emit("marker.trackActivated", {{"track", track}});
		}

		// Automation.

		// A lane's target from a command, checked against the track it automates.
		AutomationTarget automationTarget(const Project& project, const Track& track, const Json& json)
		{
			const std::optional<AutomationTargetType> parsed =
				automationTargetTypeFromName(json.at("type").get<std::string>());
			if (!parsed.has_value())
				throw CommandError(
					"target.type must be volume, pan, panLeft, panRight, mute, instrument, plugin, controller, "
					"pitchBend or channelPressure");
			const AutomationTargetType type = *parsed;
			AutomationTarget target;
			target.type = type;
			require(track.channel != 0, "track " + idText(track.id) + " has no channel to automate");
			const Channel& channel = project.channels.at(track.channel);
			const bool midi = track.type == TrackType::Midi || track.type == TrackType::Instrument;
			const auto only = [&json, type](std::initializer_list<const char*> keys)
			{
				for (const auto& [key, value] : json.items())
					require(key == "type" ||
							std::any_of(keys.begin(), keys.end(), [&key = key](const char* k) { return key == k; }),
						std::string("target.") + key + " doesn't belong to a " + automationTargetTypeName(type) +
							" target");
			};
			const auto midiChannel = [&json]()
			{
				const auto value = json.value("channel", int64_t{0});
				requireRange(value, 0, 15, "target.channel");
				return static_cast<uint8_t>(value);
			};
			switch (target.type)
			{
			case AutomationTargetType::Volume:
			case AutomationTargetType::Pan:
			case AutomationTargetType::PanLeft:
			case AutomationTargetType::PanRight:
				only({});
				require(channel.format != ChannelFormat::None ||
						(track.type == TrackType::Vca && target.type == AutomationTargetType::Volume),
					track.type == TrackType::Vca ? "a VCA track has no pan" : "a MIDI track has no volume or pan");
				break;
			case AutomationTargetType::Mute:
				only({});
				// ponytail: a MIDI track's mute would have to silence its notes as they play.
				require(channel.format != ChannelFormat::None, "a MIDI track's mute isn't automated yet");
				break;
			case AutomationTargetType::Instrument:
			{
				only({"parameter"});
				require(track.type == TrackType::Instrument, "only instrument tracks have an instrument");
				require(!channel.instrument.empty(), "track " + idText(track.id) + " plays no instrument");
				require(json.contains("parameter"), "target.parameter is required");
				const auto parameter = json.at("parameter").get<int64_t>();
				const processors::BuiltinInfo* info = processors::findBuiltin(channel.instrument);
				require(info != nullptr && isParameterId(json.at("parameter")) &&
						processors::findParameter(*info, static_cast<uint32_t>(parameter)) != nullptr,
					channel.instrument + " has no parameter " + std::to_string(parameter));
				target.parameter = static_cast<uint32_t>(parameter);
				break;
			}
			case AutomationTargetType::Plugin:
			{
				only({"slot", "parameter"});
				require(json.contains("slot") && json.at("slot").is_number_unsigned(),
					"target.slot must be a slot id (see plugin.list)");
				target.slot = json.at("slot").get<ObjectId>();
				require(std::find(channel.slots.begin(), channel.slots.end(), target.slot) != channel.slots.end(),
					"track " + idText(track.id) + " has no plugin slot " + idText(target.slot));
				const Slot& slot = project.slots.at(target.slot);
				const processors::BuiltinInfo* info = processors::findBuiltinPlugin(slot.plugin);
				// ponytail: other plugins' parameters are known once they report them (#283).
				require(info != nullptr, "only built-in plugins' parameters can be automated yet, not " + slot.plugin);
				require(json.contains("parameter"), "target.parameter is required");
				require(isParameterId(json.at("parameter")) &&
						processors::findParameter(*info, json.at("parameter").get<uint32_t>()) != nullptr,
					slot.plugin + " has no parameter " + json.at("parameter").dump());
				target.parameter = json.at("parameter").get<uint32_t>();
				break;
			}
			case AutomationTargetType::Controller:
				only({"number", "channel"});
				require(midi, "MIDI controllers are automated on MIDI and instrument tracks");
				require(json.contains("number"), "target.number is required");
				requireRange(json.at("number").get<int64_t>(), 0, 127, "target.number");
				target.number = json.at("number").get<uint8_t>();
				target.channel = midiChannel();
				break;
			case AutomationTargetType::PitchBend:
			case AutomationTargetType::ChannelPressure:
				only({"channel"});
				require(midi, "MIDI controllers are automated on MIDI and instrument tracks");
				target.channel = midiChannel();
				break;
			}
			return target;
		}

		// The parameter of an instrument or a plugin that a lane automates, or none for the other targets and for
		// parameters that are gone.
		const processors::ParameterInfo* automatedParameter(const Project& project, const AutomationLane& lane)
		{
			const processors::BuiltinInfo* info = nullptr;
			if (lane.target.type == AutomationTargetType::Instrument)
				info =
					processors::findBuiltin(project.channels.at(project.trackById.at(lane.track).channel).instrument);
			if (lane.target.type == AutomationTargetType::Plugin)
			{
				const auto slot = project.slots.find(lane.target.slot);
				if (slot != project.slots.end())
					info = processors::findBuiltinPlugin(slot->second.plugin);
			}
			return info != nullptr ? processors::findParameter(*info, lane.target.parameter) : nullptr;
		}

		// Whether a lane has a step curve: mute and stepped parameters.
		bool stepCurve(const Project& project, const AutomationLane& lane)
		{
			const processors::ParameterInfo* parameter = automatedParameter(project, lane);
			return lane.target.type == AutomationTargetType::Mute || (parameter != nullptr && parameter->stepped);
		}

		// The values a lane's points can have, in its target's unit.
		std::pair<double, double> automationRange(const Project& project, const AutomationLane& lane)
		{
			switch (lane.target.type)
			{
			case AutomationTargetType::Volume:
				return {g_minChannelVolume, project.maxVolume};
			case AutomationTargetType::Pan:
			case AutomationTargetType::PanLeft:
			case AutomationTargetType::PanRight:
				return {-1.0, 1.0};
			case AutomationTargetType::Mute:
				return {0.0, 1.0};
			case AutomationTargetType::Instrument:
			case AutomationTargetType::Plugin:
				if (const processors::ParameterInfo* parameter = automatedParameter(project, lane))
					return {parameter->minimum, parameter->maximum};
				throw CommandError(std::string("the ") +
					(lane.target.type == AutomationTargetType::Plugin ? "plugin" : "instrument") +
					" of automation lane " + idText(lane.id) + " has no parameter " +
					std::to_string(lane.target.parameter) + " any more");
			case AutomationTargetType::Controller:
			case AutomationTargetType::PitchBend:
			case AutomationTargetType::ChannelPressure:
				return {0.0, 1.0};
			}
			return {0.0, 1.0};
		}

		std::string numberText(double value)
		{
			std::ostringstream text;
			text << value;
			return text.str();
		}

		// Changes a point by what a command gives for it, checked. Positions are in the time base of the lane's track.
		// On a step curve, points are steps with whole values (reference manual p. 870: step curves for on/off
		// parameters, such as mute).
		void changePoint(AutomationPoint& point, const Json& item, const std::string& name, const Track& track,
			const std::pair<double, double>& range, bool stepped)
		{
			if (item.contains("position"))
				point.position = position(track, item.at("position"), name + ".position");
			if (item.contains("value"))
			{
				point.value = item.at("value").get<double>();
				require(point.value >= range.first && point.value <= range.second,
					name + ".value must be from " + numberText(range.first) + " to " + numberText(range.second));
			}
			if (item.contains("shape"))
			{
				const std::optional<AutomationShape> shape =
					automationShapeFromName(item.at("shape").get<std::string>());
				if (!shape.has_value())
					throw CommandError(name + ".shape must be step, linear or curve");
				point.shape = *shape;
			}
			if (item.contains("curve"))
			{
				point.curve = item.at("curve").get<double>();
				require(point.curve >= -1.0 && point.curve <= 1.0, name + ".curve must be from -1 to 1");
			}
			if (item.contains("terminator"))
				point.terminator = item.at("terminator").get<bool>();
			if (stepped)
			{
				point.shape = AutomationShape::Step;
				point.value = std::round(point.value);
			}
			if (point.shape != AutomationShape::Curve)
				point.curve = 0.0;
		}

		Json pointsJson(const std::vector<AutomationPoint>& points)
		{
			Json list = Json::array();
			for (const AutomationPoint& point : points)
				list.push_back(toJson(point));
			return list;
		}

		Json addAutomationLane(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			AutomationLane lane;
			lane.track = parameters.at("track").get<ObjectId>();
			const Track& track = find(project.trackById, lane.track, "track");
			lane.target = automationTarget(project, track, parameters.at("target"));
			lane.read = parameters.value("read", true);
			for (const auto& [id, other] : project.automation)
				require(other.track != lane.track || !(other.target == lane.target),
					"track " + idText(lane.track) + " already has this automation lane: " + idText(id));
			lane.id = project.newId();
			Json result = {{"lane", toJson(lane)}};
			project.automation[lane.id] = std::move(lane);
			context.emit("automationLane.added", result);
			return result;
		}

		Json setAutomationLane(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			AutomationLane& lane = find(project.automation, parameters.at("lane").get<ObjectId>(), "automation lane");
			lane.read = parameters.value("read", lane.read);
			Json result = {{"lane", toJson(lane)}};
			context.emit("automationLane.changed", result);
			return result;
		}

		Json removeAutomationLane(Project& project, CommandContext& context)
		{
			const ObjectId id = context.parameters().at("lane").get<ObjectId>();
			find(project.automation, id, "automation lane");
			project.automation.erase(id);
			context.emit("automationLane.removed", {{"lane", id}});
			return Json::object();
		}

		Json listAutomationLanes(const Project& project, CommandContext& context)
		{
			const auto track = context.parameters().value("track", ObjectId{0});
			if (track != 0)
				require(project.trackById.count(track) != 0, "no track " + idText(track));
			Json lanes = Json::array();
			for (const ObjectId id : project.trackOrder())
				if (track == 0 || id == track)
					for (const auto& [laneId, lane] : project.automation)
						if (lane.track == id)
							lanes.push_back(toJson(lane));
			return {{"lanes", std::move(lanes)}};
		}

		Json addAutomationPoints(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			AutomationLane& lane = find(project.automation, parameters.at("lane").get<ObjectId>(), "automation lane");
			const Json& items = parameters.at("points");
			const std::pair<double, double> range = automationRange(project, lane);
			const bool stepped = stepCurve(project, lane);
			const Track& track = project.trackById.at(lane.track);
			std::vector<AutomationPoint> points;
			for (size_t i = 0; i < items.size(); ++i)
			{
				AutomationPoint point;
				point.position.base = track.timeBase;
				changePoint(point, items[i], "points[" + std::to_string(i) + "]", track, range, stepped);
				points.push_back(point);
			}
			for (AutomationPoint& point : points)
			{
				point.id = project.newId();
				lane.points[point.id] = point;
			}
			Json added = pointsJson(points);
			context.emit("automationPoint.added", {{"lane", lane.id}, {"points", added}});
			return {{"points", added}};
		}

		Json setAutomationPoints(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			AutomationLane& lane = find(project.automation, parameters.at("lane").get<ObjectId>(), "automation lane");
			const Json& items = parameters.at("points");
			const std::pair<double, double> range = automationRange(project, lane);
			const bool stepped = stepCurve(project, lane);
			const Track& track = project.trackById.at(lane.track);
			std::vector<AutomationPoint> points;
			for (size_t i = 0; i < items.size(); ++i)
			{
				const auto id = items[i].at("id").get<ObjectId>();
				const auto found = lane.points.find(id);
				require(found != lane.points.end(),
					"no automation point " + idText(id) + " in automation lane " + idText(lane.id));
				AutomationPoint point = found->second;
				changePoint(point, items[i], "points[" + std::to_string(i) + "]", track, range, stepped);
				points.push_back(point);
			}
			for (const AutomationPoint& point : points)
				lane.points[point.id] = point;
			Json changed = pointsJson(points);
			context.emit("automationPoint.changed", {{"lane", lane.id}, {"points", changed}});
			return {{"points", changed}};
		}

		Json removeAutomationPoints(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			AutomationLane& lane = find(project.automation, parameters.at("lane").get<ObjectId>(), "automation lane");
			const auto ids = parameters.at("points").get<std::vector<ObjectId>>();
			for (const ObjectId id : ids)
				require(lane.points.count(id) != 0,
					"no automation point " + idText(id) + " in automation lane " + idText(lane.id));
			for (const ObjectId id : ids)
				lane.points.erase(id);
			context.emit("automationPoint.removed", {{"lane", lane.id}, {"points", ids}});
			return Json::object();
		}

		// Automation follows events: when an event or part moves, the automation of its track in the range it covered
		// moves with it and replaces the points where it lands. The points are in the track's time base, like the
		// event's positions.
		void followEvent(Project& project, ObjectId track, const time::Position& fromPosition,
			const time::Position& endPosition, const time::Position& toPosition, CommandContext& context)
		{
			const time::TimeBase base = project.trackById.at(track).timeBase;
			const auto inBase = [&project, base](const time::Position& position)
			{ return time::rebase(position, base, project.tempo, project.sampleRate).value; };
			const int64_t from = inBase(fromPosition);
			const int64_t end = inBase(endPosition);
			const int64_t to = inBase(toPosition);
			if (to == from || end <= from)
				return;
			const int64_t landing = to + (end - from);
			for (auto& [id, lane] : project.automation)
			{
				if (lane.track != track)
					continue;
				std::vector<AutomationPoint> moving;
				for (const auto& [pointId, point] : lane.points)
					if (point.position.value >= from && point.position.value < end)
						moving.push_back(point);
				if (moving.empty())
					continue;
				for (auto point = lane.points.begin(); point != lane.points.end();)
				{
					const int64_t at = point->second.position.value;
					const bool moves = at >= from && at < end;
					const bool covered = at >= to && at < landing;
					point = covered && !moves ? lane.points.erase(point) : std::next(point);
				}
				for (const AutomationPoint& point : moving)
					lane.points[point.id].position.value = point.position.value + (to - from);
				context.emit("automationLane.changed", {{"lane", toJson(lane)}});
			}
		}

		// Where an audio event ends, in the time base of its position.
		time::Position eventEnd(const Project& project, const AudioEvent& event)
		{
			const MediaFile& media = project.media.at(project.clips.at(event.clip).media);
			const time::Samples start = time::toSamples(event.position, project.tempo, project.sampleRate);
			const auto length = std::llround(static_cast<double>(event.length) * project.sampleRate / media.sampleRate);
			return time::rebase(
				{time::TimeBase::Linear, start + length}, event.position.base, project.tempo, project.sampleRate);
		}

		// The input, output and instrument in the parameters, if any, for channel.set and track.add.
		void route(const Project& project, const Channel& channel, Channel& changed, const Json& parameters)
		{
			const TrackType type = project.trackById.at(channel.track).type;
			if (parameters.contains("input"))
			{
				changed.input = parameters.at("input").get<ObjectId>();
				require(type == TrackType::Audio || changed.input == 0, "only audio tracks have an input");
				const Bus* bus = busOrNull(project, changed.input);
				require(changed.input == 0 || (bus != nullptr && bus->direction == BusDirection::Input),
					"input must be an input bus");
			}
			if (parameters.contains("output"))
			{
				changed.output = parameters.at("output").get<ObjectId>();
				checkOutput(project, changed, changed.output);
			}
			if (parameters.contains("instrument"))
			{
				changed.instrument = parameters.at("instrument").get<std::string>();
				require(type == TrackType::Instrument || changed.instrument.empty(),
					"only an instrument track's channel plays an instrument");
				if (!changed.instrument.empty())
				{
					const processors::BuiltinInfo* info = processors::findBuiltin(changed.instrument);
					require(info != nullptr, "no built-in processor " + changed.instrument);
					require(info->instrument, changed.instrument + " is not an instrument");
				}
				// Another instrument starts with its parameters at their defaults.
				if (changed.instrument != channel.instrument)
					changed.instrumentParameters.clear();
			}
		}

		// A new track's time base without one given: the user setting editing.defaultTimeBase (reference manual
		// p. 230) for tracks that hold events, parts, markers or automation.
		time::TimeBase defaultTimeBase(const Project& project, TrackType type, CommandContext& context)
		{
			if (type == TrackType::Folder || type == TrackType::Ruler || type == TrackType::Tempo ||
				type == TrackType::Signature)
				return time::TimeBase::Musical;
			const std::string setting =
				context.call("settings.get", {{"key", "editing.defaultTimeBase"}}).at("value").get<std::string>();
			const bool linear =
				setting == "followTimeFormat" ? project.setup.timeFormat != "barsBeats" : setting == "linear";
			return linear ? time::TimeBase::Linear : time::TimeBase::Musical;
		}

		// Commands.

		Json addTrack(Project& project, CommandContext& context);

		// The project's Group or FX folder (reference manual pp. 150-152): the first folder named so at the top level,
		// added at its end if there is none.
		// ponytail: found by name, so a renamed folder gets a new one; a flag on the track if that matters.
		ObjectId projectFolder(Project& project, const std::string& name, CommandContext& context)
		{
			for (const ObjectId id : project.tracks)
				if (const Track& track = project.trackById.at(id);
					track.type == TrackType::Folder && track.name == name)
					return id;
			const Json parameters = {{"type", "folder"}, {"name", name}};
			CommandContext inner(parameters, &context);
			return addTrack(project, inner).at("track").at("id").get<ObjectId>();
		}

		// Group Track to Selected Tracks and Effect Track to Selected Tracks (reference manual pp. 546, 554): the
		// tracks play into a new group channel, or send to a new effect channel with the given send, or else their
		// first send without a destination, at the user setting audio.defaultSendLevel.
		void routeTo(Project& project, const Channel& added, const Json& parameters, CommandContext& context)
		{
			const TrackType type = project.trackById.at(added.track).type;
			int64_t number = 0;
			if (parameters.contains("send"))
			{
				require(type == TrackType::Effect, "only effect tracks take a send");
				number = parameters.at("send").get<int64_t>();
				requireRange(number, 1, static_cast<int64_t>(g_maxSends), "send");
			}
			const double level =
				std::clamp(context.call("settings.get", {{"key", "audio.defaultSendLevel"}}).at("value").get<double>(),
					g_minChannelVolume, project.maxVolume);
			std::set<ObjectId> seen;
			for (const Json& item : parameters.at("tracks"))
			{
				const auto id = item.get<ObjectId>();
				require(seen.insert(id).second, "tracks lists track " + idText(id) + " twice");
				const Track& track = find(project.trackById, id, "track");
				const auto source = project.channels.find(track.channel);
				require(source != project.channels.end() && source->second.format != ChannelFormat::None,
					"track " + idText(id) + " has no audio channel");
				Channel& channel = source->second;
				if (type == TrackType::Group)
				{
					checkOutput(project, channel, added.id);
					channel.output = added.id;
				}
				else
				{
					// A pointer, not an iterator: std::array's iterator is a pointer only in some libraries.
					Send* send = number > 0 ? &channel.sends.at(static_cast<std::size_t>(number - 1)) : nullptr;
					for (Send& candidate : channel.sends)
					{
						if (send == nullptr && candidate.destination == 0)
						{
							send = &candidate;
						}
					}
					require(send != nullptr, "track " + idText(id) + " has no free send");
					require(!routingLoops(project, channel.id, added.id),
						"sending channel " + idText(channel.id) + " to " + idText(added.id) + " would make a loop");
					*send = {added.id, true, level, false};
				}
				context.emit("channel.changed", {{"channel", toJson(channel)}});
			}
		}

		Json addTrack(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const TrackType type = trackType(parameters.at("type").get<std::string>());
			ObjectId parent = folderParent(project, parameters);
			if (parameters.value("projectFolder", false))
			{
				require(type == TrackType::Group || type == TrackType::Effect,
					"only group and effect tracks go into the project's Group and FX folders");
				require(parent == 0, "a track in the project's folder has no other parent");
				parent = projectFolder(project, type == TrackType::Group ? "Group" : "FX", context);
			}
			const size_t index = insertIndex(parameters, siblings(project, parent));
			const bool groupChannel = parameters.value("groupChannel", false);
			require(!groupChannel || type == TrackType::Folder, "only folder tracks have an optional group channel");
			if (type == TrackType::Marker)
			{
				require(parent == 0, "marker tracks can't be inside a folder");
				require(countTracks(project, TrackType::Marker) < g_maxMarkerTracks,
					"a project has at most " + std::to_string(g_maxMarkerTracks) + " marker tracks");
			}
			if (type == TrackType::Tempo)
			{
				require(parent == 0, "the tempo track can't be inside a folder");
				require(countTracks(project, TrackType::Tempo) == 0, "a project has one tempo track at most");
			}
			if (type == TrackType::Signature)
			{
				require(parent == 0, "the signature track can't be inside a folder");
				require(countTracks(project, TrackType::Signature) == 0, "a project has one signature track at most");
			}
			const bool hasChannel =
				(type != TrackType::Folder && type != TrackType::Marker && type != TrackType::Ruler &&
					type != TrackType::Tempo && type != TrackType::Signature) ||
				groupChannel;
			const bool hasAudio = hasChannel && type != TrackType::Midi && type != TrackType::Vca;
			require(type != TrackType::Vca ||
					(!parameters.contains("input") && !parameters.contains("output") &&
						!parameters.contains("instrument")),
				"a VCA track has no input, output or instrument");
			require(hasAudio || (!parameters.contains("channels") && !parameters.contains("format")),
				"this track has no audio channels");
			require(hasChannel ||
					(!parameters.contains("input") && !parameters.contains("output") &&
						!parameters.contains("instrument")),
				"this track has no channel");
			require(
				type == TrackType::Ruler || !parameters.contains("timeFormat"), "only ruler tracks have a time format");
			const ChannelFormat format =
				hasAudio ? channelFormat(parameters, ChannelFormat::Stereo) : ChannelFormat::None;

			Track track;
			track.id = project.newId();
			track.type = type;
			track.name = parameters.value("name", defaultTrackName(project, type));
			track.color =
				parameters.contains("color") ? color(parameters, {}) : autoTrackColor(project, parent, index, context);
			track.timeBase = parameters.contains("timeBase")
				? timeBaseFromName(parameters.at("timeBase").get<std::string>())
				: defaultTimeBase(project, type, context);
			track.parent = parent;
			if (type == TrackType::Ruler)
				track.timeFormat = parameters.value("timeFormat", g_rulerTimeFormats[0]);

			// New tracks play through the main output, or through the group channel of the folder they are added to.
			std::optional<Channel> channel;
			if (hasChannel)
			{
				channel = Channel{project.newId(), track.id, format, 0, 0, {}, 0.0, 0.0};
				if (hasAudio)
					channel->output = folderOutput(project, parent);
				if (type == TrackType::Audio)
				{
					const auto mainInput = std::find_if(project.busses.begin(), project.busses.end(),
						[](const Bus& bus) { return bus.direction == BusDirection::Input; });
					if (mainInput != project.busses.end())
						channel->input = mainInput->id;
				}
				track.channel = channel->id;
				project.channels[channel->id] = *channel;
			}
			std::vector<ObjectId>& list = siblings(project, parent);
			list.insert(list.begin() + static_cast<std::ptrdiff_t>(index), track.id);
			project.trackById[track.id] = track;
			// Routing is checked with the track in place; a failure restores the project.
			if (channel)
			{
				route(project, *channel, project.channels.at(channel->id), parameters);
				channel = project.channels.at(channel->id);
			}

			if (const std::string inherited =
					parameters.contains("color") ? std::string() : folderColor(project, parent, context);
				!inherited.empty())
				project.trackById.at(track.id).color = inherited;

			Json result = {{"track", toJson(project.trackById.at(track.id))},
				{"channel", channel ? toJson(*channel) : Json(nullptr)}};
			context.emit("track.added", result);
			if (type == TrackType::Marker) // a new marker track is the active one
				activateMarkerTrack(project, track.id, context);
			if (parameters.contains("tracks") && channel && (type == TrackType::Group || type == TrackType::Effect))
			{
				routeTo(project, *channel, parameters, context);
				return result;
			}
			require(!parameters.contains("send"), "only effect tracks take a send");
			// Move Selected Tracks to New Folder (p. 223).
			if (parameters.contains("tracks"))
			{
				require(type == TrackType::Folder, "only folders, group and effect tracks take tracks");
				for (const Json& moved : parameters.at("tracks"))
				{
					const auto id = moved.get<ObjectId>();
					require(find(project.trackById, id, "track").parent != track.id,
						"tracks lists track " + idText(id) + " twice");
					moveInto(project, id, track.id,
						static_cast<int64_t>(project.trackById.at(track.id).children.size()), context);
				}
			}
			return result;
		}

		// Disconnects every channel whose input, output or send destination is one of the given objects.
		void disconnect(Project& project, const std::vector<ObjectId>& gone, CommandContext& context)
		{
			const auto isGone = [&gone](ObjectId id) { return std::find(gone.begin(), gone.end(), id) != gone.end(); };
			for (auto& [id, channel] : project.channels)
			{
				const bool input = channel.input != 0 && isGone(channel.input);
				const bool output = channel.output != 0 && isGone(channel.output);
				bool send = false;
				for (Send& item : channel.sends)
					if (item.destination != 0 && isGone(item.destination))
					{
						item.destination = 0;
						send = true;
					}
				if (input)
					channel.input = 0;
				if (output)
					channel.output = 0;
				if (input || output || send)
					context.emit("channel.changed", {{"channel", toJson(channel)}});
			}
			// Link groups lose the channels that are gone and VCA faders that are gone (docs/features/link-groups.md).
			for (auto& [id, group] : project.linkGroups)
			{
				const LinkGroup before = group;
				group.channels.erase(
					std::remove_if(group.channels.begin(), group.channels.end(), isGone), group.channels.end());
				if (isGone(group.vca))
					group.vca = 0;
				if (!(group == before))
					context.emit("linkGroup.changed", {{"group", toJson(group)}});
			}
		}

		// Turns a folder's group channel on or off (reference manual pp. 222, 224): a new one plays through the
		// folder's own folder output; the tracks inside follow it. Without it, the folder's automation goes and the
		// channels routed to it from outside are disconnected.
		void setGroupChannel(Project& project, Track& folder, bool on, CommandContext& context)
		{
			require(folder.type == TrackType::Folder, "only folder tracks have an optional group channel");
			if (on == (folder.channel != 0))
				return;
			const std::map<ObjectId, ObjectId> outputs = folderOutputs(project, folder.id);
			if (on)
			{
				const Channel channel{project.newId(), folder.id, ChannelFormat::Stereo, 0,
					folderOutput(project, folder.parent), {}, 0.0, 0.0};
				project.channels[channel.id] = channel;
				folder.channel = channel.id;
				context.emit("channel.added", {{"channel", toJson(channel)}});
				reroute(project, outputs, context);
				return;
			}
			const ObjectId gone = std::exchange(folder.channel, 0);
			project.channels.erase(gone);
			context.emit("channel.removed", {{"channel", gone}});
			for (auto lane = project.automation.begin(); lane != project.automation.end();)
			{
				if (lane->second.track != folder.id)
				{
					++lane;
					continue;
				}
				context.emit("automationLane.removed", {{"lane", lane->first}});
				lane = project.automation.erase(lane);
			}
			reroute(project, outputs, context);
			disconnect(project, {gone}, context);
		}

		Json setTrack(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			Track& track = find(project.trackById, parameters.at("track").get<ObjectId>(), "track");
			const std::string newColor = color(parameters, track.color);
			require(track.type == TrackType::Ruler || !parameters.contains("timeFormat"),
				"only ruler tracks have a time format");
			if (parameters.contains("name"))
				track.name = parameters.at("name").get<std::string>();
			track.color = newColor;
			track.timeFormat = parameters.value("timeFormat", track.timeFormat);
			track.picture = parameters.value("picture", track.picture);
			if (parameters.contains("enabled"))
			{
				const bool enabled = parameters.at("enabled").get<bool>();
				require(enabled || canDisable(track.type), "only audio, instrument and MIDI tracks can be disabled");
				track.enabled = enabled;
			}
			if (parameters.contains("timeBase"))
			{
				const time::TimeBase base = timeBaseFromName(parameters.at("timeBase").get<std::string>());
				if (base != track.timeBase)
					rebaseTrack(project, track, base, context);
			}
			if (parameters.contains("groupChannel"))
				setGroupChannel(project, track, parameters.at("groupChannel").get<bool>(), context);
			Json result = {{"track", toJson(track)}};
			context.emit("track.changed", result);
			if (track.type == TrackType::Folder && parameters.contains("color"))
				paint(project, track.id, folderColor(project, track.id, context), false, context);
			return result;
		}

		// Copies a track with its channel, events, parts, markers and automation, and the tracks inside it, into the
		// folder parent; returns the copy, not yet among parent's tracks. channels maps each copied channel to its
		// copy.
		ObjectId copyTrack(Project& project, ObjectId id, ObjectId parent, std::map<ObjectId, ObjectId>& channels,
			std::vector<std::pair<std::string, Json>>& added)
		{
			Track copy = project.trackById.at(id);
			copy.id = project.newId();
			copy.parent = parent;
			copy.children.clear();
			std::map<ObjectId, ObjectId> slots; // each copied slot's copy
			if (copy.channel != 0)
			{
				Channel channel = project.channels.at(copy.channel);
				channel.id = project.newId();
				channel.track = copy.id;
				for (ObjectId& slotId : channel.slots)
				{
					Slot slot = project.slots.at(slotId);
					slot.id = project.newId();
					slot.channel = channel.id;
					slots[slotId] = slot.id;
					slotId = slot.id;
					project.slots[slot.id] = std::move(slot);
				}
				channels[copy.channel] = channel.id;
				copy.channel = channel.id;
				project.channels[channel.id] = std::move(channel);
			}
			added.emplace_back("track.added", Json{{"track", nullptr}, {"channel", nullptr}});
			const size_t trackEvent = added.size() - 1;

			// Audio parts hold the copies of their events.
			std::map<ObjectId, ObjectId> audioParts;
			std::vector<AudioPart> copiedParts;
			for (const auto& [partId, part] : project.audioParts)
				if (part.track == id)
					copiedParts.push_back(part);
			for (AudioPart& part : copiedParts)
			{
				audioParts[part.id] = project.newId();
				part.id = audioParts.at(part.id);
				part.track = copy.id;
				project.audioParts[part.id] = part;
				added.emplace_back("audioPart.added", Json{{"part", toJson(part)}});
			}
			// Crossfades join the copies of the events they joined.
			std::map<ObjectId, ObjectId> events;
			std::vector<AudioEvent> audio;
			for (const auto& [eventId, event] : project.audioEvents)
				if (event.track == id)
				{
					audio.push_back(event);
					events[eventId] = project.newId();
				}
			for (AudioEvent& event : audio)
			{
				event.id = events.at(event.id);
				event.track = copy.id;
				const auto from = events.find(event.crossfade.from);
				event.crossfade.from = from == events.end() ? 0 : from->second;
				const auto part = audioParts.find(event.part);
				event.part = part == audioParts.end() ? 0 : part->second;
				project.audioEvents[event.id] = event;
				added.emplace_back("audioEvent.added", Json{{"event", toJson(event)}});
			}
			std::vector<MidiPart> parts;
			for (const auto& [partId, part] : project.midiParts)
				if (part.track == id)
					parts.push_back(part);
			for (MidiPart& part : parts)
			{
				part.id = project.newId();
				part.track = copy.id;
				if (part.shared == 0) // a shared copy stays one, with the notes and controllers it shares
					renumber(project, part);
				added.emplace_back("midiPart.added", Json{{"part", toJson(part)}});
				project.midiParts[part.id] = std::move(part);
			}
			std::vector<Marker> markers;
			for (const auto& [markerId, marker] : project.markers)
				if (marker.track == id)
					markers.push_back(marker);
			for (Marker& marker : markers)
			{
				marker.id = project.newId();
				marker.track = copy.id;
				project.markers[marker.id] = marker;
				added.emplace_back("marker.added", Json{{"marker", toJson(marker)}});
			}
			std::vector<AutomationLane> lanes;
			for (const auto& [laneId, lane] : project.automation)
				if (lane.track == id)
					lanes.push_back(lane);
			for (AutomationLane& lane : lanes)
			{
				lane.id = project.newId();
				lane.track = copy.id;
				if (lane.target.type == AutomationTargetType::Plugin)
					lane.target.slot = slots.at(lane.target.slot);
				std::map<ObjectId, AutomationPoint> points;
				for (const auto& [pointId, point] : lane.points)
				{
					AutomationPoint renumbered = point;
					renumbered.id = project.newId();
					points[renumbered.id] = renumbered;
				}
				lane.points = std::move(points);
				added.emplace_back("automationLane.added", Json{{"lane", toJson(lane)}});
				project.automation[lane.id] = std::move(lane);
			}

			const std::vector<ObjectId> children = project.trackById.at(id).children;
			for (const ObjectId child : children)
				copy.children.push_back(copyTrack(project, child, copy.id, channels, added));
			added[trackEvent].second["track"] = toJson(copy);
			const ObjectId copyId = copy.id;
			project.trackById[copyId] = std::move(copy);
			return copyId;
		}

		Json duplicateTrack(Project& project, CommandContext& context)
		{
			const ObjectId id = context.parameters().at("track").get<ObjectId>();
			const Track& track = find(project.trackById, id, "track");
			require(track.type != TrackType::Marker || countTracks(project, TrackType::Marker) < g_maxMarkerTracks,
				"a project has at most " + std::to_string(g_maxMarkerTracks) + " marker tracks");
			require(track.type != TrackType::Tempo, "a project has one tempo track at most");
			require(track.type != TrackType::Signature, "a project has one signature track at most");
			const ObjectId parent = track.parent;
			std::map<ObjectId, ObjectId> channels;
			std::vector<std::pair<std::string, Json>> added;
			const ObjectId copy = copyTrack(project, id, parent, channels, added);
			// Channels that played or sent to a copied channel play or send to its copy.
			for (const auto& [original, duplicate] : channels)
			{
				Channel& channel = project.channels.at(duplicate);
				if (const auto output = channels.find(channel.output); output != channels.end())
					channel.output = output->second;
				for (Send& send : channel.sends)
					if (const auto destination = channels.find(send.destination); destination != channels.end())
						send.destination = destination->second;
			}
			std::vector<ObjectId>& list = siblings(project, parent);
			list.insert(std::next(std::find(list.begin(), list.end(), id)), copy);
			for (auto& [name, data] : added)
			{
				if (name == "track.added" && data.at("track").at("channel") != 0)
					data["channel"] = toJson(project.channels.at(data.at("track").at("channel").get<ObjectId>()));
				context.emit(name, data);
			}
			return {{"track", toJson(project.trackById.at(copy))}};
		}

		Json moveTrack(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			moveInto(project, parameters.at("track").get<ObjectId>(), folderParent(project, parameters),
				parameters.at("index").get<int64_t>(), context);
			return Json::object();
		}

		Json removeTrack(Project& project, CommandContext& context)
		{
			const ObjectId id = context.parameters().at("track").get<ObjectId>();
			const Track& track = find(project.trackById, id, "track");
			std::vector<ObjectId>& list = siblings(project, track.parent);
			list.erase(std::find(list.begin(), list.end(), id));

			std::vector<ObjectId> channels;
			for (const ObjectId removed : subtree(project, id))
			{
				const Track& gone = project.trackById.at(removed);
				if (gone.channel != 0)
					channels.push_back(gone.channel);
				for (auto event = project.audioEvents.begin(); event != project.audioEvents.end();)
					event = event->second.track == removed ? project.audioEvents.erase(event) : std::next(event);
				for (auto part = project.audioParts.begin(); part != project.audioParts.end();)
					part = part->second.track == removed ? project.audioParts.erase(part) : std::next(part);
				for (auto part = project.midiParts.begin(); part != project.midiParts.end();)
					part = part->second.track == removed ? project.midiParts.erase(part) : std::next(part);
				for (auto marker = project.markers.begin(); marker != project.markers.end();)
					marker = marker->second.track == removed ? project.markers.erase(marker) : std::next(marker);
				for (auto lane = project.automation.begin(); lane != project.automation.end();)
					lane = lane->second.track == removed ? project.automation.erase(lane) : std::next(lane);
				if (gone.channel != 0)
					for (const ObjectId slot : project.channels.at(gone.channel).slots)
						project.slots.erase(slot);
				project.channels.erase(gone.channel);
				project.trackById.erase(removed);
				context.emit("track.removed", {{"track", removed}});
			}
			disconnect(project, channels, context);
			// Without the active marker track, the topmost marker track is the active one.
			if (project.activeMarkerTrack == id)
			{
				const auto top = std::find_if(project.tracks.begin(), project.tracks.end(),
					[&project](ObjectId track) { return project.trackById.at(track).type == TrackType::Marker; });
				activateMarkerTrack(project, top == project.tracks.end() ? 0 : *top, context);
			}
			return Json::object();
		}

		// The tracks track.removeEmpty removes (docs/features/track-list.md): audio, instrument, MIDI and marker
		// tracks without events, parts, markers or automation, and folders without tracks, unless a channel of
		// another track plays or sends into theirs. Group and effect tracks are where others play through, VCA
		// tracks control channels, ruler, tempo and signature tracks hold no events. What goes can leave others
		// empty, such as a folder that held only empty tracks or an instrument track that only empty MIDI tracks
		// played, so it looks again until nothing more goes. What a track holds is the same in the track list's
		// warning before Remove Selected Tracks (TrackList::build, src/ui/tracklist.cpp): change both together.
		std::set<ObjectId> emptyTracks(const Project& project)
		{
			std::set<ObjectId> holding;
			const auto hold = [&holding](const auto& objects)
			{
				for (const auto& item : objects)
					holding.insert(item.second.track);
			};
			hold(project.audioEvents);
			hold(project.audioParts);
			hold(project.midiParts);
			hold(project.markers);
			hold(project.automation);
			std::set<ObjectId> gone;
			size_t before = 0;
			do
			{
				before = gone.size();
				std::set<ObjectId> filled; // folders with tracks that stay
				std::set<ObjectId> played; // channels that tracks that stay play or send into
				for (const auto& [id, track] : project.trackById)
				{
					if (gone.count(id) != 0)
						continue;
					filled.insert(track.parent);
					if (track.channel == 0)
						continue;
					const Channel& channel = project.channels.at(track.channel);
					played.insert(channel.output);
					for (const Send& send : channel.sends)
						played.insert(send.destination);
				}
				for (const auto& [id, track] : project.trackById)
				{
					const bool kind = track.type == TrackType::Audio || track.type == TrackType::Instrument ||
						track.type == TrackType::Midi || track.type == TrackType::Marker ||
						track.type == TrackType::Folder;
					if (kind && holding.count(id) == 0 && filled.count(id) == 0 &&
						(track.channel == 0 || played.count(track.channel) == 0))
						gone.insert(id);
				}
			} while (gone.size() != before);
			return gone;
		}

		Json removeEmptyTracks(Project& project, CommandContext& context)
		{
			const std::set<ObjectId> gone = emptyTracks(project);
			Json removed = Json::array();
			std::vector<ObjectId> top; // a folder takes the tracks inside it along
			for (const ObjectId id : project.trackOrder())
				if (gone.count(id) != 0)
				{
					removed.push_back(id);
					if (gone.count(project.trackById.at(id).parent) == 0)
						top.push_back(id);
				}
			for (const ObjectId id : top)
			{
				const Json parameters = {{"track", id}}; // the context keeps a reference
				CommandContext inner(parameters, &context);
				removeTrack(project, inner);
			}
			return Json{{"tracks", std::move(removed)}};
		}

		// channel.set's sends: each changes what it has of the send with its number.
		void setSends(const Project& project, Channel& changed, const Json& sends)
		{
			require(changed.format != ChannelFormat::None, "a MIDI channel has no sends");
			for (const Json& item : sends)
			{
				const auto number = item.at("send").get<int64_t>();
				requireRange(number, 1, static_cast<int64_t>(g_maxSends), "send");
				Send& send = changed.sends[static_cast<size_t>(number - 1)];
				if (item.contains("destination"))
				{
					send.destination = item.at("destination").get<ObjectId>();
					require(send.destination == 0 || isSendDestination(project, send.destination),
						"a send's destination must be a group or effect channel");
					require(send.destination == 0 || !routingLoops(project, changed.id, send.destination),
						"sending channel " + idText(changed.id) + " to " + idText(send.destination) +
							" would make a loop");
				}
				send.on = item.value("on", send.on);
				// Like the fader, the level stops at its ends.
				if (item.contains("level"))
					send.level = std::clamp(item.at("level").get<double>(), g_minChannelVolume, project.maxVolume);
				send.preFader = item.value("preFader", send.preFader);
			}
		}

		// Whether a link group links a channel.set key (reference manual p. 458).
		bool links(const LinkGroup& group, const std::string& key)
		{
			if (key == "volume")
				return group.volume;
			if (key == "pan")
				return group.pan;
			if (key == "monitor" || key == "recordEnable")
				return group.record;
			if (key == "eq")
				return group.eq;
			if (key == "strip")
				return group.dynamics;
			if (key == "sends")
				return group.sends;
			return group.mute; // mute, solo, soloDefeat and listen
		}

		// The channels a change of key on a channel goes on to (docs/features/link-groups.md): the other channels of
		// its link group if the group links key (and linked), the channels of the group it controls if it is a VCA
		// fader and key is one of its buttons, and from each of them on in the same way.
		std::vector<ObjectId> linkedTo(const Project& project, ObjectId channel, const std::string& key, bool linked)
		{
			const bool button = key != "volume" && key != "pan" && key != "eq" && key != "strip" && key != "sends";
			std::vector<ObjectId> result;
			std::set<ObjectId> seen{channel};
			std::vector<ObjectId> pending{channel};
			while (!pending.empty())
			{
				const ObjectId at = pending.back();
				pending.pop_back();
				std::vector<ObjectId> next;
				if (const LinkGroup* group = linkGroupOf(project, at); linked && group != nullptr && links(*group, key))
					next = group->channels;
				if (const LinkGroup* group = controlledBy(project, at); button && group != nullptr)
					next.insert(next.end(), group->channels.begin(), group->channels.end());
				for (const ObjectId id : next)
					if (seen.insert(id).second)
					{
						result.push_back(id);
						pending.push_back(id);
					}
			}
			return result;
		}

		// Passes channel.set's fader, pan and buttons on to the channels linked to the channel: the fader and the pan
		// by as much as they moved, stopping at their ends, the buttons as they are now; and its eq, strip and sends
		// as they were given, to each channel that can take them. Returns the channels the solo went on to.
		std::set<ObjectId> passOn(
			Project& project, const Channel& before, ObjectId id, const Json& parameters, CommandContext& context)
		{
			const Channel& channel = project.channels.at(id);
			const bool linked = parameters.value("linked", true);
			std::map<ObjectId, Channel> changed;
			const auto follower = [&](ObjectId other) -> Channel&
			{
				const auto found = changed.find(other);
				return found != changed.end() ? found->second : changed[other] = project.channels.at(other);
			};
			const auto typeOf = [&project](const Channel& other) { return project.trackById.at(other.track).type; };
			std::set<ObjectId> soloed;
			if (parameters.contains("volume") && channel.volume != before.volume)
				for (const ObjectId other : linkedTo(project, id, "volume", linked))
				{
					Channel& to = follower(other);
					if (to.format != ChannelFormat::None || typeOf(to) == TrackType::Vca)
						to.volume = std::clamp(
							to.volume + (channel.volume - before.volume), g_minChannelVolume, project.maxVolume);
				}
			if (parameters.contains("pan") && channel.pan != before.pan)
				for (const ObjectId other : linkedTo(project, id, "pan", linked))
				{
					Channel& to = follower(other);
					if (to.format != ChannelFormat::None)
						to.pan = std::clamp(to.pan + (channel.pan - before.pan), -1.0, 1.0);
				}
			for (const char* key : {"eq", "strip", "sends"})
			{
				if (!parameters.contains(key))
					continue;
				for (const ObjectId other : linkedTo(project, id, key, linked))
				{
					Channel& to = follower(other);
					Channel trial = to;
					try
					{
						if (std::string(key) == "sends")
							setSends(project, trial, parameters.at(key));
						else
							trial.sections = sectionsFromJson(Json{{key, parameters.at(key)}}, trial.sections);
					}
					catch (const std::exception&)
					{
						continue; // a MIDI channel, or a send to itself: it keeps its own
					}
					to = trial;
				}
			}
			const std::pair<const char*, bool Channel::*> buttons[] = {{"mute", &Channel::mute},
				{"solo", &Channel::solo}, {"soloDefeat", &Channel::soloDefeat}, {"listen", &Channel::listen},
				{"monitor", &Channel::monitor}, {"recordEnable", &Channel::recordEnable}};
			for (const auto& [key, member] : buttons)
			{
				if (!parameters.contains(key))
					continue;
				const std::string name = key;
				for (const ObjectId other : linkedTo(project, id, name, linked))
				{
					Channel& to = follower(other);
					const TrackType type = typeOf(to);
					if (name == "listen" && to.format == ChannelFormat::None && type != TrackType::Vca)
						continue;
					if ((name == "monitor" || name == "recordEnable") && type != TrackType::Audio &&
						type != TrackType::Midi && type != TrackType::Instrument && type != TrackType::Vca)
						continue;
					to.*member = channel.*member;
					if (name == "solo")
						soloed.insert(other);
				}
			}
			for (auto& [other, to] : changed)
			{
				Channel& current = project.channels.at(other);
				if (current == to)
					continue;
				current = to;
				context.emit("channel.changed", {{"channel", toJson(current)}});
			}
			return soloed;
		}

		Json setChannel(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			Channel& channel = find(project.channels, parameters.at("channel").get<ObjectId>(), "channel");
			const TrackType type = project.trackById.at(channel.track).type;
			Channel changed = channel;
			if (parameters.contains("channels") || parameters.contains("format"))
			{
				require(type != TrackType::Midi, "a MIDI channel has no audio channels");
				require(type != TrackType::Vca, "a VCA channel has no audio channels");
				changed.format = channelFormat(parameters, channel.format);
			}
			require(type != TrackType::Vca ||
					(!parameters.contains("input") && !parameters.contains("output") &&
						!parameters.contains("instrument")),
				"a VCA channel has no input, output or instrument");
			route(project, channel, changed, parameters);
			for (const char* key : {"volume", "pan", "panner", "panLeft", "panRight", "panBypass"})
				if (parameters.contains(key))
				{
					require(type != TrackType::Vca || std::string(key) == "volume", "a VCA channel has no pan");
					require(changed.format != ChannelFormat::None || type == TrackType::Vca,
						"a MIDI channel has no volume or pan");
				}
			// The fader stops at its ends.
			if (parameters.contains("volume"))
				changed.volume =
					std::clamp(parameters.at("volume").get<double>(), g_minChannelVolume, project.maxVolume);
			for (const auto& [key, value] : {std::pair{"pan", &changed.pan}, std::pair{"panLeft", &changed.panLeft},
					 std::pair{"panRight", &changed.panRight}})
			{
				*value = parameters.value(key, *value);
				require(*value >= -1.0 && *value <= 1.0, std::string(key) + " must be from -1 to 1");
			}
			if (parameters.contains("panner"))
				changed.panner = parameters.at("panner") == "combined" ? StereoPanner::Combined : StereoPanner::Balance;
			require(changed.panner == StereoPanner::Balance || changed.format == ChannelFormat::Stereo,
				"the combined panner is for stereo channels");
			changed.panBypass = parameters.value("panBypass", changed.panBypass);
			for (const auto& [key, value] :
				{std::pair{"midiVolume", &changed.midiVolume}, std::pair{"midiPan", &changed.midiPan}})
			{
				if (!parameters.contains(key))
					continue;
				require(type == TrackType::Midi, std::string("only MIDI channels have ") + key);
				const Json& given = parameters.at(key);
				if (given.is_null())
				{
					value->reset();
					continue;
				}
				const auto number = given.get<int64_t>();
				requireRange(number, 0, 127, key);
				*value = static_cast<int>(number);
			}
			for (const auto& [key, value] :
				{std::pair{"monitor", &changed.monitor}, std::pair{"recordEnable", &changed.recordEnable}})
			{
				if (!parameters.contains(key))
					continue;
				require(type == TrackType::Audio || type == TrackType::Midi || type == TrackType::Instrument ||
						type == TrackType::Vca,
					std::string("only audio, MIDI and instrument tracks and VCA faders have ") + key);
				*value = parameters.at(key).get<bool>();
			}
			if (parameters.contains("midiInput"))
			{
				require(type == TrackType::Midi || type == TrackType::Instrument,
					"only MIDI and instrument tracks have midiInput");
				changed.midiInput = parameters.at("midiInput").get<std::string>();
			}
			changed.mute = parameters.value("mute", changed.mute);
			changed.solo = parameters.value("solo", changed.solo);
			changed.soloDefeat = parameters.value("soloDefeat", changed.soloDefeat);
			const bool listens = changed.format != ChannelFormat::None || type == TrackType::Vca;
			if (parameters.contains("listen"))
				require(listens, "a MIDI channel has no listen");
			changed.listen = parameters.value("listen", changed.listen) && listens;
			changed.notes = parameters.value("notes", changed.notes);
			if (parameters.contains("pre") || parameters.contains("eq") || parameters.contains("strip"))
			{
				require(changed.format != ChannelFormat::None, "a MIDI channel has no pre, eq or strip section");
				try
				{
					changed.sections = sectionsFromJson(parameters, changed.sections);
				}
				catch (const std::invalid_argument& error)
				{
					throw CommandError(error.what());
				}
			}
			if (parameters.contains("modifiers"))
			{
				require(type == TrackType::Midi || type == TrackType::Instrument,
					"only MIDI and instrument tracks have modifiers");
				try
				{
					changed.modifiers = modifiersFromJson(parameters.at("modifiers"), changed.modifiers);
				}
				catch (const std::invalid_argument& error)
				{
					throw CommandError(error.what());
				}
			}
			if (parameters.contains("sends"))
				setSends(project, changed, parameters.at("sends"));
			const Channel before = channel;
			channel = changed;
			const std::set<ObjectId> soloed = passOn(project, before, channel.id, parameters, context);
			// Exclusive solo (reference manual p. 466): every other channel's solo goes off, but for those the solo
			// went on to.
			if (parameters.value("exclusive", false))
				for (auto& [id, other] : project.channels)
					if (id != channel.id && other.solo && soloed.count(id) == 0)
					{
						other.solo = false;
						context.emit("channel.changed", {{"channel", toJson(other)}});
					}
			Json result = {{"channel", toJson(channel)}};
			context.emit("channel.changed", result);
			return result;
		}

		// Deactivate All Mute, Solo or Listen States (reference manual pp. 58, 466). Solo takes solo defeat along.
		Json deactivateAll(Project& project, CommandContext& context)
		{
			const std::string state = context.parameters().at("state").get<std::string>();
			require(state == "mute" || state == "solo" || state == "listen", "state must be mute, solo or listen");
			Json channels = Json::array();
			for (auto& [id, channel] : project.channels)
			{
				const Channel before = channel;
				if (state == "mute")
					channel.mute = false;
				else if (state == "listen")
					channel.listen = false;
				else
					channel.solo = channel.soloDefeat = false;
				if (channel == before)
					continue;
				context.emit("channel.changed", {{"channel", toJson(channel)}});
				channels.push_back(id);
			}
			return {{"channels", std::move(channels)}};
		}

		BusDirection busDirection(const Json& name)
		{
			return name.get<std::string>() == "input" ? BusDirection::Input : BusDirection::Output;
		}

		void announceBusses(const std::vector<Bus>& previous, const std::vector<Bus>& current, CommandContext& context)
		{
			const auto find = [](const std::vector<Bus>& busses, ObjectId id)
			{ return std::find_if(busses.begin(), busses.end(), [id](const Bus& bus) { return bus.id == id; }); };
			for (const Bus& bus : current)
			{
				const auto match = find(previous, bus.id);
				if (match == previous.end())
					context.emit("bus.added", {{"bus", toJson(bus)}});
				else if (!(*match == bus))
					context.emit("bus.changed", {{"bus", toJson(bus)}});
			}
			for (const Bus& bus : previous)
				if (find(current, bus.id) == current.end())
					context.emit("bus.removed", {{"bus", bus.id}});
		}

		void checkBus(const Project& project, const Bus& bus)
		{
			const std::string problem = busProblem(project, bus);
			require(problem.empty(), problem);
		}

		// "Stereo In", then "Stereo In 2" and so on.
		std::string uniqueBusName(const Project& project, const std::string& base)
		{
			const auto taken = [&project](const std::string& name)
			{
				return std::any_of(
					project.busses.begin(), project.busses.end(), [&name](const Bus& bus) { return bus.name == name; });
			};
			std::string name = base;
			for (int n = 2; taken(name); ++n)
				name = base + " " + std::to_string(n);
			return name;
		}

		// A format at the start of a bus name: "Mono", "Stereo", "5.1".
		std::string formatLabel(ChannelFormat format)
		{
			std::string label = formatName(format);
			label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
			return label;
		}

		Json addBus(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			Bus bus;
			bus.parent = parameters.value("parent", ObjectId{0});
			const Bus* parent = bus.parent != 0 ? &findBus(project, bus.parent) : nullptr;
			require(parameters.contains("direction") || parent != nullptr, "a bus needs a direction: input or output");
			bus.direction = parent != nullptr && !parameters.contains("direction")
				? parent->direction
				: busDirection(parameters.at("direction"));
			bus.format = channelFormat(parameters, ChannelFormat::Stereo);
			bus.ports = parameters.value("ports", std::vector<std::string>());
			if (parameters.contains("parentChannels")) // without a parent, checkBus refuses them
				bus.parentChannels = parentChannelsFromJson(parameters.at("parentChannels"));
			if (parent != nullptr)
			{
				require(parent->parent == 0, "a child bus's parent must be a top-level bus");
				if (!parameters.contains("parentChannels"))
					// The speakers both formats have.
					for (const Speaker speaker : speakers(bus.format))
					{
						const std::vector<Speaker>& available = speakers(parent->format);
						const auto found = std::find(available.begin(), available.end(), speaker);
						require(found != available.end(),
							std::string("the parent doesn't have the speakers of a ") + formatName(bus.format) +
								" bus; give parentChannels");
						bus.parentChannels.push_back(static_cast<int>(found - available.begin()));
					}
			}
			checkBus(project, bus);
			if (parameters.contains("name"))
			{
				bus.name = parameters.at("name").get<std::string>();
			}
			else if (parent != nullptr)
			{
				std::string names;
				for (const int channel : bus.parentChannels)
					names.append(names.empty() ? "" : " ")
						.append(speakerName(speakers(parent->format)[static_cast<size_t>(channel)]));
				bus.name = uniqueBusName(project, formatLabel(bus.format) + " (" + names + ")");
			}
			else
			{
				bus.name = uniqueBusName(
					project, formatLabel(bus.format) + (bus.direction == BusDirection::Input ? " In" : " Out"));
			}
			bus.id = project.newId();
			project.busses.push_back(bus);
			keepMainMix(project); // the first output bus
			Json result = {{"bus", toJson(project.busses.back())}};
			context.emit("bus.added", result);
			return result;
		}

		Json setBus(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const std::vector<Bus> previous = project.busses;
			Bus& bus = findBus(project, parameters.at("bus").get<ObjectId>());
			if (parameters.contains("name"))
				bus.name = parameters.at("name").get<std::string>();
			if (parameters.contains("ports"))
				bus.ports = parameters.at("ports").get<std::vector<std::string>>();
			if (parameters.contains("mainMix"))
			{
				require(parameters.at("mainMix").get<bool>(), "make another bus the main mix instead");
				require(bus.direction == BusDirection::Output, "only an output bus can be the main mix");
				for (Bus& other : project.busses)
					other.mainMix = false;
				bus.mainMix = true;
			}
			checkBus(project, bus);
			announceBusses(previous, project.busses, context);
			return {{"bus", toJson(bus)}};
		}

		Json removeBus(Project& project, CommandContext& context)
		{
			const ObjectId id = context.parameters().at("bus").get<ObjectId>();
			findBus(project, id);
			const std::vector<Bus> previous = project.busses;
			std::vector<ObjectId> gone; // the bus and its children
			for (const Bus& bus : project.busses)
				if (bus.id == id || bus.parent == id)
					gone.push_back(bus.id);
			project.busses.erase(std::remove_if(project.busses.begin(), project.busses.end(),
									 [id](const Bus& bus) { return bus.id == id || bus.parent == id; }),
				project.busses.end());
			keepMainMix(project);
			disconnect(project, gone, context);
			announceBusses(previous, project.busses, context);
			return Json::object();
		}

		// The names bus.rename gives: the name as given, then counting up its number or its capital letter.
		std::vector<std::string> nameSeries(const std::string& name, size_t count)
		{
			size_t digits = name.size();
			while (digits > 0 && std::isdigit(static_cast<unsigned char>(name[digits - 1])) != 0)
				--digits;
			std::vector<std::string> names;
			if (digits < name.size())
			{
				const size_t width = name.size() - digits;
				require(width <= 18, "the number at the end of name is too long");
				const uint64_t first = std::stoull(name.substr(digits));
				for (size_t i = 0; i < count; ++i)
				{
					std::string number = std::to_string(first + i);
					if (number.size() < width)
						number.insert(0, width - number.size(), '0');
					names.push_back(name.substr(0, digits) + number);
				}
				return names;
			}
			const char last = name.empty() ? '\0' : name.back();
			require(name.size() >= 2 && name[name.size() - 2] == ' ' && last >= 'A' && last <= 'Z',
				"name must end in a number, or in a space and a capital letter");
			require(static_cast<size_t>(last - 'A') + count <= 26, "letters only go up to Z");
			for (size_t i = 0; i < count; ++i)
				names.push_back(name.substr(0, name.size() - 1) + static_cast<char>(last + static_cast<int>(i)));
			return names;
		}

		Json renameBusses(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const auto ids = parameters.at("busses").get<std::vector<ObjectId>>();
			require(!ids.empty(), "busses must not be empty");
			std::vector<size_t> places; // in project.busses
			for (const ObjectId id : ids)
				places.push_back(static_cast<size_t>(&findBus(project, id) - project.busses.data()));
			// From the first one given down the list, then from the top.
			const size_t start = places.front();
			const size_t size = project.busses.size();
			const auto distance = [start, size](size_t place) { return (place + size - start) % size; };
			std::sort(
				places.begin(), places.end(), [&distance](size_t a, size_t b) { return distance(a) < distance(b); });
			places.erase(std::unique(places.begin(), places.end()), places.end());
			const std::vector<std::string> names = nameSeries(parameters.at("name").get<std::string>(), places.size());
			const std::vector<Bus> previous = project.busses;
			Json renamed = Json::array();
			for (size_t i = 0; i < places.size(); ++i)
			{
				project.busses[places[i]].name = names[i];
				renamed.push_back(toJson(project.busses[places[i]]));
			}
			announceBusses(previous, project.busses, context);
			return {{"busses", std::move(renamed)}};
		}

		Json remapPorts(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const BusDirection direction = busDirection(parameters.at("direction"));
			const Json& ports = parameters.at("ports");
			for (const auto& [from, to] : ports.items())
				require(to.is_string(), "ports." + from + " must be a string: the name of a port");
			const std::vector<Bus> previous = project.busses;
			Json changed = Json::array();
			for (Bus& bus : project.busses)
			{
				if (bus.direction != direction)
					continue;
				const std::vector<std::string> before = bus.ports;
				for (std::string& port : bus.ports)
					if (const auto to = ports.find(port); to != ports.end())
						port = to->get<std::string>();
				if (bus.ports != before)
					changed.push_back(toJson(bus));
			}
			announceBusses(previous, project.busses, context);
			return {{"busses", std::move(changed)}};
		}

		// The busses of a direction as a preset has them: top-level busses with their child busses.
		Json presetBusses(const Project& project, BusDirection direction)
		{
			Json busses = Json::array();
			for (const Bus& bus : project.busses)
			{
				if (bus.direction != direction || bus.parent != 0)
					continue;
				Json children = Json::array();
				for (const Bus& child : project.busses)
					if (child.parent == bus.id)
						children.push_back({{"name", child.name}, {"format", formatName(child.format)},
							{"parentChannels", child.parentChannels}});
				busses.push_back({{"name", bus.name}, {"format", formatName(bus.format)}, {"ports", bus.ports},
					{"children", std::move(children)}});
			}
			return busses;
		}

		// The user's own presets of a direction, by name.
		Json userPresets(BusDirection direction, CommandContext& context)
		{
			const Json presets = context.call("settings.get", {{"key", "audio.busPresets"}}).at("value");
			const Json mine = presets.value(directionName(direction), Json::object());
			return mine.is_object() ? mine : Json::object();
		}

		// Standard presets, those from the device's ports, then the user's own, which hide the others of their name.
		Json busPresets(BusDirection direction, CommandContext& context)
		{
			const Json user = userPresets(direction, context);
			const bool input = direction == BusDirection::Input;
			const std::string suffix = input ? " In" : " Out";
			Json presets = Json::array();
			const auto add = [&user, &presets](const std::string& name, const std::string& kind, Json busses)
			{
				if (kind == "user" || !user.contains(name))
					presets.push_back({{"name", name}, {"kind", kind}, {"busses", std::move(busses)}});
			};
			for (const ChannelFormat format :
				{ChannelFormat::Mono, ChannelFormat::Stereo, ChannelFormat::Surround51, ChannelFormat::Surround71})
				add(formatLabel(format), "standard",
					Json::array({{{"name", formatLabel(format) + suffix}, {"format", formatName(format)},
						{"ports", Json::array()}, {"children", Json::array()}}}));

			const Json device = context.call("audio.ports");
			std::vector<std::string> ports;
			for (const Json& port : device.at(input ? "inputs" : "outputs"))
				ports.push_back(port.at("name").get<std::string>());
			for (const size_t width : {size_t{1}, size_t{2}})
			{
				const size_t count = ports.size() / width;
				if (count == 0)
					continue;
				const ChannelFormat format = width == 1 ? ChannelFormat::Mono : ChannelFormat::Stereo;
				Json busses = Json::array();
				for (size_t i = 0; i < count; ++i)
					busses.push_back({{"name", formatLabel(format) + suffix + " " + std::to_string(i + 1)},
						{"format", formatName(format)},
						{"ports",
							std::vector<std::string>(ports.begin() + static_cast<std::ptrdiff_t>(i * width),
								ports.begin() + static_cast<std::ptrdiff_t>((i + 1) * width))},
						{"children", Json::array()}});
				add(std::to_string(count) + " " + formatLabel(format) + suffix + (count == 1 ? "" : "s"), "hardware",
					std::move(busses));
			}
			for (const auto& [name, busses] : user.items())
				add(name, "user", busses);
			return presets;
		}

		// Replaces the project's busses of a direction with a preset's, as busPresets lists them. Channels that used
		// the old busses move to the new bus at the same place among those of the direction, or to none (inputs) or
		// the main mix (outputs) if there is none. Returns the channels that changed.
		std::vector<ObjectId> applyPreset(Project& project, BusDirection direction, const Json& busses)
		{
			std::vector<Bus> created;
			try
			{
				const auto format = [](const Json& item)
				{ return formatFromName(item.at("format").get<std::string>()).value_or(ChannelFormat::None); };
				for (const Json& item : busses)
				{
					const ObjectId parent = project.newId();
					created.push_back({parent, item.at("name").get<std::string>(), direction, format(item),
						item.value("ports", std::vector<std::string>()), 0, {}, false});
					for (const Json& child : item.value("children", Json::array()))
						created.push_back({project.newId(), child.at("name").get<std::string>(), direction,
							format(child), {}, parent, parentChannelsFromJson(child.at("parentChannels")), false});
				}
			}
			catch (const Json::exception& error)
			{
				throw CommandError(std::string("the preset is invalid: ") + error.what());
			}

			std::vector<ObjectId> old; // the busses of the direction, in order
			for (const Bus& bus : project.busses)
				if (bus.direction == direction)
					old.push_back(bus.id);
			const auto first = std::find_if(project.busses.begin(), project.busses.end(),
				[direction](const Bus& bus) { return bus.direction == direction; });
			const auto at = first - project.busses.begin();
			project.busses.erase(std::remove_if(project.busses.begin(), project.busses.end(),
									 [direction](const Bus& bus) { return bus.direction == direction; }),
				project.busses.end());
			project.busses.insert(project.busses.begin() + at, created.begin(), created.end());
			for (const Bus& bus : created)
			{
				const std::string problem = busProblem(project, bus);
				require(problem.empty(), "the preset is invalid: " + problem);
			}
			keepMainMix(project);

			std::vector<ObjectId> changed;
			for (auto& [id, channel] : project.channels)
				for (ObjectId* end : {&channel.input, &channel.output})
				{
					const auto place = std::find(old.begin(), old.end(), *end);
					if (place == old.end())
						continue;
					const auto index = static_cast<size_t>(place - old.begin());
					const ObjectId fallback = end == &channel.output ? mainMix(project) : 0;
					*end = index < created.size() ? created[index].id : fallback;
					if (changed.empty() || changed.back() != id)
						changed.push_back(id);
				}
			return changed;
		}

		Json applyBusPreset(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const BusDirection direction = busDirection(parameters.at("direction"));
			const auto name = parameters.at("name").get<std::string>();
			const Json presets = busPresets(direction, context);
			const auto preset = std::find_if(
				presets.begin(), presets.end(), [&name](const Json& entry) { return entry.at("name") == name; });
			require(preset != presets.end(), std::string("no ") + directionName(direction) + " bus preset " + name);
			const std::vector<Bus> previous = project.busses;
			const std::vector<ObjectId> channels = applyPreset(project, direction, preset->at("busses"));
			announceBusses(previous, project.busses, context);
			for (const ObjectId id : channels)
				context.emit("channel.changed", {{"channel", toJson(project.channels.at(id))}});
			Json result = Json::array();
			for (const Bus& bus : project.busses)
				if (bus.direction == direction)
					result.push_back(toJson(bus));
			return {{"busses", std::move(result)}};
		}

		// mediaFolder is where media are copied to with copy, or empty if the project has no file yet.
		Json addMedia(Project& project, CommandContext& context, const std::filesystem::path& mediaFolder)
		{
			const Json& parameters = context.parameters();
			MediaFile media;
			media.path = parameters.at("path").get<std::string>();
			require(!media.path.empty(), "path must not be empty");
			const bool copy = parameters.value("copy", false);
			require(!copy || !mediaFolder.empty(),
				"save the project first: media are copied into the folder of its project file");
			const auto given = std::count_if(g_mediaProperties.begin(), g_mediaProperties.end(),
				[&parameters](const char* name) { return parameters.contains(name); });
			require(given == 0 || given == 3,
				"give sampleRate, channels and length together, or none of them to read them from the file");
			if (given == 3)
			{
				media.sampleRate = parameters.at("sampleRate").get<double>();
				media.channels = parameters.at("channels").get<int>();
				media.length = parameters.at("length").get<int64_t>();
			}
			else
			{
				try
				{
					const AudioFileInfo info = AudioFileReader::open(media.path)->info();
					media.sampleRate = info.sampleRate;
					media.channels = info.channels;
					media.length = info.frames;
				}
				catch (const AudioFileError& error)
				{
					throw CommandError(error.what());
				}
			}
			require(media.sampleRate > 0.0, "sampleRate must be positive");
			requireRange(media.channels, 1, g_maxAudioChannels, "channels");
			require(media.length >= 0, "length must not be negative");
			if (copy)
			{
				std::error_code error;
				require(std::filesystem::is_regular_file(std::filesystem::u8path(media.path), error),
					"can't copy " + media.path + ": there is no such file");
			}
			media.id = project.newId();
			project.media[media.id] = media;
			Json result = {{"media", toJson(media)}};
			context.emit("media.added", result);
			return result;
		}

		// The same file, by any path to it. Files that don't exist (yet) are compared by their absolute paths.
		bool sameFile(const std::string& first, const std::string& second)
		{
			const std::filesystem::path a = std::filesystem::u8path(first);
			const std::filesystem::path b = std::filesystem::u8path(second);
			std::error_code error;
			if (std::filesystem::equivalent(a, b, error))
				return true;
			std::error_code errorA;
			std::error_code errorB;
			const std::filesystem::path absoluteA = std::filesystem::absolute(a, errorA);
			const std::filesystem::path absoluteB = std::filesystem::absolute(b, errorB);
			return !errorA && !errorB && absoluteA.lexically_normal() == absoluteB.lexically_normal();
		}

		// A new file for media.convert and media.stretch: never one of the project's media files, the source among
		// them, which writing would destroy beyond what undo can bring back.
		void requireNewMediaPath(const Project& project, const std::string& path)
		{
			require(!path.empty(), "path must not be empty");
			for (const auto& [id, media] : project.media)
				require(!sameFile(path, media.path), "path is media file " + idText(id) + "; choose a new file");
		}

		Json relinkMedia(Project& project, CommandContext& context)
		{
			Json relinked = Json::array();
			Json skipped = Json::array();
			for (const Json& entry : context.parameters().at("files"))
			{
				const ObjectId id = entry.at("media").get<ObjectId>();
				const std::string path = entry.at("path").get<std::string>();
				require(!path.empty(), "path must not be empty");
				if (entry.contains("from"))
				{
					const auto found = project.media.find(id);
					if (found == project.media.end() || found->second.path != entry.at("from").get<std::string>())
					{
						skipped.push_back(id);
						continue;
					}
				}
				MediaFile& media = find(project.media, id, "media file");
				AudioFileInfo info;
				try
				{
					info = AudioFileReader::open(path)->info();
				}
				catch (const AudioFileError& error)
				{
					throw CommandError(error.what());
				}
				require(info.sampleRate == media.sampleRate && info.channels == media.channels,
					path + " has " + std::to_string(info.channels) + " channels at " +
						std::to_string(std::llround(info.sampleRate)) + " Hz, media file " + idText(id) + " " +
						std::to_string(media.channels) + " at " + std::to_string(std::llround(media.sampleRate)) +
						" Hz");
				media.path = path;
				context.emit("media.changed", {{"media", toJson(media)}});
				relinked.push_back(id);
			}
			return {{"relinked", std::move(relinked)}, {"skipped", std::move(skipped)}};
		}

		Json convertMedia(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const MediaFile& source = find(project.media, parameters.at("media").get<ObjectId>(), "media file");
			MediaFile media;
			media.path = parameters.at("path").get<std::string>();
			requireNewMediaPath(project, media.path);
			const double sampleRate = parameters.value("sampleRate", project.sampleRate);
			require(sampleRate > 0.0, "sampleRate must be positive");
			try
			{
				const AudioFileInfo info = convertSampleRate(source.path, {media.path}, sampleRate);
				media.sampleRate = info.sampleRate;
				media.channels = info.channels;
				media.length = info.frames;
			}
			catch (const AudioFileError& error)
			{
				throw CommandError(error.what());
			}
			media.id = project.newId();
			project.media[media.id] = media;
			Json result = {{"media", toJson(media)}};
			context.emit("media.added", result);
			return result;
		}

		Json stretchMedia(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const MediaFile& source = find(project.media, parameters.at("media").get<ObjectId>(), "media file");
			MediaFile media;
			media.path = parameters.at("path").get<std::string>();
			requireNewMediaPath(project, media.path);
			dsp::StretchSettings settings;
			settings.ratio = parameters.value("ratio", 1.0);
			settings.semitones = parameters.value("semitones", 0.0);
			settings.cents = parameters.value("cents", 0.0);
			settings.preserveFormants = parameters.value("formants", false);
			// The schema only lets known names through.
			settings.preset = dsp::stretchPresetFromString(parameters.value("preset", "quality"))
								  .value_or(dsp::StretchPreset::Quality);
			settings.mode =
				dsp::stretchModeFromString(parameters.value("mode", "offline")).value_or(dsp::StretchMode::Offline);
			require(settings.ratio >= dsp::g_minStretchRatio && settings.ratio <= dsp::g_maxStretchRatio,
				"ratio must be between 0.1 and 10");
			require(std::abs(settings.semitones + (settings.cents / 100.0)) <= dsp::g_maxPitchSemitones,
				"the pitch shift must be within 48 semitones either way");
			try
			{
				const AudioFileInfo info = dsp::stretchFile(source.path, media.path, settings);
				media.sampleRate = info.sampleRate;
				media.channels = info.channels;
				media.length = info.frames;
			}
			catch (const AudioFileError& error)
			{
				throw CommandError(error.what());
			}
			catch (const std::invalid_argument& error) // settings out of the real-time range
			{
				throw CommandError(error.what());
			}
			media.id = project.newId();
			project.media[media.id] = media;
			Json result = {{"media", toJson(media)}};
			context.emit("media.added", result);
			return result;
		}

		Json removeMedia(Project& project, CommandContext& context)
		{
			const ObjectId id = context.parameters().at("media").get<ObjectId>();
			find(project.media, id, "media file");
			for (const auto& [clipId, clip] : project.clips)
				require(clip.media != id, "media file " + idText(id) + " is used by clip " + idText(clipId));
			project.media.erase(id);
			context.emit("media.removed", {{"media", id}});
			return Json::object();
		}

		Json addClip(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			Clip clip;
			clip.media = parameters.at("media").get<ObjectId>();
			const MediaFile& media = find(project.media, clip.media, "media file");
			clip.name = parameters.value("name", std::filesystem::path(media.path).stem().string());
			clip.id = project.newId();
			project.clips[clip.id] = clip;
			Json result = {{"clip", toJson(clip)}};
			context.emit("clip.added", result);
			return result;
		}

		Json removeClip(Project& project, CommandContext& context)
		{
			const ObjectId id = context.parameters().at("clip").get<ObjectId>();
			find(project.clips, id, "clip");
			for (const auto& [eventId, event] : project.audioEvents)
				require(event.clip != id, "clip " + idText(id) + " is used by audio event " + idText(eventId));
			project.clips.erase(id);
			context.emit("clip.removed", {{"clip", id}});
			return Json::object();
		}

		// Checks an audio event against its track and clip; fills in the length if it is 0.
		void checkAudioEvent(Project& project, AudioEvent& event)
		{
			require(project.trackById.at(event.track).type == TrackType::Audio,
				"audio events can only be on an audio track");
			const MediaFile& media = project.media.at(find(project.clips, event.clip, "clip").media);
			require(event.offset >= 0, "offset must not be negative");
			require(event.offset < media.length, "offset is beyond the end of the clip");
			if (event.length == 0)
				event.length = media.length - event.offset;
			require(event.length > 0, "length must be positive");
			require(event.offset + event.length <= media.length,
				"the event must stay within its clip (" + std::to_string(media.length) + " sample frames)");
			require(event.volume >= g_minEventVolume && event.volume <= g_maxEventVolume,
				"volume must be from -144 to 6 dB");
		}

		Json addAudioEvent(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			AudioEvent event;
			event.track = parameters.at("track").get<ObjectId>();
			const Track& track = find(project.trackById, event.track, "track");
			event.position = position(track, parameters.at("position"), "position");
			event.clip = parameters.at("clip").get<ObjectId>();
			event.offset = parameters.value("offset", time::Samples{0});
			event.length = parameters.value("length", time::Samples{0});
			event.volume = parameters.value("volume", 0.0);
			event.mute = parameters.value("mute", false);
			require(!parameters.contains("length") || event.length > 0, "length must be positive");
			checkAudioEvent(project, event);
			event.id = project.newId();
			project.audioEvents[event.id] = event;
			Json result = {{"event", toJson(event)}};
			context.emit("audioEvent.added", result);
			return result;
		}

		Json setAudioEvent(Project& project, CommandContext& context, bool follow)
		{
			const Json& parameters = context.parameters();
			AudioEvent& event = find(project.audioEvents, parameters.at("event").get<ObjectId>(), "audio event");
			AudioEvent changed = event;
			if (parameters.contains("position"))
				changed.position = position(project.trackById.at(event.track), parameters.at("position"), "position");
			changed.clip = parameters.value("clip", event.clip);
			changed.offset = parameters.value("offset", event.offset);
			changed.length = parameters.value("length", event.length);
			changed.volume = parameters.value("volume", event.volume);
			changed.mute = parameters.value("mute", event.mute);
			require(changed.length > 0, "length must be positive");
			checkAudioEvent(project, changed);
			if (follow && changed.position != event.position)
				followEvent(project, event.track, event.position, eventEnd(project, event), changed.position, context);
			event = changed;
			Json result = {{"event", toJson(event)}};
			context.emit("audioEvent.changed", result);
			return result;
		}

		Json removeAudioEvent(Project& project, CommandContext& context)
		{
			const ObjectId id = context.parameters().at("event").get<ObjectId>();
			find(project.audioEvents, id, "audio event");
			project.audioEvents.erase(id);
			for (auto& [otherId, other] : project.audioEvents)
				if (other.crossfade.from == id)
				{
					other.crossfade = Crossfade();
					context.emit("audioEvent.changed", {{"event", toJson(other)}});
				}
			context.emit("audioEvent.removed", {{"event", id}});
			return Json::object();
		}

		Json addMidiPart(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			MidiPart part;
			part.track = parameters.at("track").get<ObjectId>();
			const Track& track = find(project.trackById, part.track, "track");
			require(track.type == TrackType::Midi || track.type == TrackType::Instrument,
				"MIDI parts can only be on a MIDI or instrument track");
			part.position = position(track, parameters.at("position"), "position");
			part.length = parameters.at("length").get<int64_t>();
			require(part.length > 0, "length must be positive");
			part.name = parameters.value("name", track.name);
			part.id = project.newId();
			Json result = {{"part", toJson(part)}};
			project.midiParts[part.id] = std::move(part);
			context.emit("midiPart.added", result);
			return result;
		}

		Json addAudioPart(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			AudioPart part;
			part.track = parameters.at("track").get<ObjectId>();
			const Track& track = find(project.trackById, part.track, "track");
			require(track.type == TrackType::Audio, "audio parts can only be on an audio track");
			part.position = position(track, parameters.at("position"), "position");
			part.length = parameters.at("length").get<int64_t>();
			require(part.length > 0, "length must be positive");
			part.name = parameters.value("name", track.name);
			part.id = project.newId();
			project.audioParts[part.id] = part;
			Json result = {{"part", toJson(part)}};
			context.emit("audioPart.added", result);
			return result;
		}

		Json setMidiPart(Project& project, CommandContext& context, bool follow)
		{
			const Json& parameters = context.parameters();
			MidiPart& part = find(project.midiParts, parameters.at("part").get<ObjectId>(), "MIDI part");
			const time::Position newPosition = parameters.contains("position")
				? position(project.trackById.at(part.track), parameters.at("position"), "position")
				: part.position;
			const int64_t length = parameters.value("length", part.length);
			require(length > 0, "length must be positive");
			if (follow && newPosition != part.position)
				followEvent(project, part.track, part.position, partEnd(part), newPosition, context);
			part.position = newPosition;
			part.length = length;
			part.name = parameters.value("name", part.name);
			for (const auto& [key, value] :
				{std::pair{"transpose", &part.transpose}, std::pair{"velocity", &part.velocity}})
				if (parameters.contains(key))
				{
					const auto number = parameters.at(key).get<int64_t>();
					requireRange(number, -127, 127, key);
					*value = static_cast<int>(number);
				}
			Json result = {{"part", toJson(part)}};
			context.emit("midiPart.changed", result);
			return result;
		}

		Json removeMidiPart(Project& project, CommandContext& context)
		{
			const ObjectId id = context.parameters().at("part").get<ObjectId>();
			find(project.midiParts, id, "MIDI part");
			project.midiParts.erase(id);
			context.emit("midiPart.removed", {{"part", id}});
			return Json::object();
		}

		double unitValue(const Json& item, const char* key, const std::string& name)
		{
			const double value = item.at(key).get<double>();
			require(value >= 0.0 && value <= 1.0, name + "." + key + " must be 0 to 1");
			return value;
		}

		Json addNotes(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			MidiPart& part = find(project.midiParts, parameters.at("part").get<ObjectId>(), "MIDI part");
			std::vector<Note> notes;
			const Json& items = parameters.at("notes");
			for (size_t i = 0; i < items.size(); ++i)
			{
				const Json& item = items[i];
				const std::string name = "notes[" + std::to_string(i) + "]";
				Note note;
				note.position = item.at("position").get<time::Ticks>();
				note.length = item.at("length").get<time::Ticks>();
				require(note.position >= 0, name + ".position must not be negative");
				require(note.length > 0, name + ".length must be positive");
				requireRange(item.at("pitch").get<int64_t>(), 0, 127, name + ".pitch");
				requireRange(item.value("channel", int64_t{0}), 0, 15, name + ".channel");
				note.pitch = item.at("pitch").get<uint8_t>();
				note.channel = item.value("channel", uint8_t{0});
				note.velocity = static_cast<uint16_t>(std::lround(unitValue(item, "velocity", name) * g_maxVelocity));
				if (item.contains("releaseVelocity"))
					note.releaseVelocity =
						static_cast<uint16_t>(std::lround(unitValue(item, "releaseVelocity", name) * g_maxVelocity));
				if (item.contains("probability"))
					note.probability = unitValue(item, "probability", name);
				note.velocityVariance = item.value("velocityVariance", 0.0);
				require(std::abs(note.velocityVariance) <= 1.0, name + ".velocityVariance must be -1 to 1");
				notes.push_back(note);
			}
			Json added = Json::array();
			for (Note& note : notes)
			{
				note.id = project.newId();
				part.notes[note.id] = note;
				added.push_back(toJson(note));
			}
			context.emit("note.added", {{"part", part.id}, {"notes", added}});
			return {{"notes", added}};
		}
	}

	Controller controllerFrom(const Json& item, const std::string& name)
	{
		const std::optional<ControllerType> parsed =
			controllerTypeFromName(item.value("type", std::string("controller")));
		if (!parsed.has_value())
			throw CommandError(
				name + ".type must be controller, pitchBend, channelPressure, polyPressure, program or sysEx");
		const ControllerType type = *parsed;
		const auto needs = [&](const char* key) { require(item.contains(key), name + "." + key + " is required"); };
		const bool numbered =
			type == ControllerType::Control || type == ControllerType::PolyPressure || type == ControllerType::Program;
		if (numbered)
		{
			needs("number");
			requireRange(item.at("number").get<int64_t>(), 0, 127, name + ".number");
		}
		if (type != ControllerType::Program && type != ControllerType::SysEx)
		{
			needs("value");
			unitValue(item, "value", name);
		}
		if (type == ControllerType::Program && item.contains("bank"))
			requireRange(item.at("bank").get<int64_t>(), 0, g_maxBank, name + ".bank");
		if (type == ControllerType::SysEx)
		{
			needs("data");
			const auto data = item.at("data").get<std::vector<int64_t>>();
			require(data.size() >= 2 && data.front() == 0xF0 && data.back() == 0xF7,
				name + ".data must start with 0xF0 and end with 0xF7");
			require(
				std::all_of(data.begin() + 1, data.end() - 1, [](int64_t byte) { return byte >= 0 && byte < 0x80; }),
				name + ".data bytes between 0xF0 and 0xF7 must be 0 to 127");
		}
		requireRange(item.value("channel", int64_t{0}), 0, 15, name + ".channel");
		Controller controller = controllerFromJson(item);
		controller.position = item.at("position").get<time::Ticks>();
		require(controller.position >= 0, name + ".position must not be negative");
		return controller;
	}

	namespace
	{
		Json addControllers(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			MidiPart& part = find(project.midiParts, parameters.at("part").get<ObjectId>(), "MIDI part");
			std::vector<Controller> controllers;
			const Json& items = parameters.at("controllers");
			controllers.reserve(items.size());
			for (size_t i = 0; i < items.size(); ++i)
			{
				controllers.push_back(controllerFrom(items[i], "controllers[" + std::to_string(i) + "]"));
			}
			Json added = Json::array();
			for (Controller& controller : controllers)
			{
				controller.id = project.newId();
				part.controllers[controller.id] = controller;
				added.push_back(toJson(controller));
			}
			context.emit("controller.added", {{"part", part.id}, {"controllers", added}});
			return {{"controllers", added}};
		}

		// Removes notes or controllers from a part; every id must be in it.
		template <typename Map> Json removeFromPart(
			Project& project, CommandContext& context, Map MidiPart::* member, const char* key, const char* what)
		{
			const Json& parameters = context.parameters();
			MidiPart& part = find(project.midiParts, parameters.at("part").get<ObjectId>(), "MIDI part");
			Map& objects = part.*member;
			const auto ids = parameters.at(key).get<std::vector<ObjectId>>();
			for (const ObjectId id : ids)
				require(objects.count(id) != 0,
					std::string("no ") + what + " " + idText(id) + " in MIDI part " + idText(part.id));
			for (const ObjectId id : ids)
				objects.erase(id);
			context.emit(std::string(what) + ".removed", {{"part", part.id}, {key, ids}});
			return Json::object();
		}

		// What project.changed announces.
		template <typename Settings> Json projectSettingsJson(const Settings& settings)
		{
			Json json = {{"name", settings.name}, {"sampleRate", settings.sampleRate},
				{"autoFades", toJson(settings.autoFades)}, {"panLaw", panLawName(settings.panLaw)},
				{"maxVolume", settings.maxVolume}, {"colors", toJson(settings.colors)}};
			json.update(toJson(settings.setup));
			return json;
		}

		// The ruler follows a new project time format (#78, reference manual p. 69), and its grid with it (#413): the
		// snap settings take that format, here rather than in the UI, so that snapping is the same with and without
		// one.
		void followTimeFormat(Project& project, const std::string& before, CommandContext& context)
		{
			if (project.setup.timeFormat == before)
				return;
			const std::optional<GridFormat> format = gridFormatFromName(project.setup.timeFormat);
			if (!format || *format == project.snap.format)
				return;
			project.snap.format = *format;
			context.emit("snap.changed", toJson(project.snap));
		}

		Json setProject(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const double sampleRate = parameters.value("sampleRate", project.sampleRate);
			require(sampleRate > 0.0, "sampleRate must be positive");
			const double maxVolume = parameters.value("maxVolume", project.maxVolume);
			require(maxVolume == 6.0 || maxVolume == 12.0, "maxVolume must be 6 or 12 dB");
			ProjectSetup setup;
			try
			{
				setup = projectSetupFromJson(parameters, project.setup);
			}
			catch (const std::invalid_argument& problem)
			{
				throw CommandError(problem.what());
			}
			// Fixed tempo or tempo track mode (reference manual p. 1446): the tempo of the other mode waits in
			// otherTempo.
			const time::TempoMap tempoBefore = project.tempo;
			const bool tempoModeBefore = project.tempoTrackActive;
			if (parameters.value("tempoTrackActive", project.tempoTrackActive) != project.tempoTrackActive)
			{
				std::swap(project.tempo, project.otherTempo);
				project.tempoTrackActive = !project.tempoTrackActive;
			}
			// The first tempo and signature, which the transport bar shows: the fixed tempo, or the tempo track's
			// first event; tempo.* and signature tracks edit the rest.
			std::vector<time::TempoPoint> tempo = project.tempo.points();
			tempo.front().bpm = parameters.value("tempo", tempo.front().bpm);
			require(tempo.front().bpm >= time::g_minBpm && tempo.front().bpm <= time::g_maxBpm,
				"tempo must be from 1 to 999 BPM");
			std::vector<time::SignatureChange> signatures = project.signatures.changes();
			if (const auto signature = parameters.find("signature"); signature != parameters.end())
			{
				const int numerator = signature->at("numerator").get<int>();
				const int denominator = signature->at("denominator").get<int>();
				require(numerator >= 1 && numerator <= 99, "the signature's numerator must be from 1 to 99");
				require(
					denominator == 1 || denominator == 2 || denominator == 4 || denominator == 8 || denominator == 16,
					"the signature's denominator must be 1, 2, 4, 8 or 16");
				signatures.front().signature = {numerator, denominator};
			}
			const time::TempoMap tempoMap(tempo);
			const time::SignatureMap signatureMap(signatures);
			project.tempo = tempoMap;
			project.signatures = signatureMap;
			if (project.tempo != tempoBefore || project.tempoTrackActive != tempoModeBefore)
				tempoChanged(project, tempoBefore, context);
			project.name = parameters.value("name", project.name);
			if (parameters.contains("panLaw"))
				project.panLaw = panLawFromName(parameters.at("panLaw").get<std::string>()).value_or(project.panLaw);
			project.maxVolume = maxVolume;
			if (parameters.contains("colors"))
			{
				require(!parameters.at("colors").empty(), "a color set has at least one color");
				for (const Json& item : parameters.at("colors"))
					require(isColor(item.at("color").get<std::string>()), "colors must be #rrggbb");
				project.colors = colorsFromJson(parameters.at("colors"));
			}
			const std::string timeFormat = project.setup.timeFormat;
			project.setup = std::move(setup);
			followTimeFormat(project, timeFormat, context);
			// Faders and send levels above a lower maximum come down to it.
			for (auto& [id, channel] : project.channels)
			{
				const Channel before = channel;
				channel.volume = std::min(channel.volume, maxVolume);
				for (Send& send : channel.sends)
					send.level = std::min(send.level, maxVolume);
				if (!(channel == before))
					context.emit("channel.changed", {{"channel", toJson(channel)}});
			}
			if (sampleRate != project.sampleRate)
				changeSampleRate(project, sampleRate, context);
			context.emit("project.changed", projectSettingsJson(project));
			return Json::object();
		}
	}

	void followAutomation(Project& project, ObjectId track, const time::Position& from, const time::Position& end,
		const time::Position& to, CommandContext& context)
	{
		followEvent(project, track, from, end, to, context);
	}

	namespace
	{
		// The undo step each command records, as shown in the history.
		struct ActionName
		{
			const char* command;
			const char* action;
		};

		constexpr ActionName g_actions[] = {
			{"project.set", "Change project"},
			{"bus.add", "Add bus"},
			{"bus.set", "Change bus"},
			{"bus.remove", "Remove bus"},
			{"bus.rename", "Rename busses"},
			{"bus.remapPorts", "Remap ports"},
			{"bus.applyPreset", "Apply bus preset"},
			{"track.add", "Add track"},
			{"track.set", "Change track"},
			{"track.move", "Move track"},
			{"track.remove", "Remove track"},
			{"track.removeEmpty", "Remove empty tracks"},
			{"track.duplicate", "Duplicate track"},
			{"channel.set", "Change channel"},
			{"channel.deactivateAll", "Deactivate all states"},
			{"parameter.set", "Change parameter"},
			{"plugin.insert", "Insert plugin"},
			{"plugin.remove", "Remove plugin"},
			{"plugin.move", "Move plugin"},
			{"plugin.set", "Change plugin"},
			{"media.add", "Add media file"},
			{"media.convert", "Convert media file"},
			{"media.stretch", "Stretch media file"},
			{"media.remove", "Remove media file"},
			{"media.relink", "Relink media files"},
			{"clip.add", "Add clip"},
			{"clip.remove", "Remove clip"},
			{"audioEvent.add", "Add audio event"},
			{"audioEvent.set", "Change audio event"},
			{"audioEvent.remove", "Remove audio event"},
			{"audioEvent.setFades", "Change fades"},
			{"clip.setFades", "Change clip fades"},
			{"crossfade.set", "Crossfade"},
			{"crossfade.remove", "Remove crossfades"},
			{"autoFades.set", "Change auto fades"},
			{"audioPart.add", "Add audio part"},
			{"midiPart.add", "Add MIDI part"},
			{"midiPart.set", "Change MIDI part"},
			{"midiPart.remove", "Remove MIDI part"},
			{"note.add", "Add notes"},
			{"note.remove", "Remove notes"},
			{"note.set", "Change notes"},
			{"note.voicing", "Change chord"},
			{"controller.add", "Add controllers"},
			{"controller.remove", "Remove controllers"},
			{"controller.set", "Change controllers"},
			{"marker.add", "Add marker"},
			{"marker.put", "Set marker"},
			{"marker.set", "Change marker"},
			{"marker.remove", "Remove markers"},
			{"marker.renumber", "Renumber markers"},
			{"marker.import", "Import markers"},
			{"tempo.add", "Add tempo event"},
			{"tempo.set", "Change tempo events"},
			{"tempo.remove", "Remove tempo events"},
			{"tempo.tap", "Tap tempo"},
			{"tempo.import", "Import tempo track"},
			{"signature.add", "Add time signature event"},
			{"signature.set", "Change time signature events"},
			{"signature.remove", "Remove time signature events"},
			{"marker.activateTrack", "Activate marker track"},
			{"automationLane.add", "Add automation lane"},
			{"automationLane.set", "Change automation lane"},
			{"automationLane.remove", "Remove automation lane"},
			{"automationPoint.add", "Add automation points"},
			{"automationPoint.set", "Change automation points"},
			{"automationPoint.remove", "Remove automation points"},
			{"midiFile.import", "Import MIDI file"},
			{"audioFile.import", "Import audio files"},
			{"recording.add", "Record"},
			{"clip.set", "Change clip"},
			{"poolFolder.add", "Add pool folder"},
			{"poolFolder.set", "Change pool folder"},
			{"poolFolder.remove", "Remove pool folder"},
			{"pool.removeUnused", "Remove unused media"},
			{"media.locate", "Locate missing files"},
			{"media.removeMissing", "Remove missing files"},
			{"media.conform", "Conform files"},
		};

		// Empty for commands without a fixed name, such as transactions.
		std::string actionName(const std::string& command)
		{
			for (const ActionName& entry : g_actions)
				if (command == entry.command)
					return entry.action;
			return {};
		}

		// The current time in UTC, e.g. 2026-10-27T21:05:00Z.
		std::string now()
		{
			const std::time_t seconds = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
			std::tm utc{};
#ifdef _WIN32
			gmtime_s(&utc, &seconds);
#else
			gmtime_r(&seconds, &utc);
#endif
			char text[32];
			std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &utc);
			return text;
		}

		// Announces undone or redone objects with the same events as the commands that change them; a track and
		// its channel come as separate track and channel events.
		template <typename T> void announceObjects(const std::vector<ObjectChange<T>>& changes, Direction direction,
			const std::string& kind, const char* key, CommandContext& context)
		{
			for (const ObjectChange<T>& change : changes)
			{
				const std::optional<T>& current = direction == Direction::Redo ? change.after : change.before;
				const std::optional<T>& previous = direction == Direction::Redo ? change.before : change.after;
				if (!current)
					context.emit(kind + ".removed", {{key, change.id}});
				else
					context.emit(kind + (previous ? ".changed" : ".added"), {{key, toJson(*current)}});
			}
		}

		// Called after the change was applied to project in the given direction.
		void announce(const ProjectModel& model, const Project& project, const ProjectChange& change,
			Direction direction, CommandContext& context)
		{
			bool reordered = false;
			bool activated = false;
			if (change.settingsBefore && change.settingsAfter)
			{
				const bool redo = direction == Direction::Redo;
				const ProjectSettings& current = redo ? *change.settingsAfter : *change.settingsBefore;
				const ProjectSettings& previous = redo ? *change.settingsBefore : *change.settingsAfter;
				if (current.name != previous.name || current.sampleRate != previous.sampleRate ||
					current.tempo != previous.tempo || current.signatures != previous.signatures ||
					!(current.autoFades == previous.autoFades) || current.panLaw != previous.panLaw ||
					current.maxVolume != previous.maxVolume || !(current.colors == previous.colors) ||
					!(current.setup == previous.setup))
					context.emit("project.changed", projectSettingsJson(current));
				announceBusses(previous.busses, current.busses, context);
				reordered = current.tracks != previous.tracks;
				activated = current.activeMarkerTrack != previous.activeMarkerTrack;
			}
			for (const ObjectChange<Track>& track : change.tracks)
				reordered = reordered || !track.before || !track.after || track.before->parent != track.after->parent ||
					track.before->children != track.after->children;
			announceObjects(change.tracks, direction, "track", "track", context);
			announceObjects(change.channels, direction, "channel", "channel", context);
			announceObjects(change.media, direction, "media", "media", context);
			announceObjects(change.poolFolders, direction, "poolFolder", "folder", context);
			announceObjects(change.clips, direction, "clip", "clip", context);
			announceObjects(change.audioParts, direction, "audioPart", "part", context);
			announceObjects(change.audioEvents, direction, "audioEvent", "event", context);
			announceObjects(change.markers, direction, "marker", "marker", context);
			announceObjects(change.linkGroups, direction, "linkGroup", "group", context);
			// Slots as plugin.insert, plugin.remove and the other plugin.* commands announce them. Those of a channel
			// that came or went are in its events.
			std::set<ObjectId> channels; // that came or went
			for (const ObjectChange<Channel>& channel : change.channels)
				if (!channel.before || !channel.after)
					channels.insert(channel.id);
			for (const ObjectChange<Slot>& slot : change.slots)
			{
				const std::optional<Slot>& current = direction == Direction::Redo ? slot.after : slot.before;
				const std::optional<Slot>& previous = direction == Direction::Redo ? slot.before : slot.after;
				if (channels.count((current ? current : previous)->channel) != 0)
					continue;
				if (!current)
					context.emit("plugin.removed", {{"slot", slot.id}});
				else
					context.emit(
						previous ? "plugin.changed" : "plugin.added", {{"slot", model.describeSlot(project, slot.id)}});
			}
			// After the tracks, so that a client knows the track that becomes active.
			if (activated)
				context.emit("marker.trackActivated", {{"track", project.activeMarkerTrack}});
			for (const MidiPartChange& part : change.midiParts)
			{
				const auto current = project.midiParts.find(part.id);
				const bool existed = direction == Direction::Redo ? part.before.has_value() : part.after.has_value();
				if (current == project.midiParts.end())
					context.emit("midiPart.removed", {{"part", part.id}});
				else
					context.emit(existed ? "midiPart.changed" : "midiPart.added", {{"part", toJson(current->second)}});
			}
			for (const AutomationLaneChange& lane : change.automation)
			{
				const auto current = project.automation.find(lane.id);
				const bool existed = direction == Direction::Redo ? lane.before.has_value() : lane.after.has_value();
				if (current == project.automation.end())
					context.emit("automationLane.removed", {{"lane", lane.id}});
				else
					context.emit(existed ? "automationLane.changed" : "automationLane.added",
						{{"lane", toJson(current->second)}});
			}
			if (reordered)
				context.emit("track.reordered", {{"tracks", project.trackOrder()}});
		}
	}

	ProjectModel::ProjectModel() : m_project(newProject()), m_snapshots(compile(m_project, 0))
	{
	}

	void ProjectModel::add(Engine& engine, CommandInfo info, Handler handler)
	{
		std::string action = actionName(info.name);
		addEdit(engine, std::move(info), std::move(action), std::move(handler));
	}

	void ProjectModel::addEdit(Engine& engine, CommandInfo info, std::string action, Handler handler)
	{
		if (info.query)
		{
			engine.registerCommand(std::move(info),
				[this, handler = std::move(handler)](CommandContext& context) { return handler(m_project, context); });
			return;
		}
		if (!action.empty())
			m_edits[info.name] = {info, handler};
		engine.registerCommand(std::move(info),
			[this, action = std::move(action), handler = std::move(handler)](CommandContext& context)
			{
				Project before = m_project;
				Json result;
				try
				{
					result = handler(m_project, context);
					syncSharedParts(before.midiParts, m_project, context);
				}
				catch (...)
				{
					m_project = std::move(before);
					throw;
				}
				if (action.empty()) // a transaction
					record(context.parameters().at("description").get<std::string>(), before, context,
						context.parameters().value("merge", std::string()));
				else
					record(action, before, context);
				publish();
				return result;
			});
	}

	Json ProjectModel::runEdit(
		const std::string& command, Project& project, const Json& parameters, CommandContext& context)
	{
		CommandContext inner(parameters, &context);
		return m_edits.at(command).handler(project, inner);
	}

	void ProjectModel::addControl(
		Engine& engine, CommandInfo info, std::function<Json(CommandContext& context)> handler)
	{
		engine.registerCommand(std::move(info), std::move(handler));
	}

	void ProjectModel::record(std::string action, const Project& before, CommandContext& context, std::string merge)
	{
		const std::vector<HistoryStep>& steps = m_history.steps();
		if (!merge.empty() && !steps.empty() && m_history.position() == steps.size() && steps.back().merge == merge)
		{
			// The step joins the last one: what changed since before that one.
			Project start = before;
			apply(start, steps.back().change, Direction::Undo);
			ProjectChange change = difference(start, m_project);
			if (change.empty())
				m_history.removeLast();
			else
				m_history.step(steps.size() - 1).change = std::move(change);
			announceHistory(context);
			return;
		}
		ProjectChange change = difference(before, m_project);
		if (change.empty())
			return;
		m_history.record({std::move(action), now(), {}, std::move(change), std::move(merge)});
		announceHistory(context);
	}

	void ProjectModel::announceHistory(CommandContext& context) const
	{
		const std::vector<HistoryStep>& steps = m_history.steps();
		const size_t position = m_history.position();
		context.emit("history.changed",
			{{"position", position}, {"steps", steps.size()},
				{"undo", position > 0 ? Json(steps[position - 1].action) : Json(nullptr)},
				{"redo", position < steps.size() ? Json(steps[position].action) : Json(nullptr)}});
	}

	void ProjectModel::publish()
	{
		m_snapshots.publish(compile(m_project, ++m_revision));
	}

	Json ProjectModel::undoOrRedo(CommandContext& context, Direction direction)
	{
		const bool undo = direction == Direction::Undo;
		const auto steps = context.parameters().value("steps", int64_t{1});
		require(steps >= 1, "steps must be at least 1");
		const size_t position = m_history.position();
		const size_t available = undo ? position : m_history.steps().size() - position;
		require(available > 0, undo ? "nothing to undo" : "nothing to redo");
		require(static_cast<size_t>(steps) <= available,
			"only " + std::to_string(available) + (undo ? " steps can be undone" : " steps can be redone"));
		const std::string timeFormat = m_project.setup.timeFormat;
		for (int64_t i = 0; i < steps; ++i)
		{
			const HistoryStep& step = undo ? m_history.undo() : m_history.redo();
			apply(m_project, step.change, direction);
			announce(*this, m_project, step.change, direction, context);
		}
		followTimeFormat(m_project, timeFormat, context);
		announceHistory(context);
		publish();
		return {{"position", m_history.position()}};
	}

	Json ProjectModel::transaction(Project& project, CommandContext& context)
	{
		Json results = Json::array();
		const Json& commands = context.parameters().at("commands");
		for (size_t i = 0; i < commands.size(); ++i)
		{
			const std::string name = commands[i].at("command").get<std::string>();
			std::string where = "commands[";
			where += std::to_string(i);
			where += ']';
			const auto edit = m_edits.find(name);
			if (edit == m_edits.end())
			{
				std::string message = where;
				message += ": ";
				message += name;
				message += " can't be part of a transaction";
				throw CommandError(message);
			}
			const Json parameters = commands[i].value("parameters", Json::object());
			std::string prefix = where;
			prefix += " (";
			prefix += name;
			prefix += "): ";
			if (const std::string problem = validate(parameters, edit->second.info.parameters, where + ".parameters");
				!problem.empty())
				throw CommandError(prefix + problem);
			CommandContext inner(parameters, &context);
			try
			{
				// Each command's edit reaches the other shared copies before the next command runs, so that edits of
				// several copies of a group all stay and a command sees what an earlier one did to another copy. Only
				// shared parts are compared, so only they are copied, not every note of every part (#426).
				const std::map<ObjectId, MidiPart> parts = sharedPartsOf(project.midiParts);
				results.push_back(edit->second.handler(project, inner));
				syncSharedParts(parts, project, inner);
			}
			catch (const std::exception& error)
			{
				throw CommandError(prefix + error.what());
			}
		}
		return {{"results", results}};
	}

	void ProjectModel::registerHistoryCommands(Engine& engine)
	{
		const Json stepsSchema = {
			{"type", "object"}, {"properties", {{"steps", {{"type", "integer"}}}}}, {"additionalProperties", false}};
		const Json positionResult = {
			{"type", "object"}, {"properties", {{"position", {{"type", "integer"}}}}}, {"required", {"position"}}};

		addControl(engine,
			{"history.get", 1,
				"The undo history: every step with its action, time (UTC) and details, the position (steps before it "
				"are done, the ones after it can be redone) and the maximum number of steps (0: no limit).",
				objectSchema({}), {{"type", "object"}}, true},
			[this](CommandContext&)
			{
				Json steps = Json::array();
				for (const HistoryStep& step : m_history.steps())
					steps.push_back({{"action", step.action}, {"time", step.time}, {"details", step.details}});
				return Json{{"position", m_history.position()}, {"maxSteps", m_history.maxSteps()}, {"steps", steps}};
			});
		addControl(engine,
			{"history.undo", 1, "Undoes the last step, or the given number of steps.", stepsSchema, positionResult,
				false},
			[this](CommandContext& context) { return undoOrRedo(context, Direction::Undo); });
		addControl(engine,
			{"history.redo", 1, "Redoes the next undone step, or the given number of steps.", stepsSchema,
				positionResult, false},
			[this](CommandContext& context) { return undoOrRedo(context, Direction::Redo); });
		addControl(engine,
			{"history.set", 1,
				"Sets the maximum number of undo steps (0: no limit). The oldest steps beyond it are dropped. A "
				"setting, so it stays when a new project starts.",
				objectSchema({{"maxSteps", {{"type", "integer"}}}}, {"maxSteps"}), Json::object(), false},
			[this](CommandContext& context)
			{
				const auto maxSteps = context.parameters().at("maxSteps").get<int64_t>();
				require(maxSteps >= 0, "maxSteps must not be negative");
				m_history.setMaxSteps(static_cast<size_t>(maxSteps));
				announceHistory(context);
				return Json::object();
			});
		addControl(engine,
			{"history.describe", 1, "Sets the details of a history step, a note for the user.",
				objectSchema({{"step", {{"type", "integer"}}}, {"details", {{"type", "string"}}}}, {"step", "details"}),
				Json::object(), false},
			[this](CommandContext& context)
			{
				const auto step = context.parameters().at("step").get<int64_t>();
				requireRange(step, 0, static_cast<int64_t>(m_history.steps().size()) - 1, "step");
				m_history.step(static_cast<size_t>(step)).details =
					context.parameters().at("details").get<std::string>();
				announceHistory(context);
				return Json::object();
			});

		const Json command =
			objectSchema({{"command", {{"type", "string"}}}, {"parameters", {{"type", "object"}}}}, {"command"});
		add(engine,
			{"history.transaction", 1,
				"Runs document commands in order as one undo step named description. If one fails, none of them "
				"changes the project. Returns each command's result. With a merge key, a transaction joins the last "
				"step instead when that step is a transaction with the same key and nothing was undone since: a "
				"gesture such as a fader drag, sent as a transaction per step with a key of its own, is one undo step "
				"(none if it ends where it started).",
				objectSchema({{"description", {{"type", "string"}}}, {"commands", arraySchema(command)},
								 {"merge", {{"type", "string"}}}},
					{"description", "commands"}),
				{{"type", "object"}, {"properties", {{"results", {{"type", "array"}}}}}, {"required", {"results"}}},
				false},
			[this](Project& project, CommandContext& context) { return transaction(project, context); });
	}

	void ProjectModel::registerCommands(Engine& engine, Jobs& jobs)
	{
		const Json idSchema = {{"type", "integer"}};
		const Json integerSchema = {{"type", "integer"}};
		const Json numberSchema = {{"type", "number"}};
		const Json stringSchema = {{"type", "string"}};
		const Json booleanSchema = {{"type", "boolean"}};
		const Json noResult = Json::object();
		const Json trackTypes = enumSchema({"audio", "instrument", "midi", "folder", "group", "effect", "marker",
			"ruler", "tempo", "signature", "vca"});
		const Json timeBases = enumSchema({"musical", "linear"});
		const Json timeFormats =
			enumSchema(std::vector<std::string>(g_rulerTimeFormats.begin(), g_rulerTimeFormats.end()));
		const Json formats = enumSchema(formatNames());
		const Json color = {{"type", {"string", "null"}}};

		add(engine,
			{"project.get", 1, "The whole project: busses, tracks in order, channels, media, clips, events and parts.",
				objectSchema({}), {{"type", "object"}}, true},
			[](Project& project, CommandContext&) { return toJson(project); });
		add(engine,
			{"graph.get", 1,
				"The project's audio processing graph: its nodes (busses, then channels in track order, then for each "
				"channel with plugin slots a channelSource node with what plays on the channel and a pluginSlot node "
				"per slot, which pass the audio on to the channel's node, and while channels listen a listenBus node "
				"that receives them) with the nodes they receive audio and events from, and the order they are "
				"processed in. object is the bus, channel or slot id.",
				objectSchema({}), {{"type", "object"}}, true},
			[this](Project&, CommandContext&) { return graphToJson(graph::describe(m_snapshots.latest())); });
		addControl(engine,
			{"project.save", 1,
				"Saves the project as a DAWproject file: as path, which is then the project's file (save as), or "
				"without path to the project's file (save). Media files are copied into it unless embedMedia is "
				"false; then the file refers to them where they are. The file becomes the first of the recent "
				"projects (project.recent).",
				objectSchema({{"path", stringSchema}, {"embedMedia", booleanSchema}}), noResult, false},
			[this](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				DawProjectSaveOptions options;
				options.embedMedia = parameters.value("embedMedia", true);
				require(
					parameters.contains("path") || !m_file.empty(), "the project has no file yet: save it with a path");
				const std::string path = parameters.value("path", m_file);
				try
				{
					saveDawProject(m_project, path, options);
				}
				catch (const std::exception& error)
				{
					throw CommandError(error.what());
				}
				saved(std::filesystem::absolute(std::filesystem::u8path(path)).u8string(), context);
				return Json::object();
			});
		addControl(engine,
			{"project.load", 1,
				"Replaces the project with a DAWproject file and clears the undo history. Media files embedded in "
				"it are extracted to mediaFolder, by default a folder named after the file next to it. With "
				"revision, it loads only if the project is still at that revision (see project.revision), and fails "
				"otherwise, so that no change made meanwhile is lost. An auto save (see project.autoSave) opens "
				"without a file and with unsaved changes, and isn't a recent project.",
				objectSchema(
					{{"path", stringSchema}, {"mediaFolder", stringSchema}, {"revision", integerSchema}}, {"path"}),
				noResult, false},
			[this](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				if (parameters.contains("revision"))
					require(parameters.at("revision").get<uint64_t>() == m_revision,
						"the project changed after revision " +
							std::to_string(parameters.at("revision").get<uint64_t>()));
				const std::string path = parameters.at("path").get<std::string>();
				const std::filesystem::path file = std::filesystem::u8path(path);
				const std::string folder = parameters.value("mediaFolder",
					(file.parent_path() / std::filesystem::u8path(file.stem().u8string() + " media")).u8string());
				Project project;
				try
				{
					project = loadDawProject(path, folder);
				}
				catch (const std::exception& error)
				{
					throw CommandError(error.what());
				}
				opened(std::move(project), std::filesystem::absolute(file).u8string(), context);
				return Json::object();
			});
		addControl(engine,
			{"project.new", 1,
				"Replaces the project with an empty one that has a stereo input and output bus, or the busses of the "
				"user's bus presets named Default (see bus.savePreset), or with template, a copy of the project "
				"template of that name (see project.templates), and clears the undo history. The new project has no "
				"file until it is saved.",
				objectSchema({{"template", stringSchema}}), noResult, false},
			[this](CommandContext& context)
			{
				if (context.parameters().contains("template"))
				{
					opened(loadTemplate(context.parameters().at("template").get<std::string>()), {}, context);
					return Json::object();
				}
				Project project = newProject();
				for (const BusDirection direction : {BusDirection::Input, BusDirection::Output})
					if (const Json presets = userPresets(direction, context); presets.contains("Default"))
					{
						// A preset that doesn't work, e.g. edited by hand, leaves the standard busses.
						Project withPreset = project;
						try
						{
							applyPreset(withPreset, direction, presets.at("Default"));
							project = std::move(withPreset);
						}
						catch (const CommandError& error)
						{
							logging::warning(
								"The {} bus preset Default isn't applied: {}", directionName(direction), error.what());
						}
					}
				opened(std::move(project), {}, context);
				return Json::object();
			});
		add(engine,
			{"project.set", 1,
				"Changes the project's name, sample rate, stereo pan law or maximum volume, or the other settings of "
				"the project setup. Linear positions keep their time at a new sample rate; media files keep theirs "
				"(see media.conform). panLaw is how much a mono channel panned to the center is turned down on "
				"each side: 0dB, -3dB (the default), -4.5dB, -6dB or equalPower (-3.01 dB, the power stays the same "
				"wherever it is panned); the side panned toward is always at 0 dB. maxVolume is the top of the "
				"faders, 6 (the default) or 12 dB; faders above a lower maximum come down to it. tempo (1 to 999 BPM) "
				"and signature {numerator 1 to 99, denominator 1, 2, 4, 8 or 16} are the project's first tempo and "
				"time signature, as the transport bar sets them: the fixed tempo, or in tempo track mode the tempo "
				"track's first event; later changes stay, and musical positions keep their ticks. tempoTrackActive "
				"switches between fixed tempo mode (false, a new project's) and tempo track mode, in which the "
				"project plays the tempo track's events (tempo.add); each mode keeps its tempo while the other is in "
				"use (emits tempo.changed). colors is the project's color set, which track and event colors are picked "
				"from: at least "
				"one {name, color (#rrggbb)}, in order. recordFileType (wav, the default, broadcastWav, aiff or "
				"flac) and recordBitDepth (int16, int24, the default, int32, float32 or float64) are what recording "
				"writes: AIFF only integers, FLAC 16 or 24 bit, 64 bit float only Wave. frameRate is the timecode's "
				"(default 25). startTime (seconds, under 24 hours) is the time shown at the project start, length "
				"(seconds, up to 24 hours, default 600) how long the project is, barOffset (-9999 to 9999) is added "
				"to the bar numbers shown, and timeFormat is the primary time format of rulers and position "
				"displays. A new timeFormat, also by undo or redo, is the snap settings' format too (see snap.set; "
				"emits snap.changed).",
				objectSchema({{"name", stringSchema}, {"sampleRate", numberSchema},
					{"panLaw", enumSchema(panLawNames())}, {"maxVolume", numberSchema}, {"tempo", numberSchema},
					{"tempoTrackActive", booleanSchema},
					{"colors",
						{{"type", "array"},
							{"items",
								objectSchema({{"name", stringSchema}, {"color", stringSchema}}, {"name", "color"})}}},
					{"signature",
						objectSchema({{"numerator", {{"type", "integer"}}}, {"denominator", {{"type", "integer"}}}},
							{"numerator", "denominator"})},
					{"recordFileType",
						enumSchema(std::vector<std::string>(g_recordFileTypes.begin(), g_recordFileTypes.end()))},
					{"recordBitDepth",
						enumSchema(std::vector<std::string>(g_recordBitDepths.begin(), g_recordBitDepths.end()))},
					{"frameRate", enumSchema(time::frameRateNames())}, {"startTime", numberSchema},
					{"length", numberSchema}, {"barOffset", integerSchema}, {"timeFormat", timeFormats}}),
				noResult, false},
			setProject);
		add(engine,
			{"project.factoryColors", 1,
				"The factory color set, which new projects have: each color's name and color (#rrggbb). The project "
				"colors setup dialog resets colors to it.",
				objectSchema(Json::object()),
				{{"type", "object"}, {"properties", {{"colors", {{"type", "array"}}}}}, {"required", {"colors"}}},
				true},
			[](Project&, CommandContext&) { return Json{{"colors", toJson(factoryColors())}}; });

		const Json directions = enumSchema({"input", "output"});
		const Json portNames = arraySchema(stringSchema);
		const Json presetName =
			objectSchema({{"direction", directions}, {"name", stringSchema}}, {"direction", "name"});
		add(engine,
			{"bus.add", 1,
				"Adds an input or output bus. format is its speaker arrangement (default stereo): mono, stereo, or a "
				"surround format with a fixed speaker order, e.g. 5.1 as L R C LFE Ls Rs. channels (1 to 8) picks "
				"mono, stereo, LRC, quadro, 5.0, 5.1, 7.0 or 7.1 instead. Without a name it is named after its format "
				"and direction, e.g. \"Stereo In 2\". ports: the device port of each channel by name (see "
				"audio.ports), an empty name for none; without ports the bus takes consecutive device ports in bus "
				"order. A port can serve several busses. The first output bus becomes the main mix. A child bus "
				"routes to some channels of its parent, a top-level bus of the same direction with more channels "
				"(direction may be left out): parentChannels are the parent's channel indices, one per channel of the "
				"child, by default the speakers both formats have; it uses its parent's ports.",
				objectSchema({{"direction", directions}, {"name", stringSchema}, {"format", formats},
					{"channels", integerSchema}, {"ports", portNames}, {"parent", idSchema},
					{"parentChannels", arraySchema(integerSchema)}}),
				resultSchema({"bus"}), false},
			addBus);
		add(engine,
			{"bus.set", 1,
				"Renames a bus, changes its ports (as in bus.add; [] for consecutive ports in bus order) or makes it "
				"the main mix (mainMix: true), the output bus new channels are routed to and that is monitored.",
				objectSchema(
					{{"bus", idSchema}, {"name", stringSchema}, {"ports", portNames}, {"mainMix", booleanSchema}},
					{"bus"}),
				resultSchema({"bus"}), false},
			setBus);
		add(engine,
			{"bus.remove", 1,
				"Removes a bus and its child busses; channels connected to them are disconnected. Removing the main "
				"mix "
				"makes the next output bus the main mix.",
				objectSchema({{"bus", idSchema}}, {"bus"}), noResult, false},
			removeBus);
		add(engine,
			{"bus.rename", 1,
				"Renames several busses with an incrementing number or letter: name ends in a number (\"Out 1\", "
				"\"Out 01\") or in a space and a capital letter (\"Bus A\", up to Z). The first bus given gets the "
				"name "
				"as given, the others follow in bus order from there, continuing from the top.",
				objectSchema({{"busses", arraySchema(idSchema)}, {"name", stringSchema}}, {"busses", "name"}),
				{{"type", "object"}, {"properties", {{"busses", {{"type", "array"}}}}}, {"required", {"busses"}}},
				false},
			renameBusses);
		add(engine,
			{"bus.remapPorts", 1,
				"Re-routes ports, e.g. missing ones (see audio.ports) after opening a project made on other hardware: "
				"ports maps port names to the names that replace them in every bus of the direction. Returns the "
				"busses that changed.",
				objectSchema({{"direction", directions},
								 {"ports", {{"type", "object"}, {"additionalProperties", stringSchema}}}},
					{"direction", "ports"}),
				{{"type", "object"}, {"properties", {{"busses", {{"type", "array"}}}}}, {"required", {"busses"}}},
				false},
			remapPorts);
		add(engine,
			{"bus.presets", 1,
				"The bus presets of a direction: standard ones with one bus of a common format, hardware ones made "
				"from the audio device's ports (all mono, or stereo pairs), and the user's own (bus.savePreset), "
				"which hide others of their name. Each has a name, a kind and its busses: name, format, ports and "
				"children (name, format, parentChannels).",
				objectSchema({{"direction", directions}}, {"direction"}),
				{{"type", "object"}, {"properties", {{"presets", {{"type", "array"}}}}}, {"required", {"presets"}}},
				true},
			[](Project&, CommandContext& context)
			{ return Json{{"presets", busPresets(busDirection(context.parameters().at("direction")), context)}}; });
		add(engine,
			{"bus.applyPreset", 1,
				"Replaces the project's busses of a direction with a preset's (see bus.presets). Channels that used "
				"the old busses move to the new bus at the same place, or to none (inputs) or the main mix "
				"(outputs). Returns the new busses.",
				presetName,
				{{"type", "object"}, {"properties", {{"busses", {{"type", "array"}}}}}, {"required", {"busses"}}},
				false},
			applyBusPreset);
		addControl(engine,
			{"bus.savePreset", 1,
				"Saves the project's busses of a direction as a user preset, for every project (the user setting "
				"audio.busPresets), replacing one of the same name. A preset named Default is applied to new projects.",
				presetName, noResult, false},
			[this, &engine](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const auto name = parameters.at("name").get<std::string>();
				require(!name.empty(), "name must not be empty");
				const BusDirection direction = busDirection(parameters.at("direction"));
				const Json busses = presetBusses(m_project, direction);
				// From the presets in the settings file, so that those another GNUbase process saved stay.
				engine.modifySetting(context, "audio.busPresets",
					[direction, &name, &busses](const Json& value)
					{
						Json presets = value;
						Json& mine = presets[directionName(direction)];
						if (!mine.is_object())
							mine = Json::object();
						mine[name] = busses;
						return presets;
					});
				return Json::object();
			});
		addControl(engine,
			{"bus.removePreset", 1, "Removes one of the user's own bus presets.", presetName, noResult, false},
			[&engine](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const auto name = parameters.at("name").get<std::string>();
				const BusDirection direction = busDirection(parameters.at("direction"));
				engine.modifySetting(context, "audio.busPresets",
					[direction, &name](const Json& value)
					{
						Json presets = value;
						Json& mine = presets[directionName(direction)];
						require(mine.is_object() && mine.erase(name) == 1,
							"no user bus preset " + name + "; only the user's own presets go");
						return presets;
					});
				return Json::object();
			});

		add(engine,
			{"track.add", 1,
				"Adds a track and its channel. Tracks go into a folder with parent, at index among its tracks "
				"(default: last). Audio channels default to stereo; format or channels set another format, as in "
				"bus.add. timeBase is musical (positions in ticks, which follow tempo changes) or linear (in "
				"samples, which keep their time); without it the user setting editing.defaultTimeBase decides. "
				"Folders get a channel only with groupChannel. New channels play through the main output, "
				"or through the group channel of the nearest folder around them that has one, and audio tracks record "
				"from the first input bus; input, output and instrument set them as channel.set does. tracks moves "
				"those tracks, each listed once, into a new folder, in that order, as track.move does (Move Selected "
				"Tracks to New Folder); for a group track, their channels play into its channel (Group Track to "
				"Selected Tracks), and for an effect track, they send to its channel with send (1 to 8), or else "
				"their first send without a destination, switched on, after the fader and at the user setting "
				"audio.defaultSendLevel (Effect Track to Selected Tracks, Add Effect Track to Send). projectFolder "
				"puts a group or effect track into the project's Group or FX folder, the first folder of that name "
				"at the top level, added at the end if there is none. With the user setting editing.applyFolderColor, "
				"a track added to a folder "
				"without a color takes the folder's. Marker tracks have no channel and stay at the top level; a "
				"project has at most 10, and a new one becomes the active marker track. Ruler tracks have no channel "
				"either; timeFormat is the time format their ruler shows (default barsBeats). The tempo track shows "
				"and "
				"edits the project's tempo (tempo.*), the signature track its time signatures (signature.*): a "
				"project has one of each at most, without a channel, at the top level. "

				"A VCA track's channel is a VCA fader (reference manual p. 153): no audio, input or output, a fader "
				"that controls a link group (linkGroup.add).",
				objectSchema(
					{{"type", trackTypes}, {"name", stringSchema}, {"color", color}, {"parent", idSchema},
						{"index", integerSchema}, {"format", formats}, {"channels", integerSchema},
						{"timeBase", timeBases}, {"groupChannel", booleanSchema}, {"input", idSchema},
						{"output", idSchema}, {"instrument", stringSchema}, {"timeFormat", timeFormats},
						{"tracks", arraySchema(idSchema)}, {"send", integerSchema}, {"projectFolder", booleanSchema}},
					{"type"}),
				{{"type", "object"},
					{"properties", {{"track", {{"type", "object"}}}, {"channel", {{"type", {"object", "null"}}}}}},
					{"required", {"track", "channel"}}},
				false},
			addTrack);
		add(engine,
			{"track.set", 1,
				"Changes a track's name, color (#rrggbb, or null for the default), time base or, for ruler tracks, "
				"time format. A new time base keeps its events, parts, markers and automation at the same time; on "
				"a linear track they keep their time when the tempo changes too, the notes in its MIDI parts with "
				"them. enabled false "
				"disables an audio, instrument or MIDI track: it keeps its events and channel settings but plays "
				"nothing and is silent. groupChannel turns a folder's group channel on or off: a new one is stereo "
				"and plays through the next group channel up, or the main output; the audio of the tracks inside "
				"then plays through it, or without it through the next group channel up, or the main output, and "
				"its automation and the routing of other channels to it go. Channels routed elsewhere by hand or "
				"disconnected keep their routing. With the user setting editing.applyFolderColor, a folder's new "
				"color goes to the tracks inside it. picture is an image file (PNG, JPEG or SVG) that the mixer shows "
				"on the track's channel, or empty for none.",
				objectSchema({{"track", idSchema}, {"name", stringSchema}, {"color", color}, {"timeBase", timeBases},
								 {"timeFormat", timeFormats}, {"enabled", booleanSchema},
								 {"groupChannel", booleanSchema}, {"picture", stringSchema}},
					{"track"}),
				resultSchema({"track"}), false},
			setTrack);
		add(engine,
			{"track.move", 1,
				"Moves a track, with what it contains, into a folder (parent, 0 for the top level) at index among its "
				"tracks. Where the group channel of the nearest folder around a moved track changes, its audio and "
				"that of the tracks inside it follows: into a folder with a group channel it plays through that, out "
				"of it through the next group channel up, or the main output. Channels routed elsewhere by hand or "
				"disconnected, and routes that would make a loop, stay as they are. With the user setting "
				"editing.applyFolderColor, tracks moved into a folder with a color take it.",
				objectSchema({{"track", idSchema}, {"parent", idSchema}, {"index", integerSchema}},
					{"track", "parent", "index"}),
				noResult, false},
			moveTrack);
		add(engine,
			{"track.remove", 1,
				"Removes a track with its channel, events, parts or markers; a folder also removes the tracks inside "
				"it. "
				"Without the active marker track, the topmost marker track becomes the active one.",
				objectSchema({{"track", idSchema}}, {"track"}), noResult, false},
			removeTrack);
		add(engine,
			{"track.removeEmpty", 1,
				"Removes the empty tracks (Remove Empty Tracks, reference manual p. 213): audio, instrument, MIDI and "
				"marker tracks without events, parts, markers or automation, and folders without tracks, unless "
				"another track's channel plays or sends into theirs (an instrument track a MIDI track plays, a "
				"folder's group channel). Group, effect, VCA, ruler, tempo and signature tracks stay. What it removes "
				"can leave others empty, such as a folder that held only empty tracks, so it looks again until no "
				"empty track is left. One undo step; none if nothing is empty. Returns the tracks removed, from top "
				"to bottom, with those inside removed folders.",
				objectSchema(Json::object()),
				Json{{"type", "object"}, {"properties", {{"tracks", arraySchema(idSchema)}}}, {"required", {"tracks"}}},
				false},
			removeEmptyTracks);
		add(engine,
			{"track.duplicate", 1,
				"Duplicates a track below it: its channel with every setting, its events, parts, markers and "
				"automation, and for a folder the tracks inside it, whose channels play through the copies of the "
				"group channels they played through. Returns the copy.",
				objectSchema({{"track", idSchema}}, {"track"}), resultSchema({"track"}), false},
			duplicateTrack);

		add(engine,
			{"channel.set", 1,
				"Changes a channel's format (or channels, as in bus.add), input (an input bus) or output (an output "
				"bus or a group channel; for MIDI tracks an instrument channel). 0 disconnects. Audio converts "
				"between formats where a channel feeds a destination of another one: speakers both formats have "
				"connect, the others are mixed down (5.1 to stereo: C and LFE into L and R at -3 dB, Ls into L and Rs "
				"into R at -6 dB; 7.1 to 5.1: Sl and Sr into the front and surround sides at -3 dB) or up (mono to C, "
				"or to L and R at the pan law's center). instrument is the built-in "
				"instrument an instrument track plays (see processor.list), or empty for none. volume is the fader in "
				"dB, from -144 (silence) to the project's maxVolume (default 0); values beyond them stop there. The "
				"panner depends on the routing: a mono channel on a stereo destination has its position pan, -1 "
				"(left) to 1 (right), placed with the project's pan law; a stereo channel on a stereo or mono "
				"destination has the balance panner (panner balance, the default: pan turns down the other side) or "
				"the combined panner (panner combined: panLeft and panRight place its left and right channel, by "
				"default -1 and 1). panBypass plays mono channels at the center and stereo ones hard left and right. "
				"Destinations wider than stereo have no panner yet. Automation of volume, pan, panLeft and panRight "
				"overrides them while it plays; changes glide over 5 ms. MIDI channels have no audio: midiVolume "
				"and midiPan (0 to 127, 64 is the center pan, null for none, the default) are sent to the "
				"instrument as MIDI volume (CC 7) and pan (CC 10) on MIDI channel 1, whenever they change and when "
				"playback starts or jumps; automation of those controllers overrides them. monitor and recordEnable "
				"are an audio track's buttons: monitor hears the input bus, recordEnable arms the track for recording "
				"and, by the user setting audio.autoMonitoring, monitors too: manual never, whileRecordEnabled always, "
				"whileRecording while the transport records, tapeMachine while it stands or records (the default). "
				"Monitoring glides in and out over one audio block. MIDI and instrument tracks have them too: "
				"recordEnable arms the track for MIDI recording, and while either is on, the track plays what comes "
				"from its MIDI input, midiInput: all (all MIDI inputs, the default) or none. mute silences a channel; "
				"while any channel is "
				"soloed (solo), only the soloed ones, the channels they play through (groups, instruments) and the "
				"channels that play into them are heard; exclusive (with solo) turns every other channel's solo off; "
				"soloDefeat keeps a channel and the channels it plays through heard while others are soloed. listen "
				"(not MIDI channels) also plays the channel after its fader on the monitoring bus, the main mix, which "
				"is turned down to the user setting audio.listenDim while any channel listens; the listening channels "
				"play there at audio.listenLevel, and their signal flow stays as it is. Muting, soloing and the dim "
				"glide over one audio block. notes is the "
				"channel's notepad, the user's text about it. modifiers (MIDI and instrument tracks; it changes only "
				"what it has, as project.get shows it) changes how the track's notes play, not the notes, both those "
				"of its parts and those played into it live: bypass, transpose (semitones, -127 to 127; notes beyond "
				"0 to 127 don't play), velocityShift (-127 to 127, added to MIDI 1.0 velocities), velocityCompression "
				"and lengthCompression ([numerator, denominator], each 1 to 16: multipliers of velocities, before the "
				"shift, and of lengths), random (two generators, [{target (off, position, pitch, velocity or length), "
				"minimum, maximum}]: each adds a value from its minimum to its maximum to every note, the same on "
				"every playback; positions and lengths in ticks, up to a quarter note either way, the others up to "
				"127; live notes get pitch and velocity only) and range (two, [{target (off, velocityLimit, "
				"velocityFilter, noteLimit or noteFilter), minimum, maximum (0 to 127)}], in order after the others: "
				"the limits move velocities to the nearest end and pitches by octaves into the range, the filters "
				"leave out the notes outside it). The channel strip's sections (audio channels only; "
				"each object changes only what it has, as project.get shows them) run before the fader: pre "
				"{gain (dB, -24 to 24), phase (inverts the polarity), lowCut and highCut {on, frequency (Hz, 20 or "
				"for the high cut 50 to 20000), slope (6, 12, 24, 36 or 48 dB per octave)}} before the plugin slots; "
				"after them strip {gate {on, threshold (dB, -80 to 0), range (dB, -80 to 0), attack (ms, 0.01 to "
				"100), release (ms, 1 to 4000)}, compressor {on, threshold (dB, -60 to 0), ratio (1 to 20), attack "
				"(ms, 0.1 to 100), release (ms, 10 to 2000), makeup (dB, 0 to 24)}}, then eq {bypass, bands: "
				"[{band (1 to 4), on, type (bell; band 1 also lowShelf or lowCut, band 4 highShelf or highCut, cuts "
				"at 12 dB per octave), frequency (Hz, 20 to 20000), gain (dB, -24 to 24), q (0.1 to 12)}]}, then "
				"the strip's limiter {on, threshold (dB, -30 to 0: no sample leaves above it, 1 ms of latency), "
				"release (ms, 1 to 1000)}. Everything starts off; changes glide. sends (audio channels only) "
				"changes the channel's eight sends, each by its send number (1 to 8) and only in what it has: "
				"destination (a group or effect channel, 0 for none), on, level (dB, from -144 to the project's "
				"maxVolume, stopping there, default 0) and preFader (before the fader and the mute button, after the "
				"strip's sections; default after the fader). A send starts off and without a destination. Neither "
				"an output nor a send may lead back to the channel through other outputs and sends, on or off, so "
				"routing never makes a loop; removing a destination's track clears the destination. A send's level "
				"and on glide over one audio block. A VCA track's channel has volume, mute, solo, soloDefeat, listen, "
				"monitor and recordEnable only. On a channel in a link group (linkGroup.add) the changes the group "
				"links go on to its other channels, the fader and the pan by as much as they moved, unless linked is "
				"false; a VCA fader's buttons go on to the channels of the group it controls, all in the same undo "
				"step.",
				objectSchema(
					{{"channel", idSchema}, {"format", formats}, {"channels", integerSchema}, {"input", idSchema},
						{"output", idSchema}, {"instrument", stringSchema},
						{"panner", enumSchema({"balance", "combined"})}, {"panLeft", numberSchema},
						{"panRight", numberSchema}, {"panBypass", booleanSchema},
						{"midiVolume", {{"type", {"integer", "null"}}}}, {"midiPan", {{"type", {"integer", "null"}}}},
						{"volume", numberSchema}, {"pan", numberSchema}, {"monitor", booleanSchema},
						{"recordEnable", booleanSchema}, {"midiInput", enumSchema({"all", "none"})},
						{"mute", booleanSchema}, {"solo", booleanSchema}, {"exclusive", booleanSchema},
						{"soloDefeat", booleanSchema}, {"listen", booleanSchema}, {"notes", stringSchema},
						{"linked", booleanSchema}, {"pre", {{"type", "object"}}}, {"eq", {{"type", "object"}}},
						{"strip", {{"type", "object"}}}, {"modifiers", {{"type", "object"}}},
						{"sends",
							arraySchema(
								objectSchema({{"send", integerSchema}, {"destination", idSchema}, {"on", booleanSchema},
												 {"level", numberSchema}, {"preFader", booleanSchema}},
									{"send"}))}},
					{"channel"}),
				resultSchema({"channel"}), false},
			setChannel);

		add(engine,
			{"channel.deactivateAll", 1,
				"Turns a state off on every channel, as the mixer toolbar's Deactivate All Mute, Solo and Listen "
				"States buttons do: mute, solo (with solo defeat) or listen. One undo step. Returns the channels "
				"that changed.",
				objectSchema({{"state", enumSchema({"mute", "solo", "listen"})}}, {"state"}),
				Json{{"type", "object"}, {"properties", {{"channels", {{"type", "array"}, {"items", idSchema}}}}},
					{"required", {"channels"}}},
				false},
			deactivateAll);

		add(engine,
			{"media.add", 1,
				"Adds an audio file. Its sample rate, channel count and length in sample frames are read from the "
				"file, or taken as given, for a file that is still being recorded. With copy, a file from outside the "
				"project folder is also copied into its media folder (see project.location); the project must have "
				"been saved. The copy runs as a job, returned as job (null without one), with the result that "
				"media.prepareArchive's job has: the media file is added at once where it is, and when the copy is "
				"done it is pointed at the copy as an undo step of its own.",
				objectSchema({{"path", stringSchema}, {"sampleRate", numberSchema}, {"channels", integerSchema},
								 {"length", integerSchema}, {"copy", booleanSchema}},
					{"path"}),
				{{"type", "object"},
					{"properties", {{"media", {{"type", "object"}}}, {"job", nullable(Jobs::schema())}}},
					{"required", {"media", "job"}}},
				false},
			[this, &jobs](Project& project, CommandContext& context)
			{
				const std::filesystem::path mediaFolder =
					m_file.empty() ? std::filesystem::path() : mediaFolderFor(std::filesystem::u8path(m_file));
				Json result = addMedia(project, context, mediaFolder);
				result["job"] = nullptr;
				const MediaFile& media = project.media.at(result.at("media").at("id").get<ObjectId>());
				if (context.parameters().value("copy", false) && !isInside(media.path, mediaFolder.parent_path()))
					result["job"] = startMediaCopy(jobs, "media.add", {MediaCopy{media.id, media.path, {}}},
						mediaFolder, "Copy media file into the project");
				return result;
			});
		add(engine,
			{"media.relink", 1,
				"Points media files at other files, such as copies, as one undo step. Each new file must be an audio "
				"file with the media file's sample rate and channel count. An entry with from applies only if the "
				"media file is still there and still points at from; otherwise it is skipped. Returns the media "
				"files it relinked and the ones it skipped.",
				objectSchema(
					{{"files",
						arraySchema(objectSchema({{"media", idSchema}, {"path", stringSchema}, {"from", stringSchema}},
							{"media", "path"}))}},
					{"files"}),
				{{"type", "object"},
					{"properties", {{"relinked", arraySchema(idSchema)}, {"skipped", arraySchema(idSchema)}}},
					{"required", {"relinked", "skipped"}}},
				false},
			relinkMedia);
		add(engine,
			{"media.convert", 1,
				"Converts a media file to another sample rate (by default the project's), writes the result to path as "
				"a 32-bit float WAV file and adds it as a new media file. path must not be one of the project's media "
				"files.",
				objectSchema(
					{{"media", idSchema}, {"path", stringSchema}, {"sampleRate", numberSchema}}, {"media", "path"}),
				resultSchema({"media"}), false},
			convertMedia);
		add(engine,
			{"media.stretch", 1,
				"Time-stretches and pitch-shifts a media file, writes the result to path as a 32-bit float WAV file "
				"and adds it as a new media file; path must not be one of the project's media files. ratio is the new "
				"length over the old one, from 0.1 to 10 (default 1: "
				"unchanged); semitones and cents shift the pitch, together within 48 semitones either way (default 0). "
				"preset picks the algorithm: quality (default) and efficient for any material, drums, plucked, pads, "
				"vocals, mix and solo tuned for kinds of material at a lower CPU cost. formants keeps the spectral "
				"envelope when the pitch shifts, e.g. for voices. mode offline (default) analyzes the whole file first "
				"for the best result; realtime processes it the way playback does, with ratios from 0.25 to 4 and "
				"pitch "
				"shifts within 24 semitones.",
				objectSchema(
					{{"media", idSchema}, {"path", stringSchema}, {"ratio", numberSchema}, {"semitones", numberSchema},
						{"cents", numberSchema}, {"preset", enumSchema(dsp::stretchPresetNames())},
						{"formants", booleanSchema}, {"mode", enumSchema(dsp::stretchModeNames())}},
					{"media", "path"}),
				resultSchema({"media"}), false},
			stretchMedia);
		add(engine,
			{"media.remove", 1, "Removes an audio file that no clip uses.",
				objectSchema({{"media", idSchema}}, {"media"}), noResult, false},
			removeMedia);
		add(engine,
			{"clip.add", 1, "Adds a clip that plays an audio file; it is named after the file unless a name is given.",
				objectSchema({{"media", idSchema}, {"name", stringSchema}}, {"media"}), resultSchema({"clip"}), false},
			addClip);
		add(engine,
			{"clip.remove", 1, "Removes a clip that no audio event uses.", objectSchema({{"clip", idSchema}}, {"clip"}),
				noResult, false},
			removeClip);

		add(engine,
			{"audioEvent.add", 1,
				"Adds an audio event that plays a section of a clip on an audio track. position is in the track's time "
				"base (ticks or samples); offset and length are in the clip's sample frames, length defaulting to the "
				"rest of the clip. volume is in dB, from -144 to 6 (default 0); a muted event doesn't play. A track "
				"plays one event at a time: where events overlap, the one added last plays, unless they crossfade; "
				"event.toFront brings another one to the front.",
				objectSchema(
					{{"track", idSchema}, {"clip", idSchema}, {"position", integerSchema}, {"offset", integerSchema},
						{"length", integerSchema}, {"volume", numberSchema}, {"mute", booleanSchema}},
					{"track", "clip", "position"}),
				resultSchema({"event"}), false},
			addAudioEvent);
		add(engine,
			{"audioEvent.set", 1, "Changes an audio event's position, clip, offset, length, volume (dB) or mute.",
				objectSchema(
					{{"event", idSchema}, {"clip", idSchema}, {"position", integerSchema}, {"offset", integerSchema},
						{"length", integerSchema}, {"volume", numberSchema}, {"mute", booleanSchema}},
					{"event"}),
				resultSchema({"event"}), false},
			[this](Project& project, CommandContext& context)
			{ return setAudioEvent(project, context, m_automationFollowsEvents); });
		add(engine,
			{"audioEvent.remove", 1, "Removes an audio event.", objectSchema({{"event", idSchema}}, {"event"}),
				noResult, false},
			removeAudioEvent);

		add(engine,
			{"audioPart.add", 1,
				"Adds an empty audio part to an audio track (reference manual p. 250): a container of audio events, "
				"which play only within it and move, copy, split and glue with it through the event commands. "
				"position and length are in the track's time base; name defaults to the track's. event.toPart puts "
				"events into a part, event.dissolvePart takes them out again.",
				objectSchema({{"track", idSchema}, {"position", integerSchema}, {"length", integerSchema},
								 {"name", stringSchema}},
					{"track", "position", "length"}),
				resultSchema({"part"}), false},
			addAudioPart);
		add(engine,
			{"midiPart.add", 1,

				"Adds a MIDI part to a MIDI or instrument track. position and length are in the track's time base.",
				objectSchema({{"track", idSchema}, {"position", integerSchema}, {"length", integerSchema},
								 {"name", stringSchema}},
					{"track", "position", "length"}),
				resultSchema({"part"}), false},
			addMidiPart);
		add(engine,
			{"midiPart.set", 1,
				"Changes a MIDI part's name, position or length, or how its notes play: transpose (semitones) and "
				"velocity (added to MIDI 1.0 velocities), each -127 to 127 and added to the track's MIDI modifiers "
				"(channel.set modifiers), also while they are bypassed. The notes themselves stay as they are.",
				objectSchema({{"part", idSchema}, {"name", stringSchema}, {"position", integerSchema},
								 {"length", integerSchema}, {"transpose", integerSchema}, {"velocity", integerSchema}},
					{"part"}),
				resultSchema({"part"}), false},
			[this](Project& project, CommandContext& context)
			{ return setMidiPart(project, context, m_automationFollowsEvents); });
		add(engine,
			{"midiPart.remove", 1, "Removes a MIDI part with its notes and controllers.",
				objectSchema({{"part", idSchema}}, {"part"}), noResult, false},
			removeMidiPart);

		const Json note =
			objectSchema({{"position", integerSchema}, {"length", integerSchema}, {"pitch", integerSchema},
							 {"velocity", numberSchema}, {"releaseVelocity", numberSchema}, {"channel", integerSchema},
							 {"probability", numberSchema}, {"velocityVariance", numberSchema}},
				{"position", "length", "pitch", "velocity"});
		add(engine,
			{"note.add", 1,
				"Adds notes to a MIDI part: position from the part's start and length in ticks, pitch 0-127, velocity "
				"0-1, release (note-off) velocity 0-1 (default 0.5), MIDI channel 0-15 (default 0), play probability "
				"0-1, the chance that the note plays (default 1), and velocity variance -1 to 1, the share by which "
				"playback raises (positive) or lowers (negative) its velocity at most (default 0).",
				objectSchema({{"part", idSchema}, {"notes", arraySchema(note)}}, {"part", "notes"}),
				{{"type", "object"}, {"properties", {{"notes", {{"type", "array"}}}}}, {"required", {"notes"}}}, false},
			addNotes);
		add(engine,
			{"note.remove", 1, "Removes notes from a MIDI part.",
				objectSchema({{"part", idSchema}, {"notes", arraySchema(idSchema)}}, {"part", "notes"}), noResult,
				false},
			[](Project& project, CommandContext& context)
			{ return removeFromPart(project, context, &MidiPart::notes, "notes", "note"); });

		const Json controller = objectSchema(
			{{"position", integerSchema},
				{"type",
					enumSchema({"controller", "pitchBend", "channelPressure", "polyPressure", "program", "sysEx"})},
				{"number", integerSchema}, {"value", numberSchema}, {"channel", integerSchema}, {"bank", integerSchema},
				{"data", arraySchema(integerSchema)}},
			{"position"});
		add(engine,
			{"controller.add", 1,
				"Adds controller events to a MIDI part: position from the part's start in ticks, MIDI channel 0-15 "
				"(default 0). type (default controller) says what else an event has: controller: number 0-127 and "
				"value 0-1; pitchBend: value 0-1, 0.5 is no bend; channelPressure: value; polyPressure: number (the "
				"pitch) and value; program: number (the program) and optionally bank 0-16383; sysEx: data, the "
				"message's bytes from 0xF0 to 0xF7.",
				objectSchema({{"part", idSchema}, {"controllers", arraySchema(controller)}}, {"part", "controllers"}),
				{{"type", "object"}, {"properties", {{"controllers", {{"type", "array"}}}}},
					{"required", {"controllers"}}},
				false},
			addControllers);
		add(engine,
			{"controller.remove", 1, "Removes controller events from a MIDI part.",
				objectSchema({{"part", idSchema}, {"controllers", arraySchema(idSchema)}}, {"part", "controllers"}),
				noResult, false},
			[](Project& project, CommandContext& context)
			{ return removeFromPart(project, context, &MidiPart::controllers, "controllers", "controller"); });

		registerMidiEditCommands(engine);
		registerMarkerCommands(engine);
		registerTempoCommands(engine);
		registerSignatureCommands(engine);
		registerAutomationCommands(engine);
		registerParameterCommands(engine);
		registerSlotCommands(engine);
		midi::registerQuantizeCommands(*this, engine);
		midi::registerFunctionCommands(*this, engine);
		midi::registerControllerLaneCommands(*this, engine);
		registerMidiFileCommands(engine);
		registerAudioFileCommands(engine, jobs);
		registerRecordingCommands(engine);
		registerRenderCommands(engine);
		registerPoolCommands(engine);
		registerFadeCommands(engine);
		registerVolumeCurveCommands(*this, engine);
		registerEventEditCommands(engine);
		registerSnapCommands(*this, engine);
		registerMixerCommands(*this, engine);
		registerLinkGroupCommands(*this, engine);
		registerChannelSettingsCommands(*this, engine);
		registerEditorCommands(*this, engine);
		registerSampleEditorCommands(*this, engine);
		registerHitpointCommands(*this, engine);
		registerHistoryCommands(engine);
		registerMediaFolderCommands(engine, jobs);
		registerProjectFileCommands(engine);
	}

	void ProjectModel::registerAutomationCommands(Engine& engine)
	{
		const Json idSchema = {{"type", "integer"}};
		const Json numberSchema = {{"type", "number"}};
		const Json booleanSchema = {{"type", "boolean"}};
		const Json noResult = Json::object();
		const Json target =
			objectSchema({{"type",
							  enumSchema({"volume", "pan", "panLeft", "panRight", "mute", "instrument", "plugin",
								  "controller", "pitchBend", "channelPressure"})},
							 {"parameter", idSchema}, {"slot", idSchema}, {"number", idSchema}, {"channel", idSchema}},
				{"type"});
		const Json shape = enumSchema({"step", "linear", "curve"});
		const Json pointsResult = {
			{"type", "object"}, {"properties", {{"points", {{"type", "array"}}}}}, {"required", {"points"}}};

		add(engine,
			{"automationLane.add", 1,
				"Adds an automation lane to a track: the curve of one parameter. target.type says which: volume (the "
				"channel's fader, in dB), pan, panLeft and panRight (-1 to 1, see channel.set) and mute (0 off, 1 on; "
				"a lane that plays overrides the mute button) on tracks with audio channels; instrument, a parameter "
				"of an instrument track's built-in instrument (target.parameter, an id from processor.list, in its "
				"range); plugin, a parameter of the built-in plugin in one of the track's slots (target.slot and "
				"target.parameter, as parameter.list gives them, in its range; the lane goes when the plugin is "
				"removed or moves to another channel); controller (target.number 0-127), pitchBend (0.5 is no bend) "
				"and channelPressure, all 0 to 1, on MIDI and instrument tracks, with target.channel 0-15 (default 0). "
				"A track has one lane per target. read (default true): the curve plays; without it, or without points, "
				"the parameter keeps its static value.",
				objectSchema({{"track", idSchema}, {"target", target}, {"read", booleanSchema}}, {"track", "target"}),
				resultSchema({"lane"}), false},
			addAutomationLane);
		add(engine,
			{"automationLane.set", 1, "Turns reading an automation lane on or off.",
				objectSchema({{"lane", idSchema}, {"read", booleanSchema}}, {"lane"}), resultSchema({"lane"}), false},
			setAutomationLane);
		add(engine,
			{"automationLane.remove", 1, "Removes an automation lane with its points.",
				objectSchema({{"lane", idSchema}}, {"lane"}), noResult, false},
			removeAutomationLane);
		add(engine,
			{"automationLane.list", 1,
				"The automation lanes of a track, or of every track in track order, with their points sorted by "
				"position.",
				objectSchema({{"track", idSchema}}),
				{{"type", "object"}, {"properties", {{"lanes", {{"type", "array"}}}}}, {"required", {"lanes"}}}, true},
			[](Project& project, CommandContext& context) { return listAutomationLanes(project, context); });

		const Json point = objectSchema({{"position", idSchema}, {"value", numberSchema}, {"shape", shape},
											{"curve", numberSchema}, {"terminator", booleanSchema}},
			{"position", "value"});
		add(engine,
			{"automationPoint.add", 1,
				"Adds points to an automation lane: position in the track's time base (ticks on musical tracks, so "
				"automation follows tempo changes; samples on linear ones, so it keeps its time), value in the "
				"target's unit, shape of the curve to the next point (step, linear or curve; "
				"default linear) and for curves the bend, curve, from -1 to 1 (above 0 the value moves fast at first). "
				"Mute and stepped parameters (parameter.list) have step curves: their points are steps with whole "
				"values. "
				"Before the first point the curve has its value, after the last point the last one's; points at one "
				"position make a jump. A ramp of volume moves the gain in a straight line. terminator (default false) "
				"ends the curve at the point: up to the next point the parameter has its static value, a gap. With "
				"virgin territory (automation.set) there are gaps before the first and after the last point too.",
				objectSchema({{"lane", idSchema}, {"points", arraySchema(point)}}, {"lane", "points"}), pointsResult,
				false},
			addAutomationPoints);
		const Json change = objectSchema({{"id", idSchema}, {"position", idSchema}, {"value", numberSchema},
											 {"shape", shape}, {"curve", numberSchema}, {"terminator", booleanSchema}},
			{"id"});
		add(engine,
			{"automationPoint.set", 1,
				"Changes points of an automation lane: their position, value, shape, curve or terminator (see "
				"automationPoint.add).",
				objectSchema({{"lane", idSchema}, {"points", arraySchema(change)}}, {"lane", "points"}), pointsResult,
				false},
			setAutomationPoints);
		add(engine,
			{"automationPoint.remove", 1, "Removes points from an automation lane.",
				objectSchema({{"lane", idSchema}, {"points", arraySchema(idSchema)}}, {"lane", "points"}), noResult,
				false},
			removeAutomationPoints);

		const Json settings = objectSchema({{"followsEvents", booleanSchema}, {"virginTerritory", booleanSchema}},
			{"followsEvents", "virginTerritory"});
		const auto settingsJson = [this]()
		{ return Json{{"followsEvents", m_automationFollowsEvents}, {"virginTerritory", m_project.virginTerritory}}; };
		addControl(engine,
			{"automation.get", 1,
				"The automation settings. followsEvents (default false): when an audio event or MIDI part moves, the "
				"automation of its track in its range moves with it and replaces the points where it lands. "
				"virginTerritory (default false; reference manual p. 881): automation lanes have no curve before "
				"their first and after their last point, where their parameters have their static values, as between "
				"a terminator point and the next one; without it they have initial values, the first point's before "
				"it and the last point's after it.",
				objectSchema({}), settings, true},
			[settingsJson](CommandContext&) { return settingsJson(); });
		addEdit(engine,
			{"automation.set", 1,
				"Changes the automation settings (see automation.get). followsEvents is a setting, so it stays when a "
				"new project starts; virginTerritory is saved with the project. Neither is an undo step.",
				objectSchema({{"followsEvents", booleanSchema}, {"virginTerritory", booleanSchema}}), settings, false},
			"Change automation settings",
			[this, settingsJson](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				m_automationFollowsEvents = parameters.value("followsEvents", m_automationFollowsEvents);
				project.virginTerritory = parameters.value("virginTerritory", project.virginTerritory);
				Json result = settingsJson();
				context.emit("automation.changed", result);
				return result;
			});
	}

	void ProjectModel::registerMediaFolderCommands(Engine& engine, Jobs& jobs)
	{
		const Json stringSchema = {{"type", "string"}};
		const Json nullableString = {{"type", {"string", "null"}}};
		const Json booleanSchema = {{"type", "boolean"}};
		const Json numberSchema = {{"type", "number"}};

		add(engine,
			{"project.location", 1,
				"Where the project is: its project file (set by project.save and project.load), the folder of that "
				"file, and the media folder in it where media.add with copy and media.prepareArchive put audio files. "
				"All null for a project that has no file yet.",
				objectSchema(Json::object()),
				objectSchema({{"file", nullableString}, {"folder", nullableString}, {"mediaFolder", nullableString}},
					{"file", "folder", "mediaFolder"}),
				true},
			[this](Project&, CommandContext&)
			{
				if (m_file.empty())
					return Json{{"file", nullptr}, {"folder", nullptr}, {"mediaFolder", nullptr}};
				const std::filesystem::path file = std::filesystem::u8path(m_file);
				return Json{{"file", m_file}, {"folder", file.parent_path().u8string()},
					{"mediaFolder", mediaFolderFor(file).u8string()}};
			});

		const Json integerSchema = {{"type", "integer"}};
		const Json copied = objectSchema({{"media", integerSchema}, {"from", stringSchema}, {"to", stringSchema}});
		const Json missing = objectSchema({{"media", integerSchema}, {"path", stringSchema}});
		addControl(engine,
			{"media.prepareArchive", 1,
				"Makes the project self-contained, so that its folder can be archived or moved: copies every media "
				"file from outside the project folder into its media folder and points the media at the copies. The "
				"copies run as a job: this returns at once, job.progress and job.finished events follow, and "
				"job.cancel stops it and removes the copies. Once they are done, the media are pointed at them as one "
				"undo step. The job's result lists the copies as copied, media files that couldn't be read as "
				"missing (they keep their paths), and media whose path changed while the job ran as changed (they "
				"keep the new path). The project must have been saved; save it again without embedding media to keep "
				"the new paths.",
				objectSchema({}), objectSchema({{"job", Jobs::schema()}}, {"job"}), false},
			[this, &jobs](CommandContext&)
			{
				require(
					!m_file.empty(), "save the project first: media are copied into the folder of its project file");
				const std::filesystem::path file = std::filesystem::u8path(m_file);
				std::vector<MediaCopy> files;
				for (const auto& [id, media] : m_project.media)
					if (!isInside(media.path, file.parent_path()))
						files.push_back({id, media.path, {}});
				return Json{{"job",
					startMediaCopy(
						jobs, "media.prepareArchive", std::move(files), mediaFolderFor(file), "Prepare archive")}};
			});
		addControl(engine,
			{"project.revision", 1,
				"A number that grows with every change of the project: every edit, undo and redo, and loading or "
				"starting a project. project.load with revision loads only if it hasn't changed. modified says "
				"whether the project changed since it was last saved, loaded or started; undoing back to the saved "
				"state still counts as a change. project identifies the open project: the revision at which it was "
				"loaded or started, so it changes only when another project is opened.",
				objectSchema(Json::object()),
				objectSchema({{"revision", integerSchema}, {"modified", booleanSchema}, {"project", integerSchema}},
					{"revision", "modified", "project"}),
				true},
			[this](CommandContext&)
			{
				return Json{{"revision", m_revision}, {"modified", m_revision != m_savedRevision},
					{"project", m_openedRevision}};
			});

		engine.registerCommand(
			{"project.backup", 1,
				"Backs the project up as it is now into folder, which must be empty or new: the project file, named "
				"name (by default after the project file, or else the project), and copies of its media in the "
				"folder Audio, which the project file refers to relative to itself, so that the folder opens on "
				"another machine. The project itself stays as it is. minimize keeps only the ranges of audio files "
				"that audio events play, with handles seconds (default 1) before and after; removeUnused leaves out "
				"clips that no audio event uses and media that no clip uses. currentVersionsOnly and skipVideo have "
				"no effect until clips have versions and projects have video. activate opens the backup once it is "
				"done, unless the project changed meanwhile. The backup runs as a job: this returns at once, "
				"job.progress and job.finished events follow, and job.cancel stops it and removes what it wrote. A "
				"folder that a running job writes into can't be used. The job's result is {file, files, missing, "
				"activated, activationError}: missing lists the media files that couldn't be read and keep their old "
				"paths; activated says whether the backup was opened, and activationError why not, if it was asked "
				"to be.",
				objectSchema({{"folder", stringSchema}, {"name", stringSchema}, {"minimize", booleanSchema},
								 {"handles", numberSchema}, {"removeUnused", booleanSchema},
								 {"currentVersionsOnly", booleanSchema}, {"skipVideo", booleanSchema},
								 {"activate", booleanSchema}},
					{"folder"}),
				objectSchema({{"job", Jobs::schema()}}, {"job"}), false},
			[this, &jobs](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				BackupOptions options;
				options.folder = parameters.at("folder").get<std::string>();
				require(!options.folder.empty(), "folder must not be empty");
				const std::filesystem::path folder = std::filesystem::u8path(options.folder);
				require(!jobs.writesInto(folder),
					"a running job writes into " + options.folder + ": choose another folder or wait for it");
				std::error_code error;
				if (std::filesystem::exists(folder, error))
				{
					require(std::filesystem::is_directory(folder, error), "folder " + options.folder + " is a file");
					require(std::filesystem::directory_iterator(folder, error) == std::filesystem::directory_iterator(),
						"choose an empty or a new folder: " + options.folder + " isn't empty");
				}
				auto project = std::make_shared<Project>(m_project); // the project as it is now, for the job's thread
				if (parameters.contains("name"))
				{
					options.name = parameters.at("name").get<std::string>();
					const std::string problem = fileNameProblem(options.name);
					require(problem.empty(), problem);
					project->name = options.name;
				}
				else
				{
					options.name = safeFileName(
						m_file.empty() ? m_project.name : projectFileStem(std::filesystem::u8path(m_file)));
				}
				options.minimize = parameters.value("minimize", false);
				options.handles = parameters.value("handles", 1.0);
				require(options.handles >= 0.0, "handles must not be negative");
				options.removeUnused = parameters.value("removeUnused", false);
				options.currentVersionsOnly = parameters.value("currentVersionsOnly", false);
				options.skipVideo = parameters.value("skipVideo", false);
				const bool activate = parameters.value("activate", false);

				// Opening the backup replaces the project, so only if it is still as it was backed up.
				const uint64_t revision = m_revision;
				return Json{{"job",
					jobs.start(
						"project.backup",
						[project = std::shared_ptr<const Project>(std::move(project)), options, activate, revision](
							JobContext& job)
						{
							Json result = backupProject(*project, options, job);
							result["activated"] = false;
							result["activationError"] = nullptr;
							if (activate)
							{
								try
								{
									job.command("project.load", {{"path", result.at("file")}, {"revision", revision}});
									result["activated"] = true;
								}
								catch (const CommandError& error)
								{
									result["activationError"] =
										std::string("the backup wasn't opened: ") + error.what();
								}
							}
							return result;
						},
						folder)}};
			});
	}

}
