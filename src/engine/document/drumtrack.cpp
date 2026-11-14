// SPDX-License-Identifier: GPL-3.0-or-later
// Drum tracks (#182, reference manual pp. 145-146, 938-980): instrument tracks whose channel plays the built-in drum
// machine (processors::DrumMachine) and names its pads and the pool clips they play (Channel::pads). The pads'
// settings are the instrument's parameters (parameter.set); the commands here make drum tracks and load, rename, copy
// and reset pads. Each is one undo step and changes nothing if it fails (ProjectModel::addEdit). The step lanes of the
// track's drum patterns follow the pads' names (synchronization with the pattern editor, p. 938).
#include "engine/document/drumtrack.h"

#include "engine/document/projectmodel.h"
#include "engine/document/samplertrack.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/midi/drummaps.h"
#include "engine/midi/patterns.h"
#include "engine/processors/drummachine.h"

#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace gnubase::engine
{
	namespace
	{
		using processors::DrumMachine;
		using processors::g_drumPads;
		using processors::g_drumPadStride;
		using Layer = DrumMachine::Layer;

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

		Channel& drumChannel(Project& project, ObjectId trackId)
		{
			const auto track = project.trackById.find(trackId);
			require(track != project.trackById.end(), "no track " + std::to_string(trackId));
			const auto channel = project.channels.find(track->second.channel);
			require(channel != project.channels.end() && channel->second.instrument == "drumMachine",
				"track " + std::to_string(trackId) + " is no drum track");
			return channel->second;
		}

		int padOf(const Json& parameters, const char* key)
		{
			const int pad = parameters.at(key).get<int>();
			require(pad >= 0 && pad < g_drumPads, std::string(key) + " must be 0 to 127");
			return pad;
		}

		// Removes a pad's parameters, which go back to their defaults.
		void clearParameters(Channel& channel, int pad)
		{
			const uint32_t first = DrumMachine::padParameter(pad, DrumMachine::Instrument);
			channel.instrumentParameters.erase(channel.instrumentParameters.lower_bound(first),
				channel.instrumentParameters.lower_bound(first + g_drumPadStride));
		}

		// The default kit of a new drum track (p. 942), on the General MIDI notes that new drum patterns start with
		// and more: a pad and the settings that aren't the defaults.
		struct KitPad
		{
			int pad = 0;
			Layer layer = Layer::Off;
			std::map<DrumMachine::PadParameter, double> settings;
		};

		const std::vector<KitPad>& defaultKit()
		{
			using D = DrumMachine;
			static const std::vector<KitPad> kit = {
				{36, Layer::Kick, {{D::Decay, 450.0}, {D::Noise, 30.0}, {D::Level, -3.0}}},
				{38, Layer::Snare, {{D::Decay, 220.0}, {D::Noise, 60.0}}},
				{39, Layer::Clap, {{D::Decay, 250.0}}},
				{42, Layer::HiHat, {{D::Decay, 70.0}, {D::Noise, 60.0}, {D::ExclusiveGroup, 1.0}, {D::Level, -6.0}}},
				{44, Layer::HiHat,
					{{D::Decay, 120.0}, {D::Noise, 60.0}, {D::Tone, 30.0}, {D::ExclusiveGroup, 1.0}, {D::Level, -8.0}}},
				{45, Layer::Tom, {{D::Tune, -5.0}, {D::Decay, 450.0}}},
				{46, Layer::HiHat, {{D::Decay, 500.0}, {D::Noise, 60.0}, {D::ExclusiveGroup, 1.0}, {D::Level, -6.0}}},
				{48, Layer::Tom, {{D::Decay, 400.0}}},
				{49, Layer::Cymbal, {{D::Decay, 1500.0}, {D::Noise, 50.0}, {D::Level, -8.0}}},
				{50, Layer::Tom, {{D::Tune, 4.0}, {D::Decay, 350.0}}},
				{56, Layer::Cowbell, {{D::Decay, 300.0}, {D::Noise, 0.0}, {D::Level, -6.0}}},
			};
			return kit;
		}

		// Renames the lanes of the track's drum patterns that play the pad after it, emitting pattern.changed.
		void syncLanes(Project& project, const Channel& channel, int pad, CommandContext& context)
		{
			const std::string name = drumPadName(project, channel, pad);
			for (auto& [id, pattern] : project.patterns)
			{
				if (pattern.track != channel.track || pattern.mode != PatternMode::Drum)
					continue;
				bool changed = false;
				for (PatternLane& lane : pattern.lanes)
					if (lane.pitch == pad && lane.name != name)
					{
						lane.name = name;
						changed = true;
					}
				if (changed)
					context.emit("pattern.changed", {{"pattern", toJson(pattern)}});
			}
		}

		Json result(const Project& project, const Channel& channel, CommandContext& context)
		{
			Json json = {{"channel", toJson(channel)}};
			context.emit("channel.changed", json);
			json["track"] = toJson(project.trackById.at(channel.track));
			return json;
		}
	}

	std::vector<int> playingDrumPads(const Channel& channel)
	{
		std::vector<int> pads;
		for (int pad = 0; pad < g_drumPads; ++pad)
		{
			const auto layer =
				channel.instrumentParameters.find(DrumMachine::padParameter(pad, DrumMachine::Instrument));
			if (layer != channel.instrumentParameters.end() && layer->second >= 0.5)
				pads.push_back(pad);
		}
		return pads;
	}

	std::string drumPadName(const Project& project, const Channel& channel, int pad)
	{
		if (const auto found = channel.pads.find(pad); found != channel.pads.end())
		{
			if (!found->second.name.empty())
				return found->second.name;
			if (const auto clip = project.clips.find(found->second.sample); clip != project.clips.end())
				return clip->second.name;
		}
		return midi::gmDrumMap().sounds.at(static_cast<size_t>(pad)).name;
	}

	const Channel* drumChannelOf(const Project& project, ObjectId track)
	{
		const auto found = project.trackById.find(track);
		if (found == project.trackById.end())
			return nullptr;
		const auto channel = project.channels.find(found->second.channel);
		return channel != project.channels.end() && channel->second.instrument == "drumMachine" ? &channel->second
																								: nullptr;
	}

	void registerDrumCommands(ProjectModel& model, Engine& engine)
	{
		const Json integer = {{"type", "integer"}};
		const Json pad = {{"type", "integer"}, {"minimum", 0}, {"maximum", g_drumPads - 1}};
		const Json resultSchema =
			objectSchema({{"track", {{"type", "object"}}}, {"channel", {{"type", "object"}}}}, {"track", "channel"});

		model.addEdit(engine,
			{"drum.create", 1,
				"Creates a drum track (Add Drum Track, reference manual pp. 145-146): an instrument track whose "
				"channel plays the built-in drum machine, named name (default: Drum and a number), at the end of the "
				"track list or as track.add places it with parent and index, playing into output as track.add's "
				"does. Its pads start with a kit of percussion synths on the General MIDI notes (kick 36, snare 38, "
				"clap 39, hi-hats 42, 44 and 46 in exclusive group 1, toms 45, 48 and 50, crash 49, cowbell 56), "
				"named after the General MIDI sounds, or all off with kit false. Emits track.added and "
				"channel.changed. Returns the track and its channel.",
				objectSchema({{"name", {{"type", "string"}}}, {"parent", integer}, {"index", integer},
					{"output", integer}, {"kit", {{"type", "boolean"}}}}),
				resultSchema, false},
			"Create drum track",
			[&model](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				Json track = {{"type", "instrument"}, {"instrument", "drumMachine"},
					{"name", numberedTrackName(project, "Drum")}};
				for (const char* key : {"name", "parent", "index", "output"})
					if (parameters.contains(key))
						track[key] = parameters.at(key);
				const Json added = model.runEdit("track.add", project, track, context);
				Channel& channel = project.channels.at(added.at("channel").at("id").get<ObjectId>());
				if (parameters.value("kit", true))
					for (const KitPad& kitPad : defaultKit())
					{
						channel.instrumentParameters[DrumMachine::padParameter(kitPad.pad, DrumMachine::Instrument)] =
							static_cast<double>(kitPad.layer);
						for (const auto& [parameter, value] : kitPad.settings)
							channel.instrumentParameters[DrumMachine::padParameter(kitPad.pad, parameter)] = value;
						channel.pads[kitPad.pad].name = drumPadName(project, channel, kitPad.pad);
					}
				return result(project, channel, context);
			});

		model.addEdit(engine,
			{"drum.loadPad", 1,
				"Loads a sample onto a pad of a drum track (reference manual pp. 942, 944): a pool clip, clip, the "
				"clip of the audio event event, or an audio file, path, which goes into the pool as a clip, from "
				"start to end in the clip's frames (default: the event's or the whole clip). The pad then plays the "
				"sample (its instrument parameter is Sample) from its start to its end, shares of the clip; its "
				"other settings go back to their defaults, unless keepSettings keeps them (Apply Hot Swap, p. 941). "
				"Lanes of the track's drum patterns that play the pad take its name. Emits channel.changed. Returns "
				"the track and its channel.",
				objectSchema({{"track", integer}, {"pad", pad}, {"event", integer}, {"clip", integer},
								 {"path", {{"type", "string"}}}, {"start", integer}, {"end", integer},
								 {"keepSettings", {{"type", "boolean"}}}},
					{"track", "pad"}),
				resultSchema, false},
			"Load drum pad",
			[&model](Project& project, CommandContext& context)
			{
				const Json parameters = withSampleClip(model, project, context.parameters(), context);
				Channel& channel = drumChannel(project, parameters.at("track").get<ObjectId>());
				const int note = padOf(parameters, "pad");
				const SampleSection section = sampleSection(project, parameters);
				const auto id = [note](DrumMachine::PadParameter parameter)
				{ return DrumMachine::padParameter(note, parameter); };
				if (!parameters.value("keepSettings", false))
					clearParameters(channel, note);
				channel.instrumentParameters.erase(id(DrumMachine::Start));
				channel.instrumentParameters.erase(id(DrumMachine::End));
				channel.instrumentParameters[id(DrumMachine::Instrument)] = static_cast<double>(Layer::Sample);
				const auto length = static_cast<double>(section.length);
				if (section.start > 0)
					channel.instrumentParameters[id(DrumMachine::Start)] = static_cast<double>(section.start) / length;
				if (section.end < section.length)
					channel.instrumentParameters[id(DrumMachine::End)] = static_cast<double>(section.end) / length;
				channel.pads[note].sample = section.clip;
				syncLanes(project, channel, note, context);
				return result(project, channel, context);
			});

		model.addEdit(engine,
			{"drum.setPad", 1,
				"Renames a pad of a drum track (Rename Pad, reference manual p. 943); an empty name goes back to its "
				"sample's clip's name, or the General MIDI sound's of its note. Lanes of the track's drum patterns "
				"that play the pad take the name (pattern.changed). Emits channel.changed. Returns the track and its "
				"channel.",
				objectSchema(
					{{"track", integer}, {"pad", pad}, {"name", {{"type", "string"}}}}, {"track", "pad", "name"}),
				resultSchema, false},
			"Rename drum pad",
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				Channel& channel = drumChannel(project, parameters.at("track").get<ObjectId>());
				const int note = padOf(parameters, "pad");
				channel.pads[note].name = parameters.at("name").get<std::string>();
				if (channel.pads[note] == DrumPad())
					channel.pads.erase(note);
				syncLanes(project, channel, note, context);
				return result(project, channel, context);
			});

		model.addEdit(engine,
			{"drum.copyPad", 1,
				"Copies a pad of a drum track onto another, to (Copy Pad and Paste Pad, reference manual p. 943): its "
				"name, its sample and all its settings, in place of the other's. With move the pad is reset "
				"afterwards (Cut Pad). Lanes of the track's drum patterns that play either pad take its name. Emits "
				"channel.changed. Returns the track and its channel.",
				objectSchema({{"track", integer}, {"pad", pad}, {"to", pad}, {"move", {{"type", "boolean"}}}},
					{"track", "pad", "to"}),
				resultSchema, false},
			"Copy drum pad",
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				Channel& channel = drumChannel(project, parameters.at("track").get<ObjectId>());
				const int from = padOf(parameters, "pad");
				const int to = padOf(parameters, "to");
				if (from == to)
					return result(project, channel, context);
				clearParameters(channel, to);
				const uint32_t first = DrumMachine::padParameter(from, DrumMachine::Instrument);
				std::map<uint32_t, double> copied;
				for (auto it = channel.instrumentParameters.lower_bound(first);
					it != channel.instrumentParameters.end() && it->first < first + g_drumPadStride; ++it)
					copied[it->first - first + DrumMachine::padParameter(to, DrumMachine::Instrument)] = it->second;
				channel.instrumentParameters.insert(copied.begin(), copied.end());
				if (const auto source = channel.pads.find(from); source != channel.pads.end())
					channel.pads[to] = source->second;
				else
					channel.pads.erase(to);
				if (parameters.value("move", false))
				{
					clearParameters(channel, from);
					channel.pads.erase(from);
				}
				syncLanes(project, channel, from, context);
				syncLanes(project, channel, to, context);
				return result(project, channel, context);
			});

		model.addEdit(engine,
			{"drum.resetPad", 1,
				"Resets a pad of a drum track, pad, or all of them without it (Reset Pad and Reset All Pads, "
				"reference manual p. 943): no name, no sample, and every setting at its default, so that it plays "
				"nothing. The global settings stay. Lanes of the track's drum patterns that play a pad reset take the "
				"General MIDI sound's name. Emits channel.changed. Returns the track and its channel.",
				objectSchema({{"track", integer}, {"pad", pad}}, {"track"}), resultSchema, false},
			"Reset drum pad",
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				Channel& channel = drumChannel(project, parameters.at("track").get<ObjectId>());
				std::vector<int> pads;
				if (parameters.contains("pad"))
					pads.push_back(padOf(parameters, "pad"));
				else
					for (int note = 0; note < g_drumPads; ++note)
						pads.push_back(note);
				for (const int note : pads)
				{
					clearParameters(channel, note);
					channel.pads.erase(note);
					syncLanes(project, channel, note, context);
				}
				return result(project, channel, context);
			});
	}
}
