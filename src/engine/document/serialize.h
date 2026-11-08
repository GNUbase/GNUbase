// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"
#include "engine/schema.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The project document as JSON: what project.get returns and the events carry, and what the DAWproject extension
// entry stores. projectFromJson(toJson(project)) gives back the same project, except for the newer build's grid types
// in its snap settings (SnapSettings::newerGrids), which only saving writes (saveDawProject).
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
	// A child bus's parentChannels. Numbers beyond int are -1, which busProblem refuses, rather than wrapping around
	// to channels that exist.
	std::vector<int> parentChannelsFromJson(const Json& json);
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
	// An audio event's volume curve: [{position, volume, curve}], and back, unchecked.
	Json toJson(const std::vector<VolumePoint>& curve);
	std::vector<VolumePoint> volumeCurveFromJson(const Json& json);
	// The inverses. Missing properties keep the value of fallback; unknown names throw std::invalid_argument.
	Fade fadeFromJson(const Json& json, const Fade& fallback = {});
	AutoFades autoFadesFromJson(const Json& json, const AutoFades& fallback = {});

	// A color set as an array of {name, color}, and back: at least one color, each #rrggbb, or std::invalid_argument.
	Json toJson(const std::vector<ProjectColor>& colors);
	std::vector<ProjectColor> colorsFromJson(const Json& json);

	// The project setup's settings, which project.get has at its top level. projectSetupFromJson changes those the
	// JSON has, and throws std::invalid_argument for unknown names and values that don't go together (see
	// projectSetupProblem).
	Json toJson(const ProjectSetup& setup);
	ProjectSetup projectSetupFromJson(const Json& json, const ProjectSetup& fallback = {});

	const char* markerTypeName(MarkerType type);
	std::optional<MarkerType> markerTypeFromName(const std::string& name);

	Json toJson(const Bus& bus);
	Json toJson(const Track& track);
	Json toJson(const Channel& channel);
	// A channel's strip sections as its pre, eq and strip objects, and back: sectionsFromJson changes what the JSON
	// has and throws std::invalid_argument for values out of range.
	Json toJson(const ChannelSections& sections);
	ChannelSections sectionsFromJson(const Json& json, const ChannelSections& fallback = {});
	// A MIDI or instrument channel's MIDI modifiers, and back: modifiersFromJson changes what the JSON has and throws
	// std::invalid_argument for values out of range.
	Json toJson(const midi::Modifiers& modifiers);
	midi::Modifiers modifiersFromJson(const Json& json, const midi::Modifiers& fallback = {});
	Json toJson(const Slot& slot); // its state as base64
	Json toJson(const MediaFile& media);
	Json toJson(const ClipRegion& region);
	Json toJson(const Clip& clip);
	Json toJson(const ClipWarp& warp);
	ClipWarp clipWarpFromJson(const Json& json); // as saved: values out of range clamped, edits out of order left out
	Json toJson(const PoolFolder& folder);
	Json toJson(const AudioEvent& event);
	Json toJson(const AudioPart& part);
	Json toJson(const Note& note);
	Json toJson(const Controller& controller);
	// A controller's type and the values the type uses, without checking them; the others are zero. Its id and
	// position are left to the caller.
	Controller controllerFromJson(const Json& json);
	Json toJson(const MidiPart& part);
	Json toJson(const Marker& marker);
	const char* automationTargetTypeName(AutomationTargetType type);
	std::optional<AutomationTargetType> automationTargetTypeFromName(const std::string& name);
	// Whether json is a parameter id: a whole number from 0 to UINT32_MAX.
	bool isParameterId(const Json& json);
	const char* automationShapeName(AutomationShape shape);
	std::optional<AutomationShape> automationShapeFromName(const std::string& name);
	// {"type": ..., plus "parameter" for instrument, "number" and "channel" for controller, "channel" for pitchBend
	// and channelPressure}.
	Json toJson(const AutomationTarget& target);
	Json toJson(const AutomationPoint& point);
	Json toJson(const AutomationLane& lane); // with its points, sorted by position
	Json toJson(const MixerView& view);      // {"hidden": [ids], "hiddenTypes": [types]}
	// {hidden, sync, configurations: [{name, hidden}], configuration}; trackVisibilityFromJson takes what is valid
	// JSON.
	Json toJson(const TrackVisibility& visibility);
	TrackVisibility trackVisibilityFromJson(const Json& json);
	// {workspaces: [{name, layout}], active: {type, name}}; workspacesFromJson takes what is valid.
	Json toJson(const ProjectWorkspaces& workspaces);
	ProjectWorkspaces workspacesFromJson(const Json& json);
	// {id, name, channels, volume, pan, mute, record, vca}; linkGroupFromJson leaves what the JSON doesn't have.
	Json toJson(const LinkGroup& group);
	LinkGroup linkGroupFromJson(const Json& json, const LinkGroup& fallback = {});
	// {id, name, sounds: [{pitch, name, snap, mute, iNote, oNote, channel (null: Any), output}], order}. The sound's
	// pitch is its index. drumSoundFromJson leaves what the JSON doesn't have and throws std::invalid_argument, naming
	// the field, for a value a sound can't have; drumMapFromJson does too, also for an order that isn't each pitch
	// once.
	Json toJson(const DrumMap& map);
	DrumSound drumSoundFromJson(const Json& json, DrumSound sound);
	DrumMap drumMapFromJson(const Json& json, DrumMap map);
	// [{"position": ticks, "bpm": ..., "ramp": ...}], and back; tempoFromJson throws std::invalid_argument for points a
	// tempo map can't have.
	Json toJson(const time::TempoMap& tempo);
	time::TempoMap tempoFromJson(const Json& points);

	// The arranger (#167) as arranger.get describes it, arranger.changed announces it and the project file keeps it:
	// the arranger track (0 for none); its events in timeline order, positions in the track's time base and start
	// and end in samples; the chains with their steps, the active chain (chain), mode and jumpMode.
	Json arrangerJson(const Project& project);

	// The transpose track (#168) as transpose.get describes it, transpose.changed announces it and the project file
	// keeps it: the transpose track (0 for none); its events in timeline order, positions in the track's time base and
	// start in samples, with their values; mute, keepInOctave and lock.
	Json transposeJson(const Project& project);

	// The whole project, with the tracks from top to bottom. The id counter is left out: undo doesn't restore it,
	// so that ids stay unique.
	Json toJson(const Project& project);

	// The inverse of toJson(const Project&). Ids continue at nextId if the JSON has it, or else after the highest
	// id. Throws std::exception for JSON that isn't such a project, and std::invalid_argument for automation that
	// the automation commands wouldn't make: on no track or a track it doesn't fit, with target numbers out of
	// range, or with points whose position, value or curve isn't a finite number in its range.
	Project projectFromJson(const Json& json);
}
