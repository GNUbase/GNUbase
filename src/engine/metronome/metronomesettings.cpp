// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/metronome/metronomesettings.h"

#include "engine/engine.h"

#include <algorithm>
#include <cmath>
#include <tuple>
#include <utility>

namespace gnubase::engine::metronome
{
	namespace
	{
		const char* soundsName(Sounds sounds)
		{
			return sounds == Sounds::Samples ? "samples" : "beeps";
		}

		const char* countInSignatureName(CountInSignature signature)
		{
			switch (signature)
			{
			case CountInSignature::Project:
				return "project";
			case CountInSignature::Custom:
				return "custom";
			case CountInSignature::Start:
				break;
			}
			return "start";
		}

		Json signatureJson(time::TimeSignature signature)
		{
			return {{"numerator", signature.numerator}, {"denominator", signature.denominator}};
		}

		bool lessThan(time::TimeSignature a, time::TimeSignature b)
		{
			return std::tie(a.numerator, a.denominator) < std::tie(b.numerator, b.denominator);
		}

		// Reads properties of metronome.set or a project file, checking each value. A strict reader throws for an
		// invalid value; a lenient one reports it as missing, so the setting keeps its value.
		class Reader
		{
		public:
			explicit Reader(bool strict) : m_strict(strict) {}

			// The property if the object has it, or nullptr.
			const Json* find(const Json& object, const char* name) const
			{
				if (!object.is_object())
					return nullptr;
				const auto found = object.find(name);
				return found == object.end() ? nullptr : &*found;
			}

			bool fail(const std::string& message) const
			{
				if (m_strict)
					throw CommandError(message);
				return false;
			}

			void boolean(const Json& object, const char* name, bool& value) const
			{
				if (const Json* json = find(object, name); json != nullptr)
				{
					if (json->is_boolean())
						value = json->get<bool>();
					else
						fail(std::string(name) + " must be true or false");
				}
			}

			void number(const Json& object, const char* name, double& value, double min, double max) const
			{
				if (const Json* json = find(object, name); json != nullptr)
				{
					const double read = json->is_number() ? json->get<double>() : std::nan("");
					if (std::isfinite(read) && read >= min && read <= max)
						value = read;
					else
						fail(std::string(name) + " must be a number from " + text(min) + " to " + text(max));
				}
			}

			void integer(const Json& object, const char* name, int& value, int min, int max) const
			{
				if (const Json* json = find(object, name); json != nullptr)
				{
					if (json->is_number_integer() && json->get<int64_t>() >= min && json->get<int64_t>() <= max)
						value = json->get<int>();
					else
						fail(std::string(name) + " must be an integer from " + std::to_string(min) + " to " +
							std::to_string(max));
				}
			}

			bool signature(const Json& json, const std::string& name, time::TimeSignature& value) const
			{
				time::TimeSignature read{0, 0};
				integer(json, "numerator", read.numerator, 1, g_maxClicks);
				integer(json, "denominator", read.denominator, 1, 16);
				if (read.numerator == 0 || read.denominator == 0 || (read.denominator & (read.denominator - 1)) != 0)
					return fail(name + " must have a numerator from 1 to " + std::to_string(g_maxClicks) +
						" and a denominator of 1, 2, 4, 8 or 16");
				value = read;
				return true;
			}

			// An array of accent objects: the given properties of each one, by index.
			template <typename T, typename Read>
			void accents(const Json& object, const char* name, std::array<T, g_accents>& values, Read read) const
			{
				const Json* json = find(object, name);
				if (json == nullptr)
					return;
				if (!json->is_array() || json->size() > g_accents)
				{
					fail(std::string(name) + " must be an array of up to " + std::to_string(g_accents) + " objects");
					return;
				}
				for (size_t i = 0; i < json->size(); ++i)
					read((*json)[i], values[i]);
			}

			bool pattern(const Json& json, ClickPattern& pattern) const
			{
				if (!signature(json, "a pattern", pattern.signature))
					return false;
				const Json* accents = find(json, "accents");
				if (accents == nullptr || !accents->is_array() || accents->empty() || accents->size() > g_maxClicks)
					return fail(
						"a pattern's accents must be an array of 1 to " + std::to_string(g_maxClicks) + " accents");
				pattern.accents.clear();
				for (const Json& accent : *accents)
				{
					if (!accent.is_number_integer() || accent.get<int64_t>() < 0 || accent.get<int64_t>() > g_accents)
						return fail("accents must be integers from 0 (no click) to " + std::to_string(g_accents));
					pattern.accents.push_back(accent.get<uint8_t>());
				}
				return true;
			}

		private:
			static std::string text(double value)
			{
				const Json json = value;
				return json.dump();
			}

			bool m_strict;
		};

		void sortPatterns(std::vector<ClickPattern>& patterns)
		{
			std::stable_sort(patterns.begin(), patterns.end(),
				[](const ClickPattern& a, const ClickPattern& b) { return lessThan(a.signature, b.signature); });
			// A later pattern of the same signature wins.
			std::vector<ClickPattern> unique;
			for (ClickPattern& pattern : patterns)
				if (!unique.empty() && unique.back().signature == pattern.signature)
					unique.back() = std::move(pattern);
				else
					unique.push_back(std::move(pattern));
			patterns = std::move(unique);
		}
	}

	bool operator==(const MetronomeSettings& a, const MetronomeSettings& b)
	{
		const auto beep = [](const Beep& x, const Beep& y) { return x.frequency == y.frequency && x.level == y.level; };
		const auto sample = [](const Sample& x, const Sample& y) { return x.path == y.path && x.level == y.level; };
		const auto note = [](const MidiNote& x, const MidiNote& y)
		{ return x.note == y.note && x.velocity == y.velocity; };
		const auto pattern = [](const ClickPattern& x, const ClickPattern& y)
		{ return x.signature == y.signature && x.accents == y.accents; };
		return a.click == b.click && a.whilePlaying == b.whilePlaying && a.whileRecording == b.whileRecording &&
			a.audio == b.audio && a.outputs == b.outputs && a.level == b.level && a.sounds == b.sounds &&
			std::equal(a.beeps.begin(), a.beeps.end(), b.beeps.begin(), beep) &&
			std::equal(a.samples.begin(), a.samples.end(), b.samples.begin(), sample) && a.midi == b.midi &&
			a.destination == b.destination && a.channel == b.channel &&
			std::equal(a.notes.begin(), a.notes.end(), b.notes.begin(), note) && a.countIn == b.countIn &&
			a.countInBars == b.countInBars && a.countInSignature == b.countInSignature &&
			a.customSignature == b.customSignature &&
			std::equal(a.patterns.begin(), a.patterns.end(), b.patterns.begin(), b.patterns.end(), pattern);
	}

	ClickPattern defaultPattern(time::TimeSignature signature)
	{
		ClickPattern pattern{signature, std::vector<uint8_t>(static_cast<size_t>(std::max(signature.numerator, 1)), 2)};
		pattern.accents.front() = 1;
		return pattern;
	}

	ClickPattern patternFor(const MetronomeSettings& settings, time::TimeSignature signature)
	{
		for (const ClickPattern& pattern : settings.patterns)
			if (pattern.signature == signature)
				return pattern;
		return defaultPattern(signature);
	}

	Json toJson(const MetronomeSettings& settings)
	{
		Json beeps = Json::array();
		for (const Beep& beep : settings.beeps)
			beeps.push_back({{"frequency", beep.frequency}, {"level", beep.level}});
		Json samples = Json::array();
		for (const Sample& sample : settings.samples)
			samples.push_back(
				{{"path", sample.path.empty() ? Json(nullptr) : Json(sample.path)}, {"level", sample.level}});
		Json notes = Json::array();
		for (const MidiNote& note : settings.notes)
			notes.push_back({{"note", note.note}, {"velocity", note.velocity}});
		Json patterns = Json::array();
		for (const ClickPattern& pattern : settings.patterns)
		{
			Json item = signatureJson(pattern.signature);
			item["accents"] = pattern.accents;
			patterns.push_back(std::move(item));
		}
		return {{"click", settings.click}, {"whilePlaying", settings.whilePlaying},
			{"whileRecording", settings.whileRecording},
			{"audio",
				{{"enabled", settings.audio}, {"outputs", settings.outputs}, {"level", settings.level},
					{"sounds", soundsName(settings.sounds)}, {"beeps", std::move(beeps)},
					{"samples", std::move(samples)}}},
			{"midi",
				{{"enabled", settings.midi},
					{"destination", settings.destination == 0 ? Json(nullptr) : Json(settings.destination)},
					{"channel", settings.channel}, {"notes", std::move(notes)}}},
			{"countIn",
				{{"enabled", settings.countIn}, {"bars", settings.countInBars},
					{"signature", countInSignatureName(settings.countInSignature)},
					{"customSignature", signatureJson(settings.customSignature)}}},
			{"patterns", std::move(patterns)}};
	}

	Json settingsSchema()
	{
		const Json boolean = {{"type", "boolean"}};
		const Json number = {{"type", "number"}};
		const Json integer = {{"type", "integer"}};
		const auto object = [](Json properties)
		{ return Json{{"type", "object"}, {"additionalProperties", false}, {"properties", std::move(properties)}}; };
		const auto array = [](Json items) { return Json{{"type", "array"}, {"items", std::move(items)}}; };
		const Json signature = object({{"numerator", integer}, {"denominator", integer}});
		return object({{"click", boolean}, {"whilePlaying", boolean}, {"whileRecording", boolean},
			{"audio",
				object({{"enabled", boolean}, {"outputs", array(integer)}, {"level", number},
					{"sounds", {{"type", "string"}, {"enum", {"beeps", "samples"}}}},
					{"beeps", array(object({{"frequency", number}, {"level", number}}))},
					{"samples", array(object({{"path", {{"type", {"string", "null"}}}}, {"level", number}}))}})},
			{"midi",
				object({{"enabled", boolean}, {"destination", {{"type", {"integer", "null"}}}}, {"channel", integer},
					{"notes", array(object({{"note", integer}, {"velocity", number}}))}})},
			{"countIn",
				object({{"enabled", boolean}, {"bars", integer},
					{"signature", {{"type", "string"}, {"enum", {"start", "project", "custom"}}}},
					{"customSignature", signature}})},
			{"patterns",
				array(object({{"numerator", integer}, {"denominator", integer}, {"accents", array(integer)}}))}});
	}

	void apply(MetronomeSettings& settings, const Json& json, bool strict)
	{
		const Reader read(strict);
		MetronomeSettings result = settings;
		read.boolean(json, "click", result.click);
		read.boolean(json, "whilePlaying", result.whilePlaying);
		read.boolean(json, "whileRecording", result.whileRecording);

		if (const Json* audio = read.find(json, "audio"); audio != nullptr)
		{
			read.boolean(*audio, "enabled", result.audio);
			if (const Json* outputs = read.find(*audio, "outputs"); outputs != nullptr)
			{
				const bool valid = outputs->is_array() &&
					std::all_of(outputs->begin(), outputs->end(),
						[](const Json& id) { return id.is_number_integer() && id.get<int64_t>() > 0; });
				if (valid)
					result.outputs = outputs->get<std::vector<ObjectId>>();
				else
					read.fail("outputs must be an array of output bus ids");
			}
			read.number(*audio, "level", result.level, g_minLevel, g_maxLevel);
			if (const Json* sounds = read.find(*audio, "sounds"); sounds != nullptr)
			{
				if (*sounds == "beeps" || *sounds == "samples")
					result.sounds = *sounds == "samples" ? Sounds::Samples : Sounds::Beeps;
				else
					read.fail("sounds must be beeps or samples");
			}
			read.accents(*audio, "beeps", result.beeps,
				[&read](const Json& item, Beep& beep)
				{
					read.number(item, "frequency", beep.frequency, g_minFrequency, g_maxFrequency);
					read.number(item, "level", beep.level, g_minLevel, g_maxLevel);
				});
			read.accents(*audio, "samples", result.samples,
				[&read](const Json& item, Sample& sample)
				{
					if (const Json* path = read.find(item, "path"); path != nullptr)
					{
						if (path->is_null() || path->is_string())
							sample.path = path->is_null() ? std::string() : path->get<std::string>();
						else
							read.fail("a sample's path must be a string or null");
					}
					read.number(item, "level", sample.level, g_minLevel, g_maxLevel);
				});
		}

		if (const Json* midi = read.find(json, "midi"); midi != nullptr)
		{
			read.boolean(*midi, "enabled", result.midi);
			if (const Json* destination = read.find(*midi, "destination"); destination != nullptr)
			{
				if (destination->is_null())
					result.destination = 0;
				else if (destination->is_number_integer() && destination->get<int64_t>() > 0)
					result.destination = destination->get<ObjectId>();
				else
					read.fail("destination must be an instrument channel's id or null");
			}
			read.integer(*midi, "channel", result.channel, 1, 16);
			read.accents(*midi, "notes", result.notes,
				[&read](const Json& item, MidiNote& note)
				{
					read.integer(item, "note", note.note, 0, 127);
					read.number(item, "velocity", note.velocity, 0.0, 1.0);
				});
		}

		if (const Json* countIn = read.find(json, "countIn"); countIn != nullptr)
		{
			read.boolean(*countIn, "enabled", result.countIn);
			read.integer(*countIn, "bars", result.countInBars, 1, g_maxCountInBars);
			if (const Json* signature = read.find(*countIn, "signature"); signature != nullptr)
			{
				if (*signature == "start")
					result.countInSignature = CountInSignature::Start;
				else if (*signature == "project")
					result.countInSignature = CountInSignature::Project;
				else if (*signature == "custom")
					result.countInSignature = CountInSignature::Custom;
				else
					read.fail("the count-in's signature must be start, project or custom");
			}
			if (const Json* custom = read.find(*countIn, "customSignature"); custom != nullptr)
				read.signature(*custom, "customSignature", result.customSignature);
		}

		if (const Json* patterns = read.find(json, "patterns"); patterns != nullptr)
		{
			if (patterns->is_array())
			{
				std::vector<ClickPattern> list;
				for (const Json& item : *patterns)
					if (ClickPattern pattern; read.pattern(item, pattern))
						list.push_back(std::move(pattern));
				sortPatterns(list);
				result.patterns = std::move(list);
			}
			else
			{
				read.fail("patterns must be an array");
			}
		}
		settings = std::move(result);
	}

	void setPattern(MetronomeSettings& settings, time::TimeSignature signature, const std::vector<int>& accents)
	{
		ClickPattern pattern;
		if (!accents.empty())
		{
			Json json = signatureJson(signature);
			json["accents"] = accents;
			Reader(true).pattern(json, pattern);
		}
		std::vector<ClickPattern>& patterns = settings.patterns;
		patterns.erase(std::remove_if(patterns.begin(), patterns.end(),
						   [signature](const ClickPattern& existing) { return existing.signature == signature; }),
			patterns.end());
		if (accents.empty())
			return;
		patterns.push_back(std::move(pattern));
		sortPatterns(patterns);
	}
}
