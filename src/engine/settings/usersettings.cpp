// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/settings/usersettings.h"

#include "engine/engine.h"

#include <utility>

namespace gnubase::engine::settings
{
	namespace
	{
		Json pathOrNull(const std::filesystem::path& path)
		{
			return path.empty() ? Json(nullptr) : Json(path.u8string());
		}

		// The setting as text for command descriptions: "`ui.scale` (number, 0.5 to 4, default 1.0): The UI scale".
		std::string summary(const Definition& definition)
		{
			const Json& schema = definition.schema;
			std::string text = "`" + definition.key + "` (";
			if (const auto choices = schema.find("enum"); choices != schema.end())
				text += "one of " + choices->dump();
			else
				text += schema.value("type", "any");
			if (schema.contains("minimum") || schema.contains("maximum"))
				text +=
					", " + schema.value("minimum", Json("")).dump() + " to " + schema.value("maximum", Json("")).dump();
			std::string description = definition.description;
			if (!description.empty() && description.back() == '.')
				description.pop_back(); // the settings are separated by semicolons
			return text + ", default " + definition.defaultValue.dump() + "): " + description;
		}

		// ui.layout: the windows' zones and strips, and the panels in windows of their own (docs/features/docking.md).
		Json layoutSchema()
		{
			const Json strings = {{"type", "array"}, {"items", {{"type", "string"}}}};
			const Json zone = {{"type", "object"},
				{"properties",
					{{"visible", {{"type", "boolean"}}}, {"size", {{"type", "number"}, {"minimum", 0}}},
						{"active", {{"type", "string"}}}, {"tabs", strings}, {"hidden", strings}}}};
			const Json window = {{"type", "object"},
				{"properties",
					{{"zones", {{"type", "object"}, {"additionalProperties", zone}}},
						{"strips", {{"type", "object"}, {"additionalProperties", {{"type", "boolean"}}}}}}}};
			const Json integer = {{"type", "integer"}};
			const Json size = {{"type", "integer"}, {"minimum", 1}};
			const Json bounds = {{"type", "object"},
				{"properties", {{"x", integer}, {"y", integer}, {"width", size}, {"height", size}}},
				{"required", Json::array({"x", "y", "width", "height"})}};
			const Json panel = {
				{"type", "object"}, {"properties", {{"floating", {{"type", "boolean"}}}, {"bounds", bounds}}}};
			return {{"type", "object"},
				{"properties",
					{{"windows", {{"type", "object"}, {"additionalProperties", window}}},
						{"panels", {{"type", "object"}, {"additionalProperties", panel}}}}}};
		}
	}

	const Format& userFormat()
	{
		static const Format format = []
		{
			Format result;
			result.name = "settings";
			result.version = 1;
			result.definitions = {
				{"audio.autoMonitoring",
					"When record-enabled audio tracks monitor their input, besides the monitor button: manual never, "
					"whileRecordEnabled always, whileRecording while the transport records, tapeMachine while it "
					"stands or records.",
					Json{{"type", "string"},
						{"enum", {"manual", "whileRecordEnabled", "whileRecording", "tapeMachine"}}},
					"tapeMachine"},
				{"audio.backend", "The audio backend, such as wasapi or coreaudio, or empty for the preferred one.",
					Json{{"type", "string"}}, ""},
				{"audio.blockSize", "Frames per audio block.",
					Json{{"type", "integer"}, {"minimum", 16}, {"maximum", 8192}}, 256},
				{"audio.busPresets",
					"The user's own bus presets, per direction (input, output) by name: each a list of busses with "
					"their name, format, ports and child busses (bus.savePreset).",
					Json{{"type", "object"}}, Json{{"input", Json::object()}, {"output", Json::object()}}},
				{"audio.device", "The audio device's id, or empty for the backend's default device.",
					Json{{"type", "string"}}, ""},
				{"audio.portSettings",
					"How the audio devices' ports are shown (audio.setPort), by backend/device id/in or out/system "
					"name, also for devices that aren't connected: the name shown (showAs) and whether it is "
					"visible. Ports without an entry show their system name and are visible.",
					Json{{"type", "object"},
						{"additionalProperties",
							{{"type", "object"},
								{"properties", {{"showAs", {{"type", "string"}}}, {"visible", {{"type", "boolean"}}}}},
								{"additionalProperties", false}}}},
					Json::object()},
				{"audio.sampleRate", "The audio device's sample rate in Hz.",
					Json{{"type", "number"}, {"minimum", 8000}, {"maximum", 768000}}, 48000.0},
				{"midi.chaseBeyondParts",
					"Chases controller events from the whole track; off, only from the MIDI parts under the "
					"position, so none outside parts.",
					Json{{"type", "boolean"}}, true},
				{"midi.chaseControllers",
					"Where playback starts or jumps, or the cursor moves while stopped, MIDI tracks send each control "
					"change's last value before that position.",
					Json{{"type", "boolean"}}, true},
				{"midi.chaseNotes",
					"Where playback starts or jumps, MIDI tracks start the notes that sound at that position.",
					Json{{"type", "boolean"}}, true},
				{"midi.chasePitchBend",
					"Where playback starts or jumps, or the cursor moves while stopped, MIDI tracks send each "
					"channel's last pitch bend before that position.",
					Json{{"type", "boolean"}}, true},
				{"midi.chasePrograms",
					"Where playback starts or jumps, or the cursor moves while stopped, MIDI tracks send each "
					"channel's last program change before that position.",
					Json{{"type", "boolean"}}, true},
				{"midi.portSettings",
					"The MIDI ports' settings (midi.setPort), by backend/in or out/port id, also for ports that aren't "
					"connected: the name shown (showAs), whether it is visible, and for inputs whether it belongs to "
					"all MIDI inputs (inAllInputs). Ports without an entry show their system name, are visible and "
					"belong to all MIDI inputs.",
					Json{{"type", "object"},
						{"additionalProperties",
							{{"type", "object"},
								{"properties",
									{{"showAs", {{"type", "string"}}}, {"visible", {{"type", "boolean"}}},
										{"inAllInputs", {{"type", "boolean"}}}}},
								{"additionalProperties", false}}}},
					Json::object()},
				{"ui.layout",
					"The layout of the windows with zones: under windows, by window (project), its zones (left, "
					"right, lower) with whether each is visible, its size in dp, the order of its tabs, the tabs "
					"hidden and the active one, and its strips (toolbar, infoLine, overviewLine, statusLine, "
					"transportBar) with whether each is shown; under panels, by panel, whether it is in a window of "
					"its own and that window's bounds. What it leaves out has its default.",
					layoutSchema(), Json::object()},
				{"ui.scale", "The UI scale, applied on top of each monitor's own scale.",
					Json{{"type", "number"}, {"minimum", 0.5}, {"maximum", 4}}, 1.0},
				{"ui.theme", "The theme's folder name in resources/themes.", Json{{"type", "string"}}, "gnubase"},
				{"ui.zoomPresets",
					"The horizontal zoom presets of the event display, for every project: each a name and the "
					"seconds the view shows.",
					Json{{"type", "array"},
						{"items",
							{{"type", "object"},
								{"properties",
									{{"name", {{"type", "string"}}},
										{"seconds", {{"type", "number"}, {"minimum", 0}}}}},
								{"required", Json::array({"name", "seconds"})}}}},
					Json::array()},
			};
			return result;
		}();
		return format;
	}

	Json changeEntry(Json map, const std::string& key, const Json& changes, const Json& defaults)
	{
		Json& entry = map[key];
		if (entry.is_null())
			entry = Json::object();
		for (const auto& [name, value] : changes.items())
		{
			if (value == defaults.at(name))
				entry.erase(name);
			else
				entry[name] = value;
		}
		if (entry.empty())
			map.erase(key);
		return map;
	}

	void registerCommands(Engine& engine, Store& store, std::function<void()> changed)
	{
		Json keys = Json::array();
		std::string list;
		for (const Definition& definition : store.format().definitions)
		{
			keys.push_back(definition.key);
			list += (list.empty() ? "" : "; ") + summary(definition);
		}
		const Json key = {{"type", "string"}, {"enum", keys}, {"description", "The setting."}};
		const Json keyAndValue = {{"type", "object"},
			{"properties", {{"key", {{"type", "string"}}}, {"value", {{"description", "The setting's value."}}}}},
			{"required", Json::array({"key", "value"})}};

		engine.registerCommand({"settings.get", 1, "The value of a user setting; settings.list describes them all.",
								   {{"type", "object"}, {"properties", {{"key", key}}},
									   {"required", Json::array({"key"})}, {"additionalProperties", false}},
								   keyAndValue, true},
			[&store](CommandContext& context)
			{
				const std::string name = context.parameters().at("key").get<std::string>();
				return Json{{"key", name}, {"value", store.value(name)}};
			});

		engine.registerCommand(
			{"settings.set", 1,
				"Changes a user setting and saves the settings file, unless settings.list reports it read-only. User "
				"settings belong to the user, not to a project. Emits settings.changed with the changed value, "
				"unless it had that value already. The settings: " +
					list + ".",
				{{"type", "object"},
					{"properties",
						{{"key", key},
							{"value", {{"description", "The new value; it must fit the setting's schema."}}}}},
					{"required", Json::array({"key", "value"})}, {"additionalProperties", false}},
				keyAndValue, false},
			[&store, changed](CommandContext& context)
			{
				const std::string name = context.parameters().at("key").get<std::string>();
				const Json& value = context.parameters().at("value");
				if (store.set(name, value))
				{
					changed();
					context.emit("settings.changed", {{"values", {{name, value}}}});
				}
				return Json{{"key", name}, {"value", store.value(name)}};
			});

		engine.registerCommand(
			{"settings.reset", 1,
				"Puts a user setting back to its default, or every one without key, and saves the settings file. "
				"Returns the settings that were reset with their values; emits settings.changed with those that "
				"changed.",
				{{"type", "object"}, {"properties", {{"key", key}}}, {"additionalProperties", false}},
				{{"type", "object"}, {"properties", {{"values", {{"type", "object"}}}}},
					{"required", Json::array({"values"})}},
				false},
			[&store, changed = std::move(changed)](CommandContext& context)
			{
				Json reset = Json::object();
				Json values = Json::object();
				if (const auto name = context.parameters().find("key"); name != context.parameters().end())
				{
					const std::string setting = name->get<std::string>();
					if (store.reset(setting))
						reset[setting] = store.value(setting);
					values[setting] = store.value(setting);
				}
				else
				{
					for (const std::string& setting : store.reset())
						reset[setting] = store.value(setting);
					values = store.values();
				}
				if (!reset.empty())
				{
					changed();
					context.emit("settings.changed", {{"values", std::move(reset)}});
				}
				return Json{{"values", std::move(values)}};
			});

		engine.registerCommand(
			{"settings.list", 1,
				"Every user setting with its key, description, JSON schema, default and value; the settings file, "
				"null when settings are kept in memory only; its version; and what happened when it was loaded: the "
				"status (inMemory, missing, loaded, migrated, reset or newer), the version the file had, the "
				"problems found, where the original file was kept, and whether the file is read-only for this "
				"session, so that changes apply but aren't saved (a newer GNUbase's file, or one that couldn't be "
				"backed up or locked).",
				CommandInfo{}.parameters,
				{{"type", "object"},
					{"properties",
						{{"file", {{"type", Json::array({"string", "null"})}}}, {"version", {{"type", "integer"}}},
							{"load", {{"type", "object"}}}, {"settings", {{"type", "array"}}}}},
					{"required", Json::array({"file", "version", "load", "settings"})}},
				true},
			[&store](CommandContext&)
			{
				Json settings = Json::array();
				for (const Definition& definition : store.format().definitions)
					settings.push_back({{"key", definition.key}, {"description", definition.description},
						{"schema", definition.schema}, {"default", definition.defaultValue},
						{"value", store.value(definition.key)}});
				const LoadReport& report = store.loadReport();
				return Json{{"file", pathOrNull(store.file())}, {"version", store.format().version},
					{"load",
						{{"status", toString(report.status)}, {"fileVersion", report.fileVersion},
							{"problems", report.problems}, {"backup", pathOrNull(report.backup)},
							{"readOnly", report.readOnly}}},
					{"settings", std::move(settings)}};
			});
	}
}
