// SPDX-License-Identifier: GPL-3.0-or-later
// Changing the notes and controllers of a MIDI part (#27): note.set and controller.set. Like every command that
// changes the project, each is one undo step and changes nothing if it fails (ProjectModel::add).
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <cmath>
#include <cstdint>
#include <string>
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

		Json setControllers(Project& project, CommandContext& context)
		{
			MidiPart& part = findPart(project, context.parameters());
			const Json& items = context.parameters().at("controllers");
			std::vector<Controller> controllers;
			for (size_t i = 0; i < items.size(); ++i)
			{
				const Json& item = items[i];
				const std::string name = "controllers[" + std::to_string(i) + "]";
				Controller controller = changed(part.controllers, item, name, "controller");
				controller.position = position(item, controller.position, name);
				controller.number = integer(item, "number", controller.number, 0, 127, name);
				controller.channel = integer(item, "channel", controller.channel, 0, 15, name);
				controller.value = scaled(item, "value", controller.value, g_maxControllerValue, name);
				controllers.push_back(controller);
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
				"length in ticks, pitch 0-127, velocity 0-1 or MIDI channel 0-15. Returns the changed notes.",
				changesSchema("notes",
					{{"position", integer}, {"length", integer}, {"pitch", integer}, {"velocity", number},
						{"channel", integer}}),
				resultSchema("notes"), false},
			setNotes);
		add(engine,
			{"controller.set", 1,
				"Changes controller events of a MIDI part: each change names a controller event by id and gives what "
				"changes, position in ticks, controller number 0-127, value 0-1 or MIDI channel 0-15. Returns the "
				"changed controller events.",
				changesSchema("controllers",
					{{"position", integer}, {"number", integer}, {"value", number}, {"channel", integer}}),
				resultSchema("controllers"), false},
			setControllers);
	}
}
