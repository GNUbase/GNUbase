// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"
#include "engine/schema.h"

#include <map>
#include <string>

namespace gnubase::engine
{
	class Engine;
	class ProjectModel;

	// The map as project.get lists it.
	Json toJson(const ArticulationMap& map);
}

// Articulation maps (#175, reference manual pp. 1262-1292): docs/features/articulation-maps.md describes them.
namespace gnubase::engine::midi
{
	// The articulation map a track's notes play through, or none.
	const ArticulationMap* articulationMapOf(const Project& project, ObjectId track);

	// The sound slot each of a part's notes plays, by note id, for the notes that have one: the slot whose
	// articulations are those of the note (its attributes and the directions in effect at its start), else the
	// closest match, the slot that matches the most important groups, else the first one, the default slot.
	std::map<ObjectId, size_t> soundSlots(const ArticulationMap& map, const MidiPart& part);

	// Remote triggers (pp. 1263, 1268) in what a recording brings to a track with the map, as recording.add takes
	// it: notes and controllers with positions in samples. Takes the notes (key switches) or program changes that
	// trigger slots out and adds directions (articulation controller events) where they select a slot, and the
	// attributes of the slot that is selected where a note starts to the note.
	void applyRemoteTriggers(const ArticulationMap& map, Json& notes, Json& controllers);

	// Applies the fields json has to map. Throws std::invalid_argument naming the field, for a value a map can't have.
	ArticulationMap articulationMapFromJson(const Json& json, ArticulationMap map);

	// A new map (Add Map, p. 1280): one group and neither articulations nor sound slots.
	ArticulationMap newArticulationMap();

	// Registers articulationMap.add, articulationMap.set, articulationMap.remove, articulationMap.export and
	// articulationMap.import.
	void registerArticulationMapCommands(ProjectModel& model, Engine& engine);
}
