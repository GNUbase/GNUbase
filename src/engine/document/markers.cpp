// SPDX-License-Identifier: GPL-3.0-or-later
// Markers (#91): position and cycle markers on marker tracks, and locating with them. marker.add, marker.put,
// marker.set, marker.remove, marker.renumber and marker.activateTrack change the project, so each is one undo step and
// changes nothing if it fails (ProjectModel::add). marker.list and marker.range are queries. marker.locate,
// marker.locateNext, marker.locatePrevious and marker.setLocators move the transport through transport.locate and
// transport.set: what key commands for markers call. marker.export and marker.import write and read marker tracks as
// CSV files (#171).
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
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
		// With the user setting transport.cycleFollowsMarkers, locating to a marker sets the locators to it: a cycle
		// marker's range, or from a position marker to the next marker after it on its track (none: they stay).
		void followMarker(const Project& project, CommandContext& context, const Marker& marker)
		{
			if (!context.call("settings.get", {{"key", "transport.cycleFollowsMarkers"}}).at("value").get<bool>())
				return;
			const time::Samples left = samples(project, marker.position);
			std::optional<time::Samples> right;
			if (marker.type == MarkerType::Cycle)
				right = samples(project, marker.end);
			else
				for (const Point& point : points(project, marker.track))
					if (point.position > left)
					{
						right = point.position;
						break;
					}
			if (right)
				context.call("transport.set", {{"left", left}, {"right", *right}});
		}

		// Marker CSV files (#171): a header row, then a row per marker, the tracks in track order and each track's
		// markers in timeline order. Track Number numbers the tracks in the file, so that tracks with the same name
		// stay apart. Positions are in beats (quarter notes) on musical tracks and in seconds on linear ones, written
		// so that they read back exactly.
		constexpr std::array<const char*, 9> g_csvColumns = {
			"Track Number", "Track", "Time Base", "Type", "ID", "Position", "End", "Description", "Color"};

		// A field, quoted if it holds a comma, a quote or a line end (RFC 4180).
		std::string csvField(const std::string& text)
		{
			if (text.find_first_of(",\"\r\n") == std::string::npos)
				return text;
			std::string quoted = "\"";
			for (const char c : text)
			{
				if (c == '"')
					quoted += '"';
				quoted += c;
			}
			return quoted + '"';
		}

		std::string csvRow(const std::vector<std::string>& fields)
		{
			std::string row;
			for (size_t i = 0; i < fields.size(); ++i)
				row += (i == 0 ? "" : ",") + csvField(fields[i]);
			return row + "\r\n";
		}

		// Ticks per beat or samples per second.
		double timeScale(const Project& project, time::TimeBase base)
		{
			return base == time::TimeBase::Musical ? static_cast<double>(time::g_ticksPerQuarter) : project.sampleRate;
		}

		// The shortest text that reads back as the same number.
		std::string csvTime(const Project& project, const time::Position& position)
		{
			return Json(static_cast<double>(position.value) / timeScale(project, position.base)).dump();
		}

		Json exportMarkers(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			std::vector<ObjectId> chosen;
			if (parameters.contains("tracks"))
				for (const Json& id : parameters.at("tracks"))
					chosen.push_back(markerTrack(project, id.get<ObjectId>()).id);
			std::vector<ObjectId> tracks;
			for (const ObjectId id : project.tracks)
				if (project.trackById.at(id).type == TrackType::Marker &&
					(chosen.empty() || std::find(chosen.begin(), chosen.end(), id) != chosen.end()))
					tracks.push_back(id);
			require(!tracks.empty(), "the project has no marker track");

			std::string text = csvRow({g_csvColumns.begin(), g_csvColumns.end()});
			size_t count = 0;
			for (size_t index = 0; index < tracks.size(); ++index)
			{
				const Track& track = project.trackById.at(tracks[index]);
				for (const Marker* marker : markersOf(project, track.id))
				{
					const bool cycle = marker->type == MarkerType::Cycle;
					text += csvRow({std::to_string(index + 1), track.name,
						track.timeBase == time::TimeBase::Musical ? "Musical" : "Linear", cycle ? "Cycle" : "Position",
						std::to_string(marker->number), csvTime(project, marker->position),
						cycle ? csvTime(project, marker->end) : "", marker->name, marker->color});
					++count;
				}
			}
			const std::string path = parameters.at("path").get<std::string>();
			std::ofstream out(std::filesystem::u8path(path), std::ios::binary | std::ios::trunc);
			out << text;
			out.close();
			require(static_cast<bool>(out), "can't write " + path);
			return {{"markers", count}};
		}

		// A CSV file's records (RFC 4180), the header first; also with LF line ends and a byte order mark.
		std::vector<std::vector<std::string>> csvRecords(const std::string& text)
		{
			std::vector<std::vector<std::string>> records;
			std::vector<std::string> record;
			std::string field;
			bool quoted = false;
			bool started = false; // whether the current record has a character
			const auto endRecord = [&records, &record, &field, &started]()
			{
				record.push_back(std::exchange(field, {}));
				records.push_back(std::exchange(record, {}));
				started = false;
			};
			const std::string byteOrderMark = "\xEF\xBB\xBF";
			for (size_t i = text.rfind(byteOrderMark, 0) == 0 ? byteOrderMark.size() : 0; i < text.size(); ++i)
			{
				const char c = text[i];
				if (quoted)
				{
					if (c != '"')
						field += c;
					else if (i + 1 < text.size() && text[i + 1] == '"')
						field += text[++i];
					else
						quoted = false;
					continue;
				}
				if (c == '\r' || c == '\n')
				{
					if (c == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
						++i;
					endRecord();
					continue;
				}
				started = true;
				if (c == '"')
					quoted = true;
				else if (c == ',')
					record.push_back(std::exchange(field, {}));
				else
					field += c;
			}
			require(!quoted, "the quote in row " + std::to_string(records.size() + 1) + " is not closed");
			if (started)
				endRecord();
			return records;
		}

		std::string trimmed(const std::string& text)
		{
			const auto space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
			const auto first = std::find_if_not(text.begin(), text.end(), space);
			const auto last = std::find_if_not(text.rbegin(), text.rend(), space).base();
			return first < last ? std::string(first, last) : std::string();
		}

		std::string lowered(std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(),
				[](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
			return text;
		}

		// A time in beats or seconds as ticks or samples.
		int64_t fromCsvTime(const Project& project, time::TimeBase base, const std::string& text, const char* column)
		{
			const Json value = Json::parse(text, nullptr, false);
			require(value.is_number(), std::string(column) + " must be a number");
			const double scaled = value.get<double>() * timeScale(project, base);
			constexpr double latest = 4.0e18; // well within 64 bits
			require(scaled >= 0.0 && scaled < latest, std::string(column) + " must be a time from 0 on");
			return std::llround(scaled);
		}

		// A marker track as a CSV file has it: its markers as marker.add's parameters, with their row numbers.
		struct CsvTrack
		{
			std::string key; // the Track Number if the row has one, otherwise the name
			std::string name;
			time::TimeBase base = time::TimeBase::Musical;
			std::vector<std::pair<size_t, Json>> markers;
		};

		// Adds the marker in a row to its track, with the columns by their header.
		void readCsvRow(const Project& project, const std::map<std::string, size_t>& columns,
			const std::vector<std::string>& record, size_t row, std::vector<CsvTrack>& tracks)
		{
			const auto field = [&columns, &record](const char* column)
			{
				const auto found = columns.find(column);
				return found != columns.end() && found->second < record.size() ? record[found->second] : std::string();
			};
			const std::string name = trimmed(field("track"));
			const std::string baseName = lowered(trimmed(field("time base")));
			require(baseName.empty() || baseName == "musical" || baseName == "linear",
				"Time Base must be Musical or Linear");
			const time::TimeBase base = timeBaseFromName(baseName);
			std::string key = "name:" + name;
			if (const std::string number = trimmed(field("track number")); !number.empty())
			{
				const Json value = Json::parse(number, nullptr, false);
				require(value.is_number_integer(), "Track Number must be a whole number");
				key = "number:" + value.dump();
			}
			auto track =
				std::find_if(tracks.begin(), tracks.end(), [&key](const CsvTrack& other) { return other.key == key; });
			if (track == tracks.end())
				track = tracks.insert(tracks.end(), CsvTrack{key, name, base, {}});
			require(track->base == base, "the markers of a track must have one time base");

			const std::string type = lowered(trimmed(field("type")));
			require(type.empty() || type == "position" || type == "cycle", "Type must be Position or Cycle");
			Json marker = {{"type", type == "cycle" ? "cycle" : "position"},
				{"position", fromCsvTime(project, base, trimmed(field("position")), "Position")},
				{"name", field("description")}};
			if (type == "cycle")
			{
				const std::string end = trimmed(field("end"));
				require(!end.empty(), "a cycle marker needs an End");
				marker["end"] = fromCsvTime(project, base, end, "End");
			}
			if (const std::string number = trimmed(field("id")); !number.empty())
			{
				const Json value = Json::parse(number, nullptr, false);
				require(value.is_number_integer(), "ID must be a whole number");
				marker["number"] = value;
			}
			if (const std::string color = trimmed(field("color")); !color.empty())
				marker["color"] = color;
			track->markers.emplace_back(row, std::move(marker));
		}

		std::vector<CsvTrack> readCsvTracks(const Project& project, const std::string& path)
		{
			std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
			require(static_cast<bool>(in), "can't open " + path);
			const std::vector<std::vector<std::string>> records =
				csvRecords({std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()});

			// Columns by their header, in any order; others are left out.
			std::map<std::string, size_t> columns;
			if (!records.empty())
				for (size_t i = 0; i < records[0].size(); ++i)
					columns.emplace(lowered(trimmed(records[0][i])), i);
			require(columns.count("position") != 0, path + " has no header row with a Position column");

			std::vector<CsvTrack> tracks;
			for (size_t row = 1; row < records.size(); ++row)
			{
				const std::vector<std::string>& record = records[row];
				if (std::all_of(record.begin(), record.end(), [](const std::string& text) { return text.empty(); }))
					continue;
				try
				{
					readCsvRow(project, columns, record, row + 1, tracks);
				}
				catch (const CommandError& error)
				{
					throw CommandError("row " + std::to_string(row + 1) + ": " + error.what());
				}
			}
			return tracks;
		}

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
			followMarker(project, context, *target->marker);
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
				"without a marker track one is added as track.add adds it, in the time base the user setting "
				"editing.defaultTimeBase gives new tracks). position and end are in the track's time base (ticks or "
				"samples; project.get reports each track's timeBase) or \"cursor\": to give numbers without knowing "
				"the setting, add the marker track first with track.add timeBase and pass it as track. A position "
				"marker defaults to the cursor, a cycle marker to the locators; "
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
			{"marker.put", 1,
				"Puts position marker number N of a marker track (default: the active one; without a marker track one "
				"is added) at the cursor: moves it there, or adds it with that number. What the key commands Set "
				"Marker 1 to 9 do.",
				objectSchema({{"number", integerSchema}, {"track", idSchema}}, {"number"}), markerResult, false},
			[this](Project& project, CommandContext& context)
			{
				ObjectId track = trackFor(project, context.parameters());
				if (track == 0)
				{
					const Json parameters = {{"type", "marker"}};
					CommandContext inner(parameters, &context);
					track = m_edits.at("track.add").handler(project, inner).at("track").at("id").get<ObjectId>();
				}
				const auto number = context.parameters().at("number").get<int64_t>();
				const auto found = std::find_if(project.markers.begin(), project.markers.end(),
					[track, number](const auto& entry)
					{
						const Marker& marker = entry.second;
						return marker.track == track && marker.type == MarkerType::Position && marker.number == number;
					});
				if (found != project.markers.end())
				{
					const Json parameters = {{"marker", found->first}, {"position", "cursor"}};
					CommandContext inner(parameters, &context);
					return setMarker(project, inner);
				}
				const Json parameters = {{"number", number}};
				CommandContext inner(parameters, &context);
				return addMarker(project, inner, track);
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
			{"marker.export", 1,
				"Exports marker tracks (tracks, default all of them) into a CSV file at path, which marker.import "
				"reads: a header row with the columns Track Number, Track, Time Base, Type, ID, Position, End, "
				"Description and "
				"Color, then a row per marker, the tracks in track order and their markers in timeline order. "
				"Track Number numbers the tracks in the file, 1 on. Positions and ends are in beats (quarter notes) on "
				"musical tracks and in seconds on linear ones. "
				"Returns the number of markers.",
				objectSchema({{"path", stringSchema}, {"tracks", {{"type", "array"}, {"items", idSchema}}}}, {"path"}),
				{{"type", "object"}, {"properties", {{"markers", integerSchema}}}, {"required", {"markers"}}}, true},
			exportMarkers);
		add(engine,
			{"marker.import", 1,
				"Imports a CSV file of markers, such as marker.export writes: each track in it becomes a new marker "
				"track at the top, in file order, with its time base and its markers' types, ids, positions, ends, "
				"descriptions and colors. Columns are found by their header, in any order; only Position is "
				"required. Rows with the same Track Number are one track, rows without one those with the same Track "
				"name. Without either all markers go on one track, without Time Base it is musical, without Type "
				"they are position markers and without an ID they get the first free number. The active marker "
				"track stays active, or the first new one becomes it. One undo step; a file with an invalid row "
				"changes nothing. Returns the new tracks and the number of markers.",
				objectSchema({{"path", stringSchema}}, {"path"}),
				{{"type", "object"},
					{"properties", {{"tracks", {{"type", "array"}, {"items", idSchema}}}, {"markers", integerSchema}}},
					{"required", {"tracks", "markers"}}},
				false},
			[this](Project& project, CommandContext& context)
			{
				const std::vector<CsvTrack> tracks =
					readCsvTracks(project, context.parameters().at("path").get<std::string>());
				const ObjectId active = project.activeMarkerTrack;
				std::vector<ObjectId> ids;
				size_t count = 0;
				for (const CsvTrack& track : tracks)
				{
					Json parameters = {
						{"type", "marker"}, {"index", ids.size()}, {"timeBase", timeBaseName(track.base)}};
					if (!track.name.empty())
						parameters["name"] = track.name;
					CommandContext inner(parameters, &context);
					ids.push_back(m_edits.at("track.add").handler(project, inner).at("track").at("id").get<ObjectId>());
					for (const auto& [row, marker] : track.markers)
					{
						Json markerParameters = marker;
						markerParameters["track"] = ids.back();
						CommandContext markerContext(markerParameters, &context);
						try
						{
							addMarker(project, markerContext, ids.back());
						}
						catch (const CommandError& error)
						{
							throw CommandError("row " + std::to_string(row) + ": " + error.what());
						}
						++count;
					}
				}
				const ObjectId keep = (active != 0 || ids.empty()) ? active : ids.front();
				if (keep != 0)
				{
					const Json parameters = {{"track", keep}};
					CommandContext inner(parameters, &context);
					activateTrack(project, inner);
				}
				return Json{{"tracks", ids}, {"markers", count}};
			});

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
				"null. With the user setting transport.cycleFollowsMarkers it also sets the locators to the marker, "
				"as marker.locate does. Returns the marker and the cursor in samples.",
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
				"(default: the active one) and type (default: position). Cycle markers locate to their start. With "
				"the user setting transport.cycleFollowsMarkers it also sets the locators to the marker: a cycle "
				"marker's range, or from a position marker to the next marker after it on its track, if there is one.",
				objectSchema(
					{{"marker", idSchema}, {"number", integerSchema}, {"type", typeSchema}, {"track", idSchema}}),
				locateResult, false},
			[this](CommandContext& context)
			{
				const Marker& marker = namedMarker(m_project, context.parameters(), markerType(context.parameters()));
				const time::Samples position = samples(m_project, marker.position);
				context.call("transport.locate", {{"position", position}});
				followMarker(m_project, context, marker);
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
