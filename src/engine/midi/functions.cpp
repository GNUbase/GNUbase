// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/midi/functions.h"

#include "engine/midi/midivalues.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace gnubase::engine::midi
{
	using time::Ticks;

	namespace
	{
		constexpr int g_sustainPedal = 64;
		constexpr int g_pedalDown = 64; // MIDI 1.0 values from 64 on press a switch

		int floorDiv(int value, int divisor)
		{
			const int quotient = value / divisor;
			return (value % divisor != 0 && (value < 0) != (divisor < 0)) ? quotient - 1 : quotient;
		}

		int floorMod(int value, int divisor)
		{
			return value - (floorDiv(value, divisor) * divisor);
		}

		// Scales.

		struct ScaleEntry
		{
			ScaleType type;
			const char* name;
			int size;
			std::array<int, 7> steps; // semitones above the root
		};

		constexpr ScaleEntry g_scales[] = {
			{ScaleType::Major, "major", 7, {0, 2, 4, 5, 7, 9, 11}},
			{ScaleType::Minor, "minor", 7, {0, 2, 3, 5, 7, 8, 10}},
			{ScaleType::HarmonicMinor, "harmonicMinor", 7, {0, 2, 3, 5, 7, 8, 11}},
			{ScaleType::MelodicMinor, "melodicMinor", 7, {0, 2, 3, 5, 7, 9, 11}},
			{ScaleType::Dorian, "dorian", 7, {0, 2, 3, 5, 7, 9, 10}},
			{ScaleType::Phrygian, "phrygian", 7, {0, 1, 3, 5, 7, 8, 10}},
			{ScaleType::Lydian, "lydian", 7, {0, 2, 4, 6, 7, 9, 11}},
			{ScaleType::Mixolydian, "mixolydian", 7, {0, 2, 4, 5, 7, 9, 10}},
			{ScaleType::Locrian, "locrian", 7, {0, 1, 3, 5, 6, 8, 10}},
			{ScaleType::MajorPentatonic, "majorPentatonic", 5, {0, 2, 4, 7, 9, 0, 0}},
			{ScaleType::MinorPentatonic, "minorPentatonic", 5, {0, 3, 5, 7, 10, 0, 0}},
			{ScaleType::Blues, "blues", 6, {0, 3, 5, 6, 7, 10, 0}},
			{ScaleType::WholeTone, "wholeTone", 6, {0, 2, 4, 6, 8, 10, 0}},
		};

		const ScaleEntry& scaleEntry(ScaleType type)
		{
			for (const ScaleEntry& entry : g_scales)
				if (entry.type == type)
					return entry;
			return g_scales[0]; // not reached: every type is in the table
		}

		// The degree of a pitch in a scale, or -1 if the scale doesn't have it.
		int degreeOf(int pitch, const Scale& scale)
		{
			const ScaleEntry& entry = scaleEntry(scale.type);
			const int step = floorMod(pitch - scale.root, 12);
			for (int i = 0; i < entry.size; ++i)
				if (entry.steps[static_cast<size_t>(i)] == step)
					return i;
			return -1;
		}

		// The closest pitch of a scale, the lower one on a tie.
		int closestInScale(int pitch, const Scale& scale)
		{
			for (int distance = 0; distance < 12; ++distance)
			{
				if (degreeOf(pitch - distance, scale) >= 0)
					return pitch - distance;
				if (degreeOf(pitch + distance, scale) >= 0)
					return pitch + distance;
			}
			return pitch; // not reached: every scale has its root
		}

		int correctToScale(int pitch, const Scale& from, const Scale& to)
		{
			// The new root as close as possible to the old one: C to B goes down a semitone, not up eleven.
			const int shift = floorMod(to.root - from.root + 6, 12) - 6;
			const ScaleEntry& source = scaleEntry(from.type);
			const ScaleEntry& target = scaleEntry(to.type);
			const int degree = degreeOf(pitch, from);
			if (degree >= 0 && source.size == target.size)
			{
				const int octave = floorDiv(pitch - from.root, 12);
				return from.root + shift + (12 * octave) + target.steps[static_cast<size_t>(degree)];
			}
			return closestInScale(pitch + shift, to);
		}

		// Moves a pitch by octaves into a range; where no octave fits, to the limit it crossed.
		int intoRange(int pitch, int lowest, int highest)
		{
			if (pitch > highest)
			{
				pitch -= 12 * ((pitch - highest + 11) / 12);
				return pitch < lowest ? highest : pitch;
			}
			if (pitch < lowest)
			{
				pitch += 12 * ((lowest - pitch + 11) / 12);
				return pitch > highest ? lowest : pitch;
			}
			return pitch;
		}

		// Notes.

		Ticks noteEnd(const Note& note)
		{
			return note.position + note.length;
		}

		// Notes in the order they start, then by pitch and id.
		bool startsBefore(const Note* a, const Note* b)
		{
			return std::tie(a->position, a->pitch, a->id) < std::tie(b->position, b->pitch, b->id);
		}

		// The notes of each MIDI channel, in the order they start, then by pitch and id.
		std::map<uint8_t, std::vector<Note*>> notesByChannel(MidiPart& part)
		{
			std::map<uint8_t, std::vector<Note*>> channels;
			for (auto& [id, note] : part.notes)
				channels[note.channel].push_back(&note);
			for (auto& [channel, notes] : channels)
				std::sort(notes.begin(), notes.end(), startsBefore);
			return channels;
		}

		// The first note after index that starts later than it and is a candidate.
		template <typename Candidate>
		const Note* nextLater(const std::vector<Note*>& notes, size_t index, Candidate candidate)
		{
			for (size_t i = index + 1; i < notes.size(); ++i)
				if (notes[i]->position > notes[index]->position && candidate(*notes[i]))
					return notes[i];
			return nullptr;
		}

		void eraseNotes(MidiPart& part, const std::vector<ObjectId>& ids)
		{
			for (const ObjectId id : ids)
				part.notes.erase(id);
		}

		// Controllers.

		bool isCurve(ControllerType type)
		{
			return type == ControllerType::Control || type == ControllerType::PitchBend ||
				type == ControllerType::ChannelPressure || type == ControllerType::PolyPressure;
		}

		// Events of one controller on one channel form a lane.
		using LaneKey = std::tuple<ControllerType, uint8_t, uint8_t>; // type, channel, number

		LaneKey laneOf(const Controller& controller)
		{
			const bool numbered =
				controller.type == ControllerType::Control || controller.type == ControllerType::PolyPressure;
			return {controller.type, controller.channel, numbered ? controller.number : uint8_t{0}};
		}

		bool isSustain(const Controller& controller)
		{
			return controller.type == ControllerType::Control && controller.number == g_sustainPedal;
		}

		template <typename Predicate> void eraseControllers(MidiPart& part, Predicate predicate)
		{
			for (auto it = part.controllers.begin(); it != part.controllers.end();)
				it = predicate(it->second) ? part.controllers.erase(it) : std::next(it);
		}

		// Positions in time.

		Ticks partStart(const Project& project, const MidiPart& part)
		{
			return time::toTicks(part.position, project.tempo, project.sampleRate);
		}

		Ticks partEnd(const Project& project, const MidiPart& part)
		{
			return time::toTicks(time::Position{part.position.base, part.position.value + part.length}, project.tempo,
				project.sampleRate);
		}

		// A project position in ticks as a position in a track's time base.
		time::Position trackPosition(const Project& project, const Track& track, Ticks tick)
		{
			return time::rebase({time::TimeBase::Musical, tick}, track.timeBase, project.tempo, project.sampleRate);
		}

		// Places a part from start to end, both project ticks, on its track.
		void place(const Project& project, MidiPart& part, Ticks start, Ticks end)
		{
			const Track& track = project.trackById.at(part.track);
			part.position = trackPosition(project, track, start);
			part.length = std::max<int64_t>(1, trackPosition(project, track, end).value - part.position.value);
		}

		// Tracks.

		// What a MIDI track made from another track plays through: the instrument the other track is or plays.
		ObjectId instrumentOf(const Project& project, ObjectId trackId)
		{
			const Track& track = project.trackById.at(trackId);
			if (track.channel == 0)
				return 0;
			return track.type == TrackType::Instrument ? track.channel : project.channels.at(track.channel).output;
		}

		// Adds a MIDI track among parent's tracks at index, routed to an instrument channel (or 0).
		ObjectId addMidiTrack(Project& project, std::string name, ObjectId parent, size_t index, ObjectId instrument,
			time::TimeBase timeBase)
		{
			Track track;
			track.id = project.newId();
			track.type = TrackType::Midi;
			track.name = std::move(name);
			track.timeBase = timeBase;
			track.parent = parent;
			const Channel channel{project.newId(), track.id, 0, 0, instrument, {}};
			track.channel = channel.id;
			project.channels[channel.id] = channel;
			std::vector<ObjectId>& siblings = parent == 0 ? project.tracks : project.trackById.at(parent).children;
			siblings.insert(siblings.begin() + static_cast<std::ptrdiff_t>(index), track.id);
			project.trackById[track.id] = track;
			return track.id;
		}

		std::string defaultMidiTrackName(const Project& project)
		{
			const auto count = std::count_if(project.trackById.begin(), project.trackById.end(),
				[](const auto& entry) { return entry.second.type == TrackType::Midi; });
			const std::string number = std::to_string(count + 1);
			return std::string("MIDI") + (number.size() < 2 ? " 0" : " ") + number;
		}

		// "C3" for middle C (60), "F#1" for 42.
		std::string pitchName(int pitch)
		{
			static constexpr std::array<const char*, 12> names{
				"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
			return std::string(names[static_cast<size_t>(pitch % 12)]) + std::to_string((pitch / 12) - 2);
		}

		bool isNoteTrack(const Track& track)
		{
			return track.type == TrackType::Midi || track.type == TrackType::Instrument;
		}

		void addUnique(std::vector<ObjectId>& ids, ObjectId id)
		{
			if (std::find(ids.begin(), ids.end(), id) == ids.end())
				ids.push_back(id);
		}
	}

	const char* scaleTypeName(ScaleType type)
	{
		return scaleEntry(type).name;
	}

	std::optional<ScaleType> scaleTypeFromName(const std::string& name)
	{
		for (const ScaleEntry& entry : g_scales)
			if (name == entry.name)
				return entry.type;
		return std::nullopt;
	}

	std::vector<std::string> scaleTypeNames()
	{
		std::vector<std::string> names;
		for (const ScaleEntry& entry : g_scales)
			names.emplace_back(entry.name);
		return names;
	}

	int transposePitch(int pitch, const TransposeSettings& settings)
	{
		int result = pitch + settings.semitones;
		if (settings.from && settings.to)
			result = correctToScale(result, *settings.from, *settings.to);
		if (settings.keepInRange)
			result = intoRange(result, settings.lowest, settings.highest);
		return intoRange(result, 0, 127);
	}

	void transpose(MidiPart& part, const NoteSelection& selection, const TransposeSettings& settings)
	{
		for (auto& [id, note] : part.notes)
			if (selection.contains(id))
				note.pitch = static_cast<uint8_t>(transposePitch(note.pitch, settings));
		if (selection.all())
			for (auto& [id, controller] : part.controllers)
				if (controller.type == ControllerType::PolyPressure)
					controller.number = static_cast<uint8_t>(transposePitch(controller.number, settings));
	}

	void legato(MidiPart& part, const NoteSelection& selection, time::Ticks overlap, bool betweenSelected)
	{
		for (auto& [channel, notes] : notesByChannel(part))
			for (size_t i = 0; i < notes.size(); ++i)
			{
				if (!selection.contains(notes[i]->id))
					continue;
				const Note* next = nextLater(
					notes, i, [&](const Note& note) { return !betweenSelected || selection.contains(note.id); });
				if (next != nullptr)
					notes[i]->length = std::max<Ticks>(1, next->position - notes[i]->position + overlap);
			}
	}

	void fixLengths(MidiPart& part, const NoteSelection& selection, time::Ticks length)
	{
		for (auto& [id, note] : part.notes)
			if (selection.contains(id))
				note.length = length;
	}

	void fixVelocities(MidiPart& part, const NoteSelection& selection, uint16_t velocity)
	{
		for (auto& [id, note] : part.notes)
			if (selection.contains(id))
				note.velocity = velocity;
	}

	void pedalToNoteLength(MidiPart& part, const NoteSelection& selection)
	{
		// Each channel's pedal events in order.
		std::map<uint8_t, std::vector<const Controller*>> pedals;
		for (const auto& [id, controller] : part.controllers)
			if (isSustain(controller))
				pedals[controller.channel].push_back(&controller);
		for (auto& [channel, events] : pedals)
			std::sort(events.begin(), events.end(), [](const Controller* a, const Controller* b)
				{ return std::tie(a->position, a->id) < std::tie(b->position, b->id); });
		const auto down = [](const Controller* pedal) { return controllerToMidi1(pedal->value) >= g_pedalDown; };

		std::set<uint8_t> processed;
		for (auto& [channel, notes] : notesByChannel(part))
			for (size_t i = 0; i < notes.size(); ++i)
			{
				Note& note = *notes[i];
				if (!selection.contains(note.id))
					continue;
				processed.insert(channel);
				const auto found = pedals.find(channel);
				if (found == pedals.end())
					continue;
				const std::vector<const Controller*>& events = found->second;
				// At the note's end its note-off comes before a pedal event at the same tick.
				const std::vector<const Controller*>::const_iterator after = std::partition_point(
					events.begin(), events.end(), [&](const Controller* pedal) { return pedal->position < noteEnd(note); });
				if (after == events.begin() || !down(*std::prev(after)))
					continue;
				const std::vector<const Controller*>::const_iterator release = std::find_if_not(after, events.end(), down);
				Ticks newEnd = release == events.end() ? std::max(part.length, noteEnd(note)) : (*release)->position;
				const Note* again = nextLater(notes, i, [&](const Note& other) { return other.pitch == note.pitch; });
				if (again != nullptr)
					newEnd = std::min(newEnd, again->position);
				note.length = std::max(note.length, newEnd - note.position);
			}
		eraseControllers(part, [&](const Controller& controller)
			{ return isSustain(controller) && processed.count(controller.channel) != 0; });
	}

	void deleteOverlaps(MidiPart& part, const NoteSelection& selection, OverlapMode mode)
	{
		for (auto& [channel, notes] : notesByChannel(part))
			for (size_t i = 0; i < notes.size(); ++i)
			{
				Note& note = *notes[i];
				if (!selection.contains(note.id))
					continue;
				const Note* next = nextLater(notes, i,
					[&](const Note& other) { return mode == OverlapMode::Poly || other.pitch == note.pitch; });
				if (next != nullptr && noteEnd(note) > next->position)
					note.length = next->position - note.position;
			}
	}

	void deleteDoubles(MidiPart& part, const NoteSelection& selection)
	{
		// The note kept at each pitch, channel and position.
		std::map<std::tuple<uint8_t, uint8_t, Ticks>, const Note*> kept;
		std::vector<ObjectId> doubles;
		for (const auto& [id, note] : part.notes)
		{
			if (!selection.contains(id))
				continue;
			const Note*& keeper = kept[{note.channel, note.pitch, note.position}];
			if (keeper == nullptr)
			{
				keeper = &note;
			}
			else if (note.length > keeper->length)
			{
				doubles.push_back(keeper->id);
				keeper = &note;
			}
			else
			{
				doubles.push_back(id);
			}
		}
		eraseNotes(part, doubles);
	}

	void deleteControllers(MidiPart& part)
	{
		eraseControllers(part, [](const Controller& controller) { return isCurve(controller.type); });
	}

	bool isContinuousController(int number)
	{
		const bool bankSelect = number == 0 || number == 32;
		const bool dataEntry = number == 6 || number == 38;
		const bool switches = number >= 64 && number <= 69;
		const bool parameters = number >= 96 && number <= 101; // data increment and decrement, NRPN and RPN
		const bool channelMode = number >= 120;
		return number >= 0 && number <= 127 && !bankSelect && !dataEntry && !switches && !parameters && !channelMode;
	}

	void deleteContinuousControllers(MidiPart& part)
	{
		eraseControllers(part, [](const Controller& controller)
			{ return controller.type == ControllerType::Control && isContinuousController(controller.number); });
	}

	void deleteNotes(MidiPart& part, const NoteSelection& selection, const DeleteNotesSettings& settings)
	{
		const bool both = settings.both && settings.minimumLength && settings.minimumVelocity;
		std::vector<ObjectId> deleted;
		for (const auto& [id, note] : part.notes)
		{
			if (!selection.contains(id))
				continue;
			const bool tooShort = settings.minimumLength && note.length < *settings.minimumLength;
			const bool tooSoft = settings.minimumVelocity && note.velocity < *settings.minimumVelocity;
			if (both ? tooShort && tooSoft : tooShort || tooSoft)
				deleted.push_back(id);
		}
		eraseNotes(part, deleted);
	}

	void restrictPolyphony(MidiPart& part, const NoteSelection& selection, int voices)
	{
		std::vector<ObjectId> silenced;
		for (auto& [channel, notes] : notesByChannel(part))
		{
			std::vector<Note*> sounding; // in the order they started, the lowest first among notes that start together
			for (Note* note : notes)
			{
				if (!selection.contains(note->id))
					continue;
				sounding.erase(std::remove_if(sounding.begin(), sounding.end(),
								   [&](const Note* other) { return noteEnd(*other) <= note->position; }),
					sounding.end());
				while (!sounding.empty() && static_cast<int>(sounding.size()) >= voices)
				{
					Note* oldest = sounding.front();
					sounding.erase(sounding.begin());
					oldest->length = note->position - oldest->position;
					if (oldest->length <= 0)
						silenced.push_back(oldest->id);
				}
				sounding.push_back(note);
			}
		}
		eraseNotes(part, silenced);
	}

	void thinOut(MidiPart& part, time::Ticks minimumDistance)
	{
		std::map<LaneKey, std::vector<const Controller*>> lanes;
		for (const auto& [id, controller] : part.controllers)
			if (isCurve(controller.type))
				lanes[laneOf(controller)].push_back(&controller);
		std::vector<ObjectId> removed;
		for (auto& [key, events] : lanes)
		{
			std::sort(events.begin(), events.end(), [](const Controller* a, const Controller* b)
				{ return std::tie(a->position, a->id) < std::tie(b->position, b->id); });
			// For each event, the next one with another value, or none.
			std::vector<const Controller*> nextChange(events.size(), nullptr);
			for (size_t i = events.size() - 1; i > 0; --i)
				nextChange[i - 1] = events[i]->value != events[i - 1]->value ? events[i] : nextChange[i];
			const auto direction = [](uint32_t from, uint32_t to) { return (to > from) - (to < from); };
			const Controller* kept = events.front();
			for (size_t i = 1; i < events.size(); ++i)
			{
				const Controller& event = *events[i];
				const bool last = i + 1 == events.size();
				const Controller* next = nextChange[i];
				const bool turns =
					next != nullptr && direction(kept->value, event.value) * direction(event.value, next->value) < 0;
				const bool repeated = event.value == kept->value;
				const bool close = event.position - kept->position < minimumDistance;
				if (repeated || (close && !turns && !last))
					removed.push_back(event.id);
				else
					kept = &event;
			}
		}
		for (const ObjectId id : removed)
			part.controllers.erase(id);
	}

	void reverse(MidiPart& part, const NoteSelection& selection)
	{
		std::vector<Ticks> positions;
		for (const auto& [id, note] : part.notes)
			if (selection.contains(id))
				positions.push_back(note.position);
		if (selection.all())
			for (const auto& [id, controller] : part.controllers)
				positions.push_back(controller.position);
		if (positions.empty())
			return;
		const auto [first, last] = std::minmax_element(positions.begin(), positions.end());
		const Ticks mirror = *first + *last;
		for (auto& [id, note] : part.notes)
			if (selection.contains(id))
				note.position = mirror - note.position;
		if (selection.all())
			for (auto& [id, controller] : part.controllers)
				controller.position = mirror - controller.position;
	}

	void invert(MidiPart& part, const NoteSelection& selection)
	{
		std::vector<Ticks> positions;
		for (const auto& [id, note] : part.notes)
			if (selection.contains(id))
			{
				positions.push_back(note.position);
				positions.push_back(noteEnd(note));
			}
		if (selection.all())
			for (const auto& [id, controller] : part.controllers)
				positions.push_back(controller.position);
		if (positions.empty())
			return;
		const auto [first, last] = std::minmax_element(positions.begin(), positions.end());
		const Ticks mirror = *first + *last;
		for (auto& [id, note] : part.notes)
			if (selection.contains(id))
				note.position = mirror - noteEnd(note);
		if (selection.all())
			for (auto& [id, controller] : part.controllers)
				controller.position = mirror - controller.position;
	}

	PartChanges merge(Project& project, const MergeSettings& settings)
	{
		const Ticks start = project.tempo.toTicks(settings.start, project.sampleRate);
		const Ticks end = project.tempo.toTicks(settings.end, project.sampleRate);
		std::vector<ObjectId> tracks = settings.tracks;
		if (tracks.empty())
			for (const ObjectId id : project.trackOrder())
				if (isNoteTrack(project.trackById.at(id)))
					tracks.push_back(id);

		// What plays in the range, in project ticks, and the last value of every controller before it.
		std::vector<Note> notes;
		std::vector<Controller> controllers;
		std::map<LaneKey, Controller> chased;
		std::map<LaneKey, Ticks> chasedAt;
		for (const auto& [id, part] : project.midiParts)
		{
			if (std::find(tracks.begin(), tracks.end(), part.track) == tracks.end())
				continue;
			const Ticks partFrom = partStart(project, part);
			const Ticks partTo = partEnd(project, part);
			for (const auto& [noteId, note] : part.notes)
			{
				const Ticks at = partFrom + note.position;
				if (note.position < part.length && at >= start && at < end)
				{
					Note copy = note;
					copy.position = at;
					copy.length = std::min(at + note.length, partTo) - at;
					notes.push_back(copy);
				}
			}
			for (const auto& [controllerId, controller] : part.controllers)
			{
				const Ticks at = partFrom + controller.position;
				if (controller.position >= part.length)
					continue;
				Controller copy = controller;
				copy.position = at;
				if (at >= start && at < end)
				{
					controllers.push_back(copy);
				}
				else if (at < start && settings.includeChase && controller.type != ControllerType::SysEx)
				{
					const LaneKey key = controller.type == ControllerType::Program
						? LaneKey{ControllerType::Program, controller.channel, 0}
						: laneOf(controller);
					const auto previous = chasedAt.find(key);
					if (previous == chasedAt.end() || previous->second <= at)
					{
						chasedAt[key] = at;
						copy.position = start;
						chased[key] = copy;
					}
				}
			}
		}

		PartChanges changes;
		ObjectId destination = settings.destination;
		if (destination == 0)
		{
			const ObjectId instrument = tracks.empty() ? 0 : instrumentOf(project, tracks.front());
			destination = addMidiTrack(
				project, defaultMidiTrackName(project), 0, project.tracks.size(), instrument, time::TimeBase::Musical);
			changes.tracks.push_back(destination);
		}
		else if (settings.eraseDestination)
		{
			for (auto it = project.midiParts.begin(); it != project.midiParts.end();)
			{
				MidiPart& part = it->second;
				const Ticks partFrom = partStart(project, part);
				if (part.track != destination)
				{
					++it;
					continue;
				}
				if (partFrom >= start && partEnd(project, part) <= end)
				{
					changes.removedParts.push_back(part.id);
					it = project.midiParts.erase(it);
					continue;
				}
				const auto inRange = [&](Ticks position)
				{ return partFrom + position >= start && partFrom + position < end; };
				const size_t before = part.notes.size() + part.controllers.size();
				for (auto note = part.notes.begin(); note != part.notes.end();)
					note = inRange(note->second.position) ? part.notes.erase(note) : std::next(note);
				eraseControllers(part, [&](const Controller& controller) { return inRange(controller.position); });
				if (part.notes.size() + part.controllers.size() != before)
					changes.changedParts.push_back(part.id);
				++it;
			}
		}

		MidiPart part;
		part.id = project.newId();
		part.track = destination;
		part.name = settings.name.empty() ? project.trackById.at(destination).name : settings.name;
		place(project, part, start, end);
		for (auto& [key, controller] : chased)
			controllers.insert(controllers.begin(), controller);
		for (Note& note : notes)
		{
			note.id = project.newId();
			note.position -= start;
			part.notes[note.id] = note;
		}
		for (Controller& controller : controllers)
		{
			controller.id = project.newId();
			controller.position -= start;
			part.controllers[controller.id] = controller;
		}
		changes.addedParts.push_back(part.id);
		project.midiParts[part.id] = std::move(part);
		return changes;
	}

	PartChanges dissolve(Project& project, const std::vector<ObjectId>& parts, DissolveBy by, bool optimize)
	{
		PartChanges changes;
		// The key of each event: its channel or pitch, or -1 for events that stay.
		const auto noteKey = [by](const Note& note) { return by == DissolveBy::Channel ? note.channel : note.pitch; };
		const auto controllerKey = [by](const Controller& controller) -> int
		{
			if (by == DissolveBy::Channel)
				return controller.type == ControllerType::SysEx ? -1 : controller.channel;
			return controller.type == ControllerType::PolyPressure ? controller.number : -1;
		};

		// The parts of each track, in the order of the track list.
		std::map<ObjectId, std::vector<ObjectId>> byTrack;
		for (const ObjectId id : parts)
			byTrack[project.midiParts.at(id).track].push_back(id);
		for (const ObjectId trackId : project.trackOrder())
		{
			const auto found = byTrack.find(trackId);
			if (found == byTrack.end())
				continue;
			std::set<int> keys;
			for (const ObjectId id : found->second)
			{
				const MidiPart& part = project.midiParts.at(id);
				for (const auto& [noteId, note] : part.notes)
					keys.insert(noteKey(note));
				for (const auto& [controllerId, controller] : part.controllers)
					if (controllerKey(controller) >= 0)
						keys.insert(controllerKey(controller));
			}

			// A track per key, after the source track.
			const Track source = project.trackById.at(trackId);
			const ObjectId instrument = instrumentOf(project, trackId);
			std::vector<ObjectId>& siblings =
				source.parent == 0 ? project.tracks : project.trackById.at(source.parent).children;
			size_t index = static_cast<size_t>(std::find(siblings.begin(), siblings.end(), trackId) - siblings.begin());
			std::map<int, ObjectId> trackFor;
			for (const int key : keys)
			{
				const std::string suffix = by == DissolveBy::Channel ? "Ch " + std::to_string(key + 1) : pitchName(key);
				trackFor[key] = addMidiTrack(
					project, source.name + " " + suffix, source.parent, ++index, instrument, source.timeBase);
				changes.tracks.push_back(trackFor[key]);
			}

			for (const ObjectId id : found->second)
			{
				MidiPart& original = project.midiParts.at(id);
				std::map<int, MidiPart> pieces;
				for (auto note = original.notes.begin(); note != original.notes.end();)
				{
					pieces[noteKey(note->second)].notes.insert(*note);
					note = original.notes.erase(note);
				}
				for (auto controller = original.controllers.begin(); controller != original.controllers.end();)
				{
					const int key = controllerKey(controller->second);
					if (key < 0)
					{
						++controller;
						continue;
					}
					pieces[key].controllers.insert(*controller);
					controller = original.controllers.erase(controller);
				}
				if (pieces.empty())
					continue;
				const Ticks originalStart = partStart(project, original);
				const Ticks originalEnd = partEnd(project, original);
				for (auto& [key, piece] : pieces)
				{
					piece.id = project.newId();
					piece.track = trackFor.at(key);
					piece.name = project.trackById.at(piece.track).name;
					Ticks from = originalStart;
					Ticks to = originalEnd;
					if (optimize)
					{
						// The bars from the first event's to the last one's end, within the part.
						Ticks first = originalEnd;
						Ticks last = originalStart;
						for (const auto& [noteId, note] : piece.notes)
						{
							first = std::min(first, originalStart + note.position);
							last = std::max(last, std::min(originalStart + noteEnd(note), originalEnd));
						}
						for (const auto& [controllerId, controller] : piece.controllers)
						{
							first = std::min(first, originalStart + controller.position);
							last = std::max(last, originalStart + controller.position + 1);
						}
						const time::SignatureMap& bars = project.signatures;
						from = std::max(originalStart, bars.barStart(bars.toBarsBeats(first).bar));
						const int64_t lastBar = bars.toBarsBeats(std::max(first, last - 1)).bar;
						to = std::min(originalEnd, bars.barStart(lastBar + 1));
					}
					place(project, piece, from, to);
					const Ticks shift = partStart(project, piece) - originalStart;
					for (auto& [noteId, note] : piece.notes)
						note.position -= shift;
					for (auto& [controllerId, controller] : piece.controllers)
						controller.position -= shift;
					changes.addedParts.push_back(piece.id);
					project.midiParts[piece.id] = std::move(piece);
				}
				if (original.notes.empty() && original.controllers.empty())
				{
					changes.removedParts.push_back(id);
					project.midiParts.erase(id);
				}
				else
				{
					changes.changedParts.push_back(id);
				}
			}
		}
		return changes;
	}

	PartChanges bounce(Project& project, const std::vector<ObjectId>& parts)
	{
		PartChanges changes;
		const MidiPart* earliest = nullptr;
		Ticks start = 0;
		Ticks end = 0;
		for (const ObjectId id : parts)
		{
			const MidiPart& part = project.midiParts.at(id);
			const Ticks from = partStart(project, part);
			if (earliest == nullptr || from < start)
			{
				earliest = &part;
				start = from;
			}
			end = std::max(end, partEnd(project, part));
		}
		if (earliest == nullptr)
			return changes;

		MidiPart combined;
		combined.id = project.newId();
		combined.track = earliest->track;
		combined.name = earliest->name;
		place(project, combined, start, end);
		const Ticks combinedStart = partStart(project, combined);
		for (const ObjectId id : parts)
		{
			const MidiPart& part = project.midiParts.at(id);
			const Ticks shift = partStart(project, part) - combinedStart;
			const Ticks partTo = partEnd(project, part);
			for (const auto& [noteId, note] : part.notes)
				if (note.position < part.length)
				{
					Note copy = note;
					copy.position += shift;
					copy.length =
						std::min(combinedStart + copy.position + note.length, partTo) - (combinedStart + copy.position);
					combined.notes[copy.id] = copy;
				}
			for (const auto& [controllerId, controller] : part.controllers)
				if (controller.position < part.length)
				{
					Controller copy = controller;
					copy.position += shift;
					combined.controllers[copy.id] = copy;
				}
			addUnique(changes.removedParts, id);
		}
		for (const ObjectId id : changes.removedParts)
			project.midiParts.erase(id);
		changes.addedParts.push_back(combined.id);
		project.midiParts[combined.id] = std::move(combined);
		return changes;
	}
}
