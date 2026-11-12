// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"
#include "engine/midi/functions.h"
#include "engine/schema.h"

#include <string>
#include <vector>

namespace gnubase::engine
{
	class Engine;
	class ProjectModel;
}

namespace gnubase::engine::midi
{
	// What the commands on MIDI parts share (the MIDI functions and the rule editor's): a closed object's and an
	// array's schema, the parts in parameters' parts, each named once (CommandError otherwise), and the notes selected
	// in parameters' notes, which must be in the parts; every note without notes.
	Json objectSchema(Json properties, const std::vector<std::string>& required = {});
	Json arraySchema(Json items);
	std::vector<MidiPart*> partsOf(Project& project, const Json& parameters);
	NoteSelection selectionOf(const std::vector<MidiPart*>& parts, const Json& parameters);

	// Registers the MIDI function commands (midi.transpose, midi.legato, midi.merge and so on) with the project
	// model: each one checks its parameters, runs its function from functions.h and records an undo step.
	void registerFunctionCommands(ProjectModel& model, Engine& engine);
}
