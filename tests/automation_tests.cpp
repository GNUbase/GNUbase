// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/automation.h"
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "engine/graph/events.h"
#include "engine/graph/graph.h"
#include "engine/processors/testsynth.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
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

	ObjectId idOf(const Json& result, const char* kind)
	{
		return result.at(kind).at("id").get<ObjectId>();
	}

	ObjectId channelOf(Client& client, ObjectId track)
	{
		const Json project = run(client, "project.get");
		ObjectId channel = 0;
		for (const Json& t : project.at("tracks"))
			if (t.at("id").get<ObjectId>() == track)
				channel = t.at("channel").get<ObjectId>();
		REQUIRE(channel != 0);
		return channel;
	}

	// Ticks for a number of samples at the default 120 BPM and 48 kHz, exact for multiples of 25 samples.
	int64_t ticks(int64_t samples)
	{
		REQUIRE(samples % 25 == 0);
		return samples / 25 * 1792;
	}

	double gain(double decibels)
	{
		return std::pow(10.0, decibels / 20.0);
	}

	// A point of a volume curve, in samples and dB, as the tests write them.
	struct TestPoint
	{
		int64_t position = 0;
		double decibels = 0.0;
		const char* shape = "linear";
		double curve = 0.0;
	};

	// The gain a volume curve has at a position, worked out independently of the engine: ramps move the gain in a
	// straight line, curves bend it by (1 - e^(-kx)) / (1 - e^(-k)) with k = 6 * curve and play as straight pieces
	// between the curve's points every 64 frames from its start.
	double expectedGain(const std::vector<TestPoint>& points, int64_t position)
	{
		size_t next = 0;
		while (next < points.size() && points[next].position <= position)
			++next;
		if (next == 0)
			return gain(points.front().decibels);
		if (next == points.size())
			return gain(points.back().decibels);
		const TestPoint& from = points[next - 1];
		const TestPoint& to = points[next];
		const std::string shape = from.shape;
		const auto at = [&](int64_t p)
		{
			const double x = static_cast<double>(p - from.position) / static_cast<double>(to.position - from.position);
			const double k = 6.0 * from.curve;
			const double bent = shape == "curve" ? (1.0 - std::exp(-k * x)) / (1.0 - std::exp(-k)) : x;
			return gain(from.decibels) + ((gain(to.decibels) - gain(from.decibels)) * bent);
		};
		if (shape == "step")
			return gain(from.decibels);
		if (shape == "linear")
			return at(position);
		const int64_t start = from.position + ((position - from.position) / 64 * 64);
		const int64_t end = std::min(start + 64, to.position);
		return at(start) +
			((at(end) - at(start)) * static_cast<double>(position - start) / static_cast<double>(end - start));
	}

	// An audio track that passes on the dummy device's input, a sine, so its output shows what its fader does.
	class Mixer
	{
	public:
		explicit Mixer(int blockSize)
		{
			run(client, "audio.setDevice",
				{{"blockSize", blockSize}, {"captureSeconds", 2},
					{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 441.0}, {"Level", 0.5}}}}}});
			track = idOf(run(client, "track.add", {{"type", "audio"}}), "track");
			channel = channelOf(client, track);
		}

		ObjectId lane(const Json& target)
		{
			return idOf(run(client, "automationLane.add", {{"track", track}, {"target", target}}), "lane");
		}

		void add(ObjectId lane, const std::vector<TestPoint>& points)
		{
			Json list = Json::array();
			for (const TestPoint& point : points)
			{
				Json item = {{"position", ticks(point.position)}, {"value", point.decibels}, {"shape", point.shape}};
				if (point.curve != 0.0)
					item["curve"] = point.curve;
				list.push_back(item);
			}
			run(client, "automationPoint.add", {{"lane", lane}, {"points", list}});
		}

		std::vector<float> capture(int output, int64_t frames)
		{
			return run(client, "audio.capture", {{"channel", output}, {"start", 0}, {"frames", frames}})
				.at("samples")
				.get<std::vector<float>>();
		}

		Engine engine;
		Client client{engine};
		ObjectId track = 0;
		ObjectId channel = 0;
	};

	// Checks every frame against the input times the curve's gain at the position played there (-1: stopped, then
	// the cursor's position is given as -2 - cursor).
	void checkGains(Mixer& mixer, const std::vector<TestPoint>& points, const std::vector<int64_t>& played)
	{
		Mixer reference(64); // the same input without automation
		const auto frames = static_cast<int64_t>(played.size());
		run(reference.client, "audio.render", {{"frames", frames}});
		const std::vector<float> input = reference.capture(0, frames);
		size_t wrong = 0;
		size_t first = 0;
		for (int channel = 0; channel < 2; ++channel)
		{
			const std::vector<float> output = mixer.capture(channel, frames);
			for (size_t i = 0; i < played.size(); ++i)
			{
				const int64_t position = played[i] >= 0 ? played[i] : -2 - played[i];
				const double expected = input[i] * expectedGain(points, position);
				if (std::abs(output[i] - expected) > 1e-6 && wrong++ == 0)
					first = i;
			}
		}
		INFO(wrong << " wrong samples, the first at frame " << first << " (position " << played[first] << ")");
		CHECK(wrong == 0);
	}

	std::vector<int64_t> positions(int64_t start, int64_t frames, int64_t left = 0, int64_t right = 0)
	{
		std::vector<int64_t> result;
		int64_t position = start;
		for (int64_t i = 0; i < frames; ++i)
		{
			result.push_back(position);
			const bool cycles = right > left && position < right;
			position = cycles && position + 1 == right ? left : position + 1;
		}
		return result;
	}

	gnubase::test::Audio stereo(Mixer& mixer, int64_t frames)
	{
		gnubase::test::Audio audio;
		audio.channels = 2;
		audio.sampleRate = 48000;
		const std::vector<float> left = mixer.capture(0, frames);
		const std::vector<float> right = mixer.capture(1, frames);
		for (size_t i = 0; i < left.size(); ++i)
		{
			audio.samples.push_back(left[i]);
			audio.samples.push_back(right[i]);
		}
		return audio;
	}
}

TEST_CASE("automation lanes hold points on tracks, with checks, events and undo", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId audio = idOf(run(client, "track.add", {{"type", "audio"}}), "track");
	const ObjectId instrument = idOf(run(client, "track.add", {{"type", "instrument"}}), "track");
	const ObjectId midi = idOf(run(client, "track.add", {{"type", "midi"}}), "track");
	run(client, "channel.set", {{"channel", channelOf(client, instrument)}, {"instrument", "testSynth"}});

	const Json added = run(client, "automationLane.add", {{"track", audio}, {"target", {{"type", "volume"}}}});
	const ObjectId volume = idOf(added, "lane");
	CHECK(added.at("lane").at("track") == audio);
	CHECK(added.at("lane").at("target") == Json{{"type", "volume"}});
	CHECK(added.at("lane").at("read") == true);
	CHECK(added.at("lane").at("points").empty());

	const Json points = run(client, "automationPoint.add",
		{{"lane", volume},
			{"points",
				{{{"position", 960}, {"value", -6.0}, {"shape", "curve"}, {"curve", 0.5}},
					{{"position", 0}, {"value", 0.0}, {"shape", "step"}}, {{"position", 1920}, {"value", 3.0}}}}})
							.at("points");
	REQUIRE(points.size() == 3);
	CHECK(points[0].at("shape") == "curve");
	CHECK(points[0].at("curve") == 0.5);
	CHECK(points[2].at("shape") == "linear"); // the default
	CHECK(!points[2].contains("curve"));

	const ObjectId pan = idOf(
		run(client, "automationLane.add", {{"track", audio}, {"target", {{"type", "pan"}}}, {"read", false}}), "lane");
	const ObjectId level = idOf(
		run(client, "automationLane.add",
			{{"track", instrument},
				{"target", {{"type", "instrument"}, {"parameter", gnubase::engine::processors::TestSynth::Level}}}}),
		"lane");
	const ObjectId modulation =
		idOf(run(client, "automationLane.add",
				 {{"track", midi}, {"target", {{"type", "controller"}, {"number", 1}, {"channel", 2}}}}),
			"lane");
	run(client, "automationLane.add", {{"track", instrument}, {"target", {{"type", "pitchBend"}}}});

	// Listed per track in track order, points by position.
	const Json lanes = run(client, "automationLane.list").at("lanes");
	REQUIRE(lanes.size() == 5);
	CHECK(lanes[0].at("id") == volume);
	CHECK(lanes[1].at("id") == pan);
	CHECK(lanes[1].at("read") == false);
	CHECK(lanes[2].at("id") == level);
	CHECK(lanes[4].at("id") == modulation);
	CHECK(lanes[4].at("target") == Json{{"type", "controller"}, {"number", 1}, {"channel", 2}});
	const Json& listed = lanes[0].at("points");
	REQUIRE(listed.size() == 3);
	CHECK(listed[0].at("position") == 0);
	CHECK(listed[1].at("position") == 960);
	CHECK(run(client, "automationLane.list", {{"track", midi}}).at("lanes").size() == 1);

	const ObjectId first = listed[0].at("id").get<ObjectId>();
	const Json changed = run(client, "automationPoint.set",
		{{"lane", volume}, {"points", {{{"id", first}, {"value", -144.0}, {"shape", "linear"}}}}})
							 .at("points");
	CHECK(changed[0].at("value") == -144.0);
	CHECK(changed[0].at("position") == 0);
	CHECK(run(client, "automationLane.set", {{"lane", pan}, {"read", true}}).at("lane").at("read") == true);
	run(client, "channel.set", {{"channel", channelOf(client, audio)}, {"volume", -10.0}, {"pan", 0.25}});

	// Undo and redo restore everything exactly.
	const Json before = run(client, "project.get");
	run(client, "automationPoint.remove", {{"lane", volume}, {"points", {first}}});
	run(client, "automationLane.remove", {{"lane", modulation}});
	run(client, "track.remove", {{"track", instrument}}); // removes its lanes
	CHECK(run(client, "automationLane.list").at("lanes").size() == 2);
	CHECK(run(client, "automationLane.list", {{"track", audio}}).at("lanes")[0].at("points").size() == 2);
	run(client, "history.undo", {{"steps", 3}});
	CHECK(run(client, "project.get") == before);
	run(client, "history.redo", {{"steps", 3}});
	run(client, "history.undo", {{"steps", 3}});
	CHECK(run(client, "project.get") == before);
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Remove track");

	// Checks.
	const auto badLane = [&](ObjectId track, const Json& target)
	{ return reject(client, "automationLane.add", {{"track", track}, {"target", target}}); };
	CHECK_THAT(badLane(audio, {{"type", "volume"}}), ContainsSubstring("already has this automation lane"));
	CHECK_THAT(badLane(midi, {{"type", "pan"}}), ContainsSubstring("no volume or pan"));
	CHECK_THAT(
		badLane(audio, {{"type", "controller"}, {"number", 7}}), ContainsSubstring("MIDI and instrument tracks"));
	CHECK_THAT(badLane(midi, {{"type", "controller"}}), ContainsSubstring("target.number"));
	CHECK_THAT(badLane(midi, {{"type", "controller"}, {"number", 128}}), ContainsSubstring("target.number"));
	CHECK_THAT(badLane(midi, {{"type", "pitchBend"}, {"channel", 16}}), ContainsSubstring("target.channel"));
	CHECK_THAT(badLane(audio, {{"type", "volume"}, {"number", 1}}), ContainsSubstring("target.number"));
	CHECK_THAT(badLane(audio, {{"type", "instrument"}, {"parameter", 0}}), ContainsSubstring("instrument tracks"));
	CHECK_THAT(badLane(instrument, {{"type", "instrument"}, {"parameter", 99}}), ContainsSubstring("no parameter 99"));
	const ObjectId folder = idOf(run(client, "track.add", {{"type", "folder"}}), "track");
	CHECK_THAT(badLane(folder, {{"type", "volume"}}), ContainsSubstring("no channel"));

	const auto badPoint = [&](ObjectId lane, const Json& point)
	{ return reject(client, "automationPoint.add", {{"lane", lane}, {"points", {point}}}); };
	CHECK_THAT(badPoint(volume, {{"position", 0}, {"value", 7.0}}), ContainsSubstring("from -144 to 6"));
	CHECK_THAT(badPoint(pan, {{"position", 0}, {"value", -1.5}}), ContainsSubstring("from -1 to 1"));
	CHECK_THAT(badPoint(level, {{"position", 0}, {"value", 2.0}}), ContainsSubstring("from 0 to 1"));
	CHECK_THAT(badPoint(volume, {{"position", -1}, {"value", 0.0}}), ContainsSubstring("negative"));
	CHECK_THAT(badPoint(volume, {{"position", 0}, {"value", 0.0}, {"shape", "curve"}, {"curve", 2.0}}),
		ContainsSubstring("curve must be"));
	CHECK_THAT(reject(client, "automationPoint.set", {{"lane", volume}, {"points", {{{"id", 12345}}}}}),
		ContainsSubstring("no automation point 12345"));
	CHECK_THAT(reject(client, "channel.set", {{"channel", channelOf(client, midi)}, {"volume", -3.0}}),
		ContainsSubstring("no volume or pan"));
	CHECK_THAT(reject(client, "channel.set", {{"channel", channelOf(client, audio)}, {"volume", 6.5}}),
		ContainsSubstring("volume must be"));
}

TEST_CASE("automation points are compiled to the project's samples, following the tempo map", "[unit]")
{
	Project project = newProject();
	project.tempo = gnubase::time::TempoMap({{0, 120.0, false}, {ticks(48000), 60.0, false}});
	const ObjectId track = project.newId();
	const ObjectId channel = project.newId();
	project.trackById[track] = {track, TrackType::Audio, "Audio", "", gnubase::time::TimeBase::Musical, 0, channel, {}};
	project.tracks.push_back(track);
	project.channels[channel] = {channel, track, 2, 0, 0, {}, -6.0, 0.5};
	AutomationLane volume;
	volume.id = project.newId();
	volume.track = track;
	volume.points[project.newId()] = {0, 0, 0.0, AutomationShape::Linear, 0.0};
	volume.points[project.newId()] = {0, ticks(48000) + ticks(24000), -20.0, AutomationShape::Step, 0.0};
	volume.points[project.newId()] = {0, ticks(24000), -6.0, AutomationShape::Curve, -0.5};
	project.automation[volume.id] = volume;
	AutomationLane unread = volume;
	unread.id = project.newId();
	unread.target.type = AutomationTargetType::Pan;
	unread.read = false;
	project.automation[unread.id] = unread;
	AutomationLane empty;
	empty.id = project.newId();
	empty.track = track;
	empty.target.type = AutomationTargetType::Pan;
	project.automation[empty.id] = empty;

	const auto snapshot = compile(project, 1);
	const RenderChannel& render = snapshot->channels.at(0);
	CHECK(std::abs(render.volume - gain(-6.0)) < 1e-12);
	CHECK(render.pan == 0.5);
	REQUIRE(render.automation.size() == 1); // without the unread and the empty lane
	const std::vector<RenderAutomationPoint>& points = render.automation[0].points;
	REQUIRE(points.size() == 3);
	CHECK(points[0].position == 0);
	CHECK(points[1].position == 24000);
	CHECK(points[1].value == gain(-6.0)); // volume as a gain
	CHECK(points[1].shape == AutomationShape::Curve);
	CHECK(points[1].curve == -0.5);
	CHECK(points[2].position == 48000 + 48000); // half a second at 120 BPM takes a second at 60
}

TEST_CASE("the automation player turns curves into parameter and controller events at their samples", "[unit]")
{
	using Kind = PlayedLane::Kind;
	using Type = graph::ProcessEvent::Type;
	AutomationPlayer player;
	auto automation = std::make_unique<ChannelAutomation>();
	// The fader follows a straight ramp with one event, other parameters glide in pieces of g_automationInterval,
	// controllers get an event at each piece.
	automation->lanes.push_back({{AutomationTargetType::Volume, 0, 0, 0}, Kind::Fader, ChannelFader::Volume, 1.0, 0.0,
		2.0, {{100, 0.5, AutomationShape::Linear, 0.0}, {300, 1.5, AutomationShape::Step, 0.0}}});
	automation->lanes.push_back({{AutomationTargetType::Instrument, 2, 0, 0}, Kind::Instrument, 2, 0.25, 0.0, 1.0,
		{{0, 0.0, AutomationShape::Linear, 0.0}, {128, 1.0, AutomationShape::Linear, 0.0}}});
	automation->lanes.push_back({{AutomationTargetType::Controller, 0, 7, 3}, Kind::Midi, 0, 0.0, 0.0, 1.0,
		{{100, 0.0, AutomationShape::Linear, 0.0}, {300, 1.0, AutomationShape::Step, 0.0},
			{400, 0.0, AutomationShape::Step, 0.0}, {400, 0.5, AutomationShape::Step, 0.0}}});
	player.setAutomation(std::move(automation));

	graph::EventBuffer source(graph::g_maxEventsPerBlock);
	graph::EventBuffer fader(graph::g_maxEventsPerBlock);
	const graph::TransportSegment segment{0, 512, 0};
	player.play({true, 0, &segment, 1}, source, fader);

	// The fader: its value at the start and the ramp, which arrives at the value the curve then keeps.
	REQUIRE(fader.size() == 2);
	CHECK(fader[0].offset == 0);
	CHECK(fader[0].type == Type::Parameter);
	CHECK(fader[0].parameterValue == 0.5);
	CHECK(fader[0].ramp == 0);
	CHECK(fader[1].offset == 100);
	CHECK(fader[1].parameterValue == 1.5);
	CHECK(fader[1].ramp == 200);

	std::vector<graph::ProcessEvent> parameters;
	std::vector<graph::ProcessEvent> controllers;
	for (const graph::ProcessEvent& event : source)
		(event.type == Type::Parameter ? parameters : controllers).push_back(event);
	REQUIRE(parameters.size() == 3);
	CHECK(parameters[0].ramp == 0);
	CHECK(parameters[1].offset == 0);
	CHECK(parameters[1].ramp == 64);
	CHECK(parameters[1].parameterValue == 0.5);
	CHECK(parameters[2].offset == 64);
	CHECK(parameters[2].ramp == 64);
	CHECK(parameters[2].parameterValue == 1.0);
	CHECK(parameters[2].parameter == 2);

	std::vector<uint32_t> offsets;
	for (const graph::ProcessEvent& event : controllers)
	{
		CHECK(event.type == Type::Controller);
		CHECK(event.key == 7);
		CHECK(event.channel == 3);
		offsets.push_back(event.offset);
	}
	CHECK(offsets == std::vector<uint32_t>{0, 164, 228, 292, 300, 400});
	CHECK(controllers[1].value == static_cast<uint32_t>(std::llround(0.32 * 4294967295.0)));
	CHECK(controllers[4].value == 0xFFFFFFFFU); // the end of the ramp, exactly
	CHECK(controllers[5].value == static_cast<uint32_t>(std::llround(0.5 * 4294967295.0))); // the last at 400

	// Going on: nothing more until the transport jumps.
	source.clear();
	fader.clear();
	const graph::TransportSegment on{0, 512, 512};
	player.play({true, 512, &on, 1}, source, fader);
	CHECK(source.empty());
	CHECK(fader.empty());
	const graph::TransportSegment jump{0, 64, 200};
	player.play({true, 200, &jump, 1}, source, fader);
	REQUIRE(fader.size() == 2);
	CHECK(fader[0].parameterValue == 1.0); // mid-ramp
	CHECK(fader[0].ramp == 0);
	CHECK(fader[1].ramp == 64);
	CHECK(std::abs(fader[1].parameterValue - 1.32) < 1e-12);

	// Stopped: the values at the cursor, once per cursor position.
	source.clear();
	fader.clear();
	player.play({false, 50, nullptr, 0}, source, fader);
	REQUIRE(fader.size() == 1);
	CHECK(fader[0].parameterValue == 0.5);
	fader.clear();
	source.clear();
	player.play({false, 50, nullptr, 0}, source, fader);
	CHECK(fader.empty());
	CHECK(source.empty());
}

TEST_CASE("volume ramps, curves, steps and pan render as their golden file", "[integration]")
{
	Mixer mixer(128);
	const ObjectId volume = mixer.lane({{"type", "volume"}});
	mixer.add(volume,
		{{0, -24.0}, {12000, 0.0}, {24000, -12.0, "curve", 0.5}, {36000, 0.0, "step"}, {42000, -6.0, "step"},
			{42000, -18.0, "linear"}, {48000, 0.0}});
	const ObjectId pan = mixer.lane({{"type", "pan"}});
	run(mixer.client, "automationPoint.add",
		{{"lane", pan},
			{"points", {{{"position", 0}, {"value", -1.0}}, {{"position", ticks(48000)}, {"value", 1.0}}}}});
	run(mixer.client, "transport.play");
	run(mixer.client, "audio.render", {{"frames", 48000}});

	const gnubase::test::AudioDiff diff = gnubase::test::compareWithGolden(
		"automation-gain-ramps", stereo(mixer, 48000), 1e-5f); // sin() differs by platform
	INFO(diff.report);
	CHECK(diff.equal);
}

TEST_CASE("volume automation is sample-accurate for any block size", "[integration]")
{
	const int blockSize = GENERATE(1, 64, 100, 333);
	INFO("block size " << blockSize);
	Mixer mixer(blockSize);
	const std::vector<TestPoint> points = {{1000, -6.0, "step"}, {1025, -12.0, "linear"}, {2000, 0.0, "linear"},
		{2000, -20.0, "curve", -0.75}, {3000, -3.0, "step"}, {3500, -40.0, "linear"}, {4000, 6.0}};
	mixer.add(mixer.lane({{"type", "volume"}}), points);
	run(mixer.client, "transport.play");
	run(mixer.client, "audio.render", {{"frames", 4500}});
	checkGains(mixer, points, positions(0, 4500));
}

TEST_CASE("automation plays right after cycle jumps, locates mid-ramp and while stopped", "[integration]")
{
	const int blockSize = GENERATE(64, 100);
	INFO("block size " << blockSize);
	Mixer mixer(blockSize);
	const std::vector<TestPoint> points = {{0, -30.0}, {6000, 0.0, "curve", 0.25}, {9000, -9.0}};
	mixer.add(mixer.lane({{"type", "volume"}}), points);
	run(mixer.client, "transport.set", {{"left", 1000}, {"right", 4000}, {"cycle", true}});
	run(mixer.client, "transport.locate", {{"position", 500}});
	run(mixer.client, "transport.play");
	run(mixer.client, "audio.render", {{"frames", 7000}});
	run(mixer.client, "transport.set", {{"cycle", false}});
	run(mixer.client, "transport.locate", {{"position", 7525}}); // into the curve, while playing
	run(mixer.client, "audio.render", {{"frames", 1000}});
	run(mixer.client, "transport.stop");
	const auto cursor = run(mixer.client, "transport.get").at("position").get<int64_t>();
	run(mixer.client, "audio.render", {{"frames", 500}});
	run(mixer.client, "transport.locate", {{"position", 2500}}); // while stopped: the value at the cursor
	run(mixer.client, "audio.render", {{"frames", 500}});

	std::vector<int64_t> played = positions(500, 7000, 1000, 4000);
	for (const int64_t position : positions(7525, 1000))
		played.push_back(position);
	played.insert(played.end(), 500, -2 - cursor);
	played.insert(played.end(), 500, -2 - 2500);
	checkGains(mixer, points, played);
}

TEST_CASE("pan automation balances a stereo channel; read off plays the static values", "[integration]")
{
	Mixer mixer(64);
	run(mixer.client, "channel.set", {{"channel", mixer.channel}, {"volume", -6.0}, {"pan", -0.5}});
	const ObjectId pan = mixer.lane({{"type", "pan"}});
	run(mixer.client, "automationPoint.add",
		{{"lane", pan}, {"points", {{{"position", 0}, {"value", 0.5}, {"shape", "step"}}}}});
	run(mixer.client, "transport.play");
	run(mixer.client, "audio.render", {{"frames", 1000}});
	run(mixer.client, "automationLane.set", {{"lane", pan}, {"read", false}});
	run(mixer.client, "audio.render", {{"frames", 1000}});

	Mixer reference(64);
	run(reference.client, "audio.render", {{"frames", 2000}});
	const std::vector<float> input = reference.capture(0, 2000);
	const std::vector<float> left = mixer.capture(0, 2000);
	const std::vector<float> right = mixer.capture(1, 2000);
	size_t wrong = 0;
	for (size_t i = 0; i < 2000; ++i)
	{
		const double volume = gain(-6.0);
		const double expectedLeft = input[i] * volume * (i < 1000 ? 0.5 : 1.0);
		const double expectedRight = input[i] * volume * (i < 1000 ? 1.0 : 0.5);
		if (std::abs(left[i] - expectedLeft) > 1e-6 || std::abs(right[i] - expectedRight) > 1e-6)
			++wrong;
	}
	CHECK(wrong == 0);
}

TEST_CASE("instrument parameter automation reaches the built-in instrument", "[integration]")
{
	Engine engine;
	Client client(engine);
	run(client, "audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}, {"captureSeconds", 1}});
	const ObjectId track = idOf(run(client, "track.add", {{"type", "instrument"}}), "track");
	run(client, "channel.set", {{"channel", channelOf(client, track)}, {"instrument", "testSynth"}});
	const ObjectId part =
		idOf(run(client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", ticks(48000)}}), "part");
	run(client, "note.add",
		{{"part", part},
			{"notes",
				{{{"position", 0}, {"length", ticks(6000)}, {"pitch", 69}, {"velocity", 1.0}},
					{{"position", ticks(12000)}, {"length", ticks(6000)}, {"pitch", 69}, {"velocity", 1.0}}}}});
	const ObjectId lane =
		idOf(run(client, "automationLane.add",
				 {{"track", track}, {"target", {{"type", "instrument"}, {"parameter", processors::TestSynth::Level}}}}),
			"lane");
	// The test synth takes its level when a note starts: silent first, then loud.
	run(client, "automationPoint.add",
		{{"lane", lane},
			{"points",
				{{{"position", 0}, {"value", 0.0}, {"shape", "step"}},
					{{"position", ticks(9000)}, {"value", 0.5}, {"shape", "step"}}}}});
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 18000}});
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

TEST_CASE("automation follows moved events and parts when the setting is on", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId track = idOf(run(client, "track.add", {{"type", "instrument"}}), "track");
	const ObjectId part =
		idOf(run(client, "midiPart.add", {{"track", track}, {"position", 1000}, {"length", 1000}}), "part");
	const ObjectId lane =
		idOf(run(client, "automationLane.add", {{"track", track}, {"target", {{"type", "volume"}}}}), "lane");
	run(client, "automationPoint.add",
		{{"lane", lane},
			{"points",
				{{{"position", 500}, {"value", -1.0}}, {{"position", 1000}, {"value", -2.0}},
					{{"position", 1999}, {"value", -3.0}}, {{"position", 2000}, {"value", -4.0}},
					{{"position", 5500}, {"value", -5.0}}}}});
	const auto positionsOf = [&]()
	{
		std::vector<std::pair<int64_t, double>> result;
		const Json lanes = run(client, "automationLane.list").at("lanes");
		for (const Json& point : lanes.at(0).at("points"))
			result.emplace_back(point.at("position").get<int64_t>(), point.at("value").get<double>());
		return result;
	};

	CHECK(run(client, "automation.get").at("followsEvents") == false);
	run(client, "midiPart.set", {{"part", part}, {"position", 3000}});
	CHECK(positionsOf() ==
		std::vector<std::pair<int64_t, double>>{{500, -1.0}, {1000, -2.0}, {1999, -3.0}, {2000, -4.0}, {5500, -5.0}});

	CHECK(run(client, "automation.set", {{"followsEvents", true}}).at("followsEvents") == true);
	run(client, "midiPart.set", {{"part", part}, {"position", 1000}}); // from 3000-4000: nothing to move
	run(client, "midiPart.set", {{"part", part}, {"position", 5000}}); // 1000-2000 to 5000-6000, over the point at 5500
	CHECK(positionsOf() ==
		std::vector<std::pair<int64_t, double>>{{500, -1.0}, {2000, -4.0}, {5000, -2.0}, {5999, -3.0}});
	run(client, "history.undo");
	CHECK(positionsOf().size() == 5);
	run(client, "automation.set", {{"followsEvents", false}});
}
