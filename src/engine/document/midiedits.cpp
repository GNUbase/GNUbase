// SPDX-License-Identifier: GPL-3.0-or-later
// Changing the notes and controllers of a MIDI part (#27): note.set and controller.set, and the key editor's chord
// editing (#127): note.chord and note.voicing. Like every command that changes the project, each is one undo step and
// changes nothing if it fails (ProjectModel::add).
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/midi/chords.h"

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
				note.probability = item.value("probability", note.probability);
				require(note.probability >= 0.0 && note.probability <= 1.0, name + ".probability must be 0 to 1");
				note.velocityVariance = item.value("velocityVariance", note.velocityVariance);
				require(std::abs(note.velocityVariance) <= 1.0, name + ".velocityVariance must be -1 to 1");
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

		// The part's notes that the parameters' notes name, in that order.
		std::vector<Note*> namedNotes(MidiPart& part, const Json& parameters)
		{
			std::vector<Note*> notes;
			for (const Json& id : parameters.at("notes"))
			{
				const auto found = part.notes.find(id.get<ObjectId>());
				require(found != part.notes.end(), "the part has no note " + id.dump());
				notes.push_back(&found->second);
			}
			require(!notes.empty(), "notes must name at least one note");
			return notes;
		}

		std::vector<int> pitchesOf(const std::vector<Note*>& notes)
		{
			std::vector<int> pitches;
			pitches.reserve(notes.size());
			for (const Note* note : notes)
				pitches.push_back(note->pitch);
			return pitches;
		}

		Json chordOf(Project& project, CommandContext& context)
		{
			MidiPart& part = findPart(project, context.parameters());
			const std::optional<midi::Chord> chord =
				midi::recognizeChord(pitchesOf(namedNotes(part, context.parameters())));
			if (!chord)
				return {{"chord", nullptr}};
			return {{"chord", {{"root", chord->root}, {"type", chord->type->name}, {"name", chord->name()}}}};
		}

		Json voiceNotes(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			MidiPart& part = findPart(project, parameters);
			const std::vector<Note*> notes = namedNotes(part, parameters);
			const std::vector<int> pitches = pitchesOf(notes);
			require(parameters.contains("chord") != parameters.contains("voicing"), "give either chord or voicing");
			std::vector<int> changed;
			if (parameters.contains("chord"))
			{
				const midi::ChordType* type = midi::chordType(parameters.at("chord").get<std::string>());
				require(type != nullptr, "no chord type " + parameters.at("chord").dump());
				changed = midi::fitChord(pitches, *type);
			}
			else
			{
				const std::optional<midi::Voicing> voicing =
					midi::voicingFromName(parameters.at("voicing").get<std::string>());
				if (!voicing)
					throw CommandError("no voicing " + parameters.at("voicing").dump());
				const std::optional<std::vector<int>> revoiced = midi::revoice(pitches, *voicing);
				if (!revoiced)
					throw CommandError("the notes can't take that voicing: too few, or out of range");
				changed = *revoiced;
			}
			Json result = Json::array();
			for (size_t i = 0; i < notes.size(); ++i)
				if (notes[i]->pitch != changed[i])
				{
					notes[i]->pitch = static_cast<uint8_t>(changed[i]);
					result.push_back(toJson(*notes[i]));
				}
			if (!result.empty())
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
				"length in ticks, pitch 0-127, velocity 0-1, release velocity 0-1, MIDI channel 0-15, play probability "
				"0-1 or velocity variance -1 to 1 (see note.add). Returns the changed notes.",
				changesSchema("notes",
					{{"position", integer}, {"length", integer}, {"pitch", integer}, {"velocity", number},
						{"releaseVelocity", number}, {"channel", integer}, {"probability", number},
						{"velocityVariance", number}}),
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

		Json chordTypes = Json::array();
		for (const midi::ChordType& type : midi::chordTypes())
			chordTypes.push_back(type.name);
		const Json notesOfPart = {{"type", "object"}, {"additionalProperties", false}, {"required", {"part", "notes"}},
			{"properties", {{"part", integer}, {"notes", {{"type", "array"}, {"items", integer}}}}}};
		add(engine,
			{"note.chord", 1,
				"The chord that notes of a MIDI part make, in any octave and order (arpeggiated notes too), as the key "
				"editor's Chord Editing section shows it: chord {root, the pitch class (0 is C), type, as note.voicing "
				"takes it, and name, such as Cm7}, or null if they make none of the chord types exactly. Of several "
				"roots that fit, the lowest note's.",
				notesOfPart,
				{{"type", "object"}, {"required", {"chord"}},
					{"properties", {{"chord", {{"type", {"object", "null"}}}}}}},
				true},
			chordOf);
		Json voicingSchema = notesOfPart;
		voicingSchema["properties"]["chord"] = {{"type", "string"}, {"enum", chordTypes}};
		voicingSchema["properties"]["voicing"] = {
			{"type", "string"}, {"enum", {"invertUp", "invertDown", "drop2", "drop3", "drop24"}}};
		add(engine,
			{"note.voicing", 1,
				"Transposes notes of a MIDI part as the key editor's Chord Editing section does (reference manual pp. "
				"1215-1217). chord fits them to a chord type (major, minor, diminished, augmented, sus2, sus4; major7, "
				"minor7, dominant7, halfDiminished7, diminished7, minorMajor7) on the root of the chord they make, or "
				"else of the lowest note: from the lowest up, each moves to the nearest pitch of a chord tone that no "
				"lower note took since every tone last had one, down on a tie. voicing changes the voicing: invertUp "
				"moves the lowest note up by octaves until it is the highest, invertDown the highest down until it is "
				"the lowest, drop2, drop3 and drop24 move the second, the third, or the second and fourth highest "
				"notes down an octave. Give chord or voicing. Returns the notes that changed.",
				voicingSchema, resultSchema("notes"), false},
			voiceNotes);
	}
}
