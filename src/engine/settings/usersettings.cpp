// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/settings/usersettings.h"

#include "engine/engine.h"
#include "engine/midi/controllerlanes.h"
#include "engine/midi/devices.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

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

		// ui.layout: the windows' zones, strips, sections and the sizes of other elements that splitters size, and the
		// panels in windows of their own (docs/features/docking.md).
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
						{"sections", {{"type", "object"}, {"additionalProperties", sections}}},
						{"sizes",
							{{"type", "object"}, {"additionalProperties", {{"type", "number"}, {"minimum", 0}}}}}}}};
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

		Json markerColumnsSchema()
		{
			const Json column = {
				{"type", "string"}, {"enum", {"number", "type", "position", "end", "length", "description"}}};
			return {{"type", "object"},
				{"properties",
					{{"order", {{"type", "array"}, {"items", column}}},
						{"widths",
							{{"type", "object"},
								{"additionalProperties", {{"type", "number"}, {"minimum", 16}, {"maximum", 2000}}}}},
						{"sort", column}, {"descending", {{"type", "boolean"}}}}}};
		}
	}

	namespace
	{
		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		// Emits settings.changed with the new values of the settings that changed, and then what follows from them.
		void emitChanged(
			CommandContext& context, const Store& store, const std::vector<std::string>& keys, const Changed& changed)
		{
			if (keys.empty())
				return;
			Json values = Json::object();
			for (const std::string& key : keys)
				values[key] = store.value(key);
			context.emit("settings.changed", {{"values", std::move(values)}});
			changed(context, keys);
		}

		// Changes preferences.presets from its value in the settings file, so that presets another GNUbase process
		// saved meanwhile stay.
		void changePresets(CommandContext& context, Store& store, const Changed& changed,
			const std::function<void(Json& presets)>& change)
		{
			const Json before = store.value("preferences.presets");
			const Json after = store.modify("preferences.presets",
				[&change](const Json& value)
				{
					Json presets = value;
					change(presets);
					return presets;
				});
			if (after != before)
				emitChanged(context, store, {"preferences.presets"}, changed);
		}

		void registerPresetCommands(Engine& engine, Store& store, const Changed& changed)
		{
			const Json name = {{"type", "string"}, {"description", "The preset's name."}};
			const Json noResult = {{"type", "object"}};
			engine.registerCommand(
				{"preferences.savePreset", 1,
					"Saves a preferences preset (the user setting preferences.presets, reference manual p. 1594): "
					"values, by setting, of all the preferences or only some, as the preferences dialog shows them. "
					"Replaces a preset of the same name. Changes no preference; settings.set with its values applies "
					"it.",
					{{"type", "object"},
						{"properties",
							{{"name", name},
								{"values",
									{{"type", "object"},
										{"description", "The values by setting; each must fit its setting."}}}}},
						{"required", Json::array({"name", "values"})}, {"additionalProperties", false}},
					noResult, false},
				[&store, changed](CommandContext& context)
				{
					const auto preset = context.parameters().at("name").get<std::string>();
					require(!preset.empty(), "name must not be empty");
					const Json& values = context.parameters().at("values");
					const auto& definitions = store.format().definitions;
					for (const auto& item : values.items())
					{
						const std::string& setting = item.key();
						const Json& value = item.value();
						const auto found = std::find_if(definitions.begin(), definitions.end(),
							[&setting](const Definition& definition) { return definition.key == setting; });
						require(found != definitions.end() && setting != "preferences.presets",
							setting + " is no preference");
						if (const std::string problem = validate(value, found->schema, setting); !problem.empty())
							throw CommandError(problem);
					}
					changePresets(context, store, changed, [&](Json& presets) { presets[preset] = values; });
					return Json::object();
				});
			engine.registerCommand(
				{"preferences.renamePreset", 1, "Renames one of the preferences presets.",
					{{"type", "object"}, {"properties", {{"name", name}, {"newName", name}}},
						{"required", Json::array({"name", "newName"})}, {"additionalProperties", false}},
					noResult, false},
				[&store, changed](CommandContext& context)
				{
					const auto preset = context.parameters().at("name").get<std::string>();
					const auto newName = context.parameters().at("newName").get<std::string>();
					require(!newName.empty(), "newName must not be empty");
					changePresets(context, store, changed,
						[&](Json& presets)
						{
							require(presets.contains(preset), "no preferences preset " + preset);
							if (newName == preset)
								return;
							require(!presets.contains(newName), "a preferences preset " + newName + " exists already");
							presets[newName] = presets.at(preset);
							presets.erase(preset);
						});
					return Json::object();
				});
			engine.registerCommand({"preferences.removePreset", 1, "Removes one of the preferences presets.",
									   {{"type", "object"}, {"properties", {{"name", name}}},
										   {"required", Json::array({"name"})}, {"additionalProperties", false}},
									   noResult, false},
				[&store, changed](CommandContext& context)
				{
					const auto preset = context.parameters().at("name").get<std::string>();
					changePresets(context, store, changed, [&preset](Json& presets)
						{ require(presets.erase(preset) == 1, "no preferences preset " + preset); });
					return Json::object();
				});
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
				{"audio.backend",
					"The backend of the audio device that the windowed app opens when it starts, such as asio, wasapi "
					"or "
					"coreaudio, or empty for the platform's default backend (see audio.setDevice). audio.setDevice "
					"with remember saves it, as Studio Setup does; a backend this build doesn't have opens the dummy "
					"device instead.",
					Json{{"type", "string"}}, ""},
				{"audio.blockSize", "Frames per audio block.",
					Json{{"type", "integer"}, {"minimum", 16}, {"maximum", 8192}}, 256},
				{"audio.busPresets",
					"The user's own bus presets, per direction (input, output) by name: each a list of busses with "
					"their name, format, ports and child busses (bus.savePreset).",
					Json{{"type", "object"}}, Json{{"input", Json::object()}, {"output", Json::object()}}},
				{"audio.defaultSendLevel",
					"The level of the sends that track.add sends to a new effect channel, and the one Ctrl-click "
					"gives a send in the mixer, in dB from -144 to 12 (reference manual p. 1623).",
					Json{{"type", "number"}, {"minimum", -144}, {"maximum", 12}}, 0.0},
				{"audio.device",
					"The id of the audio device that the windowed app opens when it starts (see audio.backend), or "
					"empty "
					"for the backend's default device. If it can't be opened, the app starts on the dummy device and "
					"Studio Setup says why.",
					Json{{"type", "string"}}, ""},
				{"audio.listenDim",
					"How far the main mix is turned down on the monitoring bus while channels listen (channel.set "
					"listen), in dB from -144 (silence: only the listening channels are heard) to 0.",
					Json{{"type", "number"}, {"minimum", -144}, {"maximum", 0}}, -144.0},
				{"audio.listenLevel",
					"The level at which listening channels play on the monitoring bus, in dB from -144 to 12.",
					Json{{"type", "number"}, {"minimum", -144}, {"maximum", 12}}, 0.0},
				{"audio.mutePreSends",
					"Mute Pre-Send when Mute: a channel's mute button also mutes its pre-fader sends (reference "
					"manual p. 1623); off, they play on while it is muted.",
					Json{{"type", "boolean"}}, false},
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
				{"editing.defaultTimeBase",
					"The time base of new tracks when track.add gives none (Default Track Time Type, reference manual "
					"pp. 230, 1595): musical (bars and beats, so their events follow tempo changes), linear (time, so "
					"they stay), or followTimeFormat: musical while the project's primary time format (project.set "
					"timeFormat) is barsBeats, linear with any other. Folder, ruler, tempo and signature tracks, "
					"which hold no events of their own, are musical.",
					Json{{"type", "string"}, {"enum", {"musical", "linear", "followTimeFormat"}}}, "musical"},
				{"editing.legatoOverlap",
					"Legato Overlap (reference manual p. 1124): how far MIDI > Functions > Legato extends notes past "
					"the start of the next note, in ticks of 480 per quarter note, -1920 to 1920; negative values "
					"leave a gap.",
					Json{{"type", "integer"}, {"minimum", -1920}, {"maximum", 1920}}, 0},
				{"editing.tempoType",
					"The type of new tempo events (reference manual p. 1448): ramp changes the tempo gradually from "
					"the event before, step jumps, auto takes the type of the event before (tempo.add, tempo.tap).",
					Json{{"type", "string"}, {"enum", {"ramp", "step", "auto"}}}, "auto"},
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
				{"midi.devices",
					"The installed MIDI devices (midiDevice.install), by name: each has banks, each bank a name and "
					"patches, each patch a name, a program (0 to 127), an optional bank (0 to 16383, sent as bank "
					"select MSB and LSB) and an optional folder it is listed in. A MIDI track that names a device "
					"(channel.set device) chooses its program and bank by patch.",
					midi::devicesSchema(), Json::object()},
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
				{"midi.recordCatchRange",
					"MIDI record catch range in ms, 0 to 1000: MIDI input that comes this long before a recording "
					"starts is recorded at its start, so that the very start isn't lost.",
					Json{{"type", "number"}, {"minimum", 0}, {"maximum", 1000}}, 0.0},
				{"midi.recordFilter",
					"What MIDI recording leaves out (it still plays): types, of notes, controllers, pitchBend, "
					"channelPressure, polyPressure and programs; channels, MIDI channels 1 to 16; controllers, "
					"controller numbers 0 to 127.",
					Json{{"type", "object"},
						{"properties",
							{{"types",
								 {{"type", "array"},
									 {"items",
										 {{"type", "string"},
											 {"enum",
												 {"notes", "controllers", "pitchBend", "channelPressure",
													 "polyPressure", "programs"}}}}}},
								{"channels",
									{{"type", "array"},
										{"items", {{"type", "integer"}, {"minimum", 1}, {"maximum", 16}}}}},
								{"controllers",
									{{"type", "array"},
										{"items", {{"type", "integer"}, {"minimum", 0}, {"maximum", 127}}}}}}}},
					Json::object()},
				{"midi.retrospectiveEvents",
					"How many MIDI events each MIDI or instrument track's retrospective buffer keeps of what came "
					"from its MIDI input while it didn't record (midi.insertRetrospective); 0 keeps none.",
					Json{{"type", "integer"}, {"minimum", 0}, {"maximum", 100000}}, 10000},
				{"midi.snapPartsToBars", "Recorded MIDI parts start and end at whole bars.", Json{{"type", "boolean"}},
					true},
				{"preferences.presets",
					"The preferences dialog's presets (reference manual pp. 1593-1595), by name: each the values of "
					"the user settings it sets, all of them or only some (preferences.savePreset). Applying one is "
					"settings.set with its values.",
					Json{{"type", "object"}, {"additionalProperties", {{"type", "object"}}}}, Json::object()},
				{"project.autoSave",
					"Saves a copy of a project with unsaved changes every project.autoSaveInterval minutes into the "
					"folder Auto Saves of its project folder (see project.autoSave).",
					Json{{"type", "boolean"}}, true},
				{"project.autoSaveFiles", "How many auto saves a project keeps; the next one overwrites the oldest.",
					Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 100}}, 5},
				{"project.autoSaveInterval", "Minutes between auto saves.",
					Json{{"type", "integer"}, {"minimum", 1}, {"maximum", 120}}, 15},
				{"transport.cycleFollowsMarkers",
					"Locating to a marker sets the left and right locators to it (marker.locate, marker.locateNext, "
					"marker.locatePrevious): a cycle marker's range, or from a position marker to the next marker.",
					Json{{"type", "boolean"}}, false},
				{"transport.tapTempoMode",
					"Where a tapped tempo goes (Tap Tempo Mode, reference manual p. 1458; see tempo.tap): auto, "
					"selection, playbackStart, cursor, project or display.",
					Json{{"type", "string"},
						{"enum", {"auto", "selection", "playbackStart", "cursor", "project", "display"}}},
					"auto"},
				{"ui.folderData",
					"When folder tracks show the events and parts of the tracks inside them as data blocks in the "
					"event display: always, never, or hideWhenExpanded (only while the folder is folded).",
					Json{{"type", "string"}, {"enum", {"always", "never", "hideWhenExpanded"}}}, "always"},
				{"ui.layout",
					"The layout of the windows with zones: under windows, by window (project), its zones (left, "
					"right, lower) with whether each is visible, its size in dp, the order of its tabs, the tabs "
					"hidden and the active one, and its strips (toolbar, infoLine, overviewLine, statusLine, "
					"transportBar) with whether each is shown, and its sets of sections (such as a mixer channel's) "
					"with the order of their sections and those hidden, and the sizes in dp of other elements that "
					"splitters size (trackList: the track list's width); under panels, by panel, whether it is in a "
					"window of its own and that window's bounds. What it leaves out has its default.",
					layoutSchema(), Json::object()},
				{"ui.markerLines",
					"Which markers the event display shows as lines across every track: the active marker track's "
					"(active), every marker track's (all), or none (off).",
					Json{{"type", "string"}, {"enum", Json::array({"active", "all", "off"})}}, "active"},
				{"ui.markerColumns",
					"The markers window's columns: order, the columns after the locate column in order (number, "
					"type, position, end, length, description); widths, each one's width in dp; sort, the column the "
					"list is sorted by, and descending. What it leaves out has its default.",
					markerColumnsSchema(), Json::object()},
				{"ui.markerIds", "Whether marker events on the marker tracks show their marker ids.",
					Json{{"type", "boolean"}}, true},
				{"ui.markersAutoScroll",
					"Whether the markers window scrolls to keep the marker at the project cursor (its locate arrow) "
					"in view.",
					Json{{"type", "boolean"}}, true},
				{"ui.markersSyncSelection",
					"Whether the markers window and the project window select the same markers. It pauses while "
					"recording.",
					Json{{"type", "boolean"}}, true},
				{"ui.removeTracksWarning",
					"Whether Remove Selected Tracks asks before it removes tracks that aren't empty: that hold events, "
					"parts, markers or automation, or are folders with such tracks inside (reference manual p. 213).",
					Json{{"type", "boolean"}}, true},
				{"ui.scale", "The UI scale, applied on top of each monitor's own scale.",
					Json{{"type", "number"}, {"minimum", 0.5}, {"maximum", 4}}, 1.0},
				{"ui.theme", "The theme's folder name in resources/themes.", Json{{"type", "string"}}, "gnubase"},
				{"ui.timeDisplay",
					"The time display window's options: beatCountOnly shows only the beat in its bar, size is the "
					"window's and its digits' size from 0 (smallest) to 4.",
					Json{{"type", "object"},
						{"properties",
							{{"beatCountOnly", {{"type", "boolean"}}},
								{"size", {{"type", "integer"}, {"minimum", 0}, {"maximum", 4}}}}}},
					Json{{"beatCountOnly", false}, {"size", 1}}},
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
				{"ui.toolSwitchNotification",
					"Whether choosing a project window tool with a key command shows the tool's name for a moment in "
					"the event display (docs/features/tools.md).",
					Json{{"type", "boolean"}}, true},
				{"ui.toolboxOnRightClick",
					"Whether a right click in the event display opens the toolbox, the toolbar's tools at the mouse, "
					"and a right click with a modifier key the context menu; when off, the other way round "
					"(docs/features/tools.md).",
					Json{{"type", "boolean"}}, false},
				{"ui.trackControls",
					"The controls the track list shows, by track type (audio, instrument, midi, group, effect, "
					"folder, marker, ruler, tempo), in order: mute, solo, record, monitor (audio, instrument and MIDI "
					"tracks), read (automation), timeBase, tempoTrack (the tempo track's Activate Tempo Track), and "
					"for "
					"ruler tracks timeFormat, timeOffsetOn, timeOffset and setTimeOffset (Set Time Offset to Project "
					"Cursor Position). "
					"Mute and solo always show on tracks with a channel; a type left out shows none.",
					Json{{"type", "object"},
						{"additionalProperties",
							{{"type", "array"},
								{"items",
									{{"type", "string"},
										{"enum",
											{"mute", "solo", "record", "monitor", "read", "timeBase", "tempoTrack",
												"timeFormat", "timeOffsetOn", "timeOffset", "setTimeOffset"}}}}}}},
					Json{{"audio", {"mute", "solo", "record", "monitor", "read"}},
						{"instrument", {"mute", "solo", "record", "monitor", "read"}},
						{"midi", {"mute", "solo", "record", "monitor", "read"}}, {"group", {"mute", "solo", "read"}},
						{"effect", {"mute", "solo", "read"}}, {"folder", {"mute", "solo", "read"}},
						{"marker", Json::array()},
						{"ruler", {"timeFormat", "timeOffsetOn", "timeOffset", "setTimeOffset"}},
						{"tempo", {"tempoTrack"}}, {"signature", Json::array()}}},
				{"ui.trackWidths",
					"The widths in the track list's rows, in dp: name, the track name's for every track type (20 to "
					"400), and controls, by track type, the width of its controls area (0 to 800), whose controls go "
					"onto more lines when they don't fit. A width left out is 120; ruler tracks' default is 140.",
					Json{{"type", "object"},
						{"properties",
							{{"name", {{"type", "number"}, {"minimum", 20}, {"maximum", 400}}},
								{"controls",
									{{"type", "object"},
										{"additionalProperties",
											{{"type", "number"}, {"minimum", 0}, {"maximum", 800}}}}}}}},
					Json{{"name", 120},
						{"controls",
							{{"audio", 120}, {"instrument", 120}, {"midi", 120}, {"group", 120}, {"effect", 120},
								{"folder", 120}, {"marker", 120}, {"ruler", 140}, {"tempo", 120},
								{"signature", 120}}}}},
				{"ui.upDownSelectsTracksOnly",
					"Whether Up and Down (track.selectPrevious, track.selectNext) only select tracks; when off, while "
					"an event or part is selected they also select the first one on the newly selected track "
					"(reference manual p. 218).",
					Json{{"type", "boolean"}}, false},
				{"ui.controllerLanePresets",
					"The controller lane presets of the MIDI editors (#128, reference manual p. 1166): each a name and "
					"the lanes it shows, as editor.set takes them.",
					Json{{"type", "array"},
						{"items",
							{{"type", "object"},
								{"properties",
									{{"name", {{"type", "string"}}},
										{"lanes",
											{{"type", "array"},
												{"items",
													{{"type", "object"}, {"additionalProperties", false},
														{"required", Json::array({"type"})},
														{"properties",
															{{"type",
																 {{"type", "string"},
																	 {"enum",
																		 std::vector<std::string>(
																			 midi::g_laneTypes.begin(),
																			 midi::g_laneTypes.end())}}},
																{"number",
																	{{"type", "integer"}, {"minimum", 0},
																		{"maximum", 127}}}}}}}}}}},
								{"required", Json::array({"name", "lanes"})}}}},
					Json::array()},
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
				"Changes a user setting, key to value, or several at once, values by key: all of them or, if one "
				"doesn't fit, none. Saves the settings file, unless settings.list reports it read-only. User "
				"settings belong to the user, not to a project. Emits settings.changed with the changed values, "
				"unless they had them already. The settings: " +
					list + ".",
				{{"type", "object"},
					{"properties",
						{{"key", key}, {"value", {{"description", "The new value; it must fit the setting's schema."}}},
							{"values",
								{{"type", "object"},
									{"description", "Instead of key and value: the new values, by setting."}}}}},
					{"additionalProperties", false}},
				{{"type", "object"},
					{"properties",
						{{"key", {{"type", "string"}}}, {"value", {{"description", "The setting's value."}}},
							{"values", {{"type", "object"}, {"description", "With values: the settings' values."}}}}}},
				false},
			[&store, changed](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				if (const auto values = parameters.find("values"); values != parameters.end())
				{
					require(!parameters.contains("key") && !parameters.contains("value"),
						"give either key and value or values");
					emitChanged(context, store, store.set(*values), changed);
					Json result = Json::object();
					for (const auto& entry : values->items())
						result[entry.key()] = store.value(entry.key());
					return Json{{"values", std::move(result)}};
				}
				require(
					parameters.contains("key") && parameters.contains("value"), "give either key and value or values");
				const std::string name = parameters.at("key").get<std::string>();
				const Json& value = parameters.at("value");
				if (store.set(name, value))
				{
					context.emit("settings.changed", {{"values", {{name, value}}}});
					changed(context, {name});
				}
				return Json{{"key", name}, {"value", store.value(name)}};
			});

		registerPresetCommands(engine, store, changed);

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
