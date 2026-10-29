// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"
#include "engine/schema.h"

#include <cstdint>
#include <optional>
#include <string>

// The project document as JSON: what project.get returns and the events carry, and what the DAWproject extension
// entry stores. projectFromJson(toJson(project)) gives back the same project.
namespace gnubase::engine
{
	// Note velocities and controller values are 0 to 1 in JSON, whatever the internal resolution.
	inline constexpr double g_maxVelocity = 65535.0;
	inline constexpr double g_maxControllerValue = 4294967295.0;

	const char* trackTypeName(TrackType type);
	std::optional<TrackType> trackTypeFromName(const std::string& name);
	const char* controllerTypeName(ControllerType type);
	std::optional<ControllerType> controllerTypeFromName(const std::string& name);
	const char* timeBaseName(time::TimeBase base);
	time::TimeBase timeBaseFromName(const std::string& name);
	const char* directionName(BusDirection direction);
	const char* markerTypeName(MarkerType type);
	std::optional<MarkerType> markerTypeFromName(const std::string& name);

	Json toJson(const Bus& bus);
	Json toJson(const Track& track);
	Json toJson(const Channel& channel);
	Json toJson(const MediaFile& media);
	Json toJson(const Clip& clip);
	Json toJson(const AudioEvent& event);
	Json toJson(const Note& note);
	Json toJson(const Controller& controller);
	// A controller's type and the values the type uses, without checking them; the others are zero. Its id and
	// position are left to the caller.
	Controller controllerFromJson(const Json& json);
	Json toJson(const MidiPart& part);
	Json toJson(const Marker& marker);
	const char* automationTargetTypeName(AutomationTargetType type);
	std::optional<AutomationTargetType> automationTargetTypeFromName(const std::string& name);
	const char* automationShapeName(AutomationShape shape);
	std::optional<AutomationShape> automationShapeFromName(const std::string& name);
	// {"type": ..., plus "parameter" for instrument, "number" and "channel" for controller, "channel" for pitchBend
	// and channelPressure}.
	Json toJson(const AutomationTarget& target);
	Json toJson(const AutomationPoint& point);
	Json toJson(const AutomationLane& lane); // with its points, sorted by position

	// The whole project, with the tracks from top to bottom. The id counter is left out: undo doesn't restore it,
	// so that ids stay unique.
	Json toJson(const Project& project);

	// The inverse of toJson(const Project&). Ids continue at nextId if the JSON has it, or else after the highest
	// id. Throws std::exception for JSON that isn't such a project, and std::invalid_argument for automation that
	// the automation commands wouldn't make: on no track or a track it doesn't fit, with target numbers out of
	// range, or with points whose position, value or curve isn't a finite number in its range.
	Project projectFromJson(const Json& json);
}
