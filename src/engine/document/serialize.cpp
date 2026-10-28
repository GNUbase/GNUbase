// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/serialize.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
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

		Json colorJson(const std::string& color)
		{
			return color.empty() ? Json(nullptr) : Json(color);
		}

		std::string colorFrom(const Json& json)
		{
			return json.is_null() ? std::string() : json.get<std::string>();
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

	Json toJson(const Bus& bus)
	{
		return {{"id", bus.id}, {"name", bus.name}, {"direction", directionName(bus.direction)},
			{"channels", bus.channels}};
	}

	Json toJson(const Track& track)
	{
		return {{"id", track.id}, {"type", trackTypeName(track.type)}, {"name", track.name},
			{"color", colorJson(track.color)}, {"timeBase", timeBaseName(track.timeBase)}, {"parent", track.parent},
			{"channel", track.channel}};
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
		return {{"id", clip.id}, {"name", clip.name}, {"media", clip.media}};
	}

	Json toJson(const AudioEvent& event)
	{
		return {{"id", event.id}, {"track", event.track}, {"clip", event.clip}, {"position", event.position.value},
			{"offset", event.offset}, {"length", event.length}, {"volume", event.volume}, {"mute", event.mute}};
	}

	Json toJson(const Note& note)
	{
		return {{"id", note.id}, {"position", note.position}, {"length", note.length}, {"pitch", note.pitch},
			{"velocity", note.velocity / g_maxVelocity}, {"channel", note.channel}};
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
		controller.number = json.value("number", uint8_t{0});
		controller.channel = json.value("channel", uint8_t{0});
		if (controller.type == ControllerType::Program)
			controller.value = json.value("bank", g_noBank);
		else if (json.contains("value"))
			controller.value =
				static_cast<uint32_t>(std::llround(json.at("value").get<double>() * g_maxControllerValue));
		if (controller.type == ControllerType::SysEx)
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
		return {{"id", marker.id}, {"position", marker.position}, {"name", marker.name},
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

	AutomationTarget automationTargetFromJson(const Json& json)
	{
		AutomationTarget target;
		target.type = automationTargetTypeFromName(json.at("type").get<std::string>()).value_or(target.type);
		target.parameter = json.value("parameter", uint32_t{0});
		target.number = json.value("number", uint8_t{0});
		target.channel = json.value("channel", uint8_t{0});
		return target;
	}

	Json toJson(const AutomationPoint& point)
	{
		Json json = {{"id", point.id}, {"position", point.position}, {"value", point.value},
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
			[](const AutomationPoint* a, const AutomationPoint* b) { return a->position < b->position; });
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
			{"signatures", signatures}, {"busses", busses}, {"tracks", tracks},
			{"channels", toJsonArray(project.channels)}, {"media", toJsonArray(project.media)},
			{"clips", toJsonArray(project.clips)}, {"audioEvents", toJsonArray(project.audioEvents)},
			{"midiParts", toJsonArray(project.midiParts)}, {"markers", toJsonArray(project.markers)},
			{"automation", toJsonArray(project.automation)}};
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
			(track.parent == 0 ? project.tracks : project.trackById.at(track.parent).children).push_back(track.id);
			project.trackById[track.id] = track;
		}
		for (const Json& item : json.at("channels"))
		{
			const auto id = item.at("id").get<ObjectId>();
			// Files from before instruments and channel volume have none.
			project.channels[id] = {id, item.at("track").get<ObjectId>(), item.at("channels").get<int>(),
				item.at("input").get<ObjectId>(), item.at("output").get<ObjectId>(),
				item.value("instrument", std::string()), item.value("volume", 0.0), item.value("pan", 0.0)};
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
			project.clips[id] = {id, item.at("name").get<std::string>(), item.at("media").get<ObjectId>()};
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
				note.velocity = static_cast<uint16_t>(std::lround(n.at("velocity").get<double>() * g_maxVelocity));
				note.channel = n.at("channel").get<uint8_t>();
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
		for (const Json& item : json.at("markers"))
		{
			const auto id = item.at("id").get<ObjectId>();
			project.markers[id] = {id, item.at("position").get<time::Ticks>(), item.at("name").get<std::string>(),
				colorFrom(item.at("color"))};
		}
		for (const Json& item : json.value("automation", Json::array()))
		{
			AutomationLane lane;
			lane.id = item.at("id").get<ObjectId>();
			lane.track = item.at("track").get<ObjectId>();
			lane.target = automationTargetFromJson(item.at("target"));
			lane.read = item.at("read").get<bool>();
			for (const Json& p : item.at("points"))
			{
				AutomationPoint point;
				point.id = p.at("id").get<ObjectId>();
				point.position = p.at("position").get<time::Ticks>();
				point.value = p.at("value").get<double>();
				point.shape = automationShapeFromName(p.at("shape").get<std::string>()).value_or(point.shape);
				point.curve = p.value("curve", 0.0);
				lane.points[point.id] = point;
			}
			project.automation[lane.id] = std::move(lane);
		}
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
		return project;
	}
}
