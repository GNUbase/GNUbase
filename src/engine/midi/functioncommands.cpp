// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/midi/functioncommands.h"

#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/midi/functions.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::engine::midi
{
	namespace
	{
		using time::Ticks;

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		void requireRange(int64_t value, int64_t min, int64_t max, const std::string& name)
		{
			require(
				value >= min && value <= max, name + " must be " + std::to_string(min) + " to " + std::to_string(max));
		}

		Json objectSchema(Json properties, const std::vector<std::string>& required = {})
		{
			return {{"type", "object"}, {"properties", std::move(properties)}, {"required", required},
				{"additionalProperties", false}};
		}

		Json arraySchema(Json items)
		{
			return {{"type", "array"}, {"items", std::move(items)}};
		}

		Json enumSchema(const std::vector<std::string>& values)
		{
			return {{"type", "string"}, {"enum", values}};
		}

		// The parts a function works on, each named once.
		std::vector<MidiPart*> partsOf(Project& project, const Json& parameters)
		{
			const auto ids = parameters.at("parts").get<std::vector<ObjectId>>();
			require(!ids.empty(), "parts must name at least one MIDI part");
			std::vector<MidiPart*> parts;
			for (const ObjectId id : ids)
			{
				const auto found = project.midiParts.find(id);
				require(found != project.midiParts.end(), "no MIDI part " + std::to_string(id));
				require(std::find(parts.begin(), parts.end(), &found->second) == parts.end(),
					"MIDI part " + std::to_string(id) + " is named twice");
				parts.push_back(&found->second);
			}
			return parts;
		}

		// The selected notes, which must be in the parts; every note without notes.
		NoteSelection selectionOf(const std::vector<MidiPart*>& parts, const Json& parameters)
		{
			if (!parameters.contains("notes"))
				return {};
			std::set<ObjectId> notes;
			for (const ObjectId id : parameters.at("notes").get<std::vector<ObjectId>>())
			{
				require(std::any_of(parts.begin(), parts.end(),
							[id](const MidiPart* part) { return part->notes.count(id) != 0; }),
					"no note " + std::to_string(id) + " in the MIDI parts");
				notes.insert(id);
			}
			return NoteSelection(std::move(notes));
		}

		uint16_t velocity(const Json& parameters, const char* key)
		{
			const double value = parameters.at(key).get<double>();
			require(value >= 0.0 && value <= 1.0, std::string(key) + " must be 0 to 1");
			return static_cast<uint16_t>(std::lround(value * g_maxVelocity));
		}

		Ticks positiveTicks(const Json& parameters, const char* key)
		{
			const auto value = parameters.at(key).get<Ticks>();
			require(value > 0, std::string(key) + " must be positive");
			return value;
		}

		// The project gives a function the tempo and new ids.
		using PartFunction = std::function<void(Project& project, MidiPart& part, const NoteSelection& selection)>;
		using Prepare = std::function<PartFunction(const Json& parameters)>;

		// Checks the parameters, runs the function on every part and announces the parts it changed.
		Json runOnParts(Project& project, CommandContext& context, const Prepare& prepare)
		{
			const Json& parameters = context.parameters();
			const std::vector<MidiPart*> parts = partsOf(project, parameters);
			const NoteSelection selection = selectionOf(parts, parameters);
			const PartFunction function = prepare(parameters);
			Json changed = Json::array();
			for (MidiPart* part : parts)
			{
				const MidiPart before = *part;
				function(project, *part, selection);
				if (part->notes != before.notes || part->controllers != before.controllers)
				{
					changed.push_back(part->id);
					context.emit("midiPart.changed", {{"part", toJson(*part)}});
				}
			}
			return {{"parts", changed}};
		}

		// Announces what a function that makes parts changed, and returns it.
		Json announce(const Project& project, const PartChanges& changes, CommandContext& context)
		{
			for (const ObjectId id : changes.tracks)
			{
				const Track& track = project.trackById.at(id);
				context.emit(
					"track.added", {{"track", toJson(track)}, {"channel", toJson(project.channels.at(track.channel))}});
			}
			for (const ObjectId id : changes.removedParts)
				context.emit("midiPart.removed", {{"part", id}});
			for (const ObjectId id : changes.changedParts)
				context.emit("midiPart.changed", {{"part", toJson(project.midiParts.at(id))}});
			for (const ObjectId id : changes.addedParts)
				context.emit("midiPart.added", {{"part", toJson(project.midiParts.at(id))}});
			return {{"tracks", changes.tracks}, {"parts", changes.addedParts}, {"changedParts", changes.changedParts},
				{"removedParts", changes.removedParts}};
		}

		const Track& noteTrack(const Project& project, ObjectId id, const std::string& name)
		{
			const auto found = project.trackById.find(id);
			require(found != project.trackById.end(), "no track " + std::to_string(id));
			require(found->second.type == TrackType::Midi || found->second.type == TrackType::Instrument,
				name + " must be a MIDI or instrument track");
			return found->second;
		}

		Scale scaleOf(const Json& value, const std::string& name)
		{
			requireRange(value.at("root").get<int64_t>(), 0, 11, name + ".root");
			const std::optional<ScaleType> type = scaleTypeFromName(value.at("type").get<std::string>());
			if (!type.has_value())
				throw CommandError(name + ".type is not a scale type");
			return {value.at("root").get<int>(), *type};
		}

		PartFunction prepareTranspose(const Json& parameters)
		{
			TransposeSettings settings;
			settings.semitones = parameters.value("semitones", 0);
			requireRange(settings.semitones, -127, 127, "semitones");
			require(parameters.contains("fromScale") == parameters.contains("toScale"),
				"fromScale and toScale go together");
			if (parameters.contains("fromScale"))
			{
				settings.from = scaleOf(parameters.at("fromScale"), "fromScale");
				settings.to = scaleOf(parameters.at("toScale"), "toScale");
			}
			require(parameters.contains("lowest") == parameters.contains("highest"), "lowest and highest go together");
			if (parameters.contains("lowest"))
			{
				settings.keepInRange = true;
				settings.lowest = parameters.at("lowest").get<int>();
				settings.highest = parameters.at("highest").get<int>();
				requireRange(settings.lowest, 0, 127, "lowest");
				requireRange(settings.highest, 0, 127, "highest");
				require(settings.lowest <= settings.highest, "lowest must not be above highest");
			}
			return [settings](Project&, MidiPart& part, const NoteSelection& selection)
			{ transpose(part, selection, settings); };
		}

		PartFunction prepareDeleteNotes(const Json& parameters)
		{
			DeleteNotesSettings settings;
			require(parameters.contains("minimumLength") || parameters.contains("minimumVelocity"),
				"minimumLength or minimumVelocity is required");
			if (parameters.contains("minimumLength"))
				settings.minimumLength = positiveTicks(parameters, "minimumLength");
			if (parameters.contains("minimumVelocity"))
				settings.minimumVelocity = velocity(parameters, "minimumVelocity");
			settings.both = parameters.value("removeWhen", std::string("either")) == "both";
			return [settings](Project&, MidiPart& part, const NoteSelection& selection)
			{ deleteNotes(part, selection, settings); };
		}

		Json runMerge(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			MergeSettings settings;
			settings.start = parameters.at("start").get<time::Samples>();
			settings.end = parameters.at("end").get<time::Samples>();
			require(settings.start >= 0, "start must not be negative");
			require(settings.end > settings.start, "end must be after start");
			for (const ObjectId id : parameters.value("tracks", std::vector<ObjectId>{}))
			{
				noteTrack(project, id, "track " + std::to_string(id));
				settings.tracks.push_back(id);
			}
			settings.destination = parameters.value("destination", ObjectId{0});
			if (settings.destination != 0)
				noteTrack(project, settings.destination, "destination");
			settings.eraseDestination = parameters.value("eraseDestination", false);
			settings.includeChase = parameters.value("includeChase", false);
			settings.name = parameters.value("name", std::string());
			return announce(project, merge(project, settings), context);
		}

		Json runDissolve(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			std::vector<ObjectId> ids;
			for (const MidiPart* part : partsOf(project, parameters))
				ids.push_back(part->id);
			const DissolveBy by =
				parameters.at("by").get<std::string>() == "pitch" ? DissolveBy::Pitch : DissolveBy::Channel;
			return announce(project, dissolve(project, ids, by, parameters.value("optimize", false)), context);
		}

		Json runBounce(Project& project, CommandContext& context)
		{
			std::vector<ObjectId> ids;
			ObjectId track = 0;
			for (const MidiPart* part : partsOf(project, context.parameters()))
			{
				require(track == 0 || part->track == track, "the parts must be on one track");
				track = part->track;
				ids.push_back(part->id);
			}
			return announce(project, bounce(project, ids), context);
		}

		struct PartCommand
		{
			const char* name;
			const char* action;
			const char* description;
			Json properties;                   // besides parts and notes
			std::vector<std::string> required; // besides parts
			bool notes;                        // takes a note selection
			Prepare prepare;
		};
	}

	void registerFunctionCommands(ProjectModel& model, Engine& engine)
	{
		const Json idSchema = {{"type", "integer"}};
		const Json integerSchema = {{"type", "integer"}};
		const Json numberSchema = {{"type", "number"}};
		const Json booleanSchema = {{"type", "boolean"}};
		const Json scale =
			objectSchema({{"root", integerSchema}, {"type", enumSchema(scaleTypeNames())}}, {"root", "type"});
		const Json partsResult = {
			{"type", "object"}, {"properties", {{"parts", arraySchema(idSchema)}}}, {"required", {"parts"}}};
		const char* const selection = " notes limits it to these notes of the parts.";

		const std::vector<PartCommand> commands = {
			{"midi.transpose", "Transpose",
				"Transposes the notes of MIDI parts by semitones (-127 to 127). fromScale and toScale (a root 0-11, 0 "
				"is C, and a scale type) correct the notes from one scale to another: a note moves from its degree "
				"in fromScale to the same degree in toScale, and notes outside fromScale, or all of them when the "
				"scales have a different number of degrees, go to the closest note of toScale. lowest and highest "
				"(0-127) keep notes in range: a note that leaves it moves by octaves into it, or to the limit it "
				"crossed if no octave fits. Notes stay within 0-127 by octaves. Poly pressure follows when no notes "
				"are given.",
				{{"semitones", integerSchema}, {"fromScale", scale}, {"toScale", scale}, {"lowest", integerSchema},
					{"highest", integerSchema}},
				{}, true, prepareTranspose},
			{"midi.legato", "Legato",
				"Extends notes to the start of the next note on their MIDI channel, plus overlap in ticks (negative: "
				"a gap; default 0). With betweenSelected, to the next of the given notes. The last notes keep their "
				"length.",
				{{"overlap", integerSchema}, {"betweenSelected", booleanSchema}}, {}, true,
				[](const Json& parameters) -> PartFunction
				{
					const Ticks overlap = parameters.value("overlap", Ticks{0});
					const bool between = parameters.value("betweenSelected", false);
					return [=](Project&, MidiPart& part, const NoteSelection& notes)
					{ legato(part, notes, overlap, between); };
				}},
			{"midi.fixLengths", "Fixed lengths", "Sets the length of notes, in ticks.", {{"length", integerSchema}},
				{"length"}, true,
				[](const Json& parameters) -> PartFunction
				{
					const Ticks length = positiveTicks(parameters, "length");
					return [=](Project&, MidiPart& part, const NoteSelection& notes)
					{ fixLengths(part, notes, length); };
				}},
			{"midi.fixVelocities", "Fixed velocities", "Sets the velocity of notes, 0-1.", {{"velocity", numberSchema}},
				{"velocity"}, true,
				[](const Json& parameters) -> PartFunction
				{
					const uint16_t value = velocity(parameters, "velocity");
					return [=](Project&, MidiPart& part, const NoteSelection& notes)
					{ fixVelocities(part, notes, value); };
				}},
			{"midi.pedalToNoteLength", "Sustain pedal to note length",
				"Extends notes that end while the sustain pedal (controller 64) on their channel is down to where it "
				"is released (the part's end if never), but not past the next note of the same pitch, and removes "
				"the sustain pedal events of their channels.",
				Json::object(), {}, true,
				[](const Json&) -> PartFunction
				{
					return [](Project& project, MidiPart& part, const NoteSelection& notes)
					{ pedalToNoteLength(part, notes, lengthInTicks(project, part)); };
				}},
			{"midi.deleteOverlaps", "Delete overlaps",
				"Shortens notes so that none still sounds when the next one on its channel starts: in mode mono the "
				"next one of the same pitch, in mode poly of any pitch. Notes that start together don't overlap.",
				{{"mode", enumSchema({"mono", "poly"})}}, {"mode"}, true,
				[](const Json& parameters) -> PartFunction
				{
					const OverlapMode mode =
						parameters.at("mode").get<std::string>() == "poly" ? OverlapMode::Poly : OverlapMode::Mono;
					return [=](Project&, MidiPart& part, const NoteSelection& notes)
					{ deleteOverlaps(part, notes, mode); };
				}},
			{"midi.deleteDoubles", "Delete doubles",
				"Of notes with the same pitch, channel and position, keeps the longest (the first added on a tie).",
				Json::object(), {}, true, [](const Json&) -> PartFunction
				{ return [](Project&, MidiPart& part, const NoteSelection& notes) { deleteDoubles(part, notes); }; }},
			{"midi.deleteControllers", "Delete controllers",
				"Removes control changes, pitch bend, channel and poly pressure; program changes and SysEx stay.",
				Json::object(), {}, false, [](const Json&) -> PartFunction
				{ return [](Project&, MidiPart& part, const NoteSelection&) { deleteControllers(part); }; }},
			{"midi.deleteContinuousControllers", "Delete continuous controllers",
				"Removes the control changes of continuous controllers. Bank select, data entry, the switches 64-69 "
				"(such as the sustain pedal), data increment and decrement, NRPN, RPN and channel mode messages "
				"stay.",
				Json::object(), {}, false, [](const Json&) -> PartFunction
				{ return [](Project&, MidiPart& part, const NoteSelection&) { deleteContinuousControllers(part); }; }},
			{"midi.deleteNotes", "Delete notes",
				"Deletes notes shorter than minimumLength (ticks) or softer than minimumVelocity (0-1); at least one "
				"is required. removeWhen both deletes only notes below both minimums (default: either).",
				{{"minimumLength", integerSchema}, {"minimumVelocity", numberSchema},
					{"removeWhen", enumSchema({"either", "both"})}},
				{}, true, prepareDeleteNotes},
			{"midi.restrictPolyphony", "Restrict polyphony",
				"Shortens notes so that at most voices (1 or more) sound at once on each MIDI channel: a new note ends "
				"the one that started first, the lowest of notes that started together. A note that would lose its "
				"whole length is deleted.",
				{{"voices", integerSchema}}, {"voices"}, true,
				[](const Json& parameters) -> PartFunction
				{
					const int voices = parameters.at("voices").get<int>();
					require(voices >= 1, "voices must be at least 1");
					return [=](Project&, MidiPart& part, const NoteSelection& notes)
					{ restrictPolyphony(part, notes, voices); };
				}},
			{"midi.thinOut", "Thin out data",
				"Thins out continuous controllers, pitch bend and pressure, per controller and channel: removes events "
				"that repeat the value before them, and events closer than minimumDistance ticks (default a 64th note) "
				"to the event kept before them, unless the curve turns there, the next event is at least "
				"minimumDistance away or it is the last one. Bank select, data entry, the switches 64-69, NRPN, RPN "
				"and channel mode messages stay, like program changes and SysEx.",
				{{"minimumDistance", integerSchema}}, {}, false,
				[](const Json& parameters) -> PartFunction
				{
					const Ticks distance = parameters.contains("minimumDistance")
						? positiveTicks(parameters, "minimumDistance")
						: time::g_ticksPerQuarter / 16;
					return [=](Project&, MidiPart& part, const NoteSelection&) { thinOut(part, distance); };
				}},
			{"midi.reverse", "Reverse",
				"Reverses the order in which notes start, keeping their lengths: the last one starts where the first "
				"one started. Without notes, controllers with a value that holds (continuous controllers, the "
				"switches 64-69, pitch bend and pressure) play their values backwards, each held as long as before, "
				"and return to their last value where the range ends; program changes, SysEx, bank select, data "
				"entry, NRPN, RPN and channel mode messages stay in place.",
				Json::object(), {}, true,
				[](const Json&) -> PartFunction
				{
					return [](Project& project, MidiPart& part, const NoteSelection& notes)
					{ reverse(part, notes, [&project] { return project.newId(); }); };
				}},
			{"midi.invert", "Invert",
				"Mirrors notes in time: each one starts where its mirror image ends. Without notes, controllers with a "
				"value that holds (continuous controllers, the switches 64-69, pitch bend and pressure) play their "
				"values backwards, each held as long as before, and return to their last value where the range ends; "
				"program changes, SysEx, bank select, data entry, NRPN, RPN and channel mode messages stay in place.",
				Json::object(), {}, true,
				[](const Json&) -> PartFunction
				{
					return [](Project& project, MidiPart& part, const NoteSelection& notes)
					{ invert(part, notes, [&project] { return project.newId(); }); };
				}},
		};

		for (const PartCommand& command : commands)
		{
			Json properties = command.properties;
			properties["parts"] = arraySchema(idSchema);
			if (command.notes)
				properties["notes"] = arraySchema(idSchema);
			std::vector<std::string> required = command.required;
			required.insert(required.begin(), "parts");
			std::string description = command.description;
			description += command.notes ? " Works on the MIDI parts in parts;" + std::string(selection)
										 : " Works on the MIDI parts in parts.";
			description += " Returns the parts it changed.";
			model.addEdit(engine,
				{command.name, 1, description, objectSchema(std::move(properties), required), partsResult, false},
				command.action, [prepare = command.prepare](Project& project, CommandContext& context)
				{ return runOnParts(project, context, prepare); });
		}

		const Json ids = arraySchema(idSchema);
		const Json changesResult = {{"type", "object"},
			{"properties", {{"tracks", ids}, {"parts", ids}, {"changedParts", ids}, {"removedParts", ids}}},
			{"required", {"tracks", "parts", "changedParts", "removedParts"}}};
		const std::string returns = " Returns the tracks it added, the parts it added (parts), changed and removed.";
		model.addEdit(engine,
			{"midi.merge", 1,
				"Merges the notes and controllers that start between start and end (project samples, like the "
				"locators, compared in samples as playback does) on the MIDI and instrument tracks in tracks (default: "
				"all of them) into a new part from "
				"start to end on the destination track, named name (default: the track's name). Notes keep their "
				"length, cut where their part ends. Without destination a new MIDI track is added at the end, routed "
				"to the instrument of the first track merged. eraseDestination first removes the destination's parts "
				"that lie between start and end, and the events of its other parts that start there. includeChase "
				"adds the last value of every controller and program before start at the new part's start." +
					returns,
				objectSchema({{"start", integerSchema}, {"end", integerSchema}, {"tracks", ids},
								 {"destination", idSchema}, {"eraseDestination", booleanSchema},
								 {"includeChase", booleanSchema}, {"name", {{"type", "string"}}}},
					{"start", "end"}),
				changesResult, false},
			"Merge MIDI in loop", runMerge);
		model.addEdit(engine,
			{"midi.dissolve", 1,
				"Dissolves MIDI parts by MIDI channel or by pitch (by: channel or pitch): for each channel or pitch "
				"found in the parts of a track, a new MIDI track after it, named after the track and the channel or "
				"pitch (middle C, 60, is C3), routed to the same instrument, gets the events of that channel or "
				"pitch in parts at the same place. By channel SysEx stays, by pitch everything but notes and poly "
				"pressure; parts left empty are removed. optimize shortens the new parts to the bars their events "
				"play in." +
					returns,
				objectSchema({{"parts", ids}, {"by", enumSchema({"channel", "pitch"})}, {"optimize", booleanSchema}},
					{"parts", "by"}),
				changesResult, false},
			"Dissolve part", runDissolve);
		model.addEdit(engine,
			{"midi.bounce", 1,
				"Bounces MIDI parts on one track into a single part from the first one's start to the last one's "
				"end, named after the first. Events keep their time; notes are cut where their part ended." +
					returns,
				objectSchema({{"parts", ids}}, {"parts"}), changesResult, false},
			"Bounce MIDI", runBounce);
	}
}
