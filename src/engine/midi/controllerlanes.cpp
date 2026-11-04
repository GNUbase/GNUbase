// SPDX-License-Identifier: GPL-3.0-or-later
// The controller lanes' commands (#128, reference manual pp. 1157-1188): note.apply, the controller lane context
// menu's Apply MIDI Play Probability and Apply MIDI Velocity Variance (pp. 1171, 1173), and controller.line, the line
// tool's modes on a lane (p. 1186). Like every command that changes the project, each is one undo step and changes
// nothing if it fails.
#include "engine/midi/controllerlanes.h"

#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace gnubase::engine::midi
{
	namespace
	{
		constexpr double g_pi = 3.14159265358979323846;

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		// A number from 0 to 1 that the part, the note and the purpose fix: splitmix64's mix of them.
		double chance(ObjectId part, ObjectId note, uint64_t purpose)
		{
			uint64_t x = (part * 0x9E3779B97F4A7C15ULL) ^ (note + (purpose * 0xD1B54A32D192ED03ULL));
			x ^= x >> 30U;
			x *= 0xBF58476D1CE4E5B9ULL;
			x ^= x >> 27U;
			x *= 0x94D049BB133111EBULL;
			x ^= x >> 31U;
			return static_cast<double>(x >> 11U) / 9007199254740992.0; // 2^53
		}

		MidiPart& findPart(Project& project, const Json& parameters)
		{
			const auto id = parameters.at("part").get<ObjectId>();
			const auto found = project.midiParts.find(id);
			require(found != project.midiParts.end(), "no MIDI part " + std::to_string(id));
			return found->second;
		}

		// The notes the parameters name, or all of the part's.
		std::vector<ObjectId> namedNotes(const MidiPart& part, const Json& parameters)
		{
			std::vector<ObjectId> ids;
			if (!parameters.contains("notes"))
			{
				for (const auto& [id, note] : part.notes)
					ids.push_back(id);
				return ids;
			}
			for (const Json& id : parameters.at("notes"))
			{
				require(part.notes.count(id.get<ObjectId>()) != 0, "the part has no note " + id.dump());
				ids.push_back(id.get<ObjectId>());
			}
			return ids;
		}

		Json applyToNotes(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			MidiPart& part = findPart(project, parameters);
			const std::string what = parameters.at("what").get<std::string>();
			require(
				what == "probability" || what == "velocityVariance", "what must be probability or velocityVariance");
			Json changed = Json::array();
			std::vector<ObjectId> removed;
			for (const ObjectId id : namedNotes(part, parameters))
			{
				Note& note = part.notes.at(id);
				if (what == "probability")
				{
					if (!notePlays(note, part.id))
						removed.push_back(id);
					else if (note.probability != 1.0)
					{
						note.probability = 1.0;
						changed.push_back(toJson(note));
					}
				}
				else if (note.velocityVariance != 0.0)
				{
					note.velocity = playedVelocity(note, part.id);
					note.velocityVariance = 0.0;
					changed.push_back(toJson(note));
				}
			}
			for (const ObjectId id : removed)
				part.notes.erase(id);
			if (!removed.empty())
				context.emit("note.removed", {{"part", part.id}, {"notes", removed}});
			if (!changed.empty())
				context.emit("note.changed", {{"part", part.id}, {"notes", changed}});
			return {{"notes", changed}, {"removed", removed}};
		}

		// A point of a line: a position from the part's start and a value in the lane's range.
		struct Point
		{
			time::Ticks position = 0;
			double value = 0.0;
		};

		Point pointFrom(const Json& parameters, const char* name, double minimum)
		{
			const Json& item = parameters.at(name);
			const Point point{item.at("position").get<time::Ticks>(), item.at("value").get<double>()};
			require(point.position >= 0, std::string(name) + ".position must not be negative");
			require(point.value >= minimum && point.value <= 1.0,
				std::string(name) + ".value must be " + (minimum < 0.0 ? "-1" : "0") + " to 1");
			return point;
		}

		// What controller.line draws: the value at a position, from the start value to the end value.
		struct Shape
		{
			std::string mode;
			Point from;
			Point to;
			bool reverse = false;
			time::Ticks period = 1;

			double at(time::Ticks position) const
			{
				const double range = to.value - from.value;
				if (mode == "line" || mode == "parabola")
				{
					const double t = to.position == from.position ? 0.0
																  : static_cast<double>(position - from.position) /
							static_cast<double>(to.position - from.position);
					if (mode == "line")
						return from.value + (range * t);
					return from.value + (range * (reverse ? 1.0 - ((1.0 - t) * (1.0 - t)) : t * t));
				}
				// The periodic modes go from the start value to the end value and back once a period.
				const double phase =
					static_cast<double>((position - from.position) % period) / static_cast<double>(period);
				if (mode == "sine")
					return from.value + (range * (1.0 - std::cos(2.0 * g_pi * phase)) / 2.0);
				if (mode == "triangle")
					return from.value + (range * (1.0 - std::abs((2.0 * phase) - 1.0)));
				return phase < 0.5 ? from.value : to.value; // square
			}
		};

		// Sets the field that a note lane shows on the notes in the shape's range.
		Json lineOnNotes(MidiPart& part, const std::string& lane, const Shape& shape, const Json& parameters,
			CommandContext& context)
		{
			Json changed = Json::array();
			for (const ObjectId id : namedNotes(part, parameters))
			{
				Note& note = part.notes.at(id);
				if (note.position < shape.from.position || note.position > shape.to.position)
					continue;
				const double value = shape.at(note.position);
				if (lane == "velocity")
					note.velocity = static_cast<uint16_t>(std::lround(value * g_maxVelocity));
				else if (lane == "offVelocity")
					note.releaseVelocity = static_cast<uint16_t>(std::lround(value * g_maxVelocity));
				else if (lane == "probability")
					note.probability = value;
				else
					note.velocityVariance = value;
				changed.push_back(toJson(note));
			}
			if (!changed.empty())
				context.emit("note.changed", {{"part", part.id}, {"notes", changed}});
			return {{"notes", changed}, {"controllers", Json::array()}, {"removed", Json::array()}};
		}

		// Replaces the lane's controller events in the shape's range with events every density ticks and at its end.
		Json lineOnControllers(Project& project, MidiPart& part, const Json& lane, const Shape& shape,
			time::Ticks density, CommandContext& context)
		{
			const ControllerType type = *controllerTypeFromName(lane.at("type").get<std::string>());
			const auto number = static_cast<uint8_t>(lane.value("number", 0));
			const auto channel = static_cast<uint8_t>(lane.value("channel", 0));
			const bool numbered = type == ControllerType::Control || type == ControllerType::PolyPressure;
			std::vector<ObjectId> removed;
			for (const auto& [id, controller] : part.controllers)
				if (controller.type == type && controller.channel == channel &&
					(!numbered || controller.number == number) && controller.position >= shape.from.position &&
					controller.position <= shape.to.position)
					removed.push_back(id);
			for (const ObjectId id : removed)
				part.controllers.erase(id);
			std::vector<time::Ticks> positions;
			for (time::Ticks position = shape.from.position; position < shape.to.position; position += density)
				positions.push_back(position);
			positions.push_back(shape.to.position);
			Json added = Json::array();
			for (const time::Ticks position : positions)
			{
				Controller controller;
				controller.id = project.newId();
				controller.position = position;
				controller.type = type;
				controller.channel = channel;
				const double value = shape.at(position);
				if (type == ControllerType::Program)
				{
					controller.number = static_cast<uint8_t>(std::lround(value * 127.0));
					controller.value = g_noBank;
				}
				else
				{
					controller.number = numbered ? number : 0;
					controller.value = static_cast<uint32_t>(std::llround(value * g_maxControllerValue));
				}
				part.controllers[controller.id] = controller;
				added.push_back(toJson(controller));
			}
			if (!removed.empty())
				context.emit("controller.removed", {{"part", part.id}, {"controllers", removed}});
			context.emit("controller.added", {{"part", part.id}, {"controllers", added}});
			return {{"notes", Json::array()}, {"controllers", added}, {"removed", removed}};
		}

		Json drawLine(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			MidiPart& part = findPart(project, parameters);
			const Json& lane = parameters.at("lane");
			const std::string type = lane.at("type").get<std::string>();
			require(isLaneType(type), "lane.type must be one of the lane types, not " + type);
			const auto number = lane.value("number", int64_t{0});
			const auto channel = lane.value("channel", int64_t{0});
			require(number >= 0 && number <= 127, "lane.number must be 0 to 127");
			require(channel >= 0 && channel <= 15, "lane.channel must be 0 to 15");
			const double minimum = type == "velocityVariance" ? -1.0 : 0.0;
			Shape shape{parameters.at("mode").get<std::string>(), pointFrom(parameters, "from", minimum),
				pointFrom(parameters, "to", minimum), parameters.value("reverse", false),
				parameters.value("period", project.quantize.grid.ticks())};
			require(shape.from.position <= shape.to.position, "from.position must not be after to.position");
			require(shape.period > 0, "period must be positive");
			if (isNoteLane(type))
				return lineOnNotes(part, type, shape, parameters, context);
			const time::Ticks density = parameters.value("density", project.quantize.grid.ticks());
			require(density > 0, "density must be positive");
			return lineOnControllers(project, part, lane, shape, density, context);
		}
	}

	bool isLaneType(const std::string& type)
	{
		return std::find(g_laneTypes.begin(), g_laneTypes.end(), type) != g_laneTypes.end();
	}

	bool isNoteLane(const std::string& type)
	{
		return type == "velocity" || type == "offVelocity" || type == "probability" || type == "velocityVariance";
	}

	bool notePlays(const Note& note, ObjectId part)
	{
		return note.probability >= 1.0 || chance(part, note.id, 1) < note.probability;
	}

	uint16_t playedVelocity(const Note& note, ObjectId part)
	{
		if (note.velocityVariance == 0.0)
			return note.velocity;
		const double varied = note.velocity * (1.0 + (note.velocityVariance * chance(part, note.id, 2)));
		return static_cast<uint16_t>(std::clamp<int64_t>(std::llround(varied), 0, static_cast<int64_t>(g_maxVelocity)));
	}

	void registerControllerLaneCommands(ProjectModel& model, Engine& engine)
	{
		const Json integer = {{"type", "integer"}};
		const Json ids = {{"type", "array"}, {"items", integer}};
		model.addEdit(engine,
			{"note.apply", 1,
				"Makes a MIDI part's notes play as they would (reference manual pp. 1171, 1173): what probability "
				"removes the notes that playback leaves out by their play probability and gives the others a "
				"probability of 1; velocityVariance gives notes the velocity playback varies them to and a variance of "
				"0. Playback's choices are fixed for each note, so this keeps what plays. notes: the notes, by default "
				"all of the part's. Returns the notes changed and the ids of those removed.",
				{{"type", "object"}, {"additionalProperties", false}, {"required", {"part", "what"}},
					{"properties",
						{{"part", integer}, {"notes", ids},
							{"what", {{"type", "string"}, {"enum", {"probability", "velocityVariance"}}}}}}},
				{{"type", "object"}, {"required", {"notes", "removed"}},
					{"properties", {{"notes", {{"type", "array"}}}, {"removed", ids}}}},
				false},
			"Apply note modifiers", applyToNotes);

		const Json point = {{"type", "object"}, {"additionalProperties", false}, {"required", {"position", "value"}},
			{"properties", {{"position", integer}, {"value", {{"type", "number"}}}}}};
		model.addEdit(engine,
			{"controller.line", 1,
				"Draws on a controller lane of a MIDI part as the line tool does (reference manual p. 1186). lane: "
				"type, one of velocity, offVelocity, probability and velocityVariance (properties of the notes) or "
				"controller, pitchBend, channelPressure, polyPressure and program (controller events), number (the "
				"controller, or the pitch of poly pressure) and channel 0-15, both default 0. from and to: positions "
				"from the part's start in ticks and values, 0-1 (velocityVariance -1 to 1; a program change's 0-1 is "
				"program 0-127). mode: line draws a straight line from the start to the end value; parabola a curve "
				"that starts slowly, with reverse fast; sine, triangle and square go from the start value to the end "
				"value and back once every period ticks (default the quantize grid's). On a note lane the notes "
				"starting in the range, or those of them named in notes, take the value at their position. On a "
				"controller "
				"lane the lane's events in the range go, and new ones come every density ticks (default the quantize "
				"grid's) and at the end. Returns the notes changed, the controller events added and the ids of those "
				"removed.",
				{{"type", "object"}, {"additionalProperties", false},
					{"required", {"part", "lane", "mode", "from", "to"}},
					{"properties",
						{{"part", integer},
							{"lane",
								{{"type", "object"}, {"additionalProperties", false}, {"required", {"type"}},
									{"properties",
										{{"type", {{"type", "string"}}}, {"number", integer}, {"channel", integer}}}}},
							{"mode",
								{{"type", "string"}, {"enum", {"line", "parabola", "sine", "triangle", "square"}}}},
							{"from", point}, {"to", point}, {"reverse", {{"type", "boolean"}}}, {"period", integer},
							{"density", integer}, {"notes", ids}}}},
				{{"type", "object"}, {"required", {"notes", "controllers", "removed"}},
					{"properties",
						{{"notes", {{"type", "array"}}}, {"controllers", {{"type", "array"}}}, {"removed", ids}}}},
				false},
			"Draw controller line", drawLine);
	}
}
