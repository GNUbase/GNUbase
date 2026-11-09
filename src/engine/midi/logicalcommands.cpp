// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/midi/logicalcommands.h"

#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/midi/midivalues.h"
#include "engine/rulepresets.h"
#include "engine/schema.h"
#include "engine/settings/store.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gnubase::engine::midi
{
	namespace
	{
		using time::Ticks;

		constexpr std::string_view g_presetsKey = "midiRules.presets";
		constexpr double g_ticksPerLogicalTick =
			static_cast<double>(time::g_ticksPerQuarter) / g_logicalTicksPerQuarter;

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

		Json arraySchema(Json items)
		{
			return {{"type", "array"}, {"items", std::move(items)}};
		}

		using PresetLibrary = RulePresets<LogicalPreset>;

		// The rules given, or a preset's.
		LogicalRules rulesOf(const PresetLibrary& presets, const Json& parameters)
		{
			require(parameters.contains("rules") != parameters.contains("preset"), "give either rules or preset");
			if (parameters.contains("preset"))
			{
				return presets.rulesOf(parameters.at("preset").get<std::string>());
			}
			try
			{
				return logicalRulesFromJson(parameters.at("rules"));
			}
			catch (const std::invalid_argument& error)
			{
				throw CommandError(error.what());
			}
		}

		std::vector<MidiPart*> partsOf(Project& project, const Json& parameters)
		{
			const auto ids = parameters.at("parts").get<std::vector<ObjectId>>();
			require(!ids.empty(), "parts must name at least one MIDI part");
			std::vector<MidiPart*> parts;
			for (const ObjectId id : ids)
			{
				const auto found = project.midiParts.find(id);
				require(found != project.midiParts.end(), "no MIDI part " + std::to_string(id));
				require(std::find(parts.begin(), parts.end(), &found->second) == parts.end(),
					"MIDI part " + std::to_string(id) + " is named twice");
				parts.push_back(&found->second);
			}
			return parts;
		}

		NoteSelection selectionOf(const std::vector<MidiPart*>& parts, const Json& parameters)
		{
			if (!parameters.contains("notes"))
				return {};
			std::set<ObjectId> notes;
			for (const ObjectId id : parameters.at("notes").get<std::vector<ObjectId>>())
			{
				require(std::any_of(parts.begin(), parts.end(),
							[id](const MidiPart* part) { return part->notes.count(id) != 0; }),
					"no note " + std::to_string(id) + " in the MIDI parts");
				notes.insert(id);
			}
			return NoteSelection(std::move(notes));
		}

		double toLogicalTicks(Ticks ticks)
		{
			return static_cast<double>(ticks) / g_ticksPerLogicalTick;
		}

		Ticks fromLogicalTicks(double ticks)
		{
			return static_cast<Ticks>(std::llround(ticks * g_ticksPerLogicalTick));
		}

		LogicalEvent noteEvent(const Note& note, Ticks start)
		{
			LogicalEvent event;
			event.type = LogicalType::Note;
			event.channel = note.channel + 1;
			event.subtype = note.pitch;
			event.mainValue = noteOnVelocityToMidi1(note.velocity);
			event.secondaryValue = velocityToMidi1(note.releaseVelocity);
			event.position = toLogicalTicks(start + note.position);
			event.length = toLogicalTicks(note.length);
			return event;
		}

		// False for SysEx, which the rules don't see.
		bool controllerEvent(const Controller& controller, Ticks start, LogicalEvent& event)
		{
			event = LogicalEvent{};
			event.channel = controller.channel + 1;
			event.subtype = controller.number;
			event.position = toLogicalTicks(start + controller.position);
			switch (controller.type)
			{
			case ControllerType::Control:
				event.type = LogicalType::Controller;
				event.mainValue = controllerToMidi1(controller.value);
				return true;
			case ControllerType::PolyPressure:
				event.type = LogicalType::PolyPressure;
				event.mainValue = controllerToMidi1(controller.value);
				return true;
			case ControllerType::Program:
				event.type = LogicalType::Program;
				return true;
			case ControllerType::ChannelPressure:
				event.type = LogicalType::Aftertouch;
				event.subtype = controllerToMidi1(controller.value);
				return true;
			case ControllerType::PitchBend:
			{
				const uint16_t bend = pitchBendToMidi1(controller.value);
				event.type = LogicalType::PitchBend;
				event.subtype = static_cast<int>(bend & 0x7FU);
				event.mainValue = static_cast<int>(bend >> 7U);
				return true;
			}
			case ControllerType::SysEx:
				return false;
			}
			return false;
		}

		Ticks positionIn(const LogicalEvent& changed, const LogicalEvent& original, Ticks old, Ticks start)
		{
			if (changed.position == original.position)
				return old;
			return std::max<Ticks>(0, fromLogicalTicks(changed.position) - start);
		}

		// Writes what the rules changed back into the note; values they left keep their full resolution.
		void writeNote(Note& note, const LogicalEvent& changed, const LogicalEvent& original, Ticks start)
		{
			note.position = positionIn(changed, original, note.position, start);
			if (changed.length != original.length)
				note.length = std::max<Ticks>(1, fromLogicalTicks(changed.length));
			note.pitch = static_cast<uint8_t>(changed.subtype);
			if (changed.mainValue != original.mainValue)
				note.velocity = velocityFromMidi1(static_cast<uint8_t>(std::max(changed.mainValue, 1)));
			note.channel = static_cast<uint8_t>(changed.channel - 1);
			if (changed.secondaryValue != original.secondaryValue)
				note.releaseVelocity = velocityFromMidi1(static_cast<uint8_t>(changed.secondaryValue));
		}

		void writeController(Controller& controller, LogicalEvent changed, const LogicalEvent& original, Ticks start)
		{
			if (changed.type == LogicalType::Note) // controllers don't turn into notes in parts
				changed.type = original.type;
			controller.position = positionIn(changed, original, controller.position, start);
			controller.channel = static_cast<uint8_t>(changed.channel - 1);
			const bool sameType = changed.type == original.type;
			const bool subtypeSame = sameType && changed.subtype == original.subtype;
			const bool mainSame = sameType && changed.mainValue == original.mainValue;
			const auto main = static_cast<uint8_t>(changed.mainValue);
			const auto subtype = static_cast<uint8_t>(changed.subtype);
			switch (changed.type)
			{
			case LogicalType::Controller:
			case LogicalType::PolyPressure:
				controller.type =
					changed.type == LogicalType::Controller ? ControllerType::Control : ControllerType::PolyPressure;
				controller.number = subtype;
				if (!mainSame)
					controller.value = controllerFromMidi1(main);
				break;
			case LogicalType::Program:
				controller.type = ControllerType::Program;
				controller.number = subtype;
				if (!sameType)
					controller.value = g_noBank;
				break;
			case LogicalType::Aftertouch:
				controller.type = ControllerType::ChannelPressure;
				controller.number = 0;
				if (!subtypeSame)
					controller.value = controllerFromMidi1(subtype);
				break;
			case LogicalType::PitchBend:
				controller.type = ControllerType::PitchBend;
				controller.number = 0;
				if (!subtypeSame || !mainSame)
					controller.value = pitchBendFromMidi1(static_cast<uint16_t>((main << 7U) | subtype));
				break;
			case LogicalType::Note:
				break;
			}
		}
	}

	LogicalResult applyLogical(const Project& project, MidiPart& part, const NoteSelection& selection,
		const LogicalRules& rules, LogicalRandom& random, const NewId& newId)
	{
		const Ticks start = time::toTicks(part.position, project.tempo, project.sampleRate);
		const LogicalFunction function = rules.function;
		const bool changes = function != LogicalFunction::Select;
		const bool transforms = function == LogicalFunction::Transform || function == LogicalFunction::Insert ||
			function == LogicalFunction::InsertExclusive;
		LogicalResult result;
		const auto found = [&rules](const LogicalEvent& event)
		{ return matches(rules.filters.data(), rules.filters.size(), event); };
		// Whether the event stays, after changing it if it is found.
		const auto apply = [&](bool isFound, LogicalEvent& event)
		{
			if (!isFound)
				return function != LogicalFunction::InsertExclusive;
			if (function == LogicalFunction::Delete)
				return false;
			return !transforms || transform(rules.actions.data(), rules.actions.size(), event, random);
		};

		std::vector<Note> added;
		for (auto it = part.notes.begin(); it != part.notes.end();)
		{
			Note& note = it->second;
			if (!selection.contains(note.id))
			{
				++it;
				continue;
			}
			const LogicalEvent original = noteEvent(note, start);
			const bool isFound = found(original);
			if (isFound)
				result.notes.insert(note.id);
			if (!changes || (!isFound && function != LogicalFunction::InsertExclusive))
			{
				++it;
				continue;
			}
			LogicalEvent changed = original;
			const bool keep = apply(isFound, changed);
			changed.type = LogicalType::Note; // notes don't turn into other events in parts
			if (function == LogicalFunction::Insert)
			{
				if (keep)
				{
					Note copy = note;
					copy.id = newId();
					copy.origin.reset();
					writeNote(copy, changed, original, start);
					added.push_back(copy);
				}
				++it;
				continue;
			}
			if (!keep)
			{
				it = part.notes.erase(it);
				continue;
			}
			writeNote(note, changed, original, start);
			++it;
		}
		for (const Note& note : added)
			part.notes[note.id] = note;

		if (!selection.all())
			return result;
		std::vector<Controller> copies;
		for (auto it = part.controllers.begin(); it != part.controllers.end();)
		{
			Controller& controller = it->second;
			LogicalEvent original;
			if (!controllerEvent(controller, start, original))
			{
				++it;
				continue;
			}
			const bool isFound = found(original);
			if (isFound)
				result.controllers.insert(controller.id);
			if (!changes || (!isFound && function != LogicalFunction::InsertExclusive))
			{
				++it;
				continue;
			}
			LogicalEvent changed = original;
			const bool keep = apply(isFound, changed);
			if (function == LogicalFunction::Insert)
			{
				if (keep)
				{
					Controller copy = controller;
					copy.id = newId();
					writeController(copy, changed, original, start);
					copies.push_back(copy);
				}
				++it;
				continue;
			}
			if (!keep)
			{
				it = part.controllers.erase(it);
				continue;
			}
			writeController(controller, changed, original, start);
			++it;
		}
		for (const Controller& controller : copies)
			part.controllers[controller.id] = controller;
		return result;
	}

	const settings::Format& logicalPresetsFormat()
	{
		static const settings::Format format{"midi-rule-presets", 1,
			{{std::string(g_presetsKey), "The user's rule editor and input transformer presets.",
				Json{{"type", "array"}}, Json::array()}},
			{}};
		return format;
	}

	void registerLogicalCommands(ProjectModel& model, Engine& engine)
	{
		const auto presets = std::make_shared<PresetLibrary>(
			RulePresetKind<LogicalPreset>{std::string(g_presetsKey), &logicalPresetsFormat(), "MIDI rule preset",
				"midiRules.presetsChanged", &factoryLogicalPresets(),
				[](const LogicalRules& rules) { return toJson(rules); }, logicalRulesFromJson},
			engine.settingsDirectory());
		const Json idSchema = {{"type", "integer"}};
		const Json stringSchema = {{"type", "string"}};
		const Json ids = arraySchema(idSchema);
		const Json rulesSchema = logicalRulesSchema();
		const std::string rulesText =
			" rules: filters, each {open, target, condition, parameter1, parameter2, close, join} (target position, "
			"length, subtype, mainValue, channel, type or secondaryValue; condition equal, unequal, bigger, "
			"biggerOrEqual, less, lessOrEqual, insideRange, outsideRange or noteIsEqualTo; open and close brackets 0 "
			"to 3; join and or or to the next line, and binding more tightly than or), function (delete, transform, "
			"insert, insertExclusive or select) and actions, each {target, operation, parameter1, parameter2} "
			"(operation add, subtract, multiplyBy, divideBy, roundBy, setRandom, setFixed, setRelativeRandom, "
			"useSubtype, useMainValue, mirror, transposeToScale or addLength), applied in order. Values are MIDI "
			"1.0's, "
			"channels 1 to 16, positions and lengths in ticks of 480 to a quarter note from the project's start; the "
			"type's parameter1 is note, polyPressure, controller, program, aftertouch or pitchBend, and "
			"transposeToScale's parameter2 a scale type, its parameter1 the root (0 is C). Subtype and main value are "
			"a note's pitch and velocity, poly pressure's pitch and pressure, a controller's number and value, a "
			"program change's program, aftertouch's pressure (subtype) and pitch bend's low and high 7 bits; the "
			"secondary value is a note's note-off velocity. preset names a preset instead of rules.";
		const Json targetProperties = {
			{"parts", ids}, {"notes", ids}, {"rules", rulesSchema}, {"preset", stringSchema}, {"seed", idSchema}};

		model.addEdit(engine,
			{"midi.applyRules", 1,
				"Applies rule editor rules to the notes of MIDI parts (all of them, or those in notes) and, "
				"without notes, to their controllers, pressure, pitch bend and program changes (not SysEx): delete "
				"removes the events the filters find, transform changes them by the actions, insert adds changed "
				"copies, insertExclusive changes them and removes the others. Pitches leaving 0 to 127 delete notes, "
				"velocities stay from 1 to 127, events don't move before their part's start and notes don't turn "
				"into other events or the other way round. seed makes the random operations repeatable." +
					rulesText +
					" Use midi.findByRules for select. Returns the parts it changed and how many events "
					"it found.",
				objectSchema(targetProperties, {"parts"}),
				{{"type", "object"}, {"properties", {{"parts", ids}, {"found", idSchema}}},
					{"required", {"parts", "found"}}},
				false},
			"Rule editor",
			[presets](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const LogicalRules rules = rulesOf(*presets, parameters);
				require(rules.function != LogicalFunction::Select, "midi.findByRules selects");
				const std::vector<MidiPart*> parts = partsOf(project, parameters);
				const NoteSelection selection = selectionOf(parts, parameters);
				LogicalRandom random{parameters.value("seed", uint64_t{0})};
				Json changed = Json::array();
				size_t found = 0;
				for (MidiPart* part : parts)
				{
					const MidiPart before = *part;
					const LogicalResult result =
						applyLogical(project, *part, selection, rules, random, [&project] { return project.newId(); });
					found += result.notes.size() + result.controllers.size();
					if (part->notes != before.notes || part->controllers != before.controllers)
					{
						changed.push_back(part->id);
						context.emit("midiPart.changed", {{"part", toJson(*part)}});
					}
				}
				return Json{{"parts", std::move(changed)}, {"found", found}};
			});
		model.addEdit(engine,
			{"midi.findByRules", 1,
				"Finds the events of MIDI parts that rule editor rules' filters find, as the select function "
				"does, and changes nothing: the notes (all of them, or of those in notes) and, without notes, the "
				"controllers, pressure, pitch bend and program changes." +
					rulesText + " Returns the notes and controllers found, by id.",
				objectSchema(targetProperties, {"parts"}),
				{{"type", "object"}, {"properties", {{"notes", ids}, {"controllers", ids}}},
					{"required", {"notes", "controllers"}}},
				true},
			"",
			[presets](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				LogicalRules rules = rulesOf(*presets, parameters);
				rules.function = LogicalFunction::Select;
				// Select changes nothing.
				const std::vector<MidiPart*> parts = partsOf(project, parameters);
				const NoteSelection selection = selectionOf(parts, parameters);
				LogicalRandom random;
				Json notes = Json::array();
				Json controllers = Json::array();
				for (MidiPart* part : parts)
				{
					const LogicalResult result =
						applyLogical(project, *part, selection, rules, random, [&project] { return project.newId(); });
					for (const ObjectId id : result.notes)
						notes.push_back(id);
					for (const ObjectId id : result.controllers)
						controllers.push_back(id);
				}
				return Json{{"notes", std::move(notes)}, {"controllers", std::move(controllers)}};
			});
		model.addEdit(engine,
			{"midi.setInputTransformer", 1,
				"Sets the project input transformer's modules, which the MIDI and instrument tracks whose input "
				"transformer is in project mode (channel.set inputTransformer) apply to what their MIDI input "
				"plays: modules, an array of at most four, each {on, rules} with what changes (the first given "
				"changes module 1, and so on). The modules that are on apply in order, before the track plays and "
				"records; their functions are delete (which filters events out), transform, insert and "
				"insertExclusive. Live events have no position or length, so rules for those are refused." +
					rulesText.substr(0, rulesText.find(" preset names")) +
					" Returns the modules, as project.get has them in inputTransformer.",
				objectSchema({{"modules", {{"type", "array"}}}}, {"modules"}),
				{{"type", "object"}, {"properties", {{"modules", {{"type", "array"}}}}}, {"required", {"modules"}}},
				false},
			"Change project input transformer",
			[](Project& project, CommandContext& context)
			{
				try
				{
					project.inputTransformer =
						inputTransformerModulesFromJson(context.parameters().at("modules"), project.inputTransformer);
				}
				catch (const std::invalid_argument& error)
				{
					throw CommandError(error.what());
				}
				const Json modules = toJson(project.inputTransformer);
				context.emit("project.inputTransformerChanged", {{"modules", modules}});
				return Json{{"modules", modules}};
			});

		engine.registerCommand(
			{"midiRules.presets", 1,
				"The rule editor and input transformer presets: the factory presets, named by category (such as "
				"\"Delete/Short notes\" and \"Transform/Octave up\"), and the user's, each with its rules as "
				"midi.applyRules takes them. The user's presets are kept in the settings folder, so they come back "
				"when GNUbase starts again: file is where (null if they are kept in memory only), readOnly whether "
				"changes are saved, and problems what was wrong with it, such as presets that this GNUbase can't "
				"read and skips; they stay in the file.",
				objectSchema({}),
				{{"type", "object"},
					{"properties",
						{{"presets", {{"type", "array"}}}, {"file", {{"type", {"string", "null"}}}},
							{"readOnly", {{"type", "boolean"}}}, {"problems", {{"type", "array"}}}}},
					{"required", {"presets", "file", "readOnly", "problems"}}},
				true},
			[presets](CommandContext&) { return presets->toJson(); });
		engine.registerCommand(
			{"midiRules.savePreset", 1,
				"Saves rules (as midi.applyRules takes them) as a MIDI rule preset named name, replacing the user's "
				"preset of that name. Factory presets can't be replaced. Presets belong to the user, not to the "
				"project: they are saved in the settings folder. midiRules.presetsChanged announces the user's "
				"presets' names.",
				objectSchema({{"name", stringSchema}, {"rules", rulesSchema}}, {"name", "rules"}),
				{{"type", "object"}, {"properties", {{"preset", {{"type", "object"}}}}}, {"required", {"preset"}}},
				false},
			[presets](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				LogicalPreset preset{parameters.at("name").get<std::string>(), {}};
				try
				{
					preset.rules = logicalRulesFromJson(parameters.at("rules"));
				}
				catch (const std::invalid_argument& error)
				{
					throw CommandError(error.what());
				}
				presets->save(preset);
				presets->announce(context);
				return Json{{"preset", presets->presetJson(preset, false)}};
			});
		engine.registerCommand({"midiRules.removePreset", 1,
								   "Removes one of the user's MIDI rule presets, and the presets of that name that "
								   "this GNUbase can't read (see midiRules.presets).",
								   objectSchema({{"name", stringSchema}}, {"name"}), Json::object(), false},
			[presets](CommandContext& context)
			{
				presets->remove(context.parameters().at("name").get<std::string>());
				presets->announce(context);
				return Json::object();
			});
	}
}
