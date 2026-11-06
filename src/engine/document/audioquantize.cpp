// SPDX-License-Identifier: GPL-3.0-or-later
// Audio quantizing (#145, docs/features/audio-quantize.md, reference manual pp. 367-380): audio event starts, warp
// quantizing of warp markers, and several audio tracks sliced, warp marked and crossfaded together.
#include "engine/document/audioquantize.h"

#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/document/warp.h"
#include "engine/dsp/warp.h"
#include "engine/engine.h"
#include "engine/time/tempomap.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
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

		time::Samples startOf(const Project& project, const AudioEvent& event)
		{
			return time::toSamples(event.position, project.tempo, project.sampleRate);
		}

		// Project samples after the event's start where its snap point plays; 0 without one inside it.
		time::Samples snapOffset(const Project& project, const AudioEvent& event)
		{
			if (!event.snapPoint || *event.snapPoint < event.offset || *event.snapPoint >= event.offset + event.length)
				return 0;
			return std::llround(eventSamples(project, event, static_cast<double>(*event.snapPoint - event.offset)));
		}

		// The project sample where a clip frame of the event plays.
		time::Samples sampleOf(const Project& project, const AudioEvent& event, time::Samples frame)
		{
			return startOf(project, event) +
				std::llround(eventSamples(project, event, static_cast<double>(frame - event.offset)));
		}

		time::Samples clipLength(const Project& project, const Clip& clip)
		{
			return project.media.at(clip.media).length;
		}

		// Where the clip's frame plays through its warp markers alone, in clip frames.
		time::Samples markedFrame(const std::vector<WarpMarker>& markers, time::Samples length, time::Samples frame)
		{
			std::vector<dsp::WarpPoint> map{{0.0, 0.0}};
			for (const WarpMarker& marker : markers)
				if (marker.position < length && marker.target < length)
					map.push_back({static_cast<double>(marker.position), static_cast<double>(marker.target)});
			map.push_back({static_cast<double>(length), static_cast<double>(length)});
			return std::llround(dsp::warpTarget(map, static_cast<double>(frame)));
		}

		// Adds a warp marker at the clip frame where it changes nothing, unless there is one or it wouldn't fit
		// between its neighbors. Returns whether it added one.
		bool addNeutralMarker(Clip& clip, time::Samples length, time::Samples frame)
		{
			std::vector<WarpMarker>& markers = clip.warp.markers;
			if (frame <= 0 || frame >= length)
				return false;
			const auto next = std::find_if(
				markers.begin(), markers.end(), [frame](const WarpMarker& marker) { return marker.position >= frame; });
			if (next != markers.end() && next->position == frame)
				return false;
			const time::Samples target = markedFrame(markers, length, frame);
			const time::Samples before = next == markers.begin() ? 0 : std::prev(next)->target;
			const time::Samples after = next == markers.end() ? length : next->target;
			if (target <= before || target >= after)
				return false;
			markers.insert(next, {frame, target});
			return true;
		}

		// Quantizes the warp markers inside one event (positions after its offset and before its end).
		void quantizeMarkers(Project& project, const AudioEvent& event, const midi::QuantizeGrid& grid)
		{
			Clip& clip = project.clips.at(event.clip);
			const time::Samples length = clipLength(project, clip);
			const time::Samples end = std::min(event.offset + event.length, length);
			const auto inside = [&event, end](const WarpMarker& marker)
			{ return marker.position > event.offset && marker.position < end; };
			std::vector<WarpMarker>& markers = clip.warp.markers;
			if (std::none_of(markers.begin(), markers.end(), inside))
			{
				const std::vector<bool> active = activeHitpoints(clip);
				for (size_t i = 0; i < clip.hitpoints.size(); ++i)
					if (active[i] && clip.hitpoints[i].position > event.offset && clip.hitpoints[i].position < end)
						addNeutralMarker(clip, length, clip.hitpoints[i].position);
			}
			if (std::none_of(markers.begin(), markers.end(), inside))
				return;
			// The event's start keeps playing the frame it plays, so that each marker inside plays where its own
			// target puts it.
			addNeutralMarker(clip, length, event.offset);

			// Where each marker plays and where quantizing takes it, in project samples after the event's start.
			struct Move
			{
				size_t index = 0;
				double from = 0.0;
				std::optional<double> to;
				time::Ticks grid = 0;
				time::Ticks distance = 0;
			};
			const time::Samples start = startOf(project, event);
			std::vector<Move> moves;
			for (size_t i = 0; i < markers.size(); ++i)
			{
				if (!inside(markers[i]))
					continue;
				Move move;
				move.index = i;
				move.from = warpedOffset(project, event, static_cast<double>(markers[i].position));
				const time::Ticks tick = project.tempo.toTicks(start + std::llround(move.from), project.sampleRate);
				move.grid = grid.nearest(grid.preQuantized(tick)).position;
				move.distance = std::llabs(move.grid - tick);
				if (const auto quantized = midi::quantizedPosition(grid, tick, static_cast<uint64_t>(tick)))
					move.to = static_cast<double>(project.tempo.toSamples(*quantized, project.sampleRate) - start);
				moves.push_back(move);
			}
			// Of the markers nearest to the same grid position, the nearest goes there (p. 370).
			for (Move& move : moves)
				for (const Move& other : moves)
					if (&other != &move && other.grid == move.grid &&
						(other.distance < move.distance ||
							(other.distance == move.distance && other.index < move.index)))
						move.to.reset();
			// The others keep their places relative to the markers around them that move, and the event's start.
			std::vector<std::pair<double, double>> anchors{{0.0, 0.0}};
			for (const Move& move : moves)
				if (move.to)
					anchors.emplace_back(move.from, *move.to);
			for (Move& move : moves)
			{
				if (move.to)
					continue;
				const auto after = std::find_if(anchors.begin(), anchors.end(),
					[&move](const std::pair<double, double>& anchor) { return anchor.first > move.from; });
				const auto& [from, to] = *std::prev(after);
				if (after == anchors.end())
					move.to = move.from + (to - from);
				else
					move.to = to + ((move.from - from) * (after->second - to) / (after->first - from));
			}
			// Each marker's target: the clip frame from which the warp plays it there, between the markers around it.
			for (const Move& move : moves)
			{
				WarpMarker& marker = markers[move.index];
				const time::Samples lowest = move.index == 0 ? 1 : markers[move.index - 1].target + 1;
				time::Samples low = lowest;
				const auto next = std::find_if(markers.begin() + static_cast<std::ptrdiff_t>(move.index) + 1,
					markers.end(), [end](const WarpMarker& each) { return each.position >= end; });
				time::Samples high = next == markers.end() ? length - 1 : next->target - 1;
				if (low > high)
					continue;
				const auto playsAt = [&](time::Samples target)
				{
					marker.target = target;
					return warpedOffset(project, event, static_cast<double>(marker.position));
				};
				while (low < high)
				{
					const time::Samples middle = low + ((high - low) / 2);
					if (playsAt(middle) < *move.to)
						low = middle + 1;
					else
						high = middle;
				}
				// The first target that plays it there or later, or the one before if that comes nearer.
				const bool earlier =
					low > lowest && std::abs(playsAt(low - 1) - *move.to) <= std::abs(playsAt(low) - *move.to);
				marker.target = earlier ? low - 1 : low;
			}
		}

		std::vector<ObjectId> eventList(const Project& project, const Json& ids)
		{
			std::vector<ObjectId> events;
			for (const Json& id : ids)
			{
				const auto event = id.get<ObjectId>();
				require(project.audioEvents.count(event) != 0, "no audio event " + std::to_string(event));
				if (std::find(events.begin(), events.end(), event) == events.end())
					events.push_back(event);
			}
			return events;
		}

		// Where several tracks' events are sliced or get warp markers (pp. 378-380): at the hitpoints that count of
		// the track with the highest priority (0 leaves a track out, default 1), then at those of the others that have
		// none of a higher priority, or of the same priority earlier, within range. In project samples.
		std::vector<time::Samples> groupPositions(
			const Project& project, const std::vector<ObjectId>& events, const Json& parameters)
		{
			std::map<ObjectId, int> priorities;
			for (const Json& item : parameters.value("priorities", Json::array()))
			{
				const int priority = item.at("priority").get<int>();
				require(priority >= 0 && priority <= 5, "priority must be 0 to 5");
				priorities[item.at("track").get<ObjectId>()] = priority;
			}
			const double range = parameters.value("range", 20.0);
			require(range >= 0.0 && range <= 1000.0, "range must be 0 to 1000 ms");
			const auto distance = std::llround(range * project.sampleRate / 1000.0);

			std::vector<std::pair<int, time::Samples>> candidates;
			for (const ObjectId id : events)
			{
				const AudioEvent& event = project.audioEvents.at(id);
				const auto found = priorities.find(event.track);
				const int priority = found == priorities.end() ? 1 : found->second;
				if (priority == 0)
					continue;
				const Clip& clip = project.clips.at(event.clip);
				const std::vector<bool> active = activeHitpoints(clip);
				for (size_t i = 0; i < clip.hitpoints.size(); ++i)
				{
					const time::Samples frame = clip.hitpoints[i].position;
					if (active[i] && frame >= event.offset && frame < event.offset + event.length)
						candidates.emplace_back(-priority, sampleOf(project, event, frame));
				}
			}
			std::sort(candidates.begin(), candidates.end());
			std::vector<time::Samples> positions;
			for (const auto& [priority, at] : candidates)
				if (std::none_of(positions.begin(), positions.end(),
						[at = at, distance](time::Samples taken) { return std::llabs(taken - at) <= distance; }))
					positions.push_back(at);
			std::sort(positions.begin(), positions.end());
			require(!positions.empty(),
				"the events have no hitpoints that count on tracks with a priority above 0: detect them first");
			return positions;
		}

		Json emitEvent(const AudioEvent& event, CommandContext& context)
		{
			context.emit("audioEvent.changed", {{"event", toJson(event)}});
			return event.id;
		}

		// quantize.slice: cuts every event offset before each position and gives the slices snap points there.
		Json slice(ProjectModel& model, Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const std::vector<ObjectId> events = eventList(project, parameters.at("events"));
			const std::vector<time::Samples> positions = groupPositions(project, events, parameters);
			const double offsetMs = parameters.value("offset", 0.0);
			require(offsetMs >= 0.0 && offsetMs <= 1000.0, "offset must be 0 to 1000 ms");
			const auto offset = std::llround(offsetMs * project.sampleRate / 1000.0);
			Json made = Json::array();
			for (const ObjectId id : events)
			{
				const AudioEvent& event = project.audioEvents.at(id);
				Json pieces = Json::array({id});
				if (event.lock)
				{
					made.push_back(id);
					continue;
				}
				const EventSpan span = eventSpan(project, event);
				Json cuts = Json::array();
				for (const time::Samples position : positions)
					if (position - offset > span.start && position - offset < span.end)
						cuts.push_back(position - offset);
				if (!cuts.empty())
				{
					const Json added =
						model.runEdit("event.split", project, {{"events", {id}}, {"positions", cuts}}, context)
							.at("events");
					pieces.insert(pieces.end(), added.begin(), added.end());
				}
				// Each slice snaps at its hitpoint, so that quantizing moves the slices of every track at the same
				// place alike.
				for (const Json& piece : pieces)
				{
					AudioEvent& each = project.audioEvents.at(piece.get<ObjectId>());
					const EventSpan at = eventSpan(project, each);
					const auto own = std::find_if(positions.begin(), positions.end(),
						[&at](time::Samples position) { return position >= at.start && position < at.end; });
					if (own != positions.end() && *own - offset <= at.start)
					{
						each.snapPoint = each.offset +
							std::llround(eventFrames(project, each, static_cast<double>(*own - at.start)));
						emitEvent(each, context);
					}
					made.push_back(piece);
				}
			}
			return {{"events", std::move(made)}, {"positions", positions}};
		}

		// quantize.createWarpMarkers: a warp marker at each position in every event's clip, changing nothing yet.
		Json createWarpMarkers(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const std::vector<ObjectId> events = eventList(project, parameters.at("events"));
			const std::vector<time::Samples> positions = groupPositions(project, events, parameters);
			std::vector<ObjectId> clips;
			for (const ObjectId id : events)
			{
				const AudioEvent& event = project.audioEvents.at(id);
				Clip& clip = project.clips.at(event.clip);
				const EventSpan span = eventSpan(project, event);
				bool added = false;
				for (const time::Samples position : positions)
					if (position >= span.start && position < span.end)
						added = addNeutralMarker(clip, clipLength(project, clip),
									event.offset +
										std::llround(
											eventFrames(project, event, static_cast<double>(position - span.start)))) ||
							added;
				if (added && std::find(clips.begin(), clips.end(), clip.id) == clips.end())
					clips.push_back(clip.id);
			}
			for (const ObjectId id : clips)
				context.emit("clip.changed", {{"clip", toJson(project.clips.at(id))}});
			return {{"clips", clips}, {"positions", positions}};
		}

		// quantize.crossfade (p. 379): on each track, removes the overlaps and closes the gaps between the events
		// one after the other, then crossfades them over length.
		Json crossfade(ProjectModel& model, Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const std::vector<ObjectId> events = eventList(project, parameters.at("events"));
			const double lengthMs = parameters.value("length", 10.0);
			require(lengthMs >= 0.0 && lengthMs <= 500.0, "length must be 0 to 500 ms");
			const double crossfadeSamples = lengthMs * project.sampleRate / 1000.0;
			std::map<ObjectId, std::vector<ObjectId>> tracks;
			for (const ObjectId id : events)
				tracks[project.audioEvents.at(id).track].push_back(id);
			std::vector<ObjectId> changed;
			const auto note = [&changed](ObjectId id)
			{
				if (std::find(changed.begin(), changed.end(), id) == changed.end())
					changed.push_back(id);
			};
			for (auto& [track, ids] : tracks)
			{
				std::sort(ids.begin(), ids.end(),
					[&project](ObjectId a, ObjectId b)
					{
						return std::make_pair(startOf(project, project.audioEvents.at(a)), a) <
							std::make_pair(startOf(project, project.audioEvents.at(b)), b);
					});
				for (size_t i = 1; i < ids.size(); ++i)
				{
					AudioEvent& a = project.audioEvents.at(ids[i - 1]);
					AudioEvent& b = project.audioEvents.at(ids[i]);
					const EventSpan first = eventSpan(project, a);
					const EventSpan second = eventSpan(project, b);
					if (a.lock || b.lock || second.start <= first.start)
						continue;
					if (first.end > second.start)
					{
						a.length =
							std::llround(eventFrames(project, a, static_cast<double>(second.start - first.start)));
						note(a.id);
					}
					else if (first.end < second.start)
					{
						const time::Samples frames = std::min<time::Samples>(
							b.offset, std::llround(static_cast<double>(second.start - first.end) / second.ratio));
						const time::Samples start =
							second.start - std::llround(static_cast<double>(frames) * second.ratio);
						b.offset -= frames;
						b.length += frames;
						b.position = time::rebase(
							{time::TimeBase::Linear, start}, b.position.base, project.tempo, project.sampleRate);
						note(b.id);
					}
					const Clip& clip = project.clips.at(a.clip);
					const time::Samples extension =
						std::min<time::Samples>(std::llround(crossfadeSamples / first.ratio),
							clipLength(project, clip) - (a.offset + a.length));
					if (extension <= 0)
						continue;
					a.length += extension;
					// Only into an event that goes on after it.
					if (eventSpan(project, a).end >= eventSpan(project, b).end)
					{
						a.length -= extension;
						continue;
					}
					note(a.id);
					model.runEdit("crossfade.set", project, {{"from", a.id}, {"to", b.id}}, context);
				}
			}
			for (const ObjectId id : changed)
				emitEvent(project.audioEvents.at(id), context);
			return {{"events", changed}};
		}
	}

	bool quantizeEventStart(const Project& project, AudioEvent& event, const midi::QuantizeGrid& grid)
	{
		if (event.lock)
			return false;
		const bool still = event.quantizeOrigin && event.quantizeOrigin->quantized == event.position;
		const time::Position original = still ? event.quantizeOrigin->original : event.position;
		AudioEvent from = event;
		from.position = grid.settings().soft ? event.position : original;
		const time::Samples snap = snapOffset(project, from);
		const time::Samples at = startOf(project, from) + snap;
		const bool musical = from.position.base == time::TimeBase::Musical;
		const time::Ticks tick =
			musical && snap == 0 ? from.position.value : project.tempo.toTicks(at, project.sampleRate);
		time::Position result = from.position;
		if (const auto moved = midi::quantizedPosition(grid, tick, static_cast<uint64_t>(tick)))
		{
			const int64_t delta = musical ? *moved - tick : project.tempo.toSamples(*moved, project.sampleRate) - at;
			result.value = std::max<int64_t>(from.position.value + delta, 0);
		}
		const time::Position before = event.position;
		event.position = result;
		if (result == original)
			event.quantizeOrigin.reset();
		else
			event.quantizeOrigin = EventQuantizeOrigin{original, result};
		return event.position != before;
	}

	bool resetEventQuantize(AudioEvent& event)
	{
		if (!event.quantizeOrigin || event.quantizeOrigin->quantized != event.position)
			return false;
		event.position = event.quantizeOrigin->original;
		event.quantizeOrigin.reset();
		return true;
	}

	std::vector<ObjectId> warpQuantize(
		Project& project, const std::vector<ObjectId>& events, const midi::QuantizeGrid& grid)
	{
		// Each clip starts from its original markers once, so that its events don't undo each other's quantizing.
		std::map<ObjectId, ClipWarp> before;
		std::map<ObjectId, std::vector<WarpMarker>> originals;
		for (const ObjectId id : events)
		{
			const AudioEvent& event = project.audioEvents.at(id);
			if (event.lock || before.count(event.clip) != 0)
				continue;
			ClipWarp& warp = project.clips.at(event.clip).warp;
			before[event.clip] = warp;
			const bool still = warp.quantizeOrigin && warp.quantizeOrigin->quantized == warp.markers;
			originals[event.clip] = still ? warp.quantizeOrigin->original : warp.markers;
			if (!grid.settings().soft)
				warp.markers = originals[event.clip];
		}
		for (const ObjectId id : events)
		{
			const AudioEvent& event = project.audioEvents.at(id);
			if (!event.lock)
				quantizeMarkers(project, event, grid);
		}
		std::vector<ObjectId> changed;
		for (const auto& [id, warp] : before)
		{
			ClipWarp& now = project.clips.at(id).warp;
			if (now.markers == originals.at(id))
				now.quantizeOrigin.reset();
			else
				now.quantizeOrigin = WarpQuantizeOrigin{originals.at(id), now.markers};
			if (!(now == warp))
				changed.push_back(id);
		}
		return changed;
	}

	bool resetWarpQuantize(Clip& clip)
	{
		ClipWarp& warp = clip.warp;
		if (!warp.quantizeOrigin || warp.quantizeOrigin->quantized != warp.markers)
			return false;
		warp.markers = warp.quantizeOrigin->original;
		warp.quantizeOrigin.reset();
		return true;
	}

	void registerAudioQuantizeCommands(ProjectModel& model, Engine& engine)
	{
		const Json id = {{"type", "integer"}};
		const Json ids = {{"type", "array"}, {"items", id}};
		const Json number = {{"type", "number"}};
		const Json priorities = {{"type", "array"},
			{"items", objectSchema({{"track", id}, {"priority", {{"type", "integer"}}}}, {"track", "priority"})}};
		const std::string groupText =
			"The positions are the hitpoints that count (see clip.setHitpoints) of the events' clips: first those of "
			"the tracks with the highest priority in priorities ([{track, priority 0-5}], default 1; 0 leaves a "
			"track's hitpoints out), then those of the others with none of a higher priority, or of the same priority "
			"earlier, within range ms (default 20) of them. The audio on all tracks should start and end together.";
		model.addEdit(engine,
			{"quantize.slice", 1,
				"Slices audio events of several tracks at the same positions, so that quantizing them keeps them phase "
				"coherent (Slice Rules, reference manual pp. 370-371, 378-379). " +
					groupText +
					" Every event is cut offset ms (default 0) before each position inside it, and each slice that "
					"starts there snaps at it (its snap point), so that quantize.apply moves the slices of every track "
					"at a position alike. Locked events stay whole. Returns the events, the slices included, and the "
					"positions (project samples).",
				objectSchema(
					{{"events", ids}, {"priorities", priorities}, {"range", number}, {"offset", number}}, {"events"}),
				objectSchema({{"events", ids}, {"positions", ids}}, {"events", "positions"}), false},
			"Slice", [&model](Project& project, CommandContext& context) { return slice(model, project, context); });
		model.addEdit(engine,
			{"quantize.createWarpMarkers", 1,
				"Creates warp markers in the clips of audio events of several tracks at the same positions, for "
				"warp quantizing them with quantize.apply (Warp Marker Creation Rules, reference manual pp. "
				"371-372, 379-380). " +
					groupText +
					" Each marker goes where the clip frame plays now, so nothing changes until quantizing moves "
					"them. Returns the clips changed and the positions (project samples).",
				objectSchema({{"events", ids}, {"priorities", priorities}, {"range", number}}, {"events"}),
				objectSchema({{"clips", ids}, {"positions", ids}}, {"clips", "positions"}), false},
			"Create warp markers", createWarpMarkers);
		model.addEdit(engine,
			{"quantize.crossfade", 1,
				"Corrects the overlaps and gaps between quantized audio events, such as slices (the quantize panel's "
				"Crossfades section, reference manual p. 379): on each track, in order, an event that overlaps the "
				"next one ends where that one starts, and an event after a gap starts earlier, where the one before "
				"ends, as far as its clip has audio there. Then each event goes on length ms (default 10, up to 500) "
				"into the next one and crossfades into it (crossfade.set). Locked events stay. Returns the events "
				"changed.",
				objectSchema({{"events", ids}, {"length", number}}, {"events"}),
				objectSchema({{"events", ids}}, {"events"}), false},
			"Crossfade",
			[&model](Project& project, CommandContext& context) { return crossfade(model, project, context); });
	}
}
