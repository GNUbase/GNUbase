// SPDX-License-Identifier: GPL-3.0-or-later
// Articulation maps (#175, reference manual pp. 1262-1292): the articulationMap.* commands, articulation map files,
// the sound slot each note plays and remote triggers. Like every command that changes the project, each is one undo
// step and changes nothing if it fails (ProjectModel::addEdit). Playback (rendersnapshot.cpp) and recording
// (recording.cpp) go through the maps.
#include "engine/midi/articulationmaps.h"

#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
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
				if (auto next = score(map, map.slots[i], active); next > bestScore)
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
				{"velocity", slot.velocity}, {"transpose", slot.transpose}};
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
			return slot;
		}

		// Checks what a map's parts say about each other: unique articulations in its groups, slots of its
		// articulations with at most one of each group.
		void checkMap(const ArticulationMap& map)
		{
			check(!map.groups.empty(), "an articulation map has at least one group");
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

		Json setMap(Project& project, CommandContext& context)
		{
			Json changes = context.parameters();
			ArticulationMap& map = findMap(project, changes);
			changes.erase("map");
			change(map, changes);
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

		// The directions that select a slot: its own, and a reset for each other group with directions.
		void selectSlot(
			const ArticulationMap& map, const SoundSlot& slot, int64_t position, const Json& take, Json& controllers)
		{
			for (size_t group = 0; group < map.groups.size(); ++group)
			{
				const Articulation* first = nullptr;
				const Articulation* own = nullptr;
				for (const Articulation& articulation : map.articulations)
					if (articulation.type == ArticulationType::Direction &&
						static_cast<size_t>(articulation.group) == group)
					{
						if (first == nullptr)
							first = &articulation;
						if (std::find(slot.articulations.begin(), slot.articulations.end(), articulation.name) !=
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

	std::map<ObjectId, size_t> soundSlots(const ArticulationMap& map, const MidiPart& part)
	{
		std::map<ObjectId, size_t> slots;
		if (map.slots.empty())
			return slots;
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
				directed.at(static_cast<size_t>(articulation->group)) = (*next)->value != 0 ? nullptr : articulation;
			}
			Active active = directed;
			for (const std::string& name : note->articulations)
				if (const Articulation* articulation = findArticulation(map, name);
					articulation != nullptr && articulation->type == ArticulationType::Attribute)
					active.at(static_cast<size_t>(articulation->group)) = articulation;
			slots[note->id] = bestSlot(map, active);
		}
		return slots;
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
		for (size_t i = 0; i < triggers.size(); ++i)
		{
			const Trigger& trigger = triggers[i];
			selectSlot(map, map.slots[trigger.slot], trigger.position, trigger.take, controllers);
			// Without latch the default slot is back where the key is released, unless another key came first.
			const bool released = trigger.end != std::numeric_limits<int64_t>::max() &&
				(i + 1 == triggers.size() || triggers[i + 1].position > trigger.end);
			if (released)
				selectSlot(map, map.slots.front(), trigger.end, trigger.take, controllers);
		}
		// The attributes of the slot selected where a note starts.
		for (Json& note : notes)
		{
			const auto position = note.at("position").get<int64_t>();
			const Trigger* selected = nullptr;
			for (const Trigger& trigger : triggers)
				if (trigger.position <= position)
					selected = &trigger;
			if (selected == nullptr || position >= selected->end)
				continue;
			Json attributes = Json::array();
			for (const std::string& name : map.slots[selected->slot].articulations)
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
					   {"length", idSchema}, {"velocity", idSchema}, {"transpose", idSchema}},
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
			"1-1000) and transpose (semitones)}; remoteTrigger, keySwitch (the default) or programChange, what selects "
			"slots in recordings; latch, whether a key switch's slot stays after the key's release (the default) "
			"instead of the default slot coming back. articulations and slots replace the map's whole lists. Tracks "
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
					mapText + " Emits articulationMap.changed and returns the map.",
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
