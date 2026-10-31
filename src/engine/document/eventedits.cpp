// SPDX-License-Identifier: GPL-3.0-or-later
// Editing parts and events in the project window (#85, reference manual pp. 245-277): the event.* commands, which
// take audio events and MIDI parts alike by id, and shared copies of MIDI parts. Like every command that changes the
// project, each is one undo step and changes nothing if it fails (ProjectModel::addEdit).
// docs/features/parts-and-events.md describes them.
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
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

		// An audio event or a MIDI part. Pointers into the project's maps stay valid while other objects are added.
		struct Item
		{
			AudioEvent* audio = nullptr;
			MidiPart* part = nullptr;

			explicit operator bool() const { return audio != nullptr || part != nullptr; }
			ObjectId id() const { return audio != nullptr ? audio->id : midi().id; }
			ObjectId track() const { return audio != nullptr ? audio->track : midi().track; }
			bool locked() const { return audio != nullptr ? audio->lock : midi().lock; }
			time::Position& position() const { return audio != nullptr ? audio->position : midi().position; }

		private:
			// The MIDI part of an item that isn't an audio event. Checked, so the static analyzer sees that one of the
			// two is always set.
			MidiPart& midi() const
			{
				if (part == nullptr)
					throw CommandError("an event is neither an audio event nor a MIDI part");
				return *part;
			}
		};

		Item findItem(Project& project, ObjectId id)
		{
			if (const auto audio = project.audioEvents.find(id); audio != project.audioEvents.end())
				return {&audio->second, nullptr};
			const auto part = project.midiParts.find(id);
			require(part != project.midiParts.end(), "no audio event or MIDI part " + std::to_string(id));
			return {nullptr, &part->second};
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

		void announce(CommandContext& context, const Item& item, const std::string& what)
		{
			if (item.audio != nullptr)
				context.emit("audioEvent." + what, {{"event", toJson(*item.audio)}});
			else
				context.emit("midiPart." + what, {{"part", toJson(*item.part)}});
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
			if (item.part != nullptr)
				return {item.part->position.base, item.part->position.value + item.part->length};
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
				require(type == TrackType::Audio, "audio events can only be on an audio track");
			else
				require(type == TrackType::Midi || type == TrackType::Instrument,
					"MIDI parts can only be on a MIDI or instrument track");
		}

		// Moves an item by shift onto a track, keeping its length; a MIDI part's length goes into the track's time
		// base.
		void place(Project& project, const Item& item, const Shift& shift, ObjectId track)
		{
			const time::TimeBase base = project.trackById.at(track).timeBase;
			const time::Position start = rebased(project, shifted(project, item.position(), shift, true), base);
			if (item.part != nullptr)
			{
				const time::Position end = rebased(project, shifted(project, endOf(project, item), shift, true), base);
				item.part->length = std::max<int64_t>(1, end.value - start.value);
				item.part->track = track;
			}
			else
			{
				item.audio->track = track;
			}
			item.position() = start;
		}

		// Gives a part's notes and controllers new ids.
		void renumber(Project& project, MidiPart& part)
		{
			std::map<ObjectId, Note> notes;
			for (auto& [id, note] : part.notes)
			{
				note.id = project.newId();
				notes[note.id] = note;
			}
			part.notes = std::move(notes);
			std::map<ObjectId, Controller> controllers;
			for (auto& [id, controller] : part.controllers)
			{
				controller.id = project.newId();
				controllers[controller.id] = std::move(controller);
			}
			part.controllers = std::move(controllers);
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
				copy.lock = false;
				copy.crossfade = Crossfade();
				return {&(project.audioEvents[copy.id] = copy), nullptr};
			}
			if (shared && item.part->shared == 0)
			{
				item.part->shared = project.newId();
				announce(context, item, "changed");
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

		// Resizing (reference manual p. 265): the start or the end goes to a position, within the clip and at least a
		// sample frame (or a tick) long. Normal sizing reveals or hides content; sizing moves contents moves the
		// content with the edge.
		void setStart(Project& project, const Item& item, const time::Position& start, bool moveContents)
		{
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
			if (item.audio != nullptr)
			{
				AudioEvent& event = *item.audio;
				const EventSpan span = eventSpan(project, event);
				const int64_t frames = std::llround(static_cast<double>(at - span.start) / span.ratio);
				if (at <= span.start || at >= span.end || frames <= 0 || frames >= event.length)
					return {};
				AudioEvent right = event;
				right.id = project.newId();
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
				announce(context, item, "changed");
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
				announce(context, copy, "added");
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
			std::vector<Item> copies;
			for (int64_t k = 1; k <= count; ++k)
				for (const Item& item : items)
				{
					const Item copy = copyOf(project, item, shared, context);
					place(project, copy, {k * (end - start), musical}, copy.track());
					announce(context, copy, "added");
					copies.push_back(copy);
				}
			return {{"events", idsOf(copies)}};
		}

		Json fillLoop(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const std::vector<Item> items = itemsFrom(project, parameters, false);
			const bool shared = parameters.value("shared", false);
			Json locators;
			if (!parameters.contains("start") || !parameters.contains("end"))
				locators = context.call("transport.get");
			const auto left = parameters.contains("start") ? parameters.at("start").get<time::Samples>()
														   : locators.at("left").get<time::Samples>();
			const auto right = parameters.contains("end") ? parameters.at("end").get<time::Samples>()
														  : locators.at("right").get<time::Samples>();
			require(left >= 0 && right > left, "the range must start at or after 0 and end after its start");
			const bool musical = allMusical(items);
			const auto [start, end] = block(project, items, musical);
			const int64_t length = end - start;
			require(length > 0, "the events are too short to fill the range with");
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
					announce(context, copy, "added");
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
				announce(context, item, "changed");
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
				for (auto& [id, event] : project.audioEvents)
					if (!event.lock)
						items.push_back({&event, nullptr});
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
						announce(context, right, "added");
						added.push_back(right);
						current = right;
						split = true;
					}
				}
				if (split)
					announce(context, item, "changed");
			}
			return {{"events", idsOf(added)}};
		}

		Json glueEvents(Project& project, CommandContext& context)
		{
			std::vector<Item> items = itemsFrom(project, context.parameters(), true);
			require(items.size() >= 2, "glue needs at least two events");
			for (const Item& item : items)
			{
				require(item.track() == items[0].track(), "only events on the same track glue");
				require((item.audio != nullptr) == (items[0].audio != nullptr),
					"audio events and MIDI parts don't glue together");
			}
			std::sort(items.begin(), items.end(), [&project](const Item& a, const Item& b)
				{ return samples(project, a.position()) < samples(project, b.position()); });
			const Item& first = items[0];
			if (first.audio != nullptr)
			{
				// ponytail: audio events glue only where one continues the other, until audio parts hold the rest.
				for (size_t i = 1; i < items.size(); ++i)
				{
					const AudioEvent& before = *items[i - 1].audio;
					const AudioEvent& after = *items[i].audio;
					require(after.clip == before.clip && after.offset == before.offset + before.length &&
							std::abs(samples(project, after.position) - eventSpan(project, before).end) <= 1,
						"audio events glue only where one continues the other in the same clip");
					first.audio->length += after.length;
					first.audio->fadeOut = after.fadeOut;
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
			announce(context, first, "changed");
			return {{"event", first.id()}};
		}

		Json setEvents(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const std::vector<Item> items = itemsFrom(project, parameters, false);
			for (const Item& item : items)
			{
				bool& mute = item.audio != nullptr ? item.audio->mute : item.part->mute;
				bool& lock = item.audio != nullptr ? item.audio->lock : item.part->lock;
				mute = parameters.value("mute", mute);
				lock = parameters.value("lock", lock);
				announce(context, item, "changed");
			}
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
				if (item.audio != nullptr)
				{
					// A clip of its own (reference manual p. 272), as the pool's new version.
					Clip clip = project.clips.at(item.audio->clip);
					clip.id = project.newId();
					project.clips[clip.id] = clip;
					context.emit("clip.added", {{"clip", toJson(clip)}});
					item.audio->clip = clip.id;
				}
				else
				{
					detach(project, *item.part);
				}
				announce(context, item, "changed");
			}
			return {{"events", idsOf(items)}};
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
				const time::Samples start = samples(project, item.position());
				Json info = {{"id", item.id()}, {"kind", audio ? "audio" : "midi"}, {"track", item.track()},
					{"name", audio ? project.clips.at(item.audio->clip).name : item.part->name}, {"start", start},
					{"end", samples(project, end)}, {"length", samples(project, end) - start},
					{"startTicks", ticks(project, item.position())}, {"endTicks", ticks(project, end)},
					{"mute", audio ? item.audio->mute : item.part->mute}, {"lock", item.locked()},
					{"shared", audio ? ObjectId{0} : item.part->shared}};
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
				announce(context, item, "changed");
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
				if (event.track == track)
					row.push_back({&event, nullptr});
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
					announce(context, item, "changed");
				}
			return {{"events", idsOf(moved)}};
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
				"Glues audio events or MIDI parts of one track into the first of them. Audio events glue where one "
				"continues the other in the same clip, as after a split; MIDI parts become one part from the first "
				"start to the last end, with all their notes and controllers. Returns the event.",
				schema({{"events", events()}}, {"events"}), schema({{"event", integer()}}, {"event"}), false},
			"Glue", glueEvents);

		addEdit(engine,
			{"event.set", 1,
				"Mutes or unmutes and locks or unlocks audio events and MIDI parts. Muted ones don't play; locked "
				"ones can't be moved, resized, split, glued or removed by the event commands. Returns the events.",
				schema({{"events", events()}, {"mute", boolean()}, {"lock", boolean()}}, {"events"}), eventsResult(),
				false},
			"Change events", setEvents);

		addEdit(engine,
			{"event.remove", 1, "Removes audio events and MIDI parts. Locked ones can't be removed.",
				schema({{"events", events()}}, {"events"}), {{"type", "object"}}, false},
			"Delete", removeEvents);

		addEdit(engine,
			{"event.get", 1,
				"Tells where audio events and MIDI parts are, as the info line shows them: for each its kind (audio or "
				"midi), track, name (an audio event's clip's), start, end and length in project samples, start and end "
				"in ticks, mute, lock and its shared copies' group (0: none).",
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
			{"event.toRealCopy", 1,
				"Converts shared copies to real copies: a MIDI part gets notes and controllers of its own, an audio "
				"event a clip of its own. Returns the events.",
				schema({{"events", events()}}, {"events"}), eventsResult(), false},
			"Convert to Real Copy", toRealCopy);
	}
}
