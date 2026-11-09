// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"
#include "engine/midi/functions.h"
#include "engine/midi/logical.h"

#include <set>

namespace gnubase::engine
{
	class Engine;
	class ProjectModel;
}

namespace gnubase::engine::settings
{
	struct Format;
}

namespace gnubase::engine::midi
{
	// What the rule editor found in a part.
	struct LogicalResult
	{
		std::set<ObjectId> notes;
		std::set<ObjectId> controllers;
	};

	// Applies rules to a part's notes, or the selected ones, and, with every note selected, its controllers besides
	// SysEx; select only finds. Type changes between notes and other events, and pitches beyond 0 to 127 of notes
	// that stay, are left out; insert gives the copies new ids (newId).
	LogicalResult applyLogical(const Project& project, MidiPart& part, const NoteSelection& selection,
		const LogicalRules& rules, LogicalRandom& random, const NewId& newId);

	// midi-rule-presets.json in the settings folder: the user's rule editor and input transformer presets, the
	// setting midiRules.presets, as a list of {name, rules}. Factory presets aren't in it.
	const settings::Format& logicalPresetsFormat();

	// Registers the rule editor's and the input transformers' commands (#178): midiRules.presets,
	// midiRules.savePreset, midiRules.removePreset, midi.applyRules, midi.findByRules and midi.setInputTransformer.
	void registerLogicalCommands(ProjectModel& model, Engine& engine);
}
