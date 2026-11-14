// SPDX-License-Identifier: GPL-3.0-or-later
// The chord track (#173, #933, reference manual pp. 165, 1319-1345): chord and scale events, voicings, Follow Chord
// Track, Live Input, Chords to MIDI, Create Chord Events from MIDI and audio, Map to Chord Track, Assign Voices to
// Notes and auditioning. chord.add, chord.set, chord.remove, chord.setTrack, chord.follow, chord.toMidi,
// chord.fromMidi, chord.fromAudio, chord.map and chord.assignVoices change the project, so each is one undo step and
// changes nothing if it fails (ProjectModel::add); chord.get is a query and chord.audition plays. The music theory is
// midi/harmony.h's; the render snapshot plays what ChordFollower maps and renderChordTrack adds.
#include "engine/document/chordtrack.h"

#include "engine/analysis/analysis.h"
#include "engine/audiofile/audiofile.h"
#include "engine/document/projectmodel.h"
#include "engine/document/rendersnapshot.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
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

		bool among(const std::vector<std::string>& names, const std::string& name)
		{
			return std::find(names.begin(), names.end(), name) != names.end();
		}

		const std::vector<std::string>& liveInputNames()
		{
			static const std::vector<std::string> names = {"off", "chords", "scales"};
			return names;
		}

		// This Track's Voicings: the chord track's or a library.
		const std::vector<std::string>& trackVoicingNames()
		{
			static const std::vector<std::string> names = {"chordTrack", "basic", "piano", "guitar"};
			return names;
		}

		midi::VoicingStyle styleOf(const ChordVoicing& voicing)
		{
			return {voicing.library, voicing.style, voicing.addRoot, voicing.duplicateRoot, voicing.fattenUp,
				voicing.lowestRoot, voicing.lowest, voicing.highest};
		}

		// The voicings a following track plays: This Track's Voicings, or a Single Voice track's master's; the chord
		// track's style if the library is the chord track's.
		midi::VoicingStyle trackStyle(const Project& project, const ChordFollow& follow)
		{
			const ChordFollow* own = &follow;
			if (follow.master != 0)
				if (const auto master = project.chords.follow.find(follow.master);
					master != project.chords.follow.end())
					own = &master->second;
			if (own->voicings == "chordTrack" || own->voicings == project.chords.voicing.library)
				return styleOf(project.chords.voicing);
			midi::VoicingStyle style;
			style.library = own->voicings;
			return style;
		}

		// A progression of chords by position in ticks, with their voicings in a style and scales: the chord track's
		// scale events without automatic scales, otherwise the chords'.
		Progression progressionOf(const Project& project,
			const std::vector<std::pair<time::Ticks, std::string>>& chords, bool scaleEvents,
			const midi::VoicingStyle& style)
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
				harmony.voicing = midi::voiceChord(harmony.chord, settings, previous, style);
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
		// With sustain (Interpret Sustain Pedal), a note that ends while the sustain pedal (controller 64) is down
		// lasts until it is let up.
		std::vector<NoteSpan> notesOf(
			const Project& project, const std::set<ObjectId>& parts, time::Ticks minLength, bool sustain = false)
		{
			std::vector<NoteSpan> notes;
			for (const ObjectId id : parts)
			{
				const MidiPart& part = project.midiParts.at(id);
				const time::Ticks start = ticksOf(project, part.position);
				const time::Ticks end =
					ticksOf(project, time::Position{part.position.base, part.position.value + part.length});
				std::map<time::Ticks, bool> pedal; // down or up, by position in the part
				if (sustain)
					for (const auto& [controllerId, controller] : part.controllers)
						if (controller.type == ControllerType::Control && controller.number == 64)
							pedal[controller.position] = controller.value >= 0x80000000U;
				for (const auto& [noteId, note] : part.notes)
				{
					time::Ticks noteEnd = note.position + note.length;
					if (const auto at = pedal.upper_bound(noteEnd); at != pedal.begin() && std::prev(at)->second)
					{
						const auto up = std::find_if(at, pedal.end(), [](const auto& entry) { return !entry.second; });
						noteEnd = up == pedal.end() ? end - start : up->first;
					}
					const NoteSpan span{start + note.position, std::min(start + noteEnd, end), note.pitch};
					const time::Ticks played = std::min(start + note.position + note.length, end) - span.start;
					if (span.end > span.start && played >= minLength)
						notes.push_back(span);
				}
			}
			return notes;
		}

		// The chords the notes make, where they change: at every start and end of a note, the chord of the notes that
		// sound there if they make one other than the last one found. With arpeggios (Detect Arpeggios), a note sounds
		// on to the end of its bar, and a chord starts where the first of its notes in that bar does; a fuller chord
		// found later at the same start replaces the one before.
		// ponytail: arpeggios are found a bar at a time; two arpeggiated chords in a bar make one, or none.
		std::vector<ChordSource> detectChords(
			const Project& project, std::vector<NoteSpan> notes, bool bass, bool tensions, bool arpeggios = false)
		{
			const time::SignatureMap& bars = project.signatures;
			const auto barOf = [&bars](time::Ticks tick) { return bars.barStart(bars.toBarsBeats(tick).bar); };
			if (arpeggios)
				for (NoteSpan& note : notes)
					note.end = std::max(note.end, bars.barStart(bars.toBarsBeats(note.start).bar + 1));
			std::set<time::Ticks> changes;
			for (const NoteSpan& note : notes)
				changes.insert({note.start, note.end});
			std::vector<ChordSource> chords;
			for (const time::Ticks at : changes)
			{
				std::vector<int> pitches;
				time::Ticks first = at;
				for (const NoteSpan& note : notes)
					if (note.start <= at && at < note.end)
					{
						pitches.push_back(note.pitch);
						if (arpeggios && note.start >= barOf(at))
							first = std::min(first, note.start);
					}
				const std::optional<midi::ChordSymbol> chord = midi::recognizeChordSymbol(pitches, bass, tensions);
				if (!chord.has_value())
					continue;
				const std::string name = midi::chordName(*chord);
				if (!chords.empty() && chords.back().position >= first)
					chords.pop_back();
				if (chords.empty() || chords.back().chord != name)
					chords.push_back({first, name});
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
			return {{"track", track}, {"mode", follow.mode}, {"source", source}, {"voicings", follow.voicings},
				{"voice", midi::voiceNames().at(static_cast<size_t>(std::clamp(follow.voice, 0, 3)))},
				{"master", follow.master}, {"liveInput", follow.liveInput}};
		}

		Json voicingJson(const ChordVoicing& voicing)
		{
			return {{"library", voicing.library}, {"style", voicing.style}, {"addRoot", voicing.addRoot},
				{"duplicateRoot", voicing.duplicateRoot}, {"fattenUp", voicing.fattenUp},
				{"lowestRoot", voicing.lowestRoot}, {"lowest", voicing.lowest}, {"highest", voicing.highest}};
		}

		// Changes a voicing library's settings as chord.setTrack's voicing gives them; refuses what doesn't fit.
		void setVoicing(ChordVoicing& voicing, const Json& changes)
		{
			voicing.library = changes.value("library", voicing.library);
			require(among(midi::voicingLibraries(), voicing.library), "unknown voicing library " + voicing.library);
			if (changes.contains("library") && !changes.contains("style"))
				voicing.style.clear();
			voicing.style = changes.value("style", voicing.style);
			require(voicing.style.empty() || among(midi::voicingStyles(voicing.library), voicing.style),
				"the " + voicing.library + " voicings have no style " + voicing.style);
			voicing.addRoot = changes.value("addRoot", voicing.addRoot);
			voicing.duplicateRoot = changes.value("duplicateRoot", voicing.duplicateRoot);
			voicing.fattenUp = changes.value("fattenUp", voicing.fattenUp);
			for (const auto& [key, value] : {std::pair{"lowestRoot", &voicing.lowestRoot},
					 std::pair{"lowest", &voicing.lowest}, std::pair{"highest", &voicing.highest}})
			{
				*value = changes.value(key, *value);
				require(*value >= 0 && *value <= 127, std::string(key) + " must be 0 to 127");
			}
			require(voicing.lowest + 12 <= voicing.highest, "highest must be an octave or more above lowest");
		}

		// The pitch classes of a harmony's chord or scale, bit n for class n.
		uint16_t classesOf(const midi::Harmony& harmony, bool scale)
		{
			uint16_t classes = 0;
			if (harmony.chord.root < 0)
				return classes;
			const int root = scale ? harmony.scale.root : harmony.chord.root;
			for (const int step : scale ? midi::scaleSteps(harmony.scale.type) : midi::chordIntervals(harmony.chord))
				classes |= static_cast<uint16_t>(1U << static_cast<unsigned>((((root + step) % 12) + 12) % 12));
			return classes;
		}

		// The notes of chord events, a scale event's from its root up an octave.
		std::vector<int> scaleNotes(const std::string& text)
		{
			std::vector<int> pitches;
			if (const std::optional<midi::Scale> scale = midi::parseScale(text))
			{
				for (const int step : midi::scaleSteps(scale->type))
					pitches.push_back(60 + scale->root + step);
				pitches.push_back(72 + scale->root);
			}
			return pitches;
		}

		// Replaces the chord events from `from` to `to` (ticks) with chords found there.
		std::vector<ObjectId> replaceChords(
			Project& project, const std::vector<ChordSource>& found, time::Ticks from, time::Ticks to)
		{
			const time::TimeBase base = chordTrack(project)->timeBase;
			std::vector<ChordEvent>& events = project.chords.events;
			events.erase(std::remove_if(events.begin(), events.end(),
							 [&](const ChordEvent& event)
							 {
								 const time::Ticks at = ticksOf(project, event.position);
								 return at >= from && at < to;
							 }),
				events.end());
			std::vector<ObjectId> made;
			for (const ChordSource& chord : found)
			{
				ChordEvent event;
				event.id = project.newId();
				event.position =
					time::rebase({time::TimeBase::Musical, chord.position}, base, project.tempo, project.sampleRate);
				event.text = chord.chord;
				events.push_back(event);
				made.push_back(event.id);
			}
			return made;
		}

		// Chord detection from audio (reference manual p. 1343): the strength of each pitch class in a window of
		// mono audio, from the power spectrum between 100 Hz and 4 kHz.
		std::array<double, 12> chromaOf(const std::vector<float>& samples, double sampleRate)
		{
			std::array<double, 12> chroma{};
			if (samples.empty())
				return chroma;
			analysis::PowerSpectrum spectrum;
			spectrum.feed(samples.data(), samples.size());
			const analysis::PowerBins bins = spectrum.finish();
			for (size_t bin = 1; bin < bins.power.size(); ++bin)
			{
				const double frequency = static_cast<double>(bin) * sampleRate / static_cast<double>(bins.size);
				if (frequency < 100.0 || frequency > 4000.0)
					continue;
				const auto pitch = static_cast<int>(std::lround(69.0 + (12.0 * std::log2(frequency / 440.0))));
				chroma.at(static_cast<size_t>(((pitch % 12) + 12) % 12)) += bins.power[bin];
			}
			return chroma;
		}

		// The chords a chroma fits, best first: triads, sus chords and sevenths on every root, by the cosine of the
		// chroma and the chord's notes. None for silence.
		std::vector<std::string> chordsOf(const std::array<double, 12>& chroma)
		{
			const double total = std::accumulate(chroma.begin(), chroma.end(), 0.0);
			if (total <= 1e-12)
				return {};
			double norm = 0.0;
			for (const double value : chroma)
				norm += value * value;
			norm = std::sqrt(norm);
			static const std::vector<std::string> types = {"", "m", "dim", "aug", "sus2", "sus4", "7", "maj7", "m7"};
			std::vector<std::pair<double, std::string>> scored;
			for (int root = 0; root < 12; ++root)
				for (const std::string& type : types)
				{
					const midi::ChordSymbol chord =
						midi::parseChord(midi::chordName({root, "maj", {}, -1}) + type).value_or(midi::ChordSymbol());
					const std::vector<int> intervals = midi::chordIntervals(chord);
					double sum = 0.0;
					for (const int interval : intervals)
						sum += chroma.at(static_cast<size_t>((root + interval) % 12));
					const double score = sum / (norm * std::sqrt(static_cast<double>(intervals.size())));
					scored.emplace_back(score, midi::chordName(chord));
				}
			std::stable_sort(
				scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
			std::vector<std::string> names;
			for (size_t i = 0; i < scored.size() && names.size() < 5; ++i)
				names.push_back(scored[i].second);
			return names;
		}

		// An audio event's audio, mixed to mono, from a project sample on for some project samples, at the media
		// file's sample rate.
		std::vector<float> eventAudio(const Project& project, const AudioEvent& event, AudioFileReader& reader,
			const MediaFile& media, time::Samples from, time::Samples count)
		{
			const double ratio = media.sampleRate / project.sampleRate;
			const time::Samples start = eventSpan(project, event).start;
			const auto frame =
				event.offset + static_cast<time::Samples>(std::llround(static_cast<double>(from - start) * ratio));
			const auto frames = static_cast<size_t>(std::llround(static_cast<double>(count) * ratio));
			const auto channels = static_cast<size_t>(std::max(reader.info().channels, 1));
			std::vector<float> interleaved(frames * channels);
			reader.seek(std::clamp<time::Samples>(frame, 0, reader.info().frames));
			const size_t read = reader.read(interleaved.data(), frames);
			std::vector<float> mono(read);
			for (size_t i = 0; i < read; ++i)
				for (size_t c = 0; c < channels; ++c)
					mono[i] += interleaved[(i * channels) + c] / static_cast<float>(channels);
			return mono;
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

	namespace
	{
		std::vector<std::pair<time::Ticks, std::string>> chordTrackChords(const Project& project)
		{
			std::vector<std::pair<time::Ticks, std::string>> chords;
			chords.reserve(project.chords.events.size());
			for (const ChordEvent& event : project.chords.events)
				chords.emplace_back(ticksOf(project, event.position), event.text);
			return chords;
		}
	}

	Progression chordProgression(const Project& project)
	{
		return progressionOf(
			project, chordTrackChords(project), !project.chords.automaticScales, styleOf(project.chords.voicing));
	}

	ChordFollower::ChordFollower(const Project& project, ObjectId track)
	{
		const auto found = project.chords.follow.find(track);
		if (found == project.chords.follow.end() || found->second.mode == "off" || chordTrack(project) == nullptr ||
			project.chords.mute)
			return;
		m_mode = found->second.mode;
		m_voice = found->second.voice;
		m_shift = project.chords.shiftChords;
		const midi::VoicingStyle style = trackStyle(project, found->second);
		std::vector<std::pair<time::Ticks, std::string>> source;
		source.reserve(found->second.source.size());
		for (const ChordSource& chord : found->second.source)
			source.emplace_back(chord.position, chord.chord);
		m_from = progressionOf(project, source, false, style);
		m_to = progressionOf(project, chordTrackChords(project), !project.chords.automaticScales, style);
	}

	int ChordFollower::pitch(int pitch, time::Ticks at) const
	{
		// Shift Chords: a negative shift lets each chord event take notes played that much before it.
		const midi::Harmony* from = harmonyAt(m_from, at - m_shift);
		const midi::Harmony* to = harmonyAt(m_to, at - m_shift);
		if (from == nullptr || to == nullptr)
			return pitch;
		return midi::followPitch(pitch, m_mode, *from, *to, m_voice);
	}

	void renderChordTrack(const Project& project, RenderSnapshot& snapshot)
	{
		const ChordTrack& chords = project.chords;
		if (chordTrack(project) == nullptr || chords.mute)
			return;
		const Progression progression = chordProgression(project);
		const auto samplesOf = [&project](time::Ticks tick)
		{ return project.tempo.toSamples(tick, project.sampleRate); };
		for (RenderChannel& channel : snapshot.channels)
		{
			// Live Input: what plays from each chord event on.
			if (const auto follow = chords.follow.find(channel.track);
				follow != chords.follow.end() && follow->second.liveInput != "off")
				for (const auto& [position, harmony] : progression)
					channel.liveInput.push_back(
						{samplesOf(position), classesOf(harmony, follow->second.liveInput == "scales")});
			// Acoustic Feedback: the chord events' voicings on the audition track, from each event to the next, the
			// last a bar long.
			if (chords.acousticFeedback && chords.audition != 0 && channel.track == chords.audition)
				for (size_t i = 0; i < progression.size(); ++i)
				{
					const time::Ticks start = progression[i].first;
					const int64_t bar = project.signatures.toBarsBeats(start).bar;
					const time::Ticks end = i + 1 < progression.size()
						? progression[i + 1].first
						: start + project.signatures.barStart(bar + 1) - project.signatures.barStart(bar);
					for (const int pitch : progression[i].second.voicing)
						channel.notes.push_back({samplesOf(start), samplesOf(end), static_cast<uint8_t>(pitch),
							static_cast<uint16_t>(49151), uint8_t{0}});
				}
		}
	}

	void recordChordEvents(Project& project, const std::vector<ChordNote>& notes, time::Ticks from, time::Ticks to,
		CommandContext& context)
	{
		if (chordTrack(project) == nullptr)
			return;
		std::vector<NoteSpan> spans;
		spans.reserve(notes.size());
		for (const ChordNote& note : notes)
			spans.push_back({note.start, note.end, note.pitch});
		replaceChords(project, detectChords(project, spans, true, true), from, to);
		settle(project, context);
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
			// Made from an audio event that is still there: undo of its removal brings the mark back.
			const bool fromAudio = event.audioEvent != 0 && project.audioEvents.count(event.audioEvent) != 0;
			events.push_back({{"id", event.id}, {"position", event.position.value},
				{"start", time::toSamples(event.position, project.tempo, project.sampleRate)}, {"chord", event.text},
				{"scale", harmony.chord.root < 0 ? std::string() : midi::scaleName(harmony.scale)},
				{"pitches", harmony.voicing}, {"audioEvent", fromAudio ? event.audioEvent : ObjectId{0}},
				{"detected", fromAudio ? event.detected : std::vector<std::string>()}});
		}
		Json scales = Json::array();
		for (const ChordEvent& event : chords.scales)
			scales.push_back({{"id", event.id}, {"position", event.position.value},
				{"start", time::toSamples(event.position, project.tempo, project.sampleRate)}, {"scale", event.text}});
		Json follow = Json::array();
		for (const auto& [id, settings] : chords.follow)
			follow.push_back(followJson(id, settings));
		return {{"track", track == nullptr ? ObjectId{0} : track->id}, {"events", events}, {"scales", scales},
			{"octaveOffset", chords.octaveOffset}, {"adaptiveVoicings", chords.adaptiveVoicings},
			{"automaticScales", chords.automaticScales}, {"follow", follow}, {"voicing", voicingJson(chords.voicing)},
			{"shiftChords", chords.shiftChords}, {"mute", chords.mute}, {"showScales", chords.showScales},
			{"recordEnable", chords.recordEnable}, {"audition", chords.audition},
			{"acousticFeedback", chords.acousticFeedback}};
	}

	void chordFromJson(Project& project, const Json& json)
	{
		const Track* track = chordTrack(project);
		if (track == nullptr)
			return;
		const time::TimeBase base = track->timeBase;
		ChordTrack& chords = project.chords;
		const auto read = [base](const Json& item, std::string text)
		{
			ChordEvent event;
			event.id = item.at("id").get<ObjectId>();
			event.position = {base, item.at("position").get<int64_t>()};
			event.text = std::move(text);
			return event;
		};
		// The text as chord.add would take it: unknown chords are undefined (X), unknown scales are left out.
		const auto chordText = [](const std::string& text)
		{ return midi::chordName(midi::parseChord(text).value_or(midi::ChordSymbol())); };
		// Events before the project start are left out, as chord.add and chord.set refuse them.
		const auto inProject = [](const Json& item) { return item.value("position", int64_t{-1}) >= 0; };
		for (const Json& item : json.value("events", Json::array()))
			if (inProject(item))
			{
				ChordEvent event = read(item, chordText(item.value("chord", std::string())));
				event.audioEvent = item.value("audioEvent", ObjectId{0});
				for (const Json& detected : item.value("detected", Json::array()))
					if (detected.is_string() && midi::parseChord(detected.get<std::string>()).has_value())
						event.detected.push_back(chordText(detected.get<std::string>()));
				chords.events.push_back(event);
			}
		for (const Json& item : json.value("scales", Json::array()))
			if (const std::optional<midi::Scale> scale = midi::parseScale(item.value("scale", std::string()));
				scale.has_value() && inProject(item))
				chords.scales.push_back(read(item, midi::scaleName(*scale)));
		// In timeline order, and one event of a kind at a position, the first, as chord.add refuses a second.
		for (std::vector<ChordEvent>* events : {&chords.events, &chords.scales})
		{
			std::stable_sort(events->begin(), events->end(),
				[](const ChordEvent& a, const ChordEvent& b) { return a.position.value < b.position.value; });
			events->erase(std::unique(events->begin(), events->end(), [](const ChordEvent& a, const ChordEvent& b)
							  { return a.position.value == b.position.value; }),
				events->end());
		}
		chords.octaveOffset = std::clamp(json.value("octaveOffset", 0), -3, 3);
		chords.adaptiveVoicings = json.value("adaptiveVoicings", true);
		chords.automaticScales = json.value("automaticScales", true);
		// Unknown settings are their defaults.
		const auto known = [](const std::vector<std::string>& names, const std::string& name, const char* fallback)
		{ return among(names, name) ? name : std::string(fallback); };
		const Json voicing = json.value("voicing", Json::object());
		chords.voicing.library = known(midi::voicingLibraries(), voicing.value("library", std::string()), "basic");
		chords.voicing.style =
			known(midi::voicingStyles(chords.voicing.library), voicing.value("style", std::string()), "");
		chords.voicing.addRoot = voicing.value("addRoot", true);
		chords.voicing.duplicateRoot = voicing.value("duplicateRoot", false);
		chords.voicing.fattenUp = voicing.value("fattenUp", false);
		const auto pitch = [&voicing](const char* key, int fallback)
		{
			const int value = voicing.value(key, fallback);
			return value >= 0 && value <= 127 ? value : fallback;
		};
		chords.voicing.lowestRoot = pitch("lowestRoot", 36);
		chords.voicing.lowest = pitch("lowest", 52);
		chords.voicing.highest = pitch("highest", 84);
		if (chords.voicing.lowest + 12 > chords.voicing.highest)
		{
			chords.voicing.lowest = 52;
			chords.voicing.highest = 84;
		}
		chords.shiftChords =
			std::clamp(json.value("shiftChords", time::Ticks{0}), -time::g_ticksPerQuarter, time::g_ticksPerQuarter);
		chords.mute = json.value("mute", false);
		chords.showScales = json.value("showScales", false);
		chords.recordEnable = json.value("recordEnable", false);
		chords.audition = json.value("audition", ObjectId{0});
		if (!canFollow(project, chords.audition))
			chords.audition = 0;
		chords.acousticFeedback = json.value("acousticFeedback", false);
		for (const Json& item : json.value("follow", Json::array()))
		{
			const auto follower = item.value("track", ObjectId{0});
			if (!canFollow(project, follower))
				continue;
			ChordFollow follow;
			follow.mode = known(midi::followModeNames(), item.value("mode", std::string()), "off");
			for (const Json& chord : item.value("source", Json::array()))
				follow.source.push_back(
					{chord.value("position", time::Ticks{0}), chordText(chord.value("chord", std::string("X")))});
			follow.voicings = known(trackVoicingNames(), item.value("voicings", std::string()), "chordTrack");
			const std::vector<std::string>& voices = midi::voiceNames();
			const auto voice = std::find(voices.begin(), voices.end(), item.value("voice", std::string()));
			follow.voice = voice == voices.end() ? 0 : static_cast<int>(voice - voices.begin());
			follow.master = item.value("master", ObjectId{0});
			if (follow.master == follower || !canFollow(project, follow.master))
				follow.master = 0;
			follow.liveInput = known(liveInputNames(), item.value("liveInput", std::string()), "off");
			chords.follow[follower] = follow;
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
					{"automaticScales", booleanSchema}, {"follow", {{"type", "array"}}},
					{"voicing", {{"type", "object"}}}, {"shiftChords", integerSchema}, {"mute", booleanSchema},
					{"showScales", booleanSchema}, {"recordEnable", booleanSchema}, {"audition", integerSchema},
					{"acousticFeedback", booleanSchema}}},
			{"required",
				{"track", "events", "scales", "octaveOffset", "adaptiveVoicings", "automaticScales", "follow",
					"voicing", "shiftChords", "mute", "showScales", "recordEnable", "audition", "acousticFeedback"}}};
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
				"seventh and so on) rather than the scale event's playing there; voicing, the voicing library "
				"(reference manual pp. 1333-1335): library basic (Octave Offset only), piano or guitar, style (the "
				"library subset; piano: triads, triadsMaj9, triadsMaj9Min9, fourNote, fourNoteOpen, fiveNote; guitar: "
				"triads, fourNote, threeString, modernJazz) and the Custom Voicing panel's piano options addRoot, "
				"duplicateRoot, fattenUp and range lowestRoot, lowest, highest (pitches 0 to 127, highest an octave or "
				"more above lowest); shiftChords (Shift Chords, ticks, at most a quarter note either way): a negative "
				"shift lets each chord event take the notes played that much before it; mute (Mute Chord Track): no "
				"track follows, no Live Input and no auditioning; showScales (Show Scales), the scale lane; "
				"recordEnable: the notes that MIDI and instrument tracks record, or play while they only monitor, "
				"during a recording become chord events, each take's over it (reference manual p. 1345); audition "
				"(Select Track for Auditioning): a MIDI or instrument track, 0 for none; acousticFeedback: the chord "
				"events' voicings play on it, and chord.audition plays there.",
				objectSchema({{"octaveOffset", integerSchema}, {"adaptiveVoicings", booleanSchema},
					{"automaticScales", booleanSchema},
					{"voicing",
						objectSchema({{"library", {{"type", "string"}, {"enum", midi::voicingLibraries()}}},
							{"style", stringSchema}, {"addRoot", booleanSchema}, {"duplicateRoot", booleanSchema},
							{"fattenUp", booleanSchema}, {"lowestRoot", integerSchema}, {"lowest", integerSchema},
							{"highest", integerSchema}})},
					{"shiftChords", integerSchema}, {"mute", booleanSchema}, {"showScales", booleanSchema},
					{"recordEnable", booleanSchema}, {"audition", idSchema}, {"acousticFeedback", booleanSchema}}),
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
				if (parameters.contains("voicing"))
					setVoicing(chords.voicing, parameters.at("voicing"));
				chords.shiftChords = parameters.value("shiftChords", chords.shiftChords);
				require(std::abs(chords.shiftChords) <= time::g_ticksPerQuarter,
					"shiftChords must be at most a quarter note either way");
				chords.mute = parameters.value("mute", chords.mute);
				chords.showScales = parameters.value("showScales", chords.showScales);
				chords.recordEnable = parameters.value("recordEnable", chords.recordEnable);
				chords.audition = parameters.value("audition", chords.audition);
				require(chords.audition == 0 || canFollow(project, chords.audition),
					"the chord track auditions on a MIDI or instrument track");
				chords.acousticFeedback = parameters.value("acousticFeedback", chords.acousticFeedback);
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
				"notes) or known with chord (Apply a Known Chord, everywhere). Drum-mapped tracks don't follow. "
				"singleVoice plays one voice of the new chord's voicing, voice: soprano (the highest, the default), "
				"alto, tenor or bass (the lowest). voicings (This Track's Voicings): chordTrack (the default, the "
				"chord track's voicings) or a library, basic, piano or guitar. master: a Single Voice track's master "
				"track, whose voicings it takes, 0 for none. liveInput (Live Input, p. 1338): chords or scales move "
				"the notes played into the track to the nearest note of the chord or scale playing at the transport "
				"position, off doesn't. Without mode, the mode stays.",
				objectSchema({{"track", idSchema}, {"mode", {{"type", "string"}, {"enum", midi::followModeNames()}}},
								 {"sync", {{"type", "string"}, {"enum", {"direct", "analyze", "known"}}}},
								 {"chord", stringSchema}, {"voice", {{"type", "string"}, {"enum", midi::voiceNames()}}},
								 {"voicings", {{"type", "string"}, {"enum", trackVoicingNames()}}},
								 {"master", idSchema}, {"liveInput", {{"type", "string"}, {"enum", liveInputNames()}}}},
					{"track"}),
				objectSchema({{"track", integerSchema}, {"mode", stringSchema}, {"source", {{"type", "array"}}},
								 {"voicings", stringSchema}, {"voice", stringSchema}, {"master", integerSchema},
								 {"liveInput", stringSchema}},
					{"track", "mode", "source", "voicings", "voice", "master", "liveInput"}),
				false},
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				require(chordTrack(project) != nullptr, "the project has no chord track");
				const auto track = parameters.at("track").get<ObjectId>();
				require(canFollow(project, track), "only MIDI and instrument tracks follow the chord track");
				ChordFollow& follow = project.chords.follow[track];
				const std::string mode = parameters.value("mode", follow.mode);
				require(among(midi::followModeNames(), mode), "unknown mode " + mode);
				const std::vector<std::string>& voices = midi::voiceNames();
				if (parameters.contains("voice"))
					follow.voice = static_cast<int>(
						std::find(voices.begin(), voices.end(), parameters.at("voice").get<std::string>()) -
						voices.begin());
				follow.voicings = parameters.value("voicings", follow.voicings);
				follow.master = parameters.value("master", follow.master);
				require(follow.master == 0 || (follow.master != track && canFollow(project, follow.master)),
					"a master is another MIDI or instrument track");
				follow.liveInput = parameters.value("liveInput", follow.liveInput);
				require(among(liveInputNames(), follow.liveInput), "unknown liveInput " + follow.liveInput);
				require(among(trackVoicingNames(), follow.voicings), "unknown voicings " + follow.voicings);
				if (!parameters.contains("mode") && !parameters.contains("sync"))
				{
					context.emit("chord.changed", chordJson(project));
					return followJson(track, follow);
				}
				const std::string sync = parameters.value("sync", std::string());
				if (sync == "known")
				{
					require(parameters.contains("chord"), "sync known needs a chord");
					follow.source = {{0, midi::chordName(chordFrom(parameters.at("chord").get<std::string>()))}};
				}
				else if (sync == "analyze")
				{
					follow.source = detectChords(project, notesOf(project, partsOf(project, track), 0), true, true);
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
					const int64_t bar = project.signatures.toBarsBeats(start).bar;
					const time::Ticks end = i + 1 < events.size()
						? progression[i + 1].first
						: start + project.signatures.barStart(bar + 1) - project.signatures.barStart(bar);
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
				"Than, default 0) don't count. detectArpeggios (Detect Arpeggios, default false): a note sounds on to "
				"the end of its bar, so chords played one note after another in a bar count, from where their first "
				"note starts. sustainPedal (Interpret Sustain Pedal, default false): a "
				"note that ends while the sustain pedal (controller 64) is down lasts until it is let up. The chord "
				"events from the first note's start to the last one's end are replaced. Returns the events made.",
				objectSchema({{"parts", idsSchema}, {"tracks", idsSchema}, {"includeBass", booleanSchema},
					{"includeTensions", booleanSchema}, {"minLength", integerSchema},
					{"detectArpeggios", booleanSchema}, {"sustainPedal", booleanSchema}}),
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
				const std::vector<NoteSpan> notes = notesOf(project, parts,
					parameters.value("minLength", time::Ticks{0}), parameters.value("sustainPedal", false));
				const std::vector<ChordSource> found =
					detectChords(project, notes, parameters.value("includeBass", true),
						parameters.value("includeTensions", true), parameters.value("detectArpeggios", false));
				if (chordTrack(project) == nullptr)
					runEdit("track.add", project, {{"type", "chord"}}, context);
				time::Ticks from = 0;
				time::Ticks to = 0;
				if (!notes.empty())
				{
					from = notes.front().start;
					to = notes.front().end;
					for (const NoteSpan& note : notes)
					{
						from = std::min(from, note.start);
						to = std::max(to, note.end);
					}
				}
				const std::vector<ObjectId> made = replaceChords(project, found, from, to);
				settle(project, context);
				Json result = Json::array();
				for (const ObjectId id : made)
					result.push_back(eventJson(project, id));
				return Json{{"events", result}};
			});
		const Json partsResult = {
			{"type", "object"}, {"properties", {{"notes", {{"type", "array"}}}}}, {"required", {"notes"}}};
		// Changes the pitches of parts' notes: each note's new pitch, by its position (ticks) and pitch. Returns the
		// notes changed.
		const auto repitch = [this](Project& project, CommandContext& context, const Json& parameters,
								 const std::function<int(time::Ticks, int, ObjectId)>& pitchOf)
		{
			require(parameters.contains("parts"), "give parts");
			require(chordTrack(project) != nullptr, "the project has no chord track");
			Json changed = Json::array();
			for (const Json& item : parameters.at("parts"))
			{
				const auto id = item.get<ObjectId>();
				require(project.midiParts.count(id) != 0, "no MIDI part " + std::to_string(id));
				const MidiPart& part = project.midiParts.at(id);
				const time::Ticks start = ticksOf(project, part.position);
				Json notes = Json::array();
				for (const auto& [noteId, note] : part.notes)
				{
					const int pitch = std::clamp(pitchOf(start + note.position, note.pitch, id), 0, 127);
					if (pitch != note.pitch)
						notes.push_back({{"id", noteId}, {"pitch", pitch}});
				}
				if (notes.empty())
					continue;
				const Json set = runEdit("note.set", project, {{"part", id}, {"notes", notes}}, context);
				for (const Json& note : set.at("notes"))
					changed.push_back(note);
			}
			return Json{{"notes", changed}};
		};
		add(engine,
			{"chord.map", 1,
				"Map to Chord Track (reference manual p. 1341): moves the notes of MIDI parts to the chord track's "
				"chords for good, as Follow Chord Track with mode would play them: each part's own chords (as "
				"chord.fromMidi finds them, C where it finds none) map to the chord events playing at each note. mode: "
				"chordsAndScales (the default), chords, scales, rootNotes or voicings. Returns the notes changed.",
				objectSchema({{"parts", idsSchema},
								 {"mode",
									 {{"type", "string"},
										 {"enum", {"chordsAndScales", "chords", "scales", "rootNotes", "voicings"}}}}},
					{"parts"}),
				partsResult, false},
			[repitch](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const std::string mode = parameters.value("mode", std::string("chordsAndScales"));
				const Progression to = chordProgression(project);
				const midi::VoicingStyle style = styleOf(project.chords.voicing);
				std::map<ObjectId, Progression> sources;
				return repitch(project, context, parameters,
					[&](time::Ticks at, int pitch, ObjectId part)
					{
						if (sources.count(part) == 0)
						{
							std::vector<std::pair<time::Ticks, std::string>> chords;
							for (const ChordSource& chord :
								detectChords(project, notesOf(project, {part}, 0), true, true))
								chords.emplace_back(chord.position, chord.chord);
							if (chords.empty())
								chords.emplace_back(0, "C");
							sources[part] = progressionOf(project, chords, false, style);
						}
						const midi::Harmony* fromHarmony = harmonyAt(sources.at(part), at);
						const midi::Harmony* toHarmony = harmonyAt(to, at);
						if (fromHarmony == nullptr)
							fromHarmony = &sources.at(part).front().second;
						return toHarmony == nullptr ? pitch : midi::followPitch(pitch, mode, *fromHarmony, *toHarmony);
					});
			});
		add(engine,
			{"chord.assignVoices", 1,
				"Assign Voices to Notes (reference manual p. 1342): moves each note of MIDI parts to the nearest pitch "
				"of the voicing of the chord event playing at its start (the lower one on a tie), so the notes play "
				"the chord track's voices. Notes before the first chord event and over undefined chords stay. Returns "
				"the notes changed.",
				objectSchema({{"parts", idsSchema}}, {"parts"}), partsResult, false},
			[repitch](Project& project, CommandContext& context)
			{
				const Progression progression = chordProgression(project);
				return repitch(project, context, context.parameters(),
					[&](time::Ticks at, int pitch, ObjectId)
					{
						const midi::Harmony* harmony = harmonyAt(progression, at);
						if (harmony == nullptr || harmony->voicing.empty())
							return pitch;
						int nearest = harmony->voicing.front();
						for (const int voice : harmony->voicing)
							if (std::abs(voice - pitch) < std::abs(nearest - pitch))
								nearest = voice;
						return nearest;
					});
			});
		add(engine,
			{"chord.fromAudio", 1,
				"Create Chord Events from audio events (reference manual pp. 1343-1344): chord events from the chords "
				"heard in audio events, on the chord track, which is added if the project has none. Each quarter note "
				"of an event (on the project's beat grid) is matched against the triads, sus chords and sevenths on "
				"every root by its pitch classes; a quarter note that differs from the one before starts a chord "
				"event, and silence makes none. Each event has the audio event (audioEvent) and the chords detected "
				"over it, the most likely first (detected), as the chord assistant's Detected tab lists them. The "
				"chord events within an audio event are replaced. The audio should be western music in equal "
				"temperament. Returns the events made.",
				objectSchema({{"events", idsSchema}}, {"events"}),
				{{"type", "object"}, {"properties", {{"events", {{"type", "array"}}}}}, {"required", {"events"}}},
				false},
			[this](Project& project, CommandContext& context)
			{
				std::vector<ObjectId> audioEvents;
				for (const Json& item : context.parameters().at("events"))
				{
					const auto id = item.get<ObjectId>();
					require(project.audioEvents.count(id) != 0, "no audio event " + std::to_string(id));
					audioEvents.push_back(id);
				}
				if (chordTrack(project) == nullptr)
					runEdit("track.add", project, {{"type", "chord"}}, context);
				std::vector<ObjectId> made;
				for (const ObjectId id : audioEvents)
				{
					const AudioEvent& event = project.audioEvents.at(id);
					const MediaFile& media = project.media.at(project.clips.at(event.clip).media);
					const EventSpan span = eventSpan(project, event);
					const time::Ticks from = project.tempo.toTicks(span.start, project.sampleRate);
					const time::Ticks to = project.tempo.toTicks(span.end, project.sampleRate);
					std::unique_ptr<AudioFileReader> reader;
					try
					{
						reader = AudioFileReader::open(media.path);
					}
					catch (const AudioFileError& error)
					{
						throw CommandError("can't read " + media.path + ": " + error.what());
					}
					// Quarter notes, each with its chroma; runs of the same chord become one event.
					struct Run
					{
						time::Ticks position = 0;
						std::string chord;
						std::array<double, 12> chroma{};
					};
					std::vector<Run> runs;
					for (time::Ticks beat = (from / time::g_ticksPerQuarter) * time::g_ticksPerQuarter; beat < to;
						beat += time::g_ticksPerQuarter)
					{
						const time::Ticks start = std::max(beat, from);
						const time::Ticks end = std::min(beat + time::g_ticksPerQuarter, to);
						const time::Samples first = project.tempo.toSamples(start, project.sampleRate);
						const time::Samples count = project.tempo.toSamples(end, project.sampleRate) - first;
						const std::array<double, 12> chroma =
							chromaOf(eventAudio(project, event, *reader, media, first, count), media.sampleRate);
						const std::vector<std::string> found = chordsOf(chroma);
						if (found.empty())
							continue;
						if (runs.empty() || runs.back().chord != found.front())
							runs.push_back({start, found.front(), {}});
						for (size_t i = 0; i < chroma.size(); ++i)
							runs.back().chroma.at(i) += chroma.at(i);
					}
					std::vector<ChordSource> chords;
					chords.reserve(runs.size());
					for (const Run& run : runs)
						chords.push_back({run.position, run.chord});
					const std::vector<ObjectId> added = replaceChords(project, chords, from, to);
					for (size_t i = 0; i < added.size(); ++i)
						for (ChordEvent& chordEvent : project.chords.events)
							if (chordEvent.id == added[i])
							{
								chordEvent.audioEvent = id;
								chordEvent.detected = chordsOf(runs[i].chroma);
							}
					made.insert(made.end(), added.begin(), added.end());
				}
				settle(project, context);
				Json result = Json::array();
				for (const ObjectId id : made)
					result.push_back(eventJson(project, id));
				return Json{{"events", result}};
			});

		// What chord.audition plays, to end it.
		auto auditioned = std::make_shared<std::pair<ObjectId, std::vector<int>>>();
		addControl(engine,
			{"chord.audition", 1,
				"Auditions a chord event's voicing or a scale event's notes (from its root up an octave), or a chord "
				"given as chord.add takes it, on the chord track's audition track (chord.setTrack audition, "
				"reference manual p. 1329), as if from its MIDI input (midi.inject): on (the default) plays it, off "
				"ends what played. Refused without an audition track, with the audition track frozen (it takes no "
				"MIDI) or with the chord track muted. Returns the "
				"pitches.",
				objectSchema({{"event", idSchema}, {"chord", stringSchema}, {"on", booleanSchema}}),
				objectSchema({{"pitches", {{"type", "array"}}}}, {"pitches"}), false},
			[this, auditioned](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const Project& project = m_project;
				const bool on = parameters.value("on", true);
				std::vector<int> pitches;
				if (on)
				{
					require(project.chords.audition != 0 && project.trackById.count(project.chords.audition) != 0,
						"the chord track has no track for auditioning");
					require(!project.trackById.at(project.chords.audition).frozen,
						"the chord track's audition track is frozen, so it takes no MIDI");
					require(!project.chords.mute, "the chord track is muted");
					require(parameters.contains("event") != parameters.contains("chord"), "give an event or a chord");
					if (parameters.contains("chord"))
					{
						pitches = midi::voiceChord(chordFrom(parameters.at("chord").get<std::string>()),
							{project.chords.octaveOffset, false}, {}, styleOf(project.chords.voicing));
					}
					else
					{
						const auto id = parameters.at("event").get<ObjectId>();
						const Json event = eventJson(project, id);
						require(!event.empty(), "no chord track event " + std::to_string(id));
						pitches = event.contains("pitches") ? event.at("pitches").get<std::vector<int>>()
															: scaleNotes(event.at("scale").get<std::string>());
					}
				}
				const auto send = [&context](ObjectId channel, const std::vector<int>& notes, bool down)
				{
					Json messages = Json::array();
					for (const int pitch : notes)
						messages.push_back({{"position", 0}, {"bytes", {down ? 0x90 : 0x80, pitch, down ? 100 : 0}}});
					if (channel != 0 && !messages.empty())
						context.call("midi.inject", {{"channel", channel}, {"messages", messages}});
				};
				// The held chord's note-offs are skipped only if its channel is no longer an unfrozen MIDI or
				// instrument track's (its track was removed or frozen, or another project loaded), so that a channel
				// that takes no MIDI never blocks the next audition. Other failures fail the command with the chord
				// still held, so that no note hangs.
				const auto& held = *auditioned;
				const bool there = std::any_of(project.trackById.begin(), project.trackById.end(),
					[&held](const auto& entry)
					{
						const Track& track = entry.second;
						return track.channel == held.first && !track.frozen &&
							(track.type == TrackType::Midi || track.type == TrackType::Instrument);
					});
				if (there)
					send(held.first, held.second, false);
				*auditioned = {};
				if (on)
				{
					const ObjectId channel = project.trackById.at(project.chords.audition).channel;
					send(channel, pitches, true);
					*auditioned = {channel, pitches};
				}
				return Json{{"pitches", pitches}};
			});
		add(engine,
			{"chord.get", 1,
				"The chord track: its id (0 for none); its chord events in timeline order (id, position in the "
				"track's time base, start in samples, chord, scale and pitches, the chord's voicing from the lowest "
				"up; for events made from audio, audioEvent and detected, the chords that fit best, else 0 and "
				"none); its scale events (id, position, start, scale); its settings as chord.setTrack takes them; "
				"and follow, the tracks that follow it with their mode, voice, voicings, master, liveInput and "
				"source, the chords their notes were played over (position in ticks, chord).",
				objectSchema({}), chordResult, true},
			[](Project& project, CommandContext&) { return chordJson(project); });
	}
}
