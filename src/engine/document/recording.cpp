// SPDX-License-Identifier: GPL-3.0-or-later
// Audio and MIDI recording (#96, #97, reference manual pp. 344-345, 350-353): recording.add puts what a recording
// wrote into the project as one undo step, through the commands a client would send, and applies the audio and MIDI
// record modes to the events and parts the takes overlap. The engine runs it when a recording ends (Recorder in
// src/engine/audio/recorder.h).
#include "engine/audio/transport.h"
#include "engine/document/chordtrack.h"
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/midi/articulationmaps.h"
#include "engine/midi/drummaps.h"
#include "engine/time/signaturemap.h"
#include "engine/time/tempomap.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		using Run = std::function<Json(const char* command, const Json& parameters)>;

		constexpr int g_lastLane = 1000; // event.set's: the takes beyond it share it

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		struct Take
		{
			time::Samples position = 0; // project samples
			time::Samples offset = 0;   // in the file
			time::Samples length = 0;
		};

		// The bar line at or before tick, and with up the one at or after it.
		time::Ticks barLine(const time::SignatureMap& bars, time::Ticks tick, bool up)
		{
			const int64_t bar = bars.toBarsBeats(tick).bar;
			const time::Ticks start = bars.barStart(bar);
			return up && start < tick ? bars.barStart(bar + 1) : start;
		}

		// A MIDI or audio part's start and end in the project's samples.
		template <typename Part>
		std::pair<time::Samples, time::Samples> partSpan(const Project& project, const Part& part)
		{
			const time::Position end{part.position.base, part.position.value + part.length};
			return {time::toSamples(part.position, project.tempo, project.sampleRate),
				time::toSamples(end, project.tempo, project.sampleRate)};
		}

		// A MIDI track's take: where it was recorded and what was recorded in it, positions in project samples.
		struct MidiTake
		{
			time::Samples position = 0;
			time::Samples length = 0;
			std::vector<Json> notes;
			std::vector<Json> controllers;

			bool empty() const { return notes.empty() && controllers.empty(); }
			time::Samples end() const { return position + length; }
		};

		// The lane after the highest one that the track's events or parts in [start, end) are on, or 0 if there are
		// none: where a cycle recording's takes go. The lane of an audio event in an audio part is one of the part's,
		// not of the track: the part's lane counts instead.
		int firstFreeLane(const Project& project, ObjectId track, time::Samples start, time::Samples end)
		{
			int lane = 0;
			for (const auto& [id, event] : project.audioEvents)
			{
				const EventSpan span = eventSpan(project, event);
				if (event.track == track && event.part == 0 && span.start < end && span.end > start)
					lane = std::max(lane, event.lane + 1);
			}
			const auto partLanes = [&](const auto& parts)
			{
				for (const auto& [id, part] : parts)
				{
					const auto [partStart, partEnd] = partSpan(project, part);
					if (part.track == track && partStart < end && partEnd > start)
						lane = std::max(lane, part.lane + 1);
				}
			};
			partLanes(project.midiParts);
			partLanes(project.audioParts);
			return lane;
		}

		// Adds a MIDI track's recorded notes and controller events (positions and lengths in project samples),
		// recorded from start to takesEnd, by the MIDI record mode: a part of their own over that range (to whole
		// bars with snapToBars), or, merging or replacing, into the first unlocked part they overlap, which grows to
		// hold them. Replacing first removes the notes that start and the controller events that lie in that range
		// from every unlocked part it overlaps. With the project's auto quantize, the notes are quantized. Returns the
		// part.
		ObjectId addMidi(Project& project, const Run& run, const Track& track, time::Samples start,
			time::Samples takesEnd, const Json& notes, const Json& controllers, const std::string& mode, bool snap)
		{
			const auto& tempo = project.tempo;
			const double rate = project.sampleRate;
			time::Samples end = takesEnd;
			for (const Json& note : notes)
				end = std::max(end, note.at("position").get<time::Samples>() + note.at("length").get<time::Samples>());
			require(start < end, "a MIDI track's recording needs a take");

			// The takes' range picks the overlapped parts and what replace removes; snapping to bars only lengthens
			// the part the recording adds or grows (reference manual pp. 352, 1616).
			const time::Ticks takesStartTick = tempo.toTicks(start, rate);
			const time::Ticks takesEndTick = tempo.toTicks(takesEnd, rate);
			time::Ticks startTick = takesStartTick;
			time::Ticks endTick = tempo.toTicks(end, rate);
			if (snap)
			{
				startTick = barLine(project.signatures, startTick, false);
				endTick = barLine(project.signatures, endTick, true);
			}
			const bool linear = track.timeBase == time::TimeBase::Linear;
			const auto inBase = [&](time::Ticks tick)
			{ return linear ? tempo.toSamples(tick, rate) : tick; }; // in the track's time base

			std::vector<ObjectId> overlapping; // unlocked parts the recording overlaps, by position
			if (mode != "newParts")
			{
				const time::Samples from = start;
				const time::Samples to = takesEnd;
				std::vector<std::pair<time::Samples, ObjectId>> found;
				for (const auto& [id, part] : project.midiParts)
				{
					const auto [partStart, partEnd] = partSpan(project, part);
					if (part.track == track.id && !part.lock && partStart < to && partEnd > from)
						found.emplace_back(partStart, id);
				}
				std::sort(found.begin(), found.end());
				for (const auto& [position, id] : found)
					overlapping.push_back(id);
			}
			if (mode == "replace")
			{
				for (const ObjectId id : overlapping)
				{
					const MidiPart& part = project.midiParts.at(id);
					const time::Ticks partTick = time::toTicks(part.position, tempo, rate);
					const auto inside = [&](time::Ticks position)
					{
						const time::Ticks at = partTick + position;
						return at >= takesStartTick && at < takesEndTick;
					};
					Json removedNotes = Json::array();
					for (const auto& [noteId, note] : part.notes)
						if (inside(note.position))
							removedNotes.push_back(noteId);
					Json removedControllers = Json::array();
					for (const auto& [controllerId, controller] : part.controllers)
						if (inside(controller.position))
							removedControllers.push_back(controllerId);
					if (!removedNotes.empty())
						run("note.remove", {{"part", id}, {"notes", removedNotes}});
					if (!removedControllers.empty())
						run("controller.remove", {{"part", id}, {"controllers", removedControllers}});
				}
			}

			ObjectId part = 0;
			if (overlapping.empty())
			{
				const time::Ticks from = inBase(startTick);
				part =
					run("midiPart.add", {{"track", track.id}, {"position", from}, {"length", inBase(endTick) - from}})
						.at("part")
						.at("id")
						.get<ObjectId>();
				// Recorded parts follow global transpose with the project root key, or with transpose events are
				// independent of it (#168, reference manual p. 420).
				const bool transposed = !project.transpose.events.empty();
				if (transposed || project.rootKey >= 0)
					run("midiPart.set",
						{{"part", part}, {"rootKey", transposed ? std::string() : rootKeyName(project.rootKey)},
							{"globalTranspose", transposed ? "independent" : "follow"}});
			}
			else
			{
				part = overlapping.front();
				const MidiPart& target = project.midiParts.at(part);
				const time::Ticks partStart = time::toTicks(target.position, tempo, rate);
				const time::Ticks partEnd =
					time::toTicks({target.position.base, target.position.value + target.length}, tempo, rate);
				// Normal sizing keeps the part's events where they are. It grows only where the recording reaches
				// past it, then to the bar line with snapToBars.
				if (takesStartTick < partStart)
					run("event.resize",
						{{"events", Json::array({part})}, {"edge", "start"}, {"ticks", startTick - partStart}});
				if (tempo.toTicks(end, rate) > partEnd)
					run("event.resize",
						{{"events", Json::array({part})}, {"edge", "end"}, {"ticks", endTick - partEnd}});
			}

			// Positions from the part's start, in ticks.
			const time::Ticks origin = time::toTicks(project.midiParts.at(part).position, tempo, rate);
			const auto ticksAt = [&](const Json& item)
			{ return tempo.toTicks(item.at("position").get<time::Samples>(), rate); };
			if (!notes.empty())
			{
				Json added = Json::array();
				for (const Json& note : notes)
				{
					const time::Ticks at = ticksAt(note);
					const time::Ticks noteEnd = tempo.toTicks(
						note.at("position").get<time::Samples>() + note.at("length").get<time::Samples>(), rate);
					Json item = note;
					item["position"] = at - origin;
					item["length"] = std::max<time::Ticks>(noteEnd - at, 1);
					// Expression points from the note's start.
					if (item.contains("expressions"))
						for (Json& point : item.at("expressions"))
							point["position"] = std::max<time::Ticks>(ticksAt(point) - at, 0);
					added.push_back(std::move(item));
				}
				const Json result = run("note.add", {{"part", part}, {"notes", added}});
				if (project.autoQuantize)
				{
					Json ids = Json::array();
					for (const Json& note : result.at("notes"))
						ids.push_back(note.at("id"));
					run("quantize.apply", {{"part", part}, {"notes", ids}});
				}
			}
			if (!controllers.empty())
			{
				Json added = Json::array();
				for (const Json& controller : controllers)
				{
					Json item = controller;
					item["position"] = ticksAt(controller) - origin;
					added.push_back(std::move(item));
				}
				run("controller.add", {{"part", part}, {"controllers", added}});
			}
			return part;
		}

		// Drops what earlier takes recorded from where a later take's input starts to that take's end: the
		// Overwrite cycle mode.
		void overwrite(std::vector<MidiTake>& takes)
		{
			const auto at = [](const Json& item) { return item.at("position").get<time::Samples>(); };
			for (size_t later = 1; later < takes.size(); ++later)
			{
				const time::Samples end = takes[later].end();
				time::Samples from = end;
				for (const std::vector<Json>* items : {&takes[later].notes, &takes[later].controllers})
					for (const Json& item : *items)
						from = std::min(from, at(item));
				for (size_t earlier = 0; earlier < later; ++earlier)
					for (std::vector<Json>* items : {&takes[earlier].notes, &takes[earlier].controllers})
					{
						std::vector<Json> kept;
						for (Json& item : *items)
							if (at(item) < from || at(item) >= end)
								kept.push_back(std::move(item));
						*items = std::move(kept);
					}
			}
		}

		// The take the Keep Last cycle mode keeps: the last complete one that recorded anything, or, if none did, the
		// last one that did. All but the last take ended with a jump back to the left locator; the last one is
		// complete if it ends where an earlier one ends, at the right locator, and not where recording stopped.
		std::optional<size_t> keptTake(const std::vector<MidiTake>& takes)
		{
			const time::Samples lastEnd = takes.back().end();
			const bool lastComplete = std::any_of(
				takes.begin(), takes.end() - 1, [lastEnd](const MidiTake& take) { return take.end() == lastEnd; });
			std::optional<size_t> found;
			for (size_t index = takes.size(); index-- > 0;)
				if (!takes[index].empty())
				{
					if (index + 1 < takes.size() || lastComplete)
						return index;
					found = found.value_or(index);
				}
			return found;
		}

		// Adds what a MIDI or instrument track recorded, by the MIDI cycle record mode (reference manual pp.
		// 352-353): mix puts every take into one part, by the MIDI record mode (addMidi); overwrite and keepLast do
		// too, after overwrite dropped what input in a later take overwrote and keepLast kept only one take
		// (keptTake); stacked and mixStacked add a new part per take that recorded anything, each on a lane of its
		// own (firstFreeLane), and stacked mutes all of them but the last. The cycle record mode applies only to a
		// cycle recording of several takes: the takes of a linear one, which a locate starts, all stay. Returns the
		// parts.
		Json addMidiRecording(Project& project, const Run& run, const Track& track, const Json& entry,
			const std::string& mode, std::string cycleMode, bool snap)
		{
			std::vector<MidiTake> takes;
			for (const Json& take : entry.at("takes"))
			{
				MidiTake next;
				next.position = take.at("position").get<time::Samples>();
				next.length = take.at("length").get<time::Samples>();
				require(next.position >= 0 && next.length > 0, "a take must have a position and a length");
				takes.push_back(std::move(next));
			}
			require(!takes.empty(), "a MIDI track's recording needs a take");
			const auto distribute = [&](const Json& items, std::vector<Json> MidiTake::* into)
			{
				for (Json item : items)
				{
					const auto index = item.value("take", size_t{0});
					require(index < takes.size(), "a note or controller event names a take that isn't there");
					item.erase("take");
					// Notes played into a track with a drum map take the pitch of the sound whose I-note they are
					// (#129, reference manual p. 1239).
					if (const Json pitch = item.value("pitch", Json()); into == &MidiTake::notes &&
						pitch.is_number_integer() && pitch.get<int64_t>() >= 0 && pitch.get<int64_t>() <= 127)
						item["pitch"] = midi::inputPitch(project, track.id, pitch.get<uint8_t>());
					(takes[index].*into).push_back(std::move(item));
				}
			};
			// Remote triggers select sound slots of the track's articulation map instead of playing (#175, reference
			// manual p. 1263): they become directions and the notes' attributes.
			Json played = entry.value("notes", Json::array());
			Json playedControllers = entry.value("controllers", Json::array());
			if (const ArticulationMap* map = midi::articulationMapOf(project, track.id))
				midi::applyRemoteTriggers(*map, played, playedControllers);
			distribute(played, &MidiTake::notes);
			distribute(playedControllers, &MidiTake::controllers);
			if (takes.size() < 2)
				cycleMode = "mix"; // mixing a single take adds it by the MIDI record mode alone

			if (cycleMode == "stacked" || cycleMode == "mixStacked")
			{
				std::vector<const MidiTake*> recorded;
				time::Samples from = std::numeric_limits<time::Samples>::max();
				time::Samples to = std::numeric_limits<time::Samples>::min();
				for (const MidiTake& take : takes)
					if (!take.empty())
					{
						recorded.push_back(&take);
						from = std::min(from, take.position);
						to = std::max(to, take.end());
					}
				if (!recorded.empty())
				{
					if (snap) // the parts take whole bars, and so do the lanes they need
					{
						const auto& tempo = project.tempo;
						const double rate = project.sampleRate;
						from = tempo.toSamples(barLine(project.signatures, tempo.toTicks(from, rate), false), rate);
						to = tempo.toSamples(barLine(project.signatures, tempo.toTicks(to, rate), true), rate);
					}
					const int lane = firstFreeLane(project, track.id, from, to);
					Json parts = Json::array();
					for (size_t index = 0; index < recorded.size(); ++index)
					{
						const MidiTake& take = *recorded[index];
						const ObjectId part = addMidi(project, run, track, take.position, take.end(), Json(take.notes),
							Json(take.controllers), "newParts", snap);
						run("event.set",
							{{"events", Json::array({part})},
								{"lane", std::min(lane + static_cast<int>(index), g_lastLane)},
								{"mute", cycleMode == "stacked" && index + 1 < recorded.size()}});
						parts.push_back(part);
					}
					return parts;
				}
			}
			if (cycleMode == "overwrite")
				overwrite(takes);
			if (cycleMode == "keepLast")
				if (const std::optional<size_t> kept = keptTake(takes))
					takes = {takes[*kept]};

			time::Samples start = std::numeric_limits<time::Samples>::max();
			time::Samples end = std::numeric_limits<time::Samples>::min();
			std::vector<Json> notes;
			std::vector<Json> controllers;
			for (const MidiTake& take : takes)
			{
				start = std::min(start, take.position);
				end = std::max(end, take.end());
				notes.insert(notes.end(), take.notes.begin(), take.notes.end());
				controllers.insert(controllers.end(), take.controllers.begin(), take.controllers.end());
			}
			return Json::array({addMidi(project, run, track, start, end, Json(notes), Json(controllers), mode, snap)});
		}
	}

	void ProjectModel::registerRecordingCommands(Engine& engine)
	{
		const Json integer = {{"type", "integer"}};
		const Json number = {{"type", "number"}};
		const Json ids = {{"type", "array"}, {"items", integer}};
		const Json take = {{"type", "object"}, {"additionalProperties", false},
			{"properties",
				{{"position", integer}, {"offset", integer}, {"length", integer},
					{"continues", {{"type", "boolean"}}}}},
			{"required", {"position", "length"}}};
		const Json takeIndex = {{"type", "integer"}, {"minimum", 0}};
		const Json note = {{"type", "object"}, {"additionalProperties", false},
			{"properties",
				{{"position", integer}, {"length", integer}, {"pitch", integer}, {"velocity", number},
					{"releaseVelocity", number}, {"channel", integer}, {"take", takeIndex},
					{"expressions", expressionsSchema()}}},
			{"required", {"position", "length", "pitch", "velocity"}}};
		const Json controller = {{"type", "object"}, {"properties", {{"take", takeIndex}}}, {"required", {"position"}}};
		const Json track = {{"type", "object"}, {"additionalProperties", false},
			{"properties",
				{{"track", integer}, {"path", {{"type", "string"}}}, {"takes", {{"type", "array"}, {"items", take}}},
					{"notes", {{"type", "array"}, {"items", note}}},
					{"controllers", {{"type", "array"}, {"items", controller}}},
					{"chordsOnly", {{"type", "boolean"}}}}},
			{"required", {"track", "takes"}}};

		add(engine,
			{"recording.add", 1,
				"Adds what a recording wrote, as the engine does when a recording ends. Per audio track its file "
				"(path) as a media file and clip, and an audio event per take. A take is a run of the file (offset "
				"and length in its frames) recorded at position (project samples), such as a cycle lap; a file may "
				"start with audio pre-record before its first take. A track may come with several files, as a "
				"recording split at 4 GB "
				"makes: they are one recording, whose takes mode and the lanes apply to together. mode, the audio "
				"record mode (transport.set), says "
				"what happens to the track's unlocked events where the takes overlap them: keepHistory (the default) "
				"keeps them, cycleHistoryReplace cuts them out and keeps every take, replace cuts out what each take "
				"overlaps, also of the takes before it. With keepHistory and cycleHistoryReplace, the events of the "
				"takes that overlap another take go onto lanes of their own (event.set's lane), one per take from the "
				"lane after the highest one that the track's events where they lie are on, up to lane 1000; the last "
				"take plays where they overlap. With cycle, the clip gets a region per take (clip.addRegion), Take 1 "
				"on, numbered across the track's files. A take with continues goes on from the last take of the "
				"track's file before, as where a split at 4 GB cut a cycle lap: the two are one lap, on one lane and "
				"with one region number. Per MIDI or instrument track the takes (position and length) "
				"and what was recorded, all in project samples: notes as note.add takes them and controllers as "
				"controller.add does, with positions and lengths in project samples (a note's expression points' "
				"too), and each with take, the index "
				"of the take it was recorded in (0 by default). They go into a MIDI part from "
				"the first take's start to the last one's end, whole bars with snapToBars, by midiMode, the MIDI "
				"record mode: newParts (the default) adds a part of their own; merge adds them to the first unlocked "
				"part they overlap, which grows to hold them; replace does too, after it removed the notes that "
				"start and the controller events that lie in that range from the unlocked parts it overlaps. "
				"cycleMode, the MIDI cycle record mode, says what becomes of the takes: mix (the default) puts them "
				"all into the part; overwrite first drops what earlier takes recorded from where a take's first note "
				"or controller event lies to that take's end; keepLast keeps only the last complete take that "
				"recorded anything (the last take is complete if it ends where an earlier one does), or the last that "
				"recorded anything if none is complete; stacked adds a new part per take that recorded anything, "
				"each on a lane of its own as for audio, after those of the parts in its whole bars with snapToBars, "
				"and mutes all of them but the last; mixStacked does too "
				"without muting. cycleMode applies only with cycle, which the engine gives a recording in cycle mode "
				"(a take that starts at the left locator where the one before ended at the right one): without it, "
				"and for a single take, the takes go in by midiMode alone, as do those that a locate starts during a "
				"linear recording. With the project's auto quantize (quantize.set) the recorded notes are quantized. "
				"On a track with an articulation map (track.set articulationMap), the remote triggers of its sound "
				"slots aren't recorded: the notes whose pitch is a slot's trigger (remoteTrigger keySwitch) or the "
				"program changes whose program is one (programChange) select their slot instead, as directions "
				"(articulation controller events) where they come and as the slot's attributes on the notes that "
				"start while it is selected; without latch the default slot comes back where the key is released. "
				"With the chord track record-enabled (chord.setTrack recordEnable, reference manual p. 1345), the "
				"chords that each take's notes make replace the chord events over that take, take after take; a MIDI "
				"or instrument track with chordsOnly only monitored while the chord track recorded: its notes make "
				"chord events and no part. chordEvents false (midi.insertRetrospective) makes none. "
				"One undo step. "
				"Announces recording.added with the result: the media files, clips and events added and the MIDI "
				"parts recorded into. project, as project.revision reports it, is the project the recording was made "
				"in: if another project is open by now, nothing is added and recording.failed is announced. A track "
				"that is gone is skipped and announced with recording.failed; the others are added. A recording that "
				"adds nothing leaves the project as it was, not modified, and records no undo step.",
				{{"type", "object"}, {"additionalProperties", false},
					{"properties",
						{{"project", integer}, {"mode", {{"type", "string"}, {"enum", g_audioRecordModes}}},
							{"midiMode", {{"type", "string"}, {"enum", g_midiRecordModes}}},
							{"cycleMode", {{"type", "string"}, {"enum", g_midiCycleRecordModes}}},
							{"cycle", {{"type", "boolean"}}}, {"snapToBars", {{"type", "boolean"}}},
							{"chordEvents", {{"type", "boolean"}}}, {"tracks", {{"type", "array"}, {"items", track}}}}},
					{"required", {"tracks"}}},
				{{"type", "object"}, {"properties", {{"media", ids}, {"clips", ids}, {"events", ids}, {"parts", ids}}},
					{"required", {"media", "clips", "events", "parts"}}},
				false},
			[this](Project& project, CommandContext& context) { return addRecording(project, context); });
	}

	Json ProjectModel::addRecording(Project& project, CommandContext& context)
	{
		const Json& parameters = context.parameters();
		const std::string mode = parameters.value("mode", std::string(g_audioRecordModes[0]));
		const std::string midiMode = parameters.value("midiMode", std::string(g_midiRecordModes[0]));
		const bool cycle = parameters.value("cycle", false);
		const std::string cycleMode =
			cycle ? parameters.value("cycleMode", std::string(g_midiCycleRecordModes[0])) : std::string("mix");
		const bool snap = parameters.value("snapToBars", false);
		const Run run = [this, &project, &context](const char* command, const Json& commandParameters)
		{ return runEdit(command, project, commandParameters, context); };

		// Cuts the track's unlocked audio events out of [start, end).
		const auto replace = [&project, &run](ObjectId track, time::Samples start, time::Samples end)
		{
			const auto overlapping = [&](bool inside)
			{
				Json found = Json::array();
				for (const auto& [id, event] : project.audioEvents)
				{
					const EventSpan span = eventSpan(project, event);
					if (event.track == track && !event.lock && span.start < end && span.end > start &&
						(!inside || (span.start >= start && span.end <= end)))
						found.push_back(id);
				}
				return found;
			};
			if (const Json cut = overlapping(false); !cut.empty())
				run("event.split", {{"events", cut}, {"positions", {start, end}}});
			if (const Json inside = overlapping(true); !inside.empty())
				run("event.remove", {{"events", inside}});
		};

		Json media = Json::array();
		Json clips = Json::array();
		Json events = Json::array();
		Json parts = Json::array();
		if (parameters.contains("project") && parameters.at("project").get<uint64_t>() != m_openedRevision)
		{
			context.emit("recording.failed",
				{{"reason", "the recording isn't added: another project was opened while it was recorded"}});
			context.markUnchanged(); // the project isn't modified (#546)
			return {{"media", media}, {"clips", clips}, {"events", events}, {"parts", parts}};
		}
		bool changed = false; // a track is there to add to, or chord events were recorded
		struct AudioTake
		{
			ObjectId track = 0;
			ObjectId clip = 0;
			Take take;
			time::Samples end = 0; // project samples
			size_t lap = 0;        // the track's laps before it: the pieces of a take that a split cut share it
		};
		std::vector<AudioTake> audio;         // the audio tracks' takes, added once every entry's file is
		std::map<ObjectId, size_t> lapCounts; // per audio track, how many laps its entries hold so far
		std::map<ObjectId, size_t> takeCount;
		for (const Json& entry : parameters.at("tracks"))
			takeCount[entry.at("track").get<ObjectId>()] += entry.at("takes").size();
		for (const Json& entry : parameters.at("tracks"))
		{
			const auto trackId = entry.at("track").get<ObjectId>();
			const auto found = project.trackById.find(trackId);
			if (found == project.trackById.end())
			{
				context.emit("recording.failed",
					{{"reason",
						"track " + std::to_string(trackId) + " was removed, so its recording " +
							entry.value("path", std::string("of MIDI")) + " isn't added"}});
				continue;
			}
			changed = true;
			const TrackType type = found->second.type;
			if (type == TrackType::Midi || type == TrackType::Instrument)
			{
				require(!entry.contains("path"),
					"track " + std::to_string(trackId) + " isn't an audio track: it records MIDI, not a file");
				if (entry.value("chordsOnly", false))
					continue; // its notes only make chord events, below
				for (const Json& part : addMidiRecording(project, run, found->second, entry, midiMode, cycleMode, snap))
					parts.push_back(part);
				continue;
			}
			require(type == TrackType::Audio, "track " + std::to_string(trackId) + " isn't an audio or MIDI track");
			require(entry.contains("path"), "audio track " + std::to_string(trackId) + " needs the path of its file");
			require(!entry.contains("notes") && !entry.contains("controllers"),
				"audio track " + std::to_string(trackId) + " records audio, not MIDI");
			const Json added = run("media.add", {{"path", entry.at("path")}}).at("media");
			const auto frames = added.at("length").get<time::Samples>();
			const double ratio = project.sampleRate / added.at("sampleRate").get<double>();
			std::vector<Take> takes;
			std::vector<size_t> takeLaps;
			for (const Json& item : entry.at("takes"))
			{
				size_t& lapCount = lapCounts[trackId];
				if (!item.value("continues", false) || lapCount == 0)
					++lapCount;
				takeLaps.push_back(lapCount - 1);
				const Take next{item.at("position").get<time::Samples>(), item.value("offset", time::Samples{0}),
					item.at("length").get<time::Samples>()};
				require(
					next.position >= 0 && next.offset >= 0 && next.length > 0 && next.offset + next.length <= frames,
					"a take must lie in its file, which has " + std::to_string(frames) + " frames");
				takes.push_back(next);
			}
			const ObjectId clip = run("clip.add", {{"media", added.at("id")}}).at("clip").at("id").get<ObjectId>();
			media.push_back(added.at("id"));
			clips.push_back(clip);
			// A cycle recording divides its clip into regions, one per take (reference manual p. 226), named by lap.
			if (cycle && takeCount[trackId] > 1)
				for (size_t index = 0; index < takes.size(); ++index)
					run("clip.addRegion",
						{{"clip", clip}, {"start", takes[index].offset},
							{"end", takes[index].offset + takes[index].length},
							{"name", "Take " + std::to_string(takeLaps[index] + 1)}});

			for (size_t index = 0; index < takes.size(); ++index)
				audio.push_back({trackId, clip, takes[index],
					takes[index].position +
						static_cast<time::Samples>(std::llround(static_cast<double>(takes[index].length) * ratio)),
					takeLaps[index]});
		}
		// All entries of a track are one recording: a recording split into several files (#580) sends one per file,
		// and the cuts and lanes of each file's takes must not touch the events of the others.
		if (mode == "cycleHistoryReplace")
			for (const AudioTake& each : audio) // before any take is added, so that every take stays
				replace(each.track, each.take.position, each.end);
		// Keeping the takes, a cycle recording puts them on lanes of their own (reference manual p. 226): those that
		// overlap another take of the track, as laps do; one that a locate during recording started may overlap none.
		std::vector<bool> stacked(audio.size(), false);
		if (mode != "replace")
			for (size_t i = 0; i < audio.size(); ++i)
				for (size_t j = i + 1; j < audio.size(); ++j)
					if (audio[i].track == audio[j].track && audio[i].lap != audio[j].lap &&
						audio[i].take.position < audio[j].end && audio[j].take.position < audio[i].end)
						stacked[i] = stacked[j] = true;
		for (size_t i = 0; i < audio.size(); ++i) // a lap's pieces all stack if one does
			for (size_t j = i + 1; j < audio.size(); ++j)
				if (audio[i].track == audio[j].track && audio[i].lap == audio[j].lap && (stacked[i] || stacked[j]))
					stacked[i] = stacked[j] = true;
		std::map<ObjectId, std::pair<time::Samples, time::Samples>> stackedSpans; // per track
		for (size_t i = 0; i < audio.size(); ++i)
			if (stacked[i])
			{
				auto& span =
					stackedSpans.try_emplace(audio[i].track, audio[i].take.position, audio[i].end).first->second;
				span = {std::min(span.first, audio[i].take.position), std::max(span.second, audio[i].end)};
			}
		std::map<ObjectId, int> lanes; // the next lane per track
		for (const auto& [track, span] : stackedSpans)
			lanes[track] = firstFreeLane(project, track, span.first, span.second);
		std::map<std::pair<ObjectId, size_t>, int> lapLanes; // per track and lap
		for (size_t i = 0; i < audio.size(); ++i)
		{
			const AudioTake& each = audio[i];
			if (mode == "replace")
				replace(each.track, each.take.position, each.end);
			const time::Position at = time::rebase({time::TimeBase::Linear, each.take.position},
				project.trackById.at(each.track).timeBase, project.tempo, project.sampleRate);
			const Json event = run("audioEvent.add",
				{{"track", each.track}, {"clip", each.clip}, {"position", at.value}, {"offset", each.take.offset},
					{"length", each.take.length}})
								   .at("event")
								   .at("id");
			// Recorded events follow global transpose with the project root key, or with transpose events are
			// independent of it, as recorded MIDI parts (#1368, reference manual p. 420).
			const bool transposed = !project.transpose.events.empty();
			if (transposed || project.rootKey >= 0)
				run("audioEvent.set",
					{{"event", event}, {"rootKey", transposed ? std::string() : rootKeyName(project.rootKey)},
						{"globalTranspose", transposed ? "independent" : "follow"}});
			if (stacked[i])
			{
				const auto [lane, inserted] = lapLanes.try_emplace({each.track, each.lap}, lanes[each.track]);
				if (inserted)
					++lanes[each.track];
				run("event.set", {{"events", Json::array({event})}, {"lane", std::min(lane->second, g_lastLane)}});
			}
			events.push_back(event);
		}
		// Recording chord events (#933, reference manual p. 1345): with the chord track record-enabled, the chords
		// that the notes recorded in each take make replace the chord events over that take, so that the laps of a
		// cycle recording don't mix and the last lap that recorded notes wins.
		if (project.chords.recordEnable && parameters.value("chordEvents", true))
		{
			struct Lap
			{
				std::vector<ChordNote> notes;
				time::Samples from = std::numeric_limits<time::Samples>::max();
				time::Samples to = std::numeric_limits<time::Samples>::min();
			};
			std::vector<Lap> laps;
			const auto ticks = [&project](time::Samples samples)
			{ return project.tempo.toTicks(samples, project.sampleRate); };
			for (const Json& entry : parameters.at("tracks"))
			{
				const Json& takes = entry.at("takes");
				if (laps.size() < takes.size())
					laps.resize(takes.size());
				for (size_t i = 0; i < takes.size(); ++i)
				{
					const auto position = takes[i].at("position").get<time::Samples>();
					laps[i].from = std::min(laps[i].from, position);
					laps[i].to = std::max(laps[i].to, position + takes[i].at("length").get<time::Samples>());
				}
				for (const Json& note : entry.value("notes", Json::array()))
				{
					const auto take = note.value("take", size_t{0});
					const auto position = note.at("position").get<time::Samples>();
					if (take < takes.size())
						laps[take].notes.push_back({ticks(position),
							ticks(position + note.at("length").get<time::Samples>()), note.at("pitch").get<int>()});
				}
			}
			for (const Lap& lap : laps)
				if (!lap.notes.empty())
				{
					recordChordEvents(project, lap.notes, ticks(lap.from), ticks(lap.to), context);
					changed = true;
				}
		}
		if (!changed)
			context.markUnchanged(); // every track is gone: the project isn't modified (#546)
		Json result = {{"media", media}, {"clips", clips}, {"events", events}, {"parts", parts}};
		context.emit("recording.added", result);
		return result;
	}
}
