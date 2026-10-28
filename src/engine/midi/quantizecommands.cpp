// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/midi/quantizecommands.h"

#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/midi/quantize.h"

#include <algorithm>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::engine::midi
{
	namespace
	{
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

		// The presets users saved, after the factory presets. Names are unique among both.
		class PresetLibrary
		{
			using Iterator = std::vector<QuantizePreset>::iterator;

		public:
			static bool isFactory(const std::string& name)
			{
				const auto& factory = factoryQuantizePresets();
				return std::any_of(
					factory.begin(), factory.end(), [&name](const QuantizePreset& p) { return p.name == name; });
			}

			const QuantizePreset* find(const std::string& name) const
			{
				for (const auto* list : {&factoryQuantizePresets(), &m_user})
					for (const QuantizePreset& preset : *list)
						if (preset.name == name)
							return &preset;
				return nullptr;
			}

			// Adds a preset, or replaces the user's preset of the same name.
			void save(const QuantizePreset& preset)
			{
				require(!preset.name.empty(), "name must not be empty");
				require(!isFactory(preset.name), preset.name + " is a factory preset");
				const Iterator existing = userPreset(preset.name);
				if (existing != m_user.end())
					*existing = preset;
				else
					m_user.push_back(preset);
			}

			void remove(const std::string& name)
			{
				require(!isFactory(name), "factory presets can't be removed");
				m_user.erase(existingUserPreset(name));
			}

			void rename(const std::string& name, const std::string& newName)
			{
				require(!isFactory(name), "factory presets can't be renamed");
				const Iterator preset = existingUserPreset(name);
				require(!newName.empty(), "newName must not be empty");
				require(!isFactory(newName), newName + " is a factory preset");
				require(
					newName == name || userPreset(newName) == m_user.end(), "a preset named " + newName + " exists");
				preset->name = newName;
			}

			Json toJson() const
			{
				Json presets = Json::array();
				for (const QuantizePreset& preset : factoryQuantizePresets())
					presets.push_back(presetJson(preset, true));
				for (const QuantizePreset& preset : m_user)
					presets.push_back(presetJson(preset, false));
				return presets;
			}

			static Json presetJson(const QuantizePreset& preset, bool factory)
			{
				return {{"name", preset.name}, {"factory", factory}, {"settings", midi::toJson(preset.settings)}};
			}

			void announce(CommandContext& context) const
			{
				Json names = Json::array();
				for (const QuantizePreset& preset : m_user)
					names.push_back(preset.name);
				context.emit("quantize.presetsChanged", {{"presets", std::move(names)}});
			}

		private:
			Iterator userPreset(const std::string& name)
			{
				return std::find_if(
					m_user.begin(), m_user.end(), [&name](const QuantizePreset& p) { return p.name == name; });
			}

			Iterator existingUserPreset(const std::string& name)
			{
				const Iterator preset = userPreset(name);
				require(preset != m_user.end(), "no quantize preset " + name);
				return preset;
			}

			std::vector<QuantizePreset> m_user;
		};

		QuantizeSettings changedSettings(const Json& json, QuantizeSettings base)
		{
			try
			{
				return quantizeSettingsFromJson(json, std::move(base));
			}
			catch (const std::invalid_argument& error)
			{
				throw CommandError(std::string("settings.") + error.what());
			}
		}

		// The project's settings, or a preset's, with what the parameters' settings change.
		QuantizeSettings settingsFor(const Project& project, const PresetLibrary& presets, const Json& parameters)
		{
			QuantizeSettings settings = project.quantize;
			if (parameters.contains("preset"))
			{
				const std::string name = parameters.at("preset").get<std::string>();
				const QuantizePreset* preset = presets.find(name);
				require(preset != nullptr, "no quantize preset " + name);
				settings = preset->settings;
			}
			if (parameters.contains("settings"))
				settings = changedSettings(parameters.at("settings"), settings);
			return settings;
		}

		MidiPart& findPart(Project& project, ObjectId id)
		{
			const auto part = project.midiParts.find(id);
			require(part != project.midiParts.end(), "no MIDI part " + std::to_string(id));
			return part->second;
		}

		struct Target
		{
			MidiPart* part = nullptr;
			std::vector<ObjectId> notes;
		};

		// The notes a command works on: some or all notes of part, or all notes of parts.
		std::vector<Target> targets(Project& project, const Json& parameters)
		{
			const bool onePart = parameters.contains("part");
			require(onePart != parameters.contains("parts"), "give part or parts, not both; notes picks notes of part");
			require(onePart || !parameters.contains("notes"), "notes needs part");
			std::vector<Target> targets;
			const std::vector<ObjectId> parts = onePart ? std::vector<ObjectId>{parameters.at("part").get<ObjectId>()}
														: parameters.at("parts").get<std::vector<ObjectId>>();
			for (const ObjectId id : parts)
			{
				Target target{&findPart(project, id), {}};
				if (parameters.contains("notes"))
				{
					target.notes = parameters.at("notes").get<std::vector<ObjectId>>();
					for (const ObjectId note : target.notes)
						require(target.part->notes.count(note) != 0,
							"no note " + std::to_string(note) + " in MIDI part " + std::to_string(id));
				}
				else
				{
					for (const auto& [note, unused] : target.part->notes)
						target.notes.push_back(note);
				}
				targets.push_back(std::move(target));
			}
			return targets;
		}

		using NoteFunction = std::function<bool(Note& note, time::Ticks partStart)>;

		// Runs function on every target note and announces the notes it changed, per part.
		Json forNotes(Project& project, CommandContext& context, const NoteFunction& function)
		{
			Json changed = Json::array();
			for (const Target& target : targets(project, context.parameters()))
			{
				const time::Ticks partStart = time::toTicks(target.part->position, project.tempo, project.sampleRate);
				Json notes = Json::array();
				for (const ObjectId id : target.notes)
				{
					Note& note = target.part->notes.at(id);
					if (function(note, partStart))
						notes.push_back(toJson(note));
				}
				if (notes.empty())
					continue;
				changed.insert(changed.end(), notes.begin(), notes.end());
				context.emit("note.changed", {{"part", target.part->id}, {"notes", std::move(notes)}});
			}
			return {{"notes", changed}};
		}

		// quantize.lengths: a note value, ticks, or nothing for the grid's.
		std::optional<time::Ticks> lengthFrom(const Json& parameters)
		{
			if (!parameters.contains("length"))
				return std::nullopt;
			const Json& length = parameters.at("length");
			const char* message =
				"length must be a note value such as 1/8, 1/8T or 1/8., or a positive number of ticks";
			if (length.is_string())
			{
				const std::optional<NoteValue> value = noteValueFromString(length.get<std::string>());
				if (!value)
					throw CommandError(message);
				return value->ticks();
			}
			require(length.get<time::Ticks>() > 0, message);
			return length.get<time::Ticks>();
		}

		Json settingsResult(const Project& project)
		{
			return {{"settings", toJson(project.quantize)}, {"autoQuantize", project.autoQuantize}};
		}
	}

	void registerQuantizeCommands(ProjectModel& model, Engine& engine)
	{
		const auto presets = std::make_shared<PresetLibrary>();
		const Json idSchema = {{"type", "integer"}};
		const Json stringSchema = {{"type", "string"}};
		const Json settingsSchema = quantizeSettingsSchema();
		const Json notesResult = {
			{"type", "object"}, {"properties", {{"notes", {{"type", "array"}}}}}, {"required", {"notes"}}};
		const Json presetResult = {
			{"type", "object"}, {"properties", {{"preset", {{"type", "object"}}}}}, {"required", {"preset"}}};
		const Json settingsResultSchema = {{"type", "object"},
			{"properties", {{"settings", {{"type", "object"}}}, {"autoQuantize", {{"type", "boolean"}}}}},
			{"required", {"settings", "autoQuantize"}}};

		const Json targetProperties = {
			{"part", idSchema}, {"notes", arraySchema(idSchema)}, {"parts", arraySchema(idSchema)}};
		Json withSettings = targetProperties;
		withSettings["preset"] = stringSchema;
		withSettings["settings"] = settingsSchema;
		Json withLength = withSettings;
		withLength["length"] = {{"type", {"string", "integer"}}};

		const std::string targetText =
			"Takes the notes of part (only notes, if given) or of every part in parts. The settings are the project's "
			"(see quantize.set), or preset's, changed by settings.";
		const std::string originText =
			"Quantizing starts from the notes' original timing unless settings.soft is true, and keeps it, so "
			"quantize.reset puts it back. Returns the notes that changed.";

		model.addEdit(engine,
			{"quantize.apply", 1,
				"Quantizes note starts: moves them toward the nearest position of the grid, or of the groove, and "
				"keeps "
				"their lengths. " +
					targetText + " " + originText +
					" Grids are in project time and start again at every bar; notes don't move before their part's "
					"start.",
				objectSchema(withSettings), notesResult, false},
			"Quantize",
			[presets](Project& project, CommandContext& context)
			{
				const QuantizeGrid grid(settingsFor(project, *presets, context.parameters()), project.signatures);
				return forNotes(project, context,
					[&grid](Note& note, time::Ticks partStart) { return quantizeStart(note, partStart, grid); });
			});
		model.addEdit(engine,
			{"quantize.ends", 1,
				"Quantizes note ends: moves them toward the nearest grid position and keeps the starts. " + targetText +
					" " + originText,
				objectSchema(withSettings), notesResult, false},
			"Quantize ends",
			[presets](Project& project, CommandContext& context)
			{
				const QuantizeGrid grid(settingsFor(project, *presets, context.parameters()), project.signatures);
				return forNotes(project, context,
					[&grid](Note& note, time::Ticks partStart) { return quantizeEnd(note, partStart, grid); });
			});
		model.addEdit(engine,
			{"quantize.lengths", 1,
				"Sets note lengths and keeps the starts: to length, a note value (\"1/8\", \"1/8T\", \"1/8.\") or "
				"ticks, "
				"or without length to the step of the grid at each note's start, so tuplets and swing count and notes "
				"outside the catch range keep their length. " +
					targetText + " quantize.reset puts the lengths back. Returns the notes that changed.",
				objectSchema(withLength), notesResult, false},
			"Quantize lengths",
			[presets](Project& project, CommandContext& context)
			{
				const std::optional<time::Ticks> length = lengthFrom(context.parameters());
				const QuantizeGrid grid(settingsFor(project, *presets, context.parameters()), project.signatures);
				return forNotes(project, context, [&grid, length](Note& note, time::Ticks partStart)
					{ return quantizeLength(note, partStart, grid, length); });
			});
		model.addEdit(engine,
			{"quantize.reset", 1,
				"Puts quantized notes back to their timing before quantizing: starts, lengths and velocities. Notes "
				"changed by other means since keep their timing. Takes the notes of part (only notes, if given) or of "
				"every part in parts and returns the notes that changed.",
				objectSchema(targetProperties), notesResult, false},
			"Reset quantize", [](Project& project, CommandContext& context)
			{ return forNotes(project, context, [](Note& note, time::Ticks) { return resetQuantize(note); }); });
		model.addEdit(engine,
			{"quantize.freeze", 1,
				"Makes quantized notes' timing permanent: quantizing starts from it from now on, and quantize.reset no "
				"longer goes back. Takes the notes of part (only notes, if given) or of every part in parts and "
				"returns "
				"the notes that changed.",
				objectSchema(targetProperties), notesResult, false},
			"Freeze quantize", [](Project& project, CommandContext& context)
			{ return forNotes(project, context, [](Note& note, time::Ticks) { return freezeQuantize(note); }); });

		engine.registerCommand(
			{"quantize.get", 1,
				"The project's quantize settings and whether auto quantize is on (see quantize.set for the settings).",
				objectSchema({}), settingsResultSchema, true},
			[&model](CommandContext&) { return settingsResult(model.project()); });
		model.addEdit(engine,
			{"quantize.set", 1,
				"Changes the project's quantize settings: to preset's, if given, then changed by settings. Settings: "
				"grid, a note value from \"1/1\" to \"1/128\", \"T\" for a triplet and \".\" for dotted; tuplet, 0 "
				"(off) "
				"or 3 to 15 notes in the time of the largest power of two below it; swing 0-1, moving every second "
				"grid "
				"position later, at 1 by a third of a step (straight grids without tuplets only); catchRange, ticks "
				"from a grid position within which notes are quantized (0: all); safeRange, ticks within which they "
				"stay; randomize, ticks by which quantized notes move at random, by seed; soft, starting from the "
				"current positions instead of the original ones; strength 0-1, the part of the way notes move. groove "
				"{length, points: [{position, velocity 0-1, length}]} quantizes to a groove instead of the grid, with "
				"preQuantize (a note value), maxMove (ticks), velocityStrength, lengthStrength and origin (where it "
				"starts repeating, in project ticks). autoQuantize is whether recording quantizes. Settings are not "
				"undo steps; they are saved with the project.",
				objectSchema(
					{{"preset", stringSchema}, {"settings", settingsSchema}, {"autoQuantize", {{"type", "boolean"}}}}),
				settingsResultSchema, false},
			"Change quantize settings",
			[presets](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				project.quantize = settingsFor(project, *presets, parameters);
				project.autoQuantize = parameters.value("autoQuantize", project.autoQuantize);
				Json result = settingsResult(project);
				context.emit("quantize.changed", result);
				return result;
			});

		engine.registerCommand(
			{"quantize.presets", 1,
				"The quantize presets: the factory presets (straight grids \"1/1\" to \"1/128\", triplets such as "
				"\"1/8 Triplet\" and dotted grids such as \"1/4 Dotted\") and the user's, each with its settings.",
				objectSchema({}),
				{{"type", "object"}, {"properties", {{"presets", {{"type", "array"}}}}}, {"required", {"presets"}}},
				true},
			[presets](CommandContext&) { return Json{{"presets", presets->toJson()}}; });
		engine.registerCommand(
			{"quantize.savePreset", 1,
				"Saves the project's quantize settings, changed by settings, as a preset, replacing the user's preset "
				"of that name. Factory presets can't be replaced. Presets belong to GNUbase, not to the project.",
				objectSchema({{"name", stringSchema}, {"settings", settingsSchema}}, {"name"}), presetResult, false},
			[presets, &model](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const QuantizePreset preset{parameters.at("name").get<std::string>(),
					changedSettings(parameters.value("settings", Json::object()), model.project().quantize)};
				presets->save(preset);
				presets->announce(context);
				return Json{{"preset", PresetLibrary::presetJson(preset, false)}};
			});
		engine.registerCommand(
			{"quantize.renamePreset", 1, "Renames one of the user's quantize presets.",
				objectSchema({{"name", stringSchema}, {"newName", stringSchema}}, {"name", "newName"}), Json::object(),
				false},
			[presets](CommandContext& context)
			{
				presets->rename(context.parameters().at("name").get<std::string>(),
					context.parameters().at("newName").get<std::string>());
				presets->announce(context);
				return Json::object();
			});
		engine.registerCommand({"quantize.removePreset", 1, "Removes one of the user's quantize presets.",
								   objectSchema({{"name", stringSchema}}, {"name"}), Json::object(), false},
			[presets](CommandContext& context)
			{
				presets->remove(context.parameters().at("name").get<std::string>());
				presets->announce(context);
				return Json::object();
			});
		engine.registerCommand(
			{"quantize.extractGroove", 1,
				"Makes a groove preset from a MIDI part's rhythm: a groove position at every note start, with the "
				"highest velocity and the longest length of the notes there. The groove is length ticks long, by "
				"default the part's length, and the preset is named name, by default after the part.",
				objectSchema({{"part", idSchema}, {"name", stringSchema}, {"length", idSchema}}, {"part"}),
				presetResult, false},
			[presets, &model](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const Project& project = model.project();
				const ObjectId id = parameters.at("part").get<ObjectId>();
				const auto found = project.midiParts.find(id);
				require(found != project.midiParts.end(), "no MIDI part " + std::to_string(id));
				const MidiPart& part = found->second;
				const time::Ticks start = time::toTicks(part.position, project.tempo, project.sampleRate);
				const time::Ticks end = time::toTicks(
					{part.position.base, part.position.value + part.length}, project.tempo, project.sampleRate);
				const auto length = parameters.value("length", end - start);
				require(length > 0, "length must be positive");
				QuantizePreset preset{parameters.value("name", part.name), {}};
				preset.settings.groove = extractGroove(part, length);
				require(!preset.settings.groove->points.empty(),
					"MIDI part " + std::to_string(id) + " has no notes to make a groove from");
				presets->save(preset);
				presets->announce(context);
				return Json{{"preset", PresetLibrary::presetJson(preset, false)}};
			});
	}
}
