// SPDX-License-Identifier: GPL-3.0-or-later
// Editing parts and events in the project window (#85, reference manual pp. 245-277): the event.* commands, which
// take audio events and MIDI parts alike by id, and shared copies of MIDI parts. Like every command that changes the
// project, each is one undo step and changes nothing if it fails (ProjectModel::addEdit). And range editing (#88,
// reference manual pp. 285-294): the range.* commands and the range clipboard.
// docs/features/parts-and-events.md and docs/features/range-editing.md describe them.
#include "engine/audio/automation.h"
#include "engine/audio/fades.h"
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/rendersnapshot.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
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

		// An audio event, an audio part or a MIDI part. Pointers into the project's maps stay valid while other objects
		// are added.
		struct Item
		{
			AudioEvent* audio = nullptr;
			MidiPart* part = nullptr;
			AudioPart* audioPart = nullptr;

			explicit operator bool() const { return audio != nullptr || part != nullptr || audioPart != nullptr; }
			ObjectId id() const { return pick(&AudioEvent::id, &AudioPart::id, &MidiPart::id); }
			ObjectId track() const { return pick(&AudioEvent::track, &AudioPart::track, &MidiPart::track); }
			bool& lock() const { return pick(&AudioEvent::lock, &AudioPart::lock, &MidiPart::lock); }
			bool& mute() const { return pick(&AudioEvent::mute, &AudioPart::mute, &MidiPart::mute); }
			std::string& color() const { return pick(&AudioEvent::color, &AudioPart::color, &MidiPart::color); }
			int& lane() const { return pick(&AudioEvent::lane, &AudioPart::lane, &MidiPart::lane); }
			bool locked() const { return lock(); }
			time::Position& position() const
			{
				return pick(&AudioEvent::position, &AudioPart::position, &MidiPart::position);
			}
			// A part's length, in the unit of its position's time base; nullptr for an audio event.
			int64_t* length() const
			{
				if (audio != nullptr)
					return nullptr;
				return audioPart != nullptr ? &audioPart->length : &midi().length;
			}

		private:
			// A member of whichever object the item is.
			template <typename T>
			T& pick(T AudioEvent::* inAudio, T AudioPart::* inAudioPart, T MidiPart::* inMidi) const
			{
				if (audio != nullptr)
					return audio->*inAudio;
				if (audioPart != nullptr)
					return audioPart->*inAudioPart;
				return midi().*inMidi;
			}

			// The MIDI part of an item that is neither an audio event nor an audio part. Checked, so the static
			// analyzer sees that one of the three is always set.
			MidiPart& midi() const
			{
				if (part == nullptr)
					throw CommandError("an event is neither an audio event nor a part");
				return *part;
			}
		};

		Item audioItem(AudioEvent& event)
		{
			return {&event, nullptr, nullptr};
		}

		Item audioPartItem(AudioPart& part)
		{
			return {nullptr, nullptr, &part};
		}

		Item findItem(Project& project, ObjectId id)
		{
			if (const auto audio = project.audioEvents.find(id); audio != project.audioEvents.end())
				return audioItem(audio->second);
			if (const auto audioPart = project.audioParts.find(id); audioPart != project.audioParts.end())
				return audioPartItem(audioPart->second);
			const auto part = project.midiParts.find(id);
			require(part != project.midiParts.end(), "no audio event or part " + std::to_string(id));
			return {nullptr, &part->second};
		}

		// The audio events in a part.
		std::vector<AudioEvent*> eventsIn(Project& project, ObjectId part)
		{
			std::vector<AudioEvent*> events;
			for (auto& [id, event] : project.audioEvents)
				if (event.part == part)
					events.push_back(&event);
			return events;
		}

		// The items a command names in events, each once. Editing ones must not be locked.
		std::vector<Item> itemsFrom(Project& project, const Json& parameters, bool editing)
		{
			const auto ids = parameters.at("events").get<std::vector<ObjectId>>();
			require(!ids.empty(), "events must not be empty");
			std::set<ObjectId> seen;
			std::vector<Item> items;
			items.reserve(ids.size());
			for (const ObjectId id : ids)
			{
				require(seen.insert(id).second, "event " + std::to_string(id) + " is named twice");
				const Item item = findItem(project, id);
				require(!editing || !item.locked(), "event " + std::to_string(id) + " is locked");
				items.push_back(item);
			}
			return items;
		}

		Json idsOf(const std::vector<Item>& items)
		{
			Json ids = Json::array();
			for (const Item& item : items)
				ids.push_back(item.id());
			return ids;
		}

		// Announces an item; an audio part's events come and change with it.
		void announce(Project& project, CommandContext& context, const Item& item, const std::string& what)
		{
			if (item.audio != nullptr)
			{
				context.emit("audioEvent." + what, {{"event", toJson(*item.audio)}});
			}
			else if (item.audioPart != nullptr)
			{
				context.emit("audioPart." + what, {{"part", toJson(*item.audioPart)}});
				for (const AudioEvent* event : eventsIn(project, item.audioPart->id))
					context.emit("audioEvent." + what, {{"event", toJson(*event)}});
			}
			else
			{
				context.emit("midiPart." + what, {{"part", toJson(*item.part)}});
			}
		}

		// Time.

		time::Samples samples(const Project& project, const time::Position& position)
		{
			return time::toSamples(position, project.tempo, project.sampleRate);
		}

		time::Ticks ticks(const Project& project, const time::Position& position)
		{
			return time::toTicks(position, project.tempo, project.sampleRate);
		}

		time::Position rebased(const Project& project, const time::Position& position, time::TimeBase base)
		{
			return time::rebase(position, base, project.tempo, project.sampleRate);
		}

		time::Position endOf(const Project& project, const Item& item)
		{
			if (const int64_t* length = item.length())
				return {item.position().base, item.position().value + *length};
			return rebased(
				project, {time::TimeBase::Linear, eventSpan(project, *item.audio).end}, item.audio->position.base);
		}

		// A distance on the timeline: ticks are exact for events on musical tracks, samples for linear ones.
		struct Shift
		{
			int64_t value = 0;
			bool ticks = false;
		};

		Shift shiftFrom(const Json& parameters)
		{
			require(!(parameters.contains("delta") && parameters.contains("ticks")), "give delta or ticks, not both");
			if (parameters.contains("ticks"))
				return {parameters.at("ticks").get<int64_t>(), true};
			return {parameters.value("delta", int64_t{0}), false};
		}

		// A position moved by shift, in its own time base. Before the project start it is refused, or clamped to it.
		time::Position shifted(const Project& project, const time::Position& position, const Shift& shift, bool refuse)
		{
			time::Position moved = shift.ticks
				? time::Position{time::TimeBase::Musical, ticks(project, position) + shift.value}
				: time::Position{time::TimeBase::Linear, samples(project, position) + shift.value};
			if (moved.value < 0)
			{
				require(!refuse, "the event would start before the project start");
				moved.value = 0;
			}
			return rebased(project, moved, position.base);
		}

		// Blocks of events (repeat, fill loop) are measured in ticks when every event is on a musical track, so that
		// copies land exactly on the beat, otherwise in samples.
		bool allMusical(const std::vector<Item>& items)
		{
			return std::all_of(items.begin(), items.end(),
				[](const Item& item) { return item.position().base == time::TimeBase::Musical; });
		}

		int64_t inUnit(const Project& project, const time::Position& position, bool musical)
		{
			return musical ? ticks(project, position) : samples(project, position);
		}

		// Where the items start and end, in the unit.
		std::pair<int64_t, int64_t> block(const Project& project, const std::vector<Item>& items, bool musical)
		{
			int64_t start = std::numeric_limits<int64_t>::max();
			int64_t end = std::numeric_limits<int64_t>::min();
			for (const Item& item : items)
			{
				start = std::min(start, inUnit(project, item.position(), musical));
				end = std::max(end, inUnit(project, endOf(project, item), musical));
			}
			return {start, end};
		}

		void checkTrack(const Project& project, const Item& item, ObjectId track)
		{
			const auto found = project.trackById.find(track);
			require(found != project.trackById.end(), "no track " + std::to_string(track));
			const TrackType type = found->second.type;
			if (item.audio != nullptr)
			{
				require(type == TrackType::Audio, "audio events can only be on an audio track");
				require(item.audio->part == 0 || track == item.audio->track,
					"audio events in a part stay on the part's track");
			}
			else if (item.audioPart != nullptr)
			{
				require(type == TrackType::Audio, "audio parts can only be on an audio track");
			}
			else
			{
				require(type == TrackType::Midi || type == TrackType::Instrument,
					"MIDI parts can only be on a MIDI or instrument track");
			}
		}

		// Moves an item by shift onto a track, keeping its length; a part's length goes into the track's time base.
		// An audio part's events go with it.
		void place(Project& project, const Item& item, const Shift& shift, ObjectId track)
		{
			const time::TimeBase base = project.trackById.at(track).timeBase;
			const time::Position start = rebased(project, shifted(project, item.position(), shift, true), base);
			if (int64_t* length = item.length())
			{
				const time::Position end = rebased(project, shifted(project, endOf(project, item), shift, true), base);
				*length = std::max<int64_t>(1, end.value - start.value);
			}
			if (item.audio != nullptr)
			{
				item.audio->track = track;
			}
			else if (item.audioPart != nullptr)
			{
				// The events first: one before the project start refuses the move, as the part's own start does.
				for (AudioEvent* event : eventsIn(project, item.audioPart->id))
				{
					event->position = rebased(project, shifted(project, event->position, shift, true), base);
					event->track = track;
				}
				item.audioPart->track = track;
			}
			else
			{
				item.part->track = track;
			}
			item.position() = start;
		}

		// Takes a part out of its shared copies before its contents change for its own sake.
		void detach(Project& project, MidiPart& part)
		{
			if (part.shared == 0)
				return;
			part.shared = 0;
			renumber(project, part);
		}

		// A copy with a new id, unlocked and without a crossfade. A MIDI part's notes and controllers get new ids, or
		// for a shared copy are the original's, with which it then shares them.
		Item copyOf(Project& project, const Item& item, bool shared, CommandContext& context)
		{
			if (item.audio != nullptr)
			{
				AudioEvent copy = *item.audio;
				copy.id = project.newId();
				copy.front = 0; // in front, as added last
				copy.lock = false;
				copy.crossfade = Crossfade();
				return audioItem(project.audioEvents[copy.id] = copy);
			}
			if (item.audioPart != nullptr)
			{
				// Copies of its events, with the crossfades between them.
				AudioPart part = *item.audioPart;
				part.id = project.newId();
				part.lock = false;
				AudioPart& copy = project.audioParts[part.id] = part;
				std::map<ObjectId, ObjectId> ids;
				std::vector<AudioEvent> events;
				for (const AudioEvent* event : eventsIn(project, item.audioPart->id))
				{
					ids[event->id] = project.newId();
					events.push_back(*event);
				}
				for (AudioEvent& event : events)
				{
					const auto from = ids.find(event.crossfade.from);
					if (from == ids.end())
						event.crossfade = Crossfade();
					else
						event.crossfade.from = from->second;
					event.id = ids.at(event.id);
					event.part = copy.id;
					project.audioEvents[event.id] = event;
				}
				return audioPartItem(copy);
			}
			if (shared && item.part->shared == 0)
			{
				item.part->shared = project.newId();
				announce(project, context, item, "changed");
			}
			MidiPart copy = *item.part;
			copy.id = project.newId();
			copy.lock = false;
			if (!shared)
			{
				copy.shared = 0;
				renumber(project, copy);
			}
			const ObjectId id = copy.id;
			return {nullptr, &(project.midiParts[id] = std::move(copy))};
		}

		// Moves notes and controllers by ticks; those that would start before the part are removed.
		// ponytail: hidden content is dropped, not kept for revealing later; keep it once parts have an offset.
		void shiftContents(MidiPart& part, time::Ticks delta)
		{
			for (auto note = part.notes.begin(); note != part.notes.end();)
			{
				note->second.position += delta;
				note = note->second.position < 0 ? part.notes.erase(note) : std::next(note);
			}
			for (auto controller = part.controllers.begin(); controller != part.controllers.end();)
			{
				controller->second.position += delta;
				controller =
					controller->second.position < 0 ? part.controllers.erase(controller) : std::next(controller);
			}
		}

		// Moves an audio part's events by ticks.
		void shiftEvents(Project& project, const AudioPart& part, time::Ticks delta)
		{
			for (AudioEvent* event : eventsIn(project, part.id))
				event->position = shifted(project, event->position, {delta, true}, false);
		}

		// Resizing (reference manual p. 265): the start or the end goes to a position, within the clip and at least a
		// sample frame (or a tick) long. Normal sizing reveals or hides content; sizing moves contents moves the
		// content with the edge.
		void setStart(Project& project, const Item& item, const time::Position& start, bool moveContents)
		{
			if (item.audioPart != nullptr)
			{
				// Its events keep their time: normal sizing reveals or hides them.
				AudioPart& part = *item.audioPart;
				const int64_t end = part.position.value + part.length;
				const int64_t value =
					std::clamp<int64_t>(rebased(project, start, part.position.base).value, 0, end - 1);
				const time::Position moved{part.position.base, value};
				if (moveContents)
					shiftEvents(project, part, ticks(project, moved) - ticks(project, part.position));
				part.position = moved;
				part.length = end - value;
				return;
			}
			if (item.audio != nullptr)
			{
				AudioEvent& event = *item.audio;
				const EventSpan span = eventSpan(project, event);
				const time::Samples media = project.media.at(project.clips.at(event.clip).media).length;
				const int64_t wanted =
					std::llround(static_cast<double>(samples(project, start) - span.start) / span.ratio);
				int64_t frames = std::min(wanted, event.length - 1);
				frames = std::max(frames, moveContents ? event.length - (media - event.offset) : -event.offset);
				frames = std::max(frames, -static_cast<int64_t>(static_cast<double>(span.start) / span.ratio));
				if (!moveContents)
					event.offset += frames;
				event.length -= frames;
				event.position = frames == wanted
					? rebased(project, start, event.position.base)
					: rebased(project,
						  {time::TimeBase::Linear, span.start + std::llround(static_cast<double>(frames) * span.ratio)},
						  event.position.base);
				return;
			}
			MidiPart& part = *item.part;
			const int64_t end = part.position.value + part.length;
			const int64_t value = std::clamp<int64_t>(rebased(project, start, part.position.base).value, 0, end - 1);
			const time::Position moved{part.position.base, value};
			if (!moveContents && moved != part.position)
			{
				detach(project, part);
				shiftContents(part, ticks(project, part.position) - ticks(project, moved));
			}
			part.position = moved;
			part.length = end - value;
		}

		void setEnd(Project& project, const Item& item, const time::Position& end, bool moveContents)
		{
			if (item.audioPart != nullptr)
			{
				AudioPart& part = *item.audioPart;
				const time::Position oldEnd = endOf(project, item);
				part.length =
					std::max<int64_t>(1, rebased(project, end, part.position.base).value - part.position.value);
				if (moveContents)
					shiftEvents(project, part, ticks(project, endOf(project, item)) - ticks(project, oldEnd));
				return;
			}
			if (item.audio != nullptr)
			{
				AudioEvent& event = *item.audio;
				const EventSpan span = eventSpan(project, event);
				const time::Samples media = project.media.at(project.clips.at(event.clip).media).length;
				const time::Samples to = samples(project, end);
				if (moveContents)
				{
					int64_t frames = std::llround(static_cast<double>(to - span.end) / span.ratio);
					frames = std::clamp<int64_t>(frames, 1 - event.length, event.offset);
					event.offset -= frames;
					event.length += frames;
				}
				else
				{
					event.length = std::clamp<int64_t>(
						std::llround(static_cast<double>(to - span.start) / span.ratio), 1, media - event.offset);
				}
				return;
			}
			MidiPart& part = *item.part;
			const time::Position oldEnd = endOf(project, item);
			const int64_t length =
				std::max<int64_t>(1, rebased(project, end, part.position.base).value - part.position.value);
			const time::Position newEnd{part.position.base, part.position.value + length};
			if (moveContents && newEnd != oldEnd)
			{
				detach(project, part);
				shiftContents(part, ticks(project, newEnd) - ticks(project, oldEnd));
			}
			part.length = length;
		}

		// Splits an item at a position in samples and returns the new item after it, or nothing if the position
		// isn't inside it.
		Item splitAt(Project& project, const Item& item, time::Samples at, bool splitNotes, CommandContext& context)
		{
			if (item.audioPart != nullptr)
			{
				// The events that the position cuts are split too; those after it go into the part after it.
				AudioPart& part = *item.audioPart;
				const int64_t end = part.position.value + part.length;
				const int64_t split = rebased(project, {time::TimeBase::Linear, at}, part.position.base).value;
				if (split <= part.position.value || split >= end)
					return {};
				AudioPart right = part;
				right.id = project.newId();
				right.position = {part.position.base, split};
				right.length = end - split;
				part.length = split - part.position.value;
				project.audioParts[right.id] = right;
				for (AudioEvent* event : eventsIn(project, part.id))
				{
					if (eventSpan(project, *event).start >= at)
					{
						event->part = right.id;
						continue;
					}
					if (const Item piece = splitAt(project, audioItem(*event), at, false, context))
						piece.audio->part = right.id;
				}
				return audioPartItem(project.audioParts.at(right.id));
			}
			if (item.audio != nullptr)
			{
				AudioEvent& event = *item.audio;
				const EventSpan span = eventSpan(project, event);
				const int64_t frames = std::llround(static_cast<double>(at - span.start) / span.ratio);
				if (at <= span.start || at >= span.end || frames <= 0 || frames >= event.length)
					return {};
				AudioEvent right = event;
				right.id = project.newId();
				right.front = frontOf(event); // the piece after the split stays where the event is in the stack
				right.position = rebased(project, {time::TimeBase::Linear, at}, event.position.base);
				right.offset += frames;
				right.length -= frames;
				right.fadeIn = Fade();
				right.crossfade = Crossfade();
				event.length = frames;
				event.fadeOut = Fade();
				// An event that crossfaded from this one now overlaps the piece after the split.
				for (auto& [id, other] : project.audioEvents)
					if (other.crossfade.from == event.id && eventSpan(project, other).start >= at)
					{
						other.crossfade.from = right.id;
						context.emit("audioEvent.changed", {{"event", toJson(other)}});
					}
				return {&(project.audioEvents[right.id] = right), nullptr};
			}
			MidiPart& part = *item.part;
			const int64_t end = part.position.value + part.length;
			const int64_t split = rebased(project, {time::TimeBase::Linear, at}, part.position.base).value;
			if (split <= part.position.value || split >= end)
				return {};
			detach(project, part);
			const time::Ticks offset = ticks(project, {part.position.base, split}) - ticks(project, part.position);
			MidiPart right;
			right.id = project.newId();
			right.track = part.track;
			right.name = part.name;
			right.position = {part.position.base, split};
			right.length = end - split;
			right.mute = part.mute;
			right.lock = part.lock;
			right.color = part.color;
			right.lane = part.lane;
			for (auto it = part.notes.begin(); it != part.notes.end();)
			{
				Note& note = it->second;
				if (note.position >= offset)
				{
					Note moved = note;
					moved.id = project.newId();
					moved.position -= offset;
					right.notes[moved.id] = moved;
					it = part.notes.erase(it);
					continue;
				}
				if (splitNotes && note.position + note.length > offset)
				{
					// The rest of the note starts the part after the split.
					Note rest = note;
					rest.id = project.newId();
					rest.position = 0;
					rest.length = note.position + note.length - offset;
					rest.origin.reset();
					right.notes[rest.id] = rest;
					note.length = offset - note.position;
					note.origin.reset();
				}
				++it;
			}
			for (auto it = part.controllers.begin(); it != part.controllers.end();)
			{
				if (it->second.position < offset)
				{
					++it;
					continue;
				}
				Controller moved = it->second;
				moved.id = project.newId();
				moved.position -= offset;
				right.controllers[moved.id] = std::move(moved);
				it = part.controllers.erase(it);
			}
			part.length = split - part.position.value;
			const ObjectId id = right.id;
			return {nullptr, &(project.midiParts[id] = std::move(right))};
		}

		// Removes an audio event or a MIDI part; crossfades from a removed event go to replacement, or end.
		void removeItem(Project& project, const Item& item, ObjectId replacement, CommandContext& context)
		{
			const ObjectId id = item.id();
			if (item.audioPart != nullptr)
			{
				for (AudioEvent* event : eventsIn(project, id))
					removeItem(project, audioItem(*event), 0, context);
				project.audioParts.erase(id);
				context.emit("audioPart.removed", {{"part", id}});
				return;
			}
			if (item.audio != nullptr)
			{
				project.audioEvents.erase(id);
				for (auto& [otherId, other] : project.audioEvents)
					if (other.crossfade.from == id)
					{
						other.crossfade.from = replacement;
						if (replacement == 0)
							other.crossfade = Crossfade();
						context.emit("audioEvent.changed", {{"event", toJson(other)}});
					}
				context.emit("audioEvent.removed", {{"event", id}});
				return;
			}
			project.midiParts.erase(id);
			context.emit("midiPart.removed", {{"part", id}});
		}

		// Commands.

		// Events with no length in the block's unit (a frame of a 192 kHz clip at 48 kHz) can't be repeated: every
		// copy would lie on the one before.
		constexpr const char* g_tooShort = "the events are too short to repeat";

		Json moveEvents(Project& project, CommandContext& context, bool follow)
		{
			const Json& parameters = context.parameters();
			const std::vector<Item> items = itemsFrom(project, parameters, true);
			const Shift shift = shiftFrom(parameters);
			for (const Item& item : items)
			{
				const ObjectId from = item.track();
				const ObjectId track = parameters.value("track", from);
				checkTrack(project, item, track);
				const time::Position start = item.position();
				const time::Position end = endOf(project, item);
				place(project, item, shift, track);
				if (follow && track == from)
					followAutomation(project, track, start, end, item.position(), context);
				announce(project, context, item, "changed");
			}
			return {{"events", idsOf(items)}};
		}

		Json copyEvents(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const std::vector<Item> items = itemsFrom(project, parameters, false);
			const Shift shift = shiftFrom(parameters);
			const bool shared = parameters.value("shared", false);
			std::vector<Item> copies;
			copies.reserve(items.size());
			for (const Item& item : items)
			{
				const ObjectId track = parameters.value("track", item.track());
				checkTrack(project, item, track);
				const Item copy = copyOf(project, item, shared, context);
				place(project, copy, shift, track);
				announce(project, context, copy, "added");
				copies.push_back(copy);
			}
			return {{"events", idsOf(copies)}};
		}

		Json duplicateEvents(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const std::vector<Item> items = itemsFrom(project, parameters, false);
			const int64_t count = parameters.value("count", int64_t{1});
			require(count >= 1 && count <= 1000, "count must be 1 to 1000");
			const bool shared = parameters.value("shared", false);
			const bool musical = allMusical(items);
			const auto [start, end] = block(project, items, musical);
			require(end > start, g_tooShort);
			std::vector<Item> copies;
			for (int64_t k = 1; k <= count; ++k)
				for (const Item& item : items)
				{
					const Item copy = copyOf(project, item, shared, context);
					place(project, copy, {k * (end - start), musical}, copy.track());
					announce(project, context, copy, "added");
					copies.push_back(copy);
				}
			return {{"events", idsOf(copies)}};
		}

		// A command's start and end in project samples, by default the locators.
		std::pair<time::Samples, time::Samples> startAndEnd(CommandContext& context)
		{
			const Json& parameters = context.parameters();
			Json locators;
			if (!parameters.contains("start") || !parameters.contains("end"))
				locators = context.call("transport.get");
			return {parameters.contains("start") ? parameters.at("start").get<time::Samples>()
												 : locators.at("left").get<time::Samples>(),
				parameters.contains("end") ? parameters.at("end").get<time::Samples>()
										   : locators.at("right").get<time::Samples>()};
		}

		Json fillLoop(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const std::vector<Item> items = itemsFrom(project, parameters, false);
			const bool shared = parameters.value("shared", false);
			const auto [left, right] = startAndEnd(context);
			require(left >= 0 && right > left, "the range must start at or after 0 and end after its start");
			const bool musical = allMusical(items);
			const auto [start, end] = block(project, items, musical);
			const int64_t length = end - start;
			require(length > 0, g_tooShort);
			const int64_t from = inUnit(project, {time::TimeBase::Linear, left}, musical);
			const int64_t to = inUnit(project, {time::TimeBase::Linear, right}, musical);
			require((to - from) / length <= 1000, "the range holds more than 1000 copies");
			std::vector<Item> copies;
			// Copies on the grid of the block's length, from the first that starts in the range to the right locator.
			for (auto k =
					 static_cast<int64_t>(std::ceil(static_cast<double>(from - start) / static_cast<double>(length)));
				start + (k * length) < to; ++k)
			{
				if (k == 0)
					continue;
				for (const Item& item : items)
				{
					if (inUnit(project, item.position(), musical) + (k * length) >= to)
						continue;
					const Item copy = copyOf(project, item, shared, context);
					place(project, copy, {k * length, musical}, copy.track());
					if (inUnit(project, endOf(project, copy), musical) > to)
						setEnd(project, copy, {time::TimeBase::Linear, right}, false);
					announce(project, context, copy, "added");
					copies.push_back(copy);
				}
			}
			return {{"events", idsOf(copies)}};
		}

		Json resizeEvents(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const std::vector<Item> items = itemsFrom(project, parameters, true);
			const Shift shift = shiftFrom(parameters);
			const bool start = parameters.at("edge").get<std::string>() == "start";
			const bool moveContents = parameters.value("mode", std::string("normal")) == "moveContents";
			// Normal sizing of audio events cuts the audio at the edge: there it can snap to a zero crossing.
			const bool zero = parameters.value("zeroCrossing", false) && !moveContents;
			for (const Item& item : items)
			{
				time::Position to = shifted(project, start ? item.position() : endOf(project, item), shift, false);
				if (zero && item.audio != nullptr)
					to = {time::TimeBase::Linear, zeroCrossing(project, *item.audio, samples(project, to))};
				if (start)
					setStart(project, item, to, moveContents);
				else
					setEnd(project, item, to, moveContents);
				announce(project, context, item, "changed");
			}
			return {{"events", idsOf(items)}};
		}

		Json splitEvents(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			auto positions = parameters.at("positions").get<std::vector<time::Samples>>();
			std::sort(positions.begin(), positions.end());
			positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
			require(!positions.empty(), "positions must not be empty");
			const bool splitNotes = parameters.value("splitNotes", false);
			const bool zero = parameters.value("zeroCrossing", false);
			// Without events, every unlocked event and part that a position cuts.
			std::vector<Item> items;
			if (parameters.contains("events"))
			{
				items = itemsFrom(project, parameters, true);
			}
			else
			{
				// An audio part's events split with it.
				for (auto& [id, event] : project.audioEvents)
					if (!event.lock && event.part == 0)
						items.push_back(audioItem(event));
				for (auto& [id, part] : project.audioParts)
					if (!part.lock)
						items.push_back(audioPartItem(part));
				for (auto& [id, part] : project.midiParts)
					if (!part.lock)
						items.push_back({nullptr, &part});
			}
			std::vector<Item> added;
			for (const Item& item : items)
			{
				Item current = item;
				bool split = false;
				for (const time::Samples position : positions)
				{
					// Only a position that cuts the event moves to a zero crossing, one that still cuts it.
					time::Samples at = position;
					if (zero && current.audio != nullptr)
					{
						const EventSpan span = eventSpan(project, *current.audio);
						if (position > span.start && position < span.end)
							at = zeroCrossing(project, *current.audio, position, true);
					}
					if (const Item right = splitAt(project, current, at, splitNotes, context))
					{
						announce(project, context, right, "added");
						added.push_back(right);
						current = right;
						split = true;
					}
				}
				if (split)
					announce(project, context, item, "changed");
			}
			return {{"events", idsOf(added)}};
		}

		// Glues audio events and parts of one track into an audio part from the first start to the last end: the first
		// of the parts, or a new one. The others' events go into it.
		Json glueIntoPart(Project& project, const std::vector<Item>& items, CommandContext& context)
		{
			for (const Item& item : items)
				require(
					item.audio == nullptr || item.audio->part == 0, "audio events in a part don't glue with others");
			const auto found =
				std::find_if(items.begin(), items.end(), [](const Item& item) { return item.audioPart != nullptr; });
			const bool added = found == items.end();
			AudioPart* part = nullptr;
			if (added)
			{
				const Track& track = project.trackById.at(items[0].track());
				AudioPart created;
				created.id = project.newId();
				created.track = track.id;
				created.name = track.name;
				part = &(project.audioParts[created.id] = created);
			}
			else
			{
				part = found->audioPart;
			}
			const time::TimeBase base = project.trackById.at(part->track).timeBase;
			const int64_t start = rebased(project, items[0].position(), base).value;
			int64_t end = start + 1;
			for (const Item& item : items)
				end = std::max(end, rebased(project, endOf(project, item), base).value);
			part->position = {base, start};
			part->length = end - start;
			for (const Item& item : items)
			{
				if (item.audio != nullptr)
				{
					item.audio->part = part->id;
					continue;
				}
				if (item.audioPart == part)
					continue;
				for (AudioEvent* event : eventsIn(project, item.audioPart->id))
					event->part = part->id;
				const ObjectId merged = item.id();
				project.audioParts.erase(merged);
				context.emit("audioPart.removed", {{"part", merged}});
			}
			context.emit(added ? "audioPart.added" : "audioPart.changed", {{"part", toJson(*part)}});
			for (const AudioEvent* event : eventsIn(project, part->id))
				context.emit("audioEvent.changed", {{"event", toJson(*event)}});
			return {{"event", part->id}};
		}

		// Whether audio events continue each other in a clip, in order, as after a split.
		bool continuous(const Project& project, const std::vector<Item>& items)
		{
			for (size_t i = 1; i < items.size(); ++i)
			{
				if (items[i - 1].audio == nullptr || items[i].audio == nullptr)
					return false;
				const AudioEvent& before = *items[i - 1].audio;
				const AudioEvent& after = *items[i].audio;
				if (after.clip != before.clip || after.part != before.part ||
					after.offset != before.offset + before.length ||
					std::abs(samples(project, after.position) - eventSpan(project, before).end) > 1)
					return false;
			}
			return true;
		}

		Json glueEvents(Project& project, CommandContext& context)
		{
			std::vector<Item> items = itemsFrom(project, context.parameters(), true);
			require(items.size() >= 2, "glue needs at least two events");
			for (const Item& item : items)
			{
				require(item.track() == items[0].track(), "only events on the same track glue");
				require((item.part != nullptr) == (items[0].part != nullptr),
					"audio events and MIDI parts don't glue together");
			}
			std::sort(items.begin(), items.end(), [&project](const Item& a, const Item& b)
				{ return samples(project, a.position()) < samples(project, b.position()); });
			const Item& first = items[0];
			if (first.part == nullptr && !continuous(project, items))
				return glueIntoPart(project, items, context); // reference manual p. 269
			if (first.audio != nullptr)
			{
				// The glued event plays at the loudest event's volume, and its volume curve is each event's own where
				// it plays, lowered by its volume's difference to the loudest: no difference is above 0 dB, so none
				// is cut at the curve's +6 dB.
				const bool curves = std::any_of(items.begin(), items.end(), [&first](const Item& item)
					{ return !item.audio->volumeCurve.empty() || item.audio->volume != first.audio->volume; });
				double loudest = g_minEventVolume;
				for (const Item& item : items)
					loudest = std::max(loudest, item.audio->volume);
				std::vector<VolumePoint> curve;
				for (const Item& item : items)
				{
					const AudioEvent& each = *item.audio;
					std::vector<VolumePoint> own = cutVolumeCurve(
						each.volumeCurve.empty() ? std::vector<VolumePoint>{{0, 0.0, 0.0}} : each.volumeCurve,
						each.offset, each.offset + each.length);
					for (VolumePoint& point : own)
						point.volume =
							std::clamp(point.volume + each.volume - loudest, g_minEventVolume, g_maxEventVolume);
					curve.insert(curve.end(), own.begin(), own.end());
				}
				// Events that continue each other in a clip become one event.
				for (size_t i = 1; i < items.size(); ++i)
				{
					first.audio->length += items[i].audio->length;
					first.audio->fadeOut = items[i].audio->fadeOut;
				}
				if (curves)
				{
					first.audio->volume = loudest;
					first.audio->volumeCurve = std::move(curve);
				}
			}
			else
			{
				MidiPart& part = *first.part;
				detach(project, part);
				const time::Ticks start = ticks(project, part.position);
				int64_t end = part.position.value + part.length;
				for (size_t i = 1; i < items.size(); ++i)
				{
					const MidiPart& other = *items[i].part;
					end = std::max(end, other.position.value + other.length);
					const time::Ticks offset = ticks(project, other.position) - start;
					for (const auto& [id, each] : other.notes)
					{
						Note note = each;
						note.id = project.newId();
						note.position += offset;
						part.notes[note.id] = note;
					}
					for (const auto& [id, each] : other.controllers)
					{
						Controller controller = each;
						controller.id = project.newId();
						controller.position += offset;
						part.controllers[controller.id] = std::move(controller);
					}
				}
				part.length = end - part.position.value;
			}
			for (size_t i = 1; i < items.size(); ++i)
				removeItem(project, items[i], first.id(), context);
			announce(project, context, first, "changed");
			return {{"event", first.id()}};
		}

		Json setEvents(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const std::vector<Item> items = itemsFrom(project, parameters, false);
			const bool recolor = parameters.contains("color");
			const std::string color = recolor && parameters.at("color").is_string()
				? parameters.at("color").get<std::string>()
				: std::string();
			require(color.empty() || isColor(color), "color must be #rrggbb");
			for (const Item& item : items)
			{
				item.mute() = parameters.value("mute", item.mute());
				item.lock() = parameters.value("lock", item.lock());
				item.lane() = parameters.value("lane", item.lane());
				if (recolor)
					item.color() = color;
				announce(project, context, item, "changed");
			}
			if (!color.empty())
				project.lastAppliedColor = color;
			return {{"events", idsOf(items)}};
		}

		Json removeEvents(Project& project, CommandContext& context)
		{
			for (const Item& item : itemsFrom(project, context.parameters(), true))
				removeItem(project, item, 0, context);
			return Json::object();
		}

		Json toRealCopy(Project& project, CommandContext& context)
		{
			const std::vector<Item> items = itemsFrom(project, context.parameters(), false);
			for (const Item& item : items)
			{
				// An audio event, or each of an audio part's, gets a clip of its own (reference manual p. 272), as
				// the pool's new version.
				std::vector<AudioEvent*> events;
				if (item.audio != nullptr)
					events.push_back(item.audio);
				else if (item.audioPart != nullptr)
					events = eventsIn(project, item.id());
				else
					detach(project, *item.part);
				for (AudioEvent* event : events)
				{
					Clip clip = project.clips.at(event->clip);
					clip.id = project.newId();
					project.clips[clip.id] = clip;
					context.emit("clip.added", {{"clip", toJson(clip)}});
					event->clip = clip.id;
				}
				announce(project, context, item, "changed");
			}
			return {{"events", idsOf(items)}};
		}

		// Events to Part (reference manual p. 250): audio events go into a new audio part on their track, from the
		// first start to the last end; events on several tracks make a part on each.
		Json eventsToPart(Project& project, CommandContext& context)
		{
			std::map<ObjectId, std::vector<Item>> byTrack;
			for (const Item& item : itemsFrom(project, context.parameters(), true))
			{
				require(item.audio != nullptr && item.audio->part == 0,
					"event " + std::to_string(item.id()) + " isn't an audio event outside a part");
				byTrack[item.track()].push_back(item);
			}
			Json parts = Json::array();
			for (auto& [track, events] : byTrack)
			{
				std::sort(events.begin(), events.end(), [&project](const Item& a, const Item& b)
					{ return samples(project, a.position()) < samples(project, b.position()); });
				parts.push_back(glueIntoPart(project, events, context).at("event"));
			}
			return {{"events", parts}};
		}

		// Dissolve Part (reference manual p. 250): an audio part's events become events of their own on its track, cut
		// at its edges so that they play as they did; those it hid entirely go.
		Json dissolveParts(Project& project, CommandContext& context)
		{
			Json events = Json::array();
			for (const Item& item : itemsFrom(project, context.parameters(), true))
			{
				require(item.audioPart != nullptr, "event " + std::to_string(item.id()) + " isn't an audio part");
				const time::Samples start = samples(project, item.position());
				const time::Samples end = samples(project, endOf(project, item));
				for (AudioEvent* event : eventsIn(project, item.id()))
				{
					const ObjectId original = event->id;
					Item kept = audioItem(*event);
					if (const Item right = splitAt(project, kept, start, false, context))
					{
						removeItem(project, kept, right.id(), context);
						kept = right;
					}
					if (const Item right = splitAt(project, kept, end, false, context))
						removeItem(project, right, 0, context);
					const EventSpan span = eventSpan(project, *kept.audio);
					if (span.end <= start || span.start >= end)
					{
						removeItem(project, kept, 0, context);
						continue;
					}
					kept.audio->part = 0;
					announce(project, context, kept, kept.id() == original ? "changed" : "added");
					events.push_back(kept.id());
				}
				const ObjectId part = item.id();
				project.audioParts.erase(part);
				context.emit("audioPart.removed", {{"part", part}});
			}
			return {{"events", events}};
		}

		// What the info line shows of an event or part (reference manual p. 72): where it is in project samples and
		// in ticks, whatever the track's time base.
		Json eventInfo(Project& project, CommandContext& context)
		{
			Json result = Json::array();
			for (const Item& item : itemsFrom(project, context.parameters(), false))
			{
				const time::Position end = endOf(project, item);
				const bool audio = item.audio != nullptr;
				const bool audioPart = item.audioPart != nullptr;
				const time::Samples start = samples(project, item.position());
				std::string kind = "midi";
				std::string name;
				if (audio)
				{
					kind = "audio";
					name = project.clips.at(item.audio->clip).name;
				}
				else if (audioPart)
				{
					kind = "audioPart";
					name = item.audioPart->name;
				}
				else
				{
					name = item.part->name;
				}
				Json info = {{"id", item.id()}, {"kind", kind}, {"track", item.track()}, {"name", name},
					{"start", start}, {"end", samples(project, end)}, {"length", samples(project, end) - start},
					{"startTicks", ticks(project, item.position())}, {"endTicks", ticks(project, end)},
					{"mute", item.mute()}, {"lock", item.locked()}, {"lane", item.lane()},
					{"shared", item.part != nullptr ? item.part->shared : ObjectId{0}}};
				result.push_back(std::move(info));
			}
			return {{"events", std::move(result)}};
		}

		// Snap Point to Cursor (reference manual p. 105): audio events snap at position, which must be inside them.
		Json setSnapPoints(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const std::vector<Item> items = itemsFrom(project, parameters, false);
			const time::Samples at = parameters.contains("position")
				? parameters.at("position").get<time::Samples>()
				: context.call("transport.get").at("position").get<time::Samples>();
			for (const Item& item : items)
			{
				require(item.audio != nullptr, "only audio events have snap points");
				const EventSpan span = eventSpan(project, *item.audio);
				require(
					at >= span.start && at < span.end, "the position isn't inside event " + std::to_string(item.id()));
				item.audio->snapPoint =
					item.audio->offset + std::llround(static_cast<double>(at - span.start) / span.ratio);
				announce(project, context, item, "changed");
			}
			return {{"events", idsOf(items)}};
		}

		// Shuffle (reference manual p. 106): the events, one after the other on a track, go to where position puts
		// them among the events that adjoin them, which close up and make room, so the row keeps its start and has
		// no gaps.
		Json shuffleEvents(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const std::vector<Item> moved = itemsFrom(project, parameters, true);
			const ObjectId track = moved[0].track();
			require(
				std::all_of(moved.begin(), moved.end(), [track](const Item& item) { return item.track() == track; }),
				"shuffled events must be on one track");
			const bool musical = project.trackById.at(track).timeBase == time::TimeBase::Musical;
			// The track's events by start.
			std::vector<Item> row;
			for (auto& [id, event] : project.audioEvents)
				if (event.track == track && event.part == 0)
					row.push_back(audioItem(event));
			for (auto& [id, part] : project.audioParts)
				if (part.track == track)
					row.push_back(audioPartItem(part));
			for (auto& [id, part] : project.midiParts)
				if (part.track == track)
					row.push_back({nullptr, &part});
			std::sort(row.begin(), row.end(), [&project, musical](const Item& a, const Item& b)
				{ return inUnit(project, a.position(), musical) < inUnit(project, b.position(), musical); });
			const auto isMoved = [&moved](const Item& item)
			{ return std::any_of(moved.begin(), moved.end(), [&item](const Item& m) { return m.id() == item.id(); }); };
			size_t first = row.size();
			size_t last = 0;
			for (size_t i = 0; i < row.size(); ++i)
				if (isMoved(row[i]))
				{
					first = std::min(first, i);
					last = i;
				}
			const auto adjoin = [&project](const Item& before, const Item& after)
			{
				return !before.locked() && !after.locked() &&
					std::abs(samples(project, endOf(project, before)) - samples(project, after.position())) <= 1;
			};
			for (size_t i = first; i < last; ++i)
				require(isMoved(row[i + 1]) && adjoin(row[i], row[i + 1]), "shuffled events must adjoin each other");
			// The row of adjoining events around them.
			while (first > 0 && adjoin(row[first - 1], row[first]))
				--first;
			while (last + 1 < row.size() && adjoin(row[last], row[last + 1]))
				++last;
			const int64_t rowStart = inUnit(project, row[first].position(), musical);
			const int64_t at =
				inUnit(project, {time::TimeBase::Linear, parameters.at("position").get<time::Samples>()}, musical);
			const auto length = [&project, musical](const Item& item)
			{ return inUnit(project, endOf(project, item), musical) - inUnit(project, item.position(), musical); };
			// The others close up; the moved events go after each one whose middle they start past. An empty slot
			// stands for them.
			std::vector<Item> order;
			std::vector<Item> block;
			int64_t x = rowStart;
			bool placed = false;
			for (size_t i = first; i <= last; ++i)
			{
				if (isMoved(row[i]))
				{
					block.push_back(row[i]);
					continue;
				}
				if (!placed && at < x + (length(row[i]) / 2))
				{
					order.emplace_back();
					placed = true;
				}
				order.push_back(row[i]);
				x += length(row[i]);
			}
			if (!placed)
				order.emplace_back();
			x = rowStart;
			for (const Item& slot : order)
				for (const Item& item : slot ? std::vector<Item>{slot} : block)
				{
					const int64_t delta = x - inUnit(project, item.position(), musical);
					x += length(item);
					if (delta == 0)
						continue;
					place(project, item, {delta, musical}, track);
					announce(project, context, item, "changed");
				}
			return {{"events", idsOf(moved)}};
		}

		// Range editing (#88, reference manual pp. 285-294): the range.* commands edit what lies between two positions
		// on some tracks, cutting the events and parts at its edges. Locked events stay as they are.

		struct Range
		{
			time::Samples start = 0;
			time::Samples end = 0;
			std::vector<ObjectId> tracks; // from top to bottom
			bool splitNotes = false;
		};

		// The range a command names: start and end (default the locators) on tracks (default every track).
		Range rangeFrom(const Project& project, CommandContext& context, bool needsLength)
		{
			const Json& parameters = context.parameters();
			Range range;
			std::tie(range.start, range.end) = startAndEnd(context);
			require(range.start >= 0 && range.end >= range.start,
				"the range must start at or after 0 and not end before its start");
			require(!needsLength || range.end > range.start, "the range is empty");
			range.tracks = project.trackOrder();
			if (parameters.contains("tracks"))
			{
				const auto named = parameters.at("tracks").get<std::set<ObjectId>>();
				for (const ObjectId id : named)
					require(project.trackById.count(id) != 0, "no track " + std::to_string(id));
				range.tracks.erase(std::remove_if(range.tracks.begin(), range.tracks.end(),
									   [&named](ObjectId id) { return named.count(id) == 0; }),
					range.tracks.end());
			}
			range.splitNotes = parameters.value("splitNotes", false);
			return range;
		}

		bool contains(const std::vector<ObjectId>& tracks, ObjectId track)
		{
			return std::find(tracks.begin(), tracks.end(), track) != tracks.end();
		}

		// The events and parts on the tracks: the unlocked ones, or with locked all.
		std::vector<Item> itemsOn(Project& project, const std::vector<ObjectId>& tracks, bool locked)
		{
			std::vector<Item> items;
			for (auto& [id, event] : project.audioEvents)
				if (contains(tracks, event.track) && (locked || !event.lock) && event.part == 0)
					items.push_back(audioItem(event));
			for (auto& [id, part] : project.audioParts)
				if (contains(tracks, part.track) && (locked || !part.lock))
					items.push_back(audioPartItem(part));
			for (auto& [id, part] : project.midiParts)
				if (contains(tracks, part.track) && (locked || !part.lock))
					items.push_back({nullptr, &part});
			return items;
		}

		// A position in project samples in a time base, to compare with positions there.
		int64_t inBase(const Project& project, time::Samples at, time::TimeBase base)
		{
			return rebased(project, {time::TimeBase::Linear, at}, base).value;
		}

		// Splits the events and parts on the tracks at a position; returns the pieces after it.
		std::vector<Item> splitTracks(Project& project, const std::vector<ObjectId>& tracks, time::Samples at,
			bool splitNotes, bool locked, CommandContext& context)
		{
			std::vector<Item> added;
			for (const Item& item : itemsOn(project, tracks, locked))
				if (const Item right = splitAt(project, item, at, splitNotes, context))
				{
					announce(project, context, item, "changed");
					announce(project, context, right, "added");
					added.push_back(right);
				}
			return added;
		}

		// Whether an event or part lies after a position it was split at: whether its middle lies at or after it. Not
		// by its start: a cut in a clip at another sample rate than the project's lands up to half a clip frame off,
		// and an event less than that past the position isn't cut at all.
		bool after(const Project& project, const Item& item, time::Samples at)
		{
			const time::TimeBase base = item.position().base;
			return item.position().value + endOf(project, item).value >= 2 * inBase(project, at, base);
		}

		// The events and parts on the tracks that lie within from and to, once both are split there.
		std::vector<Item> within(
			Project& project, const std::vector<ObjectId>& tracks, time::Samples from, time::Samples to, bool locked)
		{
			std::vector<Item> items;
			for (const Item& item : itemsOn(project, tracks, locked))
				if (after(project, item, from) && !after(project, item, to))
					items.push_back(item);
			return items;
		}

		// A stretch of time as each time base measures it: samples on linear tracks, ticks on musical ones, so that
		// what moves on a musical track moves exactly by beats.
		struct Gap
		{
			time::Samples samples = 0;
			time::Ticks ticks = 0;

			int64_t in(time::TimeBase base) const { return base == time::TimeBase::Musical ? ticks : samples; }
			Shift shift(time::TimeBase base, int64_t sign) const
			{
				return {sign * in(base), base == time::TimeBase::Musical};
			}
		};

		Gap gapOf(const Project& project, time::Samples from, time::Samples to)
		{
			return {to - from,
				ticks(project, {time::TimeBase::Linear, to}) - ticks(project, {time::TimeBase::Linear, from})};
		}

		// Moves the unlocked events and parts on the tracks that lie after a position they were split at by a gap, with
		// sign 1 later, with -1 earlier.
		void moveFrom(Project& project, const std::vector<ObjectId>& tracks, time::Samples at, const Gap& gap,
			int64_t sign, CommandContext& context)
		{
			for (const Item& item : itemsOn(project, tracks, false))
				if (after(project, item, at))
				{
					place(project, item, gap.shift(item.position().base, sign), item.track());
					announce(project, context, item, "changed");
				}
		}

		// Automation in ranges. A lane's positions are in its track's time base.

		// A lane's curve as it plays (engine/audio/automation.h): sorted by position, volume as a gain.
		std::vector<RenderAutomationPoint> curveOf(const AutomationLane& lane)
		{
			std::vector<RenderAutomationPoint> points;
			points.reserve(lane.points.size());
			for (const auto& [id, point] : lane.points)
				points.push_back({point.position.value,
					lane.target.type == AutomationTargetType::Volume ? volumeGain(point.value) : point.value,
					point.shape, point.curve});
			std::stable_sort(points.begin(), points.end(),
				[](const RenderAutomationPoint& a, const RenderAutomationPoint& b) { return a.position < b.position; });
			return points;
		}

		// The curve's value at a position: the one it arrives with from the left, or the one it goes on with.
		double valueAt(const AutomationLane& lane, int64_t at, bool fromLeft)
		{
			std::vector<RenderAutomationPoint> points = curveOf(lane);
			if (fromLeft)
			{
				const auto next = std::lower_bound(points.begin(), points.end(), at,
					[](const RenderAutomationPoint& point, int64_t position) { return point.position < position; });
				points.erase(next == points.end() ? next : std::next(next), points.end());
			}
			const double value = automationValue(points, at);
			if (lane.target.type != AutomationTargetType::Volume)
				return value;
			return value <= 0.0 ? g_minChannelVolume : std::max(g_minChannelVolume, 20.0 * std::log10(value));
		}

		// The point that shapes the curve at a position: the last one before it, or with atToo at it.
		std::optional<AutomationPoint> shapingPoint(const AutomationLane& lane, int64_t at, bool atToo)
		{
			std::optional<AutomationPoint> found;
			for (const auto& [id, point] : lane.points)
				if ((point.position.value < at || (atToo && point.position.value == at)) &&
					(!found || point.position.value >= found->position.value))
					found = point;
			return found;
		}

		// A point without an id, shaped like shape, or else a step.
		AutomationPoint makePoint(time::TimeBase base, int64_t at, double value,
			const std::optional<AutomationPoint>& shape, bool terminator = false)
		{
			AutomationPoint point;
			point.position = {base, at};
			point.value = value;
			point.shape = shape ? shape->shape : AutomationShape::Step;
			point.curve = shape ? shape->curve : 0.0;
			point.terminator = terminator;
			return point;
		}

		void addPoint(Project& project, AutomationLane& lane, AutomationPoint point)
		{
			point.id = project.newId();
			lane.points[point.id] = point;
		}

		// Whether a lane has a point from from up to before to.
		bool anyPoint(const AutomationLane& lane, int64_t from, int64_t to)
		{
			return std::any_of(lane.points.begin(), lane.points.end(), [from, to](const auto& entry)
				{ return entry.second.position.value >= from && entry.second.position.value < to; });
		}

		// Whether a lane has no curve at a position, a gap (reference manual p. 881): from a terminator up to the next
		// point, and with virgin territory before the first and after the last point. fromLeft: where the curve
		// arrives there from, else where it goes on from there. An edge of an edit in a gap gets no point with a value.
		bool inGap(const Project& project, const AutomationLane& lane, int64_t at, bool fromLeft)
		{
			const std::optional<AutomationPoint> point = shapingPoint(lane, at, !fromLeft);
			if (!point)
				return project.virginTerritory;
			return point->terminator ||
				(project.virginTerritory &&
					!anyPoint(lane, fromLeft ? at : at + 1, std::numeric_limits<int64_t>::max()));
		}

		// Takes the automation between from and to out of a lane and keeps the curve outside: the value it arrives
		// at from with holds up to to, where the curve goes on. With virgin territory the range becomes a gap instead
		// (reference manual p. 881): the point at from is a terminator. Closing the gap moves the points after it back
		// instead. Points at to make way for one that has their value and shape. Edges in gaps stay in them.
		// ponytail: a curve shaped point at an edge keeps its bend over the shorter rest of its segment.
		void cutAutomation(
			Project& project, AutomationLane& lane, int64_t from, int64_t to, bool close, CommandContext& context)
		{
			if (!anyPoint(lane, from, close ? std::numeric_limits<int64_t>::max() : to))
				return;
			const time::TimeBase base = project.trackById.at(lane.track).timeBase;
			const double left = valueAt(lane, from, true);
			const double right = valueAt(lane, to, false);
			const std::optional<AutomationPoint> shape = shapingPoint(lane, to, true);
			const bool leftGap = inGap(project, lane, from, true);
			const bool rightGap = inGap(project, lane, to, false);
			for (auto point = lane.points.begin(); point != lane.points.end();)
			{
				int64_t& at = point->second.position.value;
				if (at >= from && at <= to)
				{
					point = lane.points.erase(point);
					continue;
				}
				if (close && at > to)
					at -= to - from;
				++point;
			}
			if (!leftGap)
				addPoint(project, lane,
					makePoint(base, from, left, std::nullopt, close ? rightGap : project.virginTerritory));
			if (!rightGap)
				addPoint(project, lane, makePoint(base, close ? from : to, right, shape));
			else if (!leftGap && !close && !project.virginTerritory)
				addPoint(project, lane, makePoint(base, to, left, std::nullopt, true)); // the held value ends there
			context.emit("automationLane.changed", {{"lane", toJson(lane)}});
		}

		// Makes room of a length at a position in a lane: the value the curve arrives there with holds over it, then
		// the curve goes on. In a gap the gap goes on over it.
		void openAutomation(Project& project, AutomationLane& lane, int64_t at, int64_t length, CommandContext& context)
		{
			if (!anyPoint(lane, at, std::numeric_limits<int64_t>::max()))
				return;
			const time::TimeBase base = project.trackById.at(lane.track).timeBase;
			const double left = valueAt(lane, at, true);
			const double right = valueAt(lane, at, false);
			const std::optional<AutomationPoint> shape = shapingPoint(lane, at, false);
			const bool pointThere = anyPoint(lane, at, at + 1);
			const bool leftGap = inGap(project, lane, at, true);
			const bool rightGap = inGap(project, lane, at, false);
			for (auto& [id, point] : lane.points)
				if (point.position.value >= at)
					point.position.value += length;
			if (!leftGap)
				addPoint(project, lane, makePoint(base, at, left, std::nullopt));
			if (!pointThere && !rightGap)
				addPoint(project, lane, makePoint(base, at + length, right, shape));
			context.emit("automationLane.changed", {{"lane", toJson(lane)}});
		}

		// The automation between from and to in a lane, as the clipboard keeps it: the points there, with the curve's
		// values at both edges, and its gaps: edges in gaps are terminators, and with virgin territory the lane's last
		// point is one. The points' ids only keep their order.
		AutomationLane automationWithin(const Project& project, const AutomationLane& lane, int64_t from, int64_t to)
		{
			const time::TimeBase base = project.trackById.at(lane.track).timeBase;
			const std::optional<AutomationPoint> last = shapingPoint(lane, std::numeric_limits<int64_t>::max(), true);
			std::vector<AutomationPoint> points;
			for (const auto& [id, point] : lane.points)
				if (point.position.value >= from && point.position.value < to)
				{
					points.push_back(point);
					points.back().terminator |= project.virginTerritory && last && last->id == id;
				}
			std::stable_sort(points.begin(), points.end(),
				[](const AutomationPoint& a, const AutomationPoint& b) { return a.position.value < b.position.value; });
			if (!anyPoint(lane, from, from + 1))
				points.insert(points.begin(),
					makePoint(base, from, valueAt(lane, from, false), shapingPoint(lane, from, false),
						inGap(project, lane, from, false)));
			points.push_back(
				makePoint(base, to, valueAt(lane, to, true), std::nullopt, inGap(project, lane, to, true)));
			AutomationLane copy = lane;
			copy.points.clear();
			ObjectId id = 0;
			for (AutomationPoint& point : points)
			{
				point.id = ++id;
				copy.points[id] = point;
			}
			return copy;
		}

		// Pastes copied automation into a lane, moved by shift: it replaces the curve from its first to its last
		// point, and the curve outside stays, with its gaps.
		void pasteAutomation(Project& project, AutomationLane& lane, const AutomationLane& copied, const Gap& shift,
			CommandContext& context)
		{
			const time::TimeBase base = project.trackById.at(lane.track).timeBase;
			std::vector<AutomationPoint> points;
			for (const auto& [id, each] : copied.points)
			{
				AutomationPoint point = each;
				point.position =
					rebased(project, shifted(project, each.position, shift.shift(each.position.base, 1), true), base);
				points.push_back(point);
			}
			if (points.empty())
				return;
			const auto byPosition = [](const AutomationPoint& a, const AutomationPoint& b)
			{ return a.position.value < b.position.value; };
			const int64_t from = std::min_element(points.begin(), points.end(), byPosition)->position.value;
			const int64_t to = std::max_element(points.begin(), points.end(), byPosition)->position.value;
			const bool had = !lane.points.empty();
			const double left = had ? valueAt(lane, from, true) : 0.0;
			const double right = had ? valueAt(lane, to, false) : 0.0;
			const std::optional<AutomationPoint> shape = shapingPoint(lane, to, true);
			const bool leftGap = had && inGap(project, lane, from, true);
			const bool rightGap = had && inGap(project, lane, to, false);
			for (auto point = lane.points.begin(); point != lane.points.end();)
				point = point->second.position.value >= from && point->second.position.value <= to
					? lane.points.erase(point)
					: std::next(point);
			if (had && !leftGap)
				addPoint(project, lane, makePoint(base, from, left, std::nullopt));
			for (const AutomationPoint& point : points)
				addPoint(project, lane, point);
			if (had)
				addPoint(project, lane, makePoint(base, to, right, shape, rightGap));
			context.emit("automationLane.changed", {{"lane", toJson(lane)}});
		}

		// What a range holds, for the clipboard. The cuts at its edges are made in a copy of the project.
		RangeClipboard copyRange(const Project& project, const Range& range)
		{
			Project scratch = project;
			const Json none = Json::object();
			CommandContext quiet(none); // what splitting the copy announces goes nowhere
			splitTracks(scratch, range.tracks, range.start, range.splitNotes, true, quiet);
			splitTracks(scratch, range.tracks, range.end, range.splitNotes, true, quiet);
			RangeClipboard clipboard;
			clipboard.start = range.start;
			clipboard.end = range.end;
			clipboard.startTicks = ticks(project, {time::TimeBase::Linear, range.start});
			clipboard.endTicks = ticks(project, {time::TimeBase::Linear, range.end});
			clipboard.tracks = range.tracks;
			for (const Item& item : within(scratch, range.tracks, range.start, range.end, true))
			{
				if (item.audio != nullptr)
				{
					clipboard.audioEvents.push_back(*item.audio);
				}
				else if (item.audioPart != nullptr)
				{
					clipboard.audioParts.push_back(*item.audioPart);
					for (const AudioEvent* event : eventsIn(scratch, item.id()))
						clipboard.audioEvents.push_back(*event);
				}
				else
				{
					clipboard.midiParts.push_back(*item.part);
				}
			}
			for (const auto& [id, lane] : project.automation)
				if (contains(range.tracks, lane.track) && !lane.points.empty())
				{
					const time::TimeBase base = project.trackById.at(lane.track).timeBase;
					clipboard.automation.push_back(automationWithin(
						project, lane, inBase(project, range.start, base), inBase(project, range.end, base)));
				}
			return clipboard;
		}

		Json clipboardInfo(const RangeClipboard& clipboard)
		{
			const auto inParts = static_cast<size_t>(std::count_if(clipboard.audioEvents.begin(),
				clipboard.audioEvents.end(), [](const AudioEvent& event) { return event.part != 0; }));
			return {
				{"events",
					clipboard.audioEvents.size() - inParts + clipboard.audioParts.size() + clipboard.midiParts.size()},
				{"lanes", clipboard.automation.size()}};
		}

		// Deletes what lies in a range; close (Delete Time) moves what comes after it back by its length.
		void deleteRange(Project& project, const Range& range, bool close, CommandContext& context)
		{
			splitTracks(project, range.tracks, range.start, range.splitNotes, false, context);
			splitTracks(project, range.tracks, range.end, range.splitNotes, false, context);
			for (const Item& item : within(project, range.tracks, range.start, range.end, false))
				removeItem(project, item, 0, context);
			if (close)
				moveFrom(project, range.tracks, range.end, gapOf(project, range.start, range.end), -1, context);
			for (auto& [id, lane] : project.automation)
				if (contains(range.tracks, lane.track))
				{
					const time::TimeBase base = project.trackById.at(lane.track).timeBase;
					cutAutomation(project, lane, inBase(project, range.start, base), inBase(project, range.end, base),
						close, context);
				}
		}

		// Makes room of a gap at a position on the tracks: what starts there or later moves later, cut there first.
		void insertSpace(Project& project, const std::vector<ObjectId>& tracks, time::Samples at, const Gap& gap,
			bool splitNotes, CommandContext& context)
		{
			splitTracks(project, tracks, at, splitNotes, false, context);
			moveFrom(project, tracks, at, gap, 1, context);
			for (auto& [id, lane] : project.automation)
				if (contains(tracks, lane.track))
				{
					const time::TimeBase base = project.trackById.at(lane.track).timeBase;
					openAutomation(project, lane, inBase(project, at, base), gap.in(base), context);
				}
		}

		Json splitRange(Project& project, CommandContext& context)
		{
			const Range range = rangeFrom(project, context, false);
			std::vector<Item> added = splitTracks(project, range.tracks, range.start, range.splitNotes, false, context);
			if (range.end > range.start)
			{
				const std::vector<Item> more =
					splitTracks(project, range.tracks, range.end, range.splitNotes, false, context);
				added.insert(added.end(), more.begin(), more.end());
			}
			return {{"events", idsOf(added)}};
		}

		// Lanes and comping (#99, reference manual pp. 225-228).

		// Brings a take to the front: an audio event plays where it overlaps others on its track, as do an audio part's
		// events; a MIDI part is unmuted, and the MIDI parts on its track's other lanes that it overlaps are muted.
		void toFront(Project& project, const Item& item, CommandContext& context)
		{
			if (item.audio != nullptr)
			{
				item.audio->front = project.newId();
			}
			else if (item.audioPart != nullptr)
			{
				for (AudioEvent* event : eventsIn(project, item.audioPart->id))
					event->front = project.newId();
			}
			else
			{
				MidiPart& part = *item.part;
				part.mute = false;
				const time::Samples start = samples(project, part.position);
				const time::Samples end = samples(project, endOf(project, item));
				for (auto& [id, other] : project.midiParts)
				{
					const Item overlapping{nullptr, &other};
					if (other.track != part.track || other.lane == part.lane || other.mute ||
						samples(project, other.position) >= end ||
						samples(project, endOf(project, overlapping)) <= start)
						continue;
					other.mute = true;
					announce(project, context, overlapping, "changed");
				}
			}
			announce(project, context, item, "changed");
		}

		Json bringToFront(Project& project, CommandContext& context)
		{
			const std::vector<Item> items = itemsFrom(project, context.parameters(), false);
			for (const Item& item : items)
				toFront(project, item, context);
			return {{"events", idsOf(items)}};
		}

		// The comp tool's drag on a lane (p. 228): every take on the track splits at the range's edges, and the takes
		// of the lane within it come to the front.
		Json comp(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const auto track = parameters.at("track").get<ObjectId>();
			const auto lane = parameters.at("lane").get<int>();
			const auto start = parameters.at("start").get<time::Samples>();
			const auto end = parameters.at("end").get<time::Samples>();
			require(project.trackById.count(track) != 0, "no track " + std::to_string(track));
			require(start >= 0 && end > start, "the range is empty");
			for (const time::Samples at : {start, end})
				splitTracks(project, {track}, at, false, false, context);
			// The splits leave locked takes whole: those are taken only when they lie wholly in the range.
			std::vector<Item> taken;
			for (const Item& item : within(project, {track}, start, end, true))
				if (item.lane() == lane &&
					(!item.locked() ||
						(samples(project, item.position()) >= start && samples(project, endOf(project, item)) <= end)))
					taken.push_back(item);
			require(!taken.empty(), "lane " + std::to_string(lane) + " has nothing from start to end");
			for (const Item& item : taken)
				toFront(project, item, context);
			return {{"events", idsOf(taken)}};
		}

		// Crop (reference manual p. 293): what crosses an edge loses what lies outside.
		Json cropRange(Project& project, CommandContext& context)
		{
			const Range range = rangeFrom(project, context, true);
			std::vector<Item> cropped;
			for (const Item& item : itemsOn(project, range.tracks, false))
			{
				const time::TimeBase base = item.position().base;
				const int64_t from = inBase(project, range.start, base);
				const int64_t to = inBase(project, range.end, base);
				const int64_t start = item.position().value;
				const int64_t end = endOf(project, item).value;
				if (end <= from || start >= to || (start >= from && end <= to))
					continue; // outside the range or inside it: unaffected
				const ObjectId original = item.id();
				Item kept = item;
				if (start < from)
				{
					if (const Item right = splitAt(project, kept, range.start, range.splitNotes, context))
					{
						removeItem(project, kept, right.id(), context);
						kept = right;
					}
				}
				if (end > to)
				{
					if (const Item right = splitAt(project, kept, range.end, range.splitNotes, context))
						removeItem(project, right, 0, context);
				}
				announce(project, context, kept, kept.id() == original ? "changed" : "added");
				cropped.push_back(kept);
			}
			return {{"events", idsOf(cropped)}};
		}

		Json insertSilence(Project& project, CommandContext& context)
		{
			const Range range = rangeFrom(project, context, true);
			insertSpace(
				project, range.tracks, range.start, gapOf(project, range.start, range.end), range.splitNotes, context);
			return Json::object();
		}

		Json pasteRange(Project& project, CommandContext& context, const std::optional<RangeClipboard>& stored)
		{
			if (!stored)
				throw CommandError("nothing to paste: copy or cut a range first");
			const RangeClipboard& clipboard = *stored;
			const Json& parameters = context.parameters();
			const time::Samples at = parameters.contains("position")
				? parameters.at("position").get<time::Samples>()
				: context.call("transport.get").at("position").get<time::Samples>();
			require(at >= 0, "position must not be before the project start");
			// Onto the tracks it came from, or from track on down.
			std::vector<ObjectId> tracks = clipboard.tracks;
			if (parameters.contains("track"))
			{
				const auto first = parameters.at("track").get<ObjectId>();
				const std::vector<ObjectId> order = project.trackOrder();
				const auto found = std::find(order.begin(), order.end(), first);
				require(found != order.end(), "no track " + std::to_string(first));
				require(static_cast<size_t>(order.end() - found) >= tracks.size(),
					"there aren't enough tracks from track " + std::to_string(first) + " down for the " +
						std::to_string(tracks.size()) + " copied tracks");
				tracks.assign(found, found + static_cast<std::ptrdiff_t>(tracks.size()));
			}
			else
			{
				for (const ObjectId id : tracks)
					require(project.trackById.count(id) != 0,
						"track " + std::to_string(id) + " that the range was copied from is gone: paste onto a track");
			}
			const auto onto = [&clipboard, &tracks](ObjectId source)
			{
				const auto index =
					std::find(clipboard.tracks.begin(), clipboard.tracks.end(), source) - clipboard.tracks.begin();
				return tracks.at(static_cast<size_t>(index));
			};
			if (parameters.value("time", false))
				insertSpace(project, tracks, at,
					{clipboard.end - clipboard.start, clipboard.endTicks - clipboard.startTicks}, false, context);
			const Gap shift{at - clipboard.start, ticks(project, {time::TimeBase::Linear, at}) - clipboard.startTicks};
			std::vector<Item> added;
			const auto paste = [&](const Item& item)
			{
				const ObjectId track = onto(item.track());
				checkTrack(project, item, track);
				place(project, item, shift.shift(item.position().base, 1), track);
				announce(project, context, item, "added");
				added.push_back(item);
			};
			// Audio parts, then their events, which they then take along to where they land.
			std::map<ObjectId, ObjectId> parts;
			std::vector<AudioPart*> pastedParts;
			for (const AudioPart& copied : clipboard.audioParts)
			{
				AudioPart part = copied;
				part.id = project.newId();
				part.lock = false;
				parts[copied.id] = part.id;
				pastedParts.push_back(&(project.audioParts[part.id] = part));
			}
			for (const AudioEvent& copied : clipboard.audioEvents)
			{
				require(project.clips.count(copied.clip) != 0, "the clip of a copied audio event is gone");
				AudioEvent event = copied;
				event.id = project.newId();
				event.front = 0;
				event.lock = false;
				event.crossfade = Crossfade();
				const auto part = parts.find(copied.part);
				event.part = part == parts.end() ? 0 : part->second;
				AudioEvent& pasted = project.audioEvents[event.id] = event;
				if (pasted.part == 0)
					paste(audioItem(pasted));
			}
			for (AudioPart* part : pastedParts)
				paste(audioPartItem(*part));
			for (const MidiPart& copied : clipboard.midiParts)
			{
				MidiPart part = copied;
				part.id = project.newId();
				part.lock = false;
				part.shared = 0;
				renumber(project, part);
				const ObjectId id = part.id;
				paste({nullptr, &(project.midiParts[id] = std::move(part))});
			}
			// ponytail: automation lands only on lanes of the same parameter that the tracks already have.
			for (const AutomationLane& copied : clipboard.automation)
			{
				const ObjectId track = onto(copied.track);
				for (auto& [id, lane] : project.automation)
					if (lane.track == track && lane.target == copied.target)
						pasteAutomation(project, lane, copied, shift, context);
			}
			return {{"events", idsOf(added)}};
		}

		// Schemas.

		Json integer()
		{
			return {{"type", "integer"}};
		}

		Json boolean()
		{
			return {{"type", "boolean"}};
		}

		Json schema(Json properties, const std::vector<std::string>& required)
		{
			return {{"type", "object"}, {"properties", std::move(properties)}, {"required", required},
				{"additionalProperties", false}};
		}

		Json events()
		{
			return {{"type", "array"}, {"items", integer()}};
		}

		Json eventsResult()
		{
			return schema({{"events", {{"type", "array"}, {"items", integer()}}}}, {"events"});
		}
	}

	void syncSharedParts(const std::map<ObjectId, MidiPart>& before, Project& project, CommandContext& context)
	{
		// The first part of each group whose notes or controllers changed is what the group now holds.
		std::map<ObjectId, const MidiPart*> sources;
		std::map<ObjectId, int> members;
		for (const auto& [id, part] : project.midiParts)
		{
			if (part.shared == 0)
				continue;
			++members[part.shared];
			const auto old = before.find(id);
			if (old != before.end() && old->second.shared == part.shared && sources.count(part.shared) == 0 &&
				(old->second.notes != part.notes || old->second.controllers != part.controllers))
				sources[part.shared] = &part;
		}
		for (auto& [id, part] : project.midiParts)
		{
			if (part.shared == 0)
				continue;
			bool changed = false;
			if (members[part.shared] < 2)
			{
				part.shared = 0; // the last of its copies is a part like any other
				changed = true;
			}
			else if (const auto source = sources.find(part.shared); source != sources.end() &&
				source->second != &part &&
				(source->second->notes != part.notes || source->second->controllers != part.controllers))
			{
				part.notes = source->second->notes;
				part.controllers = source->second->controllers;
				changed = true;
			}
			if (changed)
				context.emit("midiPart.changed", {{"part", toJson(part)}});
		}
	}

	std::map<ObjectId, MidiPart> sharedPartsOf(const std::map<ObjectId, MidiPart>& parts)
	{
		std::map<ObjectId, MidiPart> shared;
		for (const auto& [id, part] : parts)
			if (part.shared != 0)
				shared.emplace_hint(shared.end(), id, part);
		return shared;
	}

	void ProjectModel::registerEventEditCommands(Engine& engine)
	{
		const Json shift = {{"delta", integer()}, {"ticks", integer()}};
		Json moveParameters = shift;
		moveParameters["events"] = events();
		moveParameters["track"] = integer();
		addEdit(engine,
			{"event.move", 1,
				"Moves audio events and MIDI parts (events, by id) by delta project samples or by ticks, which are "
				"exact on musical tracks, keeping their positions relative to each other, and onto track if given, "
				"which must be of their type. Locked events don't move. Returns the events.",
				schema(moveParameters, {"events"}), eventsResult(), false},
			"Move", [this](Project& project, CommandContext& context)
			{ return moveEvents(project, context, m_automationFollowsEvents); });

		Json copyParameters = moveParameters;
		copyParameters["shared"] = boolean();
		addEdit(engine,
			{"event.copy", 1,
				"Copies audio events and MIDI parts, moved by delta project samples or by ticks and onto track as "
				"event.move moves them. Copies of audio events play the same clip; shared copies of MIDI parts share "
				"their notes and controllers with the original, so that editing one edits all. Returns the copies.",
				schema(copyParameters, {"events"}), eventsResult(), false},
			"Copy", copyEvents);

		addEdit(engine,
			{"event.duplicate", 1,
				"Duplicates audio events and MIDI parts count times (default 1, at most 1000; Edit > Repeat) "
				"one after the other, from the end of the last one, keeping their positions relative to each other; "
				"shared makes shared copies of MIDI parts. Returns the copies.",
				schema({{"events", events()}, {"count", integer()}, {"shared", boolean()}}, {"events"}), eventsResult(),
				false},
			"Duplicate", duplicateEvents);

		addEdit(engine,
			{"event.fillLoop", 1,
				"Fills the range from start to end (project samples, default the locators) with copies of audio "
				"events and MIDI parts, one after the other on the grid of their length, cutting the last one at "
				"end. shared makes shared copies of MIDI parts. Returns the copies.",
				schema({{"events", events()}, {"start", integer()}, {"end", integer()}, {"shared", boolean()}},
					{"events"}),
				eventsResult(), false},
			"Fill Loop", fillLoop);

		Json resizeParameters = shift;
		resizeParameters["events"] = events();
		resizeParameters["edge"] = {{"type", "string"}, {"enum", {"start", "end"}}};
		resizeParameters["mode"] = {{"type", "string"}, {"enum", {"normal", "moveContents"}}};
		resizeParameters["zeroCrossing"] = boolean();
		addEdit(engine,
			{"event.resize", 1,
				"Moves the start or the end (edge) of audio events and MIDI parts by delta project samples or by "
				"ticks, within the clip and at least a sample frame or a tick long. normal sizing (the default) "
				"reveals or hides content; moveContents moves the content with the edge. A MIDI part's notes and "
				"controllers that end up before its start are removed. With zeroCrossing, normal sizing moves audio "
				"events' edges to the nearest point within 50 ms where the audio crosses zero. Locked events don't "
				"change. Returns the events.",
				schema(resizeParameters, {"events", "edge"}), eventsResult(), false},
			"Resize", resizeEvents);

		addEdit(engine,
			{"event.split", 1,
				"Splits audio events and MIDI parts at positions (project samples): events, or without it every "
				"unlocked one that a position cuts. A MIDI part's notes stay in the part they start in, or with "
				"splitNotes are cut in two. With zeroCrossing, audio events split at the nearest point within 50 ms "
				"and inside the event where their audio crosses zero. Returns the new events after the splits.",
				schema({{"events", events()}, {"positions", {{"type", "array"}, {"items", integer()}}},
						   {"splitNotes", boolean()}, {"zeroCrossing", boolean()}},
					{"positions"}),
				eventsResult(), false},
			"Split", splitEvents);

		addEdit(engine,
			{"event.glue", 1,
				"Glues audio events and parts or MIDI parts of one track together. Audio events that continue each "
				"other in the same clip, as after a split, become one event; other audio events and audio parts "
				"become one audio part from the first start to the last end, the first of the parts or a new one, "
				"holding all their events. MIDI parts become the first of them, from the first start to the last "
				"end, with all their notes and controllers. Returns the event or part.",
				schema({{"events", events()}}, {"events"}), schema({{"event", integer()}}, {"event"}), false},
			"Glue", glueEvents);

		addEdit(engine,
			{"event.set", 1,
				"Mutes or unmutes, locks or unlocks and colors audio events and MIDI parts. Muted ones don't play; "
				"locked ones can't be moved, resized, split, glued or removed by the event commands. color (#rrggbb) "
				"is a color of their own; null makes them follow their track's color again. lane puts them on that "
				"lane of their track, from 0, as cycle recordings put their takes (recording.add). Returns the events.",
				schema({{"events", events()}, {"mute", boolean()}, {"lock", boolean()},
						   {"color", {{"type", {"string", "null"}}}},
						   {"lane", {{"type", "integer"}, {"minimum", 0}, {"maximum", 1000}}}},
					{"events"}),
				eventsResult(), false},
			"Change events", setEvents);

		addEdit(engine,
			{"event.toFront", 1,
				"Brings takes to the front (To Front, the comp tool's click): audio events, and the audio events of "
				"audio parts, play where they overlap other events on their track, as if added last (their front in "
				"project.get); MIDI parts are unmuted, and the MIDI parts on their track's other lanes that they "
				"overlap are muted. Returns the events.",
				schema({{"events", events()}}, {"events"}), eventsResult(), false},
			"To Front", bringToFront);

		addEdit(engine,
			{"event.comp", 1,
				"Comps a track's takes (the comp tool's drag on a lane): splits every unlocked audio event and part "
				"and MIDI part on track at start and end (project samples), then brings what lies on lane between "
				"them to the front, as event.toFront does. Returns the events brought to the front.",
				schema({{"track", integer()}, {"lane", {{"type", "integer"}, {"minimum", 0}, {"maximum", 1000}}},
						   {"start", integer()}, {"end", integer()}},
					{"track", "lane", "start", "end"}),
				eventsResult(), false},
			"Comp", comp);

		addEdit(engine,
			{"event.remove", 1, "Removes audio events and MIDI parts. Locked ones can't be removed.",
				schema({{"events", events()}}, {"events"}), {{"type", "object"}}, false},
			"Delete", removeEvents);

		addEdit(engine,
			{"event.get", 1,
				"Tells where audio events and parts are, as the info line shows them: for each its kind (audio, "
				"audioPart or midi), track, name (an audio event's clip's), start, end and length in project samples, "
				"start and end "

				"in ticks, mute, lock, lane and its shared copies' group (0: none).",
				schema({{"events", events()}}, {"events"}),
				schema({{"events", {{"type", "array"}, {"items", {{"type", "object"}}}}}}, {"events"}), true},
			{}, eventInfo);

		addEdit(engine,
			{"event.setSnapPoint", 1,
				"Sets audio events' snap point at position (project samples, default the project cursor), which "
				"must be inside each of them (Snap Point to Cursor): where the event snaps to the grid when it moves, "
				"and attracts other events. It is a point in the clip, so it stays with the audio when the event is "
				"resized or split. Returns the events.",
				schema({{"events", events()}, {"position", integer()}}, {"events"}), eventsResult(), false},
			"Snap Point to Cursor", setSnapPoints);

		addEdit(engine,
			{"event.shuffle", 1,
				"Shuffle (snap type shuffle): events that adjoin each other on one track change places with the "
				"events that adjoin them. They go where position (project samples, where the first of them would "
				"start) puts them: after each other event whose middle it is past. The others close up and make room, "
				"so the row keeps its start and has no gaps. Locked events end the row. Returns the events.",
				schema({{"events", events()}, {"position", integer()}}, {"events", "position"}), eventsResult(), false},
			"Shuffle", shuffleEvents);

		addEdit(engine,
			{"event.toPart", 1,
				"Events to Part (Audio menu): puts audio events (events, by id) into a new audio part on their track, "
				"from the first start to the last end; events on several tracks make a part on each. Events already "
				"in a part, and locked ones, are refused. Returns the parts in events.",
				schema({{"events", events()}}, {"events"}), eventsResult(), false},
			"Events to Part", eventsToPart);

		addEdit(engine,
			{"event.dissolvePart", 1,
				"Dissolve Part (Audio menu): removes audio parts (events, by id) and makes their audio events events "
				"of "
				"their own on the track, cut at the part's edges so that they play as they did; events the part hid "
				"entirely are removed. Locked parts are refused. Returns the events.",
				schema({{"events", events()}}, {"events"}), eventsResult(), false},
			"Dissolve Part", dissolveParts);

		addEdit(engine,
			{"event.toRealCopy", 1,

				"Converts shared copies to real copies: a MIDI part gets notes and controllers of its own, an audio "
				"event a clip of its own. Returns the events.",
				schema({{"events", events()}}, {"events"}), eventsResult(), false},
			"Convert to Real Copy", toRealCopy);

		registerRangeEditCommands(engine);
	}

	void ProjectModel::registerRangeEditCommands(Engine& engine)
	{
		const std::string range = "the range from start to end (project samples, default the locators) on tracks "
								  "(ids, default every track)";
		const Json rangeParameters = {
			{"start", integer()}, {"end", integer()}, {"tracks", events()}, {"splitNotes", boolean()}};
		Json withTime = rangeParameters;
		withTime["time"] = boolean();
		const Json copiedResult = schema({{"events", integer()}, {"lanes", integer()}}, {"events", "lanes"});

		addEdit(engine,
			{"range.split", 1,
				"Splits the audio events and MIDI parts on " + range +
					" at its edges; a range of no length splits at its start. Locked events stay whole; splitNotes "
					"cuts notes across an edge in two, as event.split does. Returns the new events after the splits.",
				schema(rangeParameters, {}), eventsResult(), false},
			"Split Range", splitRange);

		addEdit(engine,
			{"range.crop", 1,
				"Crops the audio events and MIDI parts that cross an edge of " + range +
					" to it, removing what lies outside; events entirely inside or outside it stay as they are, and "
					"so do locked ones. splitNotes as in range.split. Returns the cropped events.",
				schema(rangeParameters, {}), eventsResult(), false},
			"Crop Range", cropRange);

		addEdit(engine,
			{"range.delete", 1,
				"Deletes what lies in " + range +
					": the audio events and MIDI parts in it, cut at its edges (splitNotes as in range.split), and the "
					"automation points in it. The automation outside keeps its curve: the value it arrives at the "
					"start with holds up to the end. time (Delete Time) closes the gap: what comes after the range on "
					"its tracks moves back by its length, in ticks on musical tracks. Locked events stay.",
				schema(withTime, {}), {{"type", "object"}}, false},
			"Delete",
			[](Project& project, CommandContext& context)
			{
				deleteRange(
					project, rangeFrom(project, context, true), context.parameters().value("time", false), context);
				return Json::object();
			});

		addEdit(engine,
			{"range.insertSilence", 1,
				"Inserts silence as long as " + range +
					" at its start: what lies on its tracks from the start on, cut there, moves later by the range's "
					"length, in ticks on musical tracks, and the automation holds the value it arrives at the start "
					"with over the silence. Locked events stay.",
				schema(rangeParameters, {}), {{"type", "object"}}, false},
			"Insert Silence", insertSilence);

		addControl(engine,
			{"range.copy", 1,
				"Copies " + range +
					" to the range clipboard for range.paste: the audio events and MIDI parts in it, cut at its edges "
					"(splitNotes as in range.split), and the automation in it with the curve's values at the edges. "
					"Without start, end and tracks it is Global Copy: everything between the locators. Changes "
					"nothing in the project. Returns how many events and parts and automation lanes it copied.",
				schema(rangeParameters, {}), copiedResult, false},
			[this](CommandContext& context)
			{
				RangeClipboard clipboard = copyRange(m_project, rangeFrom(m_project, context, true));
				Json result = clipboardInfo(clipboard);
				m_clipboard = std::move(clipboard);
				return result;
			});

		addEdit(engine,
			{"range.cut", 1,
				"Copies " + range +
					" to the range clipboard as range.copy does, then deletes it as range.delete does; time (Cut "
					"Time) closes the gap. Returns how many events and parts and automation lanes it copied.",
				schema(withTime, {}), copiedResult, false},
			"Cut",
			[this](Project& project, CommandContext& context)
			{
				const Range cut = rangeFrom(project, context, true);
				RangeClipboard clipboard = copyRange(project, cut);
				deleteRange(project, cut, context.parameters().value("time", false), context);
				Json result = clipboardInfo(clipboard);
				m_clipboard = std::move(clipboard);
				return result;
			});

		addEdit(engine,
			{"range.paste", 1,
				"Pastes the range clipboard (range.copy, range.cut) at position (project samples, default the project "
				"cursor): onto the tracks it was copied from, or onto track and the tracks below it in the project's "
				"order, one for each copied track, which must take the copied events. Audio events and MIDI parts "
				"come as unlocked real copies; automation replaces the curve where it lands, on a lane of the same "
				"parameter. time (Paste Time) first makes room as long as the copied range, as range.insertSilence "
				"does. Returns the new events.",
				schema({{"position", integer()}, {"track", integer()}, {"time", boolean()}}, {}), eventsResult(),
				false},
			"Paste",
			[this](Project& project, CommandContext& context) { return pasteRange(project, context, m_clipboard); });
	}
}
