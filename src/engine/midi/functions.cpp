// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/midi/functions.h"

#include "engine/document/chordtrack.h"
#include "engine/document/rendersnapshot.h"
#include "engine/midi/articulationmaps.h"
#include "engine/midi/chords.h"
#include "engine/midi/drummaps.h"
#include "engine/midi/midivalues.h"
#include "engine/midi/patterns.h"
#include "engine/midi/trackchannel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <tuple>
#include <utility>

namespace gnubase::engine::midi
{
	using time::Ticks;

	namespace
	{
		constexpr int g_sustainPedal = 64;
		constexpr int g_lastSwitch = 69;
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
			std::array<int, 8> steps; // semitones above the root
		};

		constexpr ScaleEntry g_scales[] = {
			{ScaleType::Major, "major", 7, {0, 2, 4, 5, 7, 9, 11, 0}},
			{ScaleType::Minor, "minor", 7, {0, 2, 3, 5, 7, 8, 10, 0}},
			{ScaleType::HarmonicMinor, "harmonicMinor", 7, {0, 2, 3, 5, 7, 8, 11, 0}},
			{ScaleType::MelodicMinor, "melodicMinor", 7, {0, 2, 3, 5, 7, 9, 11, 0}},
			{ScaleType::Dorian, "dorian", 7, {0, 2, 3, 5, 7, 9, 10, 0}},
			{ScaleType::Phrygian, "phrygian", 7, {0, 1, 3, 5, 7, 8, 10, 0}},
			{ScaleType::Lydian, "lydian", 7, {0, 2, 4, 6, 7, 9, 11, 0}},
			{ScaleType::Mixolydian, "mixolydian", 7, {0, 2, 4, 5, 7, 9, 10, 0}},
			{ScaleType::Locrian, "locrian", 7, {0, 1, 3, 5, 6, 8, 10, 0}},
			{ScaleType::MajorPentatonic, "majorPentatonic", 5, {0, 2, 4, 7, 9, 0, 0, 0}},
			{ScaleType::MinorPentatonic, "minorPentatonic", 5, {0, 3, 5, 7, 10, 0, 0, 0}},
			{ScaleType::Blues, "blues", 6, {0, 3, 5, 6, 7, 10, 0, 0}},
			{ScaleType::WholeTone, "wholeTone", 6, {0, 2, 4, 6, 8, 10, 0, 0}},
			{ScaleType::LydianDominant, "lydianDominant", 7, {0, 2, 4, 6, 7, 9, 10, 0}},
			{ScaleType::MixolydianFlat6, "mixolydianFlat6", 7, {0, 2, 4, 5, 7, 8, 10, 0}},
			{ScaleType::PhrygianDominant, "phrygianDominant", 7, {0, 1, 4, 5, 7, 8, 10, 0}},
			{ScaleType::Altered, "altered", 7, {0, 1, 3, 4, 6, 8, 10, 0}},
			{ScaleType::HalfWholeDiminished, "halfWholeDiminished", 8, {0, 1, 3, 4, 6, 7, 9, 10}},
			{ScaleType::WholeHalfDiminished, "wholeHalfDiminished", 8, {0, 2, 3, 5, 6, 8, 9, 11}},
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

		// A number from 0 to below 1 for a note of a part, the same every time (SplitMix64 of their ids).
		double unitRandom(ObjectId part, ObjectId note)
		{
			uint64_t value = (part * 0x9E3779B97F4A7C15ULL) ^ note;
			value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
			value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
			value ^= value >> 31U;
			return static_cast<double>(value >> 11U) / static_cast<double>(1ULL << 53U);
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

		// Continuous controllers, pitch bend and pressure.
		bool isContinuous(const Controller& controller)
		{
			return controller.type == ControllerType::Control ? isContinuousController(controller.number)
															  : isCurve(controller.type);
		}

		// Controllers whose value holds until the next event of their lane: the continuous ones and the switches 64
		// (sustain) to 69.
		bool holdsValue(const Controller& controller)
		{
			const bool isSwitch = controller.type == ControllerType::Control && controller.number >= g_sustainPedal &&
				controller.number <= g_lastSwitch;
			return isSwitch || isContinuous(controller);
		}

		bool comesBefore(const Controller* a, const Controller* b)
		{
			return std::tie(a->position, a->id) < std::tie(b->position, b->id);
		}

		// Plays the values of every lane that holds a value backwards between from and to: the value that held from
		// an event to the next one (or to the range's end) holds from the next one's mirror image to its own. Values
		// overwritten at the same tick go, and at the range's end the lane returns to the value it had there: on the
		// part's last tick if the range reaches the part's end, since playback drops events from there on.
		void mirrorControllers(MidiPart& part, Ticks from, Ticks to, Ticks partLength, const NewId& newId)
		{
			// Not before the range, for a part that ends before it (where nothing in the range plays anyway).
			const Ticks restoreAt = std::max(from, std::min(to, partLength - 1));
			std::map<LaneKey, std::vector<Controller*>> lanes;
			for (auto& [id, controller] : part.controllers)
				if (holdsValue(controller))
					lanes[laneOf(controller)].push_back(&controller);
			const Ticks mirror = from + to;
			std::vector<ObjectId> overwritten;
			std::vector<Controller> restored;
			for (auto& [key, events] : lanes)
			{
				std::sort(events.begin(), events.end(), comesBefore);
				Controller restore = *events.back(); // the value after the range
				const Controller* latest = nullptr;  // the event that plays last in the mirror image, by restoreAt
				for (size_t i = 0; i < events.size(); ++i)
				{
					Controller& event = *events[i];
					const Ticks next = i + 1 < events.size() ? events[i + 1]->position : to;
					if (next == event.position)
					{
						overwritten.push_back(event.id);
						continue;
					}
					event.position = mirror - next;
					// Mirrored positions fall as i rises. Past restoreAt an event lies past the part's end, where
					// playback drops it, so the lane holds the value of the first one at or before restoreAt.
					if (latest == nullptr && event.position <= restoreAt)
						latest = &event;
				}
				if (latest == nullptr || latest->value != restore.value)
				{
					restore.id = newId();
					restore.position = restoreAt;
					restored.push_back(restore);
				}
			}
			for (const ObjectId id : overwritten)
				part.controllers.erase(id);
			for (const Controller& controller : restored)
				part.controllers[controller.id] = controller;
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
			const Channel channel{project.newId(), track.id, ChannelFormat::None, 0, instrument, {}, 0.0, 0.0};
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

	time::Ticks lengthInTicks(const Project& project, const MidiPart& part)
	{
		return partEnd(project, part) - partStart(project, part);
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

	std::vector<int> scaleSteps(ScaleType type)
	{
		const ScaleEntry& entry = scaleEntry(type);
		return {entry.steps.begin(), entry.steps.begin() + entry.size};
	}

	int nearestInScale(int pitch, const Scale& scale) noexcept
	{
		return closestInScale(pitch, scale);
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

	void changeVelocities(MidiPart& part, const NoteSelection& selection, const VelocitySettings& settings)
	{
		constexpr auto maxVelocity = static_cast<double>(std::numeric_limits<uint16_t>::max());
		for (auto& [id, note] : part.notes)
		{
			if (!selection.contains(id))
				continue;
			const double velocity = static_cast<double>(note.velocity) / maxVelocity;
			const auto change = [&settings, &part, velocity, noteId = id]
			{
				switch (settings.type)
				{
				case VelocityChange::Add:
					return velocity + settings.amount;
				case VelocityChange::Compress:
					return velocity * settings.ratio;
				case VelocityChange::Limit:
					return std::clamp(velocity, settings.lower, settings.upper);
				case VelocityChange::Fixed:
					return settings.amount;
				case VelocityChange::Variance:
					return velocity * (1.0 - (settings.variance * unitRandom(part.id, noteId)));
				}
				return velocity;
			};
			const double changed = change();
			note.velocity = static_cast<uint16_t>(std::lround(std::clamp(changed, 0.0, 1.0) * maxVelocity));
		}
	}

	void pedalToNoteLength(MidiPart& part, const NoteSelection& selection, time::Ticks partLength)
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
				const auto after = std::partition_point(events.begin(), events.end(),
					[&](const Controller* pedal) { return pedal->position < noteEnd(note); });
				if (after == events.begin() || !down(*std::prev(after)))
					continue;
				const auto release = std::find_if_not(after, events.end(), down);
				Ticks newEnd = release == events.end() ? std::max(partLength, noteEnd(note)) : (*release)->position;
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
			if (isContinuous(controller))
				lanes[laneOf(controller)].push_back(&controller);
		std::vector<ObjectId> removed;
		for (auto& [key, events] : lanes)
		{
			std::sort(events.begin(), events.end(), comesBefore);
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
				// A value that holds a while is kept where it starts, not only where it repeats.
				const bool holds = !last && events[i + 1]->position - event.position >= minimumDistance;
				if (repeated || (close && !turns && !last && !holds))
					removed.push_back(event.id);
				else
					kept = &event;
			}
		}
		for (const ObjectId id : removed)
			part.controllers.erase(id);
	}

	void reverse(MidiPart& part, const NoteSelection& selection, Ticks partLength, const NewId& newId)
	{
		std::vector<Ticks> positions;
		for (const auto& [id, note] : part.notes)
			if (selection.contains(id))
				positions.push_back(note.position);
		if (selection.all())
			for (const auto& [id, controller] : part.controllers)
				if (holdsValue(controller))
					positions.push_back(controller.position);
		if (positions.empty())
			return;
		const auto [first, last] = std::minmax_element(positions.begin(), positions.end());
		if (*first == *last)
			return;
		const Ticks mirror = *first + *last;
		for (auto& [id, note] : part.notes)
			if (selection.contains(id))
				note.position = mirror - note.position;
		if (selection.all())
			mirrorControllers(part, *first, *last, partLength, newId);
	}

	void invert(MidiPart& part, const NoteSelection& selection, Ticks partLength, const NewId& newId)
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
				if (holdsValue(controller))
					positions.push_back(controller.position);
		if (positions.empty())
			return;
		const auto [first, last] = std::minmax_element(positions.begin(), positions.end());
		if (*first == *last)
			return;
		const Ticks mirror = *first + *last;
		for (auto& [id, note] : part.notes)
			if (selection.contains(id))
				note.position = mirror - noteEnd(note);
		if (selection.all())
			mirrorControllers(part, *first, *last, partLength, newId);
	}

	namespace
	{
		// What a track does to a note's pitch before its part's transpose, as compilePart in rendersnapshot.cpp plays
		// it: a drum map plays the note's sound, its O-note on its channel and not at all if it is muted; on a track
		// without one, the chord track, if the track follows it, maps the pitch.
		class TrackPitches
		{
		public:
			TrackPitches(const Project& project, ObjectId track)
				: m_map(drumMapOf(project, track)), m_follower(project, track),
				  m_trackChannel(trackMidiChannel(project, track))
			{
			}

			// The track's MIDI channel, which plays its events instead of their own, or none for Any.
			std::optional<int> trackChannel() const { return m_trackChannel; }

			bool silent(int pitch) const { return m_map != nullptr && sound(pitch).mute; }

			int played(int pitch, Ticks at) const
			{
				if (m_map != nullptr)
					return sound(pitch).oNote;
				return m_follower.active() ? m_follower.pitch(pitch, at) : pitch;
			}

			uint8_t channel(int pitch, uint8_t noteChannel) const
			{
				if (m_map == nullptr || sound(pitch).channel < 0)
					return noteChannel;
				return static_cast<uint8_t>(sound(pitch).channel);
			}

			// Whether a note plays the same with the semitones added to its pitch as with them added to what the track
			// makes of it; a pitch beyond 0-127 that would play doesn't. plays(target) tells whether what the track
			// makes of it, transposed, plays after what is added later. A note that doesn't play either way counts
			// where the moved pitch is beyond 0-127, as bounce leaves it out, and where the track makes of the moved
			// pitch what it makes of the note, so that it comes back alike when the track's MIDI modifiers change
			// (#1240).
			template <typename Plays>
			bool transposesInNote(const Note& note, int semitones, Ticks at, const Plays& plays) const
			{
				if (silent(note.pitch))
					return true;
				const int target = played(note.pitch, at) + semitones;
				const int moved = note.pitch + semitones;
				if (moved < 0 || moved > 127)
					return !plays(target);
				return !silent(moved) && played(moved, at) == target &&
					channel(moved, note.channel) == channel(note.pitch, note.channel);
			}

			// The pitch to store for the track to play target on a channel at a position: own if that plays it, so that
			// a track merged onto itself keeps its pitches, else the nearest that does. If none does, target, which the
			// track then maps like any note of its own.
			int stored(int target, uint8_t onChannel, Ticks at, int own) const
			{
				if (m_map == nullptr && !m_follower.active())
					return target;
				const auto plays = [&](int pitch)
				{
					return pitch >= 0 && pitch <= 127 && !silent(pitch) && played(pitch, at) == target &&
						channel(pitch, onChannel) == onChannel;
				};
				if (plays(own))
					return own;
				for (int distance = 0; distance <= 127; ++distance)
					for (const int pitch : {target - distance, target + distance})
						if (plays(pitch))
							return pitch;
				return target;
			}

			// The channel to store for the track to play an event on onChannel, a note of the pitch stored or, with -1,
			// a controller: own where the track plays it the same, so that a track merged onto itself keeps its events'
			// channels, else onChannel. A track set to a channel plays every channel on it.
			uint8_t storedChannel(uint8_t onChannel, uint8_t own, int pitch = -1) const
			{
				const auto playedOn = [&](uint8_t stored)
				{
					const auto on = static_cast<uint8_t>(m_trackChannel.value_or(stored));
					return pitch < 0 ? on : channel(pitch, on);
				};
				return playedOn(own) == playedOn(onChannel) ? own : onChannel;
			}

		private:
			const DrumSound& sound(int pitch) const { return m_map->sounds.at(static_cast<size_t>(pitch)); }

			const DrumMap* m_map;
			ChordFollower m_follower;
			std::optional<int> m_trackChannel;
		};

		// A track's MIDI modifiers as playback applies them to its parts' notes: none while they are bypassed.
		Modifiers trackModifiers(const Project& project, ObjectId track)
		{
			const Modifiers& modifiers = project.channels.at(project.trackById.at(track).channel).modifiers;
			return modifiers.bypass ? Modifiers() : modifiers;
		}

		// The smallest and the largest sum the random modifiers for a target add.
		std::pair<int64_t, int64_t> randomSpan(const Modifiers& modifiers, RandomTarget target)
		{
			std::pair<int64_t, int64_t> span{0, 0};
			for (const RandomModifier& random : modifiers.random)
				if (random.target == target && random.minimum <= random.maximum)
				{
					span.first += random.minimum;
					span.second += random.maximum;
				}
			return span;
		}

		// Whether a pitch, with the part's transpose and the global transpose, plays through the modifiers for some
		// random pitch they draw: modifyNote leaves out a key beyond 0-127 before any range modifier.
		bool pitchPlays(const Modifiers& modifiers, int pitch)
		{
			const auto [low, high] = randomSpan(modifiers, RandomTarget::Pitch);
			const int64_t key = int64_t{pitch} + modifiers.transpose;
			return key + high >= 0 && key + low <= 127;
		}

		// Whether a note with a MIDI 1.0 velocity plays as loud through the modifiers with the part's velocity added
		// to it, as bounce stores it, as with the part's velocity added after the modifiers' compression, as playback
		// adds it, for every random velocity they draw.
		bool velocityInNote(const Modifiers& modifiers, int velocity, int partVelocity)
		{
			if (partVelocity == 0)
				return true;
			// Pitches play no part: modifyNote then sees a key that passes.
			Modifiers neutral = modifiers;
			neutral.transpose = 0;
			for (RandomModifier& random : neutral.random)
				if (random.target != RandomTarget::Velocity)
					random.target = RandomTarget::Off;
			for (RangeModifier& range : neutral.range)
				if (range.target == RangeTarget::NoteLimit || range.target == RangeTarget::NoteFilter)
					range.target = RangeTarget::Off;
			// Each random value as a shift: where it adds nothing, modifyNote leaves the velocity, which is within
			// 1-127 already, as clamping it would.
			const auto [low, high] = randomSpan(neutral, RandomTarget::Velocity);
			for (RandomModifier& random : neutral.random)
				random.target = RandomTarget::Off;
			const uint16_t stored =
				velocityFromMidi1(static_cast<uint8_t>(std::clamp(velocity + partVelocity, 1, 127)));
			// Beyond ±254 every sum clamps alike.
			for (int64_t offset = std::max<int64_t>(low, -254); offset <= std::min<int64_t>(high, 254); ++offset)
			{
				Modifiers before = neutral;
				before.velocityShift += partVelocity + static_cast<int>(offset);
				Modifiers after = neutral;
				after.velocityShift += static_cast<int>(offset);
				uint8_t pitch = 60;
				uint16_t playedBefore = velocityFromMidi1(static_cast<uint8_t>(velocity));
				const bool playsBefore = modifyNote(before, 0, pitch, playedBefore);
				uint16_t playedAfter = stored;
				const bool playsAfter = modifyNote(after, 0, pitch, playedAfter);
				if (playsBefore != playsAfter ||
					(playsBefore && velocityToMidi1(playedBefore) != velocityToMidi1(playedAfter)))
					return false;
			}
			return true;
		}

		// Whether a note plays alike through the modifiers, which have the part's velocity in them as playback adds
		// it, as through them without it and with the part's velocity added after, as a new part with the part's
		// velocity plays what merge writes with Include Inserts (#1240): whether it plays and how loud, for every
		// velocity its variance gives it and the random velocity playback draws for it.
		bool velocityOnPart(const Modifiers& modifiers, uint64_t seed, const Note& note, int partVelocity)
		{
			// Pitches play no part: modifyNote then sees a key that passes.
			Modifiers with = modifiers;
			with.transpose = 0;
			for (RandomModifier& random : with.random)
				if (random.target == RandomTarget::Pitch)
					random.target = RandomTarget::Off;
			for (RangeModifier& range : with.range)
				if (range.target == RangeTarget::NoteLimit || range.target == RangeTarget::NoteFilter)
					range.target = RangeTarget::Off;
			Modifiers without = with;
			without.velocityShift -= partVelocity;
			const double varied = std::clamp(std::round(note.velocity * (1.0 + note.velocityVariance)), 0.0,
				static_cast<double>(std::numeric_limits<uint16_t>::max()));
			const int first = velocityToMidi1(note.velocity);
			const int last = velocityToMidi1(static_cast<uint16_t>(varied));
			for (int velocity = std::min(first, last); velocity <= std::max(first, last); ++velocity)
			{
				uint8_t pitch = 60;
				uint16_t played = velocityFromMidi1(static_cast<uint8_t>(velocity));
				const bool plays = modifyNote(with, seed, pitch, played);
				uint16_t kept = velocityFromMidi1(static_cast<uint8_t>(velocity));
				const bool keptPlays = modifyNote(without, seed, pitch, kept);
				if (plays != keptPlays ||
					(plays && velocityToMidi1(played) != std::clamp(velocityToMidi1(kept) + partVelocity, 1, 127)))
					return false;
			}
			return true;
		}

		// The root key and Global Transpose setting of a part (#888).
		struct GlobalSetting
		{
			int rootKey = -1;
			bool independent = false;
		};

		bool operator==(const GlobalSetting& a, const GlobalSetting& b)
		{
			return a.rootKey == b.rootKey && a.independent == b.independent;
		}

		// The global transpose a part with the setting plays at a position, as compilePart adds it.
		int globalAt(const Project& project, const GlobalSetting& setting, Ticks at)
		{
			MidiPart probe;
			probe.rootKey = setting.rootKey;
			probe.independent = setting.independent;
			return globalTranspose(project, probe, project.tempo.toSamples(at, project.sampleRate));
		}

		// The semitones a part transposes a note at a position by, against a part with another global setting: its
		// transpose and the difference in global transpose.
		int semitonesAgainst(const Project& project, const MidiPart& part, const GlobalSetting& other, Ticks at)
		{
			return part.transpose + globalAt(project, {part.rootKey, part.independent}, at) -
				globalAt(project, other, at);
		}

		// The root key and Global Transpose setting (#954) of a part made from source parts: the one they all share,
		// so that playback transposes the notes as it did, or else the project root key and follow, which each note's
		// difference to it goes into, so that it keeps the pitch it played with. Without sources, no root key.
		GlobalSetting sharedSetting(const Project& project, const std::vector<const MidiPart*>& sources)
		{
			if (sources.empty())
				return {};
			const GlobalSetting front{sources.front()->rootKey, sources.front()->independent};
			const bool shared = std::all_of(sources.begin(), sources.end(),
				[&](const MidiPart* part) { return GlobalSetting{part->rootKey, part->independent} == front; });
			return shared ? front : GlobalSetting{project.rootKey, false};
		}

		// A part to bounce, and what of its transpose, Global Transpose and velocity goes into its notes.
		struct Bounced
		{
			ObjectId id = 0;
			// Its transpose, and the difference between its global transpose and that of the global setting the new
			// part plays its notes with.
			bool pitchInNotes = false;
			GlobalSetting against;
			bool velocityInNotes = false; // its velocity
		};

		// The settings of a part that bounce makes.
		struct BouncedSetting
		{
			int transpose = 0;
			int velocity = 0;
			GlobalSetting global;
		};

		bool operator<(const BouncedSetting& a, const BouncedSetting& b)
		{
			return std::tie(a.transpose, a.velocity, a.global.rootKey, a.global.independent) <
				std::tie(b.transpose, b.velocity, b.global.rootKey, b.global.independent);
		}

		// The parts, none muted, as one new part with the setting from the first one's start to the last one's end,
		// on the lowest of their lanes.
		MidiPart combine(Project& project, const std::vector<Bounced>& parts, const BouncedSetting& setting,
			const TrackPitches& pitches)
		{
			const MidiPart* earliest = nullptr;
			Ticks start = 0;
			Ticks end = 0;
			int lane = 0;
			for (const Bounced& bounced : parts)
			{
				const MidiPart& part = project.midiParts.at(bounced.id);
				const Ticks from = partStart(project, part);
				lane = earliest == nullptr ? part.lane : std::min(lane, part.lane);
				end = earliest == nullptr ? partEnd(project, part) : std::max(end, partEnd(project, part));
				if (earliest == nullptr || from < start)
				{
					earliest = &part;
					start = from;
				}
			}
			MidiPart combined;
			combined.id = project.newId();
			combined.track = earliest->track;
			combined.name = earliest->name;
			combined.lane = lane;
			combined.transpose = setting.transpose;
			combined.velocity = setting.velocity;
			combined.rootKey = setting.global.rootKey;
			combined.independent = setting.global.independent;
			place(project, combined, start, end);
			const Ticks combinedStart = partStart(project, combined);
			for (const Bounced& bounced : parts)
			{
				const MidiPart& part = project.midiParts.at(bounced.id);
				const Ticks from = partStart(project, part);
				const Ticks shift = from - combinedStart;
				const Ticks partTo = partEnd(project, part);
				const Ticks length = partTo - from;
				for (const auto& [noteId, note] : playedNotes(project, part))
				{
					if (note.position >= length)
						continue;
					const Ticks at = from + note.position;
					Note copy = note;
					if (bounced.pitchInNotes && !pitches.silent(note.pitch))
					{
						// transposesInNote lets a pitch beyond 0-127 through only for a note that plays nowhere,
						// which is left out.
						const int moved = note.pitch + semitonesAgainst(project, part, bounced.against, at);
						if (moved < 0 || moved > 127)
							continue;
						copy.pitch = static_cast<uint8_t>(moved);
					}
					copy.id = project.newId();
					copy.position += shift;
					copy.length =
						std::min(combinedStart + copy.position + note.length, partTo) - (combinedStart + copy.position);
					if (bounced.velocityInNotes && part.velocity != 0)
					{
						// As playback shifts it: in MIDI 1.0 velocities, a note-on staying at least 1.
						const int velocity = std::clamp(velocityToMidi1(note.velocity) + part.velocity, 1, 127);
						copy.velocity = velocityFromMidi1(static_cast<uint8_t>(velocity));
					}
					combined.notes[copy.id] = copy;
				}
				for (const auto& [controllerId, controller] : part.controllers)
					if (controller.position < length)
					{
						Controller copy = controller;
						copy.id = project.newId();
						copy.position += shift;
						combined.controllers[copy.id] = copy;
					}
			}
			return combined;
		}
	}

	std::vector<ObjectId> playingTracks(const Project& project)
	{
		const std::unique_ptr<const RenderSnapshot> snapshot = compile(project, 0);
		std::map<ObjectId, size_t> channelIndex;
		for (size_t i = 0; i < snapshot->channels.size(); ++i)
			channelIndex[snapshot->channels[i].id] = i;
		const auto heard = [&snapshot](size_t index)
		{
			// Outputs form no loops; the count only guards against one.
			for (size_t steps = 0; steps <= snapshot->channels.size(); ++steps)
			{
				const RenderChannel& channel = snapshot->channels[index];
				if (channel.silent)
					return false;
				if (channel.output.kind != RenderTarget::Kind::Channel)
					return true;
				index = static_cast<size_t>(channel.output.index);
			}
			return false;
		};
		std::vector<ObjectId> tracks;
		for (const ObjectId id : project.trackOrder())
		{
			const Track& track = project.trackById.at(id);
			const auto index = channelIndex.find(track.channel);
			if (isNoteTrack(track) && index != channelIndex.end() && heard(index->second))
				tracks.push_back(id);
		}
		return tracks;
	}

	PartChanges merge(Project& project, const MergeSettings& settings)
	{
		// Positions are compared with the locators in samples, as playback does: ticks are finer than samples, so a
		// locator in ticks can lie a little after an event that plays on it.
		const auto sampleOf = [&project](Ticks tick) { return project.tempo.toSamples(tick, project.sampleRate); };
		const auto inRange = [&](Ticks tick)
		{
			const time::Samples sample = sampleOf(tick);
			return sample >= settings.start && sample < settings.end;
		};
		Ticks start = project.tempo.toTicks(settings.start, project.sampleRate); // or the first event's, if earlier
		const Ticks end = project.tempo.toTicks(settings.end, project.sampleRate);
		const std::vector<ObjectId> tracks = settings.tracks.empty() ? playingTracks(project) : settings.tracks;

		// What plays in the range, in project ticks, and the last value of every controller before it. A note waits
		// as a candidate until the new part's root key and Global Transpose setting is known, which the parts its
		// notes come from decide (#954): the global transposition's difference goes through modifyNote like the rest.
		struct Candidate
		{
			Note note;           // as its track plays it, at its place in the project
			uint64_t seed;       // its part's and its own id, as playback draws them
			Modifiers modifiers; // its track's and part's, without the global transposition
			const MidiPart* source;
			Ticks sourceAt;     // where it is stored in its part, as playback sees the global transposition there
			uint8_t ownPitch;   // the pitch it is stored with, which a destination's map prefers
			uint8_t ownChannel; // the channel it is stored with, which a destination set to a channel keeps
		};
		std::vector<Candidate> candidates;
		std::vector<Controller> controllers;
		// The directions of the merged tracks by their maps' groups, and where parts start in the range a reset of each
		// group, as they play from the default slot.
		struct GroupEvent
		{
			Ticks position;
			bool direction; // else a part's reset
			size_t group;
			Controller controller;
		};
		std::vector<GroupEvent> groupEvents;
		std::map<LaneKey, Controller> chased;
		std::map<LaneKey, Ticks> chasedAt;
		const TrackPitches destinationPitches(project, settings.destination); // a new track maps nothing
		const auto merged = [&](const MidiPart& part)
		{ return !part.mute && std::find(tracks.begin(), tracks.end(), part.track) != tracks.end(); };
		for (const auto& [id, part] : project.midiParts)
		{
			if (!merged(part))
				continue;
			const Ticks partFrom = partStart(project, part);
			const Ticks partTo = partEnd(project, part);
			// As playback plays them (compilePart in rendersnapshot.cpp): through the track's drum map or the chord
			// track, then with the part's transpose and velocity, which the new part doesn't have, its global
			// transpose and with Include Inserts the track's MIDI modifiers; the new part's global transpose then
			// comes off the pitch that plays. Without Include Inserts, the difference of the two global transposes.
			const TrackPitches sourcePitches(project, part.track);
			const ArticulationMap* articulations = articulationMapOf(project, part.track);
			Modifiers modifiers = settings.includeInserts ? trackModifiers(project, part.track) : Modifiers();
			modifiers.transpose += part.transpose;
			modifiers.velocityShift += part.velocity;
			// Playback resolves directions per part (#175): a reset of each group where a part starts in the range
			// keeps the directions of the parts before it from its notes, if one is in effect there (below).
			if (articulations != nullptr && sampleOf(partFrom) > settings.start && sampleOf(partFrom) < settings.end)
				for (size_t group = 0; group < articulations->groups.size(); ++group)
				{
					const auto direction = std::find_if(articulations->articulations.begin(),
						articulations->articulations.end(),
						[group](const Articulation& each)
						{
							return each.type == ArticulationType::Direction && static_cast<size_t>(each.group) == group;
						});
					if (direction != articulations->articulations.end())
					{
						Controller reset;
						reset.position = partFrom;
						reset.type = ControllerType::Articulation;
						reset.value = 1;
						reset.articulation = direction->name;
						groupEvents.push_back({partFrom, false, group, reset});
					}
				}
			for (const auto& [noteId, note] : playedNotes(project, part))
			{
				if (partFrom + note.position >= partTo || sourcePitches.silent(note.pitch))
					continue;
				const uint64_t seed = noteSeed(part.id, noteId);
				Note copy = note;
				copy.pitch = static_cast<uint8_t>(sourcePitches.played(note.pitch, partFrom + note.position));
				// On the channel it plays on: its drum sound's, else its track's, else its own.
				copy.channel = sourcePitches.channel(
					note.pitch, static_cast<uint8_t>(sourcePitches.trackChannel().value_or(note.channel)));
				// A note moved before its part's start, or the project's, starts there, with its length.
				const Ticks moved = note.position + randomOffset(modifiers, RandomTarget::Position, seed);
				const Ticks at = partFrom + std::max({moved, Ticks{0}, -partFrom});
				if (at >= partTo || !inRange(at))
					continue;
				const Ticks length = note.length > 0 ? modifyLength(modifiers, seed, note.length) : 0;
				copy.position = at;
				copy.length = std::min(at + length, partTo) - at;
				candidates.push_back(
					{copy, seed, modifiers, &part, partFrom + note.position, note.pitch, note.channel});
			}
			for (const auto& [controllerId, controller] : part.controllers)
			{
				const Ticks at = partFrom + controller.position;
				if (at >= partTo)
					continue;
				Controller copy = controller;
				copy.position = at;
				if (controller.type != ControllerType::Articulation)
					copy.channel = static_cast<uint8_t>(sourcePitches.trackChannel().value_or(controller.channel));
				// As the destination stores it; chased by the channel it played on.
				Controller written = copy;
				written.channel = destinationPitches.storedChannel(copy.channel, controller.channel);
				// Of each group of the track's articulation map, the direction in effect where the range starts, so
				// that the notes play the same sound slot (#175), also without includeChase. Playback resolves
				// directions per part, so only from a part that still plays there.
				int group = -1;
				if (controller.type == ControllerType::Articulation && articulations != nullptr &&
					sampleOf(partTo) > settings.start)
					for (const Articulation& each : articulations->articulations)
						if (each.name == controller.articulation)
							group = each.group;
				if (inRange(at))
				{
					controllers.push_back(written);
					if (group >= 0)
						groupEvents.push_back({at, true, static_cast<size_t>(group), written});
				}
				else if (sampleOf(at) < settings.start &&
					(controller.type == ControllerType::Articulation
							? group >= 0
							: settings.includeChase && controller.type != ControllerType::SysEx))
				{
					LaneKey key = laneOf(copy);
					if (group >= 0)
						key = {ControllerType::Articulation, 0, static_cast<uint8_t>(group)};
					else if (controller.type == ControllerType::Program)
						key = {ControllerType::Program, copy.channel, 0};
					const auto previous = chasedAt.find(key);
					if (previous == chasedAt.end() || previous->second <= at)
					{
						chasedAt[key] = at;
						chased[key] = written;
					}
				}
			}
		}

		// A part's reset goes in only where a direction of its group is in effect in the merged part, which has the
		// directions of all merged tracks (#1265); before the directions at the same position.
		for (const auto& [key, controller] : chased)
			if (std::get<0>(key) == ControllerType::Articulation)
				groupEvents.push_back({controller.position, true, std::get<2>(key), controller});
		std::stable_sort(groupEvents.begin(), groupEvents.end(), [](const GroupEvent& a, const GroupEvent& b)
			{ return std::tie(a.position, a.direction) < std::tie(b.position, b.direction); });
		std::vector<Controller> resets;
		std::map<size_t, bool> inEffect;
		for (const GroupEvent& event : groupEvents)
		{
			bool& directed = inEffect[event.group];
			if (event.direction)
			{
				directed = event.controller.value == 0;
			}
			else if (directed)
			{
				resets.push_back(event.controller);
				directed = false;
			}
		}

		// The new part's root key and Global Transpose setting and the notes as they play with it (#954): where a
		// source part's global transposition differs from the new part's, the difference goes into the note, so that
		// it keeps the pitch it played with.
		std::vector<const MidiPart*> sources;
		std::vector<std::vector<const Candidate*>> sourceCandidates; // each source's, in the order of sources
		for (const Candidate& candidate : candidates)
		{
			const auto index =
				static_cast<size_t>(std::find(sources.begin(), sources.end(), candidate.source) - sources.begin());
			if (index == sources.size())
			{
				sources.push_back(candidate.source);
				sourceCandidates.emplace_back();
			}
			sourceCandidates[index].push_back(&candidate);
		}
		const GlobalSetting newSetting = sharedSetting(project, sources);
		// Without includeInserts the destination's MIDI modifiers play the merged notes, after the stored pitch and
		// before the part's velocity.
		const Modifiers destinationModifiers = settings.includeInserts || settings.destination == 0
			? Modifiers()
			: trackModifiers(project, settings.destination);
		// Writes a note as a new part with the setting plays it, pitchKept telling whether that has its part's
		// transpose and Global Transpose setting; false where it can't hold the pitch that played.
		const auto write =
			[&](const Candidate& candidate, const BouncedSetting& setting, bool pitchKept, std::vector<Note>& into)
		{
			Note copy = candidate.note;
			const MidiPart& source = *candidate.source;
			Modifiers transposed = candidate.modifiers;
			transposed.velocityShift -= setting.velocity;
			int newGlobal = 0;
			if (pitchKept)
			{
				transposed.transpose -= source.transpose;
			}
			else
			{
				transposed.transpose += globalAt(project, {source.rootKey, source.independent}, candidate.sourceAt);
				newGlobal = globalAt(project, setting.global, copy.position);
			}
			if (settings.includeInserts)
			{
				// With its global transpose before the modifiers' note ranges, as playback adds it, then less the new
				// part's where it lands.
				if (!modifyNote(transposed, candidate.seed, copy.pitch, copy.velocity))
					return true; // it didn't play
				const int pitch = copy.pitch - newGlobal;
				if (pitch < 0 || pitch > 127)
					return false;
				copy.pitch = static_cast<uint8_t>(pitch);
			}
			else
			{
				// The track's modifiers stay on the track and play after the stored pitch, so nothing lies between the
				// two global transposes: only their difference goes into the note. A pitch beyond 0-127 plays only
				// where the destination's modifiers bring it back.
				transposed.transpose -= newGlobal;
				const int played = copy.pitch + transposed.transpose + newGlobal;
				if (!modifyNote(transposed, candidate.seed, copy.pitch, copy.velocity))
					return !pitchPlays(destinationModifiers, played);
			}
			copy.pitch = static_cast<uint8_t>(
				destinationPitches.stored(copy.pitch, copy.channel, copy.position, candidate.ownPitch));
			copy.channel = destinationPitches.storedChannel(copy.channel, candidate.ownChannel, copy.pitch);
			into.push_back(copy);
			return true;
		};
		// What a source part's notes can't hold stays on a new part with a setting that holds it, for each part on its
		// own, as bounce does (#1240, #1253). Its velocity: where a note has velocity variance, which playback applies
		// before the part's velocity, and without includeInserts where the destination's modifiers compress the
		// velocity first. A pitch that plays and that the new part can't hold: with includeInserts, whose note ranges
		// lie between the part's transpose and the new part's global transpose, in an Independent part as it played;
		// without, in a part with its part's transpose and Global Transpose setting.
		std::map<BouncedSetting, std::vector<Note>> made;
		std::map<BouncedSetting, Ticks> firstStart;   // of each new part's earliest source part
		const BouncedSetting plain{0, 0, newSetting}; // of the parts that need none of their own
		const auto isPlain = [&](const BouncedSetting& setting) { return !(setting < plain) && !(plain < setting); };
		std::map<std::pair<int, int>, bool> velocities; // velocityInNote by MIDI 1.0 velocity and part velocity
		for (size_t i = 0; i < sources.size(); ++i)
		{
			const MidiPart* source = sources[i];
			const std::vector<const Candidate*>& own = sourceCandidates[i];
			const auto inNote = [&](const Candidate* candidate)
			{
				const std::pair<int, int> key{velocityToMidi1(candidate->note.velocity), source->velocity};
				const auto found = velocities.find(key);
				if (found != velocities.end())
					return found->second;
				return velocities[key] = velocityInNote(destinationModifiers, key.first, key.second);
			};
			// With includeInserts the track's modifiers then play the notes without the part's velocity, so only where
			// every note comes out alike.
			const bool velocityKept = source->velocity != 0 &&
				std::any_of(own.begin(), own.end(),
					[&](const Candidate* candidate)
					{
						return candidate->note.velocityVariance != 0.0 ||
							(!settings.includeInserts && !inNote(candidate));
					}) &&
				(!settings.includeInserts ||
					std::all_of(own.begin(), own.end(),
						[&](const Candidate* candidate)
						{
							return velocityOnPart(
								candidate->modifiers, candidate->seed, candidate->note, source->velocity);
						}));
			BouncedSetting setting{0, velocityKept ? source->velocity : 0, newSetting};
			std::vector<Note> notes;
			const auto writeAll = [&](bool pitchKept)
			{
				return std::all_of(own.begin(), own.end(),
					[&](const Candidate* candidate) { return write(*candidate, setting, pitchKept, notes); });
			};
			if (!writeAll(false))
			{
				notes.clear();
				const bool pitchKept = !settings.includeInserts;
				setting.transpose = pitchKept ? source->transpose : 0;
				setting.global = pitchKept ? GlobalSetting{source->rootKey, source->independent}
										   : GlobalSetting{newSetting.rootKey, true};
				writeAll(pitchKept); // which holds every note
			}
			// A part of its own that no note goes into (all left out, or playing nowhere) isn't made (#1491).
			if (notes.empty() && !isPlain(setting))
				continue;
			std::vector<Note>& into = made[setting];
			into.insert(into.end(), notes.begin(), notes.end());
			const Ticks from = partStart(project, *source);
			const auto first = firstStart.find(setting);
			if (first == firstStart.end() || from < first->second)
				firstStart[setting] = from;
		}
		// The new part with the setting of the parts that need none of their own first, the others in the order of
		// their earliest source parts, as bounce orders its parts; where nothing is merged, the first is there without
		// notes.
		if (made.empty())
			made[plain];
		std::vector<std::pair<BouncedSetting, std::vector<Note>>> groups(made.begin(), made.end());
		const auto order = [&](const BouncedSetting& setting)
		{ return std::make_pair(!isPlain(setting), isPlain(setting) ? Ticks{0} : firstStart.at(setting)); };
		std::stable_sort(groups.begin(), groups.end(),
			[&](const auto& a, const auto& b) { return order(a.first) < order(b.first); });

		PartChanges changes;
		ObjectId destination = settings.destination;
		if (destination == 0)
		{
			const ObjectId instrument = tracks.empty() ? 0 : instrumentOf(project, tracks.front());
			destination = addMidiTrack(
				project, defaultMidiTrackName(project), 0, project.tracks.size(), instrument, time::TimeBase::Musical);
			changes.tracks.push_back(destination);
			// The articulation map of the first track that has one, so that the directions select its slots.
			for (const ObjectId track : tracks)
				if (const ArticulationMap* map = articulationMapOf(project, track); map != nullptr)
				{
					project.trackById.at(destination).articulationMap = map->id;
					break;
				}
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
				if (sampleOf(partFrom) >= settings.start && sampleOf(partEnd(project, part)) <= settings.end)
				{
					changes.removedParts.push_back(part.id);
					it = project.midiParts.erase(it);
					continue;
				}
				const bool erases = std::any_of(part.notes.begin(), part.notes.end(),
										[&](const auto& note) { return inRange(partFrom + note.second.position); }) ||
					std::any_of(part.controllers.begin(), part.controllers.end(),
						[&](const auto& controller) { return inRange(partFrom + controller.second.position); });
				if (erases)
				{
					// The part's shared copies lie elsewhere: syncSharedParts would erase the events from them too.
					detach(project, part);
					for (auto note = part.notes.begin(); note != part.notes.end();)
						note = inRange(partFrom + note->second.position) ? part.notes.erase(note) : std::next(note);
					eraseControllers(
						part, [&](const Controller& controller) { return inRange(partFrom + controller.position); });
					changes.changedParts.push_back(part.id);
				}
				++it;
			}
		}

		for (const auto& [setting, notes] : groups)
			for (const Note& note : notes)
				start = std::min(start, note.position);
		for (const Controller& controller : controllers)
			start = std::min(start, controller.position);
		// Before the directions at the same positions, which come after them (soundSlots goes by id there).
		controllers.insert(controllers.begin(), resets.begin(), resets.end());
		for (auto& [key, controller] : chased)
		{
			controller.position = start;
			controllers.insert(controllers.begin(), controller);
		}
		// The first new part on the first lane, with the other controllers; the others on new lanes below the
		// destination's.
		int lane = 1;
		for (const auto& [id, other] : project.midiParts)
			if (other.track == destination)
				lane = std::max(lane, other.lane + 1);
		for (size_t i = 0; i < groups.size(); ++i)
		{
			auto& [setting, notes] = groups[i];
			MidiPart part;
			part.id = project.newId();
			part.track = destination;
			part.name = settings.name.empty() ? project.trackById.at(destination).name : settings.name;
			part.lane = i == 0 ? 0 : lane++;
			part.transpose = setting.transpose;
			part.velocity = setting.velocity;
			part.rootKey = setting.global.rootKey;
			part.independent = setting.global.independent;
			place(project, part, start, end);
			for (Note& note : notes)
			{
				note.id = project.newId();
				note.position -= start;
				part.notes[note.id] = note;
			}
			// Every new part gets the articulation events, which playback reads per part and never sends.
			for (Controller controller : controllers)
				if (i == 0 || controller.type == ControllerType::Articulation)
				{
					controller.id = project.newId();
					controller.position -= start;
					part.controllers[controller.id] = controller;
				}
			changes.addedParts.push_back(part.id);
			project.midiParts[part.id] = std::move(part);
		}
		return changes;
	}

	PartChanges dissolve(
		Project& project, const std::vector<ObjectId>& parts, DissolveBy by, bool optimize, DissolveTo to)
	{
		PartChanges changes;
		// The key of each event: its channel or pitch, or -1 for events that belong to neither.
		const auto noteKey = [by](const Note& note) { return by == DissolveBy::Channel ? note.channel : note.pitch; };
		const auto controllerKey = [by](const Controller& controller) -> int
		{
			if (by == DissolveBy::Channel)
				return controller.type == ControllerType::SysEx || controller.type == ControllerType::Articulation
					? -1
					: controller.channel;
			return controller.type == ControllerType::PolyPressure ? controller.number : -1;
		};
		const auto suffixOf = [by](int key)
		{ return by == DissolveBy::Channel ? "Ch " + std::to_string(key + 1) : pitchName(key); };

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
				for (const auto& [noteId, note] : playedNotes(project, part))
					keys.insert(noteKey(note));
				for (const auto& [controllerId, controller] : part.controllers)
					if (controllerKey(controller) >= 0)
						keys.insert(controllerKey(controller));
			}
			if (keys.empty())
				continue;

			// A track per key after the source track, or a lane per key below the lanes of its parts.
			const Track source = project.trackById.at(trackId);
			std::map<int, ObjectId> trackFor;
			std::map<int, int> laneFor;
			if (to == DissolveTo::Tracks)
			{
				const ObjectId instrument = instrumentOf(project, trackId);
				const std::optional<int> trackChannel = trackMidiChannel(project, trackId);
				const std::vector<ObjectId>& siblings =
					source.parent == 0 ? project.tracks : project.trackById.at(source.parent).children;
				auto index =
					static_cast<size_t>(std::find(siblings.begin(), siblings.end(), trackId) - siblings.begin());
				for (const int key : keys)
				{
					trackFor[key] = addMidiTrack(project, source.name + " " + suffixOf(key), source.parent, ++index,
						instrument, source.timeBase);
					// The track's MIDI modifiers, so that the new parts play as the part did; by channel each new
					// track set to the channel it holds (reference manual p. 1122), which it plays on also where the
					// track played every channel on its own, by pitch to the track's.
					Channel& added = project.channels.at(project.trackById.at(trackFor[key]).channel);
					added.modifiers = project.channels.at(source.channel).modifiers;
					added.midiChannel = by == DissolveBy::Channel ? std::optional<int>(key) : trackChannel;
					changes.tracks.push_back(trackFor[key]);
				}
			}
			else
			{
				int lane = 0;
				for (const auto& [id, part] : project.midiParts)
					if (part.track == trackId)
						lane = std::max(lane, part.lane + 1);
				for (const int key : keys)
				{
					trackFor[key] = trackId;
					laneFor[key] = lane++;
				}
			}

			for (const ObjectId id : found->second)
			{
				MidiPart& original = project.midiParts.at(id);
				std::map<int, MidiPart> pieces;
				std::vector<Controller> keyless;
				for (const auto& [noteId, note] : playedNotes(project, original))
				{
					Note copy = note;
					copy.id = project.newId();
					pieces[noteKey(note)].notes[copy.id] = copy;
				}
				for (const auto& [controllerId, controller] : original.controllers)
				{
					Controller copy = controller;
					copy.id = project.newId();
					const int key = controllerKey(controller);
					if (key < 0)
						keyless.push_back(copy);
					else
						pieces[key].controllers[copy.id] = copy;
				}
				if (pieces.empty())
					continue;
				for (const Controller& controller : keyless)
					pieces.begin()->second.controllers[controller.id] = controller;
				const Ticks originalStart = partStart(project, original);
				const Ticks originalEnd = partEnd(project, original);
				for (auto& [key, piece] : pieces)
				{
					piece.id = project.newId();
					piece.track = trackFor.at(key);
					piece.mute = original.mute;
					piece.transpose = original.transpose;
					piece.velocity = original.velocity;
					piece.rootKey = original.rootKey;
					piece.independent = original.independent;
					Ticks pieceStart = originalStart;
					Ticks pieceEnd = originalEnd;
					if (to == DissolveTo::Lanes)
					{
						piece.name = original.name + " " + suffixOf(key);
						piece.color = original.color;
						piece.lane = laneFor.at(key);
					}
					else
					{
						piece.name = project.trackById.at(piece.track).name;
					}
					if (optimize && to == DissolveTo::Tracks)
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
						pieceStart = std::max(originalStart, bars.barStart(bars.toBarsBeats(first).bar));
						const int64_t lastBar = bars.toBarsBeats(std::max(first, last - 1)).bar;
						pieceEnd = std::min(originalEnd, bars.barStart(lastBar + 1));
					}
					place(project, piece, pieceStart, pieceEnd);
					const Ticks shift = partStart(project, piece) - originalStart;
					for (auto& [noteId, note] : piece.notes)
						note.position -= shift;
					for (auto& [controllerId, controller] : piece.controllers)
						controller.position -= shift;
					changes.addedParts.push_back(piece.id);
					project.midiParts[piece.id] = std::move(piece);
				}
				if (!original.mute)
				{
					original.mute = true;
					changes.changedParts.push_back(id);
				}
			}
		}
		return changes;
	}

	PartChanges bounce(Project& project, const std::vector<ObjectId>& parts)
	{
		PartChanges changes;
		std::vector<ObjectId> playing;
		for (const ObjectId id : parts)
		{
			addUnique(changes.removedParts, id);
			if (!project.midiParts.at(id).mute)
				playing.push_back(id);
		}
		std::vector<MidiPart> combined;
		if (!playing.empty())
		{
			const ObjectId track = project.midiParts.at(playing.front()).track;
			const TrackPitches pitches(project, track);
			const Modifiers modifiers = trackModifiers(project, track);
			// The new part has the root key and Global Transpose setting the parts share, or else the project root
			// key and follow, as merge takes it (#954); for each part, the difference in global transpose goes into
			// the notes with its transpose.
			// Only the parts with notes count, as merge asks the parts its notes come from.
			std::vector<const MidiPart*> sources;
			for (const ObjectId id : playing)
			{
				const MidiPart& part = project.midiParts.at(id);
				const Ticks length = partEnd(project, part) - partStart(project, part);
				const std::map<ObjectId, Note> notes = playedNotes(project, part);
				if (std::any_of(notes.begin(), notes.end(),
						[length](const auto& each) { return each.second.position < length; }))
					sources.push_back(&part);
			}
			if (sources.empty())
				for (const ObjectId id : playing)
					sources.push_back(&project.midiParts.at(id));
			const GlobalSetting shared = sharedSetting(project, sources);
			// A drum map or the chord track maps a note before its part's transpose, and the track's MIDI modifiers
			// add to it before checking the range and compress the velocity before the part's velocity, which the
			// notes can't always hold: then the part keeps them, in a new part for each setting.
			const auto pitchesHold = [&](const MidiPart& part, const GlobalSetting& against)
			{
				const Ticks from = partStart(project, part);
				const Ticks length = partEnd(project, part) - from;
				const std::map<ObjectId, Note> notes = playedNotes(project, part);
				return std::all_of(notes.begin(), notes.end(),
					[&](const auto& each)
					{
						const Note& note = each.second;
						const Ticks at = from + note.position;
						const int global = globalAt(project, against, at);
						const auto plays = [&](int target) { return pitchPlays(modifiers, target + global); };
						return note.position >= length ||
							pitches.transposesInNote(note, semitonesAgainst(project, part, against, at), at, plays);
					});
			};
			std::map<std::pair<int, int>, bool> velocities; // by MIDI 1.0 velocity and part velocity
			const auto velocitiesHold = [&](const MidiPart& part)
			{
				const Ticks length = partEnd(project, part) - partStart(project, part);
				const std::map<ObjectId, Note> notes = playedNotes(project, part);
				return part.velocity == 0 ||
					std::all_of(notes.begin(), notes.end(),
						[&](const auto& each)
						{
							const Note& note = each.second;
							if (note.position >= length || pitches.silent(note.pitch))
								return true;
							// Playback varies a note's velocity before adding the part's.
							if (note.velocityVariance != 0.0)
								return false;
							const std::pair<int, int> key{velocityToMidi1(note.velocity), part.velocity};
							const auto found = velocities.find(key);
							if (found != velocities.end())
								return found->second;
							return velocities[key] = velocityInNote(modifiers, key.first, key.second);
						});
			};
			std::map<BouncedSetting, std::vector<Bounced>> groups;
			for (const ObjectId id : playing)
			{
				const MidiPart& part = project.midiParts.at(id);
				const GlobalSetting own{part.rootKey, part.independent};
				// Against the shared global setting, else its own with only its transpose in the notes, else none of
				// them.
				Bounced bounced{id, true, shared, velocitiesHold(part)};
				if (!pitchesHold(part, shared))
				{
					bounced.against = own;
					bounced.pitchInNotes = !(own == shared) && pitchesHold(part, own);
				}
				BouncedSetting setting{bounced.pitchInNotes ? 0 : part.transpose, 0, bounced.against};
				if (!bounced.velocityInNotes)
					setting.velocity = part.velocity;
				groups[setting].push_back(bounced);
			}
			// In the order of their first parts, each on the lowest lane of its parts on which neither an earlier one
			// nor a part of the track that stays overlaps it.
			std::vector<std::pair<BouncedSetting, std::vector<Bounced>>> ordered(groups.begin(), groups.end());
			const auto firstStart = [&](const std::vector<Bounced>& group)
			{
				Ticks earliest = std::numeric_limits<Ticks>::max();
				for (const Bounced& bounced : group)
					earliest = std::min(earliest, partStart(project, project.midiParts.at(bounced.id)));
				return earliest;
			};
			std::stable_sort(ordered.begin(), ordered.end(),
				[&](const auto& a, const auto& b) { return firstStart(a.second) < firstStart(b.second); });
			for (const auto& [setting, group] : ordered)
			{
				MidiPart part = combine(project, group, setting, pitches);
				const Ticks from = partStart(project, part);
				const Ticks to = partEnd(project, part);
				const auto overlaps = [&](const MidiPart& other, int lane)
				{ return other.lane == lane && partStart(project, other) < to && from < partEnd(project, other); };
				const auto taken = [&](int lane)
				{
					const auto stays = [&](const auto& each)
					{
						return each.second.track == track &&
							std::find(changes.removedParts.begin(), changes.removedParts.end(), each.first) ==
							changes.removedParts.end() &&
							overlaps(each.second, lane);
					};
					return std::any_of(combined.begin(), combined.end(),
							   [&](const MidiPart& other) { return overlaps(other, lane); }) ||
						std::any_of(project.midiParts.begin(), project.midiParts.end(), stays);
				};
				while (taken(part.lane))
					++part.lane;
				combined.push_back(std::move(part));
			}
		}
		for (const ObjectId id : changes.removedParts)
			project.midiParts.erase(id);
		for (MidiPart& part : combined)
		{
			changes.addedParts.push_back(part.id);
			project.midiParts[part.id] = std::move(part);
		}
		return changes;
	}

	AutomationChanges extractAutomation(Project& project, const std::vector<ObjectId>& parts)
	{
		AutomationChanges changes;
		std::set<ObjectId> placed; // the points this run adds
		for (const ObjectId id : parts)
		{
			MidiPart& part = project.midiParts.at(id);
			if (part.mute)
				continue;
			const Track& track = project.trackById.at(part.track);
			const Ticks from = partStart(project, part);
			const Ticks to = partEnd(project, part);
			// The events that move, by channel and controller number.
			std::map<std::pair<uint8_t, uint8_t>, std::vector<const Controller*>> lanes;
			std::set<ObjectId> moved;
			for (const auto& [controllerId, controller] : part.controllers)
				if (controller.type == ControllerType::Control && isContinuousController(controller.number) &&
					controller.position < to - from)
				{
					lanes[{controller.channel, controller.number}].push_back(&controller);
					moved.insert(controllerId);
				}
			if (lanes.empty())
				continue;
			// The part's range in the time base of the track's automation.
			const int64_t rangeStart = trackPosition(project, track, from).value;
			const int64_t rangeEnd = trackPosition(project, track, to).value;
			for (auto& [key, events] : lanes)
			{
				std::sort(events.begin(), events.end(), comesBefore);
				AutomationTarget target;
				target.type = AutomationTargetType::Controller;
				target.channel = key.first;
				target.number = key.second;
				AutomationLane* lane = nullptr;
				for (auto& [laneId, each] : project.automation)
					if (each.track == part.track && each.target == target)
						lane = &each;
				if (lane == nullptr)
				{
					AutomationLane added;
					added.id = project.newId();
					added.track = part.track;
					added.target = target;
					changes.addedLanes.push_back(added.id);
					lane = &(project.automation[added.id] = std::move(added));
				}
				else
				{
					// Points an earlier part of this run added stay, so overlapping parts keep each other's.
					for (auto point = lane->points.begin(); point != lane->points.end();)
					{
						const int64_t at = point->second.position.value;
						const bool before = placed.count(point->first) == 0;
						point =
							before && at >= rangeStart && at < rangeEnd ? lane->points.erase(point) : std::next(point);
					}
					lane->read = true; // what was moved has to play
					addUnique(changes.changedLanes, lane->id);
				}
				for (const Controller* event : events)
				{
					AutomationPoint point;
					point.id = project.newId();
					point.position = trackPosition(project, track, from + event->position);
					point.value =
						static_cast<double>(event->value) / static_cast<double>(std::numeric_limits<uint32_t>::max());
					point.shape = AutomationShape::Step;
					placed.insert(point.id);
					lane->points[point.id] = point;
				}
			}
			eraseControllers(part, [&moved](const Controller& controller) { return moved.count(controller.id) != 0; });
			// Points play at this part's place only, so a shared copy leaves its copies, which keep their controllers:
			// syncSharedParts would give them this part's.
			detach(project, part);
			changes.changedParts.push_back(id);
		}
		// A lane added for one part and changed for a later one was added.
		for (const ObjectId id : changes.addedLanes)
			changes.changedLanes.erase(
				std::remove(changes.changedLanes.begin(), changes.changedLanes.end(), id), changes.changedLanes.end());
		return changes;
	}
}
