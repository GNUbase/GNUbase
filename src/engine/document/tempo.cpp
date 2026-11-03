// SPDX-License-Identifier: GPL-3.0-or-later
// The tempo track (#155, reference manual pp. 162, 1446-1458): tempo.add, tempo.set and tempo.remove edit the tempo
// track's tempo events, in either tempo mode, and tempo.tap applies a tapped tempo. project.set switches between fixed
// tempo and tempo track mode (tempoTrackActive). Each command is one undo step and changes nothing if it fails
// (ProjectModel::add).
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
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

		Json objectSchema(Json properties, const std::vector<std::string>& required = {})
		{
			return {{"type", "object"}, {"properties", std::move(properties)}, {"required", required},
				{"additionalProperties", false}};
		}

		double checkedBpm(const Json& value)
		{
			const auto bpm = value.get<double>();
			require(bpm >= time::g_minBpm && bpm <= time::g_maxBpm, "tempo must be from 1 to 999 BPM");
			return bpm;
		}

		// A position parameter in ticks, or "cursor".
		time::Ticks ticksFrom(const Project& project, const Json& value, CommandContext& context)
		{
			if (value.is_string())
			{
				require(value.get<std::string>() == "cursor", "position must be ticks or \"cursor\"");
				const auto cursor = context.call("transport.get").at("position").get<time::Samples>();
				return project.tempo.toTicks(cursor, project.sampleRate);
			}
			const auto ticks = value.get<time::Ticks>();
			require(ticks >= 0, "position must not be negative");
			return ticks;
		}

		// The index of the tempo event at a position.
		std::ptrdiff_t indexAt(const std::vector<time::TempoPoint>& points, time::Ticks position)
		{
			const auto found = std::find_if(points.begin(), points.end(),
				[position](const time::TempoPoint& point) { return point.position == position; });
			require(found != points.end(), "no tempo event at " + std::to_string(position));
			return found - points.begin();
		}

		// The tempo track as the given points, which must make a tempo map.
		void setTempoTrack(Project& project, std::vector<time::TempoPoint> points, CommandContext& context)
		{
			std::sort(points.begin(), points.end(),
				[](const time::TempoPoint& a, const time::TempoPoint& b) { return a.position < b.position; });
			for (size_t i = 1; i < points.size(); ++i)
				require(points[i].position != points[i - 1].position,
					"two tempo events at " + std::to_string(points[i].position));
			points.front().ramp = false; // nothing to ramp from
			project.tempoTrack() = time::TempoMap(points);
			tempoChanged(project, context);
		}

		// Whether a new tempo event ramps: the user setting editing.tempoType, with auto the type of the event before
		// it.
		bool newRamp(const std::vector<time::TempoPoint>& points, time::Ticks position, CommandContext& context)
		{
			const std::string type =
				context.call("settings.get", {{"key", "editing.tempoType"}}).at("value").get<std::string>();
			if (type != "auto")
				return type == "ramp";
			const auto before = std::find_if(points.rbegin(), points.rend(),
				[position](const time::TempoPoint& point) { return point.position < position; });
			return before != points.rend() && before->ramp;
		}

		// Adds a tempo event, or changes the one at its position.
		void addPoint(
			Project& project, time::Ticks position, double bpm, std::optional<bool> ramp, CommandContext& context)
		{
			std::vector<time::TempoPoint> points = project.tempoTrack().points();
			const bool isRamp = ramp ? *ramp : newRamp(points, position, context);
			const auto found = std::find_if(points.begin(), points.end(),
				[position](const time::TempoPoint& point) { return point.position == position; });
			if (found != points.end())
				*found = {position, bpm, isRamp};
			else
				points.push_back({position, bpm, isRamp});
			setTempoTrack(project, std::move(points), context);
		}

		Json addTempo(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const time::Ticks position = ticksFrom(project, parameters.at("position"), context);
			const double bpm = checkedBpm(parameters.at("bpm"));
			const std::optional<bool> ramp =
				parameters.contains("ramp") ? std::optional<bool>(parameters.at("ramp").get<bool>()) : std::nullopt;
			addPoint(project, position, bpm, ramp, context);
			return {{"position", position}, {"tempo", tempoJson(project)}};
		}

		Json setTempo(Project& project, CommandContext& context)
		{
			std::vector<time::TempoPoint> points = project.tempoTrack().points();
			const std::vector<time::TempoPoint> before = points;
			std::vector<time::Ticks> changed;
			for (const Json& change : context.parameters().at("events"))
			{
				const auto position = change.at("position").get<time::Ticks>();
				require(std::find(changed.begin(), changed.end(), position) == changed.end(),
					"tempo event " + std::to_string(position) + " is changed twice");
				changed.push_back(position);
				// Positions name the events as they were; the moves all happen at once.
				time::TempoPoint& point = points.at(static_cast<size_t>(indexAt(before, position)));
				if (change.contains("to"))
				{
					const auto to = change.at("to").get<time::Ticks>();
					require(position != 0 || to == 0, "the first tempo event stays at the project start");
					require(to > 0 || position == 0, "only the first tempo event is at the project start");
					point.position = to;
				}
				if (change.contains("bpm"))
					point.bpm = checkedBpm(change.at("bpm"));
				if (change.contains("ramp"))
					point.ramp = change.at("ramp").get<bool>();
			}
			setTempoTrack(project, std::move(points), context);
			return {{"tempo", tempoJson(project)}};
		}

		Json removeTempo(Project& project, CommandContext& context)
		{
			std::vector<time::TempoPoint> points = project.tempoTrack().points();
			for (const Json& item : context.parameters().at("positions"))
			{
				const auto position = item.get<time::Ticks>();
				require(position != 0, "the first tempo event can't be removed");
				points.erase(points.begin() + indexAt(points, position));
			}
			setTempoTrack(project, std::move(points), context);
			return {{"tempo", tempoJson(project)}};
		}

		// Where a tapped tempo goes (reference manual p. 1458): a new tempo event at a position in ticks, the project
		// tempo (nullopt), or nowhere (display).
		struct TapTarget
		{
			bool apply = true;
			std::optional<time::Ticks> event;
		};

		TapTarget tapTarget(
			const Project& project, const std::string& mode, const Json& parameters, CommandContext& context)
		{
			if (mode == "display")
				return {false, std::nullopt};
			if (mode == "project" || (mode == "auto" && !project.tempoTrackActive))
				return {true, std::nullopt};
			const auto selection = parameters.find("selection");
			if (mode == "selection" || (mode == "auto" && selection != parameters.end()))
			{
				if (selection == parameters.end())
					return {false, std::nullopt}; // nothing selected
				return {true, ticksFrom(project, *selection, context)};
			}
			const Json transport = context.call("transport.get");
			const bool fromStart = mode == "playbackStart" || (mode == "auto" && transport.at("playing").get<bool>());
			const auto at = transport.at(fromStart ? "start" : "position").get<time::Samples>();
			return {true, project.tempo.toTicks(at, project.sampleRate)};
		}

		Json tapTempo(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const double bpm = std::round(checkedBpm(parameters.at("bpm")) * 1000.0) / 1000.0;
			const std::string mode = parameters.contains("mode")
				? parameters.at("mode").get<std::string>()
				: context.call("settings.get", {{"key", "transport.tapTempoMode"}}).at("value").get<std::string>();
			const TapTarget target = tapTarget(project, mode, parameters, context);
			Json result = {{"bpm", bpm}, {"applied", "none"}, {"position", nullptr}};
			if (!target.apply)
				return result;
			if (target.event)
			{
				addPoint(project, *target.event, bpm, std::nullopt, context);
				result["applied"] = "event";
				result["position"] = *target.event;
				return result;
			}
			// The project tempo: the first tempo event in tempo track mode, the fixed tempo otherwise.
			std::vector<time::TempoPoint> points = project.tempo.points();
			points.front().bpm = bpm;
			project.tempo = time::TempoMap(points);
			tempoChanged(project, context);
			result["applied"] = "project";
			return result;
		}
	}

	Json tempoJson(const Project& project)
	{
		return {{"tempo", toJson(project.tempo)}, {"tempoTrackActive", project.tempoTrackActive},
			{"tempoTrack", toJson(project.tempoTrack())}, {"fixedTempo", project.fixedTempo()}};
	}

	void tempoChanged(Project& project, CommandContext& context)
	{
		// A cycle marker in ticks can get shorter than a sample at a faster tempo.
		for (auto& [id, marker] : project.markers)
			if (keepMinimumLength(project, marker))
				context.emit("marker.changed", {{"marker", toJson(marker)}});
		context.emit("tempo.changed", tempoJson(project));
	}

	void ProjectModel::registerTempoCommands(Engine& engine)
	{
		const Json positionSchema = {{"type", {"integer", "string"}}};
		const Json ticksSchema = {{"type", "integer"}};
		const Json bpmSchema = {{"type", "number"}};
		const Json booleanSchema = {{"type", "boolean"}};
		const Json tempoResult = {{"type", "object"},
			{"properties", {{"tempo", {{"type", "object"}}}, {"position", ticksSchema}}}, {"required", {"tempo"}}};

		add(engine,
			{"tempo.add", 1,
				"Adds a tempo event to the tempo track at position, in ticks or \"cursor\", with a tempo in BPM (1 to "
				"999), or changes the one there. ramp true changes the tempo gradually from the event before it, "
				"false jumps there; without it the user setting editing.tempoType decides (ramp, step, or auto: the "
				"type of the event before it). The tempo track keeps its events in fixed tempo mode too, and plays "
				"them in tempo track mode (project.set tempoTrackActive). Returns the position in ticks and the "
				"tempo as tempo.changed reports it.",
				objectSchema(
					{{"position", positionSchema}, {"bpm", bpmSchema}, {"ramp", booleanSchema}}, {"position", "bpm"}),
				tempoResult, false},
			addTempo);
		add(engine,
			{"tempo.set", 1,
				"Changes tempo events of the tempo track, each named by its position in ticks: to moves it (the "
				"first event stays at the project start, and no two events share a position), bpm sets its tempo "
				"and ramp its type. All changes happen at once.",
				objectSchema({{"events",
								 {{"type", "array"},
									 {"items",
										 objectSchema({{"position", ticksSchema}, {"to", ticksSchema},
														  {"bpm", bpmSchema}, {"ramp", booleanSchema}},
											 {"position"})}}}},
					{"events"}),
				tempoResult, false},
			setTempo);
		add(engine,
			{"tempo.remove", 1,
				"Removes tempo events from the tempo track, by their positions in ticks. The first one stays.",
				objectSchema({{"positions", {{"type", "array"}, {"items", ticksSchema}}}}, {"positions"}), tempoResult,
				false},
			removeTempo);
		add(engine,
			{"tempo.tap", 1,
				"Applies a tempo found by tapping (Tap Tempo), rounded to 0.001 BPM, as mode says (default: the user "
				"setting transport.tapTempoMode): selection adds a tempo event at selection (ticks, the start of the "
				"selected events or range), playbackStart at the last playback start position, cursor at the "
				"project cursor; project sets the project tempo (the first tempo event in tempo track mode, the "
				"fixed tempo otherwise); display changes nothing. auto sets the project tempo in fixed tempo mode, "
				"else adds an event at selection if given, at the playback start while playing, or at the cursor. "
				"New events' type follows editing.tempoType, as in tempo.add. Returns the tempo, what it did "
				"(applied: event, project or none) and an event's position.",
				objectSchema(
					{{"bpm", bpmSchema},
						{"mode",
							{{"type", "string"},
								{"enum", {"auto", "selection", "playbackStart", "cursor", "project", "display"}}}},
						{"selection", positionSchema}},
					{"bpm"}),
				{{"type", "object"},
					{"properties",
						{{"bpm", bpmSchema}, {"applied", {{"type", "string"}, {"enum", {"event", "project", "none"}}}},
							{"position", {{"type", {"integer", "null"}}}}}},
					{"required", {"bpm", "applied", "position"}}},
				false},
			tapTempo);
	}
}
