// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/midi/logical.h"

#include "engine/midi/functions.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace gnubase::engine::midi
{
	namespace
	{
		constexpr const char* g_types[] = {"note", "polyPressure", "controller", "program", "aftertouch", "pitchBend"};
		constexpr const char* g_targets[] = {
			"position", "length", "subtype", "mainValue", "channel", "type", "secondaryValue"};
		constexpr const char* g_conditions[] = {"equal", "unequal", "bigger", "biggerOrEqual", "less", "lessOrEqual",
			"insideRange", "outsideRange", "noteIsEqualTo"};
		constexpr const char* g_joins[] = {"and", "or"};
		constexpr const char* g_functions[] = {"delete", "transform", "insert", "insertExclusive", "select"};
		constexpr const char* g_operations[] = {"add", "subtract", "multiplyBy", "divideBy", "roundBy", "setRandom",
			"setFixed", "setRelativeRandom", "useSubtype", "useMainValue", "mirror", "transposeToScale", "addLength"};

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

		double valueOf(const LogicalEvent& event, LogicalTarget target) noexcept
		{
			switch (target)
			{
			case LogicalTarget::Position:
				return event.position;
			case LogicalTarget::Length:
				return event.length;
			case LogicalTarget::Subtype:
				return event.subtype;
			case LogicalTarget::MainValue:
				return event.mainValue;
			case LogicalTarget::Channel:
				return event.channel;
			case LogicalTarget::Type:
				return static_cast<double>(event.type);
			case LogicalTarget::SecondaryValue:
				return event.secondaryValue;
			}
			return 0.0;
		}

		int floorMod(int value, int divisor)
		{
			const int result = value % divisor;
			return result < 0 ? result + divisor : result;
		}

		bool test(const FilterLine& line, const LogicalEvent& event) noexcept
		{
			const double value = valueOf(event, line.target);
			const double p1 = line.parameter1;
			const double p2 = line.parameter2;
			switch (line.condition)
			{
			case LogicalCondition::Equal:
				return value == p1;
			case LogicalCondition::Unequal:
				return value != p1;
			case LogicalCondition::Bigger:
				return value > p1;
			case LogicalCondition::BiggerOrEqual:
				return value >= p1;
			case LogicalCondition::Less:
				return value < p1;
			case LogicalCondition::LessOrEqual:
				return value <= p1;
			case LogicalCondition::InsideRange:
				return value >= p1 && value <= p2;
			case LogicalCondition::OutsideRange:
				return value < p1 || value > p2;
			case LogicalCondition::NoteIsEqualTo:
				return floorMod(static_cast<int>(std::lround(value)), 12) ==
					floorMod(static_cast<int>(std::lround(p1)), 12);
			}
			return false;
		}

		// Reads the filter lines as tokens: each line's open brackets, the line, its close brackets and its join.
		class Expression
		{
		public:
			Expression(const FilterLine* lines, size_t count, const LogicalEvent& event) noexcept
				: m_lines(lines), m_count(count), m_event(event), m_open(count > 0 ? lines[0].open : 0)
			{
			}

			// expression := term (or term)*
			bool expression() noexcept
			{
				bool result = term();
				while (m_line < m_count && m_phase == Phase::Join && m_lines[m_line].join == LogicalBool::Or)
				{
					next();
					result = term() || result; // every line is read, also when the result is known
				}
				return result;
			}

		private:
			enum class Phase : uint8_t
			{
				Open,  // m_open brackets before the line
				Close, // m_close brackets after it
				Join,  // its join to the next line
			};

			// term := factor (and factor)*
			bool term() noexcept
			{
				bool result = factor();
				while (m_line < m_count && m_phase == Phase::Join && m_lines[m_line].join == LogicalBool::And)
				{
					next();
					result = factor() && result;
				}
				return result;
			}

			// factor := ( expression ) | line
			bool factor() noexcept
			{
				if (m_line >= m_count)
					return false;
				if (m_phase == Phase::Open && m_open > 0)
				{
					--m_open;
					const bool result = expression();
					if (m_phase == Phase::Close && m_close > 0)
						--m_close;
					afterClose();
					return result;
				}
				if (m_phase != Phase::Open)
					return false; // not reached for matching brackets
				const bool result = test(m_lines[m_line], m_event);
				m_phase = Phase::Close;
				m_close = m_lines[m_line].close;
				afterClose();
				return result;
			}

			void afterClose() noexcept
			{
				if (m_phase == Phase::Close && m_close == 0)
				{
					if (m_line + 1 < m_count)
						m_phase = Phase::Join;
					else
						m_line = m_count;
				}
			}

			void next() noexcept
			{
				++m_line;
				m_phase = Phase::Open;
				m_open = m_lines[m_line].open;
			}

			const FilterLine* m_lines;
			size_t m_count;
			const LogicalEvent& m_event;
			size_t m_line = 0;
			Phase m_phase = Phase::Open;
			int m_open;
			int m_close = 0;
		};

		double apply(const ActionLine& action, const LogicalEvent& event, double value, LogicalRandom& random) noexcept
		{
			const double p1 = action.parameter1;
			const double p2 = action.parameter2;
			const auto whole = [](double v) { return static_cast<int>(std::lround(v)); };
			switch (action.operation)
			{
			case LogicalOperation::Add:
				return value + p1;
			case LogicalOperation::Subtract:
				return value - p1;
			case LogicalOperation::MultiplyBy:
				return value * p1;
			case LogicalOperation::DivideBy:
				return p1 != 0.0 ? value / p1 : value;
			case LogicalOperation::RoundBy:
				return p1 != 0.0 ? std::round(value / p1) * p1 : value;
			case LogicalOperation::SetRandom:
				return random.between(whole(p1), whole(p2));
			case LogicalOperation::SetFixed:
				return p1;
			case LogicalOperation::SetRelativeRandom:
				return value + random.between(whole(p1), whole(p2));
			case LogicalOperation::UseSubtype:
				return event.subtype;
			case LogicalOperation::UseMainValue:
				return event.mainValue;
			case LogicalOperation::Mirror:
				return (2.0 * p1) - value;
			case LogicalOperation::TransposeToScale:
				return nearestInScale(whole(value), Scale{floorMod(whole(p1), 12), static_cast<ScaleType>(whole(p2))});
			case LogicalOperation::AddLength:
				return value + event.length;
			}
			return value;
		}

		void set(LogicalEvent& event, LogicalTarget target, double value) noexcept
		{
			const int whole = static_cast<int>(std::lround(std::clamp(value, -1e9, 1e9)));
			switch (target)
			{
			case LogicalTarget::Position:
				event.position = value;
				break;
			case LogicalTarget::Length:
				event.length = value;
				break;
			case LogicalTarget::Subtype:
				event.subtype = whole;
				break;
			case LogicalTarget::MainValue:
				event.mainValue = whole;
				break;
			case LogicalTarget::Channel:
				event.channel = whole;
				break;
			case LogicalTarget::Type:
				event.type = static_cast<LogicalType>(std::clamp(whole, 0, g_logicalTypes - 1));
				break;
			case LogicalTarget::SecondaryValue:
				event.secondaryValue = whole;
				break;
			}
		}

		bool isNumber(const Json& json)
		{
			return json.is_number();
		}

		double number(const Json& item, const char* key, const std::string& name)
		{
			const auto found = item.find(key);
			if (found == item.end())
				return 0.0;
			if (!isNumber(*found))
				throw std::invalid_argument(name + "." + key + " must be a number");
			const double value = found->get<double>();
			if (!std::isfinite(value) || std::abs(value) > 1e9)
				throw std::invalid_argument(name + "." + key + " is out of range");
			return value;
		}

		int brackets(const Json& item, const char* key, const std::string& name)
		{
			const double value = number(item, key, name);
			if (value != std::floor(value) || value < 0 || value > 3)
				throw std::invalid_argument(name + "." + key + " must be 0 to 3");
			return static_cast<int>(value);
		}

		std::string text(const Json& item, const char* key, const std::string& name, const char* fallback)
		{
			const auto found = item.find(key);
			if (found == item.end())
				return fallback;
			if (!found->is_string())
				throw std::invalid_argument(name + "." + key + " must be a string");
			return found->get<std::string>();
		}

		// A type, as its name.
		double typeValue(const Json& item, const char* key, const std::string& name)
		{
			const auto found = item.find(key);
			if (found == item.end() || !found->is_string())
				throw std::invalid_argument(name + "." + key + " must be an event type");
			return indexOf(g_types, found->get<std::string>(), "event type");
		}

		void requireObject(const Json& item, const std::string& name)
		{
			if (!item.is_object())
				throw std::invalid_argument(name + " must be an object");
		}

		FilterLine filterFromJson(const Json& item, const std::string& name)
		{
			requireObject(item, name);
			FilterLine line;
			line.open = brackets(item, "open", name);
			line.close = brackets(item, "close", name);
			line.target = static_cast<LogicalTarget>(indexOf(g_targets, text(item, "target", name, ""), "target"));
			line.condition =
				static_cast<LogicalCondition>(indexOf(g_conditions, text(item, "condition", name, ""), "condition"));
			line.join = static_cast<LogicalBool>(indexOf(g_joins, text(item, "join", name, "and"), "join"));
			if (line.target == LogicalTarget::Type)
			{
				if (line.condition != LogicalCondition::Equal && line.condition != LogicalCondition::Unequal)
					throw std::invalid_argument(name + ": type is either equal or unequal");
				line.parameter1 = typeValue(item, "parameter1", name);
			}
			else
			{
				line.parameter1 = number(item, "parameter1", name);
				line.parameter2 = number(item, "parameter2", name);
			}
			if (line.condition == LogicalCondition::NoteIsEqualTo && line.target != LogicalTarget::Subtype)
				throw std::invalid_argument(name + ": noteIsEqualTo is for the subtype only");
			return line;
		}

		ActionLine actionFromJson(const Json& item, const std::string& name)
		{
			requireObject(item, name);
			ActionLine action;
			action.target = static_cast<LogicalTarget>(indexOf(g_targets, text(item, "target", name, ""), "target"));
			action.operation =
				static_cast<LogicalOperation>(indexOf(g_operations, text(item, "operation", name, ""), "operation"));
			const auto only =
				[&](LogicalOperation operation, std::initializer_list<LogicalTarget> targets, const char* what)
			{
				if (action.operation == operation &&
					std::find(targets.begin(), targets.end(), action.target) == targets.end())
					throw std::invalid_argument(
						name + ": " + g_operations[static_cast<size_t>(operation)] + " is for the " + what + " only");
			};
			only(LogicalOperation::UseSubtype, {LogicalTarget::MainValue}, "main value");
			only(LogicalOperation::UseMainValue, {LogicalTarget::Subtype}, "subtype");
			only(LogicalOperation::TransposeToScale, {LogicalTarget::Subtype}, "subtype");
			only(
				LogicalOperation::Mirror, {LogicalTarget::Subtype, LogicalTarget::MainValue}, "subtype and main value");
			only(LogicalOperation::AddLength, {LogicalTarget::Position}, "position");
			if (action.target == LogicalTarget::Type)
			{
				if (action.operation != LogicalOperation::SetFixed)
					throw std::invalid_argument(name + ": the type can only be set to a fixed value");
				action.parameter1 = typeValue(item, "parameter1", name);
				return action;
			}
			action.parameter1 = number(item, "parameter1", name);
			if (action.operation == LogicalOperation::TransposeToScale)
			{
				const std::optional<ScaleType> scale = scaleTypeFromName(text(item, "parameter2", name, ""));
				if (!scale)
					throw std::invalid_argument(name + ".parameter2 must be a scale type");
				action.parameter2 = static_cast<double>(*scale);
			}
			else
			{
				action.parameter2 = number(item, "parameter2", name);
			}
			return action;
		}

		Json filterJson(const FilterLine& line)
		{
			const auto target = static_cast<size_t>(line.target);
			Json p1 = line.parameter1;
			if (line.target == LogicalTarget::Type)
				p1 = g_types[static_cast<size_t>(line.parameter1)];
			return {{"open", line.open}, {"target", g_targets[target]},
				{"condition", g_conditions[static_cast<size_t>(line.condition)]}, {"parameter1", std::move(p1)},
				{"parameter2", line.parameter2}, {"close", line.close},
				{"join", g_joins[static_cast<size_t>(line.join)]}};
		}

		Json actionJson(const ActionLine& action)
		{
			Json p1 = action.parameter1;
			Json p2 = action.parameter2;
			if (action.target == LogicalTarget::Type)
				p1 = g_types[static_cast<size_t>(action.parameter1)];
			if (action.operation == LogicalOperation::TransposeToScale)
				p2 = scaleTypeName(static_cast<ScaleType>(action.parameter2));
			return {{"target", g_targets[static_cast<size_t>(action.target)]},
				{"operation", g_operations[static_cast<size_t>(action.operation)]}, {"parameter1", std::move(p1)},
				{"parameter2", std::move(p2)}};
		}

		// Builders for the factory presets.
		FilterLine is(LogicalType type, LogicalBool join = LogicalBool::And)
		{
			return {0, LogicalTarget::Type, LogicalCondition::Equal, static_cast<double>(type), 0.0, 0, join};
		}

		FilterLine where(LogicalTarget target, LogicalCondition condition, double p1, double p2 = 0.0)
		{
			return {0, target, condition, p1, p2, 0, LogicalBool::And};
		}

		ActionLine change(LogicalTarget target, LogicalOperation operation, double p1, double p2 = 0.0)
		{
			return {target, operation, p1, p2};
		}
	}

	bool operator==(const FilterLine& a, const FilterLine& b)
	{
		return std::tie(a.open, a.target, a.condition, a.parameter1, a.parameter2, a.close, a.join) ==
			std::tie(b.open, b.target, b.condition, b.parameter1, b.parameter2, b.close, b.join);
	}

	bool operator==(const ActionLine& a, const ActionLine& b)
	{
		return std::tie(a.target, a.operation, a.parameter1, a.parameter2) ==
			std::tie(b.target, b.operation, b.parameter1, b.parameter2);
	}

	bool operator==(const LogicalRules& a, const LogicalRules& b)
	{
		return a.filters == b.filters && a.function == b.function && a.actions == b.actions;
	}

	bool operator==(const InputTransformerModule& a, const InputTransformerModule& b)
	{
		return a.on == b.on && a.rules == b.rules;
	}

	bool operator==(const InputTransformer& a, const InputTransformer& b)
	{
		return a.mode == b.mode && a.modules == b.modules;
	}

	bool matches(const FilterLine* lines, size_t count, const LogicalEvent& event) noexcept
	{
		if (count == 0)
			return true;
		return Expression(lines, count, event).expression();
	}

	int LogicalRandom::between(int low, int high) noexcept
	{
		if (low > high)
			std::swap(low, high);
		// splitmix64
		uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
		z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
		z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
		z ^= z >> 31U;
		const auto range = static_cast<uint64_t>(static_cast<int64_t>(high) - low + 1);
		return static_cast<int>(low + static_cast<int64_t>(z % range));
	}

	bool transform(const ActionLine* actions, size_t count, LogicalEvent& event, LogicalRandom& random) noexcept
	{
		for (size_t i = 0; i < count; ++i)
		{
			const ActionLine& action = actions[i];
			set(event, action.target, apply(action, event, valueOf(event, action.target), random));
		}
		const bool note = event.type == LogicalType::Note;
		if (note && (event.subtype < 0 || event.subtype > 127))
			return false;
		event.channel = std::clamp(event.channel, 1, 16);
		event.subtype = std::clamp(event.subtype, 0, 127);
		event.mainValue = std::clamp(event.mainValue, 0, 127);
		event.secondaryValue = std::clamp(event.secondaryValue, 0, 127);
		event.position = std::max(event.position, 0.0);
		event.length = std::max(event.length, 1.0);
		return true;
	}

	const char* toString(LogicalType type)
	{
		return g_types[static_cast<size_t>(type)];
	}

	const char* toString(LogicalFunction function)
	{
		return g_functions[static_cast<size_t>(function)];
	}

	Json toJson(const LogicalRules& rules)
	{
		Json filters = Json::array();
		for (const FilterLine& line : rules.filters)
			filters.push_back(filterJson(line));
		Json actions = Json::array();
		for (const ActionLine& action : rules.actions)
			actions.push_back(actionJson(action));
		return {
			{"filters", std::move(filters)}, {"function", toString(rules.function)}, {"actions", std::move(actions)}};
	}

	LogicalRules logicalRulesFromJson(const Json& json)
	{
		requireObject(json, "rules");
		const std::string problem = validate(json, logicalRulesSchema(), "rules");
		if (!problem.empty())
			throw std::invalid_argument(problem);
		LogicalRules rules;
		const Json filters = json.value("filters", Json::array());
		int depth = 0;
		for (size_t i = 0; i < filters.size(); ++i)
		{
			const FilterLine line = filterFromJson(filters[i], "rules.filters[" + std::to_string(i) + "]");
			depth += line.open - line.close;
			if (depth < 0)
				throw std::invalid_argument("rules.filters: a bracket closes that wasn't opened");
			rules.filters.push_back(line);
		}
		if (depth != 0)
			throw std::invalid_argument("rules.filters: a bracket is opened and not closed");
		rules.function = static_cast<LogicalFunction>(
			indexOf(g_functions, json.value("function", std::string("transform")), "function"));
		const Json actions = json.value("actions", Json::array());
		for (size_t i = 0; i < actions.size(); ++i)
			rules.actions.push_back(actionFromJson(actions[i], "rules.actions[" + std::to_string(i) + "]"));
		return rules;
	}

	Json logicalRulesSchema()
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
				{{"target", choice(names(g_targets))}, {"operation", choice(names(g_operations))},
					{"parameter1", value}, {"parameter2", value}}}};
		return {{"type", "object"}, {"additionalProperties", false},
			{"properties",
				{{"filters", {{"type", "array"}, {"items", filter}}}, {"function", choice(names(g_functions))},
					{"actions", {{"type", "array"}, {"items", action}}}}}};
	}

	const std::vector<LogicalPreset>& factoryLogicalPresets()
	{
		using T = LogicalTarget;
		using C = LogicalCondition;
		using O = LogicalOperation;
		using F = LogicalFunction;
		using Y = LogicalType;
		static const std::vector<LogicalPreset> presets = {
			{"Init", {}},
			{"Delete/Short notes", {{is(Y::Note), where(T::Length, C::Less, 60)}, F::Delete, {}}},
			{"Delete/Soft notes", {{is(Y::Note), where(T::MainValue, C::Less, 20)}, F::Delete, {}}},
			{"Delete/Controllers", {{is(Y::Controller)}, F::Delete, {}}},
			{"Delete/Aftertouch", {{is(Y::Aftertouch, LogicalBool::Or), is(Y::PolyPressure)}, F::Delete, {}}},
			{"Delete/Pitch bend", {{is(Y::PitchBend)}, F::Delete, {}}},
			{"Delete/Program changes", {{is(Y::Program)}, F::Delete, {}}},
			{"Transform/Octave up", {{is(Y::Note)}, F::Transform, {change(T::Subtype, O::Add, 12)}}},
			{"Transform/Octave down", {{is(Y::Note)}, F::Transform, {change(T::Subtype, O::Subtract, 12)}}},
			{"Transform/Mirror pitches around C3", {{is(Y::Note)}, F::Transform, {change(T::Subtype, O::Mirror, 60)}}},
			{"Transform/Fixed velocity 100", {{is(Y::Note)}, F::Transform, {change(T::MainValue, O::SetFixed, 100)}}},
			{"Transform/Velocity +10", {{is(Y::Note)}, F::Transform, {change(T::MainValue, O::Add, 10)}}},
			{"Transform/Velocity -10", {{is(Y::Note)}, F::Transform, {change(T::MainValue, O::Subtract, 10)}}},
			{"Transform/Invert velocities", {{is(Y::Note)}, F::Transform, {change(T::MainValue, O::Mirror, 64)}}},
			{"Transform/Humanize velocities",
				{{is(Y::Note)}, F::Transform, {change(T::MainValue, O::SetRelativeRandom, -10, 10)}}},
			{"Transform/Quantize starts to 1/16", {{}, F::Transform, {change(T::Position, O::RoundBy, 120)}}},
			{"Transform/All to channel 1", {{}, F::Transform, {change(T::Channel, O::SetFixed, 1)}}},
			{"Transform/Aftertouch to modulation",
				{{is(Y::Aftertouch)}, F::Transform,
					{change(T::MainValue, O::UseSubtype, 0), change(T::Subtype, O::SetFixed, 1),
						change(T::Type, O::SetFixed, static_cast<double>(Y::Controller))}}},
			{"Transform/Modulation to aftertouch",
				{{is(Y::Controller), where(T::Subtype, C::Equal, 1)}, F::Transform,
					{change(T::Subtype, O::UseMainValue, 0),
						change(T::Type, O::SetFixed, static_cast<double>(Y::Aftertouch))}}},
			{"Insert/Double an octave up", {{is(Y::Note)}, F::Insert, {change(T::Subtype, O::Add, 12)}}},
			{"Select/Notes above C3", {{is(Y::Note), where(T::Subtype, C::Bigger, 60)}, F::Select, {}}},
			{"Input/Split below C3 to channel 2",
				{{is(Y::Note), where(T::Subtype, C::Less, 60)}, F::Transform, {change(T::Channel, O::SetFixed, 2)}}},
			{"Input/Sustain pedal to bass drum",
				{{is(Y::Controller), where(T::Subtype, C::Equal, 64)}, F::Transform,
					{change(T::Subtype, O::SetFixed, 36), change(T::Type, O::SetFixed, static_cast<double>(Y::Note))}}},
		};
		return presets;
	}

	Json toJson(const InputTransformerModules& modules)
	{
		Json json = Json::array();
		for (const InputTransformerModule& module : modules)
			json.push_back({{"on", module.on}, {"rules", toJson(module.rules)}});
		return json;
	}

	InputTransformerModules inputTransformerModulesFromJson(const Json& json, InputTransformerModules base)
	{
		if (!json.is_array() || json.size() > g_inputTransformerModules)
			throw std::invalid_argument("modules must be an array of at most 4 modules");
		for (size_t i = 0; i < json.size(); ++i)
		{
			const std::string name = "modules[" + std::to_string(i) + "]";
			const Json& item = json[i];
			requireObject(item, name);
			for (const auto& [key, value] : item.items())
				if (key != "on" && key != "rules")
				{
					std::string message = name;
					message += " has no ";
					message += key;
					throw std::invalid_argument(message);
				}
			InputTransformerModule& module = base[i];
			if (item.contains("on"))
			{
				if (!item.at("on").is_boolean())
					throw std::invalid_argument(name + ".on must be a boolean");
				module.on = item.at("on").get<bool>();
			}
			if (item.contains("rules"))
			{
				LogicalRules rules = logicalRulesFromJson(item.at("rules"));
				if (rules.function == LogicalFunction::Select)
					throw std::invalid_argument(name + ": input transformers don't select");
				// Live events have no position or length.
				const auto timed = [](LogicalTarget target)
				{ return target == LogicalTarget::Position || target == LogicalTarget::Length; };
				if (std::any_of(rules.filters.begin(), rules.filters.end(),
						[&timed](const FilterLine& line) { return timed(line.target); }) ||
					std::any_of(rules.actions.begin(), rules.actions.end(),
						[&timed](const ActionLine& line) { return timed(line.target); }))
					throw std::invalid_argument(name + ": input transformers have no position or length");
				module.rules = std::move(rules);
			}
		}
		return base;
	}

	Json toJson(const InputTransformer& transformer)
	{
		constexpr const char* modes[] = {"off", "track", "project"};
		return {{"mode", modes[static_cast<size_t>(transformer.mode)]}, {"modules", toJson(transformer.modules)}};
	}

	InputTransformer inputTransformerFromJson(const Json& json, InputTransformer base)
	{
		requireObject(json, "inputTransformer");
		for (const auto& [key, value] : json.items())
			if (key != "mode" && key != "modules")
				throw std::invalid_argument("inputTransformer has no " + key);
		if (json.contains("mode"))
		{
			constexpr const char* modes[] = {"off", "track", "project"};
			const Json& mode = json.at("mode");
			if (!mode.is_string())
				throw std::invalid_argument("inputTransformer.mode must be off, track or project");
			base.mode = static_cast<InputTransformerMode>(indexOf(modes, mode.get<std::string>(), "mode"));
		}
		if (json.contains("modules"))
			base.modules = inputTransformerModulesFromJson(json.at("modules"), base.modules);
		return base;
	}
}
