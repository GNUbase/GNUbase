// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"
#include "engine/schema.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The project document as JSON: what project.get returns and the events carry, and what the DAWproject extension
// entry stores. projectFromJson(toJson(project)) gives back the same project.
namespace gnubase::engine
{
	// Note velocities and controller values are 0 to 1 in JSON, whatever the internal resolution.
	inline constexpr double g_maxVelocity = 65535.0;
	inline constexpr double g_maxControllerValue = 4294967295.0;

	const char* trackTypeName(TrackType type);
	std::optional<TrackType> trackTypeFromName(const std::string& name);
	const char* timeBaseName(time::TimeBase base);
	time::TimeBase timeBaseFromName(const std::string& name);
	const char* directionName(BusDirection direction);
	const char* fadeShapeName(FadeShape shape);
	std::optional<FadeShape> fadeShapeFromName(const std::string& name);
	const char* crossfadeModeName(CrossfadeMode mode);
	std::optional<CrossfadeMode> crossfadeModeFromName(const std::string& name);

	// The names of every fade shape and crossfade mode, for schemas.
	std::vector<std::string> fadeShapeNames();
	std::vector<std::string> crossfadeModeNames();

	Json toJson(const Fade& fade);
	Json toJson(const Crossfade& crossfade); // null without one
	Json toJson(const AutoFades& settings);
	// The inverses. Missing properties keep the value of fallback; unknown names throw std::invalid_argument.
	Fade fadeFromJson(const Json& json, const Fade& fallback = {});
	AutoFades autoFadesFromJson(const Json& json, const AutoFades& fallback = {});

	Json toJson(const Bus& bus);
	Json toJson(const Track& track);
	Json toJson(const Channel& channel);
	Json toJson(const MediaFile& media);
	Json toJson(const Clip& clip);
	Json toJson(const AudioEvent& event);
	Json toJson(const Note& note);
	Json toJson(const Controller& controller);
	Json toJson(const MidiPart& part);
	Json toJson(const Marker& marker);

	// The whole project, with the tracks from top to bottom. The id counter is left out: undo doesn't restore it,
	// so that ids stay unique.
	Json toJson(const Project& project);

	// The inverse of toJson(const Project&). Ids continue at nextId if the JSON has it, or else after the highest
	// id. Throws std::exception for JSON that isn't such a project.
	Project projectFromJson(const Json& json);
}
