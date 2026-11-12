// SPDX-License-Identifier: GPL-3.0-or-later
// The MIDI menu in the app (#133, docs/features/midi-functions.md, reference manual pp. 1118-1131): MIDI > Functions
// on an editor's selected notes or the project window's selected MIDI parts, as quantizing takes them, Bounce MIDI, and
// the dialogs of Transpose Setup, Merge MIDI in Loop, Dissolve Part, Velocity, Delete Notes and Restrict Polyphony, and
// the rule editor and project input transformer (#178).
#include "app/appui.h"

#include "engine/midi/quantize.h"
#include "engine/midi/quantizesettings.h"
#include "engine/time/signaturemap.h"
#include "engine/time/tempomap.h"
#include "platform/log.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::app
{
	using engine::Json;

	namespace
	{
		struct MidiFunction
		{
			const char* id;
			const char* command; // the engine's
			const char* mode;    // midi.deleteOverlaps' mode, or null
			bool notes;          // takes the selected notes
			const char* description;
		};

		constexpr std::array<MidiFunction, 14> g_midiFunctions{{
			{"midi.functionsLegato", "midi.legato", nullptr, true,
				"Extends the selected MIDI notes or parts' notes to the next notes, by Legato Overlap "
				"(editing.legatoOverlap), or to the next selected notes with editing.legatoBetweenSelected."},
			{"midi.functionsFixedLengths", "midi.fixLengths", nullptr, true,
				"Sets the length of the selected MIDI notes or parts' notes to the length quantize value."},
			{"midi.functionsFixedVelocity", "midi.fixVelocities", nullptr, true,
				"Sets the velocity of the selected MIDI notes or parts' notes to the editor's insert velocity."},
			{"midi.functionsPedalsToNoteLength", "midi.pedalToNoteLength", nullptr, true,
				"Lengthens the selected MIDI notes or parts' notes to where the sustain pedal is released."},
			{"midi.functionsDeleteOverlapsMono", "midi.deleteOverlaps", "mono", true,
				"Shortens the selected MIDI notes or parts' notes that overlap notes of the same pitch."},
			{"midi.functionsDeleteOverlapsPoly", "midi.deleteOverlaps", "poly", true,
				"Shortens the selected MIDI notes or parts' notes that overlap any note."},
			{"midi.functionsDeleteDoubles", "midi.deleteDoubles", nullptr, true,
				"Deletes the doubles among the selected MIDI notes or parts' notes."},
			{"midi.functionsDeleteControllers", "midi.deleteControllers", nullptr, false,
				"Deletes the controllers of the selected MIDI parts."},
			{"midi.functionsDeleteContinuousControllers", "midi.deleteContinuousControllers", nullptr, false,
				"Deletes the continuous controllers of the selected MIDI parts."},
			{"midi.functionsThinOut", "midi.thinOut", nullptr, false,
				"Thins out the controller data of the selected MIDI parts."},
			{"midi.functionsExtractAutomation", "midi.extractAutomation", nullptr, false,
				"Moves the continuous controllers of the selected MIDI parts into automation lanes of their tracks."},
			{"midi.functionsReverse", "midi.reverse", nullptr, true,
				"Reverses the playback order of the selected MIDI notes or parts' events."},
			{"midi.functionsMirror", "midi.invert", nullptr, true,
				"Mirrors the selected MIDI notes or parts' events in time."},
			// O-Note Conversion (#882, p. 1240), in the MIDI menu itself.
			{"midi.convertONotes", "midi.oNoteConversion", nullptr, true,
				"Gives the selected MIDI notes or parts' notes their drum sounds' O-notes as pitches."},
		}};

		// Ids as a dialog's attribute: "1,2,3".
		std::string listOf(const std::vector<uint64_t>& ids)
		{
			std::string text;
			for (const uint64_t id : ids)
				text.append(text.empty() ? "" : ",").append(std::to_string(id));
			return text;
		}
	}

	void AppUi::openArticulationMaps(ui::Window& window, uint64_t track)
	{
		const Json project = m_eventDisplay ? m_eventDisplay->project() : Json();
		std::map<uint64_t, std::string> types;
		if (project.is_object())
			for (const Json& each : project.value("tracks", Json::array()))
				types[each.at("id").get<uint64_t>()] = each.value("type", std::string());
		if (track == 0)
			for (const uint64_t selected : m_trackList ? m_trackList->selection() : std::vector<uint64_t>())
				if (types[selected] == "midi" || types[selected] == "instrument")
				{
					track = selected;
					break;
				}
		std::vector<std::pair<std::string, std::string>> attributes;
		if (track != 0)
			attributes.emplace_back("track", std::to_string(track));
		m_articulationMaps = openDialog(window, "articulation-maps.rml", attributes).GetObserverPtr();
	}

	void AppUi::addMidiFunctions()
	{
		// The parameters of a function on what quantizing takes: the parts, and their selected notes if any.
		const auto targetOf = [this](ui::Window& window, Rml::Element* zone, bool notes)
		{
			const QuantizeTarget target = quantizeTarget(window, zone);
			std::vector<uint64_t> parts;
			std::vector<uint64_t> selected;
			for (const auto& [part, partNotes] : target.parts)
			{
				parts.push_back(part);
				selected.insert(selected.end(), partNotes.begin(), partNotes.end());
			}
			Json parameters = {{"parts", parts}};
			if (notes && !selected.empty())
				parameters["notes"] = selected;
			return std::make_pair(target, parameters);
		};
		const auto hasTarget = [this](ui::Window& window, Rml::Element* zone)
		{ return !quantizeTarget(window, zone).parts.empty(); };

		for (const MidiFunction& each : g_midiFunctions)
		{
			m_keys.add({each.id, each.description, [this, each, targetOf](ui::Window& window, Rml::Element* zone)
				{
					auto [target, parameters] = targetOf(window, zone, each.notes);
					if (target.parts.empty())
						return;
					const std::string command = each.command;
					if (each.mode != nullptr)
						parameters["mode"] = each.mode;
					if (command == "midi.fixVelocities")
						parameters["velocity"] = target.insertVelocity;
					if (command == "midi.legato")
					{
						// Legato Overlap, whose ticks are 480 to a quarter note, and Legato Mode (p. 1599).
						m_binding.send("settings.get", {{"key", "editing.legatoOverlap"}},
							[this, parameters = parameters](const engine::Result& result) mutable
							{
								if (!result.ok())
									return;
								parameters["overlap"] =
									result.value.at("value").get<int64_t>() * (time::g_ticksPerQuarter / 480);
								m_binding.send("settings.get", {{"key", "editing.legatoBetweenSelected"}},
									[this, parameters = std::move(parameters)](const engine::Result& mode) mutable
									{
										if (!mode.ok())
											return;
										parameters["betweenSelected"] = mode.value.at("value").get<bool>();
										m_binding.send("midi.legato", parameters);
									});
							});
						return;
					}
					if (command == "midi.fixLengths")
					{
						// The editor's length quantize, or the quantize grid when it is linked to it, as in the
						// project window.
						const std::optional<engine::midi::NoteValue> length =
							engine::midi::noteValueFromString(target.lengthQuantize);
						if (length.has_value())
						{
							parameters["length"] = length->ticks();
							m_binding.send(command, parameters);
							return;
						}
						m_binding.send("quantize.get", Json::object(),
							[this, parameters = parameters](const engine::Result& result) mutable
							{
								if (!result.ok())
									return;
								// The grid's step with its tuplets, as Edit > Quantize Lengths takes it; swing and
								// grooves, which change from step to step, don't count.
								engine::midi::QuantizeSettings settings =
									engine::midi::quantizeSettingsFromJson(result.value.at("settings"));
								settings.groove.reset();
								const engine::midi::QuantizeGrid grid(settings, time::SignatureMap());
								parameters["length"] = std::max<int64_t>(1, std::llround(grid.step()));
								m_binding.send("midi.fixLengths", parameters);
							});
						return;
					}
					m_binding.send(command, parameters);
				}});
			m_keys.enableWhen(each.id, hasTarget);
		}

		// The functions with a dialog get what they work on as the document's attributes, and the engine's ticks per
		// tick of 480 to a quarter note, the unit the dialogs show. One dialog at a time: its Lua's state is global.
		const auto openFunction = [this](ui::Window& window, const std::string& function, const Json& parameters)
		{
			std::vector<std::pair<std::string, std::string>> attributes = {
				{"function", function}, {"tickScale", std::to_string(time::g_ticksPerQuarter / 480)}};
			for (const char* key : {"parts", "notes"})
				if (parameters.contains(key))
					attributes.emplace_back(key, listOf(parameters.at(key).get<std::vector<uint64_t>>()));
			if (parameters.contains("destination"))
				attributes.emplace_back("destination", std::to_string(parameters.at("destination").get<uint64_t>()));
			openOnce(window, "midi-function.rml", m_midiFunction, attributes);
		};
		m_keys.add({"midi.transposeSetup", "Opens the Transpose Setup dialog for the selected MIDI notes or parts.",
			[openFunction, targetOf](ui::Window& window, Rml::Element* zone)
			{
				const auto [target, parameters] = targetOf(window, zone, true);
				if (!target.parts.empty())
					openFunction(window, "transpose", parameters);
			}});
		m_keys.enableWhen("midi.transposeSetup", hasTarget);
		m_keys.add({"midi.functionsRestrictPolyphony",
			"Opens the Restrict Polyphony dialog for the selected MIDI notes or parts.",
			[openFunction, targetOf](ui::Window& window, Rml::Element* zone)
			{
				const auto [target, parameters] = targetOf(window, zone, true);
				if (!target.parts.empty())
					openFunction(window, "restrictPolyphony", parameters);
			}});
		m_keys.enableWhen("midi.functionsRestrictPolyphony", hasTarget);
		m_keys.add({"midi.functionsVelocity", "Opens the Velocity dialog for the selected MIDI notes or parts.",
			[openFunction, targetOf](ui::Window& window, Rml::Element* zone)
			{
				const auto [target, parameters] = targetOf(window, zone, true);
				if (!target.parts.empty())
					openFunction(window, "velocity", parameters);
			}});
		m_keys.enableWhen("midi.functionsVelocity", hasTarget);
		m_keys.add({"midi.functionsDeleteNotes", "Opens the Delete Notes dialog for the selected MIDI notes or parts.",
			[openFunction, targetOf](ui::Window& window, Rml::Element* zone)
			{
				const auto [target, parameters] = targetOf(window, zone, true);
				if (!target.parts.empty())
					openFunction(window, "deleteNotes", parameters);
			}});
		m_keys.enableWhen("midi.functionsDeleteNotes", hasTarget);

		// The rule editor (#178, pp. 1405-1423) on what the functions take, and the project input transformer
		// (pp. 1392-1404), one document (docs/features/midi-rules.md).
		m_keys.add({"midi.ruleEditor", "Opens the rule editor for the selected MIDI notes or parts.",
			[this, targetOf](ui::Window& window, Rml::Element* zone)
			{
				const auto [target, parameters] = targetOf(window, zone, true);
				if (target.parts.empty())
					return;
				std::vector<std::pair<std::string, std::string>> attributes;
				for (const char* key : {"parts", "notes"})
					if (parameters.contains(key))
						attributes.emplace_back(key, listOf(parameters.at(key).get<std::vector<uint64_t>>()));
				openDialog(window, "rule-editor.rml", attributes);
			}});
		m_keys.enableWhen("midi.ruleEditor", hasTarget);
		// The articulation map setup (#175, pp. 1264-1283, docs/features/articulation-maps.md), and the file dialogs of
		// its Import Map and Export Map.
		m_keys.add({"midi.articulationMapSetup",
			"Opens the articulation map setup, for the selected MIDI or instrument track if there is one.",
			[this](ui::Window& window, Rml::Element* /*zone*/) { openArticulationMaps(window, 0); }});
		m_lua.addFunction("ui.importArticulationMap",
			[this](const Json& /*parameters*/)
			{
				ui::FileDialog dialog;
				dialog.title = "Import Articulation Map";
				dialog.filters = {{"Articulation maps", "articulationmap"}, {"All files", "*"}};
				dialog.parent = m_main.id();
				m_shell.showFileDialog(dialog,
					[this](const ui::FileDialogResult& result)
					{
						if (!result.error.empty())
							logging::warning("The file dialog failed: {}", result.error);
						if (result.files.empty())
							return;
						// The setup shows the imported map as it follows articulationMap.added, and why an import
						// failed in its message.
						m_binding.send("articulationMap.import", {{"path", result.files.front().u8string()}},
							[this](const engine::Result& imported)
							{
								Rml::Element* document = m_articulationMaps.get();
								Rml::Element* message = document != nullptr && document->GetParentNode() != nullptr
									? document->GetElementById("articulation-maps-message")
									: nullptr;
								if (!imported.ok() && message != nullptr)
									message->SetInnerRML(Rml::StringUtilities::EncodeRml(imported.error));
							});
					});
				return Json::object();
			});
		m_lua.addFunction("ui.exportArticulationMap",
			[this](const Json& parameters)
			{
				const uint64_t map = parameters.value("map", uint64_t{0});
				ui::FileDialog dialog;
				dialog.kind = ui::FileDialogKind::Save;
				dialog.title = "Export Articulation Map";
				dialog.filters = {{"Articulation maps", "articulationmap"}};
				dialog.location =
					std::filesystem::u8path(parameters.value("name", std::string("Map")) + ".articulationmap");
				dialog.parent = m_main.id();
				m_shell.showFileDialog(dialog,
					[this, map](const ui::FileDialogResult& result)
					{
						if (!result.error.empty())
							logging::warning("The file dialog failed: {}", result.error);
						if (result.files.empty())
							return;
						std::filesystem::path file = result.files.front();
						if (file.extension() != ".articulationmap")
							file += ".articulationmap";
						m_binding.send("articulationMap.export", {{"map", map}, {"path", file.u8string()}});
					});
				return Json::object();
			});
		m_keys.add({"project.inputTransformer",
			"Opens the project input transformer, which changes what MIDI input plays on the tracks that use it.",
			[this](ui::Window& window, Rml::Element* /*zone*/)
			{ openDialog(window, "rule-editor.rml", {{"target", "project"}}); }});

		// Merge MIDI in Loop (pp. 1119-1120): onto the first selected MIDI or instrument track, or a new one.
		m_keys.add({"midi.mergeInLoop",
			"Opens the MIDI Merge Options dialog, which merges the MIDI events between the locators into a new part.",
			[this, openFunction](ui::Window& window, Rml::Element* /*zone*/)
			{
				Json parameters = Json::object();
				const Json project = m_eventDisplay ? m_eventDisplay->project() : Json();
				std::map<uint64_t, std::string> types;
				if (project.is_object())
					for (const Json& track : project.value("tracks", Json::array()))
						types[track.at("id").get<uint64_t>()] = track.value("type", std::string());
				for (const uint64_t track : m_trackList ? m_trackList->selection() : std::vector<uint64_t>())
					if (types[track] == "midi" || types[track] == "instrument")
					{
						parameters["destination"] = track;
						break;
					}
				openFunction(window, "merge", parameters);
			}});

		// Dissolve Part (pp. 1121-1122) and Bounce MIDI (p. 1123) of the project window's selected MIDI parts.
		const auto hasParts = [this](ui::Window& /*window*/, Rml::Element* /*zone*/)
		{ return !selected("midiParts").empty(); };
		m_keys.add({"midi.dissolvePart", "Opens the Dissolve Part dialog for the selected MIDI parts.",
			[this, openFunction](ui::Window& window, Rml::Element* /*zone*/)
			{
				const std::vector<uint64_t> parts = selected("midiParts");
				if (!parts.empty())
					openFunction(window, "dissolve", {{"parts", parts}});
			}});
		m_keys.enableWhen("midi.dissolvePart", hasParts);
		m_keys.add({"midi.bounceMidi",
			"Combines the selected MIDI parts of each track into one part, and those whose transpose or velocity "
			"the notes can't hold into one for each.",
			[this](ui::Window& /*window*/, Rml::Element* /*zone*/)
			{
				// The engine bounces the parts of one track; several tracks are one undo step.
				std::map<uint64_t, std::vector<uint64_t>> byTrack;
				const std::vector<uint64_t> parts = selected("midiParts");
				if (parts.empty())
					return;
				for (const Json& part : m_eventDisplay->project().value("midiParts", Json::array()))
					if (const auto id = part.at("id").get<uint64_t>();
						std::find(parts.begin(), parts.end(), id) != parts.end())
						byTrack[part.at("track").get<uint64_t>()].push_back(id);
				Json commands = Json::array();
				for (const auto& [track, ids] : byTrack)
					commands.push_back({{"command", "midi.bounce"}, {"parameters", {{"parts", ids}}}});
				if (commands.size() == 1)
					m_binding.send("midi.bounce", commands[0].at("parameters"));
				else if (!commands.empty())
					m_binding.send(
						"history.transaction", {{"description", "Bounce MIDI"}, {"commands", std::move(commands)}});
			}});
		m_keys.enableWhen("midi.bounceMidi", hasParts);
	}
}
