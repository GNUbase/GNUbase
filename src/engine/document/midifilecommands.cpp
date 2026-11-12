// SPDX-License-Identifier: GPL-3.0-or-later
// Standard MIDI file export and import (#137): midiFile.export and midiFile.import. Files are read and written on
// the command thread, like every command; the audio thread never sees them. The import is one undo step that adds
// tracks, parts, notes and controllers through the same commands a client would send, so it emits their events.
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/midi/midivalues.h"
#include "engine/midifile/midiconvert.h"
#include "engine/midifile/smf.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
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

		// The first program change, MIDI volume and pan on MIDI channel 1, which a MIDI track's settings play on,
		// as those settings for channel.set (reference manual p. 364: Extract First Patch, Extract First
		// Volume/Pan); they leave the controllers.
		Json extractSettings(std::vector<Controller>& controllers, bool patch, bool volumePan)
		{
			Json settings = Json::object();
			const auto extract = [&controllers](ControllerType type, int number)
			{
				const auto found = std::find_if(controllers.begin(), controllers.end(),
					[type, number](const Controller& controller)
					{
						return controller.channel == 0 && controller.type == type &&
							(number < 0 || controller.number == number);
					});
				std::optional<Controller> result;
				if (found != controllers.end())
				{
					result = *found;
					controllers.erase(found);
				}
				return result;
			};
			if (patch)
				if (const std::optional<Controller> program = extract(ControllerType::Program, -1))
				{
					settings["program"] = program->number;
					settings["bank"] = program->value != g_noBank ? Json(program->value) : Json(nullptr);
				}
			if (volumePan)
				for (const auto& [number, key] : {std::pair{7, "midiVolume"}, std::pair{10, "midiPan"}})
					if (const std::optional<Controller> controller = extract(ControllerType::Control, number))
						settings[key] = midi::controllerToMidi1(controller->value);
			return settings;
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
				"markers is false: in a type 1 file the active marker track's in the tempo track, each other marker "
				"track's in a track of its own after the others, named after it. A note ends where the next note of "
				"its pitch and channel starts, so that "
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
				"false, the file's tempo and time signatures replace the project's from position to the file's end "
				"(tempo changes on the tempo track, which becomes active); after the end, the project's are kept if "
				"they change after position or the project has content after the file's end, else the file's last "
				"ones go on. Content is parts and audio events (also of the tracks' other versions), markers and "
				"arranger events by their end, and chord, scale and transpose events and automation points by their "
				"position, so one at the file's end counts; on linear tracks it is in samples, compared with the "
				"file's end in the file's tempo. An audio event on a musical track keeps its start in ticks and its "
				"length in samples, so it counts by its start in the file's tempo plus its length (by its end in "
				"ticks if its clip plays in musical mode). One on a linear track whose clip plays in musical mode "
				"keeps its start in samples and its length in beats, so it counts by its start in ticks in the file's "
				"tempo plus that length. Unless markers is false, its "
				"markers are added to the active marker track (one is added if there is none), except that a type 1 "
				"file's tracks of nothing but a name and markers, which midiFile.export writes for marker tracks, "
				"become new marker tracks at the top while the project has room for them. The options of the "
				"reference's MIDI file import (#478): position (sample frames from the project start, default 0) "
				"is where the file starts; the project's tempo and signatures before it stay. dissolve false keeps a "
				"type 0 file in one track. singlePart puts the whole file into one part on one track: track, a "
				"MIDI or instrument track of the project, or a new one named after the file; on a linear track the "
				"part's position and length are in samples, its notes in ticks. On MIDI tracks, "
				"extractPatch makes each track's first program change (with its bank) and extractVolumePan its "
				"first MIDI volume (CC 7) and pan (CC 10) on MIDI channel 1 the track's program, bank, midiVolume "
				"and midiPan (channel.set) instead of events. automation moves the continuous controllers into "
				"automation lanes, as midi.extractAutomation does. One undo step. Returns the new tracks and the "
				"parts. A track that the extraction leaves empty keeps its settings but gets no part, so parts can "
				"be shorter than tracks, and empty with singlePart.",
				{{"type", "object"}, {"additionalProperties", false},
					{"properties",
						{{"path", string}, {"trackType", {{"type", "string"}, {"enum", {"midi", "instrument"}}}},
							{"tempo", boolean}, {"markers", boolean}, {"position", integer}, {"dissolve", boolean},
							{"singlePart", boolean}, {"track", integer}, {"extractPatch", boolean},
							{"extractVolumePan", boolean}, {"automation", boolean}}},
					{"required", {"path"}}},
				{{"type", "object"}, {"properties", {{"tracks", {{"type", "array"}}}, {"parts", {{"type", "array"}}}}},
					{"required", {"tracks", "parts"}}},
				false},
			[this](Project& project, CommandContext& context) { return importMidiFile(project, context); });
	}

	Json ProjectModel::importMidiFile(Project& project, CommandContext& context)
	{
		const Json& parameters = context.parameters();
		const auto position = parameters.value("position", int64_t{0});
		require(position >= 0, "position must not be negative");
		const bool singlePart = parameters.value("singlePart", false);
		const ObjectId target = parameters.value("track", ObjectId{0});
		require(target == 0 || singlePart, "track needs singlePart");
		if (target != 0)
		{
			const auto found = project.trackById.find(target);
			require(found != project.trackById.end(), "no track " + std::to_string(target));
			require(found->second.type == TrackType::Midi || found->second.type == TrackType::Instrument,
				"track must be a MIDI or instrument track");
		}
		const std::string path = parameters.at("path").get<std::string>();
		midifile::ImportOptions options;
		options.dissolve = parameters.value("dissolve", true);
		options.start = project.tempo.toTicks(position, project.sampleRate);
		options.tempo = project.tempo;
		options.signatures = project.signatures;
		// Where the project's content ends: its parts and audio events (also those of the tracks' other versions),
		// markers, arranger, chord, scale and transpose events and automation points. The project's tempo and
		// signatures go on after the file's end if any of it is there. Musical positions count in ticks, linear ones
		// in samples, which keep their time under the file's tempo. An event that holds from its position on (a chord,
		// scale or transpose event, an automation point) counts from there: one at the file's end is after it.
		options.sampleRate = project.sampleRate;
		const auto extend = [&options](const time::Position& at, int64_t holds = 0)
		{
			int64_t& end = at.base == time::TimeBase::Linear ? options.contentEndSamples : options.contentEnd;
			end = std::max(end, at.value + holds);
		};
		const auto extendParts = [&extend](const auto& parts)
		{
			for (const auto& part : parts)
				extend({part.position.base, part.position.value + part.length});
		};
		for (const auto& [id, part] : project.midiParts)
			extend({part.position.base, part.position.value + part.length});
		for (const auto& [id, part] : project.audioParts)
			extend({part.position.base, part.position.value + part.length});
		for (const auto& [id, track] : project.trackById)
			for (const TrackVersion& version : track.versions)
			{
				extendParts(version.midiParts);
				extendParts(version.audioParts);
			}
		// An audio event keeps its position in its track's time base. If its clip plays in musical mode, its length
		// is in beats, which follow the tempo: on a musical track it keeps its end in ticks, on a linear one it ends at
		// its start in ticks in the file's tempo plus that length. Otherwise its length is in samples: on a linear
		// track it counts by its end in samples, on a musical one by its start in the file's tempo plus its length.
		forEveryAudioEvent(project,
			[&project, &options, &extend](const AudioEvent& event)
			{
				const EventSpan span = eventSpan(project, event);
				const bool linear = event.position.base == time::TimeBase::Linear;
				if (project.clips.at(event.clip).warp.musical)
				{
					const time::Ticks end = project.tempo.toTicks(span.end, project.sampleRate);
					if (linear)
						options.beatAudioEvents.push_back(
							{span.start, end - project.tempo.toTicks(span.start, project.sampleRate)});
					else
						extend({time::TimeBase::Musical, end});
				}
				else if (linear)
				{
					extend({time::TimeBase::Linear, span.end});
				}
				else
				{
					options.audioEvents.push_back({event.position.value, span.end - span.start});
				}
			});
		for (const auto& [id, marker] : project.markers)
			extend(marker.end);
		for (const ArrangerEvent& event : project.arranger.events)
			extend(event.end);
		for (const ChordEvent& event : project.chords.events)
			extend(event.position, 1);
		for (const ChordEvent& event : project.chords.scales)
			extend(event.position, 1);
		for (const TransposeEvent& event : project.transpose.events)
			extend(event.position, 1);
		for (const auto& [id, lane] : project.automation)
			for (const auto& [pointId, point] : lane.points)
				extend(point.position, 1);
		midifile::Import imported;
		try
		{
			imported = midifile::importFile(midifile::load(path), options);
		}
		catch (const std::exception& error)
		{
			throw CommandError(error.what());
		}
		if (singlePart && !imported.tracks.empty())
		{
			// One track with every event, named after the file.
			midifile::ImportedTrack merged;
			merged.name = std::filesystem::u8path(path).stem().u8string();
			for (midifile::ImportedTrack& track : imported.tracks)
			{
				merged.notes.insert(merged.notes.end(), track.notes.begin(), track.notes.end());
				merged.controllers.insert(merged.controllers.end(), track.controllers.begin(), track.controllers.end());
			}
			std::stable_sort(merged.controllers.begin(), merged.controllers.end(),
				[](const Controller& a, const Controller& b) { return a.position < b.position; });
			imported.tracks = {std::move(merged)};
		}

		// The new tracks, parts, events and markers come from the commands that make them.
		const auto run = [this, &project, &context](const char* command, const Json& commandParameters)
		{ return runEdit(command, project, commandParameters, context); };

		if (parameters.value("tempo", true))
		{
			// Tempo changes go on the tempo track and make it active; a single tempo is the tempo of the mode in use.
			if (imported.tempo.points().size() > 1 && !project.tempoTrackActive)
			{
				project.otherTempo = project.tempo;
				project.tempoTrackActive = true;
			}
			const time::TempoMap before = project.tempo;
			project.tempo = imported.tempo;
			project.signatures = imported.signatures;
			context.emit("project.changed", {{"name", project.name}, {"sampleRate", project.sampleRate}});
			tempoChanged(project, before, context);
		}
		if (parameters.value("markers", true))
		{
			// The merged markers go on the active marker track. Each marker track of the file becomes a new one, at
			// the top in file order, while the project has room for it; otherwise its markers go on the active one
			// too. The active marker track stays active, or the first new one becomes it.
			ObjectId active = project.activeMarkerTrack;
			int index = 0;
			const auto newTrack = [&run, &active, &index](const std::string& name)
			{
				Json trackParameters = {{"type", "marker"}, {"index", index++}};
				if (!name.empty())
					trackParameters["name"] = name;
				const auto id = run("track.add", trackParameters).at("track").at("id").get<ObjectId>();
				if (active == 0)
					active = id;
				return id;
			};
			const auto addMarkers = [&run, &project](ObjectId track, const std::vector<Marker>& markers)
			{
				const time::TimeBase base = project.trackById.at(track).timeBase;
				for (const Marker& marker : markers)
					run("marker.add",
						{{"track", track},
							{"position", time::rebase(marker.position, base, project.tempo, project.sampleRate).value},
							{"name", marker.name}});
			};
			if (!imported.markers.empty())
				addMarkers(active != 0 ? active : newTrack(""), imported.markers);
			for (const midifile::ImportedMarkerTrack& track : imported.markerTracks)
			{
				const auto markerTracks = std::count_if(project.tracks.begin(), project.tracks.end(),
					[&project](ObjectId id) { return project.trackById.at(id).type == TrackType::Marker; });
				addMarkers(markerTracks < g_maxMarkerTracks ? newTrack(track.name) : active, track.markers);
			}
			if (active != 0 && project.activeMarkerTrack != active)
				run("marker.activateTrack", {{"track", active}});
		}
		Json tracks = Json::array();
		Json parts = Json::array();
		for (midifile::ImportedTrack& track : imported.tracks)
		{
			ObjectId trackId = target;
			if (trackId == 0)
			{
				Json trackParameters = {{"type", newTrackType(parameters)}, {"timeBase", "musical"}};
				if (!track.name.empty())
					trackParameters["name"] = track.name;
				trackId = run("track.add", trackParameters).at("track").at("id").get<ObjectId>();
				tracks.push_back(trackId);
			}
			if (const Track& added = project.trackById.at(trackId); added.type == TrackType::Midi)
			{
				Json settings = extractSettings(track.controllers, parameters.value("extractPatch", false),
					parameters.value("extractVolumePan", false));
				if (!settings.empty())
				{
					settings["channel"] = added.channel;
					run("channel.set", settings);
				}
			}

			// A track the extraction left empty keeps its settings but gets no part.
			if (track.notes.empty() && track.controllers.empty())
				continue;
			// The part's start and length are in the track's time base, its notes in ticks from its start.
			const midifile::PartSpan span = midifile::partSpan(track, project.signatures);
			const time::TimeBase base = project.trackById.at(trackId).timeBase;
			const auto inTrackBase = [&project, base](time::Ticks ticks)
			{ return time::rebase({time::TimeBase::Musical, ticks}, base, project.tempo, project.sampleRate).value; };
			const int64_t start = inTrackBase(span.position);
			const Json partParameters = {
				{"track", trackId}, {"position", start}, {"length", inTrackBase(span.position + span.length) - start}};
			const auto partId = run("midiPart.add", partParameters).at("part").at("id").get<ObjectId>();
			parts.push_back(partId);
			if (!track.notes.empty())
				run("note.add", {{"part", partId}, {"notes", notesJson(track.notes, span.position)}});
			if (!track.controllers.empty())
				run("controller.add",
					{{"part", partId}, {"controllers", controllersJson(track.controllers, span.position)}});
		}
		if (parameters.value("automation", false) && !parts.empty())
			run("midi.extractAutomation", {{"parts", parts}});
		return {{"tracks", tracks}, {"parts", parts}};
	}
}
