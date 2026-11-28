// SPDX-License-Identifier: GPL-3.0-or-later
// Changing the notes and controllers of a MIDI part (#27): note.set and controller.set, the key editor's split and
// glue tools (#1419): note.split and note.glue, its chord editing (#127): note.chord and note.voicing, and a track's
// note expression setup (#1513, #2120): noteExpression.set, and Convert to Note Expression (#1515):
// noteExpression.convert.
// Like every command that changes the project, each is one undo step and changes nothing if it fails
// (ProjectModel::add).
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/midi/chords.h"
#include "engine/midi/mpeinput.h"
#include "plugin/events.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <map>
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

		MidiPart& findPart(Project& project, const Json& parameters)
		{
			const auto id = parameters.at("part").get<ObjectId>();
			const auto found = project.midiParts.find(id);
			require(found != project.midiParts.end(), "no MIDI part " + std::to_string(id));
			return found->second;
		}

		// The item's value for key, from minimum to maximum, or current if the item doesn't have it.
		template <typename T> T integer(
			const Json& item, const char* key, T current, int64_t minimum, int64_t maximum, const std::string& name)
		{
			if (!item.contains(key))
				return current;
			const auto value = item.at(key).get<int64_t>();
			require(value >= minimum && value <= maximum,
				name + "." + key + " must be " + std::to_string(minimum) + " to " + std::to_string(maximum));
			return static_cast<T>(value);
		}

		time::Ticks position(const Json& item, time::Ticks current, const std::string& name)
		{
			const auto value = item.value("position", current);
			require(value >= 0, name + ".position must not be negative");
			return value;
		}

		// A value from 0 to 1 scaled to maximum, or current if the item doesn't have it.
		template <typename T>
		T scaled(const Json& item, const char* key, T current, double maximum, const std::string& name)
		{
			if (!item.contains(key))
				return current;
			const double value = item.at(key).get<double>();
			require(value >= 0.0 && value <= 1.0, name + "." + key + " must be 0 to 1");
			return static_cast<T>(std::llround(value * maximum));
		}

		// Finds the object an item changes; the part must hold it.
		template <typename Map> auto& changed(Map& objects, const Json& item, const std::string& name, const char* what)
		{
			const auto id = item.at("id").template get<ObjectId>();
			const auto found = objects.find(id);
			require(found != objects.end(), name + ": the part has no " + what + " " + std::to_string(id));
			return found->second;
		}

		Json setNotes(Project& project, CommandContext& context)
		{
			MidiPart& part = findPart(project, context.parameters());
			const Json& items = context.parameters().at("notes");
			std::vector<Note> notes;
			for (size_t i = 0; i < items.size(); ++i)
			{
				const Json& item = items[i];
				const std::string name = "notes[" + std::to_string(i) + "]";
				Note note = changed(part.notes, item, name, "note");
				const time::Ticks end = note.position + note.length;
				const time::Ticks start = note.position;
				note.position = position(item, note.position, name);
				note.length = item.value("length", note.length);
				require(note.length > 0, name + ".length must be positive");
				const time::Ticks longest = longestProjectTicks(project); // so that the end can't overflow (#1906)
				require(note.position <= longest && note.length <= longest,
					name + ".position and length must be at most " + std::to_string(longest) + ", the longest project");
				// A start resized, its end where it was: the expression stays where it is too.
				if (note.position != start && note.position + note.length == end && !item.contains("expressions"))
					note.expressions = expressionsFrom(note.expressions, note.position - start);
				note.pitch = integer(item, "pitch", note.pitch, 0, 127, name);
				note.channel = integer(item, "channel", note.channel, 0, 15, name);
				note.velocity = scaled(item, "velocity", note.velocity, g_maxVelocity, name);
				note.releaseVelocity = scaled(item, "releaseVelocity", note.releaseVelocity, g_maxVelocity, name);
				note.probability = item.value("probability", note.probability);
				require(note.probability >= 0.0 && note.probability <= 1.0, name + ".probability must be 0 to 1");
				note.velocityVariance = item.value("velocityVariance", note.velocityVariance);
				require(std::abs(note.velocityVariance) <= 1.0, name + ".velocityVariance must be -1 to 1");
				note.articulations = item.value("articulations", note.articulations);
				note.mutedArticulations = mutedArticulationsFrom(item, note, name);
				if (item.contains("expressions"))
					note.expressions = expressionsFromJson(item.at("expressions"), name + ".expressions", longest);
				notes.push_back(note);
			}
			// Changed only once every item is valid. ProjectModel::add would restore the project anyway.
			Json result = Json::array();
			for (const Note& note : notes)
			{
				part.notes[note.id] = note;
				result.push_back(toJson(note));
			}
			context.emit("note.changed", {{"part", part.id}, {"notes", result}});
			return {{"notes", result}};
		}

		// A MIDI or instrument track's note expression setup (#1513): its bypass and input assignments.
		Json setNoteExpression(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const auto id = parameters.at("track").get<ObjectId>();
			const auto track = project.trackById.find(id);
			const bool midi = track != project.trackById.end() &&
				(track->second.type == TrackType::Midi || track->second.type == TrackType::Instrument);
			const auto channel = midi ? project.channels.find(track->second.channel) : project.channels.end();
			require(channel != project.channels.end(), "no MIDI or instrument track " + std::to_string(id));
			NoteExpressionSetup& setup = channel->second.noteExpression;
			const std::set<plugin::NoteExpression> hidden = setup.hidden;
			setup = noteExpressionFromJson(parameters, setup, false);
			context.emit("channel.changed", {{"channel", toJson(channel->second)}});
			// The editors show the notes without the hidden expressions (#2120): not a step of channels only, which
			// they need not read the project for (ui::changedOnly).
			if (setup.hidden != hidden)
				context.emit("noteExpression.changed", {{"track", id}, {"noteExpression", toJson(setup)}});
			return {{"track", id}, {"noteExpression", toJson(setup)}};
		}

		// Convert to Note Expression (#1515, reference manual p. 1316): the part's controller events whose message the
		// track's setup assigns to an expression become points of it in the notes that sound there on their channel,
		// each the event's value over the expression's range, and leave the part. Those that no note sounds at stay.
		Json convertToExpression(Project& project, CommandContext& context)
		{
			MidiPart& part = findPart(project, context.parameters());
			const NoteExpressionSetup& setup =
				project.channels.at(project.trackById.at(part.track).channel).noteExpression;
			std::map<ObjectId, Note> notes; // the notes that get points, as they will be
			Json removed = Json::array();
			for (auto controller = part.controllers.begin(); controller != part.controllers.end();)
			{
				const Controller& event = controller->second;
				std::optional<uint8_t> message;
				if (event.type == ControllerType::Control)
					message = event.number;
				else if (event.type == ControllerType::PitchBend)
					message = midi::MpeSetup::g_pitchBend;
				else if (event.type == ControllerType::ChannelPressure)
					message = midi::MpeSetup::g_aftertouch;
				bool taken = false;
				for (const auto& [type, input] : setup.inputs)
				{
					if (!message || input.message != message)
						continue;
					const double value = input.min + ((input.max - input.min) * (event.value / g_maxControllerValue));
					for (const auto& [id, note] : part.notes)
						if (note.channel == event.channel && event.position >= note.position &&
							event.position < note.position + note.length)
						{
							taken = true;
							notes.try_emplace(id, note).first->second.expressions.push_back(
								{type, event.position - note.position, value});
						}
				}
				if (!taken)
				{
					++controller;
					continue;
				}
				removed.push_back(controller->first);
				controller = part.controllers.erase(controller);
			}
			Json changed = Json::array();
			for (auto& [id, note] : notes)
			{
				std::stable_sort(note.expressions.begin(), note.expressions.end(),
					[](const ExpressionPoint& a, const ExpressionPoint& b) { return a.position < b.position; });
				part.notes.at(id) = note;
				changed.push_back(toJson(note));
			}
			if (changed.empty())
			{
				context.markUnchanged();
				return {{"notes", changed}};
			}
			context.emit("note.changed", {{"part", part.id}, {"notes", changed}});
			context.emit("controller.removed", {{"part", part.id}, {"controllers", removed}});
			return {{"notes", changed}};
		}

		// The key editor's expression editor (#1512, #1795): each change replaces the note's points of one expression
		// from `from` to `to` with its points, so that an edit sent before the client saw the last one keeps it.
		Json replaceExpression(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			MidiPart& part = findPart(project, parameters);
			const auto type = parameters.at("type").get<std::string>();
			const Json& items = parameters.at("notes");
			std::map<ObjectId, Note> notes; // as the changes so far leave them, also a note named twice
			std::vector<ObjectId> order;
			for (size_t i = 0; i < items.size(); ++i)
			{
				const Json& item = items[i];
				const std::string name = "notes[" + std::to_string(i) + "]";
				const Note& current = changed(part.notes, item, name, "note");
				const auto from = item.at("from").get<time::Ticks>();
				const auto to = item.at("to").get<time::Ticks>();
				require(to >= from, name + ".to must not be before its from");
				Json points = Json::array();
				for (size_t j = 0; j < item.at("points").size(); ++j)
				{
					const Json& each = item.at("points")[j];
					const auto position = each.at("position").get<time::Ticks>();
					require(position >= from && position <= to,
						name + ".points[" + std::to_string(j) + "].position must be from " + std::to_string(from) +
							" to " + std::to_string(to));
					points.push_back({{"type", type}, {"position", position}, {"value", each.at("value")}});
				}
				const std::vector<ExpressionPoint> added =
					expressionsFromJson(points, name + ".points", longestProjectTicks(project));
				auto [found, inserted] = notes.try_emplace(current.id, current);
				if (inserted)
					order.push_back(current.id);
				std::vector<ExpressionPoint>& expressions = found->second.expressions;
				expressions.erase(std::remove_if(expressions.begin(), expressions.end(),
									  [&](const ExpressionPoint& point)
									  {
										  return g_expressionNames.at(static_cast<size_t>(point.type)) == type &&
											  point.position >= from && point.position <= to;
									  }),
					expressions.end());
				expressions.insert(expressions.end(), added.begin(), added.end());
				std::stable_sort(expressions.begin(), expressions.end(),
					[](const ExpressionPoint& a, const ExpressionPoint& b) { return a.position < b.position; });
			}
			// Changed only once every item is valid, and only the notes whose points changed.
			Json result = Json::array();
			for (const ObjectId id : order)
			{
				Note& note = part.notes.at(id);
				if (notes.at(id).expressions == note.expressions)
					continue;
				note.expressions = std::move(notes.at(id).expressions);
				result.push_back(toJson(note));
			}
			if (result.empty())
			{
				context.markUnchanged(); // no change, so no modified project without an undo step
				return {{"notes", result}};
			}
			context.emit("note.changed", {{"part", part.id}, {"notes", result}});
			return {{"notes", result}};
		}

		// The key editor's split tool (#1419, reference manual p. 1215): cuts the named notes that the position cuts.
		Json splitNotes(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			MidiPart& part = findPart(project, parameters);
			const auto at = parameters.at("position").get<time::Ticks>();
			std::vector<Note*> named;
			for (const Json& id : parameters.at("notes"))
			{
				const auto found = part.notes.find(id.get<ObjectId>());
				require(found != part.notes.end(), "the part has no note " + id.dump());
				named.push_back(&found->second);
			}
			Json changed = Json::array();
			Json added = Json::array();
			for (Note* note : named)
			{
				if (at <= note->position || at >= note->position + note->length) // also a note named twice, once cut
					continue;
				Note rest = splitNote(*note, at - note->position);
				rest.id = project.newId();
				part.notes[rest.id] = rest;
				changed.push_back(toJson(*note));
				added.push_back(toJson(rest));
			}
			if (added.empty())
			{
				context.markUnchanged(); // no change, so no modified project without an undo step (#1496)
				return {{"notes", added}};
			}
			context.emit("note.changed", {{"part", part.id}, {"notes", changed}});
			context.emit("note.added", {{"part", part.id}, {"notes", added}});
			return {{"notes", added}};
		}

		// points before cut, then from's moved to at. Of what points express and from doesn't start with (atStart
		// tells its points at its start), the join gets the default (defaultExpression), as the note that from belonged
		// to started with it. A point of the join whose value already plays there is left out: it changes nothing.
		template <typename AtStart> std::vector<ExpressionPoint> joined(const std::vector<ExpressionPoint>& points,
			time::Ticks cut, const std::vector<ExpressionPoint>& from, time::Ticks at, AtStart atStart)
		{
			std::vector<ExpressionPoint> result;
			std::array<std::optional<double>, g_expressionNames.size()> playing{}; // by type, at the join
			for (const ExpressionPoint& point : points)
				if (point.position < cut)
				{
					result.push_back(point);
					playing.at(static_cast<size_t>(point.type)) = point.value;
				}
			std::array<bool, g_expressionNames.size()> starts{};
			for (const ExpressionPoint& point : from)
				if (atStart(point.position))
					starts.at(static_cast<size_t>(point.type)) = true;
			for (size_t type = 0; type < playing.size(); ++type)
			{
				const auto expression = static_cast<plugin::NoteExpression>(type);
				const std::optional<double> value = defaultExpression(expression);
				if (playing.at(type).has_value() && !starts.at(type) && value.has_value() && value != playing.at(type))
					result.push_back({expression, at, *value});
			}
			for (const ExpressionPoint& point : from)
			{
				std::optional<double>& value = playing.at(static_cast<size_t>(point.type));
				if (atStart(point.position) && value == point.value)
					continue;
				result.push_back({point.type, point.position + at, point.value});
				value = point.value;
			}
			return result;
		}

		// The key editor's glue tool (#1419, p. 1215): the note lasts to the next one's end, which it replaces.
		Json glueNotes(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			MidiPart& part = findPart(project, parameters);
			const auto noteNamed = [&part, &parameters](const char* key) -> Note&
			{
				const auto found = part.notes.find(parameters.at(key).get<ObjectId>());
				require(found != part.notes.end(), std::string("the part has no note ") + parameters.at(key).dump());
				return found->second;
			};
			Note& note = noteNamed("note");
			const Note& next = noteNamed("next");
			require(next.position > note.position, "next must start after note");
			// What played goes on playing: the note's points up to where the next one starts, or to its own end, then
			// the next one's up to its end, and after a next one that ends inside the note the note's own again. Points
			// at or past a note's end, e.g. after a trim, never played (#1496).
			const time::Ticks offset = next.position - note.position;
			const time::Ticks nextEnd = offset + next.length;
			std::vector<ExpressionPoint> played;
			std::copy_if(next.expressions.begin(), next.expressions.end(), std::back_inserter(played),
				[&next](const ExpressionPoint& point) { return point.position < next.length; });
			// A point within the next note's first sample is one it starts with, as playback has it
			// (RenderNote::startsWith).
			const time::Ticks start = time::toTicks(part.position, project.tempo, project.sampleRate) + next.position;
			const time::Samples startSample = project.tempo.toSamples(start, project.sampleRate);
			const auto inFirstSample = [&project, start, startSample](time::Ticks position)
			{ return project.tempo.toSamples(start + position, project.sampleRate) == startSample; };
			std::vector<ExpressionPoint> points =
				joined(note.expressions, std::min(offset, note.length), played, offset, inFirstSample);
			if (nextEnd < note.length) // the first note's values there are points at 0
				points = joined(points, nextEnd, expressionsFrom(note.expressions, nextEnd), nextEnd,
					[](time::Ticks position) { return position == 0; });
			note.expressions = std::move(points);
			note.length = std::max(next.position + next.length, note.position + note.length) - note.position;
			const ObjectId removed = next.id;
			part.notes.erase(removed); // next dangles from here on
			const Json glued = toJson(note);
			context.emit("note.changed", {{"part", part.id}, {"notes", Json::array({glued})}});
			context.emit("note.removed", {{"part", part.id}, {"notes", Json::array({removed})}});
			return {{"note", glued}};
		}

		// The notes the parameters name, moved to another part by the offsets, as a drag from one in-place editor to
		// another gives them (p. 1261): every note keeps its id and every field but its position and pitch.
		Json moveNotes(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			MidiPart& source = findPart(project, parameters);
			const auto targetId = parameters.at("target").get<ObjectId>();
			const auto target = project.midiParts.find(targetId);
			require(target != project.midiParts.end(), "no MIDI part " + std::to_string(targetId));
			require(source.id == targetId || source.shared == 0 || source.shared != target->second.shared,
				"can't move notes between shared copies of one group: they must stay equal");
			const auto ticks = parameters.value("ticks", time::Ticks{0});
			const time::Ticks longest = longestProjectTicks(project); // so that the sums can't overflow (#1906)
			require(ticks >= -longest && ticks <= longest,
				"ticks must be at most " + std::to_string(longest) + ", the longest project");
			const auto semitones = parameters.value("semitones", int64_t{0});
			const auto ids = parameters.at("notes").get<std::vector<ObjectId>>();
			// The offsets count on the project's timeline, while a note's position counts from its part's start:
			// the difference of the two starts goes into the positions, in whatever time base the parts are.
			const time::Ticks sourceStart = time::toTicks(source.position, project.tempo, project.sampleRate);
			const time::Ticks targetStart = time::toTicks(target->second.position, project.tempo, project.sampleRate);
			// Checked before anything moves. ProjectModel::add would restore the project anyway.
			std::vector<Note> notes;
			for (size_t i = 0; i < ids.size(); ++i)
			{
				const std::string name = "notes[" + std::to_string(i) + "]";
				const auto found = source.notes.find(ids[i]);
				require(found != source.notes.end(), name + ": the part has no note " + std::to_string(ids[i]));
				Note note = found->second;
				note.position = sourceStart + note.position + ticks - targetStart;
				require(note.position >= 0, name + " would land before the target part's start");
				require(note.position <= longest, name + " would land past the end of the longest project");
				const int64_t pitch = static_cast<int64_t>(note.pitch) + semitones;
				require(pitch >= 0 && pitch <= 127, name + " would land outside the pitches 0 to 127");
				note.pitch = static_cast<uint8_t>(pitch);
				notes.push_back(std::move(note));
			}
			// The selected controller events go along (p. 1188), shifted in time only. One that would land before the
			// target part's start lands at its start, as the key editor's move within a part keeps one at its start.
			const auto controllerIds = parameters.value("controllers", std::vector<ObjectId>{});
			std::vector<Controller> controllers;
			for (size_t i = 0; i < controllerIds.size(); ++i)
			{
				const auto found = source.controllers.find(controllerIds[i]);
				require(found != source.controllers.end(),
					"controllers[" + std::to_string(i) + "]: the part has no controller event " +
						std::to_string(controllerIds[i]));
				Controller controller = found->second;
				controller.position = std::max<time::Ticks>(sourceStart + controller.position + ticks - targetStart, 0);
				controllers.push_back(std::move(controller));
			}
			Json added = Json::array();
			for (size_t i = 0; i < notes.size(); ++i)
			{
				source.notes.erase(ids[i]);
				added.push_back(toJson(target->second.notes[ids[i]] = std::move(notes[i])));
			}
			Json addedControllers = Json::array();
			for (size_t i = 0; i < controllers.size(); ++i)
			{
				source.controllers.erase(controllerIds[i]);
				addedControllers.push_back(toJson(target->second.controllers[controllerIds[i]] = controllers[i]));
			}
			if (!ids.empty())
			{
				context.emit("note.removed", {{"part", source.id}, {"notes", ids}});
				context.emit("note.added", {{"part", target->second.id}, {"notes", added}});
			}
			if (!controllerIds.empty())
			{
				context.emit("controller.removed", {{"part", source.id}, {"controllers", controllerIds}});
				context.emit("controller.added", {{"part", target->second.id}, {"controllers", addedControllers}});
			}
			return {{"notes", added}, {"controllers", addedControllers}};
		}

		// The part's notes that the parameters' notes name, in that order.
		std::vector<Note*> namedNotes(MidiPart& part, const Json& parameters)
		{
			std::vector<Note*> notes;
			for (const Json& id : parameters.at("notes"))
			{
				const auto found = part.notes.find(id.get<ObjectId>());
				require(found != part.notes.end(), "the part has no note " + id.dump());
				notes.push_back(&found->second);
			}
			require(!notes.empty(), "notes must name at least one note");
			return notes;
		}

		std::vector<int> pitchesOf(const std::vector<Note*>& notes)
		{
			std::vector<int> pitches;
			pitches.reserve(notes.size());
			for (const Note* note : notes)
				pitches.push_back(note->pitch);
			return pitches;
		}

		Json chordOf(Project& project, CommandContext& context)
		{
			MidiPart& part = findPart(project, context.parameters());
			const std::optional<midi::Chord> chord =
				midi::recognizeChord(pitchesOf(namedNotes(part, context.parameters())));
			if (!chord)
				return {{"chord", nullptr}};
			return {{"chord", {{"root", chord->root}, {"type", chord->type->name}, {"name", chord->name()}}}};
		}

		Json voiceNotes(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			MidiPart& part = findPart(project, parameters);
			const std::vector<Note*> notes = namedNotes(part, parameters);
			const std::vector<int> pitches = pitchesOf(notes);
			require(parameters.contains("chord") != parameters.contains("voicing"), "give either chord or voicing");
			std::vector<int> changed;
			if (parameters.contains("chord"))
			{
				const midi::ChordType* type = midi::chordType(parameters.at("chord").get<std::string>());
				require(type != nullptr, "no chord type " + parameters.at("chord").dump());
				changed = midi::fitChord(pitches, *type);
			}
			else
			{
				const std::optional<midi::Voicing> voicing =
					midi::voicingFromName(parameters.at("voicing").get<std::string>());
				if (!voicing)
					throw CommandError("no voicing " + parameters.at("voicing").dump());
				const std::optional<std::vector<int>> revoiced = midi::revoice(pitches, *voicing);
				if (!revoiced)
					throw CommandError("the notes can't take that voicing: too few, or out of range");
				changed = *revoiced;
			}
			Json result = Json::array();
			for (size_t i = 0; i < notes.size(); ++i)
				if (notes[i]->pitch != changed[i])
				{
					notes[i]->pitch = static_cast<uint8_t>(changed[i]);
					result.push_back(toJson(*notes[i]));
				}
			if (!result.empty())
				context.emit("note.changed", {{"part", part.id}, {"notes", result}});
			return {{"notes", result}};
		}

		// Whether controller events of the type have the field, besides id, position and type.
		bool uses(ControllerType type, const std::string& key)
		{
			switch (type)
			{
			case ControllerType::Control:
			case ControllerType::PolyPressure:
				return key == "number" || key == "value" || key == "channel";
			case ControllerType::PitchBend:
			case ControllerType::ChannelPressure:
				return key == "value" || key == "channel";
			case ControllerType::Program:
				return key == "number" || key == "bank" || key == "channel";
			case ControllerType::SysEx:
				return key == "data";
			case ControllerType::Articulation:
				return key == "articulation" || key == "reset" || key == "muted";
			}
			return false;
		}

		Json setControllers(Project& project, CommandContext& context)
		{
			MidiPart& part = findPart(project, context.parameters());
			const Json& items = context.parameters().at("controllers");
			std::vector<Controller> controllers;
			for (size_t i = 0; i < items.size(); ++i)
			{
				const Json& item = items[i];
				const std::string name = "controllers[" + std::to_string(i) + "]";
				const Controller& current = changed(part.controllers, item, name, "controller");
				// The event as it becomes, its fields with the changes, checked against its type as controller.add
				// checks a new one. A null bank removes it.
				Json merged = toJson(current);
				for (auto field = item.begin(); field != item.end(); ++field)
					merged[field.key()] = field.value();
				if (merged.contains("bank") && merged.at("bank").is_null())
					merged.erase("bank");
				const std::optional<ControllerType> type = controllerTypeFromName(merged.at("type").get<std::string>());
				if (type.has_value())
					for (auto field = item.begin(); field != item.end(); ++field)
						require(field.key() == "id" || field.key() == "position" || field.key() == "type" ||
								uses(*type, field.key()),
							name + "." + field.key() + " doesn't apply to " + controllerTypeName(*type) + " events");
				// A muted direction made another type isn't muted.
				if (type != ControllerType::Articulation && !item.contains("muted"))
					merged.erase("muted");
				Controller controller = controllerFrom(merged, name);
				controller.id = current.id;
				controllers.push_back(std::move(controller));
			}
			Json result = Json::array();
			for (const Controller& controller : controllers)
			{
				part.controllers[controller.id] = controller;
				result.push_back(toJson(controller));
			}
			context.emit("controller.changed", {{"part", part.id}, {"controllers", result}});
			return {{"controllers", result}};
		}

		// {"part": id, key: [changes]}, each change {"id": id, and what changes}.
		Json changesSchema(const char* key, const Json& properties)
		{
			Json change = {
				{"type", "object"}, {"additionalProperties", false}, {"required", {"id"}}, {"properties", properties}};
			change["properties"]["id"] = {{"type", "integer"}};
			return {{"type", "object"}, {"additionalProperties", false}, {"required", {"part", key}},
				{"properties", {{"part", {{"type", "integer"}}}, {key, {{"type", "array"}, {"items", change}}}}}};
		}

		Json resultSchema(const char* key)
		{
			return {{"type", "object"}, {"required", {key}}, {"properties", {{key, {{"type", "array"}}}}}};
		}
	}

	void ProjectModel::registerMidiEditCommands(Engine& engine)
	{
		const Json integer = {{"type", "integer"}};
		const Json number = {{"type", "number"}};
		add(engine,
			{"note.set", 1,
				"Changes notes of a MIDI part: each change names a note by id and gives what changes, position and "
				"length in ticks (each at most the longest project's end, see audioEvent.add), pitch 0-127, velocity "
				"0-1, release velocity 0-1, MIDI channel 0-15, play probability 0-1, velocity variance -1 to 1, "
				"articulations, the attributes it has, mutedArticulations, those of them that are muted (an attribute "
				"that goes is no longer muted), or expressions, which replace its expression points (see note.add). "
				"The points move with the note's start, but for a start that moves while the end stays: then they stay "
				"where they are, and the values at the new start become its initial ones. Returns the changed notes.",
				changesSchema("notes",
					{{"position", integer}, {"length", integer}, {"pitch", integer}, {"velocity", number},
						{"releaseVelocity", number}, {"channel", integer}, {"probability", number},
						{"velocityVariance", number},
						{"articulations", {{"type", "array"}, {"items", {{"type", "string"}}}}},
						{"mutedArticulations", {{"type", "array"}, {"items", {{"type", "string"}}}}},
						{"expressions", expressionsSchema()}}),
				resultSchema("notes"), false},
			setNotes);
		const Json span = {{"type", "object"}, {"additionalProperties", false},
			{"required", {"id", "from", "to", "points"}},
			{"properties",
				{{"id", integer},
					{"from",
						{{"type", "integer"},
							{"description",
								"where the span starts, in ticks from the note's start, as the points' positions"}}},
					{"to",
						{{"type", "integer"},
							{"description", "where the span ends, included, in ticks from the note's start"}}},
					{"points",
						{{"type", "array"},
							{"items",
								{{"type", "object"}, {"additionalProperties", false},
									{"required", {"position", "value"}},
									{"properties", {{"position", integer}, {"value", number}}}}}}}}}};
		add(engine,
			{"note.replaceExpression", 1,
				"Replaces the points of one expression of notes of a MIDI part in a span, as the key editor's "
				"expression editor draws or erases them: each change names a note by id, and its points of type from "
				"from to to (in ticks from the note's start, both included) give way to the change's points, "
				"{position, value} each, within the span and in the expression's range (see note.add). The note's "
				"other points stay, so an edit made before its sender saw the last one keeps that one. The points "
				"end up sorted by position, a new one after the old ones at its position. A note named twice gets "
				"both changes in order. Refuses, changing nothing, a part or a note that doesn't exist, a span whose "
				"to is before its from and points outside it or out of range (see note.add). Changes that leave every "
				"note's points as they were change nothing, and the project doesn't count as modified, run on its "
				"own: in a history.transaction the transaction counts as a change. The changes are one undo step. "
				"Returns the notes whose points changed.",
				{{"type", "object"}, {"additionalProperties", false}, {"required", {"part", "type", "notes"}},
					{"properties",
						{{"part", integer}, {"type", {{"type", "string"}, {"enum", g_expressionNames}}},
							{"notes", {{"type", "array"}, {"items", span}}}}}},
				resultSchema("notes"), false},
			replaceExpression);
		const Json expressionInput = {{"type", "object"}, {"additionalProperties", false}, {"required", {"type"}},
			{"properties",
				{{"type", {{"type", "string"}, {"enum", g_expressionNames}}},
					{"input",
						{{"type", "string"},
							{"description", "none, pitchBend, aftertouch or a controller, cc0 to cc127"}}},
					{"min", number}, {"max", number}}}};
		addEdit(engine,
			{"noteExpression.set", 1,
				"Changes a MIDI or instrument track's note expression setup, as the inspector's Note Expression "
				"section does (reference manual pp. 1300-1302): bypass plays the track's notes without their "
				"expression points, and each of inputs sets an expression's input assignment, the MIDI message "
				"mapped to it (input: none, pitchBend, aftertouch or cc0 to cc127, as midi.setPort names them) and "
				"the range of the expression's values that it goes over, min and max in the expression's unit and "
				"range (see note.add), min above max for a reversed one. hidden lists the expressions the "
				"inspector's visibility hides from the key editor's note expression editor and its notes (all of "
				"them visible by default). What a call leaves out stays as it was. "
				"The setup belongs to the track and is saved with the project; project.get lists it as the "
				"channel's noteExpression, whose inputs are the expressions that have a message or a range other "
				"than the whole one. Refuses, changing nothing, a track that is no MIDI or instrument track, unknown "
				"expressions and messages and values out of range. Emits channel.changed, and noteExpression.changed "
				"with the track and its setup when hidden changes. Returns the track and its "
				"setup.",
				{{"type", "object"}, {"additionalProperties", false}, {"required", {"track"}},
					{"properties",
						{{"track", integer}, {"bypass", {{"type", "boolean"}}},
							{"inputs", {{"type", "array"}, {"items", expressionInput}}},
							{"hidden",
								{{"type", "array"}, {"items", {{"type", "string"}, {"enum", g_expressionNames}}}}}}}},
				{{"type", "object"}, {"required", {"track", "noteExpression"}},
					{"properties", {{"track", integer}, {"noteExpression", {{"type", "object"}}}}}},
				false},
			"Change note expression setup", setNoteExpression);
		addEdit(engine,
			{"noteExpression.convert", 1,
				"Convert to Note Expression (reference manual p. 1316): the controller events of a MIDI part whose "
				"message its track's note expression setup assigns to an expression (noteExpression.set inputs: a "
				"controller, pitch bend or aftertouch) become points of that expression in the notes that sound at "
				"their position on their MIDI channel, every such note getting the same one, the event's value over "
				"the expression's range, and are removed from the part. Events that no note sounds at stay. Refuses a "
				"part that doesn't exist. Emits note.changed and controller.removed; with nothing to convert it "
				"changes nothing. Returns the notes that got points.",
				{{"type", "object"}, {"additionalProperties", false}, {"required", {"part"}},
					{"properties", {{"part", integer}}}},
				resultSchema("notes"), false},
			"Convert to note expression", convertToExpression);
		const Json noteMove = {{"type", "object"}, {"additionalProperties", false},
			{"required", {"part", "notes", "target"}},
			{"properties",
				{{"part", integer}, {"notes", {{"type", "array"}, {"items", integer}}}, {"target", integer},
					{"ticks",
						{{"type", "integer"},
							{"description", "how far the notes move on the project's timeline, in ticks"}}},
					{"semitones",
						{{"type", "integer"}, {"description", "how far the notes move in pitch, in semitones"}}},
					{"controllers",
						{{"type", "array"}, {"items", integer},
							{"description", "controller events of the part that move along, in time only"}}}}}};
		add(engine,
			{"note.move", 1,
				"Moves notes of a MIDI part to another part, as dragging them from one in-place editor to another does "
				"(reference manual p. 1261): each note keeps its id and every field but its position and pitch. ticks "
				"moves the notes on the project's timeline, to at most the longest project's end (see audioEvent.add), "
				"and semitones in pitch; the command converts the timeline shift to the target part's own positions, "
				"wherever the two parts start on the timeline and whatever their time bases. The source part and the "
				"target part may be the same. The move is one undo step, and the shared copies of both parts follow "
				"it. A note may reach past the target part's end, as note.add and note.set allow. Refuses, changing "
				"nothing, a part or a note that doesn't exist, a note that would land on the timeline before the "
				"target part's start or outside the pitches 0 to 127, and a move between shared copies of one group, "
				"which must stay equal. Emits note.removed for the source part and note.added for the target part. "
				"controllers names controller events of the source part that go along, as the key editor moves the "
				"selected ones with the notes (p. 1188): each keeps its id and every field, ticks shifts it and "
				"semitones doesn't, and one that would land before the target part's start lands at its start; a "
				"controller event that doesn't exist refuses the move. notes may then be empty; the undo step is "
				"\"Move notes\" also then. Emits controller.removed and controller.added for them, and the note events "
				"only when notes names any. Returns the moved notes and controller events, as the target part has "
				"them.",
				noteMove,
				{{"type", "object"}, {"required", {"notes", "controllers"}},
					{"properties", {{"notes", {{"type", "array"}}}, {"controllers", {{"type", "array"}}}}}},
				false},
			moveNotes);
		add(engine,
			{"controller.set", 1,
				"Changes controller events of a MIDI part: each change names a controller event by id and gives what "
				"changes: position in ticks, type, and the fields of the type as controller.add takes them (number, "
				"value, channel, bank, data, articulation, reset, muted). A field the type doesn't have is an error, "
				"such as value on a program change; a null bank removes it. A new type keeps the fields it shares with "
				"the old one and needs the "
				"others. Returns the changed controller events.",
				changesSchema("controllers",
					{{"position", integer},
						{"type",
							{{"type", "string"},
								{"enum",
									{"controller", "pitchBend", "channelPressure", "polyPressure", "program", "sysEx",
										"articulation"}}}},
						{"number", integer}, {"value", number}, {"channel", integer},
						{"bank", {{"type", {"integer", "null"}}}}, {"data", {{"type", "array"}, {"items", integer}}},
						{"articulation", {{"type", "string"}}}, {"reset", {{"type", "boolean"}}},
						{"muted", {{"type", "boolean"}}}}),
				resultSchema("controllers"), false},
			setControllers);

		add(engine,
			{"note.split", 1,
				"Splits notes of a MIDI part at a position in ticks from the part's start, as the key editor's split "
				"tool does: each named note that the position cuts ends there, and its rest, a new note with the same "
				"values, starts there; the others stay as they are. The expression points before the cut stay with "
				"the note, the rest starts with the values at the cut and goes on with the later points (see "
				"note.set). Both pieces lose their quantize origin, as the notes that event.split cuts with splitNotes "
				"do: quantize.reset no longer moves them. A position that cuts none of them changes nothing, and the "
				"project doesn't count as modified. Returns the rests.",
				{{"type", "object"}, {"additionalProperties", false}, {"required", {"part", "notes", "position"}},
					{"properties",
						{{"part", integer}, {"notes", {{"type", "array"}, {"items", integer}}},
							{"position", {{"type", "integer"}, {"minimum", 0}}}}}},
				resultSchema("notes"), false},
			splitNotes);
		add(engine,
			{"note.glue", 1,
				"Glues a note of a MIDI part to a later one, as the key editor's glue tool does: note then lasts to "
				"the later of their ends and keeps its values, and next is removed. Its expression points go on "
				"playing: note keeps its own up to next's start, then has next's up to next's end, and after a next "
				"that ends inside note its own again. Where one note's points give way to the other's, an expression "
				"that the first one has and the other has no point of at its start (within its first sample, as "
				"playback has it) goes back to its default, as the other note started with it: no tuning, no "
				"pressure, brightness 0.5, volume 1 and pan 0.5; vibrato and expression keep their value. A point "
				"there whose value already plays is left out.",
				{{"type", "object"}, {"additionalProperties", false}, {"required", {"part", "note", "next"}},
					{"properties", {{"part", integer}, {"note", integer}, {"next", integer}}}},
				{{"type", "object"}, {"required", {"note"}}, {"properties", {{"note", {{"type", "object"}}}}}}, false},
			glueNotes);

		Json chordTypes = Json::array();
		for (const midi::ChordType& type : midi::chordTypes())
			chordTypes.push_back(type.name);
		const Json notesOfPart = {{"type", "object"}, {"additionalProperties", false}, {"required", {"part", "notes"}},
			{"properties", {{"part", integer}, {"notes", {{"type", "array"}, {"items", integer}}}}}};
		add(engine,
			{"note.chord", 1,
				"The chord that notes of a MIDI part make, in any octave and order (arpeggiated notes too), as the key "
				"editor's Chord Editing section shows it: chord {root, the pitch class (0 is C), type, as note.voicing "
				"takes it, and name, such as Cm7}, or null if they make none of the chord types exactly. Of several "
				"roots that fit, the lowest note's.",
				notesOfPart,
				{{"type", "object"}, {"required", {"chord"}},
					{"properties", {{"chord", {{"type", {"object", "null"}}}}}}},
				true},
			chordOf);
		Json voicingSchema = notesOfPart;
		voicingSchema["properties"]["chord"] = {{"type", "string"}, {"enum", chordTypes}};
		voicingSchema["properties"]["voicing"] = {
			{"type", "string"}, {"enum", {"invertUp", "invertDown", "drop2", "drop3", "drop24"}}};
		add(engine,
			{"note.voicing", 1,
				"Transposes notes of a MIDI part as the key editor's Chord Editing section does (reference manual pp. "
				"1215-1217). chord fits them to a chord type (major, minor, diminished, augmented, sus2, sus4; major7, "
				"minor7, dominant7, halfDiminished7, diminished7, minorMajor7) on the root of the chord they make, or "
				"else of the lowest note: from the lowest up, each moves to the nearest pitch of a chord tone that no "
				"lower note took since every tone last had one, down on a tie. voicing changes the voicing: invertUp "
				"moves the lowest note up by octaves until it is the highest, invertDown the highest down until it is "
				"the lowest, drop2, drop3 and drop24 move the second, the third, or the second and fourth highest "
				"notes down an octave. Give chord or voicing. Returns the notes that changed.",
				voicingSchema, resultSchema("notes"), false},
			voiceNotes);
	}
}
