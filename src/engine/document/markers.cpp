// SPDX-License-Identifier: GPL-3.0-or-later
// Markers (#91): position and cycle markers on marker tracks, and locating with them. marker.add, marker.set,
// marker.remove, marker.renumber and marker.activateTrack change the project, so each is one undo step and changes
// nothing if it fails (ProjectModel::add). marker.list and marker.range are queries. marker.locate,
// marker.locateNext, marker.locatePrevious and marker.setLocators move the transport through transport.locate and
// transport.set: what key commands for markers call.
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		std::string idText(ObjectId id)
		{
			return std::to_string(id);
		}

		Json objectSchema(Json properties, const std::vector<std::string>& required = {})
		{
			return {{"type", "object"}, {"properties", std::move(properties)}, {"required", required},
				{"additionalProperties", false}};
		}

		std::string color(const Json& parameters, const std::string& fallback)
		{
			if (!parameters.contains("color"))
				return fallback;
			if (parameters.at("color").is_null())
				return {};
			const std::string value = parameters.at("color").get<std::string>();
			const bool valid = value.size() == 7 && value[0] == '#' &&
				std::all_of(value.begin() + 1, value.end(),
					[](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
			require(valid, "color must be #rrggbb");
			return value;
		}

		const Track& markerTrack(const Project& project, ObjectId id)
		{
			const auto found = project.trackById.find(id);
			require(found != project.trackById.end(), "no track " + idText(id));
			require(found->second.type == TrackType::Marker, "track " + idText(id) + " is not a marker track");
			return found->second;
		}

		// The marker track a command works on: the given one, or the active one; 0 if there is none.
		ObjectId trackFor(const Project& project, const Json& parameters)
		{
			if (parameters.contains("track"))
				return markerTrack(project, parameters.at("track").get<ObjectId>()).id;
			return project.activeMarkerTrack;
		}

		ObjectId requireTrack(const Project& project, const Json& parameters)
		{
			const ObjectId track = trackFor(project, parameters);
			require(track != 0, "the project has no marker track");
			return track;
		}

		Marker& findMarker(Project& project, ObjectId id)
		{
			const auto found = project.markers.find(id);
			require(found != project.markers.end(), "no marker " + idText(id));
			return found->second;
		}

		MarkerType markerType(const Json& parameters)
		{
			return markerTypeFromName(parameters.value("type", "position")).value_or(MarkerType::Position);
		}

		time::Samples samples(const Project& project, const time::Position& position)
		{
			return time::toSamples(position, project.tempo, project.sampleRate);
		}

		// A sample position in the track's time base.
		time::Position fromSamples(const Project& project, const Track& track, time::Samples position)
		{
			return time::rebase({time::TimeBase::Linear, position}, track.timeBase, project.tempo, project.sampleRate);
		}

		// The transport's state (transport.get), read once per command when it needs the cursor or the locators.
		class TransportState
		{
		public:
			explicit TransportState(CommandContext& context) : m_context(context) {}

			time::Samples get(const char* key)
			{
				if (!m_state)
					m_state = m_context.call("transport.get");
				return m_state->at(key).get<time::Samples>();
			}

		private:
			CommandContext& m_context;
			std::optional<Json> m_state;
		};

		// A position parameter: ticks or samples in the track's time base, or "cursor".
		time::Position position(const Project& project, const Track& track, const Json& value, const std::string& name,
			TransportState& transport)
		{
			if (value.is_string())
			{
				require(value.get<std::string>() == "cursor", name + " must be a position or \"cursor\"");
				return fromSamples(project, track, transport.get("position"));
			}
			const auto position = value.get<int64_t>();
			require(position >= 0, name + " must not be negative");
			return {track.timeBase, position};
		}

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

		int firstFreeNumber(const Project& project, const Marker& marker)
		{
			int number = 1;
			while (numberTaken(project, marker, number))
				++number;
			return number;
		}

		// A given number, which must be free on the marker's track; otherwise fallback.
		int chosenNumber(const Project& project, const Marker& marker, const Json& parameters, int fallback)
		{
			if (!parameters.contains("number"))
				return fallback;
			const auto number = parameters.at("number").get<int64_t>();
			require(number >= 1 && number <= 1000000, "number must be 1 to 1000000");
			require(!numberTaken(project, marker, static_cast<int>(number)),
				std::string(marker.type == MarkerType::Cycle ? "cycle" : "position") + " marker number " +
					std::to_string(number) + " is taken on track " + idText(marker.track));
			return static_cast<int>(number);
		}

		void requireRange(const Project& project, const Marker& marker)
		{
			require(hasMinimumLength(project, marker),
				"a cycle marker's end must be at least one sample after its position");
		}

		// The markers of a track in timeline order.
		std::vector<const Marker*> markersOf(const Project& project, ObjectId track)
		{
			std::vector<const Marker*> markers;
			for (const auto& [id, marker] : project.markers)
				if (marker.track == track)
					markers.push_back(&marker);
			std::sort(markers.begin(), markers.end(),
				[](const Marker* a, const Marker* b)
				{
					return std::tie(a->position.value, a->type, a->number, a->id) <
						std::tie(b->position.value, b->type, b->number, b->id);
				});
			return markers;
		}

		// Where a track's markers are, in samples: position markers and the start and end of cycle markers.
		struct Point
		{
			time::Samples position = 0;
			const Marker* marker = nullptr;
		};

		std::vector<Point> points(const Project& project, ObjectId track)
		{
			std::vector<Point> points;
			for (const Marker* marker : markersOf(project, track))
			{
				points.push_back({samples(project, marker->position), marker});
				if (marker->type == MarkerType::Cycle)
					points.push_back({samples(project, marker->end), marker});
			}
			std::stable_sort(
				points.begin(), points.end(), [](const Point& a, const Point& b) { return a.position < b.position; });
			return points;
		}

		// A marker with its position and end in the project's samples.
		Json listed(const Project& project, const Marker& marker)
		{
			Json json = toJson(marker);
			json["positionSamples"] = samples(project, marker.position);
			json["endSamples"] = marker.type == MarkerType::Cycle ? Json(samples(project, marker.end)) : Json(nullptr);
			return json;
		}

		// The marker a navigation command names: by id with marker, or by its number on the track.
		const Marker& namedMarker(const Project& project, const Json& parameters, MarkerType type)
		{
			require(parameters.contains("marker") != parameters.contains("number"), "give a marker or number");
			if (parameters.contains("marker"))
			{
				const auto id = parameters.at("marker").get<ObjectId>();
				const auto found = project.markers.find(id);
				require(found != project.markers.end(), "no marker " + idText(id));
				return found->second;
			}
			const ObjectId track = requireTrack(project, parameters);
			const auto number = parameters.at("number").get<int64_t>();
			const auto found = std::find_if(project.markers.begin(), project.markers.end(),
				[track, type, number](const auto& entry)
				{
					const Marker& marker = entry.second;
					return marker.track == track && marker.type == type && marker.number == number;
				});
			require(found != project.markers.end(),
				std::string("no ") + markerTypeName(type) + " marker " + std::to_string(number) + " on track " +
					idText(track));
			return found->second;
		}

		Json addMarker(Project& project, CommandContext& context, ObjectId trackId)
		{
			const Json& parameters = context.parameters();
			const Track& track = project.trackById.at(trackId);
			TransportState transport(context);
			Marker marker;
			marker.track = trackId;
			marker.type = markerType(parameters);
			if (marker.type == MarkerType::Position)
			{
				require(!parameters.contains("end"), "only cycle markers have an end");
				marker.position = parameters.contains("position")
					? position(project, track, parameters.at("position"), "position", transport)
					: fromSamples(project, track, transport.get("position"));
				marker.end = marker.position;
			}
			else if (parameters.contains("position") || parameters.contains("end"))
			{
				require(parameters.contains("position") && parameters.contains("end"),
					"give a cycle marker's position and end, or neither for the locators");
				marker.position = position(project, track, parameters.at("position"), "position", transport);
				marker.end = position(project, track, parameters.at("end"), "end", transport);
			}
			else
			{
				const time::Samples left = transport.get("left");
				const time::Samples right = transport.get("right");
				require(right > left, "the locators make no range: the right locator must be after the left one");
				marker.position = fromSamples(project, track, left);
				marker.end = fromSamples(project, track, right);
			}
			requireRange(project, marker);
			marker.number = chosenNumber(project, marker, parameters, firstFreeNumber(project, marker));
			marker.name = parameters.value("name", "");
			marker.color = color(parameters, {});
			marker.id = project.newId();
			project.markers[marker.id] = marker;
			Json result = {{"marker", toJson(marker)}};
			context.emit("marker.added", result);
			return result;
		}

		Json setMarker(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			Marker& stored = findMarker(project, parameters.at("marker").get<ObjectId>());
			Marker marker = stored;
			bool moved = false;
			if (parameters.contains("track"))
			{
				const Track& target = markerTrack(project, parameters.at("track").get<ObjectId>());
				if (target.id != marker.track)
				{
					marker.position = time::rebase(marker.position, target.timeBase, project.tempo, project.sampleRate);
					marker.end = time::rebase(marker.end, target.timeBase, project.tempo, project.sampleRate);
					keepMinimumLength(project, marker);
					marker.track = target.id;
					moved = true;
				}
			}
			const Track& track = project.trackById.at(marker.track);
			TransportState transport(context);
			if (parameters.contains("position"))
			{
				const time::Position start = position(project, track, parameters.at("position"), "position", transport);
				marker.end.value += start.value - marker.position.value; // a cycle marker moves as a whole
				marker.position = start;
				if (!parameters.contains("end"))
					keepMinimumLength(project, marker); // in ticks, its length in samples may round down
			}
			if (parameters.contains("end"))
			{
				require(marker.type == MarkerType::Cycle, "only cycle markers have an end");
				marker.end = position(project, track, parameters.at("end"), "end", transport);
			}
			requireRange(project, marker);
			const bool keepsNumber = !moved || !numberTaken(project, marker, marker.number);
			marker.number = chosenNumber(
				project, marker, parameters, keepsNumber ? marker.number : firstFreeNumber(project, marker));
			marker.name = parameters.value("name", marker.name);
			marker.color = color(parameters, marker.color);
			stored = marker;
			Json result = {{"marker", toJson(marker)}};
			context.emit("marker.changed", result);
			return result;
		}

		Json removeMarkers(Project& project, CommandContext& context)
		{
			const auto ids = context.parameters().at("markers").get<std::vector<ObjectId>>();
			for (const ObjectId id : ids)
				findMarker(project, id);
			for (const ObjectId id : ids)
			{
				project.markers.erase(id);
				context.emit("marker.removed", {{"marker", id}});
			}
			return Json::object();
		}

		Json renumberMarkers(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const ObjectId track = requireTrack(project, parameters);
			const MarkerType type = markerType(parameters);
			std::vector<ObjectId> ids;
			for (const Marker* marker : markersOf(project, track))
				if (marker->type == type)
					ids.push_back(marker->id);
			// Numbers move out of the way first, so no two markers share one in between.
			for (const ObjectId id : ids)
				project.markers.at(id).number = 0;
			int number = 0;
			for (const ObjectId id : ids)
			{
				Marker& marker = project.markers.at(id);
				marker.number = ++number;
				context.emit("marker.changed", {{"marker", toJson(marker)}});
			}
			return Json::object();
		}

		Json activateTrack(Project& project, CommandContext& context)
		{
			const ObjectId track = markerTrack(project, context.parameters().at("track").get<ObjectId>()).id;
			if (project.activeMarkerTrack != track)
			{
				project.activeMarkerTrack = track;
				context.emit("marker.trackActivated", {{"track", track}});
			}
			return Json::object();
		}

		Json listMarkers(const Project& project, const Json& parameters)
		{
			const ObjectId track = trackFor(project, parameters);
			Json markers = Json::array();
			for (const Marker* marker : markersOf(project, track))
				markers.push_back(listed(project, *marker));
			return {{"track", track}, {"markers", markers}};
		}

		// What a range selection between markers spans: from the marker at or before the position to the next one.
		Json markerRange(const Project& project, const Json& parameters)
		{
			const ObjectId track = requireTrack(project, parameters);
			const auto position = parameters.at("position").get<time::Samples>();
			std::optional<time::Samples> start;
			std::optional<time::Samples> end;
			for (const Point& point : points(project, track))
			{
				if (point.position <= position)
					start = point.position;
				else if (!end)
					end = point.position;
			}
			const std::string where = "position " + std::to_string(position) + " on track " + idText(track);
			if (!start)
				throw CommandError("no marker at or before " + where);
			if (!end)
				throw CommandError("no marker after " + where);
			return {{"start", *start}, {"end", *end}};
		}

		// Moves the cursor to the next marker after it, or the previous one before it. Without one it stays.
		Json locateNeighbour(const Project& project, CommandContext& context, bool next)
		{
			const time::Samples cursor = context.call("transport.get").at("position").get<time::Samples>();
			const ObjectId track = trackFor(project, context.parameters());
			std::optional<Point> target;
			for (const Point& point : points(project, track))
			{
				if (next && point.position > cursor)
				{
					target = point;
					break;
				}
				if (!next && point.position < cursor)
					target = point;
			}
			if (!target)
				return {{"marker", nullptr}, {"position", cursor}};
			context.call("transport.locate", {{"position", target->position}});
			return {{"marker", toJson(*target->marker)}, {"position", target->position}};
		}
	}

	void ProjectModel::registerMarkerCommands(Engine& engine)
	{
		const Json idSchema = {{"type", "integer"}};
		const Json integerSchema = {{"type", "integer"}};
		const Json stringSchema = {{"type", "string"}};
		const Json positionSchema = {{"type", {"integer", "string"}}};
		const Json typeSchema = {{"type", "string"}, {"enum", {"position", "cycle"}}};
		const Json colorSchema = {{"type", {"string", "null"}}};
		const Json markerResult = {
			{"type", "object"}, {"properties", {{"marker", {{"type", "object"}}}}}, {"required", {"marker"}}};
		const Json locateResult = {{"type", "object"},
			{"properties", {{"marker", {{"type", {"object", "null"}}}}, {"position", integerSchema}}},
			{"required", {"marker", "position"}}};
		const Json noResult = Json::object();

		add(engine,
			{"marker.add", 1,
				"Adds a position marker, or with type cycle a cycle marker, to a marker track (default: the active "
				"one; "
				"without a marker track one is added). position and end are in the track's time base (ticks or "
				"samples) or \"cursor\". A position marker defaults to the cursor, a cycle marker to the locators; "
				"a cycle marker ends at least one sample after its position. "
				"Position and cycle markers are numbered separately on each track; a new marker gets the first free "
				"number unless one is given.",
				objectSchema(
					{{"type", typeSchema}, {"track", idSchema}, {"position", positionSchema}, {"end", positionSchema},
						{"name", stringSchema}, {"color", colorSchema}, {"number", integerSchema}}),
				markerResult, false},
			[this](Project& project, CommandContext& context)
			{
				ObjectId track = trackFor(project, context.parameters());
				if (track == 0)
				{
					const Json parameters = {{"type", "marker"}};
					CommandContext inner(parameters, &context);
					track = m_edits.at("track.add").handler(project, inner).at("track").at("id").get<ObjectId>();
				}
				return addMarker(project, context, track);
			});
		add(engine,
			{"marker.set", 1,
				"Changes a marker: moves it (position, in the track's time base or \"cursor\"; a cycle marker keeps "
				"its length unless end is given), renames it, changes its color or number, or moves it to another "
				"marker track, where it takes the first free number if its own is taken.",
				objectSchema(
					{{"marker", idSchema}, {"track", idSchema}, {"position", positionSchema}, {"end", positionSchema},
						{"name", stringSchema}, {"color", colorSchema}, {"number", integerSchema}},
					{"marker"}),
				markerResult, false},
			setMarker);
		add(engine,
			{"marker.remove", 1, "Removes markers.",
				objectSchema({{"markers", {{"type", "array"}, {"items", idSchema}}}}, {"markers"}), noResult, false},
			removeMarkers);
		add(engine,
			{"marker.renumber", 1,
				"Numbers a marker track's position or cycle markers again from 1, in timeline order (default: the "
				"active track).",
				objectSchema({{"type", typeSchema}, {"track", idSchema}}, {"type"}), noResult, false},
			renumberMarkers);
		add(engine,
			{"marker.activateTrack", 1,
				"Makes a marker track the active one, which marker commands use when they are given no track.",
				objectSchema({{"track", idSchema}}, {"track"}), noResult, false},
			activateTrack);

		add(engine,
			{"marker.list", 1,
				"The markers of a marker track (default: the active one; track 0 if there is none) in timeline order, "
				"with their position and end also in the project's samples.",
				objectSchema({{"track", idSchema}}),
				{{"type", "object"}, {"properties", {{"track", integerSchema}, {"markers", {{"type", "array"}}}}},
					{"required", {"track", "markers"}}},
				true},
			[](Project& project, CommandContext& context) { return listMarkers(project, context.parameters()); });
		add(engine,
			{"marker.range", 1,
				"The range between the markers around a position in samples, on a marker track (default: the active "
				"one): from the last marker at or before it to the next one, counting position markers and the "
				"start and end of cycle markers. A range selection between markers spans it.",
				objectSchema({{"position", integerSchema}, {"track", idSchema}}, {"position"}),
				{{"type", "object"}, {"properties", {{"start", integerSchema}, {"end", integerSchema}}},
					{"required", {"start", "end"}}},
				true},
			[](Project& project, CommandContext& context) { return markerRange(project, context.parameters()); });

		const Json neighbourParameters = objectSchema({{"track", idSchema}});
		addControl(engine,
			{"marker.locateNext", 1,
				"Moves the project cursor to the next marker after it on a marker track (default: the active one): "
				"position markers and the start and end of cycle markers. Without one the cursor stays and marker is "
				"null. Returns the marker and the cursor in samples.",
				neighbourParameters, locateResult, false},
			[this](CommandContext& context) { return locateNeighbour(m_project, context, true); });
		addControl(engine,
			{"marker.locatePrevious", 1,
				"Moves the project cursor to the previous marker before it on a marker track (default: the active "
				"one), like marker.locateNext.",
				neighbourParameters, locateResult, false},
			[this](CommandContext& context) { return locateNeighbour(m_project, context, false); });
		addControl(engine,
			{"marker.locate", 1,
				"Moves the project cursor to a marker, given by id (marker) or by its number on a marker track "
				"(default: the active one) and type (default: position). Cycle markers locate to their start.",
				objectSchema(
					{{"marker", idSchema}, {"number", integerSchema}, {"type", typeSchema}, {"track", idSchema}}),
				locateResult, false},
			[this](CommandContext& context)
			{
				const Marker& marker = namedMarker(m_project, context.parameters(), markerType(context.parameters()));
				const time::Samples position = samples(m_project, marker.position);
				context.call("transport.locate", {{"position", position}});
				return Json{{"marker", toJson(marker)}, {"position", position}};
			});
		addControl(engine,
			{"marker.setLocators", 1,
				"Sets the left and right locators to a cycle marker's start and end, given by id (marker) or by its "
				"number on a marker track (default: the active one).",
				objectSchema({{"marker", idSchema}, {"number", integerSchema}, {"track", idSchema}}),
				{{"type", "object"},
					{"properties",
						{{"marker", {{"type", "object"}}}, {"left", integerSchema}, {"right", integerSchema}}},
					{"required", {"marker", "left", "right"}}},
				false},
			[this](CommandContext& context)
			{
				const Marker& marker = namedMarker(m_project, context.parameters(), MarkerType::Cycle);
				require(marker.type == MarkerType::Cycle, "marker " + idText(marker.id) + " is not a cycle marker");
				const time::Samples left = samples(m_project, marker.position);
				const time::Samples right = samples(m_project, marker.end);
				context.call("transport.set", {{"left", left}, {"right", right}});
				return Json{{"marker", toJson(marker)}, {"left", left}, {"right", right}};
			});
	}
}
