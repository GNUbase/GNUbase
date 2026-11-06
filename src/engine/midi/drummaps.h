// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"

#include <cstdint>
#include <string>

namespace gnubase::engine
{
	class Engine;
	class ProjectModel;
}

// Drum maps (#129, reference manual pp. 1235-1240): docs/features/drum-editor.md describes them.
namespace gnubase::engine::midi
{
	// The GM Map (p. 1237): General MIDI's percussion names, every I-note and O-note its own pitch, channel 10 and a
	// 1/16 snap. Its id is 0.
	DrumMap gmDrumMap();
	// New Map (p. 1236): "Empty Map", the sounds named Sound 1, Sound 2 and so on, on the track's channel.
	DrumMap emptyDrumMap();

	// The drum map a track's notes play through, or none.
	const DrumMap* drumMapOf(const Project& project, ObjectId track);
	// The pitch that a note played in on a track becomes: the first sound, by pitch, whose I-note it is, or itself.
	uint8_t inputPitch(const Project& project, ObjectId track, uint8_t played);

	// A drum map file (.drm): XML with the map's name, its sounds and their order.
	std::string writeDrumMapFile(const DrumMap& map);
	// Throws std::runtime_error if the text isn't one.
	DrumMap readDrumMapFile(const std::string& text);

	// Registers drumMap.add, drumMap.set, drumMap.remove, drumMap.export and drumMap.import.
	void registerDrumMapCommands(ProjectModel& model, Engine& engine);
}
