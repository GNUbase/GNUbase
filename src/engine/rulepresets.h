// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "engine/settings/store.h"
#include "platform/log.h"

#include <algorithm>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// A library of named rules (#178, #179): the factory presets, and the user's, which are user settings in a file of the
// settings folder of their own. The rule editor's MIDI rules (midi/logicalcommands.cpp) and the project rules
// (document/projectrules.cpp) keep their presets in one each.
namespace gnubase::engine
{
	// Preset has a name and rules. What a library needs to know of its presets.
	template <typename Preset> struct RulePresetKind
	{
		using Rules = decltype(Preset::rules);

		std::string key; // the setting, e.g. "midiRules.presets"
		const settings::Format* format = nullptr;
		std::string what;         // in messages, e.g. "MIDI rule preset"
		std::string changedEvent; // announces the user's presets' names, e.g. "midiRules.presetsChanged"
		const std::vector<Preset>* factory = nullptr;
		std::function<Json(const Rules&)> toJson;
		std::function<Rules(const Json&)> fromJson; // throws std::invalid_argument for rules it can't read
	};

	// The user's presets after the factory ones, names unique among both. Every change starts from the presets in the
	// file, so that none drops what another GNUbase process saved meanwhile. Presets this GNUbase can't read are
	// skipped and stay in the file.
	template <typename Preset> class RulePresets
	{
	public:
		RulePresets(RulePresetKind<Preset> kind, const std::filesystem::path& settingsDirectory)
			: m_kind(std::move(kind)), m_store(*m_kind.format, settingsDirectory)
		{
			m_user = read(m_store.value(m_kind.key), &m_skipped);
		}

		bool isFactory(const std::string& name) const
		{
			const auto& factory = *m_kind.factory;
			return std::any_of(
				factory.begin(), factory.end(), [&name](const Preset& preset) { return preset.name == name; });
		}

		const Preset* find(const std::string& name) const
		{
			for (const auto* list : {m_kind.factory, &m_user})
				for (const Preset& preset : *list)
					if (preset.name == name)
						return &preset;
			return nullptr;
		}

		// The rules of a preset, or a CommandError naming it.
		const auto& rulesOf(const std::string& name) const
		{
			const Preset* preset = find(name);
			if (preset == nullptr)
				throw CommandError("no " + m_kind.what + " " + name);
			return preset->rules;
		}

		void save(const Preset& preset)
		{
			if (preset.name.empty())
				throw CommandError("name must not be empty");
			if (isFactory(preset.name))
				throw CommandError(preset.name + " is a factory preset");
			const Json item = {{"name", preset.name}, {"rules", m_kind.toJson(preset.rules)}};
			change(
				[&preset, &item](Json& list)
				{
					const auto mine = std::find_if(list.begin(), list.end(), named(preset.name));
					if (mine != list.end())
						*mine = item;
					else
						list.push_back(item);
				});
		}

		void remove(const std::string& name)
		{
			if (isFactory(name))
				throw CommandError("factory presets can't be removed");
			change(
				[this, &name](Json& list)
				{
					const auto end = std::remove_if(list.begin(), list.end(), named(name));
					if (end == list.end())
						throw CommandError("no " + m_kind.what + " " + name);
					list.erase(end, list.end());
				});
		}

		Json presetJson(const Preset& preset, bool factory) const
		{
			return {{"name", preset.name}, {"factory", factory}, {"rules", m_kind.toJson(preset.rules)}};
		}

		Json toJson() const
		{
			const settings::LoadReport& load = m_store.loadReport();
			Json problems = load.problems;
			for (const std::string& problem : m_skipped)
				problems.push_back(problem);
			Json presets = Json::array();
			for (const Preset& preset : *m_kind.factory)
				presets.push_back(presetJson(preset, true));
			for (const Preset& preset : m_user)
				presets.push_back(presetJson(preset, false));
			return {{"presets", std::move(presets)},
				{"file", m_store.file().empty() ? Json(nullptr) : Json(m_store.file().u8string())},
				{"readOnly", load.readOnly}, {"problems", std::move(problems)}};
		}

		void announce(CommandContext& context) const
		{
			Json names = Json::array();
			for (const Preset& preset : m_user)
				names.push_back(preset.name);
			context.emit(m_kind.changedEvent, {{"presets", std::move(names)}});
		}

	private:
		// Whether an item of the presets file has that name.
		static auto named(std::string name)
		{
			return [name = std::move(name)](const Json& item)
			{
				const auto found = item.find("name"); // end() unless it is an object
				return found != item.end() && *found == name;
			};
		}

		// A preset of the file, or nothing and why this GNUbase can't read it.
		std::optional<Preset> readPreset(const Json& item, std::string& problem) const
		{
			if (!item.is_object() || !item.contains("name") || !item.at("name").is_string() || !item.contains("rules"))
			{
				problem = "it isn't an object with a name and rules";
				return std::nullopt;
			}
			Preset preset{item.at("name").get<std::string>(), {}};
			if (preset.name.empty())
				problem = "it has no name";
			else if (isFactory(preset.name))
				problem = "a factory preset has its name";
			if (!problem.empty())
				return std::nullopt;
			try
			{
				preset.rules = m_kind.fromJson(item.at("rules"));
			}
			catch (const std::invalid_argument& error)
			{
				problem = error.what();
				return std::nullopt;
			}
			return preset;
		}

		// Changes the list in the file as it is now, then uses what the file has, also when the change fails.
		void change(const std::function<void(Json& list)>& edit)
		{
			Json list;
			try
			{
				list = m_store.modify(m_kind.key,
					[&edit](const Json& value)
					{
						Json changed = value;
						edit(changed);
						return changed;
					});
			}
			catch (const settings::SettingsError& error)
			{
				throw CommandError("cannot save the " + m_kind.what + "s: " + error.what());
			}
			m_user = read(list);
		}

		std::vector<Preset> read(const Json& list, std::vector<std::string>* skipped = nullptr) const
		{
			std::vector<Preset> presets;
			for (size_t i = 0; i < list.size(); ++i)
			{
				std::string problem;
				std::optional<Preset> preset = readPreset(list[i], problem);
				if (preset &&
					std::any_of(
						presets.begin(), presets.end(), [&preset](const Preset& p) { return p.name == preset->name; }))
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
				logging::warning("{}s: {}", m_kind.what, skipped->back());
			}
			return presets;
		}

		RulePresetKind<Preset> m_kind;
		settings::Store m_store;
		std::vector<std::string> m_skipped;
		std::vector<Preset> m_user;
	};
}
