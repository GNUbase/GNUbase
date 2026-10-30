// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/midi/quantizecommands.h"

#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/midi/quantize.h"
#include "engine/schema.h"
#include "engine/settings/store.h"
#include "platform/log.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
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

		constexpr std::string_view g_presetsKey = "quantize.presets";

		// Whether an item of the presets file has that name.
		auto named(std::string name)
		{
			return [name = std::move(name)](const Json& item)
			{
				const auto found = item.find("name"); // end() unless it is an object
				return found != item.end() && *found == name;
			};
		}

		// The presets users saved, after the factory presets. Names are unique among both. The user's presets are
		// user settings: the store keeps them in the settings folder, and every change starts from the presets in
		// the file, so that no change drops the presets another GNUbase process saved meanwhile.
		class PresetLibrary
		{
		public:
			explicit PresetLibrary(const std::filesystem::path& settingsDirectory)
				: m_store(quantizePresetsFormat(), settingsDirectory)
			{
				m_user = read(m_store.value(g_presetsKey), &m_skipped);
			}

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
				const Json item = {{"name", preset.name}, {"settings", midi::toJson(preset.settings)}};
				std::string problem;
				if (!readPreset(item, problem)) // it would be saved, but not listed
					throw CommandError("the preset can't be saved: " + problem);
				change(
					[&preset, &item](Json& list, const std::vector<QuantizePreset>&)
					{
						if (const auto mine = userItem(list, preset.name); mine != list.end())
						{
							*mine = item;
							return;
						}
						require(std::none_of(list.begin(), list.end(), named(preset.name)),
							"a preset named " + preset.name +
								" is in the file but can't be read, such as one of a newer GNUbase; "
								"quantize.removePreset removes it");
						list.push_back(item);
					});
			}

			// Removes the user's preset of that name, and the presets of that name that this GNUbase can't read.
			void remove(const std::string& name)
			{
				require(!isFactory(name), "factory presets can't be removed");
				change(
					[&name](Json& list, const std::vector<QuantizePreset>&)
					{
						const auto end = std::remove_if(list.begin(), list.end(), named(name));
						if (end == list.end())
							throw CommandError("no quantize preset " + name);
						list.erase(end, list.end());
					});
			}

			void rename(const std::string& name, const std::string& newName)
			{
				require(!isFactory(name), "factory presets can't be renamed");
				change(
					[&name, &newName](Json& list, const std::vector<QuantizePreset>& user)
					{
						requireUserPreset(user, name);
						require(!newName.empty(), "newName must not be empty");
						require(!isFactory(newName), newName + " is a factory preset");
						if (newName == name)
							return;
						// Also one this GNUbase can't read: it stays for the GNUbase that wrote it.
						require(std::none_of(list.begin(), list.end(), named(newName)),
							"a preset named " + newName + " exists");
						(*userItem(list, name))["name"] = newName;
					});
			}

			// Where the presets are kept, whether changes are saved, and what was wrong with the file.
			Json report() const
			{
				const settings::LoadReport& load = m_store.loadReport();
				Json problems = load.problems;
				for (const std::string& problem : m_skipped)
					problems.push_back(problem);
				return {{"file", m_store.file().empty() ? Json(nullptr) : Json(m_store.file().u8string())},
					{"readOnly", load.readOnly}, {"problems", std::move(problems)}};
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
			using Edit = std::function<void(Json& list, const std::vector<QuantizePreset>& user)>;

			static bool hasUserPreset(const std::vector<QuantizePreset>& user, const std::string& name)
			{
				return std::any_of(
					user.begin(), user.end(), [&name](const QuantizePreset& p) { return p.name == name; });
			}

			static void requireUserPreset(const std::vector<QuantizePreset>& user, const std::string& name)
			{
				if (!hasUserPreset(user, name))
					throw CommandError("no quantize preset " + name);
			}

			// A preset of the file, or nothing and why this GNUbase can't read it.
			static std::optional<QuantizePreset> readPreset(const Json& item, std::string& problem)
			{
				const Json schema = {{"type", "object"},
					{"properties", {{"name", {{"type", "string"}}}, {"settings", quantizeSettingsSchema()}}},
					{"required", {"name", "settings"}}};
				problem = validate(item, schema, "preset");
				if (!problem.empty())
					return std::nullopt;
				QuantizePreset preset{item.at("name").get<std::string>(), {}};
				if (preset.name.empty())
					problem = "it has no name";
				else if (isFactory(preset.name))
					problem = "a factory preset has its name";
				if (!problem.empty())
					return std::nullopt;
				try
				{
					preset.settings = quantizeSettingsFromJson(item.at("settings"));
				}
				catch (const std::invalid_argument& error)
				{
					problem = std::string("preset.settings.") + error.what();
					return std::nullopt;
				}
				return preset;
			}

			// The item of the file that is the user's preset of that name: the first one of that name that this
			// GNUbase can read.
			static Json::iterator userItem(Json& list, const std::string& name)
			{
				return std::find_if(list.begin(), list.end(),
					[&name](const Json& item)
					{
						std::string problem;
						return named(name)(item) && readPreset(item, problem).has_value();
					});
			}

			// Changes the list of presets in the file as it is now, then uses what the file has: also when the
			// change fails, since another process may have changed the file. If the file can't be written, the
			// presets don't change.
			void change(const Edit& edit)
			{
				Json list;
				try
				{
					list = m_store.modify(g_presetsKey,
						[this, &edit](const Json& value)
						{
							m_user = read(value);
							Json changed = value;
							edit(changed, m_user);
							return changed;
						});
				}
				catch (const settings::SettingsError& error)
				{
					throw CommandError(std::string("cannot save the quantize presets: ") + error.what());
				}
				m_user = read(list);
			}

			// The presets of the list that this GNUbase can read. The others are skipped, and stay in the file for
			// the GNUbase that wrote them; with skipped, the log and skipped say why.
			std::vector<QuantizePreset> read(const Json& list, std::vector<std::string>* skipped = nullptr) const
			{
				std::vector<QuantizePreset> presets;
				for (size_t i = 0; i < list.size(); ++i)
				{
					std::string problem;
					std::optional<QuantizePreset> preset = readPreset(list[i], problem);
					if (preset && hasUserPreset(presets, preset->name))
					{
						problem = "an earlier preset has its name";
						preset.reset();
					}
					if (preset)
					{
						presets.push_back(std::move(*preset));
						continue;
					}
					if (skipped == nullptr)
						continue;
					skipped->push_back(m_store.file().u8string() + ": skipped preset " + std::to_string(i + 1) + ": " +
						problem + "; it stays in the file");
					logging::warning("Quantize presets: {}", skipped->back());
				}
				return presets;
			}

			settings::Store m_store;            // only the command thread uses it
			std::vector<std::string> m_skipped; // why presets of the file weren't read when the engine started
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

		PartSpan spanOf(const Project& project, const MidiPart& part)
		{
			const time::Position end{part.position.base, part.position.value + part.length};
			return {time::toTicks(part.position, project.tempo, project.sampleRate),
				time::toTicks(end, project.tempo, project.sampleRate)};
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

		using NoteFunction = std::function<bool(Note& note, const PartSpan& part)>;

		// Runs function on every target note and announces the notes it changed, per part.
		Json forNotes(Project& project, CommandContext& context, const NoteFunction& function)
		{
			Json changed = Json::array();
			for (const Target& target : targets(project, context.parameters()))
			{
				const PartSpan span = spanOf(project, *target.part);
				Json notes = Json::array();
				for (const ObjectId id : target.notes)
				{
					Note& note = target.part->notes.at(id);
					if (function(note, span))
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

	const settings::Format& quantizePresetsFormat()
	{
		static const settings::Format format{"quantize-presets", 1,
			{{std::string(g_presetsKey), "The user's quantize presets.", Json{{"type", "array"}}, Json::array()}}, {}};
		return format;
	}

	void registerQuantizeCommands(ProjectModel& model, Engine& engine)
	{
		const auto presets = std::make_shared<PresetLibrary>(engine.settingsDirectory());
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
			"quantize.reset puts it back. Notes moved by other means since they were quantized start from where they "
			"are; velocities and lengths changed by other means stay. Returns the notes that changed.";

		model.addEdit(engine,
			{"quantize.apply", 1,
				"Quantizes note starts: moves them toward the nearest position of the grid, or of the groove, and "
				"keeps "
				"their lengths. " +
					targetText + " " + originText +
					" Grids are in project time and start again at every bar; notes don't move before their part's "
					"start or to or past its end.",
				objectSchema(withSettings), notesResult, false},
			"Quantize",
			[presets](Project& project, CommandContext& context)
			{
				const QuantizeGrid grid(settingsFor(project, *presets, context.parameters()), project.signatures);
				return forNotes(project, context,
					[&grid](Note& note, const PartSpan& part) { return quantizeStart(note, part, grid); });
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
					[&grid](Note& note, const PartSpan& part) { return quantizeEnd(note, part, grid); });
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
				return forNotes(project, context, [&grid, length](Note& note, const PartSpan& part)
					{ return quantizeLength(note, part, grid, length); });
			});
		model.addEdit(engine,
			{"quantize.reset", 1,
				"Puts quantized notes back to their timing before quantizing: starts, lengths and velocities. Notes "
				"moved by other means since keep their timing, and velocities and lengths changed by other means stay. "
				"Takes the notes of part (only notes, if given) or of "
				"every part in parts and returns the notes that changed.",
				objectSchema(targetProperties), notesResult, false},
			"Reset quantize", [](Project& project, CommandContext& context)
			{ return forNotes(project, context, [](Note& note, const PartSpan&) { return resetQuantize(note); }); });
		model.addEdit(engine,
			{"quantize.freeze", 1,
				"Makes quantized notes' timing permanent: quantizing starts from it from now on, and quantize.reset no "
				"longer goes back. Takes the notes of part (only notes, if given) or of every part in parts and "
				"returns "
				"the notes that changed.",
				objectSchema(targetProperties), notesResult, false},
			"Freeze quantize", [](Project& project, CommandContext& context)
			{ return forNotes(project, context, [](Note& note, const PartSpan&) { return freezeQuantize(note); }); });

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
				"\"1/8 Triplet\" and dotted grids such as \"1/4 Dotted\") and the user's, each with its settings. The "
				"user's presets are kept in the settings folder, so they come back when GNUbase starts again: file is "
				"where (null if they are kept in memory only), readOnly whether changes are saved (not while the file "
				"belongs to a newer GNUbase), and problems what was wrong with it, such as presets that this GNUbase "
				"can't read and skips; they stay in the file.",
				objectSchema({}),
				{{"type", "object"},
					{"properties",
						{{"presets", {{"type", "array"}}}, {"file", {{"type", {"string", "null"}}}},
							{"readOnly", {{"type", "boolean"}}}, {"problems", {{"type", "array"}}}}},
					{"required", {"presets", "file", "readOnly", "problems"}}},
				true},
			[presets](CommandContext&)
			{
				Json result = presets->report();
				result["presets"] = presets->toJson();
				return result;
			});
		engine.registerCommand(
			{"quantize.savePreset", 1,
				"Saves the project's quantize settings, changed by settings, as a preset, replacing the user's preset "
				"of that name. Factory presets can't be replaced. Presets belong to the user, not to the project: they "
				"are saved in the settings folder.",
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
		engine.registerCommand({"quantize.removePreset", 1,
								   "Removes one of the user's quantize presets, and the presets of that name "
								   "that this GNUbase can't read (see quantize.presets).",
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
				"default the part's length, and the preset is named name, by default after the part. The groove's "
				"origin is the part's start, so it repeats in phase with the part.",
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
				const PartSpan span = spanOf(project, part);
				const auto length = parameters.value("length", span.end - span.start);
				require(length > 0, "length must be positive");
				QuantizePreset preset{parameters.value("name", part.name), {}};
				preset.settings.groove = extractGroove(part, length);
				// The points are relative to the part's start: the groove repeats from there, in phase with the part.
				preset.settings.origin = span.start;
				require(!preset.settings.groove->points.empty(),
					"MIDI part " + std::to_string(id) + " has no notes to make a groove from");
				presets->save(preset);
				presets->announce(context);
				return Json{{"preset", PresetLibrary::presetJson(preset, false)}};
			});
	}
}
