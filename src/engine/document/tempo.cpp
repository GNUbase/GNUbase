// SPDX-License-Identifier: GPL-3.0-or-later
// The tempo track (#155, reference manual pp. 162, 1446-1458): tempo.add, tempo.set and tempo.remove edit the tempo
// track's tempo events, in either tempo mode, and tempo.tap applies a tapped tempo. project.set switches between fixed
// tempo and tempo track mode (tempoTrackActive). Each command is one undo step and changes nothing if it fails
// (ProjectModel::add). tempo.export and tempo.import write and read the tempo track with the time signature events
// as a file of their own (#160, reference manual p. 1462). tempo.fromMidi and tempo.detect make the tempo track follow
// tapped MIDI notes or the beats of a recording (#158, reference manual pp. 1130, 1459-1461).
#include "engine/analysis/tempodetect.h"
#include "engine/dawproject/dawproject.h"
#include "engine/document/document.h"
#include "engine/document/hitpoints.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
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
			const time::TempoMap before = project.tempo;
			project.tempoTrack() = time::TempoMap(points);
			tempoChanged(project, before, context);
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
			const time::TempoMap before = project.tempo;
			project.tempo = time::TempoMap(points);
			tempoChanged(project, before, context);
			result["applied"] = "project";
			return result;
		}

		constexpr double g_secondsPerMinute = 60.0;
		constexpr double g_minDetectSeconds = 7.0; // reference manual p. 1460
		constexpr double g_chord = 0.03;           // seconds: notes that start this close together are one tap

		// The tempo track playing beats (seconds, in order) distance ticks apart: a step tempo event at each beat but
		// the last replaces the tempo events from the first beat to the last; beats before the project start don't
		// count. The first beat stays at its position;
		// with barStart it moves to the nearest bar start after the project start, which a tempo event at the bar
		// before puts at the beat's time. Activates the tempo track; its events come from the tempo playing.
		void followBeats(
			Project& project, std::vector<double> beats, time::Ticks distance, bool barStart, CommandContext& context)
		{
			beats.erase(beats.begin(), std::lower_bound(beats.begin(), beats.end(), 0.0)); // not before the start
			require(beats.size() >= 2, "a tempo needs two notes or beats at least");
			const time::TempoMap playing = project.tempo;
			time::Ticks start = playing.toTicks(beats.front());
			std::vector<time::TempoPoint> added;
			if (barStart)
			{
				const time::SignatureMap& bars = project.signatures;
				const int64_t bar = bars.toBarsBeats(start).bar;
				const time::Ticks next = bars.barStart(bar + 1);
				time::Ticks grid = start - bars.barStart(bar) <= next - start ? bars.barStart(bar) : next;
				if (grid == 0 && start > 0)
					grid = next;
				if (grid != start)
				{
					const time::Ticks before = bars.barStart(bars.toBarsBeats(grid).bar - 1);
					const double bpm = g_secondsPerMinute * static_cast<double>(grid - before) /
						static_cast<double>(time::g_ticksPerQuarter) / (beats.front() - playing.toSeconds(before));
					require(bpm >= time::g_minBpm && bpm <= time::g_maxBpm,
						"the first note is too far from a bar start to begin there");
					added.push_back({before, bpm, false});
					start = grid;
				}
			}
			const double quarters = static_cast<double>(distance) / static_cast<double>(time::g_ticksPerQuarter);
			for (size_t i = 0; i + 1 < beats.size(); ++i)
				added.push_back({start + (static_cast<time::Ticks>(i) * distance),
					std::clamp(
						g_secondsPerMinute * quarters / (beats[i + 1] - beats[i]), time::g_minBpm, time::g_maxBpm),
					false});
			const time::Ticks from = added.front().position;
			const time::Ticks to = start + (static_cast<time::Ticks>(beats.size() - 1) * distance);
			std::vector<time::TempoPoint> points;
			for (const time::TempoPoint& point : playing.points())
				if (point.position < from || point.position > to)
					points.push_back(point);
			points.insert(points.end(), added.begin(), added.end());
			if (!project.tempoTrackActive)
			{
				project.otherTempo = project.tempo;
				project.tempoTrackActive = true;
			}
			setTempoTrack(project, std::move(points), context);
		}

		// The start times, in seconds, of the notes of MIDI parts, or of the selected ones (notes), in order.
		std::vector<double> noteStarts(const Project& project, const std::vector<const MidiPart*>& parts,
			const std::optional<std::set<ObjectId>>& notes)
		{
			std::vector<double> starts;
			for (const MidiPart* part : parts)
			{
				const time::Ticks at = time::toTicks(part->position, project.tempo, project.sampleRate);
				for (const auto& [id, note] : part->notes)
					if (!notes || notes->count(id) != 0)
						starts.push_back(project.tempo.toSeconds(at + note.position));
			}
			std::sort(starts.begin(), starts.end());
			return starts;
		}

		std::vector<const MidiPart*> midiPartsOf(const Project& project, const Json& ids)
		{
			std::vector<const MidiPart*> parts;
			for (const Json& id : ids)
			{
				const auto found = project.midiParts.find(id.get<ObjectId>());
				require(found != project.midiParts.end(), "no MIDI part " + id.dump());
				parts.push_back(&found->second);
			}
			require(!parts.empty(), "parts must name at least one MIDI part");
			return parts;
		}

		// Calculate Tempo from MIDI Events (reference manual pp. 1130, 1459-1460).
		Json tempoFromMidi(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const std::vector<const MidiPart*> parts = midiPartsOf(project, parameters.at("parts"));
			std::optional<std::set<ObjectId>> notes;
			if (parameters.contains("notes"))
				notes = parameters.at("notes").get<std::set<ObjectId>>();
			const time::Ticks distance = parameters.value("distance", time::g_ticksPerQuarter);
			require(distance > 0, "distance must be positive");
			// Notes struck together, such as a chord, are one tap.
			std::vector<double> taps;
			for (const double start : noteStarts(project, parts, notes))
				if (taps.empty() || start - taps.back() > g_chord)
					taps.push_back(start);
			followBeats(project, taps, distance, parameters.value("beginAtBarStart", false), context);
			return {{"tempo", tempoJson(project)}};
		}

		// The material of an audio event or a MIDI part: where it is, in project seconds, and its pulses, from its
		// hitpoints' peaks or its notes' velocities.
		struct Material
		{
			double start = 0.0;
			double end = 0.0;
			std::vector<analysis::Pulse> pulses;
		};

		Material materialOf(const Project& project, ObjectId id)
		{
			Material material;
			if (const auto event = project.audioEvents.find(id); event != project.audioEvents.end())
			{
				const AudioEvent& audio = event->second;
				const MediaFile& media = project.media.at(project.clips.at(audio.clip).media);
				material.start =
					static_cast<double>(time::toSamples(audio.position, project.tempo, project.sampleRate)) /
					project.sampleRate;
				material.end = material.start + (static_cast<double>(audio.length) / media.sampleRate);
				for (const Hitpoint& hitpoint : detectHitpoints(media))
					if (hitpoint.position >= audio.offset && hitpoint.position < audio.offset + audio.length)
						material.pulses.push_back(
							{static_cast<double>(hitpoint.position - audio.offset) / media.sampleRate,
								std::pow(10.0, hitpoint.peak / 20.0)});
				return material;
			}
			const auto part = project.midiParts.find(id);
			require(part != project.midiParts.end(), "no audio event or MIDI part " + std::to_string(id));
			const MidiPart& midi = part->second;
			const time::Ticks at = time::toTicks(midi.position, project.tempo, project.sampleRate);
			const time::Position end{midi.position.base, midi.position.value + midi.length};
			material.start = project.tempo.toSeconds(at);
			material.end = project.tempo.toSeconds(time::toTicks(end, project.tempo, project.sampleRate));
			for (const auto& [noteId, note] : midi.notes)
				material.pulses.push_back(
					{project.tempo.toSeconds(at + note.position) - material.start, static_cast<double>(note.velocity)});
			return material;
		}

		Json exportTempo(Project& project, CommandContext& context)
		{
			const TempoTrackFile file{project.tempoTrack(), project.signatures};
			try
			{
				saveTempoTrack(file, context.parameters().at("path").get<std::string>());
			}
			catch (const DawProjectError& error)
			{
				throw CommandError(error.what());
			}
			return {{"tempoEvents", file.tempo.points().size()}, {"signatureEvents", file.signatures.changes().size()}};
		}

		Json importTempo(Project& project, CommandContext& context)
		{
			TempoTrackFile file;
			try
			{
				file = loadTempoTrack(context.parameters().at("path").get<std::string>());
			}
			catch (const DawProjectError& error)
			{
				throw CommandError(error.what());
			}
			// The file's tempo changes play: the tempo track becomes active, as for a MIDI file's (midiFile.import).
			const time::TempoMap before = project.tempo;
			if (file.tempo.points().size() > 1 && !project.tempoTrackActive)
			{
				project.otherTempo = project.tempo;
				project.tempoTrackActive = true;
			}
			project.tempoTrack() = std::move(file.tempo);
			project.signatures = std::move(file.signatures);
			context.emit("signature.changed", {{"signatures", signaturesJson(project)}});
			tempoChanged(project, before, context);
			return {{"tempo", tempoJson(project)}, {"signatures", signaturesJson(project)}};
		}
	}

	Json tempoJson(const Project& project)
	{
		return {{"tempo", toJson(project.tempo)}, {"tempoTrackActive", project.tempoTrackActive},
			{"tempoTrack", toJson(project.tempoTrack())}, {"fixedTempo", project.fixedTempo()}};
	}

	void tempoChanged(Project& project, const time::TempoMap& before, CommandContext& context)
	{
		// A linear part stays where it is in samples, and its notes and controllers, in ticks from its start, stay at
		// their time with it (reference manual p. 1446).
		std::set<ObjectId> changed;
		if (project.tempo != before)
			for (auto& [id, part] : project.midiParts)
			{
				if (part.position.base != time::TimeBase::Linear)
					continue;
				const time::Ticks startBefore = before.toTicks(part.position.value, project.sampleRate);
				const time::Ticks start = project.tempo.toTicks(part.position.value, project.sampleRate);
				const auto retime = [&](time::Ticks position)
				{ return project.tempo.toTicks(before.toSeconds(startBefore + position)) - start; };
				const auto retimeNote = [&retime](time::Ticks& position, time::Ticks& length)
				{
					const time::Ticks end = retime(position + length);
					position = retime(position);
					length = std::max<time::Ticks>(length > 0 ? 1 : 0, end - position);
				};
				for (auto& [noteId, note] : part.notes)
				{
					retimeNote(note.position, note.length);
					if (note.origin)
					{
						retimeNote(note.origin->original.position, note.origin->original.length);
						retimeNote(note.origin->quantized.position, note.origin->quantized.length);
					}
				}
				for (auto& [controllerId, controller] : part.controllers)
					controller.position = retime(controller.position);
				changed.insert(id);
			}
		// Shared copies that now hold different notes, such as copies before and after a tempo event, or a linear and
		// a musical copy, can't share them: each copy that differs from its group's first becomes a real copy, as
		// event.toRealCopy makes it. A first copy left alone is no longer shared (syncSharedParts).
		std::map<ObjectId, const MidiPart*> firsts;
		for (auto& [id, part] : project.midiParts)
		{
			if (part.shared == 0)
				continue;
			const MidiPart* first = firsts.emplace(part.shared, &part).first->second;
			if (first->notes != part.notes || first->controllers != part.controllers)
			{
				part.shared = 0;
				renumber(project, part);
				changed.insert(id);
			}
		}
		for (const ObjectId id : changed)
			context.emit("midiPart.changed", {{"part", toJson(project.midiParts.at(id))}});
		// A cycle marker in ticks can get shorter than a sample at a faster tempo.
		for (auto& [id, marker] : project.markers)
			if (keepMinimumLength(project, marker))
				context.emit("marker.changed", {{"marker", toJson(marker)}});
		context.emit("tempo.changed", tempoJson(project));
	}

	Json ProjectModel::detectTempo(Project& project, CommandContext& context)
	{
		const Json& parameters = context.parameters();
		const Material material = materialOf(project, parameters.at("event").get<ObjectId>());
		require(material.end - material.start >= g_minDetectSeconds - 1e-9,
			"tempo detection needs 7 seconds of material at least");
		if (parameters.value("reset", false))
		{
			const time::Ticks from = project.tempo.toTicks(material.start);
			const time::Ticks to = project.tempo.toTicks(material.end);
			std::vector<time::TempoPoint> points = project.tempoTrack().points();
			points.erase(std::remove_if(points.begin() + 1, points.end(), [from, to](const time::TempoPoint& point)
							 { return point.position >= from && point.position < to; }),
				points.end());
			setTempoTrack(project, std::move(points), context);
			return {{"tempo", tempoJson(project)}, {"beats", 0}};
		}
		const double factor = parameters.value("factor", 1.0);
		require(factor > 0.0, "factor must be positive");
		// Nothing moves in time while the tempo changes (reference manual p. 1461).
		std::vector<ObjectId> musical;
		for (const auto& [id, track] : project.trackById)
			if (track.timeBase == time::TimeBase::Musical && track.type != TrackType::Folder &&
				track.type != TrackType::Ruler && track.type != TrackType::Tempo && track.type != TrackType::Signature)
				musical.push_back(id);
		for (const ObjectId id : musical)
			runEdit("track.set", project, {{"track", id}, {"timeBase", "linear"}}, context);
		std::vector<double> beats =
			analysis::rescaleBeats(analysis::detectBeats(material.pulses, material.end - material.start), factor,
				parameters.value("offbeat", false));
		if (parameters.value("smooth", false))
			beats = analysis::smoothBeats(beats);
		require(beats.size() >= 2, "no beats found in " + parameters.at("event").dump());
		for (double& beat : beats)
			beat += material.start;
		followBeats(project, beats, time::g_ticksPerQuarter, false, context);
		return {{"tempo", tempoJson(project)}, {"beats", beats.size()}};
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
		add(engine,
			{"tempo.fromMidi", 1,
				"Calculates the tempo from MIDI notes tapped to freely played material (Calculate Tempo from MIDI "
				"Events): the notes of parts (MIDI part ids), or the selected ones among them (notes), are distance "
				"ticks apart (Distance of Notes, default a quarter). Notes that start within 30 ms of each other are "
				"one. A step tempo event at each note but the last replaces the tempo events from the first note to "
				"the last, so that each note is distance after the one before and keeps its time; the first one keeps "
				"its position, or with beginAtBarStart moves to the nearest bar start after the project start, where "
				"a tempo event at the bar before puts it. Notes on linear tracks stay at their time, those on musical "
				"ones follow the tempo. Makes the tempo track active; its events come from the tempo the project "
				"played. Returns the tempo as tempo.changed reports it.",
				objectSchema({{"parts", {{"type", "array"}, {"items", ticksSchema}}},
								 {"notes", {{"type", "array"}, {"items", ticksSchema}}}, {"distance", ticksSchema},
								 {"beginAtBarStart", booleanSchema}},
					{"parts"}),
				tempoResult, false},
			tempoFromMidi);
		add(engine,
			{"tempo.detect", 1,
				"Detects the tempo of an audio event or a MIDI part (event, its id) of at least 7 seconds with "
				"discernible beats (Tempo Detection) and makes the tempo track follow it: where its sounds start "
				"(the hitpoints of the clip's section, or the notes) give the beats, which may drift, and a step "
				"tempo event at each beat but the last, a quarter apart, replaces the tempo events from the first "
				"beat to the last, which keeps its position. factor (default 1) makes the beats that many times as "
				"fast: 2 (Multiply by 2), 0.5 (Divide by 2), 4/3 or 3/4. offbeat moves them by half a beat (Offbeat "
				"Correction), smooth puts them on one steady tempo (Smooth Tempo). Every musical track that holds "
				"events, parts, markers or automation becomes linear first, so nothing moves in time. reset removes "
				"the tempo events inside the event instead (Reset the analysis result). Makes the tempo track "
				"active. One undo step. Returns the tempo as tempo.changed reports it and the number of beats.",
				objectSchema({{"event", ticksSchema}, {"factor", bpmSchema}, {"offbeat", booleanSchema},
								 {"smooth", booleanSchema}, {"reset", booleanSchema}},
					{"event"}),
				{{"type", "object"}, {"properties", {{"tempo", {{"type", "object"}}}, {"beats", ticksSchema}}},
					{"required", {"tempo", "beats"}}},
				false},
			[this](Project& project, CommandContext& context) { return detectTempo(project, context); });
		const Json pathSchema = objectSchema({{"path", {{"type", "string"}}}}, {"path"});
		add(engine,
			{"tempo.export", 1,
				"Exports the tempo track's events, which fixed tempo mode keeps too, and the time signature events "
				"into an XML file at path (File > Export > Tempo Track), which tempo.import reads. Returns the number "
				"of tempo and time signature events.",
				pathSchema,
				{{"type", "object"}, {"properties", {{"tempoEvents", ticksSchema}, {"signatureEvents", ticksSchema}}},
					{"required", {"tempoEvents", "signatureEvents"}}},
				true},
			exportTempo);
		add(engine,
			{"tempo.import", 1,
				"Imports a file that tempo.export wrote (File > Import > Tempo Track): its tempo events replace the "
				"tempo track's and its time signature events the project's. A file with tempo changes makes the "
				"tempo track active; the fixed tempo stays. One undo step. Returns the tempo as tempo.changed reports "
				"it and the time signature events as signature.changed does.",
				pathSchema,
				{{"type", "object"},
					{"properties", {{"tempo", {{"type", "object"}}}, {"signatures", {{"type", "array"}}}}},
					{"required", {"tempo", "signatures"}}},
				false},
			importTempo);
	}
}
