// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/projectmodel.h"

#include "engine/audiofile/audiofile.h"
#include "engine/audiofile/resampler.h"
#include "engine/dawproject/dawproject.h"
#include "engine/document/serialize.h"
#include "engine/dsp/timestretch.h"
#include "engine/graph/graph.h"
#include "engine/jobs/jobs.h"
#include "engine/media/backup.h"
#include "engine/processors/builtin.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
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
					{"channels", node.channels}, {"audioInputs", Json::array()}, {"eventInputs", Json::array()}});
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
			return trackTypeEntry(type).defaultName + std::string(number.size() < 2 ? " 0" : " ") + number;
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
			track.timeBase = timeBaseFromName(parameters.value("timeBase", "musical"));
			track.parent = parent;

			// New tracks play through the main output, or through the folder's group channel when added to one.
			std::optional<Channel> channel;
			if (hasChannel)
			{
				channel = Channel{project.newId(), track.id, channels, 0, 0, {}};
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
				const time::TimeBase base = timeBaseFromName(parameters.at("timeBase").get<std::string>());
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
			if (parameters.contains("instrument"))
			{
				changed.instrument = parameters.at("instrument").get<std::string>();
				require(type == TrackType::Instrument || changed.instrument.empty(),
					"only an instrument track's channel plays an instrument");
				if (!changed.instrument.empty())
				{
					const auto& builtins = processors::builtins();
					const auto info = std::find_if(builtins.begin(), builtins.end(),
						[&](const processors::BuiltinInfo& b) { return b.id == changed.instrument; });
					require(info != builtins.end(), "no built-in processor " + changed.instrument);
					require(info->instrument, changed.instrument + " is not an instrument");
				}
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

		// Copies media files into the media folder as a job, then points the media at their copies with one
		// command, a transaction named action, so that it is one undo step. Media whose path changed while the job
		// copied, or that were removed, keep their path, and their copies are removed. The job's result is
		// {copied: [{media, from, to}], missing: [{media, path}], changed: [media]}. Returns the job.
		Json startMediaCopy(Jobs& jobs, std::string kind, std::vector<MediaCopy> files, std::filesystem::path folder,
			std::string action)
		{
			return jobs.start(std::move(kind),
				[files = std::move(files), folder = std::move(folder), action = std::move(action)](
					JobContext& job) mutable
				{
					copyMedia(files, folder, job);
					Json relink = Json::array();
					Json missing = Json::array();
					std::vector<std::string> made;
					for (const MediaCopy& file : files)
					{
						if (file.to.empty())
						{
							missing.push_back({{"media", file.media}, {"path", file.from}});
							continue;
						}
						relink.push_back({{"media", file.media}, {"from", file.from}, {"path", file.to}});
						made.push_back(file.to);
					}

					std::set<ObjectId> skipped;
					try
					{
						job.checkCancelled();
						if (!relink.empty())
						{
							const Json command = {{"command", "media.relink"}, {"parameters", {{"files", relink}}}};
							const Json result = job.command(
								"history.transaction", {{"description", action}, {"commands", Json::array({command})}});
							for (const Json& media : result.at("results").at(0).at("skipped"))
								skipped.insert(media.get<ObjectId>());
						}
					}
					catch (...)
					{
						removeFiles(made);
						throw;
					}

					Json copied = Json::array();
					Json changed = Json::array();
					std::vector<std::string> unused;
					for (const MediaCopy& file : files)
					{
						if (file.to.empty())
							continue;
						if (skipped.count(file.media) != 0)
						{
							changed.push_back(file.media);
							unused.push_back(file.to);
							continue;
						}
						copied.push_back({{"media", file.media}, {"from", file.from}, {"to", file.to}});
					}
					removeFiles(unused);
					return Json{{"copied", std::move(copied)}, {"missing", std::move(missing)},
						{"changed", std::move(changed)}};
				});
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
				const AudioFileInfo info = convertSampleRate(source.path, media.path, sampleRate);
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
			changed.volume = parameters.value("volume", event.volume);
			changed.mute = parameters.value("mute", event.mute);
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
			{"bus.set", "Rename bus"},
			{"bus.remove", "Remove bus"},
			{"track.add", "Add track"},
			{"track.set", "Change track"},
			{"track.move", "Move track"},
			{"track.remove", "Remove track"},
			{"channel.set", "Change channel"},
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
			{"midiPart.add", "Add MIDI part"},
			{"midiPart.set", "Change MIDI part"},
			{"midiPart.remove", "Remove MIDI part"},
			{"note.add", "Add notes"},
			{"note.remove", "Remove notes"},
			{"note.set", "Change notes"},
			{"controller.add", "Add controllers"},
			{"controller.remove", "Remove controllers"},
			{"controller.set", "Change controllers"},
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

		// Called after the change was applied to project in the given direction.
		void announce(const Project& project, const ProjectChange& change, Direction direction, CommandContext& context)
		{
			bool reordered = false;
			if (change.settingsBefore && change.settingsAfter)
			{
				const bool redo = direction == Direction::Redo;
				const ProjectSettings& current = redo ? *change.settingsAfter : *change.settingsBefore;
				const ProjectSettings& previous = redo ? *change.settingsBefore : *change.settingsAfter;
				if (current.name != previous.name || current.sampleRate != previous.sampleRate ||
					current.tempo != previous.tempo || current.signatures != previous.signatures)
					context.emit("project.changed", {{"name", current.name}, {"sampleRate", current.sampleRate}});
				announceBusses(previous.busses, current.busses, context);
				reordered = current.tracks != previous.tracks;
			}
			for (const ObjectChange<Track>& track : change.tracks)
				reordered = reordered || !track.before || !track.after || track.before->parent != track.after->parent ||
					track.before->children != track.after->children;
			announceObjects(change.tracks, direction, "track", "track", context);
			announceObjects(change.channels, direction, "channel", "channel", context);
			announceObjects(change.media, direction, "media", "media", context);
			announceObjects(change.clips, direction, "clip", "clip", context);
			announceObjects(change.audioEvents, direction, "audioEvent", "event", context);
			announceObjects(change.markers, direction, "marker", "marker", context);
			for (const MidiPartChange& part : change.midiParts)
			{
				const auto current = project.midiParts.find(part.id);
				const bool existed = direction == Direction::Redo ? part.before.has_value() : part.after.has_value();
				if (current == project.midiParts.end())
					context.emit("midiPart.removed", {{"part", part.id}});
				else
					context.emit(existed ? "midiPart.changed" : "midiPart.added", {{"part", toJson(current->second)}});
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
		if (info.query)
		{
			engine.registerCommand(std::move(info),
				[this, handler = std::move(handler)](CommandContext& context) { return handler(m_project, context); });
			return;
		}
		std::string action = actionName(info.name);
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
				}
				catch (...)
				{
					m_project = std::move(before);
					throw;
				}
				record(action.empty() ? context.parameters().at("description").get<std::string>() : action, before,
					context);
				publish();
				return result;
			});
	}

	void ProjectModel::addControl(
		Engine& engine, CommandInfo info, std::function<Json(CommandContext& context)> handler)
	{
		engine.registerCommand(std::move(info), std::move(handler));
	}

	void ProjectModel::record(std::string action, const Project& before, CommandContext& context)
	{
		ProjectChange change = difference(before, m_project);
		if (change.empty())
			return;
		m_history.record({std::move(action), now(), {}, std::move(change)});
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
		for (int64_t i = 0; i < steps; ++i)
		{
			const HistoryStep& step = undo ? m_history.undo() : m_history.redo();
			apply(m_project, step.change, direction);
			announce(m_project, step.change, direction, context);
		}
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
				results.push_back(edit->second.handler(project, inner));
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
				"changes the project. Returns each command's result.",
				objectSchema({{"description", {{"type", "string"}}}, {"commands", arraySchema(command)}},
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
		const Json trackTypes = enumSchema({"audio", "instrument", "midi", "folder", "group", "effect"});
		const Json timeBases = enumSchema({"musical", "linear"});
		const Json color = {{"type", {"string", "null"}}};

		add(engine,
			{"project.get", 1, "The whole project: busses, tracks in order, channels, media, clips, events and parts.",
				objectSchema({}), {{"type", "object"}}, true},
			[](Project& project, CommandContext&) { return toJson(project); });
		add(engine,
			{"graph.get", 1,
				"The project's audio processing graph: its nodes (busses, then channels in track order) with the nodes "
				"they receive audio and events from, and the order they are processed in.",
				objectSchema({}), {{"type", "object"}}, true},
			[this](Project&, CommandContext&) { return graphToJson(graph::describe(m_snapshots.latest())); });
		add(engine,
			{"project.save", 1,
				"Saves the project as a DAWproject file. Media files are copied into it unless embedMedia is false; "
				"then the file refers to them where they are.",
				objectSchema({{"path", stringSchema}, {"embedMedia", booleanSchema}}, {"path"}), noResult, true},
			[this](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				DawProjectSaveOptions options;
				options.embedMedia = parameters.value("embedMedia", true);
				const std::string path = parameters.at("path").get<std::string>();
				try
				{
					saveDawProject(project, path, options);
					m_file = std::filesystem::absolute(std::filesystem::u8path(path)).u8string();
				}
				catch (const std::exception& error)
				{
					throw CommandError(error.what());
				}
				context.emit("project.saved", {{"file", m_file}});
				return Json::object();
			});
		addControl(engine,
			{"project.load", 1,
				"Replaces the project with a DAWproject file and clears the undo history. Media files embedded in "
				"it are extracted to mediaFolder, by default a folder named after the file next to it. With "
				"revision, it loads only if the project is still at that revision (see project.revision), and fails "
				"otherwise, so that no change made meanwhile is lost.",
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
				try
				{
					m_project = loadDawProject(path, folder);
					m_file = std::filesystem::absolute(file).u8string();
				}
				catch (const std::exception& error)
				{
					throw CommandError(error.what());
				}
				m_history.clear();
				context.emit("project.reset");
				announceHistory(context);
				publish();
				return Json::object();
			});
		addControl(engine,
			{"project.new", 1,
				"Replaces the project with an empty one that has a stereo input and output bus, and clears the undo "
				"history.",
				objectSchema({}), noResult, false},
			[this](CommandContext& context)
			{
				m_project = newProject();
				m_file.clear();
				m_history.clear();
				context.emit("project.reset");
				announceHistory(context);
				publish();
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
				"channel; for MIDI tracks an instrument channel). 0 disconnects. instrument is the built-in "
				"instrument an instrument track plays (see processor.list), or empty for none.",
				objectSchema({{"channel", idSchema}, {"channels", integerSchema}, {"input", idSchema},
								 {"output", idSchema}, {"instrument", stringSchema}},
					{"channel"}),
				resultSchema({"channel"}), false},
			setChannel);

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
				"plays one event at a time: where events overlap, the one added last plays.",
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
		registerHistoryCommands(engine);
		registerMediaFolderCommands(engine, jobs);
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
				objectSchema({}),
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
				"starting a project. project.load with revision loads only if it hasn't changed.",
				objectSchema({}), objectSchema({{"revision", integerSchema}}, {"revision"}), true},
			[this](CommandContext&) { return Json{{"revision", m_revision}}; });

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
						m_file.empty() ? m_project.name : std::filesystem::u8path(m_file).stem().u8string());
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
