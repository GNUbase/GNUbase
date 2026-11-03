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

		// ui.layout: the windows' zones, strips and sections, and the panels in windows of their own
		// (docs/features/docking.md).
		Json layoutSchema()
		{
			const Json strings = {{"type", "array"}, {"items", {{"type", "string"}}}};
			const Json zone = {{"type", "object"},
				{"properties",
					{{"visible", {{"type", "boolean"}}}, {"size", {{"type", "number"}, {"minimum", 0}}},
						{"active", {{"type", "string"}}}, {"tabs", strings}, {"hidden", strings}}}};
			const Json sections = {{"type", "object"}, {"properties", {{"order", strings}, {"hidden", strings}}}};
			const Json window = {{"type", "object"},
				{"properties",
					{{"zones", {{"type", "object"}, {"additionalProperties", zone}}},
						{"strips", {{"type", "object"}, {"additionalProperties", {{"type", "boolean"}}}}},
						{"sections", {{"type", "object"}, {"additionalProperties", sections}}}}}};
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

		// ui.toolModifiers: by action, the modifier keys (docs/features/tools.md).
		Json toolModifiersSchema()
		{
			const Json keys = {{"type", "string"},
				{"enum", {"", "ctrl", "alt", "shift", "ctrl+alt", "ctrl+shift", "alt+shift", "ctrl+alt+shift"}}};
			Json properties = Json::object();
			for (const char* action : {"dragDrop.copy", "dragDrop.sharedCopy", "erase.deleteLaterEvents",
					 "glue.glueAllFollowing", "range.selectFullVertical", "select.setCursorPosition",
					 "select.splitEvent", "sizeObjects.disableSnapping", "split.splitRepeated"})
				properties[action] = keys;
			return {{"type", "object"}, {"properties", properties}, {"additionalProperties", false}};
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
					"visible. Ports without an entry show their system name and are visible. Members of an entry that "
					"this GNUbase doesn't know, from a later one, stay.",
					Json{{"type", "object"},
						{"additionalProperties",
							{{"type", "object"},
								{"properties",
									{{"showAs", {{"type", "string"}}}, {"visible", {{"type", "boolean"}}}}}}}},
					Json::object()},
				{"audio.fallbackDevice",
					"The device that plays while the chosen audio device is lost, until it returns: none, default (the "
					"lost device's backend's default device, or the dummy device if that doesn't open), or dummy (the "
					"dummy device: no sound, but the transport moves and the project plays on). The engine switches "
					"back once the lost device returns.",
					Json{{"type", "string"}, {"enum", {"none", "default", "dummy"}}}, "none"},
				{"audio.preRecordSeconds",
					"Audio pre-record: how many seconds of input from before recording starts go into the files of "
					"record-enabled audio tracks, 0 to 60 (0: none). The events start where recording started; "
					"resizing an event's start to the left brings out what was played before.",
					Json{{"type", "number"}, {"minimum", 0}, {"maximum", 60}}, 0.0},
				{"audio.stopAfterPunchOut",
					"Stop after automatic punch out: playback stops when recording punches out at the right locator, "
					"after the post-roll if it's on (see transport.set), unless it cycles.",
					Json{{"type", "boolean"}}, false},
				{"audio.sampleRate", "The audio device's sample rate in Hz.",
					Json{{"type", "number"}, {"minimum", 8000}, {"maximum", 768000}}, 48000.0},
				{"editing.applyFolderColor",
					"Folder tracks give their color to the tracks inside them: when the folder's color changes and "
					"when tracks are added or moved into it.",
					Json{{"type", "boolean"}}, false},
				{"editing.autoTrackColor",
					"The color new tracks get when none is given (reference manual p. 1621): default (the default "
					"color), previous (the color of the track above), previousPlusOne (the color after that one in "
					"the project's color set, or its first color), lastApplied (the color last given to events or "
					"parts) or random (a color of the set). With editing.applyFolderColor, a folder's color wins "
					"inside it.",
					Json{{"type", "string"},
						{"enum", {"default", "previous", "previousPlusOne", "lastApplied", "random"}}},
					"default"},
				{"meter.resetOnStart",
					"Reset on Start: the loudness meter, and the main mix's peak and RMS maxima, start over when "
					"playback starts (see meter.get).",
					Json{{"type", "boolean"}}, false},
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
					"belong to all MIDI inputs. Members of an entry that this GNUbase doesn't know, from a later one, "
					"stay.",
					Json{{"type", "object"},
						{"additionalProperties",
							{{"type", "object"},
								{"properties",
									{{"showAs", {{"type", "string"}}}, {"visible", {{"type", "boolean"}}},
										{"inAllInputs", {{"type", "boolean"}}}}}}}},
					Json::object()},
				{"project.autoSave",
					"Saves a copy of a project with unsaved changes every project.autoSaveInterval minutes into the "
					"folder Auto Saves of its project folder (see project.autoSave).",
					Json{{"type", "boolean"}}, true},
				{"project.autoSaveFiles", "How many auto saves a project keeps; the next one overwrites the oldest.",
					Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}, 5},
				{"project.autoSaveInterval", "Minutes between auto saves.",
					Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 120}}, 15},
				{"ui.folderData",
					"When folder tracks show the events and parts of the tracks inside them as data blocks in the "
					"event display: always, never, or hideWhenExpanded (only while the folder is folded).",
					Json{{"type", "string"}, {"enum", {"always", "never", "hideWhenExpanded"}}}, "always"},
				{"ui.layout",
					"The layout of the windows with zones: under windows, by window (project), its zones (left, "
					"right, lower) with whether each is visible, its size in dp, the order of its tabs, the tabs "
					"hidden and the active one, and its strips (toolbar, infoLine, overviewLine, statusLine, "
					"transportBar) with whether each is shown, and its sets of sections (such as a mixer channel's) "
					"with the order of their sections and those hidden; under panels, by panel, whether it is in a "
					"window of "
					"its own and that window's bounds. What it leaves out has its default.",
					layoutSchema(), Json::object()},
				{"ui.markerLines",
					"Which markers the event display shows as lines across every track: the active marker track's "
					"(active), every marker track's (all), or none (off).",
					Json{{"type", "string"}, {"enum", Json::array({"active", "all", "off"})}}, "active"},
				{"ui.scale", "The UI scale, applied on top of each monitor's own scale.",
					Json{{"type", "number"}, {"minimum", 0.5}, {"maximum", 4}}, 1.0},
				{"ui.theme", "The theme's folder name in resources/themes.", Json{{"type", "string"}}, "gnubase"},
				{"ui.timeFormats",
					"The time formats of the transport controls' displays: primary and secondary in the transport bar "
					"and panel, timeDisplay in the time display window; each barsBeats, seconds, timecode, samples or "
					"userFrames.",
					Json{{"type", "object"},
						{"additionalProperties",
							{{"type", "string"},
								{"enum", {"barsBeats", "seconds", "timecode", "samples", "userFrames"}}}}},
					Json{{"primary", "barsBeats"}, {"secondary", "timecode"}, {"timeDisplay", "barsBeats"}}},
				{"ui.toolModifiers",
					"The modifier keys of the project window's tools, by action (docs/features/tools.md), each ctrl "
					"(Cmd on macOS), alt and shift joined with +, or empty for none: dragDrop.copy (default alt), "
					"dragDrop.sharedCopy (alt+shift), erase.deleteLaterEvents (alt), glue.glueAllFollowing (alt), "
					"range.selectFullVertical (alt+shift), select.setCursorPosition (alt+shift), select.splitEvent "
					"(alt), sizeObjects.disableSnapping (ctrl) and split.splitRepeated (alt). What it leaves out has "
					"its default.",
					toolModifiersSchema(), Json::object()},
				{"ui.trackControls",
					"The controls the track list shows, by track type (audio, instrument, midi, group, effect, "
					"folder, marker, ruler), in order: mute, solo, record, monitor (audio tracks), read (automation) "
					"and timeBase. Mute and solo always show on tracks with a channel; a type left out shows none.",
					Json{{"type", "object"},
						{"additionalProperties",
							{{"type", "array"},
								{"items",
									{{"type", "string"},
										{"enum", {"mute", "solo", "record", "monitor", "read", "timeBase"}}}}}}},
					Json{{"audio", {"mute", "solo", "record", "monitor", "read"}},
						{"instrument", {"mute", "solo", "read"}}, {"midi", {"mute", "solo", "read"}},
						{"group", {"mute", "solo", "read"}}, {"effect", {"mute", "solo", "read"}},
						{"folder", {"mute", "solo", "read"}}, {"marker", Json::array()}, {"ruler", Json::array()}}},
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

	void registerCommands(Engine& engine, Store& store, Changed changed)
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
					context.emit("settings.changed", {{"values", {{name, value}}}});
					changed(context, {name});
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
					std::vector<std::string> keys;
					keys.reserve(reset.size());
					for (const auto& entry : reset.items())
						keys.push_back(entry.key());
					context.emit("settings.changed", {{"values", std::move(reset)}});
					changed(context, keys);
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
