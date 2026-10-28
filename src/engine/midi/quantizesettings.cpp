// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/midi/quantizesettings.h"

#include "engine/document/serialize.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace gnubase::engine::midi
{
	namespace
	{
		constexpr int g_largestDenominator = 128;
		constexpr int g_largestTuplet = 15;

		bool isPowerOfTwo(int value)
		{
			return value > 0 && (value & (value - 1)) == 0;
		}

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw std::invalid_argument(message);
		}

		double unit(const Json& json, const char* key)
		{
			const double value = json.at(key).get<double>();
			require(value >= 0.0 && value <= 1.0, std::string(key) + " must be 0 to 1");
			return value;
		}

		time::Ticks distance(const Json& json, const char* key)
		{
			const auto value = json.at(key).get<time::Ticks>();
			require(value >= 0, std::string(key) + " must not be negative");
			return value;
		}

		NoteValue noteValue(const Json& json, const char* key)
		{
			const std::optional<NoteValue> value = noteValueFromString(json.at(key).get<std::string>());
			require(value.has_value(),
				std::string(key) +
					" must be a note value from 1/1 to 1/128, like 1/16, 1/8T (triplet) or 1/4. (dotted)");
			return *value;
		}

		uint16_t velocityFrom(const Json& json, const std::string& name)
		{
			const double value = json.get<double>();
			require(value >= 0.0 && value <= 1.0, name + " must be 0 to 1");
			return static_cast<uint16_t>(std::lround(value * g_maxVelocity));
		}

		Groove grooveFromJson(const Json& json)
		{
			Groove groove;
			groove.length = json.at("length").get<time::Ticks>();
			require(groove.length > 0, "groove.length must be positive");
			const Json& points = json.at("points");
			require(!points.empty(), "groove.points must not be empty");
			for (size_t i = 0; i < points.size(); ++i)
			{
				const Json& item = points[i];
				const std::string name = "groove.points[" + std::to_string(i) + "]";
				GroovePoint point;
				point.position = item.at("position").get<time::Ticks>();
				require(point.position >= 0 && point.position < groove.length,
					name + ".position must be from 0 to below groove.length");
				if (item.contains("velocity") && !item.at("velocity").is_null())
					point.velocity = velocityFrom(item.at("velocity"), name + ".velocity");
				if (item.contains("length") && !item.at("length").is_null())
				{
					point.length = item.at("length").get<time::Ticks>();
					require(*point.length > 0, name + ".length must be positive");
				}
				groove.points.push_back(point);
			}
			std::sort(groove.points.begin(), groove.points.end(),
				[](const GroovePoint& a, const GroovePoint& b) { return a.position < b.position; });
			return groove;
		}

		QuantizeSettings gridPreset(int denominator, NoteValueKind kind)
		{
			QuantizeSettings settings;
			settings.grid = {denominator, kind};
			return settings;
		}
	}

	time::Ticks NoteValue::ticks() const
	{
		const time::Ticks straight = time::g_ticksPerQuarter * 4 / denominator;
		switch (kind)
		{
		case NoteValueKind::Triplet:
			return straight * 2 / 3;
		case NoteValueKind::Dotted:
			return straight * 3 / 2;
		case NoteValueKind::Straight:
			break;
		}
		return straight;
	}

	std::string toString(const NoteValue& value)
	{
		std::string text = "1/" + std::to_string(value.denominator);
		if (value.kind == NoteValueKind::Triplet)
			text += 'T';
		else if (value.kind == NoteValueKind::Dotted)
			text += '.';
		return text;
	}

	std::optional<NoteValue> noteValueFromString(const std::string& text)
	{
		if (text.size() < 3 || text.compare(0, 2, "1/") != 0)
			return std::nullopt;
		NoteValue value;
		std::string digits = text.substr(2);
		if (digits.back() == 'T' || digits.back() == '.')
		{
			value.kind = digits.back() == 'T' ? NoteValueKind::Triplet : NoteValueKind::Dotted;
			digits.pop_back();
		}
		if (digits.empty() || digits.size() > 3 ||
			!std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
			return std::nullopt;
		value.denominator = std::stoi(digits);
		if (!isPowerOfTwo(value.denominator) || value.denominator > g_largestDenominator)
			return std::nullopt;
		return value;
	}

	Json toJson(const Groove& groove)
	{
		Json points = Json::array();
		for (const GroovePoint& point : groove.points)
		{
			Json item = {{"position", point.position}};
			if (point.velocity)
				item["velocity"] = *point.velocity / g_maxVelocity;
			if (point.length)
				item["length"] = *point.length;
			points.push_back(std::move(item));
		}
		return {{"length", groove.length}, {"points", std::move(points)}};
	}

	Json toJson(const QuantizeSettings& settings)
	{
		return {{"grid", toString(settings.grid)}, {"tuplet", settings.tuplet}, {"swing", settings.swing},
			{"catchRange", settings.catchRange}, {"safeRange", settings.safeRange}, {"randomize", settings.randomize},
			{"seed", settings.seed}, {"soft", settings.soft}, {"strength", settings.strength},
			{"groove", settings.groove ? toJson(*settings.groove) : Json(nullptr)},
			{"preQuantize", settings.preQuantize ? Json(toString(*settings.preQuantize)) : Json(nullptr)},
			{"maxMove", settings.maxMove}, {"velocityStrength", settings.velocityStrength},
			{"lengthStrength", settings.lengthStrength}, {"origin", settings.origin}};
	}

	QuantizeSettings quantizeSettingsFromJson(const Json& json, QuantizeSettings base)
	{
		QuantizeSettings settings = std::move(base);
		const auto has = [&json](const char* key) { return json.contains(key); };
		if (has("grid"))
			settings.grid = noteValue(json, "grid");
		if (has("tuplet"))
		{
			settings.tuplet = json.at("tuplet").get<int>();
			require(settings.tuplet == 0 || (settings.tuplet >= 3 && settings.tuplet <= g_largestTuplet),
				"tuplet must be 0 (off) or 3 to " + std::to_string(g_largestTuplet));
		}
		if (has("swing"))
			settings.swing = unit(json, "swing");
		if (has("catchRange"))
			settings.catchRange = distance(json, "catchRange");
		if (has("safeRange"))
			settings.safeRange = distance(json, "safeRange");
		if (has("randomize"))
			settings.randomize = distance(json, "randomize");
		if (has("seed"))
		{
			const Json& seed = json.at("seed");
			require(seed.is_number_unsigned() || (seed.is_number_integer() && seed.get<int64_t>() >= 0),
				"seed must not be negative");
			settings.seed = seed.get<uint64_t>();
		}
		if (has("soft"))
			settings.soft = json.at("soft").get<bool>();
		if (has("strength"))
			settings.strength = unit(json, "strength");
		if (has("groove"))
			settings.groove =
				json.at("groove").is_null() ? std::nullopt : std::optional<Groove>(grooveFromJson(json.at("groove")));
		if (has("preQuantize"))
			settings.preQuantize = json.at("preQuantize").is_null()
				? std::nullopt
				: std::optional<NoteValue>(noteValue(json, "preQuantize"));
		if (has("maxMove"))
			settings.maxMove = distance(json, "maxMove");
		if (has("velocityStrength"))
			settings.velocityStrength = unit(json, "velocityStrength");
		if (has("lengthStrength"))
			settings.lengthStrength = unit(json, "lengthStrength");
		if (has("origin"))
			settings.origin = distance(json, "origin");
		return settings;
	}

	Json quantizeSettingsSchema()
	{
		const Json integer = {{"type", "integer"}};
		const Json number = {{"type", "number"}};
		const Json point = {{"type", "object"},
			{"properties",
				{{"position", integer}, {"velocity", {{"type", {"number", "null"}}}},
					{"length", {{"type", {"integer", "null"}}}}}},
			{"required", {"position"}}, {"additionalProperties", false}};
		const Json groove = {{"type", {"object", "null"}},
			{"properties", {{"length", integer}, {"points", {{"type", "array"}, {"items", point}}}}},
			{"required", {"length", "points"}}, {"additionalProperties", false}};
		return {{"type", "object"},
			{"properties",
				{{"grid", {{"type", "string"}}}, {"tuplet", integer}, {"swing", number}, {"catchRange", integer},
					{"safeRange", integer}, {"randomize", integer}, {"seed", integer}, {"soft", {{"type", "boolean"}}},
					{"strength", number}, {"groove", groove}, {"preQuantize", {{"type", {"string", "null"}}}},
					{"maxMove", integer}, {"velocityStrength", number}, {"lengthStrength", number},
					{"origin", integer}}},
			{"additionalProperties", false}};
	}

	const std::vector<QuantizePreset>& factoryQuantizePresets()
	{
		static const std::vector<QuantizePreset> presets = []
		{
			std::vector<QuantizePreset> list;
			for (int denominator = 1; denominator <= g_largestDenominator; denominator *= 2)
				list.push_back({"1/" + std::to_string(denominator), gridPreset(denominator, NoteValueKind::Straight)});
			for (int denominator = 2; denominator <= g_largestDenominator / 2; denominator *= 2)
				list.push_back(
					{"1/" + std::to_string(denominator) + " Triplet", gridPreset(denominator, NoteValueKind::Triplet)});
			for (int denominator = 2; denominator <= g_largestDenominator / 2; denominator *= 2)
				list.push_back(
					{"1/" + std::to_string(denominator) + " Dotted", gridPreset(denominator, NoteValueKind::Dotted)});
			return list;
		}();
		return presets;
	}
}
