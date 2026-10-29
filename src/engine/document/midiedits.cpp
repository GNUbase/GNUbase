// SPDX-License-Identifier: GPL-3.0-or-later
// Changing the notes and controllers of a MIDI part (#27): note.set and controller.set. Like every command that
// changes the project, each is one undo step and changes nothing if it fails (ProjectModel::add).
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		MidiPart& findPart(Project& project, const Json& parameters)
		{
			const auto id = parameters.at("part").get<ObjectId>();
			const auto found = project.midiParts.find(id);
			require(found != project.midiParts.end(), "no MIDI part " + std::to_string(id));
			return found->second;
		}

		// The item's value for key, from minimum to maximum, or current if the item doesn't have it.
		template <typename T> T integer(
			const Json& item, const char* key, T current, int64_t minimum, int64_t maximum, const std::string& name)
		{
			if (!item.contains(key))
				return current;
			const auto value = item.at(key).get<int64_t>();
			require(value >= minimum && value <= maximum,
				name + "." + key + " must be " + std::to_string(minimum) + " to " + std::to_string(maximum));
			return static_cast<T>(value);
		}

		time::Ticks position(const Json& item, time::Ticks current, const std::string& name)
		{
			const auto value = item.value("position", current);
			require(value >= 0, name + ".position must not be negative");
			return value;
		}

		// A value from 0 to 1 scaled to maximum, or current if the item doesn't have it.
		template <typename T>
		T scaled(const Json& item, const char* key, T current, double maximum, const std::string& name)
		{
			if (!item.contains(key))
				return current;
			const double value = item.at(key).get<double>();
			require(value >= 0.0 && value <= 1.0, name + "." + key + " must be 0 to 1");
			return static_cast<T>(std::llround(value * maximum));
		}

		// Finds the object an item changes; the part must hold it.
		template <typename Map> auto& changed(Map& objects, const Json& item, const std::string& name, const char* what)
		{
			const auto id = item.at("id").template get<ObjectId>();
			const auto found = objects.find(id);
			require(found != objects.end(), name + ": the part has no " + what + " " + std::to_string(id));
			return found->second;
		}

		Json setNotes(Project& project, CommandContext& context)
		{
			MidiPart& part = findPart(project, context.parameters());
			const Json& items = context.parameters().at("notes");
			std::vector<Note> notes;
			for (size_t i = 0; i < items.size(); ++i)
			{
				const Json& item = items[i];
				const std::string name = "notes[" + std::to_string(i) + "]";
				Note note = changed(part.notes, item, name, "note");
				note.position = position(item, note.position, name);
				note.length = item.value("length", note.length);
				require(note.length > 0, name + ".length must be positive");
				note.pitch = integer(item, "pitch", note.pitch, 0, 127, name);
				note.channel = integer(item, "channel", note.channel, 0, 15, name);
				note.velocity = scaled(item, "velocity", note.velocity, g_maxVelocity, name);
				note.releaseVelocity = scaled(item, "releaseVelocity", note.releaseVelocity, g_maxVelocity, name);
				notes.push_back(note);
			}
			// Changed only once every item is valid. ProjectModel::add would restore the project anyway.
			Json result = Json::array();
			for (const Note& note : notes)
			{
				part.notes[note.id] = note;
				result.push_back(toJson(note));
			}
			context.emit("note.changed", {{"part", part.id}, {"notes", result}});
			return {{"notes", result}};
		}

		// Whether controller events of the type have the field, besides id, position and type.
		bool uses(ControllerType type, const std::string& key)
		{
			switch (type)
			{
			case ControllerType::Control:
			case ControllerType::PolyPressure:
				return key == "number" || key == "value" || key == "channel";
			case ControllerType::PitchBend:
			case ControllerType::ChannelPressure:
				return key == "value" || key == "channel";
			case ControllerType::Program:
				return key == "number" || key == "bank" || key == "channel";
			case ControllerType::SysEx:
				return key == "data";
			}
			return false;
		}

		Json setControllers(Project& project, CommandContext& context)
		{
			MidiPart& part = findPart(project, context.parameters());
			const Json& items = context.parameters().at("controllers");
			std::vector<Controller> controllers;
			for (size_t i = 0; i < items.size(); ++i)
			{
				const Json& item = items[i];
				const std::string name = "controllers[" + std::to_string(i) + "]";
				const Controller& current = changed(part.controllers, item, name, "controller");
				// The event as it becomes, its fields with the changes, checked against its type as controller.add
				// checks a new one. A null bank removes it.
				Json merged = toJson(current);
				for (auto field = item.begin(); field != item.end(); ++field)
					merged[field.key()] = field.value();
				if (merged.contains("bank") && merged.at("bank").is_null())
					merged.erase("bank");
				const std::optional<ControllerType> type = controllerTypeFromName(merged.at("type").get<std::string>());
				if (type.has_value())
					for (auto field = item.begin(); field != item.end(); ++field)
						require(field.key() == "id" || field.key() == "position" || field.key() == "type" ||
								uses(*type, field.key()),
							name + "." + field.key() + " doesn't apply to " + controllerTypeName(*type) + " events");
				Controller controller = controllerFrom(merged, name);
				controller.id = current.id;
				controllers.push_back(std::move(controller));
			}
			Json result = Json::array();
			for (const Controller& controller : controllers)
			{
				part.controllers[controller.id] = controller;
				result.push_back(toJson(controller));
			}
			context.emit("controller.changed", {{"part", part.id}, {"controllers", result}});
			return {{"controllers", result}};
		}

		// {"part": id, key: [changes]}, each change {"id": id, and what changes}.
		Json changesSchema(const char* key, const Json& properties)
		{
			Json change = {
				{"type", "object"}, {"additionalProperties", false}, {"required", {"id"}}, {"properties", properties}};
			change["properties"]["id"] = {{"type", "integer"}};
			return {{"type", "object"}, {"additionalProperties", false}, {"required", {"part", key}},
				{"properties", {{"part", {{"type", "integer"}}}, {key, {{"type", "array"}, {"items", change}}}}}};
		}

		Json resultSchema(const char* key)
		{
			return {{"type", "object"}, {"required", {key}}, {"properties", {{key, {{"type", "array"}}}}}};
		}
	}

	void ProjectModel::registerMidiEditCommands(Engine& engine)
	{
		const Json integer = {{"type", "integer"}};
		const Json number = {{"type", "number"}};
		add(engine,
			{"note.set", 1,
				"Changes notes of a MIDI part: each change names a note by id and gives what changes, position and "
				"length in ticks, pitch 0-127, velocity 0-1, release velocity 0-1 or MIDI channel 0-15. Returns the "
				"changed notes.",
				changesSchema("notes",
					{{"position", integer}, {"length", integer}, {"pitch", integer}, {"velocity", number},
						{"releaseVelocity", number}, {"channel", integer}}),
				resultSchema("notes"), false},
			setNotes);
		add(engine,
			{"controller.set", 1,
				"Changes controller events of a MIDI part: each change names a controller event by id and gives what "
				"changes: position in ticks, type, and the fields of the type as controller.add takes them (number, "
				"value, channel, bank, data). A field the type doesn't have is an error, such as value on a program "
				"change; a null bank removes it. A new type keeps the fields it shares with the old one and needs the "
				"others. Returns the changed controller events.",
				changesSchema("controllers",
					{{"position", integer},
						{"type",
							{{"type", "string"},
								{"enum",
									{"controller", "pitchBend", "channelPressure", "polyPressure", "program",
										"sysEx"}}}},
						{"number", integer}, {"value", number}, {"channel", integer},
						{"bank", {{"type", {"integer", "null"}}}}, {"data", {{"type", "array"}, {"items", integer}}}}),
				resultSchema("controllers"), false},
			setControllers);
	}
}
