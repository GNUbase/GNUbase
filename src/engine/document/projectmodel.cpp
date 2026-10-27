// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/projectmodel.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
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
		};

		const TrackTypeName& trackTypeName(TrackType type)
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

		const char* timeBaseName(time::TimeBase base)
		{
			return base == time::TimeBase::Musical ? "musical" : "linear";
		}

		time::TimeBase timeBase(const std::string& name)
		{
			return name == "linear" ? time::TimeBase::Linear : time::TimeBase::Musical;
		}

		const char* directionName(BusDirection direction)
		{
			return direction == BusDirection::Input ? "input" : "output";
		}

		// Note velocities and controller values are 0 to 1 in JSON, whatever the internal resolution.
		constexpr double g_maxVelocity = 65535.0;
		constexpr double g_maxControllerValue = 4294967295.0;

		// JSON for every object.

		Json toJson(const Bus& bus)
		{
			return {{"id", bus.id}, {"name", bus.name}, {"direction", directionName(bus.direction)},
				{"channels", bus.channels}};
		}

		Json toJson(const Track& track)
		{
			return {{"id", track.id}, {"type", trackTypeName(track.type).name}, {"name", track.name},
				{"color", track.color.empty() ? Json(nullptr) : Json(track.color)},
				{"timeBase", timeBaseName(track.timeBase)}, {"parent", track.parent}, {"channel", track.channel}};
		}

		Json toJson(const Channel& channel)
		{
			return {{"id", channel.id}, {"track", channel.track}, {"channels", channel.channels},
				{"input", channel.input}, {"output", channel.output}};
		}

		Json toJson(const MediaFile& media)
		{
			return {{"id", media.id}, {"path", media.path}, {"sampleRate", media.sampleRate},
				{"channels", media.channels}, {"length", media.length}};
		}

		Json toJson(const Clip& clip)
		{
			return {{"id", clip.id}, {"name", clip.name}, {"media", clip.media}};
		}

		Json toJson(const AudioEvent& event)
		{
			return {{"id", event.id}, {"track", event.track}, {"clip", event.clip}, {"position", event.position.value},
				{"offset", event.offset}, {"length", event.length}};
		}

		Json toJson(const Note& note)
		{
			return {{"id", note.id}, {"position", note.position}, {"length", note.length}, {"pitch", note.pitch},
				{"velocity", note.velocity / g_maxVelocity}, {"channel", note.channel}};
		}

		Json toJson(const Controller& controller)
		{
			return {{"id", controller.id}, {"position", controller.position}, {"number", controller.number},
				{"value", controller.value / g_maxControllerValue}, {"channel", controller.channel}};
		}

		Json toJson(const MidiPart& part)
		{
			Json notes = Json::array();
			for (const auto& [id, note] : part.notes)
				notes.push_back(toJson(note));
			Json controllers = Json::array();
			for (const auto& [id, controller] : part.controllers)
				controllers.push_back(toJson(controller));
			return {{"id", part.id}, {"track", part.track}, {"name", part.name}, {"position", part.position.value},
				{"length", part.length}, {"notes", notes}, {"controllers", controllers}};
		}

		template <typename Map> Json toJsonArray(const Map& objects)
		{
			Json array = Json::array();
			for (const auto& entry : objects)
				array.push_back(toJson(entry.second));
			return array;
		}

		Json toJson(const Project& project)
		{
			Json busses = Json::array();
			for (const Bus& bus : project.busses)
				busses.push_back(toJson(bus));
			Json tracks = Json::array();
			for (const ObjectId id : project.trackOrder())
				tracks.push_back(toJson(project.trackById.at(id)));
			return {{"name", project.name}, {"sampleRate", project.sampleRate}, {"busses", busses}, {"tracks", tracks},
				{"channels", toJsonArray(project.channels)}, {"media", toJsonArray(project.media)},
				{"clips", toJsonArray(project.clips)}, {"audioEvents", toJsonArray(project.audioEvents)},
				{"midiParts", toJsonArray(project.midiParts)}};
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

		int audioChannels(const Json& parameters, int fallback)
		{
			const int channels = parameters.value("channels", fallback);
			requireRange(channels, 1, g_maxAudioChannels, "channels");
			return channels;
		}

		std::string color(const Json& parameters, const std::string& fallback)
		{
			if (!parameters.contains("color") || parameters.at("color").is_null())
				return parameters.contains("color") ? std::string() : fallback;
			const std::string value = parameters.at("color").get<std::string>();
			const bool valid = value.size() == 7 && value[0] == '#' &&
				std::all_of(value.begin() + 1, value.end(),
					[](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
			require(valid, "color must be #rrggbb");
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

		// Group tracks and folders with their group channel on sum other channels.
		bool isGroupChannel(const Project& project, ObjectId channelId)
		{
			const auto channel = project.channels.find(channelId);
			if (channel == project.channels.end())
				return false;
			const TrackType type = project.trackById.at(channel->second.track).type;
			return type == TrackType::Group || type == TrackType::Folder;
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
			require(isGroupChannel(project, output), "output must be an output bus or a group channel");
			for (ObjectId next = output; next != 0 && project.channels.count(next) != 0;
				next = project.channels.at(next).output)
				require(next != channel.id,
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
			for (auto& [id, part] : project.midiParts)
				if (part.track == track.id)
				{
					const time::Position end = time::rebase(partEnd(part), base, project.tempo, project.sampleRate);
					part.position = time::rebase(part.position, base, project.tempo, project.sampleRate);
					part.length = end.value - part.position.value;
					context.emit("midiPart.changed", {{"part", toJson(part)}});
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
			project.sampleRate = sampleRate;
		}

		std::string defaultTrackName(const Project& project, TrackType type)
		{
			const auto count = std::count_if(project.trackById.begin(), project.trackById.end(),
				[type](const auto& entry) { return entry.second.type == type; });
			const std::string number = std::to_string(count + 1);
			return trackTypeName(type).defaultName + std::string(number.size() < 2 ? " 0" : " ") + number;
		}

		// Commands.

		Json addTrack(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const TrackType type = trackType(parameters.at("type").get<std::string>());
			const ObjectId parent = folderParent(project, parameters);
			const size_t index = insertIndex(parameters, siblings(project, parent));
			const bool groupChannel = parameters.value("groupChannel", false);
			require(!groupChannel || type == TrackType::Folder, "only folder tracks have an optional group channel");
			const bool hasChannel = type != TrackType::Folder || groupChannel;
			const bool hasAudio = hasChannel && type != TrackType::Midi;
			require(hasAudio || !parameters.contains("channels"), "this track has no audio channels");
			const int channels = hasAudio ? audioChannels(parameters, 2) : 0;

			Track track;
			track.id = project.newId();
			track.type = type;
			track.name = parameters.value("name", defaultTrackName(project, type));
			track.color = color(parameters, {});
			track.timeBase = timeBase(parameters.value("timeBase", "musical"));
			track.parent = parent;

			// New tracks play through the main output, or through the folder's group channel when added to one.
			std::optional<Channel> channel;
			if (hasChannel)
			{
				channel = Channel{project.newId(), track.id, channels, 0, 0};
				const auto mainOutput = std::find_if(project.busses.begin(), project.busses.end(),
					[](const Bus& bus) { return bus.direction == BusDirection::Output; });
				if (hasAudio && mainOutput != project.busses.end())
					channel->output = mainOutput->id;
				if (hasAudio && parent != 0 && project.trackById.at(parent).channel != 0)
					channel->output = project.trackById.at(parent).channel;
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

			Json result = {{"track", toJson(track)}, {"channel", channel ? toJson(*channel) : Json(nullptr)}};
			context.emit("track.added", result);
			return result;
		}

		Json setTrack(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			Track& track = find(project.trackById, parameters.at("track").get<ObjectId>(), "track");
			const std::string newColor = color(parameters, track.color);
			if (parameters.contains("name"))
				track.name = parameters.at("name").get<std::string>();
			track.color = newColor;
			if (parameters.contains("timeBase"))
			{
				const time::TimeBase base = timeBase(parameters.at("timeBase").get<std::string>());
				if (base != track.timeBase)
					rebaseTrack(project, track, base, context);
			}
			Json result = {{"track", toJson(track)}};
			context.emit("track.changed", result);
			return result;
		}

		Json moveTrack(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const ObjectId id = parameters.at("track").get<ObjectId>();
			Track& track = find(project.trackById, id, "track");
			const ObjectId parent = folderParent(project, parameters);
			const std::vector<ObjectId> moved = subtree(project, id);
			require(std::find(moved.begin(), moved.end(), parent) == moved.end(),
				"can't move a folder into itself or a folder inside it");

			std::vector<ObjectId>& from = siblings(project, track.parent);
			const auto current = std::find(from.begin(), from.end(), id);
			const auto oldIndex = current - from.begin();
			std::vector<ObjectId>& to = siblings(project, parent);
			// The index counts the siblings without the moved track.
			const size_t size = to.size() - (track.parent == parent ? 1 : 0);
			const auto index = parameters.at("index").get<int64_t>();
			requireRange(index, 0, static_cast<int64_t>(size), "index");

			from.erase(from.begin() + oldIndex);
			to.insert(to.begin() + index, id);
			track.parent = parent;
			context.emit("track.moved", {{"track", id}, {"parent", parent}, {"index", index}});
			return Json::object();
		}

		// Disconnects every channel whose input or output is one of the given objects.
		void disconnect(Project& project, const std::vector<ObjectId>& gone, CommandContext& context)
		{
			const auto isGone = [&gone](ObjectId id) { return std::find(gone.begin(), gone.end(), id) != gone.end(); };
			for (auto& [id, channel] : project.channels)
			{
				const bool input = channel.input != 0 && isGone(channel.input);
				const bool output = channel.output != 0 && isGone(channel.output);
				if (input)
					channel.input = 0;
				if (output)
					channel.output = 0;
				if (input || output)
					context.emit("channel.changed", {{"channel", toJson(channel)}});
			}
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
				for (auto part = project.midiParts.begin(); part != project.midiParts.end();)
					part = part->second.track == removed ? project.midiParts.erase(part) : std::next(part);
				project.channels.erase(gone.channel);
				project.trackById.erase(removed);
				context.emit("track.removed", {{"track", removed}});
			}
			disconnect(project, channels, context);
			return Json::object();
		}

		Json setChannel(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			Channel& channel = find(project.channels, parameters.at("channel").get<ObjectId>(), "channel");
			const TrackType type = project.trackById.at(channel.track).type;
			Channel changed = channel;
			if (parameters.contains("channels"))
			{
				require(type != TrackType::Midi, "a MIDI channel has no audio channels");
				changed.channels = audioChannels(parameters, channel.channels);
			}
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
			channel = changed;
			Json result = {{"channel", toJson(channel)}};
			context.emit("channel.changed", result);
			return result;
		}

		Json addBus(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			Bus bus;
			bus.direction =
				parameters.at("direction").get<std::string>() == "input" ? BusDirection::Input : BusDirection::Output;
			bus.name = parameters.at("name").get<std::string>();
			bus.channels = audioChannels(parameters, 2);
			bus.id = project.newId();
			project.busses.push_back(bus);
			Json result = {{"bus", toJson(bus)}};
			context.emit("bus.added", result);
			return result;
		}

		Json setBus(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			Bus& bus = findBus(project, parameters.at("bus").get<ObjectId>());
			if (parameters.contains("name"))
				bus.name = parameters.at("name").get<std::string>();
			Json result = {{"bus", toJson(bus)}};
			context.emit("bus.changed", result);
			return result;
		}

		Json removeBus(Project& project, CommandContext& context)
		{
			const ObjectId id = context.parameters().at("bus").get<ObjectId>();
			findBus(project, id);
			project.busses.erase(std::find_if(
				project.busses.begin(), project.busses.end(), [id](const Bus& bus) { return bus.id == id; }));
			disconnect(project, {id}, context);
			context.emit("bus.removed", {{"bus", id}});
			return Json::object();
		}

		Json addMedia(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			MediaFile media;
			media.path = parameters.at("path").get<std::string>();
			media.sampleRate = parameters.at("sampleRate").get<double>();
			media.channels = parameters.at("channels").get<int>();
			media.length = parameters.at("length").get<int64_t>();
			require(!media.path.empty(), "path must not be empty");
			require(media.sampleRate > 0.0, "sampleRate must be positive");
			requireRange(media.channels, 1, g_maxAudioChannels, "channels");
			require(media.length >= 0, "length must not be negative");
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
			require(!parameters.contains("length") || event.length > 0, "length must be positive");
			checkAudioEvent(project, event);
			event.id = project.newId();
			project.audioEvents[event.id] = event;
			Json result = {{"event", toJson(event)}};
			context.emit("audioEvent.added", result);
			return result;
		}

		Json setAudioEvent(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			AudioEvent& event = find(project.audioEvents, parameters.at("event").get<ObjectId>(), "audio event");
			AudioEvent changed = event;
			if (parameters.contains("position"))
				changed.position = position(project.trackById.at(event.track), parameters.at("position"), "position");
			changed.clip = parameters.value("clip", event.clip);
			changed.offset = parameters.value("offset", event.offset);
			changed.length = parameters.value("length", event.length);
			require(changed.length > 0, "length must be positive");
			checkAudioEvent(project, changed);
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

		Json setMidiPart(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			MidiPart& part = find(project.midiParts, parameters.at("part").get<ObjectId>(), "MIDI part");
			const time::Position newPosition = parameters.contains("position")
				? position(project.trackById.at(part.track), parameters.at("position"), "position")
				: part.position;
			const int64_t length = parameters.value("length", part.length);
			require(length > 0, "length must be positive");
			part.position = newPosition;
			part.length = length;
			part.name = parameters.value("name", part.name);
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

		Json addControllers(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			MidiPart& part = find(project.midiParts, parameters.at("part").get<ObjectId>(), "MIDI part");
			std::vector<Controller> controllers;
			const Json& items = parameters.at("controllers");
			for (size_t i = 0; i < items.size(); ++i)
			{
				const Json& item = items[i];
				const std::string name = "controllers[" + std::to_string(i) + "]";
				Controller controller;
				controller.position = item.at("position").get<time::Ticks>();
				require(controller.position >= 0, name + ".position must not be negative");
				requireRange(item.at("number").get<int64_t>(), 0, 127, name + ".number");
				requireRange(item.value("channel", int64_t{0}), 0, 15, name + ".channel");
				controller.number = item.at("number").get<uint8_t>();
				controller.channel = item.value("channel", uint8_t{0});
				controller.value =
					static_cast<uint32_t>(std::llround(unitValue(item, "value", name) * g_maxControllerValue));
				controllers.push_back(controller);
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

		Json setProject(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const double sampleRate = parameters.value("sampleRate", project.sampleRate);
			require(sampleRate > 0.0, "sampleRate must be positive");
			project.name = parameters.value("name", project.name);
			if (sampleRate != project.sampleRate)
				changeSampleRate(project, sampleRate, context);
			context.emit("project.changed", {{"name", project.name}, {"sampleRate", project.sampleRate}});
			return Json::object();
		}
	}

	ProjectModel::ProjectModel() : m_project(newProject()), m_snapshots(compile(m_project, 0))
	{
	}

	void ProjectModel::add(Engine& engine, CommandInfo info, Handler handler)
	{
		const bool query = info.query;
		engine.registerCommand(std::move(info),
			[this, query, handler = std::move(handler)](CommandContext& context)
			{
				Json result = handler(m_project, context);
				if (!query)
					m_snapshots.publish(compile(m_project, ++m_revision));
				return result;
			});
	}

	void ProjectModel::registerCommands(Engine& engine)
	{
		const Json idSchema = {{"type", "integer"}};
		const Json integerSchema = {{"type", "integer"}};
		const Json numberSchema = {{"type", "number"}};
		const Json stringSchema = {{"type", "string"}};
		const Json booleanSchema = {{"type", "boolean"}};
		const Json noResult = Json::object();
		const Json trackTypes = enumSchema({"audio", "instrument", "midi", "folder", "group", "effect"});
		const Json timeBases = enumSchema({"musical", "linear"});
		const Json color = {{"type", {"string", "null"}}};

		add(engine,
			{"project.get", 1, "The whole project: busses, tracks in order, channels, media, clips, events and parts.",
				objectSchema({}), {{"type", "object"}}, true},
			[](Project& project, CommandContext&) { return toJson(project); });
		add(engine,
			{"project.new", 1, "Replaces the project with an empty one that has a stereo input and output bus.",
				objectSchema({}), noResult, false},
			[](Project& project, CommandContext& context)
			{
				project = newProject();
				context.emit("project.reset");
				return Json::object();
			});
		add(engine,
			{"project.set", 1,
				"Changes the project's name or sample rate. Linear positions keep their time at a new sample rate.",
				objectSchema({{"name", stringSchema}, {"sampleRate", numberSchema}}), noResult, false},
			setProject);

		add(engine,
			{"bus.add", 1, "Adds an input or output bus with 1 to 16 audio channels.",
				objectSchema({{"direction", enumSchema({"input", "output"})}, {"name", stringSchema},
								 {"channels", integerSchema}},
					{"direction", "name"}),
				resultSchema({"bus"}), false},
			addBus);
		add(engine,
			{"bus.set", 1, "Renames a bus.", objectSchema({{"bus", idSchema}, {"name", stringSchema}}, {"bus"}),
				resultSchema({"bus"}), false},
			setBus);
		add(engine,
			{"bus.remove", 1, "Removes a bus; channels connected to it are disconnected.",
				objectSchema({{"bus", idSchema}}, {"bus"}), noResult, false},
			removeBus);

		add(engine,
			{"track.add", 1,
				"Adds a track and its channel. Tracks go into a folder with parent, at index among its tracks "
				"(default: "
				"last). Audio channels default to stereo; folders get a channel only with groupChannel. New channels "
				"play through the main output, or through the folder's group channel.",
				objectSchema({{"type", trackTypes}, {"name", stringSchema}, {"color", color}, {"parent", idSchema},
								 {"index", integerSchema}, {"channels", integerSchema}, {"timeBase", timeBases},
								 {"groupChannel", booleanSchema}},
					{"type"}),
				{{"type", "object"},
					{"properties", {{"track", {{"type", "object"}}}, {"channel", {{"type", {"object", "null"}}}}}},
					{"required", {"track", "channel"}}},
				false},
			addTrack);
		add(engine,
			{"track.set", 1,
				"Changes a track's name, color (#rrggbb, or null for the default) or time base. A new time base keeps "
				"its events and parts at the same time.",
				objectSchema({{"track", idSchema}, {"name", stringSchema}, {"color", color}, {"timeBase", timeBases}},
					{"track"}),
				resultSchema({"track"}), false},
			setTrack);
		add(engine,
			{"track.move", 1, "Moves a track, with what it contains, into a folder (parent, 0 for the top level).",
				objectSchema({{"track", idSchema}, {"parent", idSchema}, {"index", integerSchema}},
					{"track", "parent", "index"}),
				noResult, false},
			moveTrack);
		add(engine,
			{"track.remove", 1,
				"Removes a track with its channel, events and parts; a folder also removes the tracks inside it.",
				objectSchema({{"track", idSchema}}, {"track"}), noResult, false},
			removeTrack);

		add(engine,
			{"channel.set", 1,
				"Changes a channel's audio channels, input (an input bus) or output (an output bus or a group "
				"channel; for MIDI tracks an instrument channel). 0 disconnects.",
				objectSchema(
					{{"channel", idSchema}, {"channels", integerSchema}, {"input", idSchema}, {"output", idSchema}},
					{"channel"}),
				resultSchema({"channel"}), false},
			setChannel);

		add(engine,
			{"media.add", 1, "Adds an audio file with its sample rate, channel count and length in sample frames.",
				objectSchema({{"path", stringSchema}, {"sampleRate", numberSchema}, {"channels", integerSchema},
								 {"length", integerSchema}},
					{"path", "sampleRate", "channels", "length"}),
				resultSchema({"media"}), false},
			addMedia);
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
				"rest of the clip.",
				objectSchema({{"track", idSchema}, {"clip", idSchema}, {"position", integerSchema},
								 {"offset", integerSchema}, {"length", integerSchema}},
					{"track", "clip", "position"}),
				resultSchema({"event"}), false},
			addAudioEvent);
		add(engine,
			{"audioEvent.set", 1, "Changes an audio event's position, clip, offset or length.",
				objectSchema({{"event", idSchema}, {"clip", idSchema}, {"position", integerSchema},
								 {"offset", integerSchema}, {"length", integerSchema}},
					{"event"}),
				resultSchema({"event"}), false},
			setAudioEvent);
		add(engine,
			{"audioEvent.remove", 1, "Removes an audio event.", objectSchema({{"event", idSchema}}, {"event"}),
				noResult, false},
			removeAudioEvent);

		add(engine,
			{"midiPart.add", 1,
				"Adds a MIDI part to a MIDI or instrument track. position and length are in the track's time base.",
				objectSchema({{"track", idSchema}, {"position", integerSchema}, {"length", integerSchema},
								 {"name", stringSchema}},
					{"track", "position", "length"}),
				resultSchema({"part"}), false},
			addMidiPart);
		add(engine,
			{"midiPart.set", 1, "Changes a MIDI part's name, position or length.",
				objectSchema({{"part", idSchema}, {"name", stringSchema}, {"position", integerSchema},
								 {"length", integerSchema}},
					{"part"}),
				resultSchema({"part"}), false},
			setMidiPart);
		add(engine,
			{"midiPart.remove", 1, "Removes a MIDI part with its notes and controllers.",
				objectSchema({{"part", idSchema}}, {"part"}), noResult, false},
			removeMidiPart);

		const Json note =
			objectSchema({{"position", integerSchema}, {"length", integerSchema}, {"pitch", integerSchema},
							 {"velocity", numberSchema}, {"channel", integerSchema}},
				{"position", "length", "pitch", "velocity"});
		add(engine,
			{"note.add", 1,
				"Adds notes to a MIDI part: position from the part's start and length in ticks, pitch 0-127, velocity "
				"0-1, MIDI channel 0-15 (default 0).",
				objectSchema({{"part", idSchema}, {"notes", arraySchema(note)}}, {"part", "notes"}),
				{{"type", "object"}, {"properties", {{"notes", {{"type", "array"}}}}}, {"required", {"notes"}}}, false},
			addNotes);
		add(engine,
			{"note.remove", 1, "Removes notes from a MIDI part.",
				objectSchema({{"part", idSchema}, {"notes", arraySchema(idSchema)}}, {"part", "notes"}), noResult,
				false},
			[](Project& project, CommandContext& context)
			{ return removeFromPart(project, context, &MidiPart::notes, "notes", "note"); });

		const Json controller = objectSchema({{"position", integerSchema}, {"number", integerSchema},
												 {"value", numberSchema}, {"channel", integerSchema}},
			{"position", "number", "value"});
		add(engine,
			{"controller.add", 1,
				"Adds controller events to a MIDI part: position from the part's start in ticks, controller number "
				"0-127, value 0-1, MIDI channel 0-15 (default 0).",
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
	}
}
