// SPDX-License-Identifier: GPL-3.0-or-later
// Articulation maps (#175, reference manual pp. 1262-1292): the articulationMap.* commands, articulation map files,
// the sound slot each note plays and remote triggers. Like every command that changes the project, each is one undo
// step and changes nothing if it fails (ProjectModel::addEdit). Playback (rendersnapshot.cpp) and recording
// (recording.cpp) go through the maps.
#include "engine/midi/articulationmaps.h"

#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/midi/midivalues.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace gnubase::engine::midi
{
	namespace
	{
		constexpr const char* g_fileFormat = "gnubase.articulationMap";
		constexpr std::array<const char*, 3> g_statusNames = {"note", "controller", "program"};

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		void check(bool condition, const std::string& message)
		{
			if (!condition)
				throw std::invalid_argument(message);
		}

		int integer(const Json& json, const char* key, int fallback, int minimum, int maximum, const std::string& name)
		{
			if (!json.contains(key))
				return fallback;
			const auto value = json.at(key).get<int64_t>();
			check(value >= minimum && value <= maximum,
				name + key + " must be " + std::to_string(minimum) + " to " + std::to_string(maximum));
			return static_cast<int>(value);
		}

		// An optional value: -1 for null.
		int optional(const Json& json, const char* key, int fallback, int maximum, const std::string& name)
		{
			if (!json.contains(key))
				return fallback;
			if (json.at(key).is_null())
				return -1;
			return integer(json, key, fallback, 0, maximum, name);
		}

		const Articulation* findArticulation(const ArticulationMap& map, const std::string& name)
		{
			const auto found = std::find_if(map.articulations.begin(), map.articulations.end(),
				[&name](const Articulation& articulation) { return articulation.name == name; });
			return found != map.articulations.end() ? &*found : nullptr;
		}

		// Of each group, the articulation in effect, or none.
		using Active = std::vector<const Articulation*>;

		// Whether a slot is better for what is active than another: it has the active articulations of more
		// important groups, then fewer that aren't active.
		std::pair<std::vector<bool>, int> score(const ArticulationMap& map, const SoundSlot& slot, const Active& active)
		{
			std::vector<bool> matches(active.size());
			int extra = 0;
			for (const std::string& name : slot.articulations)
			{
				const Articulation* articulation = findArticulation(map, name);
				if (articulation == nullptr)
					continue;
				const auto group = static_cast<size_t>(articulation->group);
				if (group < active.size() && active[group] == articulation)
					matches[group] = true;
				else
					++extra;
			}
			return {matches, -extra};
		}

		size_t bestSlot(const ArticulationMap& map, const Active& active)
		{
			size_t best = 0;
			auto bestScore = score(map, map.slots.front(), active);
			for (size_t i = 1; i < map.slots.size(); ++i)
				if (auto next = score(map, map.slots[i], active); !map.slots[i].addOn && next > bestScore)
				{
					best = i;
					bestScore = std::move(next);
				}
			return best;
		}

		Json outputJson(const OutputEvent& output)
		{
			return {{"status", g_statusNames.at(static_cast<size_t>(output.status))}, {"data1", output.data1},
				{"data2", output.data2}, {"off", output.off}};
		}

		OutputEvent outputFromJson(const Json& json, const std::string& name)
		{
			OutputEvent output;
			const auto status = json.value("status", std::string("note"));
			size_t index = 0;
			while (index < g_statusNames.size() && status != g_statusNames.at(index))
				++index;
			check(index < g_statusNames.size(), name + "status must be note, controller or program");
			output.status = static_cast<OutputStatus>(index);
			output.data1 = static_cast<uint8_t>(integer(json, "data1", 0, 0, 127, name));
			output.data2 = static_cast<uint8_t>(
				integer(json, "data2", output.status == OutputStatus::Note ? 100 : 0, 0, 127, name));
			output.off = json.value("off", false);
			return output;
		}

		Json slotJson(const SoundSlot& slot)
		{
			Json outputs = Json::array();
			for (const OutputEvent& output : slot.outputs)
				outputs.push_back(outputJson(output));
			return {{"name", slot.name}, {"color", slot.color}, {"articulations", slot.articulations},
				{"trigger", slot.trigger < 0 ? Json(nullptr) : Json(slot.trigger)}, {"outputs", outputs},
				{"channel", slot.channel < 0 ? Json(nullptr) : Json(slot.channel)}, {"length", slot.length},
				{"velocity", slot.velocity}, {"transpose", slot.transpose}, {"addOn", slot.addOn},
				{"attack", slot.attack}, {"separation", slot.separation}, {"minPitch", slot.minPitch},
				{"maxPitch", slot.maxPitch}, {"minVelocity", slot.minVelocity}, {"maxVelocity", slot.maxVelocity}};
		}

		SoundSlot slotFromJson(const Json& json, const std::string& name)
		{
			SoundSlot slot;
			slot.name = json.value("name", std::string());
			slot.color = json.value("color", std::string());
			check(slot.color.empty() || isColor(slot.color), name + "color must be #rrggbb or empty");
			slot.articulations = json.value("articulations", std::vector<std::string>());
			slot.trigger = optional(json, "trigger", -1, 127, name);
			const Json outputs = json.value("outputs", Json::array());
			for (size_t i = 0; i < outputs.size(); ++i)
				slot.outputs.push_back(outputFromJson(outputs[i], name + "outputs[" + std::to_string(i) + "]."));
			slot.channel = optional(json, "channel", -1, 15, name);
			slot.length = integer(json, "length", 100, 1, 1000, name);
			slot.velocity = integer(json, "velocity", 100, 1, 1000, name);
			slot.transpose = integer(json, "transpose", 0, -127, 127, name);
			slot.addOn = json.value("addOn", false);
			slot.attack = integer(json, "attack", 0, 0, 1000, name);
			slot.separation = integer(json, "separation", 0, 0, 1000, name);
			slot.minPitch = integer(json, "minPitch", 0, 0, 127, name);
			slot.maxPitch = integer(json, "maxPitch", 127, slot.minPitch, 127, name);
			slot.minVelocity = integer(json, "minVelocity", 1, 1, 127, name);
			slot.maxVelocity = integer(json, "maxVelocity", 127, slot.minVelocity, 127, name);
			return slot;
		}

		// Checks what a map's parts say about each other: unique articulations in its groups, slots of its
		// articulations with at most one of each group.
		void checkMap(const ArticulationMap& map)
		{
			check(!map.groups.empty(), "an articulation map has at least one group");
			check(map.slots.empty() || !map.slots.front().addOn, "the default slot (slots[0]) can't be an add-on");
			for (size_t i = 0; i < map.articulations.size(); ++i)
			{
				const Articulation& articulation = map.articulations[i];
				const std::string name = "articulations[" + std::to_string(i) + "]";
				check(!articulation.name.empty(), name + ".name must not be empty");
				check(articulation.group >= 0 && static_cast<size_t>(articulation.group) < map.groups.size(),
					name + ".group must be one of the map's groups");
				check(findArticulation(map, articulation.name) == &articulation,
					"the map has the articulation " + articulation.name + " twice");
			}
			for (size_t i = 0; i < map.slots.size(); ++i)
			{
				std::vector<bool> groups(map.groups.size());
				for (const std::string& articulation : map.slots[i].articulations)
				{
					const Articulation* found = findArticulation(map, articulation);
					const std::string name = "slots[" + std::to_string(i) + "]";
					if (found == nullptr)
					{
						std::string message = name;
						message += " has ";
						message += articulation;
						message += ", which isn't an articulation of the map";
						throw std::invalid_argument(message);
					}
					check(!groups.at(static_cast<size_t>(found->group)),
						name + " may have only one articulation of each group");
					groups.at(static_cast<size_t>(found->group)) = true;
				}
			}
		}

		ArticulationMap& findMap(Project& project, const Json& parameters)
		{
			const auto id = parameters.at("map").get<ObjectId>();
			const auto found = project.articulationMaps.find(id);
			require(found != project.articulationMaps.end(), "no articulation map " + std::to_string(id));
			return found->second;
		}

		void change(ArticulationMap& map, const Json& parameters)
		{
			try
			{
				map = articulationMapFromJson(parameters, map);
			}
			catch (const std::invalid_argument& error)
			{
				throw CommandError(error.what());
			}
		}

		Json announce(const ArticulationMap& map, const char* event, CommandContext& context)
		{
			Json result = {{"map", toJson(map)}};
			context.emit(event, result);
			return result;
		}

		Json addMap(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			ArticulationMap map = newArticulationMap();
			if (parameters.contains("copy"))
			{
				map = findMap(project, Json{{"map", parameters.at("copy")}});
				map.name += " Copy";
			}
			Json changes = parameters;
			changes.erase("copy");
			changes.erase("id");
			change(map, changes);
			map.id = project.newId();
			project.articulationMaps[map.id] = map;
			return announce(map, "articulationMap.added", context);
		}

		// Renames (to the new name) or removes (to "") a part's directions and its notes' attributes.
		void rename(MidiPart& part, const std::map<std::string, std::string>& renamed, CommandContext* context)
		{
			Json changed = Json::array();
			Json removed = Json::array();
			for (auto it = part.controllers.begin(); it != part.controllers.end();)
			{
				Controller& controller = it->second;
				const auto found = renamed.find(controller.articulation);
				if (controller.type != ControllerType::Articulation || found == renamed.end())
				{
					++it;
					continue;
				}
				if (found->second.empty())
				{
					removed.push_back(controller.id);
					it = part.controllers.erase(it);
					continue;
				}
				controller.articulation = found->second;
				changed.push_back(toJson(controller));
				++it;
			}
			Json notes = Json::array();
			for (auto& [id, note] : part.notes)
			{
				std::vector<std::string> kept;
				for (const std::string& name : note.articulations)
				{
					const auto found = renamed.find(name);
					if (found == renamed.end())
						kept.push_back(name);
					else if (!found->second.empty())
						kept.push_back(found->second);
				}
				if (kept == note.articulations)
					continue;
				note.articulations = std::move(kept);
				notes.push_back(toJson(note));
			}
			if (context == nullptr)
				return;
			if (!changed.empty())
				context->emit("controller.changed", {{"part", part.id}, {"controllers", changed}});
			if (!removed.empty())
				context->emit("controller.removed", {{"part", part.id}, {"controllers", removed}});
			if (!notes.empty())
				context->emit("note.changed", {{"part", part.id}, {"notes", notes}});
		}

		// Renaming or removing articulations does so in the parts of the tracks that use the map, and in their
		// shared copies and other track versions: an articulation is renamed where the list is as long as before, it
		// has a new name at its place and its old one is gone; one whose name is gone otherwise is removed.
		void followArticulations(
			Project& project, const ArticulationMap& before, const ArticulationMap& after, CommandContext& context)
		{
			std::map<std::string, std::string> renamed;
			for (size_t i = 0; i < before.articulations.size(); ++i)
			{
				const std::string& name = before.articulations[i].name;
				if (findArticulation(after, name) != nullptr)
					continue;
				const bool moved = before.articulations.size() == after.articulations.size() &&
					findArticulation(before, after.articulations[i].name) == nullptr;
				renamed[name] = moved ? after.articulations[i].name : std::string();
			}
			if (renamed.empty())
				return;
			// An inactive version's shared copies are all in that version (track-versions.md), so they are all on
			// the track; its parts change unannounced, as they aren't there to see.
			std::set<ObjectId> shared;
			for (const auto& [id, part] : project.midiParts)
				if (part.shared != 0 && articulationMapOf(project, part.track) == &after)
					shared.insert(part.shared);
			forEveryEventAndPart(
				project, [](const AudioEvent&, bool) {}, [](const AudioPart&, bool) {},
				[&](MidiPart& part, bool plays)
				{
					if (articulationMapOf(project, part.track) == &after || (plays && shared.count(part.shared) != 0))
						rename(part, renamed, plays ? &context : nullptr);
				});
		}

		Json setMap(Project& project, CommandContext& context)
		{
			Json changes = context.parameters();
			ArticulationMap& map = findMap(project, changes);
			const ArticulationMap before = map;
			changes.erase("map");
			change(map, changes);
			followArticulations(project, before, map, context);
			return announce(map, "articulationMap.changed", context);
		}

		Json removeMap(Project& project, CommandContext& context)
		{
			const ObjectId id = findMap(project, context.parameters()).id;
			project.articulationMaps.erase(id);
			for (auto& [trackId, track] : project.trackById)
				if (track.articulationMap == id)
				{
					track.articulationMap = 0;
					context.emit("track.changed", {{"track", engine::toJson(track)}});
				}
			context.emit("articulationMap.removed", {{"map", id}});
			return Json::object();
		}

		Json exportMap(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			Json file = toJson(findMap(project, parameters));
			file.erase("id");
			file["format"] = g_fileFormat;
			const std::string path = parameters.at("path").get<std::string>();
			std::ofstream out(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
			out << file.dump(1, '\t');
			out.close();
			require(static_cast<bool>(out), "can't write " + path);
			return Json::object();
		}

		// The sound slot each note of a part plays (#1498), as the sound slots lane shows it.
		Json listSoundSlots(Project& project, CommandContext& context)
		{
			const auto id = context.parameters().at("part").get<ObjectId>();
			const auto part = project.midiParts.find(id);
			require(part != project.midiParts.end(), "no MIDI part " + std::to_string(id));
			const ArticulationMap* map = articulationMapOf(project, part->second.track);
			Json notes = Json::array();
			if (map != nullptr)
				for (const auto& [note, slot] : soundSlots(*map, part->second))
					notes.push_back({{"note", note}, {"slot", slot}, {"name", map->slots.at(slot).name}});
			return {{"map", map != nullptr ? map->id : 0}, {"notes", notes}};
		}

		// Transpose All, Reassign All and Remove All of the remote triggers (pp. 1272-1275).
		Json remoteTriggers(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			ArticulationMap& map = findMap(project, parameters);
			const auto action = parameters.at("action").get<std::string>();
			std::vector<ArticulationMap*> maps = {&map};
			if (action == "transpose")
			{
				if (parameters.value("allMaps", false))
				{
					maps.clear();
					for (auto& [id, each] : project.articulationMaps)
						maps.push_back(&each);
				}
				const int semitones = parameters.value("semitones", 0);
				for (ArticulationMap* each : maps)
					for (SoundSlot& slot : each->slots)
						if (slot.trigger >= 0)
						{
							slot.trigger += semitones;
							require(slot.trigger >= 0 && slot.trigger <= 127,
								each->name + ": a remote trigger would leave 0-127");
						}
			}
			else if (action == "reassign")
			{
				// From the start key on the keys chosen (key switches), or consecutive programs; none past 127.
				const auto keys =
					map.programTriggers ? std::string("all") : parameters.value("keys", std::string("all"));
				const auto fits = [&keys](int key)
				{
					const int pitch = key % 12;
					const bool black = pitch == 1 || pitch == 3 || pitch == 6 || pitch == 8 || pitch == 10;
					return keys == "all" || black == (keys == "black");
				};
				int key = parameters.value("start", 0);
				const bool skipFirst = parameters.value("skipFirst", false);
				// Skip First Slot: the first slot keeps its trigger, which no other slot gets again (#1667).
				const int kept = skipFirst && !map.slots.empty() ? map.slots.front().trigger : -1;
				for (size_t i = skipFirst ? 1 : 0; i < map.slots.size(); ++i)
				{
					while (key <= 127 && (!fits(key) || key == kept))
						++key;
					map.slots[i].trigger = key <= 127 ? key : -1;
					++key;
				}
			}
			else
			{
				for (SoundSlot& slot : map.slots)
					slot.trigger = -1;
			}
			for (const ArticulationMap* each : maps)
				context.emit("articulationMap.changed", {{"map", toJson(*each)}});
			return Json::object();
		}

		Json importMap(Project& project, CommandContext& context)
		{
			const std::string path = context.parameters().at("path").get<std::string>();
			std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
			require(static_cast<bool>(in), "can't open " + path);
			const Json file = Json::parse(in, nullptr, false);
			require(file.is_object() && file.value("format", std::string()) == g_fileFormat,
				path + ": the file isn't an articulation map");
			ArticulationMap map = newArticulationMap();
			Json settings = file;
			settings.erase("format");
			settings.erase("id");
			try
			{
				map = articulationMapFromJson(settings, map);
			}
			catch (const std::exception& error)
			{
				throw CommandError(path + ": " + error.what());
			}
			map.id = project.newId();
			project.articulationMaps[map.id] = map;
			return announce(map, "articulationMap.added", context);
		}

		// A slot a remote trigger selected, from position (samples) to end.
		struct Trigger
		{
			int64_t position = 0;
			int64_t end = std::numeric_limits<int64_t>::max();
			size_t slot = 0;
			Json take;
		};

		int slotTriggeredBy(const ArticulationMap& map, int64_t value)
		{
			for (size_t i = 0; i < map.slots.size(); ++i)
				if (map.slots[i].trigger == value)
					return static_cast<int>(i);
			return -1;
		}

		// Per slot, the groups it has an articulation of.
		std::vector<std::vector<bool>> slotGroups(const ArticulationMap& map)
		{
			std::vector<std::vector<bool>> groups(map.slots.size(), std::vector<bool>(map.groups.size(), false));
			for (size_t i = 0; i < map.slots.size(); ++i)
				for (const std::string& name : map.slots[i].articulations)
					if (const Articulation* articulation = findArticulation(map, name); articulation != nullptr)
						groups[i].at(static_cast<size_t>(articulation->group)) = true;
			return groups;
		}

		// The directions that select a slot in the groups written: its own, else a reset, in each of them that has
		// directions; with reset only resets.
		void selectSlot(const ArticulationMap& map, const SoundSlot& slot, int64_t position, const Json& take,
			Json& controllers, const std::vector<bool>& written, bool reset = false)
		{
			for (size_t group = 0; group < map.groups.size(); ++group)
			{
				if (!written[group])
					continue;
				const Articulation* first = nullptr;
				const Articulation* own = nullptr;
				for (const Articulation& articulation : map.articulations)
					if (articulation.type == ArticulationType::Direction &&
						static_cast<size_t>(articulation.group) == group)
					{
						if (first == nullptr)
							first = &articulation;
						if (!reset &&
							std::find(slot.articulations.begin(), slot.articulations.end(), articulation.name) !=
								slot.articulations.end())
							own = &articulation;
					}
				if (first == nullptr)
					continue;
				Json direction = {{"position", position}, {"type", "articulation"},
					{"articulation", (own != nullptr ? own : first)->name}, {"reset", own == nullptr}};
				if (!take.is_null())
					direction["take"] = take;
				controllers.push_back(std::move(direction));
			}
		}
	}

	const ArticulationMap* articulationMapOf(const Project& project, ObjectId track)
	{
		const auto found = project.trackById.find(track);
		if (found == project.trackById.end() || found->second.articulationMap == 0)
			return nullptr;
		const auto map = project.articulationMaps.find(found->second.articulationMap);
		return map != project.articulationMaps.end() ? &map->second : nullptr;
	}

	namespace
	{
		// Calls each(note, active) for each note of a part by start, with the articulation of each group in effect
		// for it: its attribute, else the direction in effect where it starts.
		template <typename Each> void forEachNote(const ArticulationMap& map, const MidiPart& part, Each each)
		{
			std::vector<const Controller*> directions;
			for (const auto& [id, controller] : part.controllers)
				if (controller.type == ControllerType::Articulation)
					directions.push_back(&controller);
			std::stable_sort(directions.begin(), directions.end(),
				[](const Controller* a, const Controller* b) { return a->position < b->position; });
			std::vector<const Note*> notes;
			notes.reserve(part.notes.size());
			for (const auto& [id, note] : part.notes)
				notes.push_back(&note);
			std::stable_sort(
				notes.begin(), notes.end(), [](const Note* a, const Note* b) { return a->position < b->position; });

			Active directed(map.groups.size());
			auto next = directions.begin();
			for (const Note* note : notes)
			{
				for (; next != directions.end() && (*next)->position <= note->position; ++next)
				{
					const Articulation* articulation = findArticulation(map, (*next)->articulation);
					if (articulation == nullptr || articulation->type != ArticulationType::Direction)
						continue; // not the map's: it doesn't play (p. 1291)
					directed.at(static_cast<size_t>(articulation->group)) =
						(*next)->value != 0 ? nullptr : articulation;
				}
				Active active = directed;
				for (const std::string& name : note->articulations)
					if (const Articulation* articulation = findArticulation(map, name);
						articulation != nullptr && articulation->type == ArticulationType::Attribute)
						active.at(static_cast<size_t>(articulation->group)) = articulation;
				each(*note, active);
			}
		}
	}

	std::map<ObjectId, size_t> soundSlots(const ArticulationMap& map, const MidiPart& part)
	{
		std::map<ObjectId, size_t> slots;
		if (!map.slots.empty())
			forEachNote(
				map, part, [&](const Note& note, const Active& active) { slots[note.id] = bestSlot(map, active); });
		return slots;
	}

	std::map<ObjectId, std::vector<size_t>> addOnSlots(const ArticulationMap& map, const MidiPart& part)
	{
		std::map<ObjectId, std::vector<size_t>> slots;
		if (std::none_of(map.slots.begin(), map.slots.end(), [](const SoundSlot& slot) { return slot.addOn; }))
			return slots;
		forEachNote(map, part,
			[&](const Note& note, const Active& active)
			{
				for (size_t i = 0; i < map.slots.size(); ++i)
					if (map.slots[i].addOn && !map.slots[i].articulations.empty() &&
						std::all_of(map.slots[i].articulations.begin(), map.slots[i].articulations.end(),
							[&](const std::string& name)
							{
								const Articulation* articulation = findArticulation(map, name);
								return articulation != nullptr &&
									active.at(static_cast<size_t>(articulation->group)) == articulation;
							}))
						slots[note.id].push_back(i);
			});
		return slots;
	}

	std::map<ObjectId, size_t> soundSlots(const Json& map, const Json& part)
	{
		MidiPart parsed;
		for (const Json& json : part.value("notes", Json::array()))
		{
			Note note;
			note.id = json.at("id").get<ObjectId>();
			note.position = json.at("position").get<time::Ticks>();
			note.articulations = json.value("articulations", std::vector<std::string>());
			parsed.notes[note.id] = std::move(note);
		}
		for (const Json& json : part.value("controllers", Json::array()))
			if (json.value("type", std::string()) == "articulation")
			{
				Controller controller = controllerFromJson(json);
				controller.id = json.at("id").get<ObjectId>();
				controller.position = json.at("position").get<time::Ticks>();
				parsed.controllers[controller.id] = std::move(controller);
			}
		return soundSlots(articulationMapFromJson(map, ArticulationMap()), parsed);
	}

	uint16_t slotVelocity(const SoundSlot& slot, uint16_t velocity) noexcept
	{
		// The range's defaults, 1 and 127, leave the velocities as they are.
		const int64_t low = slot.minVelocity > 1 ? velocityFromMidi1(static_cast<uint8_t>(slot.minVelocity)) : 1;
		const int64_t high = slot.maxVelocity < 127 ? velocityFromMidi1(static_cast<uint8_t>(slot.maxVelocity))
													: std::numeric_limits<uint16_t>::max();
		return static_cast<uint16_t>(std::clamp<int64_t>((int64_t{velocity} * slot.velocity) / 100, low, high));
	}

	bool shareGroup(const ArticulationMap& map, const SoundSlot& a, const SoundSlot& b) noexcept
	{
		for (const std::string& first : a.articulations)
			if (const Articulation* one = findArticulation(map, first); one != nullptr)
				for (const std::string& second : b.articulations)
					if (const Articulation* other = findArticulation(map, second);
						other != nullptr && other->group == one->group)
						return true;
		return false;
	}

	void applyRemoteTriggers(const ArticulationMap& map, Json& notes, Json& controllers)
	{
		std::vector<Trigger> triggers;
		const auto take = [](const Json& item) { return item.value("take", Json()); };
		if (map.programTriggers)
		{
			Json kept = Json::array();
			for (Json& controller : controllers)
			{
				const int slot = controller.value("type", std::string()) == "program"
					? slotTriggeredBy(map, controller.value("number", int64_t{-1}))
					: -1;
				if (slot < 0)
					kept.push_back(std::move(controller));
				else
					triggers.push_back({controller.at("position").get<int64_t>(), std::numeric_limits<int64_t>::max(),
						static_cast<size_t>(slot), take(controller)});
			}
			controllers = std::move(kept);
		}
		else
		{
			Json kept = Json::array();
			for (Json& note : notes)
			{
				const int slot = slotTriggeredBy(map, note.value("pitch", int64_t{-1}));
				if (slot < 0)
				{
					kept.push_back(std::move(note));
					continue;
				}
				const auto position = note.at("position").get<int64_t>();
				triggers.push_back({position,
					map.latch ? std::numeric_limits<int64_t>::max() : position + note.at("length").get<int64_t>(),
					static_cast<size_t>(slot), take(note)});
			}
			notes = std::move(kept);
		}
		if (triggers.empty())
			return;
		std::stable_sort(triggers.begin(), triggers.end(),
			[](const Trigger& a, const Trigger& b) { return a.position < b.position; });
		// Per group, the triggers of the slots with an articulation of it, by position: the last one up to a position
		// holds the group there. Where that is an add-on slot that hasn't ended yet, slots that aren't add-ons leave
		// the group alone, unless they have an articulation of it (#1669), as the track does while it records
		// (MidiSequencer::triggerSlot).
		const std::vector<std::vector<bool>> groupsOf = slotGroups(map);
		std::vector<std::vector<size_t>> byGroup(map.groups.size());
		for (size_t i = 0; i < triggers.size(); ++i)
			for (size_t group = 0; group < map.groups.size(); ++group)
				if (groupsOf[triggers[i].slot][group])
					byGroup[group].push_back(i);
		// The trigger that holds a group at a position, or triggers.size() for none.
		const auto holder = [&](size_t group, int64_t position)
		{
			const std::vector<size_t>& list = byGroup[group];
			const auto after = std::upper_bound(list.begin(), list.end(), position,
				[&](int64_t at, size_t index) { return at < triggers[index].position; });
			return after == list.begin() ? triggers.size() : *std::prev(after);
		};
		// The groups a slot's directions go to at a position. The default slot coming back where a key is released
		// (back) leaves those of an add-on slot whose key is still held alone, also with an articulation of its own
		// there, as the track keeps the add-on on (#1718).
		const auto written = [&](size_t slot, int64_t position, bool back = false)
		{
			std::vector<bool> groups = groupsOf[slot];
			if (!map.slots[slot].addOn)
				for (size_t group = 0; group < groups.size(); ++group)
				{
					const size_t held = holder(group, position);
					const bool free = held == triggers.size() || !map.slots[triggers[held].slot].addOn ||
						position >= triggers[held].end;
					groups[group] = free || (groups[group] && !back);
				}
			return groups;
		};
		for (size_t i = 0; i < triggers.size(); ++i)
		{
			const Trigger& trigger = triggers[i];
			const SoundSlot& slot = map.slots[trigger.slot];
			selectSlot(map, slot, trigger.position, trigger.take, controllers, written(trigger.slot, trigger.position));
			// Without latch the default slot is back where the key is released, unless the key of another slot that
			// isn't an add-on came first.
			const auto next = std::find_if(triggers.begin() + static_cast<std::ptrdiff_t>(i + 1), triggers.end(),
				[&map](const Trigger& later) { return !map.slots[later.slot].addOn; });
			const bool released = trigger.end != std::numeric_limits<int64_t>::max() &&
				(slot.addOn || next == triggers.end() || next->position > trigger.end);
			if (!released)
				continue;
			if (!slot.addOn)
			{
				selectSlot(
					map, map.slots.front(), trigger.end, trigger.take, controllers, written(0, trigger.end, true));
				continue;
			}
			// An add-on slot's resets, in the groups it still holds.
			std::vector<bool> held = groupsOf[trigger.slot];
			for (size_t group = 0; group < held.size(); ++group)
				held[group] = held[group] && holder(group, trigger.end) == i;
			selectSlot(map, slot, trigger.end, trigger.take, controllers, held, true);
		}
		// The attributes of the slot selected where a note starts, and of the add-on slots selected there that no
		// later trigger of a slot with an articulation of their groups replaced.
		for (Json& note : notes)
		{
			const auto position = note.at("position").get<int64_t>();
			std::vector<const Trigger*> selected;
			for (const Trigger& trigger : triggers)
				if (trigger.position <= position)
				{
					const SoundSlot& slot = map.slots[trigger.slot];
					selected.erase(std::remove_if(selected.begin(), selected.end(),
									   [&](const Trigger* before)
									   {
										   // A slot of a group of an add-on slot takes its place (#1667).
										   if (!slot.addOn && !map.slots[before->slot].addOn)
											   return true;
										   for (size_t group = 0; group < map.groups.size(); ++group)
											   if (groupsOf[trigger.slot][group] && groupsOf[before->slot][group])
												   return true;
										   return false;
									   }),
						selected.end());
					selected.push_back(&trigger);
				}
			Json attributes = Json::array();
			for (const Trigger* trigger : selected)
				if (position < trigger->end)
					for (const std::string& name : map.slots[trigger->slot].articulations)
						if (const Articulation* articulation = findArticulation(map, name);
							articulation != nullptr && articulation->type == ArticulationType::Attribute)
							attributes.push_back(name);
			if (!attributes.empty())
				note["articulations"] = attributes;
		}
	}

	ArticulationMap articulationMapFromJson(const Json& json, ArticulationMap map)
	{
		map.id = json.value("id", map.id);
		map.name = json.value("name", map.name);
		map.groups = json.value("groups", map.groups);
		if (json.contains("articulations"))
		{
			map.articulations.clear();
			const Json& items = json.at("articulations");
			for (size_t i = 0; i < items.size(); ++i)
			{
				const std::string name = "articulations[" + std::to_string(i) + "].";
				Articulation articulation;
				articulation.name = items[i].value("name", std::string());
				const auto type = items[i].value("type", std::string("direction"));
				check(type == "direction" || type == "attribute", name + "type must be direction or attribute");
				articulation.type = type == "attribute" ? ArticulationType::Attribute : ArticulationType::Direction;
				articulation.group = items[i].value("group", 0);
				map.articulations.push_back(std::move(articulation));
			}
		}
		if (json.contains("slots"))
		{
			map.slots.clear();
			const Json& items = json.at("slots");
			for (size_t i = 0; i < items.size(); ++i)
				map.slots.push_back(slotFromJson(items[i], "slots[" + std::to_string(i) + "]."));
		}
		if (json.contains("remoteTrigger"))
		{
			const auto trigger = json.at("remoteTrigger").get<std::string>();
			check(trigger == "keySwitch" || trigger == "programChange",
				"remoteTrigger must be keySwitch or programChange");
			map.programTriggers = trigger == "programChange";
		}
		map.latch = json.value("latch", map.latch);
		checkMap(map);
		return map;
	}

	ArticulationMap newArticulationMap()
	{
		ArticulationMap map;
		map.name = "New Map";
		map.groups = {"Group 1"};
		return map;
	}

	void registerArticulationMapCommands(ProjectModel& model, Engine& engine)
	{
		const Json idSchema = {{"type", "integer"}};
		const Json stringSchema = {{"type", "string"}};
		const Json booleanSchema = {{"type", "boolean"}};
		const Json nullableInteger = {{"type", {"integer", "null"}}};
		const auto object = [](Json properties, std::vector<std::string> required)
		{
			return Json{{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)},
				{"additionalProperties", false}};
		};
		const Json articulationSchema =
			object({{"name", stringSchema}, {"type", {{"type", "string"}, {"enum", {"direction", "attribute"}}}},
					   {"group", idSchema}},
				{"name"});
		const Json outputSchema = object({{"status", {{"type", "string"}, {"enum", {"note", "controller", "program"}}}},
											 {"data1", idSchema}, {"data2", idSchema}, {"off", booleanSchema}},
			{});
		const Json slotSchema =
			object({{"name", stringSchema}, {"color", stringSchema},
					   {"articulations", {{"type", "array"}, {"items", stringSchema}}}, {"trigger", nullableInteger},
					   {"outputs", {{"type", "array"}, {"items", outputSchema}}}, {"channel", nullableInteger},
					   {"length", idSchema}, {"velocity", idSchema}, {"transpose", idSchema}, {"addOn", booleanSchema},
					   {"attack", idSchema}, {"separation", idSchema}, {"minPitch", idSchema}, {"maxPitch", idSchema},
					   {"minVelocity", idSchema}, {"maxVelocity", idSchema}},
				{});
		const Json settings = {{"name", stringSchema}, {"groups", {{"type", "array"}, {"items", stringSchema}}},
			{"articulations", {{"type", "array"}, {"items", articulationSchema}}},
			{"slots", {{"type", "array"}, {"items", slotSchema}}},
			{"remoteTrigger", {{"type", "string"}, {"enum", {"keySwitch", "programChange"}}}},
			{"latch", booleanSchema}};
		const Json mapResult = {
			{"type", "object"}, {"properties", {{"map", {{"type", "object"}}}}}, {"required", {"map"}}};
		const std::string mapText =
			"An articulation map (reference manual pp. 1262-1292) has a name; groups, the names of its mutual "
			"exclusion "
			"groups, the first the most important (at least one); articulations, each {name, type, group}: a "
			"direction (the default), valid from its articulation controller event (controller.add type articulation) "
			"to the next one of its group, or an attribute, valid for the notes that have it (note.add articulations), "
			"and its group's index; slots, the sound slots, the first the default: {name, color (#rrggbb or empty), "
			"articulations (by name, at most one of each group), trigger (the remote trigger, a key or a program "
			"0-127, or null), outputs (the output mapping: {status note, controller or program, data1 the pitch, "
			"controller or program, data2 the velocity or value, off: sent when the slot ends instead of when it "
			"starts}), channel (0-15, or null for the notes' own), length and velocity (percent of the notes', "
			"1-1000), transpose (semitones), addOn (an add-on slot, not the default: its output events go besides the "
			"slot "
			"of the other groups, for the notes that have all its articulations, on their channel; its MIDI modifiers "
			"do nothing), attack (Attack Compensation: ms 0-1000 the notes start earlier), separation (ms 0-1000 the "
			"output events go before the notes, but after the note before; 0: key switches one sample before, the "
			"others with the note), minPitch and maxPitch, minVelocity and maxVelocity (the ranges the notes' pitch, "
			"0-127, and velocity, 1-127, are kept in)}; remoteTrigger, keySwitch (the default) or programChange, what "
			"selects slots in recordings; latch, whether a key switch's slot stays after the key's release (the "
			"default) instead of the default slot coming back. articulations and slots replace the map's whole "
			"lists. Tracks "
			"use one with track.set articulationMap; each note plays the slot whose articulations are its "
			"attributes and the directions in effect at its start, else the closest one.";

		Json addProperties = settings;
		addProperties["copy"] = idSchema;
		model.addEdit(engine,
			{"articulationMap.add", 1,
				"Adds an articulation map to the project: Add Map (p. 1280), named New Map with one group, Group 1, "
				"and neither articulations nor slots, or Duplicate Map of the map copy (named after it, with Copy) "
				"(p. 1267), then changed by what is given. " +
					mapText + " Emits articulationMap.added and returns the map.",
				object(addProperties, {}), mapResult, false},
			"Add articulation map", addMap);
		Json setProperties = settings;
		setProperties["map"] = idSchema;
		model.addEdit(engine,
			{"articulationMap.set", 1,
				"Changes an articulation map: what isn't given stays; the changes go to every track using it. " +
					mapText +
					" An articulation renamed at its place in articulations (as long a list as before, its old name "
					"gone and the new one not the map's before) is renamed in the parts of the tracks that use the "
					"map, their directions and their notes' attributes; one left out is removed from them (emitting "
					"controller.changed, controller.removed and note.changed). Emits articulationMap.changed and "
					"returns "
					"the map.",
				object(setProperties, {"map"}), mapResult, false},
			"Change articulation map", setMap);
		model.addEdit(engine,
			{"articulationMap.remove", 1,
				"Removes an articulation map from the project (p. 1267); the tracks that used it have none. Emits "
				"articulationMap.removed and track.changed for those tracks.",
				object({{"map", idSchema}}, {"map"}), Json::object(), false},
			"Remove articulation map", removeMap);
		model.addEdit(engine,
			{"articulationMap.export", 1,
				"Saves an articulation map as an articulation map file (.articulationmap, p. 1292) at path: JSON with "
				"the map as articulationMap.set takes it, which articulationMap.import reads back.",
				object({{"map", idSchema}, {"path", stringSchema}}, {"map", "path"}), Json::object(), true},
			"", exportMap);
		model.addEdit(engine,
			{"articulationMap.import", 1,
				"Loads an articulation map file (.articulationmap, p. 1283), as articulationMap.export saves it, into "
				"the project as a new map. Emits articulationMap.added and returns the map.",
				object({{"path", stringSchema}}, {"path"}), mapResult, false},
			"Import articulation map", importMap);
		model.addEdit(engine,
			{"articulationMap.soundSlots", 1,
				"The sound slot each note of a MIDI part plays (reference manual p. 1287), as the Articulations lane's "
				"sound slots lane shows it: of each group the note's attribute, else the direction in effect at its "
				"start, and the slot with exactly those articulations, else the closest one, else the default slot. "
				"Returns map, the part's track's articulation map (0 for none), and notes, each {note, slot (the "
				"slot's index in the map's slots), name (the slot's)}, by note id; none without a map or slots.",
				object({{"part", idSchema}}, {"part"}),
				object({{"map", idSchema},
						   {"notes",
							   {{"type", "array"},
								   {"items",
									   object({{"note", idSchema}, {"slot", idSchema}, {"name", stringSchema}},
										   {"note", "slot", "name"})}}}},
					{"map", "notes"}),
				true},
			"", listSoundSlots);
		model.addEdit(engine,
			{"articulationMap.remoteTriggers", 1,
				"Changes the remote triggers of an articulation map's sound slots at once (reference manual pp. "
				"1272-1275). action transpose (Transpose All): adds semitones (negative: down; an octave is 12) to "
				"every trigger, of every map of the project with allMaps; refused if one would leave 0-127; semitones "
				"is -127 to 127. action reassign (Reassign All): the slots in order get the keys from start on, of "
				"keys all (the default), white or black (Key Mapping, key switches only; program changes are "
				"consecutive), skipping the default slot with skipFirst, which keeps its trigger and no other slot "
				"gets it, and no trigger past 127. action remove (Remove All): no slot has a "
				"trigger. Emits articulationMap.changed for each map it changes.",
				object(
					{{"map", idSchema}, {"action", {{"type", "string"}, {"enum", {"transpose", "reassign", "remove"}}}},
						{"semitones", {{"type", "integer"}, {"minimum", -127}, {"maximum", 127}}},
						{"allMaps", booleanSchema}, {"start", {{"type", "integer"}, {"minimum", 0}, {"maximum", 127}}},
						{"keys", {{"type", "string"}, {"enum", {"all", "white", "black"}}}},
						{"skipFirst", booleanSchema}},
					{"map", "action"}),
				Json::object(), false},
			"Change remote triggers", remoteTriggers);
	}
}

namespace gnubase::engine
{
	Json toJson(const ArticulationMap& map)
	{
		Json articulations = Json::array();
		for (const Articulation& articulation : map.articulations)
			articulations.push_back({{"name", articulation.name},
				{"type", articulation.type == ArticulationType::Attribute ? "attribute" : "direction"},
				{"group", articulation.group}});
		Json slots = Json::array();
		for (const SoundSlot& slot : map.slots)
			slots.push_back(midi::slotJson(slot));
		return {{"id", map.id}, {"name", map.name}, {"groups", map.groups}, {"articulations", articulations},
			{"slots", slots}, {"remoteTrigger", map.programTriggers ? "programChange" : "keySwitch"},
			{"latch", map.latch}};
	}
}
