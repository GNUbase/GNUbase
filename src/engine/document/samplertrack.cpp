// SPDX-License-Identifier: GPL-3.0-or-later
// Sampler tracks (#180, reference manual pp. 143-144, 710-738): instrument tracks whose channel plays the built-in
// sampler (processors::Sampler) and names the pool clip it plays (Channel::sample). Its settings are the instrument's
// parameters (parameter.set); the commands here make sampler tracks and load and normalize their samples, each one
// undo step that changes nothing if it fails (ProjectModel::addEdit), and find the sample's zero crossings.
#include "engine/document/samplertrack.h"

#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/document/snap.h"
#include "engine/engine.h"
#include "engine/processors/sampler.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		using processors::Sampler;

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		Json objectSchema(Json properties, const std::vector<std::string>& required = {})
		{
			return {{"type", "object"}, {"properties", std::move(properties)}, {"required", required},
				{"additionalProperties", false}};
		}

		// The channel of a sampler track.
		Channel& samplerChannel(Project& project, ObjectId trackId)
		{
			const auto track = project.trackById.find(trackId);
			require(track != project.trackById.end(), "no track " + std::to_string(trackId));
			const auto channel = project.channels.find(track->second.channel);
			require(channel != project.channels.end() && channel->second.instrument == "sampler",
				"track " + std::to_string(trackId) + " is no sampler track");
			return channel->second;
		}

		// Loads the section into the sampler: its clip, and its start and end as the sampler's.
		void load(Channel& channel, const SampleSection& section)
		{
			channel.sample = section.clip;
			const auto length = static_cast<double>(section.length);
			const double start = static_cast<double>(section.start) / length;
			const double end = static_cast<double>(section.end) / length;
			channel.instrumentParameters.erase(Sampler::Start);
			channel.instrumentParameters.erase(Sampler::End);
			if (start > 0.0)
				channel.instrumentParameters[Sampler::Start] = start;
			if (end < 1.0)
				channel.instrumentParameters[Sampler::End] = end;
		}

		double parameter(const Channel& channel, uint32_t id, double defaultValue)
		{
			const auto found = channel.instrumentParameters.find(id);
			return found != channel.instrumentParameters.end() ? found->second : defaultValue;
		}

		// The command's parameters with a file's clip in place of path, in the pool folder Sampler Tracks (p. 710).
		Json samplerClip(ProjectModel& model, Project& project, CommandContext& context)
		{
			const Json parameters = withSampleClip(model, project, context.parameters(), context);
			if (context.parameters().contains("path"))
				model.runEdit("clip.set", project,
					{{"clip", parameters.at("clip")}, {"folder", samplerPoolFolder(project, &context)}}, context);
			return parameters;
		}

		Json result(const Project& project, const Channel& channel, CommandContext& context)
		{
			Json json = {{"channel", toJson(channel)}};
			context.emit("channel.changed", json);
			json["track"] = toJson(project.trackById.at(channel.track));
			return json;
		}
	}

	std::string numberedTrackName(const Project& project, const std::string& prefix)
	{
		std::set<std::string> taken;
		for (const auto& entry : project.trackById)
			taken.insert(entry.second.name);
		for (int number = 1;; ++number)
		{
			const std::string digits = std::to_string(number);
			std::string name = prefix;
			name.append(digits.size() < 2 ? " 0" : " ").append(digits);
			if (taken.count(name) == 0)
				return name;
		}
	}

	SampleSection sampleSection(const Project& project, const Json& parameters)
	{
		require(parameters.contains("event") != parameters.contains("clip"), "give one of event, clip and path");
		SampleSection section;
		if (parameters.contains("event"))
		{
			const auto id = parameters.at("event").get<ObjectId>();
			const auto event = project.audioEvents.find(id);
			require(event != project.audioEvents.end(), "no audio event " + std::to_string(id));
			section.clip = event->second.clip;
			section.start = event->second.offset;
			section.end = event->second.offset + event->second.length;
		}
		else
		{
			section.clip = parameters.at("clip").get<ObjectId>();
			require(project.clips.count(section.clip) != 0, "no clip " + std::to_string(section.clip));
		}
		section.length = project.media.at(project.clips.at(section.clip).media).length;
		if (parameters.contains("clip"))
			section.end = section.length;
		section.start = parameters.value("start", section.start);
		section.end = parameters.value("end", section.end);
		require(section.start >= 0 && section.start < section.end && section.end <= section.length,
			"start and end must be within the clip, start before end");
		return section;
	}

	ObjectId topPoolFolder(Project& project, const std::string& name, CommandContext* context)
	{
		for (const auto& [id, each] : project.poolFolders)
			if (each.parent == 0 && each.name == name)
				return id;
		PoolFolder folder;
		folder.id = project.newId();
		folder.name = name;
		project.poolFolders[folder.id] = folder;
		if (context != nullptr)
			context->emit("poolFolder.added", {{"folder", toJson(folder)}});
		return folder.id;
	}

	ObjectId samplerPoolFolder(Project& project, CommandContext* context)
	{
		return topPoolFolder(project, "Sampler Tracks", context);
	}

	Json withSampleClip(ProjectModel& model, Project& project, const Json& parameters, CommandContext& context)
	{
		if (!parameters.contains("path"))
			return parameters;
		require(!parameters.contains("event") && !parameters.contains("clip"), "give one of event, clip and path");
		const Json media = model.runEdit("media.add", project, {{"path", parameters.at("path")}}, context);
		Json changed = parameters;
		changed.erase("path");
		changed["clip"] =
			model.runEdit("clip.add", project, {{"media", media.at("media").at("id")}}, context).at("clip").at("id");
		return changed;
	}

	void registerSamplerCommands(ProjectModel& model, Engine& engine)
	{
		const Json integer = {{"type", "integer"}};
		const Json number = {{"type", "number"}};
		const Json resultSchema =
			objectSchema({{"track", {{"type", "object"}}}, {"channel", {{"type", "object"}}}}, {"track", "channel"});
		const std::string sectionText =
			"The sample is a pool clip, clip, the clip of the audio event event, or an audio file, path, which goes "
			"into the pool as a clip, in the pool folder Sampler Tracks, which is made if the pool has none (p. 710), "
			"from start to end in the clip's frames (default: the event's or the whole clip).";

		model.addEdit(engine,
			{"sampler.create", 1,
				"Creates a sampler track (reference manual pp. 143-144, 711): an instrument track whose channel plays "
				"the built-in sampler, named name (default: Sampler and a number), at the end of the track list or as "
				"track.add places it with parent and index, playing into output as track.add's does, from midiInput as "
				"channel.set's (default all, p. 144). " +
					sectionText +
					" Without a sample it starts empty. The sample's start and end are the sampler's Start and End "
					"parameters, shares of the clip. Emits track.added and channel.changed, and for path media.added, "
					"clip.added, clip.changed and, when it makes the folder, poolFolder.added. Returns the track and "
					"its channel.",
				objectSchema({{"event", integer}, {"clip", integer}, {"path", {{"type", "string"}}}, {"start", integer},
					{"end", integer}, {"name", {{"type", "string"}}}, {"parent", integer}, {"index", integer},
					{"output", integer}, {"midiInput", {{"type", "string"}, {"enum", {"all", "chordPads", "none"}}}}}),
				resultSchema, false},
			"Create sampler track",
			[&model](Project& project, CommandContext& context)
			{
				const Json parameters = samplerClip(model, project, context);
				const bool sample = parameters.contains("event") || parameters.contains("clip");
				require(sample || (!parameters.contains("start") && !parameters.contains("end")),
					"start and end need event, clip or path");
				const SampleSection section = sample ? sampleSection(project, parameters) : SampleSection();
				Json track = {
					{"type", "instrument"}, {"instrument", "sampler"}, {"name", numberedTrackName(project, "Sampler")}};
				for (const char* key : {"name", "parent", "index", "output"})
					if (parameters.contains(key))
						track[key] = parameters.at(key);
				const Json added = model.runEdit("track.add", project, track, context);
				Channel& channel = project.channels.at(added.at("channel").at("id").get<ObjectId>());
				if (sample)
					load(channel, section);
				channel.midiInput = parameters.value("midiInput", channel.midiInput);
				return result(project, channel, context);
			});

		model.addEdit(engine,
			{"sampler.load", 1,
				"Loads a sample into a sampler track (reference manual p. 710; Apply Hot Swap, p. 716), in place of "
				"the one it has, while it plays. " +
					sectionText +
					" Its settings go back to their defaults, unless keepSettings (Lock Parameter Settings, p. 715) "
					"keeps those that aren't the sample's: all but the root key, start, end, the fades and the loop's "
					"start, end and crossfade. Emits channel.changed, and for path media.added, clip.added, "
					"clip.changed "
					"and, when it makes the folder, poolFolder.added. Returns the track and its channel.",
				objectSchema({{"track", integer}, {"event", integer}, {"clip", integer}, {"path", {{"type", "string"}}},
								 {"start", integer}, {"end", integer}, {"keepSettings", {{"type", "boolean"}}}},
					{"track"}),
				resultSchema, false},
			"Load sample",
			[&model](Project& project, CommandContext& context)
			{
				const Json parameters = samplerClip(model, project, context);
				Channel& channel = samplerChannel(project, parameters.at("track").get<ObjectId>());
				const SampleSection section = sampleSection(project, parameters);
				if (parameters.value("keepSettings", false))
					for (const uint32_t id : {Sampler::RootKey, Sampler::LoopStart, Sampler::LoopEnd, Sampler::FadeIn,
							 Sampler::FadeOut, Sampler::LoopCrossfade})
						channel.instrumentParameters.erase(id);
				else
					channel.instrumentParameters.clear();
				load(channel, section);
				return result(project, channel, context);
			});

		model.addEdit(engine,
			{"sampler.normalize", 1,
				"Normalize Sample (reference manual pp. 715, 735): sets the sampler's Sample gain parameter so that "
				"the highest peak between its Start and End reaches level (dBFS, default 0), within the parameter's "
				"range of -48 to 48 dB. Refused for a silent or missing sample. Emits channel.changed. Returns the "
				"track and its channel.",
				objectSchema({{"track", integer}, {"level", number}}, {"track"}), resultSchema, false},
			"Normalize sample",
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				Channel& channel = samplerChannel(project, parameters.at("track").get<ObjectId>());
				require(channel.sample != 0, "the sampler has no sample");
				const double level = parameters.value("level", 0.0);
				require(level <= 0.0 && level >= -96.0, "level must be -96 to 0 dBFS");
				const std::string& path = project.media.at(project.clips.at(channel.sample).media).path;
				const std::shared_ptr<const processors::Sample> sample = processors::sharedSample(path);
				require(sample->frames() > 0, "the sample's file " + path + " is missing or can't be read");
				const auto frames = static_cast<double>(sample->frames());
				// Reversed, the sampler counts Start and End from the sample's end.
				const bool reverse = parameter(channel, Sampler::Reverse, 0.0) >= 0.5;
				const double start = parameter(channel, Sampler::Start, 0.0);
				const double end = parameter(channel, Sampler::End, 1.0);
				const auto from = static_cast<size_t>(std::floor((reverse ? 1.0 - end : start) * frames));
				const auto to = static_cast<size_t>(std::ceil((reverse ? 1.0 - start : end) * frames));
				float peak = 0.0f;
				for (const std::vector<float>* data : {&sample->left, &sample->right})
					for (size_t i = from; i < std::min(to, data->size()); ++i)
						peak = std::max(peak, std::abs((*data)[i]));
				require(peak > 0.0f, "the sample is silent between its start and end");
				channel.instrumentParameters[Sampler::Gain] =
					std::clamp(level - (20.0 * std::log10(static_cast<double>(peak))), -48.0, 48.0);
				return result(project, channel, context);
			});

		const Json numbers = {{"type", "array"}, {"items", number}};
		model.addEdit(engine,
			{"sampler.zeroCrossings", 1,
				"Where Snap to Zero Crossing (reference manual p. 713) moves positions of a sampler track's sample, "
				"shares of it as Start, End and the loop's start and end are, counted from its end while Reverse is "
				"on: each to the nearest frame within 50 ms where the sum of the sample's channels changes sign (of "
				"the two frames there, the one nearer to zero). A position without a zero crossing near stays. Refused "
				"for a sampler without a sample. Returns positions in the same order.",
				objectSchema({{"track", integer}, {"positions", numbers}}, {"track", "positions"}),
				objectSchema({{"positions", numbers}}, {"positions"}), true},
			{},
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const Channel& channel = samplerChannel(project, parameters.at("track").get<ObjectId>());
				require(channel.sample != 0, "the sampler has no sample");
				const MediaFile& media = project.media.at(project.clips.at(channel.sample).media);
				const bool reverse = parameter(channel, Sampler::Reverse, 0.0) >= 0.5;
				const auto frames = static_cast<double>(media.length);
				Json positions = Json::array();
				for (const Json& position : parameters.at("positions"))
				{
					const double share = std::clamp(position.get<double>(), 0.0, 1.0);
					const auto frame = std::llround((reverse ? 1.0 - share : share) * frames);
					const time::Samples found = zeroCrossingFrame(media, frame, 0, media.length - 1);
					const double snapped = static_cast<double>(found) / frames;
					if (found == frame)
						positions.push_back(share);
					else
						positions.push_back(reverse ? 1.0 - snapped : snapped);
				}
				return Json{{"positions", std::move(positions)}};
			});
	}
}
