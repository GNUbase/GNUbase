// SPDX-License-Identifier: GPL-3.0-or-later
// Audio and MIDI recording (#96, #97, reference manual pp. 344-345, 350-353): recording.add puts what a recording
// wrote into the project as one undo step, through the commands a client would send, and applies the audio and MIDI
// record modes to the events and parts the takes overlap. The engine runs it when a recording ends (Recorder in
// src/engine/audio/recorder.h).
#include "engine/audio/transport.h"
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/engine.h"
#include "engine/time/signaturemap.h"
#include "engine/time/tempomap.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		using Run = std::function<Json(const char* command, const Json& parameters)>;

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

		// A part's start and end in the project's samples.
		std::pair<time::Samples, time::Samples> partSpan(const Project& project, const MidiPart& part)
		{
			const time::Position end{part.position.base, part.position.value + part.length};
			return {time::toSamples(part.position, project.tempo, project.sampleRate),
				time::toSamples(end, project.tempo, project.sampleRate)};
		}

		// Adds a MIDI track's recorded notes and controller events (positions and lengths in project samples) by the
		// MIDI record mode: a part of their own from the start of the first take to the end of the last (to whole
		// bars with snapToBars), or, merging or replacing, into the first unlocked part they overlap, which grows to
		// hold them. Replacing first removes the notes that start and the controller events that lie in that range
		// from every unlocked part it overlaps. With the project's auto quantize, the notes are quantized. Returns the
		// part.
		ObjectId addMidi(
			Project& project, const Run& run, const Track& track, const Json& entry, const std::string& mode, bool snap)
		{
			const auto& tempo = project.tempo;
			const double rate = project.sampleRate;
			time::Samples start = std::numeric_limits<time::Samples>::max();
			time::Samples end = std::numeric_limits<time::Samples>::min();
			for (const Json& take : entry.at("takes"))
			{
				const auto position = take.at("position").get<time::Samples>();
				const auto length = take.at("length").get<time::Samples>();
				require(position >= 0 && length > 0, "a take must have a position and a length");
				start = std::min(start, position);
				end = std::max(end, position + length);
			}
			const Json notes = entry.value("notes", Json::array());
			const Json controllers = entry.value("controllers", Json::array());
			for (const Json& note : notes)
				end = std::max(end, note.at("position").get<time::Samples>() + note.at("length").get<time::Samples>());
			require(start < end, "a MIDI track's recording needs a take");

			time::Ticks startTick = tempo.toTicks(start, rate);
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
				const time::Samples from = tempo.toSamples(startTick, rate);
				const time::Samples to = tempo.toSamples(endTick, rate);
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
						return at >= startTick && at < endTick;
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
			}
			else
			{
				part = overlapping.front();
				const MidiPart& target = project.midiParts.at(part);
				const time::Ticks partStart = time::toTicks(target.position, tempo, rate);
				const time::Ticks partEnd =
					time::toTicks({target.position.base, target.position.value + target.length}, tempo, rate);
				// Normal sizing keeps the part's events where they are.
				if (startTick < partStart)
					run("event.resize",
						{{"events", Json::array({part})}, {"edge", "start"}, {"ticks", startTick - partStart}});
				if (endTick > partEnd)
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
	}

	void ProjectModel::registerRecordingCommands(Engine& engine)
	{
		const Json integer = {{"type", "integer"}};
		const Json number = {{"type", "number"}};
		const Json ids = {{"type", "array"}, {"items", integer}};
		const Json take = {{"type", "object"}, {"additionalProperties", false},
			{"properties", {{"position", integer}, {"offset", integer}, {"length", integer}}},
			{"required", {"position", "length"}}};
		const Json note = {{"type", "object"}, {"additionalProperties", false},
			{"properties",
				{{"position", integer}, {"length", integer}, {"pitch", integer}, {"velocity", number},
					{"releaseVelocity", number}, {"channel", integer}}},
			{"required", {"position", "length", "pitch", "velocity"}}};
		const Json controller = {{"type", "object"}, {"required", {"position"}}};
		const Json track = {{"type", "object"}, {"additionalProperties", false},
			{"properties",
				{{"track", integer}, {"path", {{"type", "string"}}}, {"takes", {{"type", "array"}, {"items", take}}},
					{"notes", {{"type", "array"}, {"items", note}}},
					{"controllers", {{"type", "array"}, {"items", controller}}}}},
			{"required", {"track", "takes"}}};

		add(engine,
			{"recording.add", 1,
				"Adds what a recording wrote, as the engine does when a recording ends. Per audio track its file "
				"(path) as a media file and clip, and an audio event per take. A take is a run of the file (offset "
				"and length in its frames) recorded at position (project samples), such as a cycle lap; a file may "
				"start with audio pre-record before its first take. mode, the audio record mode (transport.set), says "
				"what happens to the track's unlocked events where the takes overlap them: keepHistory (the default) "
				"keeps them, cycleHistoryReplace cuts them out and keeps every take, replace cuts out what each take "
				"overlaps, also of the takes before it. Per MIDI or instrument track the takes (position and length) "
				"and what was recorded, all in project samples: notes as note.add takes them and controllers as "
				"controller.add does, with positions and lengths in project samples. They go into a MIDI part from "
				"the first take's start to the last one's end, whole bars with snapToBars, by midiMode, the MIDI "
				"record mode: newParts (the default) adds a part of their own; merge adds them to the first unlocked "
				"part they overlap, which grows to hold them; replace does too, after it removed the notes that "
				"start and the controller events that lie in that range from the unlocked parts it overlaps. With "
				"the project's auto quantize (quantize.set) the recorded notes are quantized. One undo step. "
				"Announces recording.added with the result: the media files, clips and events added and the MIDI "
				"parts recorded into. project, as project.revision reports it, is the project the recording was made "
				"in: if another project is open by now, nothing is added and recording.failed is announced. A track "
				"that is gone is skipped and announced with recording.failed; the others are added.",
				{{"type", "object"}, {"additionalProperties", false},
					{"properties",
						{{"project", integer}, {"mode", {{"type", "string"}, {"enum", g_audioRecordModes}}},
							{"midiMode", {{"type", "string"}, {"enum", g_midiRecordModes}}},
							{"snapToBars", {{"type", "boolean"}}}, {"tracks", {{"type", "array"}, {"items", track}}}}},
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
		const bool snap = parameters.value("snapToBars", false);
		const Run run = [this, &project, &context](const char* command, const Json& commandParameters)
		{
			CommandContext inner(commandParameters, &context);
			return m_edits.at(command).handler(project, inner);
		};

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
			return {{"media", media}, {"clips", clips}, {"events", events}, {"parts", parts}};
		}
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
			const TrackType type = found->second.type;
			if (type == TrackType::Midi || type == TrackType::Instrument)
			{
				require(!entry.contains("path"),
					"track " + std::to_string(trackId) + " isn't an audio track: it records MIDI, not a file");
				parts.push_back(addMidi(project, run, found->second, entry, midiMode, snap));
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
			for (const Json& item : entry.at("takes"))
			{
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

			const auto endOf = [ratio](const Take& each)
			{ return each.position + std::llround(static_cast<double>(each.length) * ratio); };
			if (mode == "cycleHistoryReplace")
				for (const Take& each : takes) // before any take is added, so that every take stays
					replace(trackId, each.position, endOf(each));
			for (const Take& each : takes)
			{
				if (mode == "replace")
					replace(trackId, each.position, endOf(each));
				const time::Position at = time::rebase(
					{time::TimeBase::Linear, each.position}, found->second.timeBase, project.tempo, project.sampleRate);
				events.push_back(run("audioEvent.add",
					{{"track", trackId}, {"clip", clip}, {"position", at.value}, {"offset", each.offset},
						{"length", each.length}})
						.at("event")
						.at("id"));
			}
		}
		Json result = {{"media", media}, {"clips", clips}, {"events", events}, {"parts", parts}};
		context.emit("recording.added", result);
		return result;
	}
}
