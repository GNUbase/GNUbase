// SPDX-License-Identifier: GPL-3.0-or-later
// Quantizing in the app (#132, docs/features/quantize.md, reference manual pp. 367-378): the quantize panel and the
// Edit menu's quantize commands.
#include "app/appui.h"
#include "platform/log.h"

#include <RmlUi/Core/Element.h>

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::app
{
	using engine::Json;

	namespace
	{
		struct QuantizeCommand
		{
			const char* id;
			const char* command; // the engine's
			const char* step;    // the undo step of several parts' notes
			const char* description;
			const char* keys;
		};

		constexpr std::array<QuantizeCommand, 5> g_quantizeCommands{{
			{"edit.quantize", "quantize.apply", "Quantize",
				"Quantizes the starts of the selected MIDI notes or parts, or audio events or parts, to the quantize "
				"grid.",
				"Q"},
			{"edit.resetQuantize", "quantize.reset", "Reset quantize",
				"Puts the selected MIDI notes or parts, or audio events or parts, back to their timing before "
				"quantizing.",
				nullptr},
			{"edit.quantizeLengths", "quantize.lengths", "Quantize lengths",
				"Sets the lengths of the selected MIDI notes or parts to the length quantize value.", nullptr},
			{"edit.quantizeEnds", "quantize.ends", "Quantize ends",
				"Quantizes the ends of the selected MIDI notes or parts to the quantize grid.", nullptr},
			{"edit.freezeQuantize", "quantize.freeze", "Freeze quantize",
				"Makes the quantized timing of the selected MIDI notes or parts permanent.", nullptr},
		}};
	}

	void AppUi::addQuantize()
	{
		// The quantize panel (pp. 372-378), a window of its own there: the right zone, or a window of its own here.
		m_docking.add({"quantize", "Quantize", "project", "right", [this](Rml::Element& element)
			{
				auto panel = std::make_shared<ui::QuantizePanel>(
					m_binding, element, ui::QuantizePanel::Layout::Panel,
					[this](const std::string& command)
					{
						ui::Window* window = m_docking.windowOf("quantize");
						m_keys.run(command, Json::object(), window != nullptr ? *window : m_main, nullptr);
					},
					[this](const std::string& command, Json parameters)
					{ quantizeAudioTracks(command, std::move(parameters)); });
				panel->showHitpointTracks(hitpointTracks());
				m_quantizePanel = panel;
				return panel;
			}});
		m_keys.add({"window.quantizePanel", "Opens the quantize panel.",
			[this](ui::Window& /*window*/, Rml::Element* /*zone*/) { m_docking.open("quantize"); }});

		for (const QuantizeCommand& each : g_quantizeCommands)
		{
			m_keys.add({each.id, each.description,
				[this, command = std::string(each.command), step = std::string(each.step)](ui::Window& window,
					Rml::Element* zone) { quantize(command, step, quantizeTarget(window, zone)); }});
			// Audio events and parts have starts to quantize, not ends or lengths.
			const bool audio =
				each.command == std::string("quantize.apply") || each.command == std::string("quantize.reset");
			m_keys.enableWhen(each.id,
				[this, audio](ui::Window& window, Rml::Element* zone)
				{
					const QuantizeTarget target = quantizeTarget(window, zone);
					return !target.parts.empty() || (audio && (!target.events.empty() || !target.audioParts.empty()));
				});
			if (each.keys != nullptr)
				m_keys.bind({each.keys, each.id});
		}

		// Create Groove Quantize Preset (pp. 367, 373, 376): a groove of the first part it would quantize, or of the
		// first selected audio event's hitpoints, which then is the project's quantize settings.
		m_keys.add({"edit.createGrooveQuantize",
			"Makes a groove quantize preset of the selected MIDI part's rhythm, or of the selected audio event's "
			"hitpoints, and quantizes to it.",
			[this](ui::Window& window, Rml::Element* zone)
			{
				const QuantizeTarget target = quantizeTarget(window, zone);
				if (target.parts.empty() && target.events.empty())
					return;
				m_binding.send("quantize.extractGroove",
					target.parts.empty() ? Json{{"event", target.events.front()}}
										 : Json{{"part", target.parts.front().first}},
					[this](const engine::Result& result)
					{
						if (result.ok())
							m_binding.send(
								"quantize.set", {{"preset", result.value.at("preset").at("name").get<std::string>()}});
					});
			}});
		m_keys.enableWhen("edit.createGrooveQuantize",
			[this](ui::Window& window, Rml::Element* zone)
			{
				const QuantizeTarget target = quantizeTarget(window, zone);
				return !target.parts.empty() || !target.events.empty();
			});
	}

	AppUi::QuantizeTarget AppUi::quantizeTarget(ui::Window& window, Rml::Element* zone) const
	{
		// An editor's selected notes, or with selected ones required none, every note of the parts it edits.
		const auto ofView = [](const Json& view, bool selected)
		{
			QuantizeTarget target;
			// The audio part editor's parts have no notes to quantize.
			if (!view.value("open", false) || view.value("content", std::string()) == "audioPart")
				return target;
			std::map<uint64_t, std::vector<uint64_t>> notes;
			for (const Json& note : view.value("selection", Json::array()))
				notes[note.at("part").get<uint64_t>()].push_back(note.at("note").get<uint64_t>());
			if (notes.empty() && !selected)
				for (const Json& part : view.value("editable", Json::array()))
					notes[part.get<uint64_t>()];
			target.parts.assign(notes.begin(), notes.end());
			target.lengthQuantize = view.value("lengthQuantize", std::string("link"));
			target.insertVelocity = view.value("insertVelocity", target.insertVelocity);
			return target;
		};
		const auto ofEditor = [this, &ofView](const std::string& editor, bool selected)
		{ return ofView(m_editorViews.value(editor, Json::object()), selected); };
		// From an in-place editor (#131): its notes, never the project window's selected parts under it.
		if (zone != nullptr && zone->GetAttribute<Rml::String>("focuszone", "") == "inPlace")
		{
			const Json* view =
				m_inPlace ? m_inPlace->view(std::stoull(zone->GetAttribute<Rml::String>("track", "0"))) : nullptr;
			return view != nullptr ? ofView(*view, false) : QuantizeTarget{};
		}
		// From an editor: its window, or the lower zone while the zone shows the Editor tab.
		if (m_editor.window != 0 && window.id() == m_editor.window)
			return ofEditor("window", false);
		if (zone != nullptr && zone->GetAttribute<Rml::String>("focuszone", "") == "lower" &&
			m_docking.showing("editor"))
			return ofEditor("lower", false);
		// From no zone (the quantize panel, Lua, MCP): an open editor's selected notes first.
		if (zone == nullptr)
			for (const char* editor : {"window", "lower"})
				if (QuantizeTarget target = ofEditor(editor, true); !target.parts.empty())
					return target;
		QuantizeTarget target;
		for (const uint64_t part : selected("midiParts"))
			target.parts.emplace_back(part, std::vector<uint64_t>());
		target.events = selected("audioEvents");
		target.audioParts = selected("audioParts");
		return target;
	}

	std::vector<uint64_t> AppUi::selectedAudioEvents() const
	{
		std::vector<uint64_t> events = selected("audioEvents");
		const std::vector<uint64_t> parts = selected("audioParts");
		if (m_eventDisplay && !parts.empty())
			for (const Json& event : m_eventDisplay->project().value("audioEvents", Json::array()))
				if (std::find(parts.begin(), parts.end(), event.value("part", uint64_t{0})) != parts.end())
					events.push_back(event.at("id").get<uint64_t>());
		return events;
	}

	Json AppUi::hitpointTracks() const
	{
		Json tracks = Json::array();
		const std::vector<uint64_t> events = selectedAudioEvents();
		if (!m_eventDisplay || events.empty())
			return tracks;
		const Json& project = m_eventDisplay->project();
		std::set<uint64_t> withHitpoints; // clips
		for (const Json& clip : project.value("clips", Json::array()))
			if (!clip.value("hitpoints", Json::array()).empty())
				withHitpoints.insert(clip.at("id").get<uint64_t>());
		std::set<uint64_t> found; // tracks
		for (const Json& event : project.value("audioEvents", Json::array()))
			if (std::find(events.begin(), events.end(), event.at("id").get<uint64_t>()) != events.end() &&
				withHitpoints.count(event.at("clip").get<uint64_t>()) != 0)
				found.insert(event.at("track").get<uint64_t>());
		for (const Json& track : project.value("tracks", Json::array()))
			if (found.count(track.at("id").get<uint64_t>()) != 0)
				tracks.push_back({{"track", track.at("id")}, {"name", track.value("name", std::string())}});
		return tracks;
	}

	void AppUi::quantizeAudioTracks(const std::string& command, Json parameters)
	{
		const std::vector<uint64_t> parts = selected("audioParts");
		std::vector<uint64_t> events = selectedAudioEvents();
		if (events.empty())
			return;
		parameters["events"] = events;
		if (command != "quantize.slice")
		{
			m_binding.send(command, std::move(parameters));
			return;
		}
		// Every slice is selected, so that Quantize and Crossfade go on with all of them (p. 371). Selected parts stay
		// selected for the slices in them, which the project window shows only as the parts' waveforms.
		m_binding.send(command, std::move(parameters),
			[this, parts](const engine::Result& result)
			{
				if (!result.ok())
				{
					logging::warning("Slicing audio events failed: {}", result.error);
					return;
				}
				if (!m_eventDisplay)
					return;
				std::vector<uint64_t> selection = parts;
				for (const Json& event : result.value.at("events"))
					selection.push_back(event.get<uint64_t>());
				m_eventDisplay->select(std::move(selection));
			});
	}

	void AppUi::quantize(const std::string& command, const std::string& step, const QuantizeTarget& target)
	{
		const bool audio = (command == "quantize.apply" || command == "quantize.reset") &&
			(!target.events.empty() || !target.audioParts.empty());
		if (target.parts.empty() && !audio)
			return;
		Json extra = Json::object();
		if (command == "quantize.lengths" && !target.lengthQuantize.empty() && target.lengthQuantize != "link")
			extra["length"] = target.lengthQuantize;
		// Whole parts in one command; the notes of several parts as one undo step.
		Json commands = Json::array();
		Json whole = Json::array();
		for (const auto& [part, notes] : target.parts)
		{
			if (notes.empty())
			{
				whole.push_back(part);
				continue;
			}
			Json parameters = extra;
			parameters["part"] = part;
			parameters["notes"] = notes;
			commands.push_back({{"command", command}, {"parameters", std::move(parameters)}});
		}
		if (!whole.empty() || audio)
		{
			Json parameters = extra;
			if (!whole.empty())
				parameters["parts"] = std::move(whole);
			if (audio)
			{
				parameters["events"] = target.events;
				parameters["audioParts"] = target.audioParts;
			}
			commands.push_back({{"command", command}, {"parameters", std::move(parameters)}});
		}
		if (commands.size() == 1)
			m_binding.send(command, commands[0].at("parameters"));
		else
			m_binding.send("history.transaction", {{"description", step}, {"commands", std::move(commands)}});
	}
}
