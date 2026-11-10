// SPDX-License-Identifier: GPL-3.0-or-later
// Sampler tracks (#180, reference manual pp. 143-144, 710-738): instrument tracks whose channel plays the built-in
// sampler (processors::Sampler) and names the pool clip it plays (Channel::sample). Its settings are the instrument's
// parameters (parameter.set); the commands here make sampler tracks and load and normalize their samples. Each is one
// undo step and changes nothing if it fails (ProjectModel::addEdit).
#include "engine/document/samplertrack.h"

#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/processors/sampler.h"

#include <algorithm>
#include <cmath>
#include <memory>
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

		// The clip that the parameters name with clip or event, and the section of it to play in its frames: start
		// and end if given, else the event's or the whole clip.
		struct Section
		{
			ObjectId clip = 0;
			time::Samples start = 0;
			time::Samples end = 0;
			time::Samples length = 0; // the clip's
		};

		Section sectionOf(const Project& project, const Json& parameters)
		{
			require(parameters.contains("event") != parameters.contains("clip"), "give either event or clip");
			Section section;
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

		// Loads the section into the sampler: its clip, and its start and end as the sampler's.
		void load(Channel& channel, const Section& section)
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

		Json result(const Project& project, const Channel& channel, CommandContext& context)
		{
			Json json = {{"channel", toJson(channel)}};
			context.emit("channel.changed", json);
			json["track"] = toJson(project.trackById.at(channel.track));
			return json;
		}
	}

	void registerSamplerCommands(ProjectModel& model, Engine& engine)
	{
		const Json integer = {{"type", "integer"}};
		const Json number = {{"type", "number"}};
		const Json resultSchema =
			objectSchema({{"track", {{"type", "object"}}}, {"channel", {{"type", "object"}}}}, {"track", "channel"});
		const std::string sectionText = "The sample is a pool clip, clip, or the clip of the audio event event, from "
										"start to end in the clip's frames (default: the event's or the whole clip).";

		model.addEdit(engine,
			{"sampler.create", 1,
				"Creates a sampler track (reference manual p. 711): an instrument track at the end of the track list "
				"whose channel plays the built-in sampler, named name (default: Sampler and a number). " +
					sectionText +
					" Without either it starts empty. The sample's start and end are the sampler's Start and End "
					"parameters, shares of the clip. Emits track.added and channel.changed. Returns the track and its "
					"channel.",
				objectSchema({{"event", integer}, {"clip", integer}, {"start", integer}, {"end", integer},
					{"name", {{"type", "string"}}}}),
				resultSchema, false},
			"Create sampler track",
			[&model](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const bool sample = parameters.contains("event") || parameters.contains("clip");
				require(sample || (!parameters.contains("start") && !parameters.contains("end")),
					"start and end need event or clip");
				const Section section = sample ? sectionOf(project, parameters) : Section();
				const auto samplers = std::count_if(project.channels.begin(), project.channels.end(),
					[](const auto& entry) { return entry.second.instrument == "sampler"; });
				const std::string number = std::to_string(samplers + 1);
				const std::string name =
					parameters.value("name", "Sampler" + std::string(number.size() < 2 ? " 0" : " ") + number);
				const Json added = model.runEdit(
					"track.add", project, {{"type", "instrument"}, {"instrument", "sampler"}, {"name", name}}, context);
				Channel& channel = project.channels.at(added.at("channel").at("id").get<ObjectId>());
				if (sample)
					load(channel, section);
				return result(project, channel, context);
			});

		model.addEdit(engine,
			{"sampler.load", 1,
				"Loads a sample into a sampler track (reference manual p. 710; Apply Hot Swap, p. 716), in place of "
				"the one it has, while it plays. " +
					sectionText +
					" Its settings go back to their defaults, unless keepSettings (Lock Parameter Settings, p. 715) "
					"keeps those that aren't the sample's: all but the root key, start, end and the loop's start and "
					"end. Emits channel.changed. Returns the track and its channel.",
				objectSchema({{"track", integer}, {"event", integer}, {"clip", integer}, {"start", integer},
								 {"end", integer}, {"keepSettings", {{"type", "boolean"}}}},
					{"track"}),
				resultSchema, false},
			"Load sample",
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				Channel& channel = samplerChannel(project, parameters.at("track").get<ObjectId>());
				const Section section = sectionOf(project, parameters);
				if (parameters.value("keepSettings", false))
					for (const uint32_t id : {Sampler::RootKey, Sampler::LoopStart, Sampler::LoopEnd})
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
				const std::unique_ptr<const processors::Sample> sample =
					processors::loadSample(project.media.at(project.clips.at(channel.sample).media).path);
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
	}
}
