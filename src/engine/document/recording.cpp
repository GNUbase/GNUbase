// SPDX-License-Identifier: GPL-3.0-or-later
// Audio recording (#96, reference manual pp. 344-345): recording.add puts what a recording wrote into the project as
// one undo step, through the commands a client would send, and applies the audio record mode to the events the takes
// overlap. The engine runs it when a recording ends (Recorder in src/engine/audio/recorder.h).
#include "engine/audio/transport.h"
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/engine.h"
#include "engine/time/tempomap.h"

#include <algorithm>
#include <cmath>
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

		struct Take
		{
			time::Samples position = 0; // project samples
			time::Samples offset = 0;   // in the file
			time::Samples length = 0;
		};
	}

	void ProjectModel::registerRecordingCommands(Engine& engine)
	{
		const Json integer = {{"type", "integer"}};
		const Json ids = {{"type", "array"}, {"items", integer}};
		const Json take = {{"type", "object"}, {"additionalProperties", false},
			{"properties", {{"position", integer}, {"offset", integer}, {"length", integer}}},
			{"required", {"position", "offset", "length"}}};
		const Json track = {{"type", "object"}, {"additionalProperties", false},
			{"properties",
				{{"track", integer}, {"path", {{"type", "string"}}}, {"takes", {{"type", "array"}, {"items", take}}}}},
			{"required", {"track", "path", "takes"}}};

		add(engine,
			{"recording.add", 1,
				"Adds what a recording wrote, as the engine does when a recording ends: per audio track its file as a "
				"media file and clip, and an audio event per take. A take is a run of the file (offset and length in "
				"its frames) recorded at position (project samples), such as a cycle lap; a file may start with "
				"audio pre-record before its first take. mode, the audio record mode (transport.set), says what "
				"happens to the track's unlocked events where the takes overlap them: keepHistory (the default) keeps "
				"them, cycleHistoryReplace cuts them out and keeps every take, replace cuts out what each take "
				"overlaps, also of the takes before it. One undo step. Announces recording.added with the result: "
				"the media files, clips and events added.",
				{{"type", "object"}, {"additionalProperties", false},
					{"properties",
						{{"mode", {{"type", "string"}, {"enum", g_audioRecordModes}}},
							{"tracks", {{"type", "array"}, {"items", track}}}}},
					{"required", {"tracks"}}},
				{{"type", "object"}, {"properties", {{"media", ids}, {"clips", ids}, {"events", ids}}},
					{"required", {"media", "clips", "events"}}},
				false},
			[this](Project& project, CommandContext& context) { return addRecording(project, context); });
	}

	Json ProjectModel::addRecording(Project& project, CommandContext& context)
	{
		const Json& parameters = context.parameters();
		const std::string mode = parameters.value("mode", std::string(g_audioRecordModes[0]));
		const auto run = [this, &project, &context](const char* command, const Json& commandParameters)
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
		for (const Json& entry : parameters.at("tracks"))
		{
			const auto trackId = entry.at("track").get<ObjectId>();
			const auto found = project.trackById.find(trackId);
			require(found != project.trackById.end() && found->second.type == TrackType::Audio,
				"track " + std::to_string(trackId) + " isn't an audio track");
			const Json added = run("media.add", {{"path", entry.at("path")}}).at("media");
			const auto frames = added.at("length").get<time::Samples>();
			const double ratio = project.sampleRate / added.at("sampleRate").get<double>();
			std::vector<Take> takes;
			for (const Json& item : entry.at("takes"))
			{
				const Take next{item.at("position").get<time::Samples>(), item.at("offset").get<time::Samples>(),
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
			if (mode == "cycleHistoryReplace" && !takes.empty())
			{
				time::Samples start = takes.front().position;
				time::Samples end = endOf(takes.front());
				for (const Take& each : takes)
				{
					start = std::min(start, each.position);
					end = std::max(end, endOf(each));
				}
				replace(trackId, start, end);
			}
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
		Json result = {{"media", media}, {"clips", clips}, {"events", events}};
		context.emit("recording.added", result);
		return result;
	}
}
