// SPDX-License-Identifier: GPL-3.0-or-later
// Standard MIDI file export and import (#137): midiFile.export and midiFile.import. Files are read and written on
// the command thread, like every command; the audio thread never sees them. The import is one undo step that adds
// tracks, parts, notes and controllers through the same commands a client would send, so it emits their events.
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/midifile/midiconvert.h"
#include "engine/midifile/smf.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		constexpr int g_minTicksPerQuarter = 24;
		constexpr int g_maxTicksPerQuarter = 960;

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		Json exportFile(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			midifile::ExportOptions options;
			options.format = parameters.value("type", 1);
			require(options.format == 0 || options.format == 1, "type must be 0 or 1");
			options.ticksPerQuarter = parameters.value("ppq", 480);
			require(options.ticksPerQuarter >= g_minTicksPerQuarter && options.ticksPerQuarter <= g_maxTicksPerQuarter,
				"ppq must be " + std::to_string(g_minTicksPerQuarter) + " to " + std::to_string(g_maxTicksPerQuarter));
			options.tracks = parameters.value("tracks", std::vector<ObjectId>{});
			options.markers = parameters.value("markers", true);
			if (parameters.contains("range"))
			{
				const Json& range = parameters.at("range");
				options.start = range.at("start").get<time::Ticks>();
				options.end = range.at("end").get<time::Ticks>();
				require(options.start >= 0, "range.start must not be negative");
				require(options.end > options.start, "range.end must be after range.start");
			}
			try
			{
				const midifile::File file = midifile::exportProject(project, options);
				midifile::save(file, parameters.at("path").get<std::string>());
				return Json{{"tracks", file.tracks.size()}};
			}
			catch (const std::exception& error)
			{
				throw CommandError(error.what());
			}
		}

		std::string newTrackType(const Json& parameters)
		{
			return parameters.value("trackType", std::string("midi"));
		}

		Json notesJson(const std::vector<Note>& notes, time::Ticks partStart)
		{
			Json result = Json::array();
			for (const Note& note : notes)
				result.push_back({{"position", note.position - partStart}, {"length", note.length},
					{"pitch", note.pitch}, {"velocity", note.velocity / g_maxVelocity},
					{"releaseVelocity", note.releaseVelocity / g_maxVelocity}, {"channel", note.channel}});
			return result;
		}

		Json controllersJson(const std::vector<Controller>& controllers, time::Ticks partStart)
		{
			Json result = Json::array();
			for (const Controller& controller : controllers)
			{
				Json item = toJson(controller);
				item.erase("id");
				item["position"] = controller.position - partStart;
				result.push_back(std::move(item));
			}
			return result;
		}
	}

	void ProjectModel::registerMidiFileCommands(Engine& engine)
	{
		const Json integer = {{"type", "integer"}};
		const Json boolean = {{"type", "boolean"}};
		const Json string = {{"type", "string"}};
		const Json range = {{"type", "object"}, {"additionalProperties", false},
			{"properties", {{"start", integer}, {"end", integer}}}, {"required", {"start", "end"}}};

		add(engine,
			{"midiFile.export", 1,
				"Exports MIDI and instrument tracks (tracks, default all of them) as a standard MIDI file of type 0 "
				"(one track with every MIDI channel) or 1 (default: a tempo track, then a track per exported track), "
				"with ppq ticks per quarter note (24-960, default 480). The file has the notes and controllers as they "
				"play, converted to MIDI 1.0, the track names, tempo and time signatures, and the markers unless "
				"markers is false. A note ends where the next note of its pitch and channel starts, so that "
				"overlapping notes read back the same. range {start, end} in ticks exports only what starts in it, "
				"moved to the file's start. Returns the number of tracks in the file.",
				{{"type", "object"}, {"additionalProperties", false},
					{"properties",
						{{"path", string}, {"type", {{"type", "integer"}, {"enum", {0, 1}}}}, {"ppq", integer},
							{"tracks", {{"type", "array"}, {"items", integer}}}, {"range", range},
							{"markers", boolean}}},
					{"required", {"path"}}},
				{{"type", "object"}, {"properties", {{"tracks", integer}}}, {"required", {"tracks"}}}, true},
			exportFile);

		add(engine,
			{"midiFile.import", 1,
				"Imports a standard MIDI file (type 0, 1 or 2) into new MIDI tracks (trackType midi, the default) or "
				"instrument tracks, one part per track from the bar of its first event to the bar after its last. A "
				"type 0 file gets a track per MIDI channel; the sequences of a type 2 file are laid out one after "
				"another, each from a bar start. Names that aren't UTF-8 are read as Windows-1252. Unless tempo is "
				"false, the file's tempo and time signatures replace the project's; unless markers is false, its "
				"markers are added. One undo step. Returns the new tracks and parts.",
				{{"type", "object"}, {"additionalProperties", false},
					{"properties",
						{{"path", string}, {"trackType", {{"type", "string"}, {"enum", {"midi", "instrument"}}}},
							{"tempo", boolean}, {"markers", boolean}}},
					{"required", {"path"}}},
				{{"type", "object"}, {"properties", {{"tracks", {{"type", "array"}}}, {"parts", {{"type", "array"}}}}},
					{"required", {"tracks", "parts"}}},
				false},
			[this](Project& project, CommandContext& context) { return importMidiFile(project, context); });
	}

	Json ProjectModel::importMidiFile(Project& project, CommandContext& context)
	{
		const Json& parameters = context.parameters();
		midifile::Import imported;
		try
		{
			imported = midifile::importFile(midifile::load(parameters.at("path").get<std::string>()));
		}
		catch (const std::exception& error)
		{
			throw CommandError(error.what());
		}

		if (parameters.value("tempo", true))
		{
			project.tempo = imported.tempo;
			project.signatures = imported.signatures;
			context.emit("project.changed", {{"name", project.name}, {"sampleRate", project.sampleRate}});
		}
		if (parameters.value("markers", true))
			for (Marker& marker : imported.markers)
			{
				marker.id = project.newId();
				context.emit("marker.added", {{"marker", toJson(marker)}});
				project.markers[marker.id] = std::move(marker);
			}

		// The new tracks, parts and events come from the commands that make them.
		const auto run = [this, &project, &context](const char* command, const Json& commandParameters)
		{
			CommandContext inner(commandParameters, &context);
			return m_edits.at(command).handler(project, inner);
		};
		Json tracks = Json::array();
		Json parts = Json::array();
		for (const midifile::ImportedTrack& track : imported.tracks)
		{
			Json trackParameters = {{"type", newTrackType(parameters)}};
			if (!track.name.empty())
				trackParameters["name"] = track.name;
			const auto trackId = run("track.add", trackParameters).at("track").at("id").get<ObjectId>();
			tracks.push_back(trackId);

			const midifile::PartSpan span = midifile::partSpan(track, project.signatures);
			const Json partParameters = {{"track", trackId}, {"position", span.position}, {"length", span.length}};
			const auto partId = run("midiPart.add", partParameters).at("part").at("id").get<ObjectId>();
			parts.push_back(partId);
			if (!track.notes.empty())
				run("note.add", {{"part", partId}, {"notes", notesJson(track.notes, span.position)}});
			if (!track.controllers.empty())
				run("controller.add",
					{{"part", partId}, {"controllers", controllersJson(track.controllers, span.position)}});
		}
		return {{"tracks", tracks}, {"parts", parts}};
	}
}
