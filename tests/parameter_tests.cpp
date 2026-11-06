// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/processors/builtin.h"
#include "engine/processors/testsynth.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;

namespace
{
	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	std::string reject(Client& client, const std::string& command, Json parameters)
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " succeeded: " << result.value.dump());
		REQUIRE(!result.ok());
		return result.error;
	}

	// A track of a type and its channel.
	struct TestTrack
	{
		ObjectId track = 0;
		ObjectId channel = 0;
	};

	TestTrack addTrack(Client& client, const char* type)
	{
		const Json added = run(client, "track.add", {{"type", type}});
		return {added.at("track").at("id").get<ObjectId>(), added.at("channel").at("id").get<ObjectId>()};
	}

	TestTrack addSynth(Client& client)
	{
		const TestTrack synth = addTrack(client, "instrument");
		run(client, "channel.set", {{"channel", synth.channel}, {"instrument", "testSynth"}});
		return synth;
	}

	Json instrumentTarget(uint32_t parameter)
	{
		return {{"type", "instrument"}, {"parameter", parameter}};
	}

	Json get(Client& client, ObjectId channel, const Json& target)
	{
		return run(client, "parameter.get", {{"channel", channel}, {"target", target}}).at("parameter");
	}

	Json set(Client& client, ObjectId channel, const Json& target, const char* key, const Json& value)
	{
		return run(client, "parameter.set", {{"channel", channel}, {"target", target}, {key, value}}).at("parameter");
	}

	// The channel strip's section parameters of an audio channel (docs/features/channel-strip.md).
	constexpr size_t g_stripParameterCount = 41;

	Json stripTarget(uint32_t parameter)
	{
		return {{"type", "strip"}, {"parameter", parameter}};
	}

	// A copy: GCC warns about references from a function that takes temporaries.
	processors::ParameterInfo builtinParameter(const std::string& processor, const std::string& name)
	{
		const processors::BuiltinInfo* info = processors::findBuiltin(processor);
		REQUIRE(info != nullptr);
		const auto found = std::find_if(info->parameters.begin(), info->parameters.end(),
			[&name](const processors::ParameterInfo& parameter) { return parameter.name == name; });
		REQUIRE(found != info->parameters.end());
		return *found;
	}
}

TEST_CASE("parameter.list returns a channel's parameters with their ranges, values and display text", "[unit]")
{
	Engine engine;
	Client client(engine);
	const TestTrack audio = addTrack(client, "audio");
	const Json parameters = run(client, "parameter.list", {{"channel", audio.channel}}).at("parameters");
	std::vector<std::string> types;
	for (const Json& parameter : parameters)
		if (parameter.at("target").at("type") != "strip") // see the test of the strip's parameters
			types.push_back(parameter.at("target").at("type").get<std::string>());
	CHECK(types == std::vector<std::string>{"volume", "pan", "panLeft", "panRight", "mute"});

	const Json& volume = parameters.at(0);
	CHECK(volume.at("name") == "Volume");
	CHECK(volume.at("minimum") == -144.0);
	CHECK(volume.at("maximum") == 6.0); // the project's maxVolume
	CHECK(volume.at("default") == 0.0);
	CHECK(volume.at("unit") == "dB");
	CHECK(volume.at("value") == 0.0);
	CHECK(volume.at("text") == "0.00 dB");
	CHECK(parameters.at(2).at("value") == -1.0);
	CHECK(parameters.at(2).at("text") == "-1.00");
	// Mute is the channel's mute button, a stepped parameter.
	const Json& mute = parameters.at(4);
	CHECK(mute.at("stepped") == true);
	CHECK(mute.at("options") == Json::array({"Off", "On"}));
	CHECK(mute.at("value") == 0.0);
	set(client, audio.channel, {{"type", "mute"}}, "value", 1.0);
	CHECK(get(client, audio.channel, {{"type", "mute"}}).at("text") == "On");
	set(client, audio.channel, {{"type", "mute"}}, "value", 0.0);

	run(client, "project.set", {{"maxVolume", 12.0}});
	run(client, "channel.set", {{"channel", audio.channel}, {"volume", -6.0}, {"pan", 0.25}});
	CHECK(get(client, audio.channel, {{"type", "volume"}}).at("maximum") == 12.0);
	CHECK(get(client, audio.channel, {{"type", "volume"}}).at("text") == "-6.00 dB");
	CHECK(get(client, audio.channel, {{"type", "pan"}}).at("value") == 0.25);

	// An instrument track's channel also has its instrument's parameters, with targets automation lanes take.
	const TestTrack synth = addSynth(client);
	const Json synthParameters = run(client, "parameter.list", {{"channel", synth.channel}}).at("parameters");
	REQUIRE(synthParameters.size() == 8 + g_stripParameterCount);
	const Json& level = synthParameters.at(5);
	CHECK(level.at("target") == instrumentTarget(processors::TestSynth::Level));
	CHECK(level.at("name") == "Level");
	CHECK(level.at("value") == 0.25);
	CHECK(level.at("text") == "0.25");
	CHECK(synthParameters.at(6).at("unit") == "s");
	run(client, "automationLane.add", {{"track", synth.track}, {"target", level.at("target")}});

	// MIDI channels have none: their volume and pan are MIDI controllers (channel.set).
	const TestTrack midi = addTrack(client, "midi");
	CHECK(run(client, "parameter.list", {{"channel", midi.channel}}).at("parameters").empty());
	CHECK_THAT(reject(client, "parameter.list", {{"channel", 9999}}), ContainsSubstring("9999"));
}

TEST_CASE("the channel strip's sections have parameters that parameter.set and channel.set change alike", "[unit]")
{
	Engine engine;
	Client client(engine);
	const TestTrack audio = addTrack(client, "audio");
	std::vector<Json> strip;
	const Json parameters = run(client, "parameter.list", {{"channel", audio.channel}}).at("parameters");
	for (const Json& parameter : parameters)
		if (parameter.at("target").at("type") == "strip")
			strip.push_back(parameter);
	REQUIRE(strip.size() == g_stripParameterCount);
	CHECK(strip.at(0).at("target") == stripTarget(0));
	CHECK(strip.at(0).at("name") == "Pre Gain");
	CHECK(strip.at(0).at("minimum") == -24.0);
	CHECK(strip.at(0).at("maximum") == 24.0);
	CHECK(strip.at(0).at("unit") == "dB");
	const Json slope = get(client, audio.channel, stripTarget(4));
	CHECK(slope.at("name") == "Low Cut Slope");
	CHECK(slope.at("stepped") == true);
	CHECK(slope.at("options") == Json::array({"6 dB/oct", "12 dB/oct", "24 dB/oct", "36 dB/oct", "48 dB/oct"}));
	CHECK(slope.at("text") == "12 dB/oct");
	const Json type = get(client, audio.channel, stripTarget(12));
	CHECK(type.at("name") == "EQ 1 Type");
	CHECK(type.at("options") == Json::array({"Bell", "Low Shelf", "Low Cut"}));
	CHECK(type.at("text") == "Low Shelf");
	CHECK(get(client, audio.channel, stripTarget(70)).at("options") == Json::array({"Off", "On"}));

	// parameter.set changes the sections as channel.set does, and parameter.get reads what channel.set changed.
	set(client, audio.channel, stripTarget(0), "text", "-6 dB");
	set(client, audio.channel, stripTarget(1), "value", 1.0);
	set(client, audio.channel, stripTarget(4), "text", "48 dB/oct");
	set(client, audio.channel, stripTarget(42), "text", "high cut");
	set(client, audio.channel, stripTarget(62), "value", 50.0);
	const Json channel = run(client, "project.get").at("channels").at(0);
	CHECK(channel.at("pre").at("gain") == -6.0);
	CHECK(channel.at("pre").at("phase") == true);
	CHECK(channel.at("pre").at("lowCut").at("slope") == 48);
	CHECK(channel.at("eq").at("bands").at(3).at("type") == "highCut");
	CHECK(channel.at("strip").at("compressor").at("ratio") == 20.0);
	run(client, "channel.set", {{"channel", audio.channel}, {"eq", {{"bands", {{{"band", 2}, {"gain", 3.0}}}}}}});
	CHECK(get(client, audio.channel, stripTarget(24)).at("value") == 3.0);
	CHECK(get(client, audio.channel, stripTarget(24)).at("text") == "3.00 dB");

	// automationLane.targets groups them by section.
	std::map<uint32_t, std::string> groups;
	const Json targets = run(client, "automationLane.targets", {{"track", audio.track}}).at("targets");
	for (const Json& target : targets)
		if (target.at("target").at("type") == "strip")
			groups[target.at("target").at("parameter").get<uint32_t>()] = target.at("group").get<std::string>();
	CHECK(groups.at(0) == "Pre");
	CHECK(groups.at(10) == "Equalizer");
	CHECK(groups.at(70) == "Strip");

	// Only audio channels have them.
	const TestTrack midi = addTrack(client, "midi");
	CHECK_THAT(reject(client, "parameter.get", {{"channel", midi.channel}, {"target", stripTarget(0)}}),
		ContainsSubstring("MIDI"));
	CHECK_THAT(reject(client, "parameter.get", {{"channel", audio.channel}, {"target", stripTarget(99)}}),
		ContainsSubstring("no parameter 99"));
}

TEST_CASE("parameter.set takes a value or a display text, stops at the range and is one undo step", "[unit]")
{
	Engine engine;
	Client client(engine);
	const TestTrack audio = addTrack(client, "audio");
	const Json volume = {{"type", "volume"}};
	const Json pan = {{"type", "pan"}};

	const Json changed = set(client, audio.channel, volume, "value", -6.0);
	CHECK(changed.at("value") == -6.0);
	CHECK(changed.at("text") == "-6.00 dB");
	CHECK(run(client, "project.get").at("channels").at(0).at("volume") == -6.0);
	CHECK(set(client, audio.channel, volume, "value", 100.0).at("value") == 6.0);
	CHECK(set(client, audio.channel, volume, "value", -1000.0).at("value") == -144.0);
	CHECK(set(client, audio.channel, volume, "text", "-12 dB").at("value") == -12.0);
	CHECK(set(client, audio.channel, volume, "text", " -3.5db ").at("value") == -3.5);
	CHECK(set(client, audio.channel, volume, "text", "-6dB").at("value") == -6.0);
	CHECK(set(client, audio.channel, pan, "text", "-0.5").at("value") == -0.5);
	CHECK(set(client, audio.channel, pan, "value", 3.0).at("value") == 1.0);
	CHECK(set(client, audio.channel, {{"type", "panRight"}}, "value", 0.0).at("value") == 0.0);

	// Each set is one undo step.
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Change parameter");
	run(client, "history.undo");
	CHECK(get(client, audio.channel, {{"type", "panRight"}}).at("value") == 1.0);
	run(client, "history.redo");
	CHECK(get(client, audio.channel, {{"type", "panRight"}}).at("value") == 0.0);

	// Wrong arguments change nothing.
	const Json before = run(client, "project.get");
	const auto refused = [&](const Json& arguments, const std::string& expected)
	{ CHECK_THAT(reject(client, "parameter.set", arguments), ContainsSubstring(expected)); };
	refused({{"channel", audio.channel}, {"target", pan}, {"text", "left"}}, "text");
	refused({{"channel", audio.channel}, {"target", pan}, {"text", "0.5 dB"}}, "text");
	refused({{"channel", audio.channel}, {"target", pan}}, "value or text");
	refused({{"channel", audio.channel}, {"target", pan}, {"value", 0.5}, {"text", "0.5"}}, "value or text");
	refused({{"channel", audio.channel}, {"target", {{"type", "controller"}}}, {"value", 0.5}}, "target.type");
	refused({{"channel", audio.channel}, {"target", {{"type", "pan"}, {"parameter", 1}}}, {"value", 0.5}},
		"target.parameter");
	refused({{"channel", audio.channel}, {"target", instrumentTarget(0)}, {"value", 0.5}}, "instrument");
	refused({{"channel", 9999}, {"target", pan}, {"value", 0.5}}, "9999");
	const TestTrack midi = addTrack(client, "midi");
	const Json withMidi = run(client, "project.get");
	refused({{"channel", midi.channel}, {"target", volume}, {"value", -6.0}}, "MIDI");
	CHECK(run(client, "project.get") == withMidi);
	run(client, "track.remove", {{"track", midi.track}});
	CHECK(run(client, "project.get").at("channels") == before.at("channels"));
}

TEST_CASE("instrument parameters keep their values per channel until the instrument changes", "[unit]")
{
	Engine engine;
	Client client(engine);
	const TestTrack synth = addSynth(client);
	const Json level = instrumentTarget(processors::TestSynth::Level);
	const Json release = instrumentTarget(processors::TestSynth::Release);

	CHECK(set(client, synth.channel, level, "value", 0.5).at("text") == "0.50");
	CHECK(set(client, synth.channel, release, "text", "2 S").at("value") == 2.0);
	CHECK(set(client, synth.channel, release, "value", 50.0).at("value") == 10.0);
	CHECK(get(client, synth.channel, level).at("value") == 0.5);
	const Json channel = run(client, "project.get").at("channels").at(0);
	CHECK(channel.at("instrumentParameters") ==
		Json::array({{{"parameter", processors::TestSynth::Level}, {"value", 0.5}},
			{{"parameter", processors::TestSynth::Release}, {"value", 10.0}}}));
	CHECK_THAT(
		reject(client, "parameter.set", {{"channel", synth.channel}, {"target", instrumentTarget(99)}, {"value", 0.5}}),
		ContainsSubstring("99"));

	// Another instrument, or the same one again, starts at its defaults.
	run(client, "channel.set", {{"channel", synth.channel}, {"instrument", ""}});
	CHECK(run(client, "parameter.list", {{"channel", synth.channel}}).at("parameters").size() ==
		5 + g_stripParameterCount);
	CHECK_THAT(reject(client, "parameter.get", {{"channel", synth.channel}, {"target", level}}),
		ContainsSubstring("instrument"));
	run(client, "channel.set", {{"channel", synth.channel}, {"instrument", "testSynth"}});
	CHECK(get(client, synth.channel, level).at("value") == 0.25);
	run(client, "history.undo", {{"steps", 2}});
	CHECK(get(client, synth.channel, level).at("value") == 0.5);
}

TEST_CASE("instrument parameter values are saved, and files from before them or newer builds load", "[unit]")
{
	Engine engine;
	Client client(engine);
	const TestTrack synth = addSynth(client);
	set(client, synth.channel, instrumentTarget(processors::TestSynth::Level), "value", 0.75);
	const Json saved = run(client, "project.get");
	const Project loaded = projectFromJson(saved);
	const Channel& channel = loaded.channels.at(synth.channel);
	CHECK(channel.instrumentParameters == std::map<uint32_t, double>{{processors::TestSynth::Level, 0.75}});
	CHECK(toJson(loaded) == saved);
	const gnubase::test::TempDir dir;
	const std::string file = (dir.path() / "parameters.dawproject").u8string();
	run(client, "project.save", {{"path", file}});
	run(client, "project.new");
	run(client, "project.load", {{"path", file}});
	CHECK(get(client, synth.channel, instrumentTarget(processors::TestSynth::Level)).at("value") == 0.75);

	// A file from before instrument parameters, and one with a parameter this build's instrument doesn't have.
	Json old = saved;
	old["channels"][0].erase("instrumentParameters");
	CHECK(projectFromJson(old).channels.at(synth.channel).instrumentParameters.empty());
	Json newer = saved;
	newer["channels"][0]["instrumentParameters"].push_back({{"parameter", 77}, {"value", 1000.0}});
	CHECK(projectFromJson(newer).channels.at(synth.channel).instrumentParameters.size() == 1);
	// An instrument this build doesn't have plays none, so its values go too.
	Json unknown = saved;
	unknown["channels"][0]["instrument"] = "futureSynth";
	CHECK(projectFromJson(unknown).channels.at(synth.channel).instrumentParameters.empty());

	const auto refused = [&saved](const std::function<void(Json&)>& change)
	{
		Json bad = saved;
		change(bad);
		CHECK_THROWS_AS(projectFromJson(bad), std::invalid_argument);
	};
	// A value beyond the range, e.g. from a build that widened it, stops at its end.
	Json wider = saved;
	wider["channels"][0]["instrumentParameters"][0]["value"] = 2.0;
	CHECK(
		projectFromJson(wider).channels.at(synth.channel).instrumentParameters.at(processors::TestSynth::Level) == 1.0);
	refused([](Json& p) { p["channels"][0]["instrumentParameters"][0]["value"] = "loud"; });
	refused([](Json& p)
		{ p["channels"][0]["instrumentParameters"][0]["value"] = std::numeric_limits<double>::infinity(); });
	refused([](Json& p) { p["channels"][0]["instrumentParameters"][0]["parameter"] = -1; });
	refused([](Json& p)
		{ p["channels"][0]["instrumentParameters"].push_back(p["channels"][0]["instrumentParameters"][0]); });
	refused([](Json& p) { p["channels"][0]["instrumentParameters"] = 5; });
}

TEST_CASE("parameter values set on an instrument play", "[integration]")
{
	Engine engine;
	Client client(engine);
	run(client, "audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}, {"captureSeconds", 1}});
	const TestTrack synth = addSynth(client);
	// The test synth takes its level when a note starts: silent first, then loud.
	set(client, synth.channel, instrumentTarget(processors::TestSynth::Level), "value", 0.0);
	const ObjectId part = run(client, "midiPart.add", {{"track", synth.track}, {"position", 0}, {"length", 3440640}})
							  .at("part")
							  .at("id")
							  .get<ObjectId>();
	// At 120 BPM and 48 kHz, 1792 ticks are 25 frames: notes at 0 and 12000 frames, 6000 frames long.
	run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", 0}, {"length", 430080}, {"pitch", 69}, {"velocity", 1.0}},
					{{"position", 860160}, {"length", 430080}, {"pitch", 69}, {"velocity", 1.0}}}}});
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 9000}});
	set(client, synth.channel, instrumentTarget(processors::TestSynth::Level), "text", "0.5");
	run(client, "audio.render", {{"frames", 9000}});
	const std::vector<float> output = run(client, "audio.capture", {{"channel", 0}, {"start", 0}, {"frames", 18000}})
										  .at("samples")
										  .get<std::vector<float>>();
	const auto peak = [&output](size_t begin, size_t end)
	{
		float result = 0.0f;
		for (size_t i = begin; i < end; ++i)
			result = std::max(result, std::abs(output[i]));
		return result;
	};
	CHECK(peak(0, 12000) == 0.0f);
	CHECK(peak(12000, 18000) > 0.4f);
}

TEST_CASE("parameter values convert to display text and back", "[unit]")
{
	const processors::ParameterInfo detection = builtinParameter("compressor", "Detection");
	CHECK(processors::valueToText(detection, 1.0) == "RMS");
	CHECK(processors::textToValue(detection, "rms") == std::optional<double>(1.0));
	CHECK(processors::textToValue(detection, " Peak ") == std::optional<double>(0.0));
	CHECK(processors::textToValue(detection, "1") == std::optional<double>(1.0));
	CHECK(!processors::textToValue(detection, "loud").has_value());

	const processors::ParameterInfo bits = builtinParameter("dither", "Bits");
	CHECK(processors::valueToText(bits, 16.0) == "16");
	CHECK(processors::textToValue(bits, "20.4") == std::optional<double>(20.0));
	CHECK(processors::textToValue(bits, "99") == std::optional<double>(24.0));

	const processors::ParameterInfo decay = builtinParameter("reverb", "Decay");
	CHECK(processors::valueToText(decay, 2.5) == "2.50 s");
	CHECK(processors::textToValue(decay, "2.5 s") == std::optional<double>(2.5));
	CHECK(processors::textToValue(decay, "2.5s") == std::optional<double>(2.5));
	CHECK(!processors::textToValue(decay, "2.5 ms").has_value());
	CHECK(!processors::textToValue(decay, "").has_value());
	CHECK(!processors::textToValue(decay, "nan").has_value());
	CHECK(!processors::textToValue(decay, "inf").has_value());
	CHECK(!processors::textToValue(decay, "s").has_value());
	CHECK(!processors::textToValue(decay, ".").has_value());
	// The unit may follow the number directly, also after an exponent; every standard library reads it the same.
	CHECK(processors::textToValue(decay, "2.5S") == std::optional<double>(2.5));
	CHECK(processors::textToValue(decay, "1e1s") == std::optional<double>(10.0));
	CHECK(processors::textToValue(decay, ".5e+1 s") == std::optional<double>(5.0));
	CHECK(processors::textToValue(decay, "3.") == std::optional<double>(3.0));
	CHECK(!processors::textToValue(decay, "1e s").has_value());
	CHECK(!processors::textToValue(decay, "2..5").has_value());

	const processors::ParameterInfo pan = builtinParameter("gainPan", "Pan");
	CHECK(processors::valueToText(pan, -0.001) == "0.00");
	CHECK(processors::valueToText(pan, -0.005) == "-0.01");
}

TEST_CASE("automationLane.targets lists what a track's automation tracks can show, with the lanes it has", "[unit]")
{
	Engine engine;
	Client client(engine);
	const TestTrack audio = addTrack(client, "audio");
	const Json gain =
		run(client, "plugin.insert", {{"channel", audio.channel}, {"plugin", "builtin:gainPan"}}).at("slot").at("id");
	const Json lane =
		run(client, "automationLane.add", {{"track", audio.track}, {"target", {{"type", "pan"}}}}).at("lane").at("id");
	const Json targets = run(client, "automationLane.targets", {{"track", audio.track}}).at("targets");
	// The channel's parameters as parameter.list has them, plugins' included; each with the lane it has, or 0.
	const Json parameters = run(client, "parameter.list", {{"channel", audio.channel}}).at("parameters");
	REQUIRE(targets.size() == parameters.size());
	for (size_t i = 0; i < targets.size(); ++i)
	{
		CHECK(targets[i].at("target") == parameters[i].at("target"));
		CHECK(targets[i].at("name") == parameters[i].at("name"));
		CHECK(targets[i].at("value") == parameters[i].at("value"));
		CHECK(targets[i].at("minimum") == parameters[i].at("minimum"));
	}
	CHECK(targets[0].at("group") == "Channel");
	CHECK(targets[0].at("lane") == 0);
	CHECK(targets[1].at("lane") == lane);
	CHECK(targets.back().at("target").at("slot") == gain);
	CHECK(targets.back().at("group") == processors::findBuiltinPlugin("builtin:gainPan")->name);

	// MIDI and instrument tracks also have pitch bend, channel pressure and the 128 controllers, from 0 to 1.
	const TestTrack midi = addTrack(client, "midi");
	const Json midiTargets = run(client, "automationLane.targets", {{"track", midi.track}}).at("targets");
	REQUIRE(midiTargets.size() == 130);
	CHECK(midiTargets[0].at("target").at("type") == "pitchBend");
	CHECK(midiTargets[0].at("value") == 0.5);
	CHECK(midiTargets[1].at("target").at("type") == "channelPressure");
	CHECK(midiTargets[9].at("target").at("type") == "controller");
	CHECK(midiTargets[9].at("target").at("number") == 7);
	CHECK(midiTargets[9].at("name") == "Controller 7");
	CHECK(midiTargets[9].at("group") == "MIDI");
	CHECK(midiTargets[9].at("maximum") == 1.0);
	CHECK(midiTargets[9].at("stepped") == false);
	// The targets are the ones automationLane.add takes.
	run(client, "automationLane.add", {{"track", midi.track}, {"target", midiTargets[9].at("target")}});
	CHECK(run(client, "automationLane.targets", {{"track", midi.track}}).at("targets")[9].at("lane") != 0);
	const TestTrack synth = addSynth(client);
	CHECK(run(client, "automationLane.targets", {{"track", synth.track}}).at("targets").size() ==
		8 + g_stripParameterCount + 130);

	// Tracks without a channel have none.
	const Json marker = run(client, "track.add", {{"type", "marker"}}).at("track").at("id");
	CHECK(run(client, "automationLane.targets", {{"track", marker}}).at("targets").empty());
	CHECK_THAT(reject(client, "automationLane.targets", {{"track", 9999}}), ContainsSubstring("9999"));
}
