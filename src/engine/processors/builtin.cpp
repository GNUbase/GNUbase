// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/processors/builtin.h"

#include "engine/engine.h"
#include "engine/processors/effects.h"
#include "engine/processors/gainpan.h"
#include "engine/processors/generators.h"
#include "engine/processors/testsynth.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <iomanip>
#include <locale>
#include <sstream>
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

		ParameterInfo number(
			uint32_t id, std::string name, double minimum, double maximum, double defaultValue, std::string unit = {})
		{
			return {id, std::move(name), minimum, maximum, defaultValue, std::move(unit), false, {}};
		}

		ParameterInfo whole(
			uint32_t id, std::string name, double minimum, double maximum, double defaultValue, std::string unit = {})
		{
			return {id, std::move(name), minimum, maximum, defaultValue, std::move(unit), true, {}};
		}

		ParameterInfo choice(uint32_t id, std::string name, std::vector<std::string> options, double defaultValue)
		{
			const auto maximum = static_cast<double>(options.size() - 1);
			return {id, std::move(name), 0.0, maximum, defaultValue, {}, true, std::move(options)};
		}

		ParameterInfo onOff(uint32_t id, std::string name, bool on)
		{
			return choice(id, std::move(name), {"Off", "On"}, on ? 1.0 : 0.0);
		}

		std::vector<ParameterInfo> equalizerParameters()
		{
			using dsp::Equalizer;
			std::vector<ParameterInfo> parameters = {number(Equalizer::Output, "Output", -24.0, 24.0, 0.0, "dB")};
			for (int band = 0; band < Equalizer::g_bands; ++band)
			{
				const Equalizer::BandSettings settings = Equalizer::defaultBand(band);
				const std::string prefix = "Band " + std::to_string(band + 1) + " ";
				parameters.push_back(onOff(Equalizer::parameter(band, Equalizer::On), prefix + "on", settings.on));
				parameters.push_back(choice(Equalizer::parameter(band, Equalizer::Type), prefix + "type",
					{"Bell", "Low shelf", "High shelf", "Low cut", "High cut"}, static_cast<double>(settings.type)));
				parameters.push_back(number(Equalizer::parameter(band, Equalizer::Frequency), prefix + "frequency",
					20.0, 20000.0, settings.frequency, "Hz"));
				parameters.push_back(number(
					Equalizer::parameter(band, Equalizer::Gain), prefix + "gain", -24.0, 24.0, settings.gain, "dB"));
				parameters.push_back(
					number(Equalizer::parameter(band, Equalizer::Q), prefix + "Q", 0.1, 12.0, settings.q));
				parameters.push_back(choice(Equalizer::parameter(band, Equalizer::Slope), prefix + "slope",
					{"6 dB/oct", "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct"},
					static_cast<double>(settings.slope)));
			}
			return parameters;
		}

		std::vector<std::string> noteNames()
		{
			std::vector<std::string> names;
			for (const dsp::Delay::NoteValue& note : dsp::Delay::noteValues())
				names.emplace_back(note.name);
			return names;
		}

		template <typename T> std::function<std::shared_ptr<graph::Processor>()> make()
		{
			return [] { return std::make_shared<T>(); };
		}

		// Sorted by id; parameters are listed in the order of their ids.
		const std::vector<Builtin>& registry()
		{
			static const std::vector<Builtin> builtins = {
				{{"compressor", "Compressor",
					 "A compressor with a soft knee, peak or RMS detection and makeup gain. Channels are linked.",
					 false,
					 {number(dsp::Compressor::Threshold, "Threshold", -60.0, 0.0, -20.0, "dB"),
						 number(dsp::Compressor::Ratio, "Ratio", 1.0, 20.0, 4.0),
						 number(dsp::Compressor::Knee, "Knee", 0.0, 24.0, 6.0, "dB"),
						 number(dsp::Compressor::Attack, "Attack", 0.1, 100.0, 10.0, "ms"),
						 number(dsp::Compressor::Release, "Release", 10.0, 2000.0, 100.0, "ms"),
						 number(dsp::Compressor::Makeup, "Makeup", 0.0, 24.0, 0.0, "dB"),
						 choice(dsp::Compressor::Detection, "Detection", {"Peak", "RMS"}, 0.0)}},
					make<Compressor>()},
				{{"delay", "Delay",
					 "A stereo delay with feedback, its time in ms or synced to the tempo, and ping-pong echoes.",
					 false,
					 {number(dsp::Delay::Time, "Time", 1.0, 5000.0, 500.0, "ms"),
						 onOff(dsp::Delay::Sync, "Sync", false),
						 choice(dsp::Delay::Note, "Note", noteNames(), dsp::Delay::g_defaultNote),
						 number(dsp::Delay::Feedback, "Feedback", 0.0, 100.0, 30.0, "%"),
						 number(dsp::Delay::Mix, "Mix", 0.0, 100.0, 30.0, "%"),
						 onOff(dsp::Delay::PingPong, "Ping-pong", false)}},
					make<Delay>()},
				{{"dither", "Dither",
					 "Reduces the word length to 8 to 24 bits with TPDF dither and optional noise shaping.", false,
					 {whole(dsp::Dither::Bits, "Bits", 8.0, 24.0, 16.0),
						 choice(
							 dsp::Dither::NoiseShaping, "Noise shaping", {"Off", "First order", "Second order"}, 0.0)}},
					make<Dither>()},
				{{"eq", "Parametric EQ",
					 "Six bands, each a bell, low or high shelf, or low or high cut with a slope of 6 to 48 dB/oct.",
					 false, equalizerParameters()},
					make<Equalizer>()},
				{{"gainPan", "Gain and pan",
					 "Gain in dB and, on stereo channels, balance with unity gain in the center.", false,
					 {number(GainPan::Gain, "Gain", -96.0, 12.0, 0.0, "dB"),
						 number(GainPan::Pan, "Pan", -1.0, 1.0, 0.0)}},
					make<GainPan>()},
				{{"gate", "Gate", "A noise gate with range, attack, hold and release. Channels are linked.", false,
					 {number(dsp::Gate::Threshold, "Threshold", -80.0, 0.0, -40.0, "dB"),
						 number(dsp::Gate::Range, "Range", -80.0, 0.0, -80.0, "dB"),
						 number(dsp::Gate::Attack, "Attack", 0.01, 100.0, 0.1, "ms"),
						 number(dsp::Gate::Hold, "Hold", 0.0, 2000.0, 20.0, "ms"),
						 number(dsp::Gate::Release, "Release", 1.0, 4000.0, 100.0, "ms")}},
					make<Gate>()},
				{{"impulse", "Impulse generator", "Single samples at the level, repeating at the interval.", false,
					 {number(Impulse::Interval, "Interval", 0.001, 10.0, 1.0, "s"),
						 number(Impulse::Level, "Level", 0.0, 1.0, 1.0)}},
					make<Impulse>()},
				{{"limiter", "Limiter",
					 "A look-ahead peak limiter: no sample exceeds the ceiling. Adds 1 ms of latency.", false,
					 {number(dsp::Limiter::Input, "Input", 0.0, 24.0, 0.0, "dB"),
						 number(dsp::Limiter::Ceiling, "Ceiling", -30.0, 0.0, -0.3, "dB"),
						 number(dsp::Limiter::Release, "Release", 1.0, 1000.0, 50.0, "ms")}},
					make<Limiter>()},
				{{"noise", "Noise generator", "White noise, the same samples every time.", false,
					 {number(Noise::Level, "Level", 0.0, 1.0, 0.5)}},
					make<Noise>()},
				{{"noteProbe", "Note probe",
					 "An instrument for tests: each frame, how many notes are on (channel 0 and from channel 2 on) and "
					 "the sum of their pitches (channel 1).",
					 true, {}},
					make<NoteProbe>()},
				{{"positionProbe", "Position probe",
					 "Each frame's project position in samples while playing, -1 while stopped. For tests.", false, {}},
					make<PositionProbe>()},
				{{"reverb", "Reverb", "An algorithmic reverb with room size, decay time, damping, predelay and mix.",
					 false,
					 {number(dsp::Reverb::Size, "Size", 0.0, 100.0, 50.0, "%"),
						 number(dsp::Reverb::Decay, "Decay", 0.1, 20.0, 2.0, "s"),
						 number(dsp::Reverb::Damping, "Damping", 0.0, 100.0, 50.0, "%"),
						 number(dsp::Reverb::Predelay, "Predelay", 0.0, 200.0, 20.0, "ms"),
						 number(dsp::Reverb::Mix, "Mix", 0.0, 100.0, 25.0, "%")}},
					make<Reverb>()},
				{{"sine", "Sine generator", "A sine wave at the frequency and level.", false,
					 {number(Sine::Frequency, "Frequency", 1.0, 20000.0, 440.0, "Hz"),
						 number(Sine::Level, "Level", 0.0, 1.0, 0.5)}},
					make<Sine>()},
				{{"testSynth", "Test synth",
					 "A simple polyphonic sine synth with 16 voices, linear attack and release, for tests.", true,
					 {number(TestSynth::Level, "Level", 0.0, 1.0, 0.25),
						 number(TestSynth::Attack, "Attack", 0.0, 10.0, 0.005, "s"),
						 number(TestSynth::Release, "Release", 0.0, 10.0, 0.05, "s")}},
					make<TestSynth>()},
			};
			return builtins;
		}

		std::string_view trimmed(std::string_view text)
		{
			const auto space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
			while (!text.empty() && space(text.front()))
				text.remove_prefix(1);
			while (!text.empty() && space(text.back()))
				text.remove_suffix(1);
			return text;
		}

		bool sameIgnoringCase(std::string_view a, std::string_view b)
		{
			return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y)
				{ return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
		}

		Json toJson(const BuiltinInfo& info)
		{
			Json parameters = Json::array();
			for (const ParameterInfo& parameter : info.parameters)
				parameters.push_back({{"id", parameter.id}, {"name", parameter.name}, {"minimum", parameter.minimum},
					{"maximum", parameter.maximum}, {"default", parameter.defaultValue}, {"unit", parameter.unit},
					{"stepped", parameter.stepped}, {"options", parameter.options}});
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

	const BuiltinInfo* findBuiltin(const std::string& id)
	{
		const std::vector<BuiltinInfo>& infos = builtins();
		const auto found =
			std::find_if(infos.begin(), infos.end(), [&id](const BuiltinInfo& info) { return info.id == id; });
		return found != infos.end() ? &*found : nullptr;
	}

	const ParameterInfo* findParameter(const BuiltinInfo& info, uint32_t id)
	{
		const auto found = std::find_if(info.parameters.begin(), info.parameters.end(),
			[id](const ParameterInfo& parameter) { return parameter.id == id; });
		return found != info.parameters.end() ? &*found : nullptr;
	}

	std::string valueToText(const ParameterInfo& parameter, double value)
	{
		if (!parameter.options.empty())
		{
			const auto option = static_cast<size_t>(std::clamp(
				std::round(value - parameter.minimum), 0.0, static_cast<double>(parameter.options.size() - 1)));
			return parameter.options[option];
		}
		std::ostringstream text;
		text.imbue(std::locale::classic());
		text << std::fixed << std::setprecision(parameter.stepped ? 0 : 2) << value;
		if (!parameter.unit.empty())
			text << ' ' << parameter.unit;
		return text.str();
	}

	std::optional<double> textToValue(const ParameterInfo& parameter, std::string_view text)
	{
		text = trimmed(text);
		for (size_t i = 0; i < parameter.options.size(); ++i)
			if (sameIgnoringCase(text, parameter.options[i]))
				return parameter.minimum + static_cast<double>(i);
		std::istringstream stream{std::string(text)};
		stream.imbue(std::locale::classic());
		double value = 0.0;
		if (!(stream >> value) || !std::isfinite(value))
			return std::nullopt;
		std::string rest;
		std::getline(stream, rest);
		if (!trimmed(rest).empty() && !sameIgnoringCase(trimmed(rest), parameter.unit))
			return std::nullopt;
		value = std::clamp(value, parameter.minimum, parameter.maximum);
		return parameter.stepped ? std::round(value) : value;
	}

	void registerCommands(Engine& engine)
	{
		engine.registerCommand(
			{"processor.list", 1,
				"Every built-in processor: its id, name, description, whether it is an instrument, and its parameters "
				"with their ids, ranges, defaults and units. A stepped parameter takes whole numbers only; its "
				"options, if any, name its values from the minimum on.",
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
