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
			{TrackType::Marker, "marker"},
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
			{"channel", track.channel}};
	}

	Json toJson(const Channel& channel)
	{
		return {{"id", channel.id}, {"track", channel.track}, {"channels", channel.channels}, {"input", channel.input},
			{"output", channel.output}};
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
		return {{"id", controller.id}, {"position", controller.position}, {"number", controller.number},
			{"value", controller.value / g_maxControllerValue}, {"channel", controller.channel}};
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
			{"activeMarkerTrack", project.activeMarkerTrack}};
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
			project.channels[id] = {id, item.at("track").get<ObjectId>(), item.at("channels").get<int>(),
				item.at("input").get<ObjectId>(), item.at("output").get<ObjectId>()};
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
				Controller controller;
				controller.id = c.at("id").get<ObjectId>();
				controller.position = c.at("position").get<time::Ticks>();
				controller.number = c.at("number").get<uint8_t>();
				controller.value =
					static_cast<uint32_t>(std::llround(c.at("value").get<double>() * g_maxControllerValue));
				controller.channel = c.at("channel").get<uint8_t>();
				part.controllers[controller.id] = controller;
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
		project.nextId = std::max(json.value("nextId", ObjectId{0}), highest + 1);
		placeOldMarkers(project);
		return project;
	}
}
