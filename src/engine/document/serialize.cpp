// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/serialize.h"

#include "engine/processors/builtin.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		constexpr struct
		{
			TrackType type;
			const char* name;
		} g_trackTypeNames[] = {
			{TrackType::Audio, "audio"},
			{TrackType::Instrument, "instrument"},
			{TrackType::Midi, "midi"},
			{TrackType::Folder, "folder"},
			{TrackType::Group, "group"},
			{TrackType::Effect, "effect"},
			{TrackType::Marker, "marker"},
		};

		constexpr struct
		{
			ControllerType type;
			const char* name;
		} g_controllerTypeNames[] = {
			{ControllerType::Control, "controller"},
			{ControllerType::PitchBend, "pitchBend"},
			{ControllerType::ChannelPressure, "channelPressure"},
			{ControllerType::PolyPressure, "polyPressure"},
			{ControllerType::Program, "program"},
			{ControllerType::SysEx, "sysEx"},
		};

		constexpr struct
		{
			FadeShape shape;
			const char* name;
		} g_fadeShapeNames[] = {
			{FadeShape::Linear, "linear"},
			{FadeShape::Exponential, "exponential"},
			{FadeShape::Logarithmic, "logarithmic"},
			{FadeShape::SCurve, "sCurve"},
			{FadeShape::EqualPower, "equalPower"},
		};

		constexpr struct
		{
			AutomationTargetType type;
			const char* name;
		} g_automationTargetNames[] = {
			{AutomationTargetType::Volume, "volume"},
			{AutomationTargetType::Pan, "pan"},
			{AutomationTargetType::Instrument, "instrument"},
			{AutomationTargetType::Controller, "controller"},
			{AutomationTargetType::PitchBend, "pitchBend"},
			{AutomationTargetType::ChannelPressure, "channelPressure"},
		};

		constexpr struct
		{
			AutomationShape shape;
			const char* name;
		} g_automationShapeNames[] = {
			{AutomationShape::Step, "step"},
			{AutomationShape::Linear, "linear"},
			{AutomationShape::Curve, "curve"},
		};

		constexpr struct
		{
			CrossfadeMode mode;
			const char* name;
		} g_crossfadeModeNames[] = {
			{CrossfadeMode::EqualGain, "equalGain"},
			{CrossfadeMode::EqualPower, "equalPower"},
			{CrossfadeMode::Custom, "custom"},
		};

		FadeShape shapeFrom(const Json& json)
		{
			const auto shape = fadeShapeFromName(json.get<std::string>());
			if (!shape)
				throw std::invalid_argument("unknown fade shape " + json.dump());
			return *shape;
		}

		CrossfadeMode modeFrom(const Json& json)
		{
			const auto mode = crossfadeModeFromName(json.get<std::string>());
			if (!mode)
				throw std::invalid_argument("unknown crossfade mode " + json.dump());
			return *mode;
		}

		Crossfade crossfadeFrom(const Json& json)
		{
			Crossfade crossfade;
			if (json.is_null())
				return crossfade;
			crossfade.from = json.at("from").get<ObjectId>();
			crossfade.mode = modeFrom(json.at("mode"));
			crossfade.shape = shapeFrom(json.at("shape"));
			crossfade.outShape = shapeFrom(json.at("outShape"));
			crossfade.length = json.at("length").get<time::Samples>();
			crossfade.outLength = json.at("outLength").get<time::Samples>();
			return crossfade;
		}

		Json colorJson(const std::string& color)
		{
			return color.empty() ? Json(nullptr) : Json(color);
		}

		std::string colorFrom(const Json& json)
		{
			return json.is_null() ? std::string() : json.get<std::string>();
		}

		Json timingJson(const NoteTiming& timing)
		{
			return {{"position", timing.position}, {"length", timing.length},
				{"velocity", timing.velocity / g_maxVelocity}};
		}

		uint16_t velocityFrom(const Json& json)
		{
			return static_cast<uint16_t>(std::lround(json.get<double>() * g_maxVelocity));
		}

		NoteTiming timingFrom(const Json& json)
		{
			return {json.at("position").get<time::Ticks>(), json.at("length").get<time::Ticks>(),
				velocityFrom(json.at("velocity"))};
		}

		template <typename Map> Json toJsonArray(const Map& objects)
		{
			Json array = Json::array();
			for (const auto& entry : objects)
				array.push_back(toJson(entry.second));
			return array;
		}
	}

	const char* trackTypeName(TrackType type)
	{
		for (const auto& entry : g_trackTypeNames)
			if (entry.type == type)
				return entry.name;
		return "audio"; // not reached: every type is in the table
	}

	std::optional<TrackType> trackTypeFromName(const std::string& name)
	{
		for (const auto& entry : g_trackTypeNames)
			if (name == entry.name)
				return entry.type;
		return std::nullopt;
	}

	const char* controllerTypeName(ControllerType type)
	{
		for (const auto& entry : g_controllerTypeNames)
			if (entry.type == type)
				return entry.name;
		return "controller"; // not reached: every type is in the table
	}

	std::optional<ControllerType> controllerTypeFromName(const std::string& name)
	{
		for (const auto& entry : g_controllerTypeNames)
			if (name == entry.name)
				return entry.type;
		return std::nullopt;
	}

	const char* automationTargetTypeName(AutomationTargetType type)
	{
		for (const auto& entry : g_automationTargetNames)
			if (entry.type == type)
				return entry.name;
		return "volume"; // not reached: every type is in the table
	}

	std::optional<AutomationTargetType> automationTargetTypeFromName(const std::string& name)
	{
		for (const auto& entry : g_automationTargetNames)
			if (name == entry.name)
				return entry.type;
		return std::nullopt;
	}

	const char* automationShapeName(AutomationShape shape)
	{
		for (const auto& entry : g_automationShapeNames)
			if (entry.shape == shape)
				return entry.name;
		return "linear"; // not reached: every shape is in the table
	}

	std::optional<AutomationShape> automationShapeFromName(const std::string& name)
	{
		for (const auto& entry : g_automationShapeNames)
			if (name == entry.name)
				return entry.shape;
		return std::nullopt;
	}

	const char* timeBaseName(time::TimeBase base)
	{
		return base == time::TimeBase::Musical ? "musical" : "linear";
	}

	time::TimeBase timeBaseFromName(const std::string& name)
	{
		return name == "linear" ? time::TimeBase::Linear : time::TimeBase::Musical;
	}

	const char* directionName(BusDirection direction)
	{
		return direction == BusDirection::Input ? "input" : "output";
	}

	const char* fadeShapeName(FadeShape shape)
	{
		for (const auto& entry : g_fadeShapeNames)
			if (entry.shape == shape)
				return entry.name;
		return "linear"; // not reached: every shape is in the table
	}

	std::optional<FadeShape> fadeShapeFromName(const std::string& name)
	{
		for (const auto& entry : g_fadeShapeNames)
			if (name == entry.name)
				return entry.shape;
		return std::nullopt;
	}

	const char* crossfadeModeName(CrossfadeMode mode)
	{
		for (const auto& entry : g_crossfadeModeNames)
			if (entry.mode == mode)
				return entry.name;
		return "equalGain"; // not reached: every mode is in the table
	}

	std::optional<CrossfadeMode> crossfadeModeFromName(const std::string& name)
	{
		for (const auto& entry : g_crossfadeModeNames)
			if (name == entry.name)
				return entry.mode;
		return std::nullopt;
	}

	std::vector<std::string> fadeShapeNames()
	{
		std::vector<std::string> names;
		for (const auto& entry : g_fadeShapeNames)
			names.emplace_back(entry.name);
		return names;
	}

	std::vector<std::string> crossfadeModeNames()
	{
		std::vector<std::string> names;
		for (const auto& entry : g_crossfadeModeNames)
			names.emplace_back(entry.name);
		return names;
	}

	Json toJson(const Fade& fade)
	{
		return {{"length", fade.length}, {"shape", fadeShapeName(fade.shape)}};
	}

	Json toJson(const Crossfade& crossfade)
	{
		if (crossfade.from == 0)
			return nullptr;
		return {{"from", crossfade.from}, {"mode", crossfadeModeName(crossfade.mode)},
			{"shape", fadeShapeName(crossfade.shape)}, {"outShape", fadeShapeName(crossfade.outShape)},
			{"length", crossfade.length}, {"outLength", crossfade.outLength}};
	}

	Json toJson(const AutoFades& settings)
	{
		return {{"fadeIn", settings.fadeIn}, {"fadeOut", settings.fadeOut}, {"crossfades", settings.crossfades},
			{"length", settings.length}, {"fadeInShape", fadeShapeName(settings.fadeInShape)},
			{"fadeOutShape", fadeShapeName(settings.fadeOutShape)},
			{"crossfadeMode", crossfadeModeName(settings.crossfadeMode)},
			{"crossfadeShape", fadeShapeName(settings.crossfadeShape)}};
	}

	Fade fadeFromJson(const Json& json, const Fade& fallback)
	{
		Fade fade = fallback;
		if (json.contains("length"))
			fade.length = json.at("length").get<time::Samples>();
		if (json.contains("shape"))
			fade.shape = shapeFrom(json.at("shape"));
		return fade;
	}

	AutoFades autoFadesFromJson(const Json& json, const AutoFades& fallback)
	{
		AutoFades settings = fallback;
		settings.fadeIn = json.value("fadeIn", settings.fadeIn);
		settings.fadeOut = json.value("fadeOut", settings.fadeOut);
		settings.crossfades = json.value("crossfades", settings.crossfades);
		settings.length = json.value("length", settings.length);
		if (json.contains("fadeInShape"))
			settings.fadeInShape = shapeFrom(json.at("fadeInShape"));
		if (json.contains("fadeOutShape"))
			settings.fadeOutShape = shapeFrom(json.at("fadeOutShape"));
		if (json.contains("crossfadeMode"))
			settings.crossfadeMode = modeFrom(json.at("crossfadeMode"));
		if (json.contains("crossfadeShape"))
			settings.crossfadeShape = shapeFrom(json.at("crossfadeShape"));
		return settings;
	}

	const char* markerTypeName(MarkerType type)
	{
		return type == MarkerType::Cycle ? "cycle" : "position";
	}

	std::optional<MarkerType> markerTypeFromName(const std::string& name)
	{
		if (name == "position")
			return MarkerType::Position;
		if (name == "cycle")
			return MarkerType::Cycle;
		return std::nullopt;
	}

	Json toJson(const Bus& bus)
	{
		return {{"id", bus.id}, {"name", bus.name}, {"direction", directionName(bus.direction)},
			{"channels", bus.channels}};
	}

	Json toJson(const Track& track)
	{
		return {{"id", track.id}, {"type", trackTypeName(track.type)}, {"name", track.name},
			{"color", colorJson(track.color)}, {"timeBase", timeBaseName(track.timeBase)}, {"parent", track.parent},
			{"channel", track.channel}, {"autoFades", track.autoFades ? toJson(*track.autoFades) : Json(nullptr)}};
	}

	Json toJson(const Channel& channel)
	{
		return {{"id", channel.id}, {"track", channel.track}, {"channels", channel.channels}, {"input", channel.input},
			{"output", channel.output}, {"instrument", channel.instrument}, {"volume", channel.volume},
			{"pan", channel.pan}};
	}

	Json toJson(const MediaFile& media)
	{
		return {{"id", media.id}, {"path", media.path}, {"sampleRate", media.sampleRate}, {"channels", media.channels},
			{"length", media.length}};
	}

	Json toJson(const Clip& clip)
	{
		return {{"id", clip.id}, {"name", clip.name}, {"media", clip.media}, {"fadeIn", toJson(clip.fadeIn)},
			{"fadeOut", toJson(clip.fadeOut)}};
	}

	Json toJson(const AudioEvent& event)
	{
		return {{"id", event.id}, {"track", event.track}, {"clip", event.clip}, {"position", event.position.value},
			{"offset", event.offset}, {"length", event.length}, {"volume", event.volume}, {"mute", event.mute},
			{"fadeIn", toJson(event.fadeIn)}, {"fadeOut", toJson(event.fadeOut)},
			{"crossfade", toJson(event.crossfade)}};
	}

	Json toJson(const Note& note)
	{
		Json json = {{"id", note.id}, {"position", note.position}, {"length", note.length}, {"pitch", note.pitch},
			{"velocity", note.velocity / g_maxVelocity}, {"releaseVelocity", note.releaseVelocity / g_maxVelocity},
			{"channel", note.channel}};
		if (note.origin)
			json["origin"] = {
				{"original", timingJson(note.origin->original)}, {"quantized", timingJson(note.origin->quantized)}};
		return json;
	}

	Json toJson(const Controller& controller)
	{
		Json json = {
			{"id", controller.id}, {"position", controller.position}, {"type", controllerTypeName(controller.type)}};
		switch (controller.type)
		{
		case ControllerType::Control:
		case ControllerType::PolyPressure:
			json["number"] = controller.number;
			json["value"] = controller.value / g_maxControllerValue;
			break;
		case ControllerType::PitchBend:
		case ControllerType::ChannelPressure:
			json["value"] = controller.value / g_maxControllerValue;
			break;
		case ControllerType::Program:
			json["number"] = controller.number;
			if (controller.value != g_noBank)
				json["bank"] = controller.value;
			break;
		case ControllerType::SysEx:
			json["data"] = controller.data;
			return json;
		}
		json["channel"] = controller.channel;
		return json;
	}

	Controller controllerFromJson(const Json& json)
	{
		Controller controller;
		controller.type =
			controllerTypeFromName(json.value("type", std::string("controller"))).value_or(ControllerType::Control);
		// Only what the type uses; the rest stays zero, as toJson leaves it out.
		const ControllerType type = controller.type;
		if (type == ControllerType::Control || type == ControllerType::PolyPressure || type == ControllerType::Program)
			controller.number = json.value("number", uint8_t{0});
		if (type != ControllerType::SysEx)
			controller.channel = json.value("channel", uint8_t{0});
		if (type == ControllerType::Program)
			controller.value = json.value("bank", g_noBank);
		else if (type != ControllerType::SysEx && json.contains("value"))
			controller.value =
				static_cast<uint32_t>(std::llround(json.at("value").get<double>() * g_maxControllerValue));
		if (type == ControllerType::SysEx)
			controller.data = json.at("data").get<std::vector<uint8_t>>();
		return controller;
	}

	Json toJson(const MidiPart& part)
	{
		return {{"id", part.id}, {"track", part.track}, {"name", part.name}, {"position", part.position.value},
			{"length", part.length}, {"notes", toJsonArray(part.notes)},
			{"controllers", toJsonArray(part.controllers)}};
	}

	Json toJson(const Marker& marker)
	{
		return {{"id", marker.id}, {"track", marker.track}, {"type", markerTypeName(marker.type)},
			{"number", marker.number}, {"position", marker.position.value},
			{"end", marker.type == MarkerType::Cycle ? Json(marker.end.value) : Json(nullptr)}, {"name", marker.name},
			{"color", colorJson(marker.color)}};
	}

	Json toJson(const AutomationTarget& target)
	{
		Json json = {{"type", automationTargetTypeName(target.type)}};
		switch (target.type)
		{
		case AutomationTargetType::Volume:
		case AutomationTargetType::Pan:
			break;
		case AutomationTargetType::Instrument:
			json["parameter"] = target.parameter;
			break;
		case AutomationTargetType::Controller:
			json["number"] = target.number;
			json["channel"] = target.channel;
			break;
		case AutomationTargetType::PitchBend:
		case AutomationTargetType::ChannelPressure:
			json["channel"] = target.channel;
			break;
		}
		return json;
	}

	Json toJson(const AutomationPoint& point)
	{
		Json json = {{"id", point.id}, {"position", point.position.value}, {"value", point.value},
			{"shape", automationShapeName(point.shape)}};
		if (point.shape == AutomationShape::Curve)
			json["curve"] = point.curve;
		return json;
	}

	Json toJson(const AutomationLane& lane)
	{
		std::vector<const AutomationPoint*> points;
		points.reserve(lane.points.size());
		for (const auto& [id, point] : lane.points)
			points.push_back(&point);
		std::stable_sort(points.begin(), points.end(),
			[](const AutomationPoint* a, const AutomationPoint* b) { return a->position.value < b->position.value; });
		Json list = Json::array();
		for (const AutomationPoint* point : points)
			list.push_back(toJson(*point));
		return {{"id", lane.id}, {"track", lane.track}, {"target", toJson(lane.target)}, {"read", lane.read},
			{"points", std::move(list)}};
	}

	Json toJson(const Project& project)
	{
		Json busses = Json::array();
		for (const Bus& bus : project.busses)
			busses.push_back(toJson(bus));
		Json tracks = Json::array();
		for (const ObjectId id : project.trackOrder())
			tracks.push_back(toJson(project.trackById.at(id)));
		Json tempo = Json::array();
		for (const time::TempoPoint& point : project.tempo.points())
			tempo.push_back({{"position", point.position}, {"bpm", point.bpm}, {"ramp", point.ramp}});
		Json signatures = Json::array();
		for (const time::SignatureChange& change : project.signatures.changes())
			signatures.push_back({{"bar", change.bar}, {"numerator", change.signature.numerator},
				{"denominator", change.signature.denominator}});
		return {{"name", project.name}, {"sampleRate", project.sampleRate}, {"tempo", tempo},
			{"signatures", signatures}, {"autoFades", toJson(project.autoFades)}, {"busses", busses},
			{"tracks", tracks}, {"channels", toJsonArray(project.channels)}, {"media", toJsonArray(project.media)},
			{"clips", toJsonArray(project.clips)}, {"audioEvents", toJsonArray(project.audioEvents)},
			{"midiParts", toJsonArray(project.midiParts)}, {"markers", toJsonArray(project.markers)},
			{"automation", toJsonArray(project.automation)}, {"quantize", midi::toJson(project.quantize)},
			{"autoQuantize", project.autoQuantize}, {"activeMarkerTrack", project.activeMarkerTrack}};
	}

	namespace
	{
		bool numberTaken(const Project& project, const Marker& marker, int number)
		{
			return std::any_of(project.markers.begin(), project.markers.end(),
				[&marker, number](const auto& entry)
				{
					const Marker& other = entry.second;
					return other.id != marker.id && other.track == marker.track && other.type == marker.type &&
						other.number == number;
				});
		}

		// Markers without a track go onto a new marker track at the top, which becomes the active one if none is.
		// Markers without a number get the first free ones on their track, in timeline order. Cycle markers shorter
		// than a sample get longer.
		void placeOldMarkers(Project& project)
		{
			const bool trackless = std::any_of(project.markers.begin(), project.markers.end(),
				[](const auto& entry) { return entry.second.track == 0; });
			if (trackless)
			{
				Track track;
				track.id = project.newId();
				track.type = TrackType::Marker;
				track.name = "Markers";
				project.trackById[track.id] = track;
				project.tracks.insert(project.tracks.begin(), track.id);
				if (project.activeMarkerTrack == 0)
					project.activeMarkerTrack = track.id;
				for (auto& [id, marker] : project.markers)
					if (marker.track == 0)
						marker.track = track.id;
			}
			std::vector<Marker*> unnumbered;
			for (auto& [id, marker] : project.markers)
			{
				keepMinimumLength(project, marker);
				if (marker.number == 0)
					unnumbered.push_back(&marker);
			}
			std::stable_sort(unnumbered.begin(), unnumbered.end(),
				[](const Marker* a, const Marker* b) { return a->position.value < b->position.value; });
			for (Marker* marker : unnumbered)
			{
				int number = 1;
				while (numberTaken(project, *marker, number))
					++number;
				marker->number = number;
			}
		}

		std::string numberText(double value)
		{
			std::ostringstream text;
			text << value;
			return text.str();
		}

		// A number from a file: finite and from minimum to maximum.
		double checkedNumber(const Json& value, double minimum, double maximum, const std::string& name)
		{
			const auto number = value.get<double>();
			if (!std::isfinite(number) || number < minimum || number > maximum)
				throw std::invalid_argument(
					name + " must be a number from " + numberText(minimum) + " to " + numberText(maximum));
			return number;
		}

		// An optional whole number from a file, 0 if it is missing.
		int64_t checkedInteger(const Json& json, const char* key, int64_t maximum, const std::string& name)
		{
			const auto value = json.value(key, int64_t{0});
			if (value < 0 || value > maximum)
				throw std::invalid_argument(name + "." + key + " must be from 0 to " + std::to_string(maximum));
			return value;
		}

		// An automation lane from a file, checked as far as the automation commands check what they get without
		// the instrument (whose parameters may have changed): the track it fits, the target's numbers and its
		// points. NaN or a curve that overflows exp() would otherwise reach the audio.
		AutomationLane automationLaneFromJson(const Project& project, const Json& item)
		{
			AutomationLane lane;
			lane.id = item.at("id").get<ObjectId>();
			lane.track = item.at("track").get<ObjectId>();
			lane.read = item.at("read").get<bool>();
			const std::string name = "automation lane " + std::to_string(lane.id);
			const auto found = project.trackById.find(lane.track);
			if (found == project.trackById.end() || project.channels.count(found->second.channel) == 0)
				throw std::invalid_argument(name + " is on no track with a channel");
			const Track& track = found->second;

			const Json& target = item.at("target");
			const std::optional<AutomationTargetType> type =
				automationTargetTypeFromName(target.at("type").get<std::string>());
			if (!type.has_value())
				throw std::invalid_argument(name + " has an unknown target type " + target.at("type").dump());
			lane.target.type = *type;
			lane.target.parameter =
				static_cast<uint32_t>(checkedInteger(target, "parameter", UINT32_MAX, name + " target"));
			lane.target.number = static_cast<uint8_t>(checkedInteger(target, "number", 127, name + " target"));
			lane.target.channel = static_cast<uint8_t>(checkedInteger(target, "channel", 15, name + " target"));
			const bool midi = track.type == TrackType::Midi || track.type == TrackType::Instrument;
			bool fits = midi;
			double minimum = 0.0;
			double maximum = 1.0;
			switch (lane.target.type)
			{
			case AutomationTargetType::Volume:
			case AutomationTargetType::Pan:
				fits = track.type != TrackType::Midi && project.channels.at(track.channel).channels > 0;
				minimum = lane.target.type == AutomationTargetType::Volume ? g_minChannelVolume : -1.0;
				maximum = lane.target.type == AutomationTargetType::Volume ? g_maxChannelVolume : 1.0;
				break;
			case AutomationTargetType::Instrument:
				// The instrument's range applies when it plays; the instrument may have changed since.
				fits = track.type == TrackType::Instrument;
				minimum = std::numeric_limits<double>::lowest();
				maximum = std::numeric_limits<double>::max();
				break;
			case AutomationTargetType::Controller:
			case AutomationTargetType::PitchBend:
			case AutomationTargetType::ChannelPressure:
				break;
			}
			if (!fits)
				throw std::invalid_argument(name + " has a target that track " + std::to_string(track.id) + " lacks");

			for (const Json& p : item.at("points"))
			{
				AutomationPoint point;
				point.id = p.at("id").get<ObjectId>();
				const std::string pointName = name + " point " + std::to_string(point.id);
				point.position = {track.timeBase, p.at("position").get<int64_t>()};
				if (point.position.value < 0)
					throw std::invalid_argument(pointName + " has a negative position");
				point.value = checkedNumber(p.at("value"), minimum, maximum, pointName + " value");
				const std::optional<AutomationShape> shape = automationShapeFromName(p.at("shape").get<std::string>());
				if (!shape.has_value())
					throw std::invalid_argument(pointName + " has an unknown shape " + p.at("shape").dump());
				point.shape = *shape;
				if (point.shape == AutomationShape::Curve)
					point.curve = checkedNumber(p.value("curve", Json(0.0)), -1.0, 1.0, pointName + " curve");
				lane.points[point.id] = point;
			}
			return lane;
		}
	}

	Project projectFromJson(const Json& json)
	{
		Project project;
		project.name = json.at("name").get<std::string>();
		project.sampleRate = json.at("sampleRate").get<double>();
		std::vector<time::TempoPoint> tempo;
		for (const Json& point : json.at("tempo"))
			tempo.push_back(
				{point.at("position").get<time::Ticks>(), point.at("bpm").get<double>(), point.at("ramp").get<bool>()});
		project.tempo = time::TempoMap(tempo);
		std::vector<time::SignatureChange> signatures;
		for (const Json& change : json.at("signatures"))
			signatures.push_back({change.at("bar").get<int64_t>(),
				{change.at("numerator").get<int>(), change.at("denominator").get<int>()}});
		project.signatures = time::SignatureMap(signatures);
		// Files from before fades have none of them.
		project.autoFades = autoFadesFromJson(json.value("autoFades", Json::object()));

		for (const Json& bus : json.at("busses"))
			project.busses.push_back({bus.at("id").get<ObjectId>(), bus.at("name").get<std::string>(),
				bus.at("direction") == "input" ? BusDirection::Input : BusDirection::Output,
				bus.at("channels").get<int>()});
		// Tracks come from top to bottom, folders before what they contain.
		for (const Json& item : json.at("tracks"))
		{
			Track track;
			track.id = item.at("id").get<ObjectId>();
			const auto type = trackTypeFromName(item.at("type").get<std::string>());
			if (!type)
				throw std::invalid_argument("unknown track type " + item.at("type").dump());
			track.type = *type;
			track.name = item.at("name").get<std::string>();
			track.color = colorFrom(item.at("color"));
			track.timeBase = timeBaseFromName(item.at("timeBase").get<std::string>());
			track.parent = item.at("parent").get<ObjectId>();
			track.channel = item.at("channel").get<ObjectId>();
			if (const Json autoFades = item.value("autoFades", Json()); !autoFades.is_null())
				track.autoFades = autoFadesFromJson(autoFades);
			(track.parent == 0 ? project.tracks : project.trackById.at(track.parent).children).push_back(track.id);
			project.trackById[track.id] = track;
		}
		for (const Json& item : json.at("channels"))
		{
			const auto id = item.at("id").get<ObjectId>();
			const std::string name = "channel " + std::to_string(id);
			// Files from before instruments and channel volume have none.
			Channel channel{id, item.at("track").get<ObjectId>(), item.at("channels").get<int>(),
				item.at("input").get<ObjectId>(), item.at("output").get<ObjectId>(),
				item.value("instrument", std::string()),
				checkedNumber(
					item.value("volume", Json(0.0)), g_minChannelVolume, g_maxChannelVolume, name + " volume"),
				checkedNumber(item.value("pan", Json(0.0)), -1.0, 1.0, name + " pan")};
			// An instrument this build doesn't have, e.g. in a file from a newer one, or one on a track that doesn't
			// play instruments: the channel plays none.
			const auto track = project.trackById.find(channel.track);
			if (!processors::isInstrument(channel.instrument) || track == project.trackById.end() ||
				track->second.type != TrackType::Instrument)
				channel.instrument.clear();
			project.channels[id] = std::move(channel);
		}
		for (const Json& item : json.at("media"))
		{
			const auto id = item.at("id").get<ObjectId>();
			project.media[id] = {id, item.at("path").get<std::string>(), item.at("sampleRate").get<double>(),
				item.at("channels").get<int>(), item.at("length").get<time::Samples>()};
		}
		for (const Json& item : json.at("clips"))
		{
			const auto id = item.at("id").get<ObjectId>();
			project.clips[id] = {id, item.at("name").get<std::string>(), item.at("media").get<ObjectId>(),
				fadeFromJson(item.value("fadeIn", Json::object())),
				fadeFromJson(item.value("fadeOut", Json::object()))};
		}
		const auto base = [&project](ObjectId track) { return project.trackById.at(track).timeBase; };
		for (const Json& item : json.at("audioEvents"))
		{
			AudioEvent event;
			event.id = item.at("id").get<ObjectId>();
			event.track = item.at("track").get<ObjectId>();
			event.position = {base(event.track), item.at("position").get<int64_t>()};
			event.clip = item.at("clip").get<ObjectId>();
			event.offset = item.at("offset").get<time::Samples>();
			event.length = item.at("length").get<time::Samples>();
			event.volume = item.value("volume", 0.0); // files from before event volume and mute
			event.mute = item.value("mute", false);
			event.fadeIn = fadeFromJson(item.value("fadeIn", Json::object()));
			event.fadeOut = fadeFromJson(item.value("fadeOut", Json::object()));
			event.crossfade = crossfadeFrom(item.value("crossfade", Json()));
			project.audioEvents[event.id] = event;
		}
		for (const Json& item : json.at("midiParts"))
		{
			MidiPart part;
			part.id = item.at("id").get<ObjectId>();
			part.track = item.at("track").get<ObjectId>();
			part.name = item.at("name").get<std::string>();
			part.position = {base(part.track), item.at("position").get<int64_t>()};
			part.length = item.at("length").get<int64_t>();
			for (const Json& n : item.at("notes"))
			{
				Note note;
				note.id = n.at("id").get<ObjectId>();
				note.position = n.at("position").get<time::Ticks>();
				note.length = n.at("length").get<time::Ticks>();
				note.pitch = n.at("pitch").get<uint8_t>();
				note.velocity = velocityFrom(n.at("velocity"));
				note.channel = n.at("channel").get<uint8_t>();
				if (n.contains("releaseVelocity")) // projects saved before #137 don't have it
					note.releaseVelocity =
						static_cast<uint16_t>(std::lround(n.at("releaseVelocity").get<double>() * g_maxVelocity));
				if (n.contains("origin"))
					note.origin = QuantizeOrigin{
						timingFrom(n.at("origin").at("original")), timingFrom(n.at("origin").at("quantized"))};
				part.notes[note.id] = note;
			}
			for (const Json& c : item.at("controllers"))
			{
				Controller controller = controllerFromJson(c);
				controller.id = c.at("id").get<ObjectId>();
				controller.position = c.at("position").get<time::Ticks>();
				part.controllers[controller.id] = std::move(controller);
			}
			project.midiParts[part.id] = std::move(part);
		}
		// Files from before marker tracks have markers without a track, type and number: position markers in ticks.
		for (const Json& item : json.at("markers"))
		{
			Marker marker;
			marker.id = item.at("id").get<ObjectId>();
			marker.track = item.value("track", ObjectId{0});
			const std::string typeName = item.value("type", std::string("position"));
			const auto type = markerTypeFromName(typeName);
			if (!type)
				throw std::invalid_argument("unknown marker type " + typeName);
			marker.type = *type;
			marker.number = item.value("number", 0); // 0: numbered below
			const time::TimeBase timeBase = marker.track == 0 ? time::TimeBase::Musical : base(marker.track);
			marker.position = {timeBase, item.at("position").get<int64_t>()};
			marker.end = marker.type == MarkerType::Cycle ? time::Position{timeBase, item.at("end").get<int64_t>()}
														  : marker.position;
			marker.name = item.at("name").get<std::string>();
			marker.color = colorFrom(item.at("color"));
			project.markers[marker.id] = marker;
		}
		project.activeMarkerTrack = json.value("activeMarkerTrack", ObjectId{0});
		for (const Json& item : json.value("automation", Json::array()))
		{
			AutomationLane lane = automationLaneFromJson(project, item);
			const ObjectId id = lane.id;
			project.automation[id] = std::move(lane);
		}
		// Files from before quantizing have neither.
		if (json.contains("quantize"))
			project.quantize = midi::quantizeSettingsFromJson(json.at("quantize"));
		project.autoQuantize = json.value("autoQuantize", false);

		// Without a counter, ids continue after the highest one.
		ObjectId highest = 0;
		const auto see = [&highest](ObjectId id) { highest = std::max(highest, id); };
		for (const Bus& bus : project.busses)
			see(bus.id);
		for (const auto& [id, track] : project.trackById)
			see(id);
		for (const auto& [id, channel] : project.channels)
			see(id);
		for (const auto& [id, media] : project.media)
			see(id);
		for (const auto& [id, clip] : project.clips)
			see(id);
		for (const auto& [id, event] : project.audioEvents)
			see(id);
		for (const auto& [id, part] : project.midiParts)
		{
			see(id);
			for (const auto& [noteId, note] : part.notes)
				see(noteId);
			for (const auto& [controllerId, controller] : part.controllers)
				see(controllerId);
		}
		for (const auto& [id, marker] : project.markers)
			see(id);
		for (const auto& [id, lane] : project.automation)
		{
			see(id);
			for (const auto& [pointId, point] : lane.points)
				see(pointId);
		}
		project.nextId = std::max(json.value("nextId", ObjectId{0}), highest + 1);
		placeOldMarkers(project);
		return project;
	}
}
