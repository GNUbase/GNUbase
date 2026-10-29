// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/processors/builtin.h"

#include "engine/engine.h"
#include "engine/processors/gainpan.h"
#include "engine/processors/generators.h"
#include "engine/processors/testsynth.h"

#include <functional>
#include <stdexcept>
#include <utility>

namespace gnubase::engine::processors
{
	namespace
	{
		struct Builtin
		{
			BuiltinInfo info;
			std::function<std::shared_ptr<graph::Processor>()> create;
		};

		// Parameters are listed in the order of their ids.
		const std::vector<Builtin>& registry()
		{
			static const std::vector<Builtin> builtins = {
				{{"gainPan", "Gain and pan",
					 "Gain in dB and, on stereo channels, balance with unity gain in the center.", false,
					 {{GainPan::Gain, "Gain", -96.0, 12.0, 0.0, "dB"}, {GainPan::Pan, "Pan", -1.0, 1.0, 0.0, ""}}},
					[] { return std::make_shared<GainPan>(); }},
				{{"impulse", "Impulse generator", "Single samples at the level, repeating at the interval.", false,
					 {{Impulse::Interval, "Interval", 0.001, 10.0, 1.0, "s"},
						 {Impulse::Level, "Level", 0.0, 1.0, 1.0, ""}}},
					[] { return std::make_shared<Impulse>(); }},
				{{"noise", "Noise generator", "White noise, the same samples every time.", false,
					 {{Noise::Level, "Level", 0.0, 1.0, 0.5, ""}}},
					[] { return std::make_shared<Noise>(); }},
				{{"noteProbe", "Note probe",
					 "An instrument for tests: each frame, how many notes are on (channel 0 and from channel 2 on) and "
					 "the sum of their pitches (channel 1).",
					 true, {}},
					[] { return std::make_shared<NoteProbe>(); }},
				{{"positionProbe", "Position probe",
					 "Each frame's project position in samples while playing, -1 while stopped. For tests.", false, {}},
					[] { return std::make_shared<PositionProbe>(); }},
				{{"sine", "Sine generator", "A sine wave at the frequency and level.", false,
					 {{Sine::Frequency, "Frequency", 1.0, 20000.0, 440.0, "Hz"},
						 {Sine::Level, "Level", 0.0, 1.0, 0.5, ""}}},
					[] { return std::make_shared<Sine>(); }},
				{{"testSynth", "Test synth",
					 "A simple polyphonic sine synth with 16 voices, linear attack and release, for tests.", true,
					 {{TestSynth::Level, "Level", 0.0, 1.0, 0.25, ""},
						 {TestSynth::Attack, "Attack", 0.0, 10.0, 0.005, "s"},
						 {TestSynth::Release, "Release", 0.0, 10.0, 0.05, "s"}}},
					[] { return std::make_shared<TestSynth>(); }},
			};
			return builtins;
		}

		Json toJson(const BuiltinInfo& info)
		{
			Json parameters = Json::array();
			for (const ParameterInfo& parameter : info.parameters)
				parameters.push_back({{"id", parameter.id}, {"name", parameter.name}, {"minimum", parameter.minimum},
					{"maximum", parameter.maximum}, {"default", parameter.defaultValue}, {"unit", parameter.unit}});
			return {{"id", info.id}, {"name", info.name}, {"description", info.description},
				{"instrument", info.instrument}, {"parameters", std::move(parameters)}};
		}
	}

	const std::vector<BuiltinInfo>& builtins()
	{
		static const std::vector<BuiltinInfo> infos = []
		{
			std::vector<BuiltinInfo> result;
			for (const Builtin& builtin : registry())
				result.push_back(builtin.info);
			return result;
		}();
		return infos;
	}

	std::shared_ptr<graph::Processor> createBuiltin(const std::string& id)
	{
		for (const Builtin& builtin : registry())
			if (builtin.info.id == id)
				return builtin.create();
		throw std::invalid_argument("no built-in processor " + id);
	}

	bool isInstrument(const std::string& id)
	{
		for (const Builtin& builtin : registry())
			if (builtin.info.id == id)
				return builtin.info.instrument;
		return false;
	}

	void registerCommands(Engine& engine)
	{
		engine.registerCommand(
			{"processor.list", 1,
				"Every built-in processor: its id, name, description, whether it is an instrument, and its parameters "
				"with their ids, ranges, defaults and units.",
				CommandInfo{}.parameters,
				Json::parse(R"({"type": "object", "properties": {"processors": {"type": "array"}},
					"required": ["processors"]})"),
				true},
			[](CommandContext&)
			{
				Json list = Json::array();
				for (const BuiltinInfo& info : builtins())
					list.push_back(toJson(info));
				return Json{{"processors", std::move(list)}};
			});
	}
}
