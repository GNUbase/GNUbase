// SPDX-License-Identifier: GPL-3.0-or-later
// Quantizing in the app (#132, docs/features/quantize.md, reference manual pp. 367-378): the quantize panel and the
// Edit menu's quantize commands.
#include "app/appui.h"

#include <RmlUi/Core/Element.h>

#include <array>
#include <map>
#include <memory>
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
				"Quantizes the starts of the selected MIDI notes or parts to the quantize grid.", "Q"},
			{"edit.resetQuantize", "quantize.reset", "Reset quantize",
				"Puts the selected MIDI notes or parts back to their timing before quantizing.", nullptr},
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
				return std::make_shared<ui::QuantizePanel>(m_binding, element, ui::QuantizePanel::Layout::Panel,
					[this](const std::string& command)
					{
						ui::Window* window = m_docking.windowOf("quantize");
						m_keys.run(command, Json::object(), window != nullptr ? *window : m_main, nullptr);
					});
			}});
		m_keys.add({"window.quantizePanel", "Opens the quantize panel.",
			[this](ui::Window& /*window*/, Rml::Element* /*zone*/) { m_docking.open("quantize"); }});

		for (const QuantizeCommand& each : g_quantizeCommands)
		{
			m_keys.add({each.id, each.description,
				[this, command = std::string(each.command), step = std::string(each.step)](ui::Window& window,
					Rml::Element* zone) { quantize(command, step, quantizeTarget(window, zone)); }});
			m_keys.enableWhen(each.id,
				[this](ui::Window& window, Rml::Element* zone) { return !quantizeTarget(window, zone).parts.empty(); });
			if (each.keys != nullptr)
				m_keys.bind({each.keys, each.id});
		}

		// Create Groove Quantize Preset (pp. 373, 376): a groove of the first part it would quantize, which then is the
		// project's quantize settings.
		m_keys.add({"edit.createGrooveQuantize",
			"Makes a groove quantize preset of the selected MIDI part's rhythm and quantizes to it.",
			[this](ui::Window& window, Rml::Element* zone)
			{
				const QuantizeTarget target = quantizeTarget(window, zone);
				if (target.parts.empty())
					return;
				m_binding.send("quantize.extractGroove", {{"part", target.parts.front().first}},
					[this](const engine::Result& result)
					{
						if (result.ok())
							m_binding.send(
								"quantize.set", {{"preset", result.value.at("preset").at("name").get<std::string>()}});
					});
			}});
		m_keys.enableWhen("edit.createGrooveQuantize",
			[this](ui::Window& window, Rml::Element* zone) { return !quantizeTarget(window, zone).parts.empty(); });
	}

	AppUi::QuantizeTarget AppUi::quantizeTarget(ui::Window& window, Rml::Element* zone) const
	{
		// An editor's selected notes, or with selected ones required none, every note of the parts it edits.
		const auto ofEditor = [this](const std::string& editor, bool selected)
		{
			QuantizeTarget target;
			const Json view = m_editorViews.value(editor, Json::object());
			if (!view.value("open", false))
				return target;
			std::map<uint64_t, std::vector<uint64_t>> notes;
			for (const Json& note : view.value("selection", Json::array()))
				notes[note.at("part").get<uint64_t>()].push_back(note.at("note").get<uint64_t>());
			if (notes.empty() && !selected)
				for (const Json& part : view.value("editable", Json::array()))
					notes[part.get<uint64_t>()];
			target.parts.assign(notes.begin(), notes.end());
			target.lengthQuantize = view.value("lengthQuantize", std::string("link"));
			return target;
		};
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
		return target;
	}

	void AppUi::quantize(const std::string& command, const std::string& step, const QuantizeTarget& target)
	{
		if (target.parts.empty())
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
		if (!whole.empty())
		{
			Json parameters = extra;
			parameters["parts"] = std::move(whole);
			commands.push_back({{"command", command}, {"parameters", std::move(parameters)}});
		}
		if (commands.size() == 1)
			m_binding.send(command, commands[0].at("parameters"));
		else
			m_binding.send("history.transaction", {{"description", step}, {"commands", std::move(commands)}});
	}
}
