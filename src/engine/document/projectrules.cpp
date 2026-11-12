// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/projectrules.h"

#include "engine/document/projectmodel.h"
#include "engine/engine.h"
#include "engine/rulepresets.h"
#include "engine/settings/store.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace gnubase::engine
{
	namespace
	{
		constexpr std::string_view g_presetsKey = "projectRules.presets";
		constexpr double g_logicalTick = static_cast<double>(time::g_ticksPerQuarter) / midi::g_logicalTicksPerQuarter;

		constexpr const char* g_targets[] = {
			"mediaType", "containerType", "name", "position", "length", "colorName", "property"};
		constexpr const char* g_conditions[] = {"equal", "unequal", "bigger", "biggerOrEqual", "less", "lessOrEqual",
			"insideRange", "outsideRange", "allTypes", "propertyIsSet", "propertyIsNotSet", "contains", "containsNot"};
		constexpr const char* g_joins[] = {"and", "or"};
		constexpr const char* g_functions[] = {"delete", "transform", "select", "deselect"};
		constexpr const char* g_actionTargets[] = {"position", "length", "trackOperation", "name", "color"};
		constexpr const char* g_operations[] = {"add", "subtract", "multiplyBy", "divideBy", "roundBy", "setRandom",
			"setRelativeRandom", "setFixed", "replace", "append", "prepend", "generateName", "replaceSearchString",
			"eraseBefore", "eraseAfter", "eraseFrontCharacter", "eraseEndCharacter", "mute", "solo", "record",
			"monitor", "timeDomain", "increment", "decrement"};
		constexpr const char* g_mediaTypes[] = {"audio", "midi", "marker", "tempo", "signature", "arranger",
			"transpose", "chord", "group", "effect", "vca"};
		constexpr const char* g_containerTypes[] = {"folder", "track", "part", "event"};
		constexpr const char* g_properties[] = {"isMuted", "isSelected", "isEmpty", "isHidden", "hasTrackVersion",
			"followsChordTrack", "isDisabled", "parentIsSelected"};
		constexpr const char* g_switches[] = {"on", "off", "toggle"};
		constexpr const char* g_timeDomains[] = {"musical", "linear", "toggle"};

		template <size_t N> std::vector<std::string> names(const char* const (&list)[N])
		{
			return {std::begin(list), std::end(list)};
		}

		template <size_t N> int indexOf(const char* const (&list)[N], const std::string& name, const std::string& what)
		{
			for (size_t i = 0; i < N; ++i)
				if (name == list[i])
					return static_cast<int>(i);
			throw std::invalid_argument(what + " " + name + " is unknown");
		}

		template <size_t N> bool isOneOf(const char* const (&list)[N], const std::string& name)
		{
			return std::any_of(std::begin(list), std::end(list), [&name](const char* item) { return name == item; });
		}

		void requireObject(const Json& item, const std::string& name)
		{
			if (!item.is_object())
				throw std::invalid_argument(name + " must be an object");
		}

		double number(const Json& item, const char* key, const std::string& name)
		{
			const auto found = item.find(key);
			if (found == item.end())
				return 0.0;
			if (!found->is_number())
				throw std::invalid_argument(name + "." + key + " must be a number");
			const double value = found->get<double>();
			if (!std::isfinite(value) || std::abs(value) > 1e12)
				throw std::invalid_argument(name + "." + key + " is out of range");
			return value;
		}

		std::string text(const Json& item, const char* key, const std::string& name, const char* fallback = "")
		{
			const auto found = item.find(key);
			if (found == item.end())
				return fallback;
			if (!found->is_string())
				throw std::invalid_argument(name + "." + key + " must be a string");
			return found->get<std::string>();
		}

		int brackets(const Json& item, const char* key, const std::string& name)
		{
			const double value = number(item, key, name);
			if (value != std::floor(value) || value < 0 || value > 3)
				throw std::invalid_argument(name + "." + key + " must be 0 to 3");
			return static_cast<int>(value);
		}

		bool isTextTarget(ProjectTarget target)
		{
			return target != ProjectTarget::Position && target != ProjectTarget::Length;
		}

		ProjectFilterLine filterFromJson(const Json& item, const std::string& name)
		{
			using C = ProjectCondition;
			using T = ProjectTarget;
			requireObject(item, name);
			ProjectFilterLine line;
			line.open = brackets(item, "open", name);
			line.close = brackets(item, "close", name);
			line.target = static_cast<T>(indexOf(g_targets, text(item, "target", name), "target"));
			line.condition = static_cast<C>(indexOf(g_conditions, text(item, "condition", name), "condition"));
			line.join = static_cast<midi::LogicalBool>(indexOf(g_joins, text(item, "join", name, "and"), "join"));
			const auto allow = [&](std::initializer_list<C> conditions)
			{
				if (std::find(conditions.begin(), conditions.end(), line.condition) == conditions.end())
					throw std::invalid_argument(name + ": " + g_targets[static_cast<size_t>(line.target)] +
						" has no condition " + g_conditions[static_cast<size_t>(line.condition)]);
			};
			if (isTextTarget(line.target))
			{
				line.text = text(item, "parameter1", name);
			}
			else
			{
				line.parameter1 = number(item, "parameter1", name);
				line.parameter2 = number(item, "parameter2", name);
			}
			switch (line.target)
			{
			case T::MediaType:
			case T::ContainerType:
				allow({C::Equal, C::Unequal, C::AllTypes});
				if (line.condition != C::AllTypes &&
					!(line.target == T::MediaType ? isOneOf(g_mediaTypes, line.text)
												  : isOneOf(g_containerTypes, line.text)))
					throw std::invalid_argument(name + ": " + line.text + " is no " +
						(line.target == T::MediaType ? "media type" : "container type"));
				break;
			case T::Name:
			case T::ColorName:
				allow({C::Equal, C::Unequal, C::Contains, C::ContainsNot});
				break;
			case T::Position:
			case T::Length:
				allow({C::Equal, C::Unequal, C::Bigger, C::BiggerOrEqual, C::Less, C::LessOrEqual, C::InsideRange,
					C::OutsideRange});
				break;
			case T::Property:
				allow({C::PropertyIsSet, C::PropertyIsNotSet});
				if (!isOneOf(g_properties, line.text))
					throw std::invalid_argument(name + ": " + line.text + " is no property");
				break;
			}
			return line;
		}

		ProjectActionLine actionFromJson(const Json& item, const std::string& name)
		{
			using O = ProjectOperation;
			using T = ProjectActionTarget;
			requireObject(item, name);
			ProjectActionLine action;
			action.target = static_cast<T>(indexOf(g_actionTargets, text(item, "target", name), "target"));
			action.operation = static_cast<O>(indexOf(g_operations, text(item, "operation", name), "operation"));
			const auto in = [&action](O first, O last)
			{ return action.operation >= first && action.operation <= last; };
			bool fits = false;
			switch (action.target)
			{
			case T::Position:
			case T::Length:
				fits = in(O::Add, O::SetFixed);
				action.parameter1 = number(item, "parameter1", name);
				action.parameter2 = number(item, "parameter2", name);
				break;
			case T::Name:
				fits = in(O::Replace, O::EraseEndCharacter);
				action.text = text(item, "parameter1", name);
				if (action.operation == O::GenerateName)
					action.parameter2 = number(item, "parameter2", name);
				else if (action.operation == O::ReplaceSearchString)
					action.text2 = text(item, "parameter2", name);
				break;
			case T::TrackOperation:
				fits = in(O::Mute, O::TimeDomain);
				action.text = text(item, "parameter1", name, action.operation == O::TimeDomain ? "toggle" : "on");
				if (action.operation == O::TimeDomain ? !isOneOf(g_timeDomains, action.text)
													  : !isOneOf(g_switches, action.text))
					throw std::invalid_argument(name + ".parameter1 must be " +
						(action.operation == O::TimeDomain ? "musical, linear or toggle" : "on, off or toggle"));
				break;
			case T::Color:
				fits = action.operation == O::SetFixed || in(O::Increment, O::Decrement);
				if (action.operation == O::SetFixed)
				{
					action.text = text(item, "parameter1", name);
					if (action.text.empty())
						throw std::invalid_argument(name + ".parameter1 must name a color");
				}
				break;
			}
			if (!fits)
				throw std::invalid_argument(name + ": " + g_actionTargets[static_cast<size_t>(action.target)] +
					" has no operation " + g_operations[static_cast<size_t>(action.operation)]);
			return action;
		}

		std::vector<ProjectRuleCommand> commandsFromJson(const Json& json, const std::string& name)
		{
			std::vector<ProjectRuleCommand> commands;
			if (json.size() > g_maxRuleCommands)
				throw std::invalid_argument(name + " has at most 4 commands");
			for (size_t i = 0; i < json.size(); ++i)
			{
				const std::string where = name + "[" + std::to_string(i) + "]";
				requireObject(json[i], where);
				ProjectRuleCommand command{
					text(json[i], "command", where), json[i].value("parameters", Json::object())};
				if (command.command.empty())
					throw std::invalid_argument(where + ".command must name a key command");
				commands.push_back(std::move(command));
			}
			return commands;
		}

		Json filterJson(const ProjectFilterLine& line)
		{
			Json p1 = isTextTarget(line.target) ? Json(line.text) : Json(line.parameter1);
			return {{"open", line.open}, {"target", g_targets[static_cast<size_t>(line.target)]},
				{"condition", g_conditions[static_cast<size_t>(line.condition)]}, {"parameter1", std::move(p1)},
				{"parameter2", line.parameter2}, {"close", line.close},
				{"join", g_joins[static_cast<size_t>(line.join)]}};
		}

		Json actionJson(const ProjectActionLine& action)
		{
			const bool numbers =
				action.target == ProjectActionTarget::Position || action.target == ProjectActionTarget::Length;
			Json p2 = action.operation == ProjectOperation::ReplaceSearchString ? Json(action.text2)
																				: Json(action.parameter2);
			return {{"target", g_actionTargets[static_cast<size_t>(action.target)]},
				{"operation", g_operations[static_cast<size_t>(action.operation)]},
				{"parameter1", numbers ? Json(action.parameter1) : Json(action.text)}, {"parameter2", std::move(p2)}};
		}

		Json commandsJson(const std::vector<ProjectRuleCommand>& commands)
		{
			Json json = Json::array();
			for (const ProjectRuleCommand& command : commands)
				json.push_back({{"command", command.command}, {"parameters", command.parameters}});
			return json;
		}

		// Builders for the factory presets.
		ProjectFilterLine where(ProjectTarget target, ProjectCondition condition, std::string text,
			midi::LogicalBool join = midi::LogicalBool::And, int open = 0, int close = 0)
		{
			return {open, target, condition, std::move(text), 0.0, 0.0, close, join};
		}

		ProjectFilterLine is(
			const char* container, midi::LogicalBool join = midi::LogicalBool::And, int open = 0, int close = 0)
		{
			return where(ProjectTarget::ContainerType, ProjectCondition::Equal, container, join, open, close);
		}

		ProjectFilterLine has(const char* property)
		{
			return where(ProjectTarget::Property, ProjectCondition::PropertyIsSet, property);
		}

		ProjectActionLine change(ProjectActionTarget target, ProjectOperation operation, std::string text = {},
			double p1 = 0.0, double p2 = 0.0)
		{
			return {target, operation, std::move(text), {}, p1, p2};
		}

		// What the rules see of a track, part or event.
		enum class Kind : uint8_t
		{
			Track,
			AudioPart,
			MidiPart,
			AudioEvent,
			Marker,
		};

		struct Element
		{
			Kind kind = Kind::Track;
			ObjectId id = 0;
			const Track* track = nullptr; // its own, or the one it is on
			std::string mediaType;        // empty for none
			std::string container;
			std::string name;
			std::string color; // its own, else its track's
			bool timed = false;
			double position = 0.0; // logical ticks
			double length = 0.0;
			bool locked = false;
			std::set<std::string> properties;
		};

		double logicalTicks(const Project& project, const time::Position& position)
		{
			return static_cast<double>(time::toTicks(position, project.tempo, project.sampleRate)) / g_logicalTick;
		}

		std::string mediaTypeOf(TrackType type)
		{
			switch (type)
			{
			case TrackType::Audio:
				return "audio";
			case TrackType::Instrument:
			case TrackType::Midi:
				return "midi";
			case TrackType::Marker:
				return "marker";
			case TrackType::Tempo:
				return "tempo";
			case TrackType::Signature:
				return "signature";
			case TrackType::Arranger:
				return "arranger";
			case TrackType::Transpose:
				return "transpose";
			case TrackType::Chord:
				return "chord";
			case TrackType::Group:
				return "group";
			case TrackType::Effect:
				return "effect";
			case TrackType::Vca:
				return "vca";
			case TrackType::Folder:
			case TrackType::Ruler:
				break;
			}
			return {};
		}

		// The project's elements: tracks from top to bottom, then audio and MIDI parts, audio events and markers, each
		// by id.
		std::vector<Element> elementsOf(const Project& project, const std::set<ObjectId>& selected)
		{
			std::vector<Element> elements;
			const auto onTrack = [&](Element element, ObjectId track, const std::string& color)
			{
				element.track = &project.trackById.at(track);
				element.color = color.empty() ? element.track->color : color;
				if (selected.count(element.id) != 0)
					element.properties.insert("isSelected");
				if (selected.count(track) != 0)
					element.properties.insert("parentIsSelected");
				elements.push_back(std::move(element));
			};
			const auto has = [](const auto& map, ObjectId track)
			{
				return std::any_of(
					map.begin(), map.end(), [track](const auto& entry) { return entry.second.track == track; });
			};
			const auto addTrack = [&](const auto& self, ObjectId id) -> void
			{
				const Track& track = project.trackById.at(id);
				Element element;
				element.id = id;
				element.name = track.name;
				element.mediaType = mediaTypeOf(track.type);
				element.container = track.type == TrackType::Folder ? "folder" : "track";
				std::set<std::string>& p = element.properties;
				const auto channel = project.channels.find(track.channel);
				if (channel != project.channels.end() && channel->second.mute)
					p.insert("isMuted");
				bool empty = false;
				switch (track.type)
				{
				case TrackType::Audio:
					empty = !has(project.audioEvents, id) && !has(project.audioParts, id);
					break;
				case TrackType::Instrument:
				case TrackType::Midi:
					empty = !has(project.midiParts, id);
					break;
				case TrackType::Marker:
					empty = !has(project.markers, id);
					break;
				case TrackType::Folder:
					empty = track.children.empty();
					break;
				default:
					break;
				}
				if (empty)
					p.insert("isEmpty");
				const auto& hidden = project.trackVisibility.hidden;
				if (std::find(hidden.begin(), hidden.end(), id) != hidden.end())
					p.insert("isHidden");
				if (!track.versions.empty())
					p.insert("hasTrackVersion");
				if (const auto follow = project.chords.follow.find(id);
					follow != project.chords.follow.end() && follow->second.mode != "off")
					p.insert("followsChordTrack");
				if (!track.enabled)
					p.insert("isDisabled");
				onTrack(std::move(element), id, {});
				if (track.parent != 0 && selected.count(track.parent) != 0)
					elements.back().properties.insert("parentIsSelected");
				else
					elements.back().properties.erase("parentIsSelected"); // a track isn't its own parent
				for (const ObjectId child : track.children)
					self(self, child);
			};
			for (const ObjectId id : project.tracks)
				addTrack(addTrack, id);

			const auto part = [&](Kind kind, const auto& item, bool empty)
			{
				Element element;
				element.kind = kind;
				element.id = item.id;
				element.name = item.name;
				element.mediaType = kind == Kind::AudioPart ? "audio" : "midi";
				element.container = "part";
				element.timed = true;
				element.position = logicalTicks(project, item.position);
				element.length =
					logicalTicks(project, {item.position.base, item.position.value + item.length}) - element.position;
				element.locked = item.lock;
				if (item.mute)
					element.properties.insert("isMuted");
				if (empty)
					element.properties.insert("isEmpty");
				onTrack(std::move(element), item.track, item.color);
			};
			for (const auto& [id, item] : project.audioParts)
				part(Kind::AudioPart, item,
					std::none_of(project.audioEvents.begin(), project.audioEvents.end(),
						[id = id](const auto& entry) { return entry.second.part == id; }));
			for (const auto& [id, item] : project.midiParts)
				part(Kind::MidiPart, item, item.notes.empty() && item.controllers.empty());
			for (const auto& [id, event] : project.audioEvents)
			{
				Element element;
				element.kind = Kind::AudioEvent;
				element.id = id;
				element.name = project.clips.at(event.clip).name;
				element.mediaType = "audio";
				element.container = "event";
				element.timed = true;
				const EventSpan span = eventSpan(project, event);
				element.position =
					static_cast<double>(project.tempo.toTicks(span.start, project.sampleRate)) / g_logicalTick;
				element.length =
					(static_cast<double>(project.tempo.toTicks(span.end, project.sampleRate)) / g_logicalTick) -
					element.position;
				element.locked = event.lock;
				if (event.mute)
					element.properties.insert("isMuted");
				onTrack(std::move(element), event.track, event.color);
				if (event.part != 0 && selected.count(event.part) != 0)
					elements.back().properties.insert("parentIsSelected");
			}
			for (const auto& [id, marker] : project.markers)
			{
				Element element;
				element.kind = Kind::Marker;
				element.id = id;
				element.name = marker.name;
				element.mediaType = "marker";
				element.container = "event";
				element.timed = true;
				element.position = logicalTicks(project, marker.position);
				element.length = logicalTicks(project, marker.end) - element.position;
				onTrack(std::move(element), marker.track, marker.color);
			}
			return elements;
		}

		std::string colorName(const Project& project, const std::string& color)
		{
			for (const ProjectColor& each : project.colors)
				if (each.color == color)
					return each.name;
			return {};
		}

		bool contains(const std::string& text, const std::string& part)
		{
			return text.find(part) != std::string::npos;
		}

		bool test(const Project& project, const ProjectFilterLine& line, const Element& element)
		{
			using C = ProjectCondition;
			const auto compare = [&line](const std::string& value)
			{
				switch (line.condition)
				{
				case C::Equal:
					return value == line.text;
				case C::Unequal:
					return value != line.text;
				case C::Contains:
					return contains(value, line.text);
				case C::ContainsNot:
					return !contains(value, line.text);
				case C::AllTypes:
					return !value.empty();
				default:
					return false;
				}
			};
			const auto numeric = [&line](double value)
			{
				const double p1 = line.parameter1;
				const double p2 = line.parameter2;
				switch (line.condition)
				{
				case C::Equal:
					return value == p1;
				case C::Unequal:
					return value != p1;
				case C::Bigger:
					return value > p1;
				case C::BiggerOrEqual:
					return value >= p1;
				case C::Less:
					return value < p1;
				case C::LessOrEqual:
					return value <= p1;
				case C::InsideRange:
					return value >= p1 && value <= p2;
				case C::OutsideRange:
					return value < p1 || value > p2;
				default:
					return false;
				}
			};
			switch (line.target)
			{
			case ProjectTarget::MediaType:
				return compare(element.mediaType);
			case ProjectTarget::ContainerType:
				return compare(element.container);
			case ProjectTarget::Name:
				return compare(element.name);
			case ProjectTarget::ColorName:
				return compare(colorName(project, element.color));
			case ProjectTarget::Position:
				return element.timed && numeric(std::round(element.position));
			case ProjectTarget::Length:
				return element.timed && numeric(std::round(element.length));
			case ProjectTarget::Property:
				return (element.properties.count(line.text) != 0) == (line.condition == C::PropertyIsSet);
			}
			return false;
		}

		double apply(const ProjectActionLine& action, double value, midi::LogicalRandom& random)
		{
			const double p1 = action.parameter1;
			const auto whole = [](double v) { return static_cast<int>(std::lround(std::clamp(v, -1e9, 1e9))); };
			switch (action.operation)
			{
			case ProjectOperation::Add:
				return value + p1;
			case ProjectOperation::Subtract:
				return value - p1;
			case ProjectOperation::MultiplyBy:
				return value * p1;
			case ProjectOperation::DivideBy:
				return p1 != 0.0 ? value / p1 : value;
			case ProjectOperation::RoundBy:
				return p1 != 0.0 ? std::round(value / p1) * p1 : value;
			case ProjectOperation::SetRandom:
				return random.between(whole(p1), whole(action.parameter2));
			case ProjectOperation::SetRelativeRandom:
				return value + random.between(whole(p1), whole(action.parameter2));
			case ProjectOperation::SetFixed:
				return p1;
			default:
				return value;
			}
		}

		std::string rename(const ProjectActionLine& action, std::string name, int64_t& counter)
		{
			const std::string& t = action.text;
			switch (action.operation)
			{
			case ProjectOperation::Replace:
				return t;
			case ProjectOperation::Append:
				return name + t;
			case ProjectOperation::Prepend:
				return t + name;
			case ProjectOperation::GenerateName:
				return t + std::to_string(counter++);
			case ProjectOperation::ReplaceSearchString:
				if (!t.empty())
					for (size_t at = name.find(t); at != std::string::npos; at = name.find(t, at + action.text2.size()))
						name.replace(at, t.size(), action.text2);
				return name;
			case ProjectOperation::EraseBefore:
				if (const size_t at = name.find(t); !t.empty() && at != std::string::npos)
					name.erase(0, at);
				return name;
			case ProjectOperation::EraseAfter:
				if (const size_t at = name.find(t); !t.empty() && at != std::string::npos)
					name.erase(at + t.size());
				return name;
			case ProjectOperation::EraseFrontCharacter:
				return name.empty() ? name : name.substr(1);
			case ProjectOperation::EraseEndCharacter:
				if (!name.empty())
					name.pop_back();
				return name;
			default:
				return name;
			}
		}

		std::string recolor(const Project& project, const ProjectActionLine& action, const std::string& color)
		{
			if (action.operation == ProjectOperation::SetFixed)
			{
				for (const ProjectColor& each : project.colors)
					if (each.name == action.text)
						return each.color;
				if (!isColor(action.text))
					throw CommandError("no project color " + action.text);
				return action.text;
			}
			const auto& colors = project.colors;
			const auto at = std::find_if(
				colors.begin(), colors.end(), [&color](const ProjectColor& each) { return each.color == color; });
			const auto count = static_cast<int64_t>(colors.size());
			int64_t index = at == colors.end() ? -1 : at - colors.begin();
			if (at == colors.end())
				index = action.operation == ProjectOperation::Increment ? -1 : 0;
			index += action.operation == ProjectOperation::Increment ? 1 : -1;
			return colors[static_cast<size_t>(((index % count) + count) % count)].color;
		}

		struct Found
		{
			std::vector<const Element*> elements;
			Json json() const
			{
				Json tracks = Json::array();
				Json events = Json::array();
				Json markers = Json::array();
				for (const Element* element : elements)
				{
					if (element->kind == Kind::Track)
						tracks.push_back(element->id);
					else if (element->kind == Kind::Marker)
						markers.push_back(element->id);
					else
						events.push_back(element->id);
				}
				return {{"tracks", std::move(tracks)}, {"events", std::move(events)}, {"markers", std::move(markers)}};
			}
		};

		Found find(const Project& project, const ProjectRules& rules, const std::vector<Element>& elements)
		{
			Found found;
			for (const Element& element : elements)
				if (midi::evaluateLines(rules.filters.data(), rules.filters.size(),
						[&](const ProjectFilterLine& line) { return test(project, line, element); }))
					found.elements.push_back(&element);
			return found;
		}

		using Commands = std::vector<std::pair<std::string, Json>>;

		bool hasEvents(TrackType type)
		{
			return type == TrackType::Audio || type == TrackType::Instrument || type == TrackType::Midi;
		}

		// The commands that delete what was found: events and markers on tracks that stay, then the tracks that no
		// folder found takes with it.
		Commands deleting(const Project& project, const Found& found)
		{
			// An audio part goes with all its events, so one that holds a locked event stays.
			std::set<ObjectId> holdingLocked;
			for (const auto& [id, event] : project.audioEvents)
				if (event.lock)
					holdingLocked.insert(event.part);
			std::set<ObjectId> tracks;
			std::set<ObjectId> parts;
			for (const Element* element : found.elements)
				if (element->kind == Kind::Track)
					tracks.insert(element->id);
				else if (element->kind == Kind::AudioPart && !element->locked && holdingLocked.count(element->id) == 0)
					parts.insert(element->id);
			const auto removed = [&](ObjectId track)
			{
				for (ObjectId at = track; at != 0; at = project.trackById.at(at).parent)
					if (tracks.count(at) != 0)
						return true;
				return false;
			};
			Json events = Json::array();
			Json markers = Json::array();
			for (const Element* element : found.elements)
			{
				if (element->kind == Kind::Track || element->locked || removed(element->track->id))
					continue;
				if (element->kind == Kind::Marker)
					markers.push_back(element->id);
				else if (element->kind == Kind::MidiPart ||
					(element->kind == Kind::AudioPart && parts.count(element->id) != 0) ||
					(element->kind == Kind::AudioEvent && parts.count(project.audioEvents.at(element->id).part) == 0))
					events.push_back(element->id);
			}
			Commands commands;
			if (!events.empty())
				commands.emplace_back("event.remove", Json{{"events", std::move(events)}});
			if (!markers.empty())
				commands.emplace_back("marker.remove", Json{{"markers", std::move(markers)}});
			std::vector<ObjectId> top;
			for (const ObjectId track : tracks)
				if (project.trackById.at(track).parent == 0 || !removed(project.trackById.at(track).parent))
					top.push_back(track);
			// The active marker track goes last, as in track.removeEmpty, so that track.remove activates a marker track
			// that stays rather than one that goes next. Moving it to the end of the top tracks is enough: marker
			// tracks can't be inside a folder, so no folder that goes takes the active marker track along before the
			// other marker tracks.
			const auto active = std::find(top.begin(), top.end(), project.activeMarkerTrack);
			if (active != top.end())
				std::rotate(active, std::next(active), top.end());
			for (const ObjectId track : top)
				commands.emplace_back("track.remove", Json{{"track", track}});
			return commands;
		}

		bool switched(const std::string& how, bool now)
		{
			return how == "toggle" ? !now : how == "on";
		}

		// The commands that change what was found by the actions.
		Commands transforming(const Project& project, const ProjectRules& rules, const Found& found, uint64_t seed)
		{
			using O = ProjectOperation;
			using T = ProjectActionTarget;
			midi::LogicalRandom random{seed};
			std::vector<int64_t> counters;
			counters.reserve(rules.actions.size());
			for (const ProjectActionLine& action : rules.actions)
				counters.push_back(std::llround(action.parameter2));
			// An audio event in a part that moves too goes with it (place in eventedits.cpp) and keeps its place in it.
			std::set<ObjectId> parts;
			for (const Element* element : found.elements)
				if (element->kind == Kind::AudioPart && !element->locked)
					parts.insert(element->id);
			// Each action's result stops at the longest project, so that values stay finite and fit the ticks they
			// become. They may go below the project's start between two actions.
			const double longest = static_cast<double>(longestProjectTicks(project)) / g_logicalTick;
			const auto bounded = [longest](double value) { return std::clamp(value, -longest, longest); };
			Commands commands;
			for (const Element* element : found.elements)
			{
				double position = element->position;
				double length = element->length;
				std::string name = element->name;
				// Locked parts and events stay as they are, and audio events keep their clip's name.
				const bool nameable = !element->locked && element->kind != Kind::AudioEvent;
				std::string color = element->color;
				Json channel = Json::object();
				Json track = Json::object();
				const bool isTrack = element->kind == Kind::Track;
				const Channel* own = nullptr;
				if (isTrack)
					if (const auto at = project.channels.find(element->track->channel); at != project.channels.end())
						own = &at->second;
				for (size_t i = 0; i < rules.actions.size(); ++i)
				{
					const ProjectActionLine& action = rules.actions[i];
					switch (action.target)
					{
					case T::Position:
						position = bounded(apply(action, position, random));
						break;
					case T::Length:
						length = bounded(apply(action, length, random));
						break;
					case T::Name:
						if (nameable)
							name = rename(action, name, counters[i]);
						break;
					case T::Color:
						color = recolor(project, action, color);
						break;
					case T::TrackOperation:
						if (!isTrack)
							break;
						if (action.operation == O::TimeDomain)
						{
							if (!hasEvents(element->track->type) && element->track->type != TrackType::Marker)
								break;
							const bool linear = track.contains("timeBase")
								? track.at("timeBase") == "linear"
								: element->track->timeBase == time::TimeBase::Linear;
							const bool toLinear = action.text == "toggle" ? !linear : action.text == "linear";
							track["timeBase"] = toLinear ? "linear" : "musical";
							break;
						}
						if (own == nullptr ||
							((action.operation == O::Record || action.operation == O::Monitor) &&
								!hasEvents(element->track->type)))
							break;
						{
							// The channel's button and its key in channel.set.
							const std::array<std::pair<const char*, bool>, 4> buttons{{{"mute", own->mute},
								{"solo", own->solo}, {"recordEnable", own->recordEnable}, {"monitor", own->monitor}}};
							const auto& [key, current] =
								buttons[static_cast<size_t>(action.operation) - static_cast<size_t>(O::Mute)];
							const bool now = channel.contains(key) ? channel.at(key).get<bool>() : current;
							channel[key] = switched(action.text, now);
						}
						break;
					}
				}
				const bool renamed = name != element->name;
				const bool recolored = color != element->color;
				switch (element->kind)
				{
				case Kind::Track:
					if (renamed)
						track["name"] = name;
					if (recolored)
						track["color"] = color;
					if (!track.empty())
					{
						track["track"] = element->id;
						commands.emplace_back("track.set", std::move(track));
					}
					if (!channel.empty())
					{
						channel["channel"] = element->track->channel;
						commands.emplace_back("channel.set", std::move(channel));
					}
					break;
				case Kind::AudioPart:
				case Kind::MidiPart:
				case Kind::AudioEvent:
				{
					const Json events = Json::array({element->id});
					if (element->locked)
						break;
					Json set = Json::object();
					if (renamed)
						set["name"] = name;
					if (recolored)
						set["color"] = color;
					if (!set.empty())
					{
						set["events"] = events;
						commands.emplace_back("event.set", std::move(set));
					}
					const auto ticks = [](double logical)
					{ return std::llround(std::max(logical, 0.0) * g_logicalTick); };
					const bool withPart =
						element->kind == Kind::AudioEvent && parts.count(project.audioEvents.at(element->id).part) != 0;
					if (const int64_t delta = ticks(position) - ticks(element->position); delta != 0 && !withPart)
						commands.emplace_back("event.move", Json{{"events", events}, {"ticks", delta}});
					if (const int64_t delta = ticks(std::max(length, 1.0)) - ticks(element->length); delta != 0)
						commands.emplace_back(
							"event.resize", Json{{"events", events}, {"edge", "end"}, {"ticks", delta}});
					break;
				}
				case Kind::Marker:
				{
					const Marker& marker = project.markers.at(element->id);
					Json set = Json::object();
					if (renamed)
						set["name"] = name;
					if (recolored)
						set["color"] = color;
					const auto at = [&](double logical)
					{
						const time::Position ticks{
							time::TimeBase::Musical, std::llround(std::max(logical, 0.0) * g_logicalTick)};
						return time::rebase(ticks, marker.position.base, project.tempo, project.sampleRate).value;
					};
					if (position != element->position || length != element->length)
					{
						set["position"] = at(position);
						if (marker.type == MarkerType::Cycle)
						{
							// At least one sample long, as marker.set wants it, also on a musical track.
							Marker moved = marker;
							moved.position.value = at(position);
							moved.end = {marker.position.base, at(position + std::max(length, 0.0))};
							keepMinimumLength(project, moved);
							set["end"] = moved.end.value;
						}
					}
					if (!set.empty())
					{
						set["marker"] = element->id;
						commands.emplace_back("marker.set", std::move(set));
					}
					break;
				}
				}
			}
			return commands;
		}

		using PresetLibrary = RulePresets<ProjectRulePreset>;

		ProjectRules rulesOf(const PresetLibrary& presets, const Json& parameters)
		{
			if (parameters.contains("rules") == parameters.contains("preset"))
				throw CommandError("give either rules or preset");
			if (parameters.contains("preset"))
				return presets.rulesOf(parameters.at("preset").get<std::string>());
			try
			{
				return projectRulesFromJson(parameters.at("rules"));
			}
			catch (const std::invalid_argument& error)
			{
				throw CommandError(error.what());
			}
		}

		std::set<ObjectId> selectedOf(const Json& parameters)
		{
			const auto ids = parameters.value("selected", std::vector<ObjectId>{});
			return {ids.begin(), ids.end()};
		}

		Json objectSchema(Json properties, const std::vector<std::string>& required = {})
		{
			return {{"type", "object"}, {"properties", std::move(properties)}, {"required", required},
				{"additionalProperties", false}};
		}
	}

	bool operator==(const ProjectRules& a, const ProjectRules& b)
	{
		return toJson(a) == toJson(b);
	}

	Json toJson(const ProjectRules& rules)
	{
		Json filters = Json::array();
		for (const ProjectFilterLine& line : rules.filters)
			filters.push_back(filterJson(line));
		Json actions = Json::array();
		for (const ProjectActionLine& action : rules.actions)
			actions.push_back(actionJson(action));
		return {{"filters", std::move(filters)}, {"function", g_functions[static_cast<size_t>(rules.function)]},
			{"actions", std::move(actions)}, {"pre", commandsJson(rules.pre)}, {"post", commandsJson(rules.post)}};
	}

	ProjectRules projectRulesFromJson(const Json& json)
	{
		requireObject(json, "rules");
		const std::string problem = validate(json, projectRulesSchema(), "rules");
		if (!problem.empty())
			throw std::invalid_argument(problem);
		ProjectRules rules;
		const Json filters = json.value("filters", Json::array());
		int depth = 0;
		for (size_t i = 0; i < filters.size(); ++i)
		{
			const ProjectFilterLine line = filterFromJson(filters[i], "rules.filters[" + std::to_string(i) + "]");
			depth += line.open;
			if (depth > midi::g_maxLogicalBrackets)
				throw std::invalid_argument("rules.filters: brackets nest at most 3 deep");
			depth -= line.close;
			if (depth < 0)
				throw std::invalid_argument("rules.filters: a bracket closes that wasn't opened");
			rules.filters.push_back(line);
		}
		if (depth != 0)
			throw std::invalid_argument("rules.filters: a bracket is opened and not closed");
		rules.function = static_cast<ProjectFunction>(
			indexOf(g_functions, json.value("function", std::string("transform")), "function"));
		const Json actions = json.value("actions", Json::array());
		for (size_t i = 0; i < actions.size(); ++i)
			rules.actions.push_back(actionFromJson(actions[i], "rules.actions[" + std::to_string(i) + "]"));
		rules.pre = commandsFromJson(json.value("pre", Json::array()), "rules.pre");
		rules.post = commandsFromJson(json.value("post", Json::array()), "rules.post");
		return rules;
	}

	Json projectRulesSchema()
	{
		const Json number = {{"type", "number"}};
		const Json value = {{"type", {"number", "string"}}};
		const Json bracket = {{"type", "integer"}, {"minimum", 0}, {"maximum", 3}};
		const auto choice = [](std::vector<std::string> values) { return Json{{"type", "string"}, {"enum", values}}; };
		const Json filter = {{"type", "object"}, {"additionalProperties", false}, {"required", {"target", "condition"}},
			{"properties",
				{{"open", bracket}, {"target", choice(names(g_targets))}, {"condition", choice(names(g_conditions))},
					{"parameter1", value}, {"parameter2", number}, {"close", bracket},
					{"join", choice(names(g_joins))}}}};
		const Json action = {{"type", "object"}, {"additionalProperties", false}, {"required", {"target", "operation"}},
			{"properties",
				{{"target", choice(names(g_actionTargets))}, {"operation", choice(names(g_operations))},
					{"parameter1", value}, {"parameter2", value}}}};
		const Json command = {{"type", "object"}, {"additionalProperties", false}, {"required", {"command"}},
			{"properties", {{"command", {{"type", "string"}}}, {"parameters", {{"type", "object"}}}}}};
		const Json commands = {{"type", "array"}, {"items", command}, {"maxItems", g_maxRuleCommands}};
		return {{"type", "object"}, {"additionalProperties", false},
			{"properties",
				{{"filters", {{"type", "array"}, {"items", filter}}}, {"function", choice(names(g_functions))},
					{"actions", {{"type", "array"}, {"items", action}}}, {"pre", commands}, {"post", commands}}}};
	}

	const std::vector<ProjectRulePreset>& factoryProjectRulePresets()
	{
		using A = ProjectActionTarget;
		using C = ProjectCondition;
		using F = ProjectFunction;
		using O = ProjectOperation;
		using T = ProjectTarget;
		constexpr auto either = midi::LogicalBool::Or;
		const auto mediaIs = [](const char* type) { return where(T::MediaType, C::Equal, type); };
		static const std::vector<ProjectRulePreset> presets = {
			{"Init", {}},
			{"Delete/Empty parts", {{is("part"), has("isEmpty")}, F::Delete, {}, {}, {}}},
			{"Delete/Muted parts and events",
				{{is("part", either, 1), is("event", midi::LogicalBool::And, 0, 1), has("isMuted")}, F::Delete, {}, {},
					{}}},
			{"Delete/Empty tracks",
				{{is("track", either, 1), is("folder", midi::LogicalBool::And, 0, 1), has("isEmpty")}, F::Delete, {},
					{}, {}}},
			{"Select/MIDI parts", {{mediaIs("midi"), is("part")}, F::Select, {}, {}, {}}},
			{"Select/Audio events", {{mediaIs("audio"), is("event")}, F::Select, {}, {}, {}}},
			{"Select/Muted parts and events",
				{{is("part", either, 1), is("event", midi::LogicalBool::And, 0, 1), has("isMuted")}, F::Select, {}, {},
					{}}},
			{"Select/Tracks named Vocals",
				{{is("track"), where(T::Name, C::Contains, "Vocals")}, F::Select, {}, {}, {}}},
			{"Select/Parts of the selected tracks", {{is("part"), has("parentIsSelected")}, F::Select, {}, {}, {}}},
			{"Deselect/All", {{}, F::Deselect, {}, {}, {}}},
			{"Transform/Unmute all tracks",
				{{is("track")}, F::Transform, {change(A::TrackOperation, O::Mute, "off")}, {}, {}}},
			{"Transform/Mute the selected tracks",
				{{is("track"), has("isSelected")}, F::Transform, {change(A::TrackOperation, O::Mute, "on")}, {}, {}}},
			{"Transform/Number the selected tracks",
				{{is("track"), has("isSelected")}, F::Transform, {change(A::Name, O::GenerateName, "Track ", 0, 1)}, {},
					{}}},
			{"Transform/Selected parts a quarter note later",
				{{is("part"), has("isSelected")}, F::Transform, {change(A::Position, O::Add, {}, 480)}, {}, {}}},
			{"Transform/Selected parts to quarter notes",
				{{is("part"), has("isSelected")}, F::Transform, {change(A::Position, O::RoundBy, {}, 480)}, {}, {}}},
			{"Transform/Next color for the selected tracks",
				{{is("track"), has("isSelected")}, F::Transform, {change(A::Color, O::Increment)}, {}, {}}},
		};
		return presets;
	}

	const settings::Format& projectRulePresetsFormat()
	{
		static const settings::Format format{"project-rule-presets", 1,
			{{std::string(g_presetsKey), "The user's project rule presets.", Json{{"type", "array"}}, Json::array()}},
			{}};
		return format;
	}

	void registerProjectRuleCommands(ProjectModel& model, Engine& engine)
	{
		const auto presets = std::make_shared<PresetLibrary>(
			RulePresetKind<ProjectRulePreset>{std::string(g_presetsKey), &projectRulePresetsFormat(),
				"project rule preset", "projectRules.presetsChanged", &factoryProjectRulePresets(),
				[](const ProjectRules& rules) { return toJson(rules); }, projectRulesFromJson},
			engine.settingsDirectory());
		const Json ids = {{"type", "array"}, {"items", {{"type", "integer"}}}};
		const Json stringSchema = {{"type", "string"}};
		const Json rulesSchema = projectRulesSchema();
		const std::string rulesText =
			" rules: filters, each {open, target, condition, parameter1, parameter2, close, join}: target mediaType "
			"(parameter1 audio, midi, marker, tempo, signature, arranger, transpose, chord, group, effect or vca; "
			"condition equal, unequal or allTypes), containerType (folder, track, part or event, the same conditions), "
			"name or colorName (a color name of the project's colors; condition equal, unequal, contains or "
			"containsNot), position or length (in ticks of 480 to a quarter note from the project's start; condition "
			"equal, unequal, bigger, biggerOrEqual, less, lessOrEqual, insideRange or outsideRange with parameter2; "
			"tracks have neither) or property (isMuted, isSelected, isEmpty, isHidden, hasTrackVersion, "
			"followsChordTrack, isDisabled or parentIsSelected; condition propertyIsSet or propertyIsNotSet); open and "
			"close brackets 0 to 3, which match and nest at most 3 deep; join and or or to the next line, and binding "
			"more tightly than or. function "
			"delete, transform, select or deselect. actions, each {target, operation, parameter1, parameter2} in "
			"order: position or length (add, subtract, multiplyBy, divideBy, roundBy, setRandom, setRelativeRandom "
			"or setFixed), name (replace, append, prepend with parameter1; generateName: parameter1 and a number from "
			"parameter2, one up for each element renamed; replaceSearchString: parameter1 by parameter2; eraseBefore "
			"and eraseAfter parameter1; eraseFrontCharacter, eraseEndCharacter), trackOperation (mute, solo, record or "
			"monitor with parameter1 on, off or toggle; timeDomain musical, linear or toggle) or color (setFixed: a "
			"project color's name or #rrggbb; increment, decrement: the next or previous project color). pre and post: "
			"up to 4 key commands each, {command, parameters}, that the UI runs before and after; the engine keeps "
			"them with the rules. Tracks are found from top to bottom, then parts, audio events and markers. preset "
			"names a preset instead of rules. selected: the ids of the selected tracks, parts, events and markers, for "
			"isSelected and parentIsSelected.";
		const Json properties = {
			{"rules", rulesSchema}, {"preset", stringSchema}, {"selected", ids}, {"seed", {{"type", "integer"}}}};
		const Json foundSchema = {{"type", "object"},
			{"properties", {{"tracks", ids}, {"events", ids}, {"markers", ids}}},
			{"required", {"tracks", "events", "markers"}}};

		model.addEdit(engine,
			{"project.applyRules", 1,
				"Applies project rules to the project's tracks, audio and MIDI parts, audio events and markers, in one "
				"undo step: delete removes what the filters find (a found folder takes its tracks with it), transform "
				"changes it by the actions through the commands that make such changes (event.move, event.resize, "
				"event.set, track.set, channel.set, marker.set). Locked parts and events stay as they are on tracks "
				"that stay, except that a locked event moves with an audio part that moves; delete keeps an audio "
				"part that holds a locked event. Audio events keep their clip's name, track operations change tracks "
				"only. Positions and lengths stop at the longest project. seed makes the random operations "
				"repeatable; without it each call gives new random values." +
					rulesText + " Use project.findByRules for select and deselect. Returns what the filters found.",
				objectSchema(properties), foundSchema, false},
			"Project Rule Editor",
			[&model, presets](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const ProjectRules rules = rulesOf(*presets, parameters);
				if (rules.function == ProjectFunction::Select || rules.function == ProjectFunction::Deselect)
					throw CommandError("project.findByRules selects and deselects");
				const std::vector<Element> elements = elementsOf(project, selectedOf(parameters));
				const Found found = find(project, rules, elements);
				const Commands commands = rules.function == ProjectFunction::Delete
					? deleting(project, found)
					: transforming(project, rules, found,
						  parameters.contains("seed") ? parameters.at("seed").get<uint64_t>()
													  : midi::nextLogicalSeed());
				const Json result = found.json();
				for (const auto& [command, commandParameters] : commands)
					model.runEdit(command, project, commandParameters, context);
				return result;
			});
		model.addEdit(engine,
			{"project.findByRules", 1,
				"Finds the tracks, audio and MIDI parts, audio events and markers that project rules' filters find, "
				"as select does, and changes nothing; the UI selects them, or for deselect deselects everything." +
					rulesText + " Returns what the filters found, by id.",
				objectSchema(properties), foundSchema, true},
			"",
			[presets](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const ProjectRules rules = rulesOf(*presets, parameters);
				const std::vector<Element> elements = elementsOf(project, selectedOf(parameters));
				return find(project, rules, elements).json();
			});

		engine.registerCommand(
			{"projectRules.presets", 1,
				"The project rule presets: the factory presets, named by category (such as \"Delete/Empty parts\" "
				"and \"Select/MIDI parts\"), and the user's, each with its rules as project.applyRules takes them. The "
				"user's presets are kept in the settings folder, so they come back when GNUbase starts again: file is "
				"where (null if they are kept in memory only), readOnly whether changes are saved, and problems what "
				"was wrong with it, such as presets that this GNUbase can't read and skips; they stay in the file.",
				objectSchema({}),
				{{"type", "object"},
					{"properties",
						{{"presets", {{"type", "array"}}}, {"file", {{"type", {"string", "null"}}}},
							{"readOnly", {{"type", "boolean"}}}, {"problems", {{"type", "array"}}}}},
					{"required", {"presets", "file", "readOnly", "problems"}}},
				true},
			[presets](CommandContext&) { return presets->toJson(); });
		engine.registerCommand(
			{"projectRules.savePreset", 1,
				"Saves rules (as project.applyRules takes them, with their pre and post commands) as a project rule "
				"preset named name, replacing the user's preset of that name. Factory presets can't be replaced. "
				"Presets belong to the user, not to the project: they are saved in the settings folder. "
				"projectRules.presetsChanged announces the user's presets' names.",
				objectSchema({{"name", stringSchema}, {"rules", rulesSchema}}, {"name", "rules"}),
				{{"type", "object"}, {"properties", {{"preset", {{"type", "object"}}}}}, {"required", {"preset"}}},
				false},
			[presets](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				ProjectRulePreset preset{parameters.at("name").get<std::string>(), {}};
				try
				{
					preset.rules = projectRulesFromJson(parameters.at("rules"));
				}
				catch (const std::invalid_argument& error)
				{
					throw CommandError(error.what());
				}
				presets->save(preset);
				presets->announce(context);
				return Json{{"preset", presets->presetJson(preset, false)}};
			});
		engine.registerCommand({"projectRules.removePreset", 1,
								   "Removes one of the user's project rule presets, and the presets of that name that "
								   "this GNUbase can't read (see projectRules.presets).",
								   objectSchema({{"name", stringSchema}}, {"name"}), Json::object(), false},
			[presets](CommandContext& context)
			{
				presets->remove(context.parameters().at("name").get<std::string>());
				presets->announce(context);
				return Json::object();
			});
	}
}
