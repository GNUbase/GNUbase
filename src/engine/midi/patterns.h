// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"
#include "engine/schema.h"

#include <map>
#include <string>

namespace gnubase::engine
{
	class CommandContext;
	class Engine;
	class ProjectModel;

	// The pattern as project.get lists it.
	Json toJson(const Pattern& pattern);
}

// Patterns of the pattern editor (#181, reference manual pp. 895-937): docs/features/pattern-editor.md describes them.
namespace gnubase::engine::midi
{
	inline constexpr int g_maxPatternSteps = 128;

	// The notes a pattern event of that length (in ticks) plays, from its start, which is from ticks into the pattern
	// (MidiPart::patternOffset): the pattern over and over, each lane on its own with its own length and resolution,
	// as their steps and parameters say. A note that offsets or swing move before the event's start doesn't play,
	// except that a pattern starting with the event (from 0) plays it at the start. The notes' ids are their order,
	// from 1, so each repeat of a step has its own random chance and variance in playback.
	std::map<ObjectId, Note> patternNotes(const Pattern& pattern, time::Ticks from, time::Ticks length);

	// The notes a MIDI part plays: a pattern event's pattern's (patternNotes), else its own. What plays, for the
	// functions that read it, such as merge, dissolve and bounce.
	std::map<ObjectId, Note> playedNotes(const Project& project, const MidiPart& part);

	// After an edit (ProjectModel::addEdit): a pattern event on another track than its pattern's, copied or moved
	// there or on a duplicated track, plays a copy of the pattern on its own track. A track without pattern events
	// of its own gets copies of all the patterns of that track, else just the one (reference manual pp. 936-937).
	// The patterns of a removed track go. Emits pattern.added, midiPart.changed and pattern.removed.
	void adoptPatterns(Project& project, CommandContext& context);

	// Applies the fields json has to pattern, lanes replacing the whole list. Switching between drum and melodic
	// mode clears the lanes, from poly to mono mode keeps the lowest step at each position, as the reference manual
	// p. 913 says. Throws std::invalid_argument naming the field, for a value a pattern can't have.
	Pattern patternFromJson(const Json& json, Pattern pattern);

	// Registers pattern.add, pattern.set, pattern.remove, pattern.convert, pattern.export and pattern.import.
	void registerPatternCommands(ProjectModel& model, Engine& engine);
}
