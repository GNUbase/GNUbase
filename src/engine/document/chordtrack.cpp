// SPDX-License-Identifier: GPL-3.0-or-later
// The chord track (#173, reference manual pp. 165, 1319-1345): chord and scale events, voicings, Follow Chord Track,
// Chords to MIDI and Create Chord Events. chord.add, chord.set, chord.remove, chord.setTrack, chord.follow,
// chord.toMidi and chord.fromMidi change the project, so each is one undo step and changes nothing if it fails
// (ProjectModel::add); chord.get is a query. The music theory is midi/harmony.h's; the render snapshot plays what
// ChordFollower maps.
#include "engine/document/chordtrack.h"

#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <algorithm>
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

		const Track* chordTrack(const Project& project)
		{
			for (const auto& [id, track] : project.trackById)
				if (track.type == TrackType::Chord)
					return &track;
			return nullptr;
		}

		time::Ticks ticksOf(const Project& project, time::Position position)
		{
			return time::toTicks(position, project.tempo, project.sampleRate);
		}

		midi::ChordSymbol chordFrom(const std::string& text)
		{
			const std::optional<midi::ChordSymbol> chord = midi::parseChord(text);
			require(chord.has_value(), "unknown chord " + text);
			return chord.value_or(midi::ChordSymbol());
		}

		std::string scaleFrom(const std::string& text)
		{
			const std::optional<midi::Scale> scale = midi::parseScale(text);
			require(scale.has_value(), "unknown scale " + text);
			return midi::scaleName(scale.value_or(midi::Scale()));
		}

		bool canFollow(const Project& project, ObjectId id)
		{
			const auto found = project.trackById.find(id);
			return found != project.trackById.end() &&
				(found->second.type == TrackType::Midi || found->second.type == TrackType::Instrument);
		}

		// A progression of chords by position in ticks, with their voicings and scales: the chord track's scale events
		// without automatic scales, otherwise the chords'.
		Progression progressionOf(
			const Project& project, const std::vector<std::pair<time::Ticks, std::string>>& chords, bool scaleEvents)
		{
			const ChordTrack& track = project.chords;
			const midi::VoicingSettings settings{track.octaveOffset, track.adaptiveVoicings};
			Progression progression;
			progression.reserve(chords.size());
			std::vector<int> previous;
			for (const auto& [position, text] : chords)
			{
				midi::Harmony harmony;
				harmony.chord = midi::parseChord(text).value_or(midi::ChordSymbol());
				harmony.scale = midi::automaticScale(harmony.chord).value_or(midi::Scale());
				if (scaleEvents)
					for (const ChordEvent& event : track.scales)
						if (ticksOf(project, event.position) <= position)
							harmony.scale = midi::parseScale(event.text).value_or(harmony.scale);
				harmony.voicing = midi::voiceChord(harmony.chord, settings, previous);
				if (!harmony.voicing.empty())
					previous = harmony.voicing;
				progression.emplace_back(position, std::move(harmony));
			}
			return progression;
		}

		const midi::Harmony* harmonyAt(const Progression& progression, time::Ticks at)
		{
			const midi::Harmony* found = nullptr;
			for (const auto& [position, harmony] : progression)
			{
				if (position > at)
					break;
				found = &harmony;
			}
			return found;
		}

		struct NoteSpan
		{
			time::Ticks start = 0;
			time::Ticks end = 0;
			int pitch = 0;
		};

		// The notes of parts, by position in ticks, cut at their parts' ends, without those shorter than minLength.
		std::vector<NoteSpan> notesOf(const Project& project, const std::set<ObjectId>& parts, time::Ticks minLength)
		{
			std::vector<NoteSpan> notes;
			for (const ObjectId id : parts)
			{
				const MidiPart& part = project.midiParts.at(id);
				const time::Ticks start = ticksOf(project, part.position);
				const time::Ticks end =
					ticksOf(project, time::Position{part.position.base, part.position.value + part.length});
				for (const auto& [noteId, note] : part.notes)
				{
					const NoteSpan span{
						start + note.position, std::min(start + note.position + note.length, end), note.pitch};
					if (span.end > span.start && span.end - span.start >= minLength)
						notes.push_back(span);
				}
			}
			return notes;
		}

		// The chords the notes make, where they change: at every start and end of a note, the chord of the notes that
		// sound there if they make one other than the last one found.
		std::vector<ChordSource> detectChords(const std::vector<NoteSpan>& notes, bool bass, bool tensions)
		{
			std::set<time::Ticks> changes;
			for (const NoteSpan& note : notes)
				changes.insert({note.start, note.end});
			std::vector<ChordSource> chords;
			for (const time::Ticks at : changes)
			{
				std::vector<int> pitches;
				for (const NoteSpan& note : notes)
					if (note.start <= at && at < note.end)
						pitches.push_back(note.pitch);
				const std::optional<midi::ChordSymbol> chord = midi::recognizeChordSymbol(pitches, bass, tensions);
				if (!chord.has_value())
					continue;
				const std::string name = midi::chordName(*chord);
				if (chords.empty() || chords.back().chord != name)
					chords.push_back({at, name});
			}
			return chords;
		}

		std::set<ObjectId> partsOf(const Project& project, ObjectId track)
		{
			std::set<ObjectId> parts;
			for (const auto& [id, part] : project.midiParts)
				if (part.track == track)
					parts.insert(id);
			return parts;
		}

		std::vector<ChordSource> chordTrackSource(const Project& project)
		{
			std::vector<ChordSource> source;
			source.reserve(project.chords.events.size());
			for (const ChordEvent& event : project.chords.events)
				source.push_back({ticksOf(project, event.position), event.text});
			return source;
		}

		// Sorts the events into timeline order, refuses two of a kind at one position and announces them.
		void settle(Project& project, CommandContext& context)
		{
			for (std::vector<ChordEvent>* events : {&project.chords.events, &project.chords.scales})
			{
				std::stable_sort(events->begin(), events->end(),
					[](const ChordEvent& a, const ChordEvent& b) { return a.position.value < b.position.value; });
				for (size_t i = 1; i < events->size(); ++i)
					require((*events)[i].position.value != (*events)[i - 1].position.value,
						events == &project.chords.events ? "there is a chord event at that position already"
														 : "there is a scale event at that position already");
			}
			context.emit("chord.changed", chordJson(project));
		}

		Json eventJson(const Project& project, ObjectId id)
		{
			const Json chord = chordJson(project);
			for (const char* kind : {"events", "scales"})
				for (const Json& event : chord.at(kind))
					if (event.at("id") == id)
						return event;
			return Json::object();
		}

		Json followJson(ObjectId track, const ChordFollow& follow)
		{
			Json source = Json::array();
			for (const ChordSource& chord : follow.source)
				source.push_back({{"position", chord.position}, {"chord", chord.chord}});
			return {{"track", track}, {"mode", follow.mode}, {"source", source}};
		}

		// A chord track event by id, and whether it is a scale event.
		std::pair<ChordEvent*, bool> findEvent(Project& project, ObjectId id)
		{
			for (std::vector<ChordEvent>* events : {&project.chords.events, &project.chords.scales})
			{
				const auto found = std::find_if(
					events->begin(), events->end(), [id](const ChordEvent& event) { return event.id == id; });
				if (found != events->end())
					return {&*found, events == &project.chords.scales};
			}
			throw CommandError("no chord track event " + std::to_string(id));
		}
	}

	Progression chordProgression(const Project& project)
	{
		std::vector<std::pair<time::Ticks, std::string>> chords;
		chords.reserve(project.chords.events.size());
		for (const ChordEvent& event : project.chords.events)
			chords.emplace_back(ticksOf(project, event.position), event.text);
		return progressionOf(project, chords, !project.chords.automaticScales);
	}

	ChordFollower::ChordFollower(const Project& project, ObjectId track)
	{
		const auto found = project.chords.follow.find(track);
		if (found == project.chords.follow.end() || found->second.mode == "off" || chordTrack(project) == nullptr)
			return;
		m_mode = found->second.mode;
		std::vector<std::pair<time::Ticks, std::string>> source;
		source.reserve(found->second.source.size());
		for (const ChordSource& chord : found->second.source)
			source.emplace_back(chord.position, chord.chord);
		m_from = progressionOf(project, source, false);
		m_to = chordProgression(project);
	}

	int ChordFollower::pitch(int pitch, time::Ticks at) const
	{
		const midi::Harmony* from = harmonyAt(m_from, at);
		const midi::Harmony* to = harmonyAt(m_to, at);
		if (from == nullptr || to == nullptr)
			return pitch;
		return midi::followPitch(pitch, m_mode, *from, *to);
	}

	Json chordJson(const Project& project)
	{
		const Track* track = chordTrack(project);
		const ChordTrack& chords = project.chords;
		const Progression progression = chordProgression(project);
		Json events = Json::array();
		for (size_t i = 0; i < chords.events.size(); ++i)
		{
			const ChordEvent& event = chords.events[i];
			const midi::Harmony& harmony = progression.at(i).second;
			events.push_back({{"id", event.id}, {"position", event.position.value},
				{"start", time::toSamples(event.position, project.tempo, project.sampleRate)}, {"chord", event.text},
				{"scale", harmony.chord.root < 0 ? std::string() : midi::scaleName(harmony.scale)},
				{"pitches", harmony.voicing}});
		}
		Json scales = Json::array();
		for (const ChordEvent& event : chords.scales)
			scales.push_back({{"id", event.id}, {"position", event.position.value},
				{"start", time::toSamples(event.position, project.tempo, project.sampleRate)}, {"scale", event.text}});
		Json follow = Json::array();
		for (const auto& [id, settings] : chords.follow)
			if (project.trackById.count(id) != 0)
				follow.push_back(followJson(id, settings));
		return {{"track", track == nullptr ? ObjectId{0} : track->id}, {"events", events}, {"scales", scales},
			{"octaveOffset", chords.octaveOffset}, {"adaptiveVoicings", chords.adaptiveVoicings},
			{"automaticScales", chords.automaticScales}, {"follow", follow}};
	}

	void chordFromJson(Project& project, const Json& json)
	{
		const Track* track = chordTrack(project);
		if (track == nullptr)
			return;
		const time::TimeBase base = track->timeBase;
		ChordTrack& chords = project.chords;
		const auto read = [base](const Json& item, const char* text)
		{
			ChordEvent event;
			event.id = item.at("id").get<ObjectId>();
			event.position = {base, item.at("position").get<int64_t>()};
			event.text = item.value(text, std::string());
			return event;
		};
		for (const Json& item : json.value("events", Json::array()))
			chords.events.push_back(read(item, "chord"));
		for (const Json& item : json.value("scales", Json::array()))
			chords.scales.push_back(read(item, "scale"));
		for (std::vector<ChordEvent>* events : {&chords.events, &chords.scales})
			std::stable_sort(events->begin(), events->end(),
				[](const ChordEvent& a, const ChordEvent& b) { return a.position.value < b.position.value; });
		chords.octaveOffset = std::clamp(json.value("octaveOffset", 0), -3, 3);
		chords.adaptiveVoicings = json.value("adaptiveVoicings", true);
		chords.automaticScales = json.value("automaticScales", true);
		for (const Json& item : json.value("follow", Json::array()))
		{
			ChordFollow follow;
			follow.mode = item.value("mode", std::string("off"));
			for (const Json& chord : item.value("source", Json::array()))
				follow.source.push_back({chord.at("position").get<time::Ticks>(), chord.value("chord", "X")});
			chords.follow[item.at("track").get<ObjectId>()] = follow;
		}
	}

	void ProjectModel::registerChordCommands(Engine& engine)
	{
		const Json idSchema = {{"type", "integer"}};
		const Json integerSchema = {{"type", "integer"}};
		const Json booleanSchema = {{"type", "boolean"}};
		const Json stringSchema = {{"type", "string"}};
		const Json idsSchema = {{"type", "array"}, {"items", idSchema}};
		const Json eventResult = {
			{"type", "object"}, {"properties", {{"event", {{"type", "object"}}}}}, {"required", {"event"}}};
		const Json chordResult = {{"type", "object"},
			{"properties",
				{{"track", integerSchema}, {"events", {{"type", "array"}}}, {"scales", {{"type", "array"}}},
					{"octaveOffset", integerSchema}, {"adaptiveVoicings", booleanSchema},
					{"automaticScales", booleanSchema}, {"follow", {{"type", "array"}}}}},
			{"required",
				{"track", "events", "scales", "octaveOffset", "adaptiveVoicings", "automaticScales", "follow"}}};
		const Json eventSchema = objectSchema(
			{{"event", idSchema}, {"position", integerSchema}, {"chord", stringSchema}, {"scale", stringSchema}},
			{"event"});
		const Json partResult = {
			{"type", "object"}, {"properties", {{"part", {{"type", "object"}}}}}, {"required", {"part"}}};

		add(engine,
			{"chord.add", 1,
				"Adds a chord event to the chord track, which is added if the project has none, or with scale a scale "
				"event. position is in the track's time base, ticks or samples (default the cursor); each event plays "
				"to the next of its kind. chord is the chord as the chord editor's text input takes it: a root (C to "
				"B with # or b), a type (m, maj, dim, aug, sus2, sus4), tensions (6, 7, maj7, 9, 11, 13, b9, #9, #11, "
				"b13; 9, 11 and 13 bring the seventh unless after add) and a bass note after a slash, such as Cmaj7, "
				"Dm7(9) or C/E; the default, \"\" or X, is an undefined chord. scale is a root and a scale type, such "
				"as \"D dorian\" (midi.transpose's scale types). The event comes back with its chord's name, its scale "
				"and its voicing (pitches). Refused where an event of the kind is.",
				objectSchema({{"position", integerSchema}, {"chord", stringSchema}, {"scale", stringSchema}}),
				eventResult, false},
			[this](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				require(!parameters.contains("chord") || !parameters.contains("scale"),
					"an event has a chord or a scale, not both");
				if (chordTrack(project) == nullptr)
					runEdit("track.add", project, {{"type", "chord"}}, context);
				const time::TimeBase base = chordTrack(project)->timeBase;
				ChordEvent event;
				event.id = project.newId();
				if (parameters.contains("position"))
					event.position = {base, parameters.at("position").get<int64_t>()};
				else
					event.position = time::rebase(
						{time::TimeBase::Linear, context.call("transport.get").at("position").get<int64_t>()}, base,
						project.tempo, project.sampleRate);
				require(event.position.value >= 0, "position must not be before the project start");
				if (parameters.contains("scale"))
				{
					event.text = scaleFrom(parameters.at("scale").get<std::string>());
					project.chords.scales.push_back(event);
				}
				else
				{
					event.text = midi::chordName(chordFrom(parameters.value("chord", std::string())));
					project.chords.events.push_back(event);
				}
				settle(project, context);
				return Json{{"event", eventJson(project, event.id)}};
			});
		add(engine,
			{"chord.set", 1,
				"Changes chord track events: each moves to position (in the chord track's time base) or gets a new "
				"chord (chord events) or scale (scale events), as chord.add takes them.",
				objectSchema({{"events", {{"type", "array"}, {"items", eventSchema}}}}, {"events"}), Json::object(),
				false},
			[](Project& project, CommandContext& context)
			{
				require(chordTrack(project) != nullptr, "the project has no chord track");
				for (const Json& item : context.parameters().at("events"))
				{
					const auto [event, scale] = findEvent(project, item.at("event").get<ObjectId>());
					if (item.contains("position"))
					{
						event->position.value = item.at("position").get<int64_t>();
						require(event->position.value >= 0, "position must not be before the project start");
					}
					require(!item.contains(scale ? "chord" : "scale"),
						scale ? "a scale event has no chord" : "a chord event has no scale");
					if (item.contains("chord"))
						event->text = midi::chordName(chordFrom(item.at("chord").get<std::string>()));
					if (item.contains("scale"))
						event->text = scaleFrom(item.at("scale").get<std::string>());
				}
				settle(project, context);
				return Json::object();
			});
		add(engine,
			{"chord.remove", 1, "Removes chord and scale events.", objectSchema({{"events", idsSchema}}, {"events"}),
				Json::object(), false},
			[](Project& project, CommandContext& context)
			{
				require(chordTrack(project) != nullptr, "the project has no chord track");
				const auto ids = context.parameters().at("events").get<std::set<ObjectId>>();
				for (const ObjectId id : ids)
					findEvent(project, id);
				for (std::vector<ChordEvent>* events : {&project.chords.events, &project.chords.scales})
					events->erase(std::remove_if(events->begin(), events->end(),
									  [&ids](const ChordEvent& event) { return ids.count(event.id) != 0; }),
						events->end());
				settle(project, context);
				return Json::object();
			});
		add(engine,
			{"chord.setTrack", 1,
				"Changes the chord track's settings (reference manual p. 1333): octaveOffset (-3 to 3), the basic "
				"voicings' Octave Offset from C3: the bass note from C2, the chord above it from C3; "
				"adaptiveVoicings, voicings that take the inversion nearest to the chord before; automaticScales, "
				"each chord event's scale is its chord's (major, mixolydian with the seventh, minor, dorian with the "
				"seventh and so on) rather than the scale event's playing there.",
				objectSchema({{"octaveOffset", integerSchema}, {"adaptiveVoicings", booleanSchema},
					{"automaticScales", booleanSchema}}),
				chordResult, false},
			[](Project& project, CommandContext& context)
			{
				require(chordTrack(project) != nullptr, "the project has no chord track");
				const Json& parameters = context.parameters();
				ChordTrack& chords = project.chords;
				chords.octaveOffset = parameters.value("octaveOffset", chords.octaveOffset);
				require(chords.octaveOffset >= -3 && chords.octaveOffset <= 3, "octaveOffset must be -3 to 3");
				chords.adaptiveVoicings = parameters.value("adaptiveVoicings", chords.adaptiveVoicings);
				chords.automaticScales = parameters.value("automaticScales", chords.automaticScales);
				Json result = chordJson(project);
				context.emit("chord.changed", result);
				return result;
			});
		add(engine,
			{"chord.follow", 1,
				"Sets how a MIDI or instrument track follows the chord track (Follow Chord Track, reference manual pp. "
				"1339-1341): mode off, chordsAndScales (the old chord's notes become the same notes of the new chord, "
				"root, third and so on, the others the same degree of the new scale), chords (to the nearest note of "
				"the new chord), scales (to the nearest note of the new scale), rootNotes (by the step between the "
				"roots, the nearer way, as the transpose track does) or voicings (the same voice of the new chord's "
				"voicing). Every mode but voicings first moves notes by the step between the roots. The notes stay as "
				"they are; playback maps each from the chord it was played over to the chord event playing at its "
				"start. sync says what that chord was, the first time (Follow Chord Track dialog): direct (the "
				"default, Follow Directly: the chord track as it is now), analyze (the chords of the track's MIDI "
				"notes) or known with chord (Apply a Known Chord, everywhere). Drum-mapped tracks don't follow.",
				objectSchema({{"track", idSchema}, {"mode", {{"type", "string"}, {"enum", midi::followModeNames()}}},
								 {"sync", {{"type", "string"}, {"enum", {"direct", "analyze", "known"}}}},
								 {"chord", stringSchema}},
					{"track", "mode"}),
				objectSchema({{"track", integerSchema}, {"mode", stringSchema}, {"source", {{"type", "array"}}}},
					{"track", "mode", "source"}),
				false},
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				require(chordTrack(project) != nullptr, "the project has no chord track");
				const auto track = parameters.at("track").get<ObjectId>();
				require(canFollow(project, track), "only MIDI and instrument tracks follow the chord track");
				const std::string mode = parameters.at("mode").get<std::string>();
				const std::vector<std::string>& modes = midi::followModeNames();
				require(std::find(modes.begin(), modes.end(), mode) != modes.end(), "unknown mode " + mode);
				ChordFollow& follow = project.chords.follow[track];
				const std::string sync = parameters.value("sync", std::string());
				if (sync == "known")
				{
					require(parameters.contains("chord"), "sync known needs a chord");
					follow.source = {{0, midi::chordName(chordFrom(parameters.at("chord").get<std::string>()))}};
				}
				else if (sync == "analyze")
				{
					follow.source = detectChords(notesOf(project, partsOf(project, track), 0), true, true);
				}
				else if (sync == "direct" || follow.source.empty())
				{
					follow.source = chordTrackSource(project);
				}
				follow.mode = mode;
				context.emit("chord.changed", chordJson(project));
				return followJson(track, follow);
			});
		add(engine,
			{"chord.toMidi", 1,
				"Converts chord events to MIDI (reference manual p. 1336): a new MIDI part on a MIDI or instrument "
				"track with each chord's voicing (chord.get pitches) from its event to the next, the last a bar long. "
				"events are the chord events to convert, in a row (dragged onto the track); without them, all, and the "
				"track then follows the chord track with voicings (Chords to MIDI). Undefined chords make no notes.",
				objectSchema({{"track", idSchema}, {"events", idsSchema}}, {"track"}), partResult, false},
			[this](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const auto track = parameters.at("track").get<ObjectId>();
				require(canFollow(project, track), "chord events go to a MIDI or instrument track");
				const std::vector<ChordEvent>& events = project.chords.events;
				std::set<ObjectId> chosen;
				if (parameters.contains("events"))
					for (const Json& item : parameters.at("events"))
					{
						const auto id = item.get<ObjectId>();
						require(std::any_of(events.begin(), events.end(),
									[id](const ChordEvent& event) { return event.id == id; }),
							"no chord event " + std::to_string(id));
						chosen.insert(id);
					}
				else
					for (const ChordEvent& event : events)
						chosen.insert(event.id);
				require(!chosen.empty(), "there are no chord events");
				const Progression progression = chordProgression(project);
				struct Span
				{
					time::Ticks start = 0;
					time::Ticks end = 0;
					std::vector<int> pitches;
				};
				std::vector<Span> spans;
				for (size_t i = 0; i < events.size(); ++i)
				{
					if (chosen.count(events[i].id) == 0)
						continue;
					const time::Ticks start = progression[i].first;
					const time::Ticks end = i + 1 < events.size()
						? progression[i + 1].first
						: project.signatures.barStart(project.signatures.toBarsBeats(start).bar + 1);
					spans.push_back({start, end, progression[i].second.voicing});
				}
				const time::TimeBase base = project.trackById.at(track).timeBase;
				const auto inBase = [&](time::Ticks tick)
				{
					return time::rebase({time::TimeBase::Musical, tick}, base, project.tempo, project.sampleRate).value;
				};
				const time::Ticks first = spans.front().start;
				const Json added = runEdit("midiPart.add", project,
					{{"track", track}, {"position", inBase(first)},
						{"length", inBase(spans.back().end) - inBase(first)}},
					context);
				const auto part = added.at("part").at("id").get<ObjectId>();
				Json notes = Json::array();
				for (const Span& span : spans)
					for (const int pitch : span.pitches)
						notes.push_back({{"position", span.start - first}, {"length", span.end - span.start},
							{"pitch", pitch}, {"velocity", 0.75}});
				if (!notes.empty())
					runEdit("note.add", project, {{"part", part}, {"notes", notes}}, context);
				if (!parameters.contains("events"))
				{
					ChordFollow& follow = project.chords.follow[track];
					follow.mode = "voicings";
					follow.source = chordTrackSource(project);
					context.emit("chord.changed", chordJson(project));
				}
				return Json{{"part", toJson(project.midiParts.at(part))}};
			});
		add(engine,
			{"chord.fromMidi", 1,
				"Create Chord Events (reference manual pp. 1342-1343): chord events from the MIDI notes of parts or of "
				"tracks' parts, on the chord track, which is added if the project has none. Where the notes that "
				"sound change, the chord they make, if any and other than the one before, becomes a chord event: with "
				"includeBass (default true) a lowest note other than the root is its bass note, with includeTensions "
				"(default true) other notes are its tensions; notes shorter than minLength ticks (Ignore Notes Shorter "
				"Than, default 0) don't count. The chord events from the first note's start to the last one's end are "
				"replaced. Returns the events made.",
				objectSchema({{"parts", idsSchema}, {"tracks", idsSchema}, {"includeBass", booleanSchema},
					{"includeTensions", booleanSchema}, {"minLength", integerSchema}}),
				{{"type", "object"}, {"properties", {{"events", {{"type", "array"}}}}}, {"required", {"events"}}},
				false},
			[this](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				require(parameters.contains("parts") || parameters.contains("tracks"), "give parts or tracks");
				std::set<ObjectId> parts;
				for (const Json& item : parameters.value("parts", Json::array()))
				{
					const auto id = item.get<ObjectId>();
					require(project.midiParts.count(id) != 0, "no MIDI part " + std::to_string(id));
					parts.insert(id);
				}
				for (const Json& item : parameters.value("tracks", Json::array()))
				{
					const auto id = item.get<ObjectId>();
					require(project.trackById.count(id) != 0, "no track " + std::to_string(id));
					const std::set<ObjectId> ofTrack = partsOf(project, id);
					parts.insert(ofTrack.begin(), ofTrack.end());
				}
				const std::vector<NoteSpan> notes =
					notesOf(project, parts, parameters.value("minLength", time::Ticks{0}));
				const std::vector<ChordSource> found = detectChords(
					notes, parameters.value("includeBass", true), parameters.value("includeTensions", true));
				if (chordTrack(project) == nullptr)
					runEdit("track.add", project, {{"type", "chord"}}, context);
				const time::TimeBase base = chordTrack(project)->timeBase;
				std::vector<ChordEvent>& events = project.chords.events;
				if (!notes.empty())
				{
					time::Ticks from = notes.front().start;
					time::Ticks to = notes.front().end;
					for (const NoteSpan& note : notes)
					{
						from = std::min(from, note.start);
						to = std::max(to, note.end);
					}
					events.erase(std::remove_if(events.begin(), events.end(),
									 [&](const ChordEvent& event)
									 {
										 const time::Ticks at = ticksOf(project, event.position);
										 return at >= from && at < to;
									 }),
						events.end());
				}
				std::vector<ObjectId> made;
				for (const ChordSource& chord : found)
				{
					ChordEvent event;
					event.id = project.newId();
					event.position = time::rebase(
						{time::TimeBase::Musical, chord.position}, base, project.tempo, project.sampleRate);
					event.text = chord.chord;
					events.push_back(event);
					made.push_back(event.id);
				}
				settle(project, context);
				Json result = Json::array();
				for (const ObjectId id : made)
					result.push_back(eventJson(project, id));
				return Json{{"events", result}};
			});
		add(engine,
			{"chord.get", 1,
				"The chord track: its id (0 for none); its chord events in timeline order (id, position in the "
				"track's time base, start in samples, chord, scale and pitches, the chord's voicing from the lowest "
				"up); its scale events (id, position, start, scale); octaveOffset, adaptiveVoicings and "
				"automaticScales; and follow, the tracks that follow it with their mode and source, the chords their "
				"notes were played over (position in ticks, chord).",
				objectSchema({}), chordResult, true},
			[](Project& project, CommandContext&) { return chordJson(project); });
	}
}
