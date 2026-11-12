// SPDX-License-Identifier: GPL-3.0-or-later
// Time warp, Process Tempo, Process Bars, Set Definition from Tempo and copying warp markers (#159, reference manual
// pp. 1463-1468). Each command is one undo step and changes nothing if it fails (ProjectModel::addEdit). The tempo
// commands edit the tempo track; what keeps its time while they do (Warp Grid, Reinterpret Bars) is on tracks made
// linear meanwhile (ProjectModel::runEdit of track.set), as tempo.detect does it.
#include "engine/document/timewarp.h"

#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/document/warp.h"
#include "engine/engine.h"
#include "engine/time/signaturemap.h"
#include "engine/time/tempomap.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		using time::TempoPoint;
		using time::Ticks;

		constexpr double g_maxWarpBpm = 360.0; // the time warp tool's limit (reference manual p. 1465)

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

		void requireTempoTrack(const Project& project, const std::string& what)
		{
			require(project.tempoTrackActive, what + " needs tempo track mode (project.set tempoTrackActive)");
		}

		// The tracks of a time base that can hold events, parts, markers or automation.
		std::vector<ObjectId> tracksIn(const Project& project, time::TimeBase base)
		{
			std::vector<ObjectId> tracks;
			for (const auto& [id, track] : project.trackById)
				if (track.timeBase == base && track.type != TrackType::Folder && track.type != TrackType::Ruler &&
					track.type != TrackType::Tempo && track.type != TrackType::Signature)
					tracks.push_back(id);
			return tracks;
		}

		void setTimeBase(ProjectModel& model, Project& project, const std::vector<ObjectId>& tracks, const char* base,
			CommandContext& context)
		{
			for (const ObjectId id : tracks)
				model.runEdit("track.set", project, {{"track", id}, {"timeBase", base}}, context);
		}

		// The tempo track as the given points, which must make a tempo map.
		void setTempoTrack(Project& project, std::vector<TempoPoint> points, CommandContext& context)
		{
			for (size_t i = 1; i < points.size(); ++i)
				require(points[i].position > points[i - 1].position,
					"two tempo events at " + std::to_string(points[i].position));
			for (const TempoPoint& point : points)
				require(
					point.bpm >= time::g_minBpm && point.bpm <= time::g_maxBpm, "the tempo would leave 1 to 999 BPM");
			points.front().ramp = false;
			const time::TempoMap before = project.tempo;
			project.tempoTrack() = time::TempoMap(points);
			tempoChanged(project, before, context);
		}

		// Sets the tempo track, also in fixed tempo mode, where it doesn't play; with keepTime what the musical tracks
		// hold keeps its time instead of its position.
		void setTempoTrack(ProjectModel& model, Project& project, std::vector<TempoPoint> points, bool keepTime,
			CommandContext& context)
		{
			const std::vector<ObjectId> musical =
				keepTime ? tracksIn(project, time::TimeBase::Musical) : std::vector<ObjectId>();
			setTimeBase(model, project, musical, "linear", context);
			setTempoTrack(project, std::move(points), context);
			setTimeBase(model, project, musical, "musical", context);
		}

		// The tempo track with the ticks from start to end replaced by span ticks at factor times their tempo; the
		// tempo events after them move by the difference. A ramp into start or end that the replacement would cut
		// ends a tick early, so that the tempo jumps there and keeps its time before start and after end.
		std::vector<TempoPoint> replaceTempo(
			const time::TempoMap& map, Ticks start, Ticks end, Ticks span, double factor)
		{
			const std::vector<TempoPoint>& old = map.points();
			const auto rampsInto = [&old](Ticks at)
			{
				const auto next = std::lower_bound(old.begin(), old.end(), at,
					[](const TempoPoint& point, Ticks position) { return point.position < position; });
				return next != old.end() && next->ramp;
			};
			const auto add = [](std::vector<TempoPoint>& points, const TempoPoint& point)
			{
				if (points.empty() || points.back().position < point.position)
					points.push_back(point);
			};
			std::vector<TempoPoint> points;
			for (const TempoPoint& point : old)
				if (point.position < start)
					points.push_back(point);
			const bool cut = start > 0 && rampsInto(start) && (factor != 1.0 || span == 0);
			if (cut)
				add(points, {start - 1, map.bpmAt(start - 1), true});
			if (span > 0)
				add(points, {start, map.bpmAt(start) * factor, start > 0 && rampsInto(start) && !cut});
			// Deleted ticks (span 0) drop their tempo events, so that the tempo at end carries on from start.
			for (const TempoPoint& point : old)
				if (span > 0 && point.position > start && point.position < end)
				{
					const auto at = static_cast<double>(point.position - start) * static_cast<double>(span) /
						static_cast<double>(end - start);
					add(points, {start + std::llround(at), point.bpm * factor, point.ramp});
				}
			if (span > 0 && end > start && rampsInto(end))
				add(points, {start + span - 1, map.bpmAt(end - 1) * factor, true});
			add(points, {start + span, map.bpmAt(end), false});
			for (const TempoPoint& point : old)
				if (point.position > end)
					points.push_back({point.position + span - (end - start), point.bpm, point.ramp});
			return points;
		}

		// A locator range parameter: start and end in ticks, by default the locators'.
		std::pair<Ticks, Ticks> rangeOf(const Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			Ticks start = 0;
			Ticks end = 0;
			if (!parameters.contains("start") || !parameters.contains("end"))
			{
				const Json transport = context.call("transport.get");
				start = project.tempo.toTicks(transport.at("left").get<time::Samples>(), project.sampleRate);
				end = project.tempo.toTicks(transport.at("right").get<time::Samples>(), project.sampleRate);
			}
			start = parameters.value("start", start);
			end = parameters.value("end", end);
			require(start >= 0 && end > start, "the range must start at or after 0 and end after its start");
			return {start, end};
		}

		// Warp Grid (reference manual pp. 1465-1466): the tempo event before position gets the tempo that makes
		// position play at to; a tempo event at position, if later ones exist, keeps the tempo after it.
		Json warpTempo(ProjectModel& model, Project& project, CommandContext& context)
		{
			requireTempoTrack(project, "time warp");
			const Json& parameters = context.parameters();
			const auto position = parameters.at("position").get<Ticks>();
			require(position > 0, "the position must be after the project start");
			const double target = static_cast<double>(parameters.at("to").get<time::Samples>()) / project.sampleRate;
			std::vector<TempoPoint> points = project.tempo.points();
			const auto next = std::lower_bound(points.begin(), points.end(), position,
				[](const TempoPoint& point, Ticks at) { return point.position < at; });
			const auto before = static_cast<size_t>(next - points.begin()) - 1;
			if (next != points.end() && next->position != position)
				points.insert(next, {position, project.tempo.bpmAt(position), next->ramp});
			const auto secondsWith = [&points, before, position](double bpm)
			{
				std::vector<TempoPoint> trial = points;
				trial[before].bpm = bpm;
				return time::TempoMap(trial).toSeconds(position);
			};
			require(secondsWith(g_maxWarpBpm) <= target && secondsWith(time::g_minBpm) >= target,
				"the position can't play at that time with a tempo from 1 to 360 BPM");
			// The later position plays the slower the tempo: bisection.
			double slow = time::g_minBpm;
			double fast = g_maxWarpBpm;
			for (int i = 0; i < 100; ++i)
			{
				const double middle = (slow + fast) / 2.0;
				if (secondsWith(middle) > target)
					slow = middle;
				else
					fast = middle;
			}
			points[before].bpm = (slow + fast) / 2.0;
			const TempoPoint changed = points[before];
			setTempoTrack(model, project, std::move(points), !parameters.value("follow", false), context);
			return {{"event", changed.position}, {"bpm", changed.bpm}, {"tempo", tempoJson(project)}};
		}

		// Process Tempo (reference manual pp. 1463-1464): the range lasts the new length.
		Json processTempo(Project& project, CommandContext& context)
		{
			requireTempoTrack(project, "process tempo");
			const Json& parameters = context.parameters();
			const auto [start, end] = rangeOf(project, context);
			const double from = project.tempo.toSeconds(start);
			const double length = project.tempo.toSeconds(end) - from;
			require(parameters.contains("newLength") != parameters.contains("newEnd"),
				"give newLength or newEnd, not both");
			const double newLength = parameters.contains("newLength") ? parameters.at("newLength").get<double>()
																	  : parameters.at("newEnd").get<double>() - from;
			require(newLength > 0.0, "the new range must be longer than nothing");
			const double factor = length / newLength;
			setTempoTrack(project, replaceTempo(project.tempo, start, end, end - start, factor), context);
			return {{"factor", factor}, {"tempo", tempoJson(project)}};
		}

		time::TimeSignature signatureFrom(const Json& parameters)
		{
			const time::TimeSignature signature = {
				parameters.value("numerator", 4), parameters.value("denominator", 4)};
			const int d = signature.denominator;
			require(signature.numerator >= 1 && signature.numerator <= 99 &&
					(d == 1 || d == 2 || d == 4 || d == 8 || d == 16),
				"the time signature must be 1 to 99 beats of 1, 2, 4, 8 or 16");
			return signature;
		}

		Ticks barLength(const time::TimeSignature& signature)
		{
			return 4 * time::g_ticksPerQuarter * signature.numerator / signature.denominator;
		}

		// Process Bars (reference manual p. 1464): bars inserted, deleted, replaced or reinterpreted in the time
		// signature events and the tempo track.
		Json processBars(ProjectModel& model, Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const auto bar = parameters.at("bar").get<int64_t>();
			const auto bars = parameters.at("bars").get<int64_t>();
			require(bar >= 0 && bars >= 1, "bar must not be negative and bars at least 1");
			const std::string type = parameters.at("type").get<std::string>();
			const time::TimeSignature signature = signatureFrom(parameters);
			const time::SignatureMap& map = project.signatures;
			const Ticks start = map.barStart(bar);
			const Ticks end = map.barStart(bar + bars);
			const time::TimeSignature atStart = map.signatureAt(start);
			const time::TimeSignature atEnd = map.signatureAt(end);
			const std::vector<time::SignatureChange> old = map.changes();
			const auto has = [&old](int64_t at)
			{
				return std::any_of(
					old.begin(), old.end(), [at](const time::SignatureChange& change) { return change.bar == at; });
			};
			std::vector<time::SignatureChange> changes;
			for (const time::SignatureChange& change : old)
				if (change.bar < bar)
					changes.push_back(change);
			std::vector<TempoPoint> tempo;
			bool keepTime = false;
			if (type == "insert")
			{
				changes.push_back({bar, signature});
				if (!has(bar))
					changes.push_back({bar + bars, atStart});
				for (const time::SignatureChange& change : old)
					if (change.bar >= bar)
						changes.push_back({change.bar + bars, change.signature});
				tempo = replaceTempo(project.tempoTrack(), start, start, bars * barLength(signature), 1.0);
			}
			else if (type == "delete")
			{
				if (!has(bar + bars))
					changes.push_back({bar, atEnd});
				for (const time::SignatureChange& change : old)
					if (change.bar >= bar + bars)
						changes.push_back({change.bar - bars, change.signature});
				tempo = replaceTempo(project.tempoTrack(), start, end, 0, 1.0);
			}
			else
			{
				require(
					type == "replace" || type == "reinterpret", "type must be insert, delete, replace or reinterpret");
				changes.push_back({bar, signature});
				if (!has(bar + bars))
					changes.push_back({bar + bars, atEnd});
				for (const time::SignatureChange& change : old)
					if (change.bar >= bar + bars)
						changes.push_back(change);
				const Ticks span = bars * barLength(signature);
				keepTime = type == "reinterpret";
				const double factor = keepTime ? static_cast<double>(span) / static_cast<double>(end - start) : 1.0;
				tempo = replaceTempo(project.tempoTrack(), start, end, span, factor);
			}
			project.signatures = time::SignatureMap(changes);
			context.emit("signature.changed", {{"signatures", signaturesJson(project)}});
			setTempoTrack(model, project, std::move(tempo), keepTime, context);
			return {{"tempo", tempoJson(project)}, {"signatures", signaturesJson(project)}};
		}

		std::vector<AudioEvent*> audioEventsOf(Project& project, const Json& ids)
		{
			std::vector<AudioEvent*> events;
			for (const Json& id : ids)
			{
				const auto found = project.audioEvents.find(id.get<ObjectId>());
				require(found != project.audioEvents.end(), "no audio event " + id.dump());
				events.push_back(&found->second);
			}
			require(!events.empty(), "events must name at least one audio event");
			return events;
		}

		// Where an audio event plays its clip frames without warping: project samples from its clip frames, and back.
		struct EventTime
		{
			double start = 0.0; // project samples
			double offset = 0.0;
			double ratio = 1.0; // project samples per clip frame

			EventTime(const Project& project, const AudioEvent& event)
				: start(static_cast<double>(time::toSamples(event.position, project.tempo, project.sampleRate))),
				  offset(static_cast<double>(event.offset)),
				  ratio(project.sampleRate / project.media.at(project.clips.at(event.clip).media).sampleRate)
			{
			}

			double samplesAt(double frame) const { return start + ((frame - offset) * ratio); }
			double frameAt(double samples) const { return offset + ((samples - start) / ratio); }
		};

		// Set Definition from Tempo (reference manual pp. 1467-1468): the clip's definition grid on the project's
		// quarters where the event plays them now, from the first after its start to the clip's end, in musical
		// mode, so that it plays as before and follows the tempo from then on.
		void defineFromTempo(Project& project, const AudioEvent& event)
		{
			Clip& clip = project.clips.at(event.clip);
			require(!clip.warp.musical, "the clip of audio event " + std::to_string(event.id) + " is in musical mode");
			const auto length = static_cast<double>(project.media.at(clip.media).length);
			const EventTime timing(project, event);
			const auto frameAt = [&project, &timing](Ticks tick)
			{ return timing.frameAt(project.tempo.toSeconds(tick) * project.sampleRate); };
			const Ticks startTicks = time::toTicks(event.position, project.tempo, project.sampleRate);
			const Ticks quarter = time::g_ticksPerQuarter;
			const Ticks first = (std::max<Ticks>(0, startTicks) + quarter - 1) / quarter * quarter;
			require(frameAt(first) < length,
				"audio event " + std::to_string(event.id) + "'s clip ends before the next beat");
			ClipWarp& warp = clip.warp;
			warp.musical = true;
			warp.gridStart = std::llround(frameAt(first));
			warp.beats.clear();
			int64_t beat = 1;
			for (; frameAt(first + (beat * quarter)) < length; ++beat)
				warp.beats.push_back({beat, std::llround(frameAt(first + (beat * quarter)))});
			// The clip's tempo holds beyond the beats edited: the tempo after the last one, before the clip ends.
			// ponytail: so does it before the first beat; a beat edit before the grid start would need one there.
			warp.tempo = project.tempo.bpmAt(first + ((beat - 1) * quarter));
		}

		Json setDefinitionFromTempo(ProjectModel& model, Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			std::vector<ObjectId> eventIds;
			for (const AudioEvent* event : audioEventsOf(project, parameters.at("events")))
				eventIds.push_back(event->id);
			// The tracks become musical first, keeping their time, so that the events start on ticks.
			std::vector<ObjectId> linear = tracksIn(project, time::TimeBase::Linear);
			if (!parameters.value("allTracks", false))
			{
				std::set<ObjectId> own;
				for (const ObjectId id : eventIds)
					own.insert(project.audioEvents.at(id).track);
				linear.erase(std::remove_if(linear.begin(), linear.end(),
								 [&own](ObjectId track) { return own.count(track) == 0; }),
					linear.end());
			}
			setTimeBase(model, project, linear, "musical", context);
			std::set<ObjectId> clips;
			for (const ObjectId id : eventIds)
			{
				const AudioEvent& event = project.audioEvents.at(id);
				if (clips.insert(event.clip).second)
					defineFromTempo(project, event);
			}
			Json result = Json::array();
			for (const ObjectId id : clips)
			{
				context.emit("clip.changed", {{"clip", toJson(project.clips.at(id))}});
				result.push_back(id);
			}
			return {{"clips", result}};
		}

		// Whether a warp marker fits among a clip's markers: at a position of its own, passing none of them, and
		// within the real-time stretcher's range of those around it (checkMarkers; sections it leaves as they are
		// may be beyond it).
		bool fits(const std::vector<WarpMarker>& markers, const WarpMarker& marker, time::Samples length)
		{
			ClipWarp warp;
			warp.markers = markers;
			warp.markers.push_back(marker);
			try
			{
				checkMarkers(warp, length, markers);
				return true;
			}
			catch (const CommandError&)
			{
				return false;
			}
		}

		// Paste Warp Markers to Selected Events (reference manual p. 1467): the warp markers of from's clip into
		// the clips of the events, at the same times.
		Json pasteWarpMarkers(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const AudioEvent& source = *audioEventsOf(project, Json::array({parameters.at("from")})).front();
			const std::vector<WarpMarker> markers = project.clips.at(source.clip).warp.markers;
			require(!markers.empty(), "the clip of audio event " + std::to_string(source.id) + " has no warp markers");
			const EventTime from(project, source);
			size_t pasted = 0;
			std::set<ObjectId> changed;
			for (const AudioEvent* event : audioEventsOf(project, parameters.at("events")))
			{
				Clip& clip = project.clips.at(event->clip);
				if (clip.id == source.clip)
					continue; // the markers are there already
				const time::Samples length = project.media.at(clip.media).length;
				const EventTime to(project, *event);
				const auto frame = [&from, &to](time::Samples at)
				{ return std::llround(to.frameAt(from.samplesAt(static_cast<double>(at)))); };
				for (const WarpMarker& marker : markers)
				{
					const WarpMarker copy{frame(marker.position), frame(marker.target)};
					if (copy.position < event->offset || copy.position >= event->offset + event->length ||
						!fits(clip.warp.markers, copy, length))
						continue;
					clip.warp.markers.push_back(copy);
					++pasted;
					changed.insert(clip.id);
				}
				std::sort(clip.warp.markers.begin(), clip.warp.markers.end(),
					[](const WarpMarker& a, const WarpMarker& b) { return a.position < b.position; });
			}
			for (const ObjectId id : changed)
				context.emit("clip.changed", {{"clip", toJson(project.clips.at(id))}});
			return {{"markers", pasted}};
		}
	}

	void registerTimeWarpCommands(ProjectModel& model, Engine& engine)
	{
		const Json integer = {{"type", "integer"}};
		const Json number = {{"type", "number"}};
		const Json boolean = {{"type", "boolean"}};
		const Json ids = {{"type", "array"}, {"items", integer}};

		model.addEdit(engine,
			{"tempo.warp", 1,
				"Time warp in Warp Grid mode (reference manual pp. 1465-1466): makes the musical position position "
				"(ticks, after the project start) play at the time to (project samples) by changing the tempo of the "
				"last tempo event before it, to 1 to 360 BPM; if later tempo events exist, a tempo event at position "
				"keeps the tempo after it as it was. Musical tracks keep the time of what they hold (Warp Grid), or "
				"with follow it keeps its position and follows the tempo (Warp Grid, musical events follow). Needs "
				"tempo track mode. Returns the position of the tempo event changed, its tempo, and the tempo as "
				"tempo.changed reports it.",
				objectSchema({{"position", integer}, {"to", integer}, {"follow", boolean}}, {"position", "to"}),
				{{"type", "object"},
					{"properties", {{"event", integer}, {"bpm", number}, {"tempo", {{"type", "object"}}}}},
					{"required", {"event", "bpm", "tempo"}}},
				false},
			"Time Warp",
			[&model](Project& project, CommandContext& context) { return warpTempo(model, project, context); });

		model.addEdit(engine,
			{"tempo.process", 1,
				"Process Tempo (reference manual pp. 1463-1464): scales the tempo track's tempo from start to end "
				"(ticks, default the locators) so that the range lasts newLength seconds, or ends at newEnd (seconds "
				"of project time). The tempo after the range stays, and musical tracks follow. A ramp across an edge "
				"of the range ends a tick before it. Needs tempo track mode. Returns the factor the tempo was "
				"multiplied by (Equivalent Tempo Scaling) and the tempo as tempo.changed reports it.",
				objectSchema({{"start", integer}, {"end", integer}, {"newLength", number}, {"newEnd", number}}),
				{{"type", "object"}, {"properties", {{"factor", number}, {"tempo", {{"type", "object"}}}}},
					{"required", {"factor", "tempo"}}},
				false},
			"Process Tempo", processTempo);

		model.addEdit(engine,
			{"tempo.processBars", 1,
				"Process Bars (reference manual p. 1464) from bar (zero-based) for bars bars, in the time signature "
				"events and the tempo track's events: insert puts bars bars of numerator/denominator (default 4/4) "
				"there, at the tempo there, and moves the later events by them; delete removes the bars and moves "
				"the later events back; replace gives the bars the time signature, at the same tempo; reinterpret "
				"too, with the tempo scaled so that they last as long as before, and what musical tracks hold keeps "
				"its time. Otherwise events, parts, markers and automation keep their positions, as for any time "
				"signature change. Returns the tempo as tempo.changed and the time signature events as "
				"signature.changed report them.",
				objectSchema(
					{{"bar", integer}, {"bars", integer},
						{"type", {{"type", "string"}, {"enum", {"insert", "delete", "replace", "reinterpret"}}}},
						{"numerator", integer}, {"denominator", integer}},
					{"bar", "bars", "type"}),
				{{"type", "object"},
					{"properties", {{"tempo", {{"type", "object"}}}, {"signatures", {{"type", "array"}}}}},
					{"required", {"tempo", "signatures"}}},
				false},
			"Process Bars",
			[&model](Project& project, CommandContext& context) { return processBars(model, project, context); });

		model.addEdit(engine,
			{"event.setDefinitionFromTempo", 1,
				"Set Definition from Tempo (reference manual pp. 1467-1468): gives the clips of the audio events "
				"(ids) the project's tempo as their definition, the grid on each quarter where the event plays it "
				"now, and turns musical mode on, so that they play as before and follow tempo changes from then on. "
				"The events' tracks, or with allTracks every track, become musical first, keeping their time. A clip "
				"in musical mode already is refused. Returns the clips.",
				objectSchema({{"events", ids}, {"allTracks", boolean}}, {"events"}),
				{{"type", "object"}, {"properties", {{"clips", ids}}}, {"required", {"clips"}}}, false},
			"Set Definition from Tempo", [&model](Project& project, CommandContext& context)
			{ return setDefinitionFromTempo(model, project, context); });

		model.addEdit(engine,
			{"event.pasteWarpMarkers", 1,
				"Copies the free warp markers of the audio event from's clip into the clips of the audio events "
				"(Copy Warp Markers from Selected Event, Paste Warp Markers to Selected Events, reference manual p. "
				"1467): each at the same time as in from, where the event plays it unwarped, if that is within the "
				"event, passes none of its clip's markers and makes no section around it play more than 4 times or "
				"less than a quarter as long, the real-time stretcher's range. The markers there stay. Returns how "
				"many it pasted.",
				objectSchema({{"from", integer}, {"events", ids}}, {"from", "events"}),
				{{"type", "object"}, {"properties", {{"markers", integer}}}, {"required", {"markers"}}}, false},
			"Paste Warp Markers", pasteWarpMarkers);
	}
}
