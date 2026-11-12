// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/midi/functioncommands.h"

#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/midi/drummaps.h"
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

		Json enumSchema(const std::vector<std::string>& values)
		{
			return {{"type", "string"}, {"enum", values}};
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
				if (part->pattern != 0) // a pattern event plays its pattern's steps, not its notes
					continue;
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
			if (settings.tracks.empty())
			{
				settings.tracks = playingTracks(project);
				require(!settings.tracks.empty(), "no MIDI or instrument track plays");
			}
			settings.destination = parameters.value("destination", ObjectId{0});
			if (settings.destination != 0)
				noteTrack(project, settings.destination, "destination");
			settings.eraseDestination = parameters.value("eraseDestination", false);
			settings.includeChase = parameters.value("includeChase", false);
			settings.includeInserts = parameters.value("includeInserts", false);
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
			const DissolveTo to =
				parameters.value("to", std::string("tracks")) == "lanes" ? DissolveTo::Lanes : DissolveTo::Tracks;
			const bool optimize = parameters.value("optimize", false);
			require(!optimize || to == DissolveTo::Tracks, "optimize works with to tracks only");
			return announce(project, dissolve(project, ids, by, optimize, to), context);
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

		Json runExtractAutomation(Project& project, CommandContext& context)
		{
			std::vector<ObjectId> ids;
			for (const MidiPart* part : partsOf(project, context.parameters()))
				ids.push_back(part->id);
			const AutomationChanges changes = extractAutomation(project, ids);
			for (const ObjectId id : changes.changedParts)
				context.emit("midiPart.changed", {{"part", toJson(project.midiParts.at(id))}});
			for (const ObjectId id : changes.addedLanes)
				context.emit("automationLane.added", {{"lane", toJson(project.automation.at(id))}});
			for (const ObjectId id : changes.changedLanes)
				context.emit("automationLane.changed", {{"lane", toJson(project.automation.at(id))}});
			return {
				{"parts", changes.changedParts}, {"lanes", changes.addedLanes}, {"changedLanes", changes.changedLanes}};
		}

		PartFunction prepareVelocity(const Json& parameters)
		{
			VelocitySettings settings;
			const std::string type = parameters.at("type").get<std::string>();
			const auto number = [&parameters](const char* key, double min, double max)
			{
				require(parameters.contains(key), std::string(key) + " is required");
				const double value = parameters.at(key).get<double>();
				require(value >= min && value <= max,
					std::string(key) + " must be " + std::to_string(static_cast<int>(min)) + " to " +
						std::to_string(static_cast<int>(max)));
				return value;
			};
			if (type == "add")
			{
				settings.type = VelocityChange::Add;
				settings.amount = number("amount", -1.0, 1.0);
			}
			else if (type == "compress")
			{
				settings.type = VelocityChange::Compress;
				settings.ratio = number("ratio", 0.0, 3.0);
			}
			else if (type == "limit")
			{
				settings.type = VelocityChange::Limit;
				settings.lower = number("lower", 0.0, 1.0);
				settings.upper = number("upper", 0.0, 1.0);
				require(settings.lower <= settings.upper, "lower must not be above upper");
			}
			else if (type == "fixed")
			{
				settings.type = VelocityChange::Fixed;
				settings.amount = number("amount", 0.0, 1.0);
			}
			else
			{
				settings.type = VelocityChange::Variance;
				settings.variance = number("variance", 0.0, 1.0);
			}
			return [settings](Project&, MidiPart& part, const NoteSelection& selection)
			{ changeVelocities(part, selection, settings); };
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

	Json objectSchema(Json properties, const std::vector<std::string>& required)
	{
		return {{"type", "object"}, {"properties", std::move(properties)}, {"required", required},
			{"additionalProperties", false}};
	}

	Json arraySchema(Json items)
	{
		return {{"type", "array"}, {"items", std::move(items)}};
	}

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

	NoteSelection selectionOf(const std::vector<MidiPart*>& parts, const Json& parameters)
	{
		if (!parameters.contains("notes"))
			return {};
		std::set<ObjectId> notes;
		for (const ObjectId id : parameters.at("notes").get<std::vector<ObjectId>>())
		{
			require(std::any_of(
						parts.begin(), parts.end(), [id](const MidiPart* part) { return part->notes.count(id) != 0; }),
				"no note " + std::to_string(id) + " in the MIDI parts");
			notes.insert(id);
		}
		return NoteSelection(std::move(notes));
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
			{"midi.velocity", "Velocity",
				"Changes the velocity of notes (0-1) by type: add adds amount (-1 to 1); compress multiplies by ratio "
				"(0 to 3), below 1 compressing the differences between velocities, above 1 expanding them; limit "
				"keeps velocities from lower to upper (0-1); fixed sets them to amount (0-1); variance lowers each "
				"note by a random part of its velocity, up to variance (0-1), the same part for a note each time. "
				"Velocities stay within 0-1.",
				{{"type", enumSchema({"add", "compress", "limit", "fixed", "variance"})}, {"amount", numberSchema},
					{"ratio", numberSchema}, {"lower", numberSchema}, {"upper", numberSchema},
					{"variance", numberSchema}},
				{"type"}, true, prepareVelocity},
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
			{"midi.oNoteConversion", "O-Note conversion",
				"O-Note Conversion (reference manual p. 1240): gives the notes of a track with a drum map their "
				"sound's O-note as their pitch (drumMap.set), so that they play the same sounds without the map. "
				"Notes of tracks without one stay.",
				Json::object(), {}, true,
				[](const Json&) -> PartFunction
				{
					return [](Project& project, MidiPart& part, const NoteSelection& notes)
					{
						if (const DrumMap* map = drumMapOf(project, part.track))
							for (auto& entry : part.notes)
								if (notes.contains(entry.first))
									entry.second.pitch = map->sounds.at(entry.second.pitch).oNote;
					};
				}},
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
				"and return to their last value where the range ends, or on the part's last tick if the range "
				"reaches its end; program changes, SysEx, bank select, data entry, NRPN, RPN and channel mode "
				"messages stay in place.",
				Json::object(), {}, true,
				[](const Json&) -> PartFunction
				{
					return [](Project& project, MidiPart& part, const NoteSelection& notes)
					{ reverse(part, notes, lengthInTicks(project, part), [&project] { return project.newId(); }); };
				}},
			{"midi.invert", "Invert",
				"Mirrors notes in time: each one starts where its mirror image ends. Without notes, controllers with a "
				"value that holds (continuous controllers, the switches 64-69, pitch bend and pressure) play their "
				"values backwards, each held as long as before, and return to their last value where the range ends, "
				"or on the part's last tick if the range reaches its end; program changes, SysEx, bank select, data "
				"entry, NRPN, RPN and channel mode messages stay in place.",
				Json::object(), {}, true,
				[](const Json&) -> PartFunction
				{
					return [](Project& project, MidiPart& part, const NoteSelection& notes)
					{ invert(part, notes, lengthInTicks(project, part), [&project] { return project.newId(); }); };
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
			description += " Pattern events (pattern.add), which play their pattern's steps, stay as they are.";
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
				"locators, compared in samples as playback does) on the MIDI and instrument tracks in tracks into a "
				"new part on the destination track from start, or from the first merged event if that is earlier, to "
				"end, named "
				"name (default: the track's name). Without tracks, the MIDI and instrument tracks that play, by "
				"playback's solo and mute rules: neither their channel nor one it plays into is muted, silenced by a "
				"solo or on a disabled track (sends aren't followed); it refuses if none does. Muted parts are left "
				"out. Notes keep their "
				"length, cut where their part ends, and get their parts' transpose and velocity as playback applies "
				"them, after their tracks' drum maps and the chord track, so they are written as played; a destination "
				"with a drum map or following the chord track gets a pitch that plays them, or if none does what "
				"played, "
				"which it then maps. includeInserts also plays their tracks' MIDI modifiers into them (channel.set "
				"modifiers, with "
				"the random values playback draws), leaving out notes they filter out or move out of the range. "
				"Without destination a new MIDI track is added at the end, routed "
				"to the instrument of the first track merged. eraseDestination first removes the destination's parts "
				"that lie between start and end, and the events of its other parts that start there, which leave "
				"their shared copies for it: their notes get new ids, so play probability, velocity variance and "
				"random MIDI modifier values draw anew. includeChase "
				"adds the last value of every controller and program before start at the new part's start. Either way "
				"the new part starts with the directions of the tracks' articulation maps in effect at start, so that "
				"its notes play the same sound slots." +
					returns,
				objectSchema(
					{{"start", integerSchema}, {"end", integerSchema}, {"tracks", ids}, {"destination", idSchema},
						{"eraseDestination", booleanSchema}, {"includeChase", booleanSchema},
						{"includeInserts", booleanSchema}, {"name", {{"type", "string"}}}},
					{"start", "end"}),
				changesResult, false},
			"Merge MIDI in loop", runMerge);
		model.addEdit(engine,
			{"midi.dissolve", 1,
				"Dissolves MIDI parts by MIDI channel or by pitch (by: channel or pitch): for each channel or pitch "
				"found in the parts of a track, the events of that channel or pitch are copied into parts at the same "
				"place, and the parts are muted. to tracks (the default) puts them on a new MIDI track after the "
				"track, named after it and the channel or pitch (middle C, 60, is C3) and routed to the same "
				"instrument; to lanes on a new lane of the track, below the lanes its parts are on, named after the "
				"part and the channel or pitch. Events of no channel or pitch (SysEx by channel, everything but notes "
				"and poly pressure by pitch) go with the first new part of their part; a part with nothing to "
				"separate stays as it is. New parts keep the part's mute, transpose and velocity; their events get new "
				"ids, so play probability and velocity variance draw anew. optimize (to tracks only) shortens the new "
				"parts to the bars their events play in." +
					returns,
				objectSchema({{"parts", ids}, {"by", enumSchema({"channel", "pitch"})}, {"optimize", booleanSchema},
								 {"to", enumSchema({"tracks", "lanes"})}},
					{"parts", "by"}),
				changesResult, false},
			"Dissolve part", runDissolve);
		model.addEdit(engine,
			{"midi.bounce", 1,
				"Bounces MIDI parts on one track into a single part from the first one's start to the last one's "
				"end, named after the first, on the lowest lane they are on. Muted parts are removed and add nothing. "
				"Events keep their time; notes are cut where their part ended. The parts' transpose and velocity go "
				"into the notes as playback applies them; a note transposed beyond 0-127, which doesn't play, is left "
				"out. On a track whose drum map or chord track following plays a transposed note differently, the new "
				"part keeps the transpose, and parts with different ones make a part for each. The copied notes "
				"get new ids, so play probability and velocity variance draw anew." +
					returns,
				objectSchema({{"parts", ids}}, {"parts"}), changesResult, false},
			"Bounce MIDI", runBounce);
		model.addEdit(engine,
			{"midi.extractAutomation", 1,
				"Moves the continuous controllers of MIDI parts (as midi.deleteContinuousControllers takes them) into "
				"controller automation lanes of their tracks, one per controller and MIDI channel: each event becomes "
				"a step point where it plays, its value 0 to 1. A lane the track already has loses its points in the "
				"part's range first, except those an earlier part of the call added, and reads from then on. A shared "
				"copy leaves its copies, which keep their controllers; all its events get new ids, so the play "
				"probability, velocity variance and random MIDI modifier values of its notes draw anew. Pitch bend, "
				"pressure, switches "
				"such as the sustain pedal, events past a part's "
				"end and muted parts stay. Returns the parts it changed, the lanes it added (lanes) and the lanes it "
				"changed.",
				objectSchema({{"parts", ids}}, {"parts"}),
				{{"type", "object"}, {"properties", {{"parts", ids}, {"lanes", ids}, {"changedLanes", ids}}},
					{"required", {"parts", "lanes", "changedLanes"}}},
				false},
			"Extract MIDI automation", runExtractAutomation);
	}
}
