// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/midi/logical.h"
#include "engine/schema.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gnubase::engine
{
	class Engine;
	class ProjectModel;

	namespace settings
	{
		struct Format;
	}

	// Project rules (#179, reference manual pp. 1424-1445, docs/features/project-rules.md): filter lines that find
	// tracks, parts and events, a function that says what happens to them, action lines that change them, and key
	// commands that run before and after. Positions and lengths are in ticks of 480 to a quarter note from the
	// project's start, as the MIDI rules have them (midi/logical.h), whose bracket and join reading they share.
	enum class ProjectTarget : uint8_t
	{
		MediaType,
		ContainerType,
		Name,
		Position,
		Length,
		ColorName,
		Property,
	};

	enum class ProjectCondition : uint8_t
	{
		Equal,
		Unequal,
		Bigger,
		BiggerOrEqual,
		Less,
		LessOrEqual,
		InsideRange,  // from parameter 1 to parameter 2, both included
		OutsideRange, // below parameter 1 or above parameter 2
		AllTypes,     // media or container type: any
		PropertyIsSet,
		PropertyIsNotSet,
		Contains, // name or color name
		ContainsNot,
	};

	// A filter line. For media and container types and properties, text is the name of the type or property, for
	// names and color names the text compared; positions and lengths compare with parameter 1 and 2.
	struct ProjectFilterLine
	{
		int open = 0; // 0 to 3
		ProjectTarget target = ProjectTarget::ContainerType;
		ProjectCondition condition = ProjectCondition::Equal;
		std::string text;
		double parameter1 = 0.0;
		double parameter2 = 0.0;
		int close = 0; // 0 to 3
		midi::LogicalBool join = midi::LogicalBool::And;
	};

	enum class ProjectFunction : uint8_t
	{
		Delete,
		Transform,
		Select,   // the UI selects what is found
		Deselect, // the UI deselects everything
	};

	enum class ProjectActionTarget : uint8_t
	{
		Position,
		Length,
		TrackOperation,
		Name,
		Color,
	};

	enum class ProjectOperation : uint8_t
	{
		// Position and length
		Add,
		Subtract,
		MultiplyBy,
		DivideBy,
		RoundBy,
		SetRandom,
		SetRelativeRandom,
		SetFixed, // also a color: text, a color name of the project's colors or #rrggbb
				  // Names: text and text2
		Replace,
		Append,
		Prepend,
		GenerateName, // text followed by a number from parameter 2, one up for each element found
		ReplaceSearchString,
		EraseBefore,
		EraseAfter,
		EraseFrontCharacter,
		EraseEndCharacter,
		// Track operations: text is on, off or toggle (time domain: musical, linear or toggle)
		Mute,
		Solo,
		Record,
		Monitor,
		TimeDomain,
		// Colors: the next or previous color of the project's colors
		Increment,
		Decrement,
	};

	struct ProjectActionLine
	{
		ProjectActionTarget target = ProjectActionTarget::Position;
		ProjectOperation operation = ProjectOperation::Add;
		std::string text;  // parameter 1 for names, track operations and colors
		std::string text2; // parameter 2 for names
		double parameter1 = 0.0;
		double parameter2 = 0.0;
	};

	// A key command (docs/features/key-commands.md) that runs before or after the rules: an engine command or one of
	// the UI's, with parameters.
	struct ProjectRuleCommand
	{
		std::string command;
		Json parameters = Json::object();
	};

	inline constexpr size_t g_maxRuleCommands = 4;

	struct ProjectRules
	{
		std::vector<ProjectFilterLine> filters; // none find everything
		ProjectFunction function = ProjectFunction::Transform;
		std::vector<ProjectActionLine> actions;
		std::vector<ProjectRuleCommand> pre;  // up to 4
		std::vector<ProjectRuleCommand> post; // up to 4
	};

	bool operator==(const ProjectRules& a, const ProjectRules& b);

	Json toJson(const ProjectRules& rules);
	// Throws std::invalid_argument for rules that don't fit projectRulesSchema, have brackets that don't match or nest
	// deeper than midi::g_maxLogicalBrackets, or conditions and operations their target doesn't have.
	ProjectRules projectRulesFromJson(const Json& json);
	Json projectRulesSchema();

	struct ProjectRulePreset
	{
		std::string name; // "Category/Name" for a factory preset
		ProjectRules rules;
	};

	const std::vector<ProjectRulePreset>& factoryProjectRulePresets();

	// project-rule-presets.json in the settings folder: the user's project rule presets, the setting
	// projectRules.presets.
	const settings::Format& projectRulePresetsFormat();

	// project.applyRules, project.findByRules, projectRules.presets, projectRules.savePreset and
	// projectRules.removePreset.
	void registerProjectRuleCommands(ProjectModel& model, Engine& engine);
}
