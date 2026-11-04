// SPDX-License-Identifier: GPL-3.0-or-later
// Drum maps (#129, reference manual pp. 1235-1240): the GM Map, the drumMap.* commands and drum map files. Like
// every command that changes the project, each is one undo step and changes nothing if it fails
// (ProjectModel::addEdit). Playback (rendersnapshot.cpp) and input (editors.cpp, recording.cpp) go through the maps.
#include "engine/midi/drummaps.h"

#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::engine::midi
{
	namespace
	{
		constexpr int g_firstGmSound = 35;
		// General MIDI's percussion sounds, from pitch 35 (B0) to 81 (A4).
		constexpr std::array<const char*, 47> g_gmSounds = {"Acoustic Bass Drum", "Bass Drum 1", "Side Stick",
			"Acoustic Snare", "Hand Clap", "Electric Snare", "Low Floor Tom", "Closed Hi-Hat", "High Floor Tom",
			"Pedal Hi-Hat", "Low Tom", "Open Hi-Hat", "Low-Mid Tom", "Hi-Mid Tom", "Crash Cymbal 1", "High Tom",
			"Ride Cymbal 1", "Chinese Cymbal", "Ride Bell", "Tambourine", "Splash Cymbal", "Cowbell", "Crash Cymbal 2",
			"Vibraslap", "Ride Cymbal 2", "Hi Bongo", "Low Bongo", "Mute Hi Conga", "Open Hi Conga", "Low Conga",
			"High Timbale", "Low Timbale", "High Agogo", "Low Agogo", "Cabasa", "Maracas", "Short Whistle",
			"Long Whistle", "Short Guiro", "Long Guiro", "Claves", "Hi Wood Block", "Low Wood Block", "Mute Cuica",
			"Open Cuica", "Mute Triangle", "Open Triangle"};
		constexpr int g_gmChannel = 9; // MIDI channel 10

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		// Every I-note and O-note its own pitch, in pitch order from the top.
		DrumMap identityMap()
		{
			DrumMap map;
			for (int pitch = 0; pitch < g_drumSounds; ++pitch)
			{
				const auto index = static_cast<size_t>(pitch);
				map.sounds.at(index).iNote = static_cast<uint8_t>(pitch);
				map.sounds.at(index).oNote = static_cast<uint8_t>(pitch);
				map.order.at(index) = static_cast<uint8_t>(pitch);
			}
			return map;
		}

		DrumMap& findMap(Project& project, const Json& parameters)
		{
			const auto id = parameters.at("map").get<ObjectId>();
			const auto found = project.drumMaps.find(id);
			require(found != project.drumMaps.end(), "no drum map " + std::to_string(id));
			return found->second;
		}

		// Applies drumMap.set's or drumMap.add's name, sounds and order to a map.
		void change(DrumMap& map, const Json& parameters)
		{
			try
			{
				map = drumMapFromJson(parameters, map);
			}
			catch (const std::invalid_argument& error)
			{
				throw CommandError(error.what());
			}
		}

		Json announce(const DrumMap& map, const char* event, CommandContext& context)
		{
			Json result = {{"map", toJson(map)}};
			context.emit(event, result);
			return result;
		}

		Json addMap(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			DrumMap map = emptyDrumMap();
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
			project.drumMaps[map.id] = map;
			return announce(map, "drumMap.added", context);
		}

		Json setMap(Project& project, CommandContext& context)
		{
			Json changes = context.parameters();
			DrumMap& map = findMap(project, changes);
			changes.erase("map");
			change(map, changes);
			return announce(map, "drumMap.changed", context);
		}

		Json removeMap(Project& project, CommandContext& context)
		{
			const ObjectId id = findMap(project, context.parameters()).id;
			project.drumMaps.erase(id);
			for (auto& [trackId, track] : project.trackById)
				if (track.drumMap == id)
				{
					track.drumMap = 0;
					context.emit("track.changed", {{"track", toJson(track)}});
				}
			context.emit("drumMap.removed", {{"map", id}});
			return Json::object();
		}

		Json exportMap(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const DrumMap& map = findMap(project, parameters);
			const std::string path = parameters.at("path").get<std::string>();
			std::ofstream out(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
			out << writeDrumMapFile(map);
			out.close();
			require(static_cast<bool>(out), "can't write " + path);
			return Json::object();
		}

		Json importMap(Project& project, CommandContext& context)
		{
			const std::string path = context.parameters().at("path").get<std::string>();
			std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
			require(static_cast<bool>(in), "can't open " + path);
			const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
			DrumMap map;
			try
			{
				map = readDrumMapFile(text);
			}
			catch (const std::runtime_error& error)
			{
				throw CommandError(path + ": " + error.what());
			}
			map.id = project.newId();
			project.drumMaps[map.id] = map;
			return announce(map, "drumMap.added", context);
		}

		pugi::xml_node named(const pugi::xml_node& parent, const char* element, const char* name)
		{
			for (const pugi::xml_node child : parent.children(element))
				if (std::string(child.attribute("name").as_string()) == name)
					return child;
			return {};
		}

		void addValue(pugi::xml_node parent, const char* element, const char* name, const std::string& value)
		{
			pugi::xml_node node = parent.append_child(element);
			node.append_attribute("name") = name;
			node.append_attribute("value") = value.c_str();
		}

		pugi::xml_node addList(pugi::xml_node parent, const char* name, const char* type)
		{
			pugi::xml_node list = parent.append_child("list");
			list.append_attribute("name") = name;
			list.append_attribute("type") = type;
			return list;
		}
	}

	DrumMap gmDrumMap()
	{
		DrumMap map = identityMap();
		map.name = "GM Map";
		for (size_t i = 0; i < g_gmSounds.size(); ++i)
			map.sounds.at(static_cast<size_t>(g_firstGmSound) + i).name = g_gmSounds.at(i);
		for (DrumSound& sound : map.sounds)
			sound.channel = g_gmChannel;
		return map;
	}

	DrumMap emptyDrumMap()
	{
		DrumMap map = identityMap();
		map.name = "Empty Map";
		for (size_t pitch = 0; pitch < map.sounds.size(); ++pitch)
			map.sounds.at(pitch).name = "Sound " + std::to_string(pitch + 1);
		return map;
	}

	const DrumMap* drumMapOf(const Project& project, ObjectId track)
	{
		const auto found = project.trackById.find(track);
		if (found == project.trackById.end() || found->second.drumMap == 0)
			return nullptr;
		const auto map = project.drumMaps.find(found->second.drumMap);
		return map != project.drumMaps.end() ? &map->second : nullptr;
	}

	uint8_t inputPitch(const Project& project, ObjectId track, uint8_t played)
	{
		if (const DrumMap* map = drumMapOf(project, track))
			for (size_t pitch = 0; pitch < map->sounds.size(); ++pitch)
				if (map->sounds.at(pitch).iNote == played)
					return static_cast<uint8_t>(pitch);
		return played;
	}

	// <DrumMap> with the name, the snap values the sounds use (Quantize: Grid, the note value's denominator, and
	// Type 0 straight, 1 triplet, 2 dotted), the sounds by pitch (Map), the drum sound list's order and the outputs
	// the sounds' PortIndex names, the first being the track's (Default).
	std::string writeDrumMapFile(const DrumMap& map)
	{
		pugi::xml_document document;
		pugi::xml_node root = document.append_child("DrumMap");
		addValue(root, "string", "Name", map.name);
		std::vector<NoteValue> snaps;
		std::vector<std::string> outputs = {""};
		for (const DrumSound& sound : map.sounds)
		{
			if (std::find(snaps.begin(), snaps.end(), sound.snap) == snaps.end())
				snaps.push_back(sound.snap);
			if (std::find(outputs.begin(), outputs.end(), sound.output) == outputs.end())
				outputs.push_back(sound.output);
		}
		pugi::xml_node quantize = addList(root, "Quantize", "list");
		for (const NoteValue& snap : snaps)
		{
			pugi::xml_node item = quantize.append_child("item");
			addValue(item, "int", "Grid", std::to_string(snap.denominator));
			addValue(item, "int", "Type", std::to_string(static_cast<int>(snap.kind)));
		}
		pugi::xml_node sounds = addList(root, "Map", "list");
		for (const DrumSound& sound : map.sounds)
		{
			pugi::xml_node item = sounds.append_child("item");
			addValue(item, "int", "INote", std::to_string(sound.iNote));
			addValue(item, "int", "ONote", std::to_string(sound.oNote));
			addValue(item, "int", "Channel", std::to_string(sound.channel));
			addValue(item, "int", "Mute", sound.mute ? "1" : "0");
			addValue(item, "string", "Name", sound.name);
			addValue(item, "int", "QuantizeIndex",
				std::to_string(std::find(snaps.begin(), snaps.end(), sound.snap) - snaps.begin()));
			addValue(item, "int", "PortIndex",
				std::to_string(std::find(outputs.begin(), outputs.end(), sound.output) - outputs.begin()));
		}
		pugi::xml_node order = addList(root, "Order", "int");
		for (const uint8_t pitch : map.order)
			order.append_child("item").append_attribute("value") = static_cast<int>(pitch);
		pugi::xml_node devices = addList(root, "OutputDevices", "list");
		for (const std::string& output : outputs)
			addValue(devices.append_child("item"), "string", "DeviceName", output.empty() ? "Default Device" : output);
		std::ostringstream text;
		document.save(text, "   ", pugi::format_default, pugi::encoding_utf8);
		return text.str();
	}

	DrumMap readDrumMapFile(const std::string& text)
	{
		pugi::xml_document document;
		const pugi::xml_node root = document.load_string(text.c_str()) ? document.child("DrumMap") : pugi::xml_node();
		if (!root)
			throw std::runtime_error("the file isn't a drum map");
		DrumMap map = emptyDrumMap();
		map.name = named(root, "string", "Name").attribute("value").as_string(map.name.c_str());
		std::vector<NoteValue> snaps;
		for (const pugi::xml_node item : named(root, "list", "Quantize").children("item"))
		{
			NoteValue snap;
			snap.denominator = named(item, "int", "Grid").attribute("value").as_int(16);
			const int kind = named(item, "int", "Type").attribute("value").as_int(0);
			snap.kind = kind == 1 ? NoteValueKind::Triplet
				: kind == 2       ? NoteValueKind::Dotted
								  : NoteValueKind::Straight;
			snaps.push_back(noteValueFromString(toString(snap)).value_or(NoteValue{}));
		}
		std::vector<std::string> outputs;
		for (const pugi::xml_node item : named(root, "list", "OutputDevices").children("item"))
			outputs.emplace_back(named(item, "string", "DeviceName").attribute("value").as_string());
		size_t pitch = 0;
		for (const pugi::xml_node item : named(root, "list", "Map").children("item"))
		{
			if (pitch >= map.sounds.size())
				break;
			DrumSound& sound = map.sounds.at(pitch++);
			const auto note = [&item](const char* name, uint8_t fallback)
			{
				const int value = named(item, "int", name).attribute("value").as_int(fallback);
				return static_cast<uint8_t>(value >= 0 && value <= 127 ? value : fallback);
			};
			sound.iNote = note("INote", sound.iNote);
			sound.oNote = note("ONote", sound.oNote);
			const int channel = named(item, "int", "Channel").attribute("value").as_int(-1);
			sound.channel = channel >= 0 && channel <= 15 ? channel : -1;
			sound.mute = named(item, "int", "Mute").attribute("value").as_int(0) != 0;
			sound.name = named(item, "string", "Name").attribute("value").as_string(sound.name.c_str());
			const auto snap = static_cast<size_t>(named(item, "int", "QuantizeIndex").attribute("value").as_int(0));
			if (snap < snaps.size())
				sound.snap = snaps[snap];
			const auto port = static_cast<size_t>(named(item, "int", "PortIndex").attribute("value").as_int(0));
			sound.output = port > 0 && port < outputs.size() ? outputs[port] : "";
		}
		Json order = Json::array();
		for (const pugi::xml_node item : named(root, "list", "Order").children("item"))
			order.push_back(item.attribute("value").as_int(-1));
		try
		{
			map = drumMapFromJson({{"order", order}}, map);
		}
		catch (const std::invalid_argument&)
		{
			// ponytail: an order that isn't each pitch once is dropped for the pitch order, not repaired
		}
		return map;
	}

	void registerDrumMapCommands(ProjectModel& model, Engine& engine)
	{
		const Json idSchema = {{"type", "integer"}};
		const Json stringSchema = {{"type", "string"}};
		const Json soundSchema = {{"type", "object"}, {"additionalProperties", false}, {"required", {"pitch"}},
			{"properties",
				{{"pitch", idSchema}, {"name", stringSchema}, {"snap", stringSchema}, {"mute", {{"type", "boolean"}}},
					{"iNote", idSchema}, {"oNote", idSchema}, {"channel", {{"type", {"integer", "null"}}}},
					{"output", stringSchema}}}};
		const Json settings = {{"name", stringSchema}, {"sounds", {{"type", "array"}, {"items", soundSchema}}},
			{"order", {{"type", "array"}, {"items", idSchema}}}};
		const Json mapResult = {
			{"type", "object"}, {"properties", {{"map", {{"type", "object"}}}}}, {"required", {"map"}}};
		const std::string mapText =
			"A drum map (reference manual pp. 1235-1240) has a name, a sound for each of the 128 pitches and the drum "
			"sound list's order, from the top: each pitch once. sounds changes those named by pitch, each with what "
			"it has: name; snap, the note value (such as 1/16, 1/8T or 1/4.) that the drum editor snaps to and that "
			"is the length of the sound's new notes with lengthQuantize drumMap; mute, it doesn't play; iNote, the "
			"note played in (editor.input, recording) that becomes the pitch; oNote, the note that plays; channel, "
			"0-15, the MIDI channel it plays on, or null for Any, the note's own; output, the MIDI output it plays on, "
			"or empty for the track's (Default). The map is {id, name, sounds: [{pitch, name, snap, mute, iNote, "
			"oNote, channel, output}], order}, as project.get lists the project's drumMaps; tracks use one with "
			"track.set drumMap.";
		const auto object = [](Json properties, std::vector<std::string> required)
		{
			return Json{{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)},
				{"additionalProperties", false}};
		};

		Json addProperties = settings;
		addProperties["copy"] = idSchema;
		model.addEdit(engine,
			{"drumMap.add", 1,
				"Adds a drum map to the project: New Map, Empty Map with the sounds Sound 1 to Sound 128 on their own "
				"pitch and the track's channel, or New Copy of the map copy (named after it, with Copy) (p. 1236), "
				"then changed by what is given. " +
					mapText + " Emits drumMap.added and returns the map.",
				object(addProperties, {}), mapResult, false},
			"Add drum map", addMap);
		Json setProperties = settings;
		setProperties["map"] = idSchema;
		model.addEdit(engine,
			{"drumMap.set", 1,
				"Changes a drum map: what isn't given stays; the changes go to every track using it (p. 1237). " +
					mapText + " Emits drumMap.changed and returns the map.",
				object(setProperties, {"map"}), mapResult, false},
			"Change drum map", setMap);
		model.addEdit(engine,
			{"drumMap.remove", 1,
				"Removes a drum map from the project (p. 1236); the tracks that used it have none. Emits "
				"drumMap.removed and track.changed for those tracks.",
				object({{"map", idSchema}}, {"map"}), Json::object(), false},
			"Remove drum map", removeMap);
		model.addEdit(engine,
			{"drumMap.export", 1,
				"Saves a drum map as a drum map file (.drm, p. 1236) at path: XML with its name, sounds and order, "
				"which drumMap.import reads back.",
				object({{"map", idSchema}, {"path", stringSchema}}, {"map", "path"}), Json::object(), true},
			"", exportMap);
		model.addEdit(engine,
			{"drumMap.import", 1,
				"Loads a drum map file (.drm, p. 1236), as drumMap.export saves it, into the project as a new drum "
				"map. Settings the file doesn't have are Empty Map's. Emits drumMap.added and returns the map.",
				object({{"path", stringSchema}}, {"path"}), mapResult, false},
			"Load drum map", importMap);
	}
}
