// SPDX-License-Identifier: GPL-3.0-or-later
// Patterns of the pattern editor (#181, reference manual pp. 895-937): the pattern.* commands, pattern presets and
// the notes pattern events play. Like every command that changes the project, each is one undo step and changes
// nothing if it fails (ProjectModel::addEdit). Playback (rendersnapshot.cpp) plays a pattern event's patternNotes.
#include "engine/midi/patterns.h"

#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/midi/drummaps.h"
#include "engine/midi/midivalues.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		constexpr std::array<const char*, 3> g_modes = {"drum", "mono", "poly"};
		constexpr std::array<const char*, 3> g_directions = {"forward", "backward", "alternating"};

		Json toJson(const PatternStep& step)
		{
			return {{"index", step.index}, {"length", step.length}, {"velocity", step.velocity},
				{"repeats", step.repeats}, {"offset", step.offset}, {"probability", step.probability},
				{"velocityVariance", step.velocityVariance}, {"gate", step.gate}};
		}

		Json toJson(const PatternLane& lane)
		{
			Json steps = Json::array();
			for (const PatternStep& step : lane.steps)
				steps.push_back(toJson(step));
			return {{"pitch", lane.pitch}, {"name", lane.name}, {"mute", lane.mute}, {"solo", lane.solo},
				{"length", lane.length},
				{"resolution", lane.resolution ? Json(midi::toString(*lane.resolution)) : Json(nullptr)},
				{"direction", g_directions.at(static_cast<size_t>(lane.direction))}, {"swing", lane.swing},
				{"offset", lane.offset}, {"steps", std::move(steps)}};
		}
	}

	Json toJson(const Pattern& pattern)
	{
		Json lanes = Json::array();
		for (const PatternLane& lane : pattern.lanes)
			lanes.push_back(toJson(lane));
		return {{"id", pattern.id}, {"track", pattern.track}, {"name", pattern.name},
			{"mode", g_modes.at(static_cast<size_t>(pattern.mode))}, {"length", pattern.length},
			{"resolution", midi::toString(pattern.resolution)},
			{"direction", g_directions.at(static_cast<size_t>(pattern.direction))}, {"swing", pattern.swing},
			{"lanes", std::move(lanes)}};
	}
}

namespace gnubase::engine::midi
{
	namespace
	{
		void check(bool condition, const std::string& message)
		{
			if (!condition)
				throw std::invalid_argument(message);
		}

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		template <size_t N>
		size_t indexOf(const std::array<const char*, N>& names, const Json& json, const std::string& name)
		{
			const auto text = json.get<std::string>();
			for (size_t i = 0; i < N; ++i)
				if (text == names.at(i))
					return i;
			std::string list;
			for (const char* each : names)
				list += (list.empty() ? "" : ", ") + std::string(each);
			throw std::invalid_argument(name + " must be one of " + list);
		}

		double number(
			const Json& json, const char* key, double fallback, double low, double high, const std::string& name)
		{
			const double value = json.value(key, fallback);
			check(value >= low && value <= high,
				name + key + " must be " + std::to_string(low) + " to " + std::to_string(high));
			return value;
		}

		int whole(const Json& json, const char* key, int fallback, int low, int high, const std::string& name)
		{
			const int value = json.value(key, fallback);
			check(value >= low && value <= high,
				name + key + " must be " + std::to_string(low) + " to " + std::to_string(high));
			return value;
		}

		NoteValue resolutionFrom(const Json& json, const std::string& name)
		{
			const std::optional<NoteValue> value = noteValueFromString(json.get<std::string>());
			check(value.has_value() && value->denominator <= 64,
				name + " must be a note value from 1/1 to 1/64, such as 1/16, 1/8T or 1/4.");
			return value.value_or(NoteValue{});
		}

		PatternStep stepFromJson(const Json& json, const std::string& name)
		{
			PatternStep step;
			step.index = whole(json, "index", 0, 0, g_maxPatternSteps - 1, name);
			step.length = whole(json, "length", 1, 1, g_maxPatternSteps, name);
			step.velocity = whole(json, "velocity", step.velocity, 1, 127, name);
			step.repeats = whole(json, "repeats", 1, 1, 10, name);
			step.offset = number(json, "offset", 0.0, -0.5, 0.5, name);
			step.probability = number(json, "probability", 1.0, 0.0, 1.0, name);
			step.velocityVariance = number(json, "velocityVariance", 0.0, 0.0, 1.0, name);
			step.gate = number(json, "gate", 1.0, 0.01, 1.0, name);
			return step;
		}

		PatternLane laneFromJson(const Json& json, const std::string& name)
		{
			PatternLane lane;
			lane.pitch = whole(json, "pitch", lane.pitch, 0, 127, name);
			lane.name = json.value("name", std::string());
			lane.mute = json.value("mute", false);
			lane.solo = json.value("solo", false);
			lane.length = whole(json, "length", 0, 0, g_maxPatternSteps, name);
			if (json.contains("resolution") && !json.at("resolution").is_null())
				lane.resolution = resolutionFrom(json.at("resolution"), name + "resolution");
			if (json.contains("direction"))
				lane.direction =
					static_cast<PlayDirection>(indexOf(g_directions, json.at("direction"), name + "direction"));
			lane.swing = number(json, "swing", 0.0, -1.0, 1.0, name);
			lane.offset = number(json, "offset", 0.0, -1.0, 1.0, name);
			const Json steps = json.value("steps", Json::array());
			for (size_t i = 0; i < steps.size(); ++i)
				lane.steps.push_back(stepFromJson(steps[i], name + "steps[" + std::to_string(i) + "]."));
			std::sort(lane.steps.begin(), lane.steps.end(),
				[](const PatternStep& a, const PatternStep& b) { return a.index < b.index; });
			return lane;
		}

		int laneLength(const Pattern& pattern, const PatternLane& lane)
		{
			return lane.length > 0 ? lane.length : pattern.length;
		}

		// Each lane once, its steps on it and one at each index; in mono mode at most one step at each position,
		// counting the steps tied to it, and the lanes as long as the pattern.
		void checkPattern(const Pattern& pattern)
		{
			std::set<int> pitches;
			std::vector<bool> taken(static_cast<size_t>(pattern.length), false);
			for (size_t i = 0; i < pattern.lanes.size(); ++i)
			{
				const PatternLane& lane = pattern.lanes[i];
				const std::string name = "lanes[" + std::to_string(i) + "].";
				check(pitches.insert(lane.pitch).second,
					name + "pitch " + std::to_string(lane.pitch) + " has a lane already");
				check(pattern.mode != PatternMode::Mono || (lane.length == 0 && !lane.resolution),
					name + "length and resolution are the pattern's in mono mode");
				const int length = laneLength(pattern, lane);
				for (size_t s = 0; s < lane.steps.size(); ++s)
				{
					const PatternStep& step = lane.steps[s];
					check(step.index < length && step.length <= length,
						name + "steps[" + std::to_string(s) + "] must lie on the lane's " + std::to_string(length) +
							" steps");
					check(s == 0 || lane.steps[s - 1].index != step.index,
						name + "steps[" + std::to_string(s) + "]: a step starts at " + std::to_string(step.index) +
							" already");
					if (pattern.mode != PatternMode::Mono)
						continue;
					for (int cell = step.index; cell < step.index + step.length; ++cell)
					{
						auto at = taken.at(static_cast<size_t>(cell % length));
						check(!at, name + "steps[" + std::to_string(s) + "]: mono mode plays one step at a time");
						taken.at(static_cast<size_t>(cell % length)) = true;
					}
				}
			}
		}

		// Poly to mono mode (p. 913): the lowest step at each position stays, harmonic voices and overlapping steps
		// go, and so do the lane controls' settings.
		void toMono(Pattern& pattern)
		{
			std::sort(pattern.lanes.begin(), pattern.lanes.end(),
				[](const PatternLane& a, const PatternLane& b) { return a.pitch < b.pitch; });
			std::vector<bool> taken(static_cast<size_t>(pattern.length), false);
			for (PatternLane& lane : pattern.lanes)
			{
				lane.length = 0;
				lane.resolution.reset();
				lane.direction = PlayDirection::Forward;
				lane.swing = 0.0;
				lane.offset = 0.0;
				std::vector<PatternStep> kept;
				for (PatternStep step : lane.steps)
				{
					if (step.index >= pattern.length)
						continue;
					step.length = std::min(step.length, pattern.length);
					bool free = true;
					for (int cell = step.index; cell < step.index + step.length; ++cell)
						free = free && !taken.at(static_cast<size_t>(cell % pattern.length));
					if (!free)
						continue;
					for (int cell = step.index; cell < step.index + step.length; ++cell)
						taken.at(static_cast<size_t>(cell % pattern.length)) = true;
					kept.push_back(step);
				}
				lane.steps = std::move(kept);
			}
		}

		std::vector<Pattern*> patternsOf(Project& project, ObjectId track)
		{
			std::vector<Pattern*> patterns;
			for (auto& [id, pattern] : project.patterns)
				if (pattern.track == track)
					patterns.push_back(&pattern);
			return patterns;
		}

		Pattern& findPattern(Project& project, ObjectId id)
		{
			const auto found = project.patterns.find(id);
			require(found != project.patterns.end(), "no pattern " + std::to_string(id));
			return found->second;
		}

		// What check throws as a command's error.
		template <typename Function> auto commandChecked(Function function)
		{
			try
			{
				return function();
			}
			catch (const std::invalid_argument& error)
			{
				throw CommandError(error.what());
			}
		}

		const Track& patternTrack(const Project& project, ObjectId id)
		{
			const auto found = project.trackById.find(id);
			require(found != project.trackById.end(), "no track " + std::to_string(id));
			require(found->second.type == TrackType::Midi || found->second.type == TrackType::Instrument,
				"patterns are on MIDI and instrument tracks");
			return found->second;
		}

		void change(Pattern& pattern, const Json& parameters)
		{
			pattern = commandChecked([&] { return patternFromJson(parameters, pattern); });
		}

		Json announce(const Pattern& pattern, const char* event, CommandContext& context)
		{
			Json result = {{"pattern", toJson(pattern)}};
			context.emit(event, result);
			return result;
		}

		// A new pattern of the track (New Pattern, p. 899). Drum mode starts with the lanes of a kick, a snare and
		// closed and open hi-hats, named after the track's drum map's sounds, or General MIDI's.
		Pattern newPattern(const Project& project, ObjectId track, PatternMode mode)
		{
			Pattern pattern;
			pattern.track = track;
			pattern.mode = mode;
			int count = 1;
			for (const auto& [id, other] : project.patterns)
				count += other.track == track ? 1 : 0;
			pattern.name = "Pattern " + std::to_string(count);
			if (mode != PatternMode::Drum)
				return pattern;
			const DrumMap* map = drumMapOf(project, track);
			const DrumMap gm = gmDrumMap();
			for (const int pitch : {36, 38, 42, 46})
			{
				PatternLane lane;
				lane.pitch = pitch;
				lane.name = (map != nullptr ? *map : gm).sounds.at(static_cast<size_t>(pitch)).name;
				pattern.lanes.push_back(std::move(lane));
			}
			return pattern;
		}

		Json addPattern(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			MidiPart* part = nullptr;
			if (parameters.contains("part"))
			{
				const auto found = project.midiParts.find(parameters.at("part").get<ObjectId>());
				require(found != project.midiParts.end(), "no MIDI part " + parameters.at("part").dump());
				part = &found->second;
			}
			ObjectId track = 0;
			if (parameters.contains("track"))
				track = parameters.at("track").get<ObjectId>();
			else if (part != nullptr)
				track = part->track;
			Pattern pattern;
			if (parameters.contains("copy"))
			{
				pattern = findPattern(project, parameters.at("copy").get<ObjectId>());
				pattern.name += " Copy";
				track = track != 0 ? track : pattern.track;
			}
			else
			{
				require(track != 0, "pattern.add needs a track, a part or a copy");
				const Json mode = parameters.value("mode", Json("drum"));
				pattern = newPattern(project, track,
					commandChecked([&mode] { return static_cast<PatternMode>(indexOf(g_modes, mode, "mode")); }));
			}
			pattern.track = track;
			patternTrack(project, pattern.track);
			Json changes = parameters;
			for (const char* key : {"copy", "track", "part", "id"})
				changes.erase(key);
			if (parameters.contains("copy"))
			{
				change(pattern, changes);
			}
			else
			{
				changes.erase("mode"); // newPattern made it in that mode
				change(pattern, changes);
			}
			pattern.id = project.newId();
			project.patterns[pattern.id] = pattern;
			Json result = announce(pattern, "pattern.added", context);
			if (part != nullptr)
			{
				part->pattern = pattern.id;
				context.emit("midiPart.changed", {{"part", toJson(*part)}});
			}
			return result;
		}

		Json setPattern(Project& project, CommandContext& context)
		{
			Json changes = context.parameters();
			Pattern& pattern = findPattern(project, changes.at("pattern").get<ObjectId>());
			for (const char* key : {"pattern", "id", "track"})
				changes.erase(key);
			change(pattern, changes);
			return announce(pattern, "pattern.changed", context);
		}

		Json removePattern(Project& project, CommandContext& context)
		{
			const ObjectId id = findPattern(project, context.parameters().at("pattern").get<ObjectId>()).id;
			project.patterns.erase(id);
			for (auto it = project.midiParts.begin(); it != project.midiParts.end();)
			{
				if (it->second.pattern != id)
				{
					++it;
					continue;
				}
				context.emit("midiPart.removed", {{"part", it->first}});
				it = project.midiParts.erase(it);
			}
			context.emit("pattern.removed", {{"pattern", id}});
			return Json::object();
		}

		time::Ticks partTicks(const Project& project, const MidiPart& part)
		{
			const time::Position end{part.position.base, part.position.value + part.length};
			return time::toTicks(end, project.tempo, project.sampleRate) -
				time::toTicks(part.position, project.tempo, project.sampleRate);
		}

		// Convert Pattern Event to MIDI Part (p. 937): the part gets the notes it played and plays them from then on.
		Json convertPattern(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const auto found = project.midiParts.find(parameters.at("part").get<ObjectId>());
			require(found != project.midiParts.end(), "no MIDI part " + parameters.at("part").dump());
			MidiPart& part = found->second;
			require(part.pattern != 0, "the part isn't a pattern event");
			findPattern(project, part.pattern);
			const std::map<ObjectId, Note> played = playedNotes(project, part);
			part.notes.clear();
			for (auto [index, note] : played)
			{
				note.id = project.newId();
				part.notes[note.id] = note;
			}
			part.pattern = 0;
			part.patternOffset = 0;
			Json result = {{"part", toJson(part)}};
			context.emit("midiPart.changed", result);
			return result;
		}

		// A pattern preset (p. 932): the track's patterns, in a JSON file.
		Json exportPatterns(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const ObjectId track = patternTrack(project, parameters.at("track").get<ObjectId>()).id;
			Json patterns = Json::array();
			for (const Pattern* pattern : patternsOf(project, track))
			{
				Json json = toJson(*pattern);
				json.erase("id");
				json.erase("track");
				patterns.push_back(std::move(json));
			}
			require(!patterns.empty(), "the track has no patterns");
			const std::string path = parameters.at("path").get<std::string>();
			std::ofstream out(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
			out << Json{{"patternPreset", 1}, {"patterns", patterns}}.dump(1, '\t');
			out.close();
			require(static_cast<bool>(out), "can't write " + path);
			return Json::object();
		}

		Json importPatterns(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const ObjectId track = patternTrack(project, parameters.at("track").get<ObjectId>()).id;
			const std::string path = parameters.at("path").get<std::string>();
			std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
			require(static_cast<bool>(in), "can't open " + path);
			const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
			const Json file = Json::parse(text, nullptr, false);
			require(file.is_object() && file.contains("patternPreset") && file.value("patterns", Json()).is_array(),
				path + ": the file isn't a pattern preset");
			std::vector<Pattern> loaded;
			for (const Json& item : file.at("patterns"))
			{
				Pattern pattern;
				pattern.mode = PatternMode::Drum;
				try
				{
					// The mode first, so that the lanes are the file's, not cleared by switching modes.
					if (item.contains("mode"))
						pattern.mode = static_cast<PatternMode>(indexOf(g_modes, item.at("mode"), "mode"));
					Json fields = item;
					fields.erase("id");
					fields.erase("track");
					fields.erase("mode");
					pattern = patternFromJson(fields, pattern);
				}
				catch (const std::exception& error)
				{
					throw CommandError(path + ": " + error.what());
				}
				loaded.push_back(std::move(pattern));
			}
			Json result = {{"patterns", Json::array()}};
			for (Pattern& pattern : loaded)
			{
				pattern.track = track;
				pattern.id = project.newId();
				project.patterns[pattern.id] = pattern;
				result.at("patterns").push_back(announce(pattern, "pattern.added", context).at("pattern"));
			}
			return result;
		}
	}

	std::map<ObjectId, Note> patternNotes(const Pattern& pattern, time::Ticks from, time::Ticks length)
	{
		std::map<ObjectId, Note> notes;
		const bool solo =
			std::any_of(pattern.lanes.begin(), pattern.lanes.end(), [](const PatternLane& lane) { return lane.solo; });
		const time::Ticks to = from + length;
		for (const PatternLane& lane : pattern.lanes)
		{
			if (lane.mute || (solo && !lane.solo))
				continue;
			const int steps = laneLength(pattern, lane);
			const time::Ticks step = lane.resolution.value_or(pattern.resolution).ticks();
			const time::Ticks cycle = steps * step;
			const double swing = std::clamp(pattern.swing + lane.swing, -1.0, 1.0);
			const PlayDirection direction =
				lane.direction == PlayDirection::Forward ? pattern.direction : lane.direction;
			// Swing and offsets move a step by less than two steps: the repeats whose notes can fall between from and
			// to. Before the pattern's start (from below 0: a start sized earlier) its repeats go on.
			const time::Ticks low = from - (2 * step);
			int64_t repeat = low >= 0 ? low / cycle : 0;
			if (from < 0)
				repeat = ((low + 1) / cycle) - 1; // rounded down
			for (; repeat * cycle < to + (2 * step); ++repeat)
			{
				const bool backward = direction == PlayDirection::Backward ||
					(direction == PlayDirection::Alternating && ((repeat % 2) + 2) % 2 == 1);
				for (const PatternStep& each : lane.steps)
				{
					// Backward, a step with the steps tied to it plays where the last of them is forward.
					const int slot =
						backward ? (((steps - each.index - each.length) % steps) + steps) % steps : each.index;
					auto start = static_cast<double>((repeat * cycle) + (slot * step));
					if (slot % 2 == 1)
						start += swing * static_cast<double>(step) / 2.0;
					start += (lane.offset + each.offset) * static_cast<double>(step);
					const double sub = static_cast<double>(each.length * step) / each.repeats;
					for (int r = 0; r < each.repeats; ++r)
					{
						time::Ticks position = std::llround(start + (r * sub));
						// A pattern that starts with the event plays a step moved before it at its start.
						if (from >= 0)
							position = std::max<time::Ticks>(position, 0);
						if (position < from || position >= to)
							continue;
						Note note;
						note.id = notes.size() + 1;
						note.position = position - from;
						note.length = std::max<time::Ticks>(std::llround(sub * each.gate), 1);
						note.pitch = static_cast<uint8_t>(lane.pitch);
						note.velocity = velocityFromMidi1(static_cast<uint8_t>(each.velocity));
						note.probability = each.probability;
						note.velocityVariance = each.velocityVariance;
						notes[note.id] = note;
					}
				}
			}
		}
		return notes;
	}

	std::map<ObjectId, Note> playedNotes(const Project& project, const MidiPart& part)
	{
		const auto pattern = project.patterns.find(part.pattern);
		if (part.pattern == 0 || pattern == project.patterns.end())
			return part.notes;
		return patternNotes(pattern->second, part.patternOffset, partTicks(project, part));
	}

	void adoptPatterns(Project& project, CommandContext& context)
	{
		std::map<std::pair<ObjectId, ObjectId>, ObjectId> copies; // by the track and the pattern copied
		for (auto& [id, part] : project.midiParts)
		{
			const auto found = project.patterns.find(part.pattern);
			if (part.pattern == 0 || found == project.patterns.end() || found->second.track == part.track)
				continue;
			const ObjectId source = found->second.track;
			if (copies.count({part.track, part.pattern}) == 0)
			{
				const bool own = std::any_of(project.midiParts.begin(), project.midiParts.end(),
					[&project, track = part.track](const auto& each)
					{
						const auto played = project.patterns.find(each.second.pattern);
						return each.second.track == track && played != project.patterns.end() &&
							played->second.track == track;
					});
				std::vector<Pattern> copied;
				for (const Pattern* each : patternsOf(project, source))
					if (each->id == part.pattern || (!own && copies.count({part.track, each->id}) == 0))
						copied.push_back(*each);
				for (Pattern& copy : copied)
				{
					const ObjectId original = copy.id;
					copy.id = project.newId();
					copy.track = part.track;
					project.patterns[copy.id] = copy;
					copies[{part.track, original}] = copy.id;
					announce(copy, "pattern.added", context);
				}
			}
			part.pattern = copies.at({part.track, part.pattern});
			context.emit("midiPart.changed", {{"part", toJson(part)}});
		}
		for (auto it = project.patterns.begin(); it != project.patterns.end();)
		{
			if (project.trackById.count(it->second.track) != 0)
			{
				++it;
				continue;
			}
			context.emit("pattern.removed", {{"pattern", it->first}});
			it = project.patterns.erase(it);
		}
	}

	Pattern patternFromJson(const Json& json, Pattern pattern)
	{
		pattern.id = json.value("id", pattern.id);
		pattern.track = json.value("track", pattern.track);
		pattern.name = json.value("name", pattern.name);
		if (json.contains("mode"))
		{
			const auto mode = static_cast<PatternMode>(indexOf(g_modes, json.at("mode"), "mode"));
			// Drum and melodic mode don't convert: switching clears the pattern (p. 913).
			if ((mode == PatternMode::Drum) != (pattern.mode == PatternMode::Drum))
				pattern.lanes.clear();
			pattern.mode = mode;
			if (mode == PatternMode::Mono)
				toMono(pattern);
		}
		const int before = pattern.length;
		pattern.length = whole(json, "length", pattern.length, 1, g_maxPatternSteps, "");
		// A shorter Number of Steps drops the steps that no longer fit on the lanes that follow it and ends the ties
		// of the others at the lanes' end; lanes given with it must fit it.
		if (pattern.length < before && !json.contains("lanes"))
			for (PatternLane& lane : pattern.lanes)
			{
				if (lane.length > 0)
					continue;
				const int length = pattern.length;
				lane.steps.erase(std::remove_if(lane.steps.begin(), lane.steps.end(),
									 [length](const PatternStep& step) { return step.index >= length; }),
					lane.steps.end());
				for (PatternStep& step : lane.steps)
					step.length = std::min(step.length, length - step.index);
			}
		if (json.contains("resolution"))
			pattern.resolution = resolutionFrom(json.at("resolution"), "resolution");
		if (json.contains("direction"))
			pattern.direction = static_cast<PlayDirection>(indexOf(g_directions, json.at("direction"), "direction"));
		pattern.swing = number(json, "swing", pattern.swing, -1.0, 1.0, "");
		if (json.contains("lanes"))
		{
			pattern.lanes.clear();
			const Json& lanes = json.at("lanes");
			for (size_t i = 0; i < lanes.size(); ++i)
				pattern.lanes.push_back(laneFromJson(lanes[i], "lanes[" + std::to_string(i) + "]."));
		}
		checkPattern(pattern);
		return pattern;
	}

	void registerPatternCommands(ProjectModel& model, Engine& engine)
	{
		const Json idSchema = {{"type", "integer"}};
		const Json stringSchema = {{"type", "string"}};
		const Json numberSchema = {{"type", "number"}};
		const Json boolean = {{"type", "boolean"}};
		const Json direction = {
			{"type", "string"}, {"enum", std::vector<std::string>(g_directions.begin(), g_directions.end())}};
		const Json stepSchema = {{"type", "object"}, {"additionalProperties", false}, {"required", {"index"}},
			{"properties",
				{{"index", idSchema}, {"length", idSchema}, {"velocity", idSchema}, {"repeats", idSchema},
					{"offset", numberSchema}, {"probability", numberSchema}, {"velocityVariance", numberSchema},
					{"gate", numberSchema}}}};
		const Json laneSchema = {{"type", "object"}, {"additionalProperties", false}, {"required", {"pitch"}},
			{"properties",
				{{"pitch", idSchema}, {"name", stringSchema}, {"mute", boolean}, {"solo", boolean},
					{"length", idSchema}, {"resolution", {{"type", {"string", "null"}}}}, {"direction", direction},
					{"swing", numberSchema}, {"offset", numberSchema},
					{"steps", {{"type", "array"}, {"items", stepSchema}}}}}};
		const Json settings = {{"name", stringSchema},
			{"mode", {{"type", "string"}, {"enum", std::vector<std::string>(g_modes.begin(), g_modes.end())}}},
			{"length", idSchema}, {"resolution", stringSchema}, {"direction", direction}, {"swing", numberSchema},
			{"lanes", {{"type", "array"}, {"items", laneSchema}}}};
		const Json patternResult = {
			{"type", "object"}, {"properties", {{"pattern", {{"type", "object"}}}}}, {"required", {"pattern"}}};
		const std::string patternText =
			"A pattern (reference manual pp. 895-937) is {id, track, name, mode, length, resolution, direction, swing, "
			"lanes}, as project.get lists the project's patterns: mode drum (a lane per drum sound), mono or poly "
			"(melodic, a lane per pitch; mono plays one step at a time); length, the Number of Steps, 1-128 (default "
			"16); resolution, the Step Resolution, a note value from 1/1 to 1/64 such as 1/16, 1/8T or 1/4.; "
			"direction forward, backward or alternating; swing -1 to 1, how far every second step moves, in half "
			"steps. lanes are the step lanes, {pitch, name, mute, solo, length (0: the pattern's), resolution (null: "
			"the pattern's), direction (forward: the pattern's), swing (added to the pattern's), offset (-1 to 1 "
			"steps), steps}, one per pitch; in mono mode length and resolution are the pattern's. steps are {index "
			"(from 0), length (in steps: more ties the steps after it to it), velocity 1-127 (default 100), repeats "
			"1-10 (sub-steps), offset -0.5 to 0.5 steps, probability 0-1, velocityVariance 0-1, gate 0.01-1 (of its "
			"length)}: one starting at each index; in mono mode one step at a time. Pattern events are MIDI parts "
			"with a pattern (midiPart.add and midiPart.set pattern), which play it from their start over and over "
			"instead of their notes, each lane on its own; their patternOffset (project.get) is how many ticks of the "
			"pattern went before their start, after a split or a start sized, so that they continue it. A pattern "
			"event on another track than its pattern's, copied, moved or on a duplicated track, plays a copy on its "
			"own track: a track without pattern events gets copies of all the patterns of the pattern's track (p. "
			"936). Removing a track removes its patterns.";
		const auto object = [](Json properties, std::vector<std::string> required)
		{
			return Json{{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)},
				{"additionalProperties", false}};
		};

		Json addProperties = settings;
		addProperties["track"] = idSchema;
		addProperties["part"] = idSchema;
		addProperties["copy"] = idSchema;
		model.addEdit(engine,
			{"pattern.add", 1,
				"Adds a pattern to a MIDI or instrument track (New Pattern, p. 899): Pattern 1, 2 and so on, 16 steps "
				"of 1/16; in drum mode (the default) with lanes for a kick, a snare and closed and open hi-hats, "
				"named after the track's drum map's sounds, in melodic mode without lanes. copy duplicates a pattern "
				"instead (Duplicate Pattern), on its track unless track is given. With part, the pattern is that "
				"part's track's and the part plays it: it becomes a pattern event. Then changed by the settings "
				"given. " +
					patternText + " Emits pattern.added (and midiPart.changed with part) and returns the pattern.",
				object(addProperties, {}), patternResult, false},
			"Add pattern", addPattern);
		Json setProperties = settings;
		setProperties["pattern"] = idSchema;
		model.addEdit(engine,
			{"pattern.set", 1,
				"Changes a pattern: what isn't given stays, and lanes replace the whole list; its pattern events play "
				"the change. Switching between drum and melodic mode clears the lanes; from poly to mono mode the "
				"lowest step at each position stays, and the lanes' own settings go (p. 913). " +
					patternText + " Emits pattern.changed and returns the pattern.",
				object(setProperties, {"pattern"}), patternResult, false},
			"Change pattern", setPattern);
		model.addEdit(engine,
			{"pattern.remove", 1,
				"Removes a pattern and the pattern events that play it (Pattern Management, p. 899). Emits "
				"midiPart.removed for those and pattern.removed.",
				object({{"pattern", idSchema}}, {"pattern"}), Json::object(), false},
			"Remove pattern", removePattern);
		model.addEdit(engine,
			{"pattern.convert", 1,
				"Convert Pattern Event to MIDI Part (p. 937): the pattern event's part gets the notes its pattern "
				"played in it, with each step's probability and velocity variance, and plays them as an ordinary part "
				"from then on. Emits midiPart.changed and returns the part.",
				object({{"part", idSchema}}, {"part"}), {{"type", "object"}}, false},
			"Convert pattern event", convertPattern);
		model.addEdit(engine,
			{"pattern.export", 1,
				"Saves a track's patterns as a pattern preset (Save Preset, p. 932) at path: a JSON file with each "
				"pattern's mode, steps and settings, which pattern.import loads.",
				object({{"track", idSchema}, {"path", stringSchema}}, {"track", "path"}), Json::object(), true},
			"", exportPatterns);
		model.addEdit(engine,
			{"pattern.import", 1,
				"Loads a pattern preset (Load Preset, p. 933), as pattern.export saves it, into a MIDI or instrument "
				"track: its patterns become the track's as new patterns. Emits pattern.added for each and returns "
				"{patterns}.",
				object({{"track", idSchema}, {"path", stringSchema}}, {"track", "path"}),
				{{"type", "object"}, {"properties", {{"patterns", {{"type", "array"}}}}}, {"required", {"patterns"}}},
				false},
			"Load pattern preset", importPatterns);
	}
}
