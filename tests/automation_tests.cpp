// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/automation.h"
#include "engine/audio/sequencer.h"
#include "engine/document/rendersnapshot.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/graph/events.h"
#include "engine/graph/graph.h"
#include "engine/midi/midivalues.h"
#include "engine/processors/testsynth.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
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

	// The loudest of samples from begin to end.
	float loudest(const std::vector<float>& samples, size_t begin, size_t end)
	{
		float result = 0.0F;
		for (size_t i = begin; i < end; ++i)
			result = std::max(result, std::abs(samples[i]));
		return result;
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
			run(client, "channel.set", {{"channel", channel}, {"monitor", true}});
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
	// The fader stops at the project's maximum volume.
	CHECK(run(client, "channel.set", {{"channel", channelOf(client, audio)}, {"volume", 6.5}})
			  .at("channel")
			  .at("volume") == 6.0);
}

TEST_CASE("automation points are compiled to the project's samples, following the tempo map", "[unit]")
{
	Project project = newProject();
	project.tempo = gnubase::time::TempoMap({{0, 120.0, false}, {ticks(48000), 60.0, false}});
	const ObjectId track = project.newId();
	const ObjectId channel = project.newId();
	project.trackById[track] = {
		track, TrackType::Audio, "Audio", "", gnubase::time::TimeBase::Musical, 0, channel, {}, {}, {}};
	project.tracks.push_back(track);
	project.channels[channel] = {channel, track, ChannelFormat::Stereo, 0, 0, {}, -6.0, 0.5};
	AutomationLane volume;
	volume.id = project.newId();
	volume.track = track;
	const auto musical = [](int64_t value) { return gnubase::time::Position{gnubase::time::TimeBase::Musical, value}; };
	volume.points[project.newId()] = {0, musical(0), 0.0, AutomationShape::Linear, 0.0};
	volume.points[project.newId()] = {0, musical(ticks(48000) + ticks(24000)), -20.0, AutomationShape::Step, 0.0};
	volume.points[project.newId()] = {0, musical(ticks(24000)), -6.0, AutomationShape::Curve, -0.5};
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

TEST_CASE("automation points on linear tracks keep their time when the tempo changes", "[unit]")
{
	using gnubase::time::TimeBase;
	Project project = newProject();
	project.tempo = gnubase::time::TempoMap(100.0); // was 120 BPM when the points were made
	const auto add = [&project](TimeBase base, int64_t position)
	{
		const ObjectId track = project.newId();
		const ObjectId channel = project.newId();
		project.trackById[track] = {track, TrackType::Audio, "Audio", "", base, 0, channel, {}, {}, {}};
		project.tracks.push_back(track);
		project.channels[channel] = {channel, track, ChannelFormat::Stereo, 0, 0, {}, 0.0, 0.0};
		AutomationLane lane;
		lane.id = project.newId();
		lane.track = track;
		lane.points[project.newId()] = {0, {base, position}, -20.0, AutomationShape::Step, 0.0};
		project.automation[lane.id] = lane;
	};
	add(TimeBase::Linear, 480000);         // a dip at 10 s
	add(TimeBase::Musical, ticks(480000)); // a dip at beat 21, 10 s at 120 BPM

	const auto snapshot = compile(project, 1);
	CHECK(snapshot->channels.at(0).automation.at(0).points.at(0).position == 480000); // still at 10 s
	CHECK(snapshot->channels.at(1).automation.at(0).points.at(0).position == 576000); // 12 s at 100 BPM
}

TEST_CASE("automation points take positions in their track's time base and keep their time when it changes", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId track = idOf(run(client, "track.add", {{"type", "instrument"}, {"timeBase", "linear"}}), "track");
	const ObjectId lane =
		idOf(run(client, "automationLane.add", {{"track", track}, {"target", {{"type", "volume"}}}}), "lane");
	run(client, "automationPoint.add", {{"lane", lane}, {"points", {{{"position", 480000}, {"value", -20.0}}}}});
	const auto positionsOf = [&client]()
	{
		std::vector<int64_t> result;
		const Json lanes = run(client, "automationLane.list").at("lanes");
		for (const Json& point : lanes.at(0).at("points"))
			result.push_back(point.at("position").get<int64_t>());
		return result;
	};
	CHECK(positionsOf() == std::vector<int64_t>{480000}); // samples

	// Switching the time base keeps the time, and so does a new sample rate on a linear track.
	run(client, "track.set", {{"track", track}, {"timeBase", "musical"}});
	CHECK(positionsOf() == std::vector<int64_t>{ticks(480000)});
	run(client, "track.set", {{"track", track}, {"timeBase", "linear"}});
	CHECK(positionsOf() == std::vector<int64_t>{480000});
	run(client, "project.set", {{"sampleRate", 96000.0}});
	CHECK(positionsOf() == std::vector<int64_t>{960000});
	run(client, "project.set", {{"sampleRate", 48000.0}});

	// Saved in the track's time base.
	const Project loaded = projectFromJson(run(client, "project.get"));
	REQUIRE(loaded.automation.size() == 1);
	CHECK(loaded.automation.begin()->second.points.begin()->second.position ==
		gnubase::time::Position{gnubase::time::TimeBase::Linear, 480000});

	// Following a part in samples.
	run(client, "automation.set", {{"followsEvents", true}});
	const ObjectId part =
		idOf(run(client, "midiPart.add", {{"track", track}, {"position", 48000}, {"length", 48000}}), "part");
	run(client, "automationPoint.add", {{"lane", lane}, {"points", {{{"position", 60000}, {"value", -6.0}}}}});
	run(client, "midiPart.set", {{"part", part}, {"position", 96000}});
	CHECK(positionsOf() == std::vector<int64_t>{108000, 480000});
	run(client, "automation.set", {{"followsEvents", false}});
}

TEST_CASE("the automation player turns curves into parameter and controller events at their samples", "[unit]")
{
	using Kind = PlayedLane::Kind;
	using Type = graph::ProcessEvent::Type;
	AutomationPlayer player;
	auto automation = std::make_unique<ChannelAutomation>();
	// The fader follows a straight ramp with one event; instrument parameters and controllers get the value of each
	// g_automationInterval frame at that frame.
	automation->lanes.push_back({{AutomationTargetType::Volume, 0, 0, 0, 0}, Kind::Fader, ChannelFader::Volume, 1.0,
		0.0, 2.0, {{100, 0.5, AutomationShape::Linear, 0.0}, {300, 1.5, AutomationShape::Step, 0.0}}});
	automation->lanes.push_back({{AutomationTargetType::Instrument, 2, 0, 0, 0}, Kind::Instrument, 2, 0.25, 0.0, 1.0,
		{{0, 0.0, AutomationShape::Linear, 0.0}, {128, 1.0, AutomationShape::Linear, 0.0}}});
	automation->lanes.push_back({{AutomationTargetType::Controller, 0, 7, 3, 0}, Kind::Midi, 0, 0.0, 0.0, 1.0,
		{{100, 0.0, AutomationShape::Linear, 0.0}, {300, 1.0, AutomationShape::Step, 0.0},
			{400, 0.0, AutomationShape::Step, 0.0}, {400, 0.5, AutomationShape::Step, 0.0}}});
	player.setAutomation(std::move(automation));

	graph::EventBuffer source(graph::g_maxEventsPerBlock);
	graph::EventBuffer fader(graph::g_maxEventsPerBlock);
	graph::EventBuffer plugins(graph::g_maxEventsPerBlock);
	const graph::TransportSegment segment{0, 512, 0};
	player.play({true, 0, &segment, 1}, {}, {}, source, fader, plugins);

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
	for (const graph::ProcessEvent& event : parameters)
	{
		CHECK(event.ramp == 0);
		CHECK(event.parameter == 2);
	}
	CHECK(parameters[0].offset == 0);
	CHECK(parameters[0].parameterValue == 0.0);
	CHECK(parameters[1].offset == 64);
	CHECK(parameters[1].parameterValue == 0.5);
	CHECK(parameters[2].offset == 128);
	CHECK(parameters[2].parameterValue == 1.0);

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
	player.play({true, 512, &on, 1}, {}, {}, source, fader, plugins);
	CHECK(source.empty());
	CHECK(fader.empty());
	const graph::TransportSegment jump{0, 64, 200};
	player.play({true, 200, &jump, 1}, {}, {}, source, fader, plugins);
	REQUIRE(fader.size() == 2);
	CHECK(fader[0].parameterValue == 1.0); // mid-ramp
	CHECK(fader[0].ramp == 0);
	CHECK(fader[1].ramp == 64);
	CHECK(std::abs(fader[1].parameterValue - 1.32) < 1e-12);

	// Stopped: the values at the cursor, once per cursor position.
	source.clear();
	fader.clear();
	player.play({false, 50, nullptr, 0}, {}, {}, source, fader, plugins);
	REQUIRE(fader.size() == 1);
	CHECK(fader[0].parameterValue == 0.5);
	fader.clear();
	source.clear();
	player.play({false, 50, nullptr, 0}, {}, {}, source, fader, plugins);
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

namespace
{
	// An event the automation player sent, at its project position.
	struct PlayedEvent
	{
		int64_t position = 0;
		graph::ProcessEvent::Type type = graph::ProcessEvent::Type::Parameter;
		uint32_t parameter = 0;
		double value = 0.0; // parameterValue, or the MIDI value
		uint32_t ramp = 0;

		bool operator==(const PlayedEvent& other) const
		{
			return position == other.position && type == other.type && parameter == other.parameter &&
				value == other.value && ramp == other.ramp;
		}
	};

	// Plays frames from position 0 in blocks of blockSize and returns the events for the source.
	std::vector<PlayedEvent> playInBlocks(AutomationPlayer& player, int64_t frames, int64_t blockSize)
	{
		std::vector<PlayedEvent> result;
		graph::EventBuffer source(graph::g_maxEventsPerBlock);
		graph::EventBuffer fader(graph::g_maxEventsPerBlock);
		graph::EventBuffer plugins(graph::g_maxEventsPerBlock);
		for (int64_t start = 0; start < frames; start += blockSize)
		{
			source.clear();
			fader.clear();
			const graph::TransportSegment segment{0, static_cast<uint32_t>(std::min(blockSize, frames - start)), start};
			player.play({true, start, &segment, 1}, {}, {}, source, fader, plugins);
			for (const graph::ProcessEvent& event : source)
				result.push_back({start + event.offset, event.type, event.parameter,
					event.type == graph::ProcessEvent::Type::Parameter ? event.parameterValue
																	   : static_cast<double>(event.value),
					event.ramp});
		}
		return result;
	}
}

TEST_CASE(
	"instrument parameters and MIDI controllers get their values at their own frames for any block size", "[unit]")
{
	using Kind = PlayedLane::Kind;
	using Type = graph::ProcessEvent::Type;
	const auto automation = []()
	{
		auto result = std::make_unique<ChannelAutomation>();
		result->lanes.push_back({{AutomationTargetType::Instrument, 1, 0, 0, 0}, Kind::Instrument, 1, 0.0, 0.0, 1.0,
			{{0, 0.0, AutomationShape::Linear, 0.0}, {1280, 1.0, AutomationShape::Curve, 0.5},
				{2000, 0.25, AutomationShape::Step, 0.0}}});
		result->lanes.push_back({{AutomationTargetType::Controller, 0, 1, 0, 0}, Kind::Midi, 0, 0.0, 0.0, 1.0,
			{{100, 0.0, AutomationShape::Linear, 0.0}, {1500, 1.0, AutomationShape::Curve, -0.5},
				{2500, 0.0, AutomationShape::Step, 0.0}}});
		return result;
	};
	AutomationPlayer whole;
	whole.setAutomation(automation());
	const std::vector<PlayedEvent> expected = playInBlocks(whole, 3000, 3000);

	// The instrument gets each step's value at that step, never one from further on.
	std::vector<PlayedEvent> ramp;
	for (const PlayedEvent& event : expected)
		if (event.type == Type::Parameter && event.position <= 1280)
			ramp.push_back(event);
	REQUIRE(ramp.size() == 21);
	for (size_t i = 0; i < ramp.size(); ++i)
	{
		CHECK(ramp[i].position == static_cast<int64_t>(i) * 64);
		CHECK(ramp[i].value == static_cast<double>(i) / 20.0);
		CHECK(ramp[i].ramp == 0);
	}

	const int blockSize = GENERATE(1, 64, 100, 333);
	INFO("block size " << blockSize);
	AutomationPlayer player;
	player.setAutomation(automation());
	CHECK(playInBlocks(player, 3000, blockSize) == expected);
}

TEST_CASE("a new source processor gets every automation value again", "[unit]")
{
	using Kind = PlayedLane::Kind;
	AutomationPlayer player;
	auto automation = std::make_unique<ChannelAutomation>();
	automation->instrument = "testSynth";
	automation->lanes.push_back(
		{{AutomationTargetType::Instrument, 0, 0, 0, 0}, Kind::Instrument, 0, 0.25, 0.0, 1.0, {}});
	player.setAutomation(std::move(automation));

	// The new automation reaches the audio thread while the old graph still plays, with the old instrument.
	const int oldInstrument = 0;
	const int newInstrument = 0;
	graph::EventBuffer source(graph::g_maxEventsPerBlock);
	graph::EventBuffer fader(graph::g_maxEventsPerBlock);
	graph::EventBuffer plugins(graph::g_maxEventsPerBlock);
	const graph::TransportSegment first{0, 64, 0};
	player.play({true, 0, &first, 1}, {&oldInstrument}, {}, source, fader, plugins);
	CHECK(source.size() == 1);
	source.clear();
	const graph::TransportSegment second{0, 64, 64};
	player.play({true, 64, &second, 1}, {&oldInstrument}, {}, source, fader, plugins);
	CHECK(source.empty());

	// Then the graph with the new instrument: it gets the values at once.
	const graph::TransportSegment third{0, 64, 128};
	player.play({true, 128, &third, 1}, {&newInstrument}, {}, source, fader, plugins);
	REQUIRE(source.size() == 1);
	CHECK(source[0].offset == 0);
	CHECK(source[0].parameterValue == 0.25);
	source.clear();
	player.play({false, 192, nullptr, 0}, {&newInstrument}, {}, source, fader, plugins);
	source.clear();
	player.play({false, 192, nullptr, 0}, {&oldInstrument}, {}, source, fader, plugins); // stopped too
	CHECK(source.size() == 1);
}

TEST_CASE("MIDI controller ramps send only changes and leave room for notes", "[unit]")
{
	using Kind = PlayedLane::Kind;
	using Type = graph::ProcessEvent::Type;
	constexpr uint32_t blockSize = 8192;

	// A slow ramp changes the MIDI 1.0 value twice in a block: three events, not one every 64 frames.
	{
		AutomationPlayer player;
		auto automation = std::make_unique<ChannelAutomation>();
		automation->lanes.push_back({{AutomationTargetType::Controller, 0, 7, 0, 0}, Kind::Midi, 0, 0.0, 0.0, 1.0,
			{{0, 0.0, AutomationShape::Linear, 0.0},
				{int64_t{blockSize} * 2, 5.0 / 128.0, AutomationShape::Step, 0.0}}});
		player.setAutomation(std::move(automation));
		const std::vector<PlayedEvent> events = playInBlocks(player, blockSize, blockSize);
		REQUIRE(events.size() == 3);
		CHECK(events[0].position == 0);
		CHECK(gnubase::engine::midi::controllerToMidi1(static_cast<uint32_t>(events[1].value)) == 1);
		CHECK(gnubase::engine::midi::controllerToMidi1(static_cast<uint32_t>(events[2].value)) == 2);
	}

	// Sixteen controllers ramping over the whole range in one block, and a sequencer's notes after them.
	auto state = std::make_shared<ChannelState>();
	auto automation = std::make_unique<ChannelAutomation>();
	for (uint8_t number = 0; number < 16; ++number)
		automation->lanes.push_back({{AutomationTargetType::Controller, 0, number, 0, 0}, Kind::Midi, 0, 0.0, 0.0, 1.0,
			{{0, 0.0, AutomationShape::Linear, 0.0}, {blockSize, 1.0, AutomationShape::Step, 0.0}}});
	state->automation.setAutomation(std::move(automation));
	auto sequencer = std::make_shared<MidiSequencer>();
	auto sequence = std::make_unique<MidiSequence>();
	for (int64_t i = 0; i < 200; ++i)
		sequence->notes.push_back({i * 40, (i * 40) + 20, static_cast<uint8_t>(i % 128), 30000, 1});
	sequencer->setSequence(std::move(sequence));
	ChannelStrip strip(state, sequencer);
	strip.prepareFor(48000.0, blockSize);
	const graph::EventBuffer in(graph::g_maxEventsPerBlock);
	graph::EventBuffer out(graph::g_maxEventsPerBlock);
	const graph::TransportSegment segment{0, blockSize, 0};
	const graph::TransportBlock transport{true, 0, &segment, 1};
	strip.process({nullptr, 0, blockSize, in, out, transport});

	size_t on = 0;
	size_t off = 0;
	size_t controllers = 0;
	for (const graph::ProcessEvent& event : out)
	{
		on += event.type == Type::NoteOn ? 1 : 0;
		off += event.type == Type::NoteOff ? 1 : 0;
		controllers += event.type == Type::Controller ? 1 : 0;
	}
	CHECK(on == 200);
	CHECK(off == 200);
	CHECK(controllers >= 16);
	CHECK(controllers <= (graph::g_maxEventsPerBlock / 2) + 16);
}

TEST_CASE("projects whose automation or channels have values out of range or not finite are refused", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId audio = idOf(run(client, "track.add", {{"type", "audio"}}), "track");
	const ObjectId midi = idOf(run(client, "track.add", {{"type", "midi"}}), "track");
	run(client, "automationPoint.add",
		{{"lane",
			 idOf(run(client, "automationLane.add", {{"track", audio}, {"target", {{"type", "volume"}}}}), "lane")},
			{"points", {{{"position", 0}, {"value", -6.0}, {"shape", "curve"}, {"curve", 0.5}}}}});
	run(client, "automationPoint.add",
		{{"lane",
			 idOf(run(client, "automationLane.add",
					  {{"track", midi}, {"target", {{"type", "controller"}, {"number", 7}, {"channel", 2}}}}),
				 "lane")},
			{"points", {{{"position", 0}, {"value", 0.5}}}}});
	const Json good = run(client, "project.get");
	CHECK(projectFromJson(good).automation.size() == 2);

	// Each change makes the file one that the commands couldn't have made.
	const auto refused = [&good](const std::function<void(Json&)>& change)
	{
		Json bad = good;
		change(bad);
		CHECK_THROWS_AS(projectFromJson(bad), std::invalid_argument);
	};
	const double nan = std::numeric_limits<double>::quiet_NaN();
	const double infinity = std::numeric_limits<double>::infinity();
	refused([](Json& p) { p["automation"][0]["points"][0]["curve"] = -200.0; }); // exp() would overflow
	refused([nan](Json& p) { p["automation"][0]["points"][0]["curve"] = nan; });
	refused([nan](Json& p) { p["automation"][0]["points"][0]["value"] = nan; });
	refused([infinity](Json& p) { p["automation"][0]["points"][0]["value"] = -infinity; });
	refused([](Json& p) { p["automation"][0]["points"][0]["value"] = 7.0; });
	refused([](Json& p) { p["automation"][0]["points"][0]["position"] = -1; });
	refused([](Json& p) { p["automation"][0]["points"][0]["shape"] = "wobble"; });
	refused([](Json& p) { p["automation"][1]["points"][0]["value"] = 1.5; });
	refused([](Json& p) { p["automation"][0]["track"] = 9999; });
	refused([midi](Json& p) { p["automation"][0]["track"] = midi; });   // a MIDI track has no volume
	refused([audio](Json& p) { p["automation"][1]["track"] = audio; }); // nor an audio track controllers
	refused([](Json& p) { p["automation"][0]["target"]["type"] = "brightness"; });
	refused([](Json& p) { p["automation"][1]["target"]["number"] = 300; });
	refused([](Json& p) { p["automation"][1]["target"]["channel"] = 16; });
	refused([](Json& p) { p["channels"][0]["volume"] = 1000.0; });
	refused([nan](Json& p) { p["channels"][0]["pan"] = nan; });
}

namespace
{
	Json insertPlugin(Client& client, ObjectId channel, const char* plugin)
	{
		return run(client, "plugin.insert", {{"channel", channel}, {"plugin", plugin}}).at("slot").at("id");
	}

	// Waits until a slot's plugin is active, then builds the graph with it, as the engine does on its own then.
	void waitUntilActive(Client& client, const Json& slot)
	{
		const auto status = [&]()
		{ return run(client, "plugin.get", {{"slot", slot}}).at("slot").at("status").get<std::string>(); };
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (status() != "active" && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		REQUIRE(status() == "active");
		run(client, "audio.updatePlugins");
	}

	Json pluginTarget(const Json& slot, uint32_t parameter)
	{
		return {{"type", "plugin"}, {"slot", slot}, {"parameter", parameter}};
	}

	std::vector<ObjectId> laneIds(Client& client)
	{
		std::vector<ObjectId> ids;
		const Json lanes = run(client, "automationLane.list").at("lanes");
		for (const Json& lane : lanes)
			ids.push_back(lane.at("id").get<ObjectId>());
		return ids;
	}
}

TEST_CASE("the automation player sends plugin parameters to their slots, and new plugins get every value", "[unit]")
{
	using Kind = PlayedLane::Kind;
	using Type = graph::ProcessEvent::Type;
	AutomationPlayer player;
	auto automation = std::make_unique<ChannelAutomation>();
	automation->lanes.push_back({{AutomationTargetType::Plugin, 3, 0, 0, 77}, Kind::Plugin, 3, 0.5, 0.0, 1.0,
		{{0, 0.0, AutomationShape::Step, 0.0}, {100, 1.0, AutomationShape::Step, 0.0}}});
	player.setAutomation(std::move(automation));

	graph::EventBuffer source(graph::g_maxEventsPerBlock);
	graph::EventBuffer fader(graph::g_maxEventsPerBlock);
	graph::EventBuffer plugins(graph::g_maxEventsPerBlock);
	const int instrument = 0;
	const int plugin = 0;
	const int newPlugin = 0;
	const std::vector<ObjectId> slots{11, 12, 77};
	const graph::TransportSegment first{0, 200, 0};
	player.play({true, 0, &first, 1}, {&instrument, nullptr, nullptr, &plugin}, slots, source, fader, plugins);
	CHECK(source.empty());
	CHECK(fader.empty());
	// The value at the start, then the step at its sample, both keyed by the slot's index.
	REQUIRE(plugins.size() == 2);
	for (const graph::ProcessEvent& event : plugins)
	{
		CHECK(event.type == Type::Parameter);
		CHECK(event.parameter == 3);
		CHECK(event.key == 2);
	}
	CHECK(plugins[0].offset == 0);
	CHECK(plugins[0].parameterValue == 0.0);
	CHECK(plugins[1].offset == 100);
	CHECK(plugins[1].parameterValue == 1.0);

	plugins.clear();
	const graph::TransportSegment second{0, 64, 200};
	player.play({true, 200, &second, 1}, {&instrument, nullptr, nullptr, &plugin}, slots, source, fader, plugins);
	CHECK(plugins.empty());

	// The slot's plugin is made again (e.g. activated anew): it gets the value at once.
	const graph::TransportSegment third{0, 64, 264};
	player.play({true, 264, &third, 1}, {&instrument, nullptr, nullptr, &newPlugin}, slots, source, fader, plugins);
	REQUIRE(plugins.size() == 1);
	CHECK(plugins[0].offset == 0);
	CHECK(plugins[0].parameterValue == 1.0);
}

namespace
{
	// A plugin that keeps the values of parameter 0 that it gets.
	class ParameterRecorder : public graph::Processor
	{
	public:
		void process(const graph::ProcessContext& context) noexcept override
		{
			for (const graph::ProcessEvent& event : context.eventsIn)
				if (event.type == graph::ProcessEvent::Type::Parameter && event.parameter == 0)
					values.push_back(event.parameterValue);
		}

		std::vector<double> values;
	};
}

TEST_CASE("plugin automation reaches the slots by their ids while a program for another chain still plays", "[unit]")
{
	// A plugin C was inserted above A and B: the automation for [C, A, B] arrives before the program, whose strips
	// are still those for [A, B].
	const ObjectId a = 1;
	const ObjectId b = 2;
	const ObjectId c = 3;
	RenderChannel channel;
	for (const auto& [id, gain] : {std::pair{c, 3.0}, std::pair{a, -6.0}, std::pair{b, -12.0}})
	{
		RenderSlot slot;
		slot.id = id;
		slot.plugin = "builtin:gainPan";
		slot.parameters[0] = gain;
		channel.slots.push_back(slot);
	}
	auto state = std::make_shared<ChannelState>();
	state->automation.setAutomation(channelAutomation(channel, 6.0));

	auto pluginA = std::make_shared<ParameterRecorder>();
	auto pluginB = std::make_shared<ParameterRecorder>();
	ChannelStrip source(
		state, nullptr, {}, std::nullopt, false, StripPart::Source, {}, {pluginA.get(), pluginB.get()}, {a, b});
	SlotStrip slotA(state, pluginA, 0);
	SlotStrip slotB(state, pluginB, 1);
	for (graph::Processor* processor : std::initializer_list<graph::Processor*>{&source, &slotA, &slotB})
		processor->prepareFor(48000.0, 64);
	const graph::EventBuffer in(graph::g_maxEventsPerBlock);
	graph::EventBuffer out(graph::g_maxEventsPerBlock);
	const graph::TransportSegment segment{0, 64, 0};
	const graph::TransportBlock transport{true, 0, &segment, 1};
	for (graph::Processor* processor : std::initializer_list<graph::Processor*>{&source, &slotA, &slotB})
		processor->process({nullptr, 0, 64, in, out, transport});

	// Each plugin gets its own value, none of C's or the other one's.
	CHECK(pluginA->values == std::vector<double>{-6.0});
	CHECK(pluginB->values == std::vector<double>{-12.0});
}

TEST_CASE("plugin parameters in slots have automation lanes that go with the plugin", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId track = idOf(run(client, "track.add", {{"type", "audio"}}), "track");
	const ObjectId other = idOf(run(client, "track.add", {{"type", "audio"}}), "track");
	const ObjectId channel = channelOf(client, track);
	const Json gain = insertPlugin(client, channel, "builtin:gainPan");
	const Json eq = insertPlugin(client, channel, "builtin:eq");
	const Json elsewhere = insertPlugin(client, channelOf(client, other), "builtin:gainPan");

	const Json added = run(client, "automationLane.add", {{"track", track}, {"target", pluginTarget(gain, 0)}});
	CHECK(added.at("lane").at("target") == pluginTarget(gain, 0));
	const ObjectId lane = idOf(added, "lane");
	CHECK_THAT(reject(client, "automationLane.add", {{"track", track}, {"target", pluginTarget(elsewhere, 0)}}),
		ContainsSubstring("has no plugin slot"));
	CHECK_THAT(reject(client, "automationLane.add", {{"track", track}, {"target", pluginTarget(gain, 99)}}),
		ContainsSubstring("has no parameter 99"));
	CHECK_THAT(
		reject(client, "automationLane.add", {{"track", track}, {"target", {{"type", "plugin"}, {"parameter", 0}}}}),
		ContainsSubstring("target.slot"));
	CHECK_THAT(
		reject(client, "automationPoint.add", {{"lane", lane}, {"points", {{{"position", 0}, {"value", -100.0}}}}}),
		ContainsSubstring("from -96 to 12"));
	run(client, "automationPoint.add",
		{{"lane", lane}, {"points", {{{"position", 0}, {"value", -6.0}, {"shape", "curve"}, {"curve", 0.5}}}}});

	// A stepped parameter has a step curve with whole values (reference manual p. 870).
	Json stepped;
	const Json parameters = run(client, "parameter.list", {{"channel", channel}}).at("parameters");
	for (const Json& parameter : parameters)
		if (stepped.is_null() && parameter.at("target").value("slot", Json()) == eq &&
			parameter.at("stepped") == true && parameter.at("maximum").get<double>() >= 1.0)
			stepped = parameter;
	REQUIRE(!stepped.is_null());
	const ObjectId steps =
		idOf(run(client, "automationLane.add", {{"track", track}, {"target", stepped.at("target")}}), "lane");
	const Json point =
		run(client, "automationPoint.add", {{"lane", steps}, {"points", {{{"position", 0}, {"value", 0.75}}}}})
			.at("points")
			.at(0);
	CHECK(point.at("shape") == "step");
	CHECK(point.at("value") == 1.0);
	const Json changed = run(client, "automationPoint.set",
		{{"lane", steps}, {"points", {{{"id", point.at("id")}, {"value", 0.25}, {"shape", "linear"}}}}})
							 .at("points")
							 .at(0);
	CHECK(changed.at("shape") == "step");
	CHECK(changed.at("value") == 0.0);

	// The lanes are saved and loaded with the project.
	const Project loaded = projectFromJson(run(client, "project.get"));
	CHECK(loaded.automation.at(lane).target.slot == gain.get<ObjectId>());

	// A copy of the track automates the copies of its plugins.
	const ObjectId copy = idOf(run(client, "track.duplicate", {{"track", track}}), "track");
	const Json copySlots = run(client, "plugin.list", {{"channel", channelOf(client, copy)}}).at("slots");
	const Json copyLanes = run(client, "automationLane.list", {{"track", copy}}).at("lanes");
	REQUIRE(copyLanes.size() == 2);
	CHECK(copyLanes.at(0).at("target") == pluginTarget(copySlots.at(0).at("id"), 0));
	CHECK(copyLanes.at(0).at("points").size() == 1);

	// Moving the plugin to another slot of the channel keeps its automation; to another channel, or removing it,
	// takes it away, as one undo step each.
	run(client, "plugin.move", {{"slot", gain}, {"index", 1}});
	CHECK(run(client, "automationLane.list", {{"track", track}}).at("lanes").size() == 2);
	const std::vector<ObjectId> before = laneIds(client);
	run(client, "plugin.move", {{"slot", gain}, {"channel", channelOf(client, other)}, {"index", 0}});
	CHECK(run(client, "automationLane.list", {{"track", track}}).at("lanes").size() == 1);
	run(client, "history.undo");
	CHECK(laneIds(client) == before);
	run(client, "plugin.remove", {{"slot", gain}});
	CHECK(run(client, "automationLane.list", {{"track", track}}).at("lanes").size() == 1);
	run(client, "history.undo");
	CHECK(laneIds(client) == before);
}

TEST_CASE("plugin parameter automation plays sample-accurately in its slot", "[integration]")
{
	const int blockSize = GENERATE(64, 100);
	INFO("block size " << blockSize);
	Mixer mixer(blockSize);
	const Json slot = insertPlugin(mixer.client, mixer.channel, "builtin:gainPan");
	waitUntilActive(mixer.client, slot);
	const ObjectId lane = idOf(
		run(mixer.client, "automationLane.add", {{"track", mixer.track}, {"target", pluginTarget(slot, 0)}}), "lane");
	// Steps at their samples; a ramp as the value of every 64th frame from its start, since gainPan doesn't glide.
	mixer.add(lane, {{1000, -12.0, "step"}, {2000, -12.0, "linear"}, {3000, 0.0, "step"}, {3500, -6.0, "step"}});
	run(mixer.client, "transport.play");
	run(mixer.client, "audio.render", {{"frames", 4000}});
	// Without reading, the plugin returns to its static value.
	run(mixer.client, "automationLane.set", {{"lane", lane}, {"read", false}});
	run(mixer.client, "audio.render", {{"frames", 500}});

	const auto expected = [](int64_t position)
	{
		if (position >= 4000)
			return 1.0;
		double decibels = -12.0;
		if (position >= 3500)
		{
			decibels = -6.0;
		}
		else if (position >= 3000)
		{
			decibels = 0.0;
		}
		else if (position >= 2000)
		{
			const int64_t sinceRamp = (position - 2000) / 64 * 64;
			decibels = -12.0 + (12.0 * static_cast<double>(sinceRamp) / 1000.0);
		}
		return gain(decibels);
	};
	Mixer reference(64);
	run(reference.client, "audio.render", {{"frames", 4500}});
	const std::vector<float> input = reference.capture(0, 4500);
	size_t wrong = 0;
	size_t first = 0;
	for (int channel = 0; channel < 2; ++channel)
	{
		const std::vector<float> output = mixer.capture(channel, 4500);
		for (size_t i = 0; i < output.size(); ++i)
			if (std::abs(output[i] - (input[i] * expected(static_cast<int64_t>(i)))) > 1e-6 && wrong++ == 0)
				first = i;
	}
	INFO(wrong << " wrong samples, the first at frame " << first);
	CHECK(wrong == 0);
}

TEST_CASE("mute automation is a step curve that mutes from its sample and overrides the mute button", "[integration]")
{
	const int blockSize = GENERATE(64, 100);
	INFO("block size " << blockSize);
	Mixer mixer(blockSize);
	CHECK_THAT(reject(mixer.client, "automationLane.add",
				   {{"track", idOf(run(mixer.client, "track.add", {{"type", "vca"}}), "track")},
					   {"target", {{"type", "mute"}}}}),
		ContainsSubstring("VCA"));
	run(mixer.client, "channel.set", {{"channel", mixer.channel}, {"mute", true}});
	const ObjectId lane = mixer.lane({{"type", "mute"}});
	const Json points = run(mixer.client, "automationPoint.add",
		{{"lane", lane},
			{"points",
				{{{"position", 0}, {"value", 0.0}}, {{"position", ticks(1000)}, {"value", 0.6}},
					{{"position", ticks(3000)}, {"value", 0.0}, {"shape", "linear"}}}}})
							.at("points");
	for (const Json& point : points)
		CHECK(point.at("shape") == "step");
	CHECK(points.at(1).at("value") == 1.0);
	run(mixer.client, "transport.play");
	run(mixer.client, "audio.render", {{"frames", 4000}});
	// Without reading, the mute button mutes again.
	run(mixer.client, "automationLane.set", {{"lane", lane}, {"read", false}});
	run(mixer.client, "audio.render", {{"frames", 1000}});

	// Mute glides over 5 ms (240 frames) from the point's sample on, so it doesn't click.
	const auto audible = [](int64_t position)
	{
		if (position < 1000)
			return 1.0;
		if (position < 3000)
			return std::max(0.0, 1.0 - (static_cast<double>(position - 1000) / 240.0));
		return std::min(1.0, static_cast<double>(position - 3000) / 240.0);
	};
	Mixer reference(64);
	run(reference.client, "audio.render", {{"frames", 5000}});
	const std::vector<float> input = reference.capture(0, 5000);
	size_t wrong = 0;
	size_t first = 0;
	for (int channel = 0; channel < 2; ++channel)
	{
		const std::vector<float> output = mixer.capture(channel, 5000);
		for (size_t i = 0; i < 4000; ++i)
			if (std::abs(output[i] - (input[i] * audible(static_cast<int64_t>(i)))) > 1e-6 && wrong++ == 0)
				first = i;
		CHECK(std::all_of(output.begin() + 4500, output.end(), [](float sample) { return sample == 0.0f; }));
	}
	INFO(wrong << " wrong samples, the first at frame " << first);
	CHECK(wrong == 0);
}

TEST_CASE("virgin territory is a project setting, and terminators end curves, with undo", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId track = idOf(run(client, "track.add", {{"type", "audio"}}), "track");
	const ObjectId lane =
		idOf(run(client, "automationLane.add", {{"track", track}, {"target", {{"type", "volume"}}}}), "lane");
	const Json points = run(client, "automationPoint.add",
		{{"lane", lane},
			{"points",
				{{{"position", 0}, {"value", -6.0}, {"terminator", true}}, {{"position", 960}, {"value", 0.0}}}}})
							.at("points");
	CHECK(points[0].at("terminator") == true);
	CHECK(!points[1].contains("terminator")); // only on terminators
	const auto first = points[0].at("id").get<ObjectId>();
	const auto terminatorOf = [&]()
	{ return run(client, "automationLane.list").at("lanes")[0].at("points")[0].value("terminator", false); };
	run(client, "automationPoint.set", {{"lane", lane}, {"points", {{{"id", first}, {"terminator", false}}}}});
	CHECK(!terminatorOf());
	run(client, "history.undo");
	CHECK(terminatorOf());

	CHECK(run(client, "automation.get") == Json{{"followsEvents", false}, {"virginTerritory", false}});
	const size_t steps = run(client, "history.get").at("steps").size();
	CHECK(run(client, "automation.set", {{"virginTerritory", true}}) ==
		Json{{"followsEvents", false}, {"virginTerritory", true}});
	CHECK(run(client, "history.get").at("steps").size() == steps); // a setting, not an undo step
	run(client, "history.undo");                                   // the points' addition
	CHECK(run(client, "automation.get").at("virginTerritory") == true);
	run(client, "history.redo");

	// Saved with the project, with the terminators; a new project starts with initial values.
	const Project saved = projectFromJson(run(client, "project.get"));
	CHECK(saved.virginTerritory);
	CHECK(saved.automation.at(lane).points.at(first).terminator);
	CHECK(toJson(saved).at("virginTerritory") == true);
	run(client, "project.new");
	CHECK(run(client, "automation.get").at("virginTerritory") == false);
}

TEST_CASE("only virgin territory changes the project, and a failed transaction takes the settings back", "[unit]")
{
	Engine engine;
	Client client(engine);
	const auto revision = [&client]() { return run(client, "project.revision"); };
	const Json start = revision();
	CHECK(start.at("modified") == false);

	// followsEvents is a user setting: the project stays as it is, also when virginTerritory is given unchanged.
	run(client, "automation.set", {{"followsEvents", true}});
	run(client, "automation.set", {{"followsEvents", false}, {"virginTerritory", false}});
	CHECK(revision() == start);

	// virginTerritory is the project's.
	run(client, "automation.set", {{"virginTerritory", true}});
	const Json changed = revision();
	CHECK(changed.at("revision").get<uint64_t>() > start.at("revision").get<uint64_t>());
	CHECK(changed.at("modified") == true);

	// A transaction that fails leaves both settings as they were.
	const std::string error = reject(client, "history.transaction",
		{{"description", "Settings"},
			{"commands",
				{{{"command", "automation.set"}, {"parameters", {{"followsEvents", true}, {"virginTerritory", false}}}},
					{{"command", "track.remove"}, {"parameters", {{"track", 12345}}}}}}});
	CHECK_THAT(error, ContainsSubstring("commands[1]"));
	CHECK(run(client, "automation.get") == Json{{"followsEvents", false}, {"virginTerritory", true}});
	CHECK(revision() == changed);

	// One that succeeds keeps them.
	run(client, "history.transaction",
		{{"description", "Settings"},
			{"commands",
				{{{"command", "automation.set"},
					{"parameters", {{"followsEvents", true}, {"virginTerritory", false}}}}}}});
	CHECK(run(client, "automation.get") == Json{{"followsEvents", true}, {"virginTerritory", false}});
	run(client, "automation.set", {{"followsEvents", false}});
}

TEST_CASE("in virgin territory and after terminators a lane plays its static value", "[unit]")
{
	using gnubase::time::TimeBase;
	Project project = newProject();
	const ObjectId track = project.newId();
	const ObjectId channel = project.newId();
	project.trackById[track] = {track, TrackType::Audio, "Audio", "", TimeBase::Linear, 0, channel, {}, {}, {}};
	project.tracks.push_back(track);
	project.channels[channel] = {channel, track, ChannelFormat::Stereo, 0, 0, {}, -20.0, 0.0};
	project.channels[channel].mute = true;
	const auto lane = [&](AutomationTargetType type, std::initializer_list<AutomationPoint> points)
	{
		AutomationLane added;
		added.id = project.newId();
		added.track = track;
		added.target.type = type;
		for (AutomationPoint point : points)
		{
			point.id = project.newId();
			added.points[point.id] = point;
		}
		project.automation[added.id] = added;
	};
	const auto at = [](int64_t samples) { return gnubase::time::Position{TimeBase::Linear, samples}; };
	lane(AutomationTargetType::Volume,
		{{0, at(1000), -6.0, AutomationShape::Linear, 0.0}, {0, at(2000), -12.0, AutomationShape::Linear, 0.0, true},
			{0, at(3000), 0.0, AutomationShape::Linear, 0.0}, {0, at(4000), -6.0, AutomationShape::Linear, 0.0}});
	lane(AutomationTargetType::Mute,
		{{0, at(1000), 0.0, AutomationShape::Step, 0.0}, {0, at(2000), 0.0, AutomationShape::Step, 0.0}});
	// A lane whose points all sit at one position (#717).
	lane(AutomationTargetType::Pan,
		{{0, at(5000), -0.5, AutomationShape::Linear, 0.0}, {0, at(5000), 0.5, AutomationShape::Linear, 0.0}});
	const auto valueAt = [&](AutomationTargetType type, int64_t position)
	{
		const std::unique_ptr<ChannelAutomation> automation =
			channelAutomation(compile(project, 1)->channels.at(0), 6.0);
		for (const PlayedLane& played : automation->lanes)
			if (played.target.type == type)
				return automationValue(played.points, position);
		FAIL("no lane");
		return 0.0;
	};
	const auto near = [](double a, double b) { return std::abs(a - b) < 1e-12; };

	SECTION("initial value")
	{
		// The curve covers the whole timeline, but for the gap after the terminator.
		CHECK(near(valueAt(AutomationTargetType::Volume, 0), gain(-6.0)));
		CHECK(near(valueAt(AutomationTargetType::Volume, 1500), (gain(-6.0) + gain(-12.0)) / 2.0));
		CHECK(near(valueAt(AutomationTargetType::Volume, 2000), gain(-20.0)));
		CHECK(near(valueAt(AutomationTargetType::Volume, 2999), gain(-20.0)));
		CHECK(near(valueAt(AutomationTargetType::Volume, 3000), gain(0.0)));
		CHECK(near(valueAt(AutomationTargetType::Volume, 9000), gain(-6.0)));
		CHECK(valueAt(AutomationTargetType::Mute, 0) == 0.0);
		CHECK(valueAt(AutomationTargetType::Mute, 9000) == 0.0);
		CHECK(valueAt(AutomationTargetType::Pan, 0) == -0.5);
		CHECK(valueAt(AutomationTargetType::Pan, 5000) == 0.5);
	}
	SECTION("virgin territory")
	{
		project.virginTerritory = true;
		// Before the first point and after the last one too; a mute lane's gaps have the mute button.
		CHECK(near(valueAt(AutomationTargetType::Volume, 0), gain(-20.0)));
		CHECK(near(valueAt(AutomationTargetType::Volume, 999), gain(-20.0)));
		CHECK(near(valueAt(AutomationTargetType::Volume, 1000), gain(-6.0)));
		CHECK(near(valueAt(AutomationTargetType::Volume, 2500), gain(-20.0)));
		CHECK(near(valueAt(AutomationTargetType::Volume, 3500), (gain(0.0) + gain(-6.0)) / 2.0));
		CHECK(near(valueAt(AutomationTargetType::Volume, 4000), gain(-20.0)));
		CHECK(near(valueAt(AutomationTargetType::Volume, 9000), gain(-20.0)));
		CHECK(valueAt(AutomationTargetType::Mute, 500) == 1.0);
		CHECK(valueAt(AutomationTargetType::Mute, 1500) == 0.0);
		CHECK(valueAt(AutomationTargetType::Mute, 2500) == 1.0);
		// Points at one position have no curve between them and the gaps around them: they never play, and the
		// parameter keeps its static value there too.
		project.channels[channel].pan = 0.25;
		for (const int64_t position : {0, 4999, 5000, 9000})
			CHECK(valueAt(AutomationTargetType::Pan, position) == 0.25);
	}
	SECTION("MIDI lanes' gaps")
	{
		// A controller lane without a static value of the channel sends nothing in its gaps, like a lane that doesn't
		// read: here before 100, from the terminator at 300 up to 400 and after 500. After a gap its value is sent
		// even where it is the gap point's.
		using Type = graph::ProcessEvent::Type;
		const auto gap = [](int64_t position)
		{ return RenderAutomationPoint{position, 0.0, AutomationShape::Step, 0.0, true}; };
		RenderChannel midi;
		midi.type = TrackType::Midi;
		midi.format = ChannelFormat::None;
		midi.automation = {{{AutomationTargetType::Controller, 0, 11, 0, 0},
			{gap(100), {100, 0.5, AutomationShape::Linear, 0.0}, {300, 1.0, AutomationShape::Step, 0.0}, gap(300),
				{400, 0.0, AutomationShape::Step, 0.0}, {500, 0.0, AutomationShape::Step, 0.0}, gap(500)}}};
		AutomationPlayer player;
		player.setAutomation(channelAutomation(midi, 6.0));
		std::vector<int64_t> sent;
		for (const PlayedEvent& event : playInBlocks(player, 1000, 1000))
		{
			CHECK(event.type == Type::Controller);
			sent.push_back(event.position);
		}
		CHECK(sent == std::vector<int64_t>{100, 164, 228, 292, 300, 400});

		// A first point of 0 after virgin territory, e.g. the sustain pedal up, is sent too.
		RenderChannel pedal = midi;
		pedal.automation[0].points = {gap(100), {100, 0.0, AutomationShape::Step, 0.0}};
		AutomationPlayer pedalPlayer;
		pedalPlayer.setAutomation(channelAutomation(pedal, 6.0));
		const std::vector<PlayedEvent> pedalEvents = playInBlocks(pedalPlayer, 1000, 1000);
		REQUIRE(pedalEvents.size() == 1);
		CHECK(pedalEvents[0].position == 100);
		CHECK(pedalEvents[0].value == 0);

		// A lone point between two gaps, as compile() makes it of a single point with virgin territory, is sent once,
		// like a terminator's value, and nothing after it.
		RenderChannel lone = midi;
		lone.automation[0].points = {gap(100), {100, 1.0, AutomationShape::Step, 0.0}, gap(100)};
		AutomationPlayer lonePlayer;
		lonePlayer.setAutomation(channelAutomation(lone, 6.0));
		const std::vector<PlayedEvent> loneEvents = playInBlocks(lonePlayer, 1000, 1000);
		REQUIRE(loneEvents.size() == 1);
		CHECK(loneEvents[0].position == 100);
		CHECK(loneEvents[0].value > 0.0);

		// Nor at a locate into a gap, or stopped in one.
		graph::EventBuffer source(graph::g_maxEventsPerBlock);
		graph::EventBuffer fader(graph::g_maxEventsPerBlock);
		graph::EventBuffer plugins(graph::g_maxEventsPerBlock);
		const graph::TransportSegment intoGap{0, 50, 320};
		player.play({true, 320, &intoGap, 1}, {}, {}, source, fader, plugins);
		CHECK(source.empty());
		for (const int64_t position : {50, 350, 600})
		{
			player.play({false, position, nullptr, 0}, {}, {}, source, fader, plugins);
			CHECK(source.empty());
		}
		player.play({false, 450, nullptr, 0}, {}, {}, source, fader, plugins);
		REQUIRE(source.size() == 1);
		CHECK(source[0].value == 0);
	}
}

TEST_CASE("volume automation plays its static value in virgin territory and gaps", "[integration]")
{
	const int blockSize = GENERATE(64, 100);
	const bool virgin = GENERATE(false, true);
	INFO("block size " << blockSize << (virgin ? ", virgin territory" : ", initial value"));
	Mixer mixer(blockSize);
	run(mixer.client, "channel.set", {{"channel", mixer.channel}, {"volume", -20.0}});
	run(mixer.client, "automation.set", {{"virginTerritory", virgin}});
	const ObjectId lane = mixer.lane({{"type", "volume"}});
	run(mixer.client, "automationPoint.add",
		{{"lane", lane},
			{"points",
				{{{"position", ticks(1000)}, {"value", -6.0}},
					{{"position", ticks(2000)}, {"value", -12.0}, {"terminator", true}},
					{{"position", ticks(3000)}, {"value", 0.0}}, {{"position", ticks(4000)}, {"value", -6.0}}}}});
	run(mixer.client, "transport.play");
	run(mixer.client, "audio.render", {{"frames", 5000}});
	// The static value, -20 dB, from the terminator to the next point, and in virgin territory before the first point
	// and after the last one.
	std::vector<TestPoint> expected = {{1000, -6.0}, {2000, -12.0}, {2000, -20.0, "step"}, {3000, 0.0}, {4000, -6.0}};
	if (virgin)
	{
		expected.insert(expected.begin(), {1000, -20.0, "step"});
		expected.push_back({4000, -20.0, "step"});
	}
	checkGains(mixer, expected, positions(0, 5000));
}

namespace
{
	// The lane of a track with a target of this type: its id and its points as (position, value) pairs in the order
	// they play; 0 and none if the track has no such lane.
	struct LanePoints
	{
		ObjectId id = 0;
		std::vector<std::pair<int64_t, double>> points;
	};

	LanePoints lanePoints(Client& client, ObjectId track, const std::string& type)
	{
		LanePoints found;
		const Json lanes = run(client, "automationLane.list", {{"track", track}}).at("lanes");
		for (const Json& lane : lanes)
			if (lane.at("target").at("type") == type)
			{
				found.id = lane.at("id").get<ObjectId>();
				for (const Json& point : lane.at("points"))
					found.points.emplace_back(point.at("position").get<int64_t>(), point.at("value").get<double>());
			}
		return found;
	}

	std::vector<ObjectId> sortedIds(const Json& ids)
	{
		auto sorted = ids.get<std::vector<ObjectId>>();
		std::sort(sorted.begin(), sorted.end());
		return sorted;
	}
}

TEST_CASE("automation.copy and automation.paste copy automation between tracks with other parameters", "[unit]")
{
	using Points = std::vector<std::pair<int64_t, double>>;
	Engine engine;
	Client client(engine);
	const auto addTrack = [&](const char* type)
	{
		const ObjectId track = idOf(run(client, "track.add", {{"type", type}}), "track");
		run(client, "track.set", {{"track", track}, {"timeBase", "linear"}}); // points in samples
		return track;
	};
	const auto addLane = [&](ObjectId track, const Json& target, const Json& points)
	{
		const ObjectId id = idOf(run(client, "automationLane.add", {{"track", track}, {"target", target}}), "lane");
		run(client, "automationPoint.add", {{"lane", id}, {"points", points}});
		return id;
	};
	// The source tracks: an audio track with Gain and pan and Gate in its slots, and an instrument track.
	const ObjectId audio = addTrack("audio");
	const Json gainPan = insertPlugin(client, channelOf(client, audio), "builtin:gainPan");
	const Json gate = insertPlugin(client, channelOf(client, audio), "builtin:gate");
	const ObjectId volume = addLane(audio, {{"type", "volume"}},
		{{{"position", 0}, {"value", -6.0}}, {{"position", 48000}, {"value", 0.0}},
			{{"position", 96000}, {"value", -12.0}, {"shape", "step"}}});
	const ObjectId pan =
		addLane(audio, {{"type", "pan"}}, {{{"position", 0}, {"value", -1.0}}, {{"position", 96000}, {"value", 1.0}}});
	addLane(audio, pluginTarget(gainPan, 0), {{{"position", 0}, {"value", -10.0}}});
	addLane(audio, pluginTarget(gate, 0), {{{"position", 0}, {"value", -40.0}}});
	const ObjectId instrument = addTrack("instrument");
	run(client, "channel.set", {{"channel", channelOf(client, instrument)}, {"instrument", "testSynth"}});
	addLane(instrument, {{"type", "volume"}}, {{{"position", 0}, {"value", -3.0}}});
	addLane(instrument, {{"type", "instrument"}, {"parameter", gnubase::engine::processors::TestSynth::Level}},
		{{{"position", 0}, {"value", 0.5}}});
	addLane(instrument, {{"type", "controller"}, {"number", 1}}, {{{"position", 0}, {"value", 0.25}}});
	// The target tracks: an audio track with Gain and pan and Delay, a MIDI track and another audio track.
	const ObjectId target = addTrack("audio");
	const Json targetGainPan = insertPlugin(client, channelOf(client, target), "builtin:gainPan");
	insertPlugin(client, channelOf(client, target), "builtin:delay");
	addLane(target, {{"type", "volume"}}, {{{"position", 0}, {"value", -20.0}, {"shape", "step"}}});
	const ObjectId midi = addTrack("midi");
	const ObjectId other = addTrack("audio");

	// Copy Automation of Selected Tracks (Locator Range): the automation between the locators.
	run(client, "transport.set", {{"left", 0}, {"right", 96000}});
	CHECK(run(client, "automation.copy", {{"tracks", {audio, instrument}}}) == Json{{"lanes", 7}});
	// Pasted onto the selected tracks in order, between the locators: the parameters each track has get it, and it
	// replaces the automation there. A plugin's lane needs the same plugin in the slot at the same place (reference
	// manual p. 878): Gain and pan's goes onto the target's, Gate's not onto Delay. The MIDI track takes the
	// instrument track's controller, but not its volume or its instrument's parameter.
	run(client, "transport.set", {{"left", 192000}, {"right", 288000}});
	const size_t steps = run(client, "history.get").at("steps").size();
	const Json before = run(client, "project.get");
	const Json pasted = run(client, "automation.paste", {{"tracks", {target, midi}}}).at("lanes");
	CHECK(pasted.size() == 4);
	CHECK(run(client, "history.get").at("steps").size() == steps + 1);
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Paste Automation");
	const LanePoints targetVolume = lanePoints(client, target, "volume");
	CHECK(targetVolume.points ==
		Points{{0, -20.0}, {192000, -20.0}, {192000, -6.0}, {240000, 0.0}, {288000, -12.0}, {288000, -20.0}});
	CHECK(lanePoints(client, target, "pan").points == Points{{192000, -1.0}, {288000, 1.0}});
	const Json targetLanes = run(client, "automationLane.list", {{"track", target}}).at("lanes");
	REQUIRE(targetLanes.size() == 3);
	CHECK(targetLanes[2].at("target") == pluginTarget(targetGainPan, 0));
	CHECK(targetLanes[2].at("points")[0].at("value") == -10.0);
	const Json midiLanes = run(client, "automationLane.list", {{"track", midi}}).at("lanes");
	REQUIRE(midiLanes.size() == 1);
	CHECK(midiLanes[0].at("target").at("type") == "controller");
	CHECK(midiLanes[0].at("target").at("number") == 1);
	CHECK(sortedIds(pasted) ==
		sortedIds(Json{
			targetVolume.id, lanePoints(client, target, "pan").id, targetLanes[2].at("id"), midiLanes[0].at("id")}));
	// One undo step.
	run(client, "history.undo");
	CHECK(run(client, "project.get") == before);
	run(client, "history.redo");

	// A shorter range shortens the copy, at the curve's value there; in a longer one its last value holds up to the
	// right locator (reference manual p. 879).
	run(client, "automation.paste", {{"tracks", {other}}, {"start", 0}, {"end", 48000}});
	CHECK(lanePoints(client, other, "volume").points == Points{{0, -6.0}, {48000, 0.0}});
	CHECK(lanePoints(client, other, "pan").points == Points{{0, -1.0}, {48000, 0.0}});
	run(client, "automation.paste", {{"tracks", {other}}, {"start", 400000}, {"end", 600000}});
	CHECK(lanePoints(client, other, "volume").points ==
		Points{{0, -6.0}, {48000, 0.0}, {400000, 0.0}, {400000, -6.0}, {448000, 0.0}, {496000, -12.0}, {600000, -12.0},
			{600000, 0.0}});

	// Copy Automation of Selected Parameters: one parameter onto the selected lanes of the same parameter.
	CHECK(run(client, "automation.copy", {{"lanes", {pan}}, {"start", 0}, {"end", 96000}}) == Json{{"lanes", 1}});
	const ObjectId otherPan = lanePoints(client, other, "pan").id;
	const ObjectId otherVolume = lanePoints(client, other, "volume").id;
	const Json onto =
		run(client, "automation.paste", {{"lanes", {otherPan, otherVolume}}, {"start", 96000}, {"end", 192000}});
	CHECK(onto.at("lanes") == Json{otherPan});
	const Points otherPanPoints = lanePoints(client, other, "pan").points;
	REQUIRE(otherPanPoints.size() >= 6);
	CHECK(Points(otherPanPoints.begin(), otherPanPoints.begin() + 6) ==
		Points{{0, -1.0}, {48000, 0.0}, {96000, 0.0}, {96000, -1.0}, {192000, 1.0}, {192000, 0.0}});
	// One copied track goes onto every selected track; a track without the parameter takes nothing.
	CHECK(run(client, "automation.paste", {{"tracks", {midi, target}}, {"start", 0}, {"end", 96000}}).at("lanes") ==
		Json{lanePoints(client, target, "pan").id});

	// Copy Automation of All Tracks, pasted onto all tracks: each lane onto its own track, here later.
	run(client, "automation.copy", {{"start", 0}, {"end", 96000}});
	run(client, "automation.paste", {{"start", 960000}, {"end", 1056000}});
	const LanePoints source = lanePoints(client, audio, "volume");
	CHECK(source.id == volume);
	CHECK(std::count(source.points.begin(), source.points.end(), std::pair<int64_t, double>{1008000, 0.0}) == 1);

	// Checks.
	Engine fresh;
	Client empty(fresh);
	CHECK_THAT(reject(empty, "automation.paste", Json::object()), ContainsSubstring("nothing to paste"));
	CHECK_THAT(
		reject(client, "automation.paste", {{"tracks", {target}}, {"lanes", {pan}}}), ContainsSubstring("not both"));
	CHECK_THAT(reject(client, "automation.paste", {{"start", 100}, {"end", 100}}), ContainsSubstring("empty"));
	CHECK_THAT(reject(client, "automation.copy", {{"lanes", {12345}}}), ContainsSubstring("no automation lane 12345"));
	CHECK_THAT(reject(client, "automation.paste", {{"tracks", {12345}}}), ContainsSubstring("no track 12345"));
}

TEST_CASE("automation.paste pastes VCA faders' volume, and a new project has nothing to paste", "[unit]")
{
	using Points = std::vector<std::pair<int64_t, double>>;
	Engine engine;
	Client client(engine);
	const auto addVca = [&]
	{
		const ObjectId track = idOf(run(client, "track.add", {{"type", "vca"}}), "track");
		run(client, "track.set", {{"track", track}, {"timeBase", "linear"}}); // points in samples
		return track;
	};
	const ObjectId vca = addVca();
	const ObjectId other = addVca();
	const ObjectId lane =
		idOf(run(client, "automationLane.add", {{"track", vca}, {"target", {{"type", "volume"}}}}), "lane");
	run(client, "automationPoint.add",
		{{"lane", lane},
			{"points",
				{{{"position", 0}, {"value", -6.0}, {"shape", "step"}}, {{"position", 96000}, {"value", -12.0}}}}});

	// Copy Automation of All Tracks, pasted onto all tracks: the VCA fader's volume goes onto its own lane.
	run(client, "automation.copy", {{"start", 0}, {"end", 96000}});
	CHECK(run(client, "automation.paste", {{"start", 192000}, {"end", 288000}}).at("lanes") == Json{lane});
	CHECK(lanePoints(client, vca, "volume").points ==
		Points{{0, -6.0}, {96000, -12.0}, {192000, -12.0}, {192000, -6.0}, {288000, -12.0}, {288000, -12.0}});
	// And onto another VCA track.
	const Json pasted = run(client, "automation.paste", {{"tracks", {other}}, {"start", 0}, {"end", 96000}});
	const LanePoints otherVolume = lanePoints(client, other, "volume");
	CHECK(pasted.at("lanes") == Json{otherVolume.id});
	CHECK(otherVolume.points == Points{{0, -6.0}, {96000, -12.0}});

	// The clipboard holds this project's track ids, which a new project hands out again.
	run(client, "automation.copy", {{"start", 0}, {"end", 96000}});
	run(client, "project.new");
	addVca();
	CHECK_THAT(reject(client, "automation.paste", Json::object()), ContainsSubstring("nothing to paste"));
}

TEST_CASE("pre gain automation is sample-accurate, and the static gain plays again with reading off", "[integration]")
{
	const int blockSize = GENERATE(64, 100);
	INFO("block size " << blockSize);
	Mixer mixer(blockSize);
	run(mixer.client, "channel.set", {{"channel", mixer.channel}, {"pre", {{"gain", 3.0}}}});
	const Json target = {{"type", "strip"}, {"parameter", 0}};
	const ObjectId lane = mixer.lane(target);
	// Like the fader's volume, the pre gain glides along ramps in a straight line of its linear gain.
	const std::vector<TestPoint> points = {
		{1000, -12.0, "step"}, {2000, -12.0, "linear"}, {3000, 6.0, "step"}, {3500, -6.0, "step"}};
	mixer.add(lane, points);
	run(mixer.client, "transport.play");
	run(mixer.client, "audio.render", {{"frames", 4000}});
	run(mixer.client, "automationLane.set", {{"lane", lane}, {"read", false}});
	run(mixer.client, "audio.render", {{"frames", 500}});

	Mixer reference(64);
	run(reference.client, "audio.render", {{"frames", 4500}});
	const std::vector<float> input = reference.capture(0, 4500);
	size_t wrong = 0;
	size_t first = 0;
	for (int channel = 0; channel < 2; ++channel)
	{
		const std::vector<float> output = mixer.capture(channel, 4500);
		for (size_t i = 0; i < output.size(); ++i)
		{
			const auto position = static_cast<int64_t>(i);
			const double expected = input[i] * (position < 4000 ? expectedGain(points, position) : gain(3.0));
			if (std::abs(output[i] - expected) > 1e-6 && wrong++ == 0)
				first = i;
		}
	}
	INFO(wrong << " wrong samples, the first at frame " << first);
	CHECK(wrong == 0);
}

TEST_CASE("a section switch's automation is a step curve that switches at its point's sample", "[integration]")
{
	const int blockSize = GENERATE(64, 100);
	INFO("block size " << blockSize);
	Mixer mixer(blockSize);
	const ObjectId lane = mixer.lane({{"type", "strip"}, {"parameter", 1}}); // the phase
	const Json points = run(mixer.client, "automationPoint.add",
		{{"lane", lane},
			{"points",
				{{{"position", 0}, {"value", 0.0}, {"shape", "linear"}}, {{"position", ticks(1000)}, {"value", 0.6}}}}})
							.at("points");
	for (const Json& point : points)
		CHECK(point.at("shape") == "step");
	CHECK(points.at(1).at("value") == 1.0);
	run(mixer.client, "transport.play");
	run(mixer.client, "audio.render", {{"frames", 40000}});

	// Up to the point the audio passes unchanged; from its sample on the polarity glides through zero to inverted.
	Mixer reference(64);
	run(reference.client, "audio.render", {{"frames", 40000}});
	const std::vector<float> input = reference.capture(0, 40000);
	const std::vector<float> output = mixer.capture(0, 40000);
	CHECK(std::equal(input.begin(), input.begin() + 1000, output.begin()));
	CHECK(output[1000] != input[1000]);
	size_t wrong = 0;
	for (size_t i = 30000; i < output.size(); ++i)
		wrong += output[i] == -input[i] ? 0 : 1;
	CHECK(wrong == 0);
}

TEST_CASE("section parameters' automation lanes have their ranges and are saved with the project", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId track = idOf(run(client, "track.add", {{"type", "audio"}}), "track");
	const ObjectId midi = idOf(run(client, "track.add", {{"type", "midi"}}), "track");
	const Json frequency = {{"type", "strip"}, {"parameter", 33}}; // EQ band 3's frequency
	const ObjectId lane = idOf(run(client, "automationLane.add", {{"track", track}, {"target", frequency}}), "lane");
	CHECK_THAT(reject(client, "automationPoint.add", {{"lane", lane}, {"points", {{{"position", 0}, {"value", 5.0}}}}}),
		ContainsSubstring("from 20 to 20000"));
	run(client, "automationPoint.add",
		{{"lane", lane}, {"points", {{{"position", 0}, {"value", 200.0}}, {{"position", 960}, {"value", 4000.0}}}}});
	// The low cut's slope is a stepped choice, with a step curve.
	const ObjectId slope =
		idOf(run(client, "automationLane.add", {{"track", track}, {"target", {{"type", "strip"}, {"parameter", 4}}}}),
			"lane");
	CHECK(run(client, "automationPoint.add", {{"lane", slope}, {"points", {{{"position", 0}, {"value", 3.4}}}}})
			  .at("points")
			  .at(0)
			  .at("value") == 3.0);
	CHECK_THAT(reject(client, "automationLane.add",
				   {{"track", track}, {"target", {{"type", "strip"}, {"parameter", 22}}}}), // band 2 is a bell
		ContainsSubstring("no parameter 22"));
	CHECK_THAT(reject(client, "automationLane.add", {{"track", track}, {"target", {{"type", "strip"}, {"slot", 1}}}}),
		ContainsSubstring("target.slot"));
	CHECK_THAT(
		reject(client, "automationLane.add", {{"track", midi}, {"target", frequency}}), ContainsSubstring("MIDI"));

	// Saved in GNUbase's entry of the project file and loaded again.
	const gnubase::test::TempDir dir;
	const std::string path = (dir.path() / "strip.dawproject").u8string();
	run(client, "project.save", {{"path", path}});
	run(client, "project.new");
	run(client, "project.load", {{"path", path}});
	const ObjectId opened = run(client, "project.get").at("tracks").at(0).at("id").get<ObjectId>();
	const Json lanes = run(client, "automationLane.list", {{"track", opened}}).at("lanes");
	REQUIRE(lanes.size() == 2);
	CHECK(lanes.at(0).at("target") == frequency);
	CHECK(lanes.at(0).at("points").size() == 2);
	CHECK(lanes.at(0).at("points").at(1).at("value") == 4000.0);
	CHECK(lanes.at(1).at("target") == Json{{"type", "strip"}, {"parameter", 4}});
	CHECK(lanes.at(1).at("points").at(0).at("shape") == "step");

	// A copy of the track has them too.
	const ObjectId copy = idOf(run(client, "track.duplicate", {{"track", opened}}), "track");
	CHECK(run(client, "automationLane.list", {{"track", copy}}).at("lanes").size() == 2);
}

TEST_CASE("a lane of the limiter's switch plays it bypassed with its look-ahead, and limits once it turns it on",
	"[integration]")
{
	const int blockSize = GENERATE(64, 100);
	INFO("block size " << blockSize);
	Mixer mixer(blockSize);
	// The sine's peaks (0.5) are above the threshold (0.25); the limiter is off, so only its lane switches it.
	const double threshold = gain(-12.0);
	run(mixer.client, "channel.set",
		{{"channel", mixer.channel},
			{"strip", {{"limiter", {{"on", false}, {"threshold", -12.0}, {"release", 1.0}}}}}});
	const ObjectId lane = mixer.lane({{"type", "strip"}, {"parameter", 70}});
	mixer.add(lane, {{0, 0.0, "step"}, {2000, 1.0, "step"}, {6000, 0.0, "step"}});
	run(mixer.client, "transport.play");
	run(mixer.client, "audio.render", {{"frames", 12000}});

	Mixer reference(64);
	run(reference.client, "audio.render", {{"frames", 12000}});
	const std::vector<float> input = reference.capture(0, 12000);
	const size_t lookAhead = 48; // 1 ms at 48 kHz, the whole time
	for (int channel = 0; channel < 2; ++channel)
	{
		const std::vector<float> output = mixer.capture(channel, 12000);
		size_t wrong = 0;
		float before = 0.0F;
		float limited = 0.0F;
		for (size_t i = 0; i < output.size(); ++i)
		{
			const float delayed = i < lookAhead ? 0.0F : input[i - lookAhead];
			if (i < 2000)
			{
				// Off: the input delayed by the look-ahead, unclamped.
				wrong += output[i] == delayed ? 0 : 1;
				before = std::max(before, std::abs(output[i]));
			}
			else if (i < 6000)
			{
				wrong += std::abs(output[i]) <= threshold * (1.0 + 1e-6) ? 0 : 1;
				limited = std::max(limited, std::abs(output[i]));
			}
			else if (i >= 8000) // off again, once its gain has released: the same delay as before
			{
				wrong += std::abs(output[i] - delayed) <= 1e-5 ? 0 : 1;
			}
		}
		INFO("channel " << channel);
		CHECK(wrong == 0);
		CHECK(before > 0.49F);
		CHECK(limited > 0.9 * threshold);
	}
}

TEST_CASE("switching between the mute button and mute automation lets no audio through", "[integration]")
{
	const int blockSize = GENERATE(64, 512);
	INFO("block size " << blockSize);
	Mixer mixer(blockSize);
	run(mixer.client, "channel.set", {{"channel", mixer.channel}, {"mute", true}});
	run(mixer.client, "transport.play");
	run(mixer.client, "audio.render", {{"frames", 2048}});
	const ObjectId lane = mixer.lane({{"type", "mute"}});
	run(mixer.client, "audio.render", {{"frames", 2048}});
	// The lane's first point takes over from the button, and without points the button mutes again.
	const Json point =
		run(mixer.client, "automationPoint.add", {{"lane", lane}, {"points", {{{"position", 0}, {"value", 1.0}}}}})
			.at("points")
			.at(0);
	run(mixer.client, "audio.render", {{"frames", 2048}});
	run(mixer.client, "automationPoint.remove", {{"lane", lane}, {"points", {point.at("id")}}});
	run(mixer.client, "audio.render", {{"frames", 2048}});
	// Reading off and on again.
	run(mixer.client, "automationPoint.add", {{"lane", lane}, {"points", {{{"position", 0}, {"value", 1.0}}}}});
	run(mixer.client, "audio.render", {{"frames", 2048}});
	run(mixer.client, "automationLane.set", {{"lane", lane}, {"read", false}});
	run(mixer.client, "audio.render", {{"frames", 2048}});
	// Unmuted, it is heard: the input reaches the channel.
	run(mixer.client, "channel.set", {{"channel", mixer.channel}, {"mute", false}});
	run(mixer.client, "audio.render", {{"frames", 2048}});
	for (int channel = 0; channel < 2; ++channel)
	{
		INFO("channel " << channel);
		const std::vector<float> output = mixer.capture(channel, int64_t{7} * 2048);
		REQUIRE(output.size() == size_t{7} * 2048);
		CHECK(loudest(output, 0, size_t{6} * 2048) == 0.0F);
		CHECK(loudest(output, size_t{6} * 2048, output.size()) > 0.3F);
	}
}

TEST_CASE("switching between the mute button and mute automation on a channel another's solo silences lets no audio "
		  "through",
	"[integration]")
{
	const int blockSize = GENERATE(64, 512);
	INFO("block size " << blockSize);
	Mixer mixer(blockSize);
	// Another track, which doesn't monitor and so plays nothing, is soloed.
	const ObjectId soloist =
		channelOf(mixer.client, idOf(run(mixer.client, "track.add", {{"type", "audio"}}), "track"));
	run(mixer.client, "channel.set", {{"channel", soloist}, {"solo", true}});
	run(mixer.client, "transport.play");
	run(mixer.client, "audio.render", {{"frames", 2048}});
	// Muting and unmuting.
	run(mixer.client, "channel.set", {{"channel", mixer.channel}, {"mute", true}});
	run(mixer.client, "audio.render", {{"frames", 2048}});
	run(mixer.client, "channel.set", {{"channel", mixer.channel}, {"mute", false}});
	run(mixer.client, "audio.render", {{"frames", 2048}});
	// Adding and removing a mute lane's first point while the button is on.
	run(mixer.client, "channel.set", {{"channel", mixer.channel}, {"mute", true}});
	run(mixer.client, "audio.render", {{"frames", 2048}});
	const ObjectId lane = mixer.lane({{"type", "mute"}});
	const Json point =
		run(mixer.client, "automationPoint.add", {{"lane", lane}, {"points", {{{"position", 0}, {"value", 0.0}}}}})
			.at("points")
			.at(0);
	run(mixer.client, "audio.render", {{"frames", 2048}});
	run(mixer.client, "automationPoint.remove", {{"lane", lane}, {"points", {point.at("id")}}});
	run(mixer.client, "audio.render", {{"frames", 2048}});
	// Neither muted nor soloed out, it is heard: the input reaches the channel.
	run(mixer.client, "channel.set", {{"channel", mixer.channel}, {"mute", false}});
	run(mixer.client, "channel.set", {{"channel", soloist}, {"solo", false}});
	run(mixer.client, "audio.render", {{"frames", 2048}});
	for (int channel = 0; channel < 2; ++channel)
	{
		INFO("channel " << channel);
		const std::vector<float> output = mixer.capture(channel, int64_t{7} * 2048);
		REQUIRE(output.size() == size_t{7} * 2048);
		CHECK(loudest(output, 0, size_t{6} * 2048) == 0.0F);
		CHECK(loudest(output, size_t{6} * 2048, output.size()) > 0.3F);
	}
}

TEST_CASE("points of mute and stepped parameters' lanes load from files as steps with whole values", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId audio = idOf(run(client, "track.add", {{"type", "audio"}}), "track");
	const ObjectId midi = idOf(run(client, "track.add", {{"type", "midi"}}), "track");
	const Json delay = insertPlugin(client, channelOf(client, audio), "builtin:delay");
	for (const auto& [track, target] : {std::pair{audio, Json{{"type", "mute"}}},
			 std::pair{midi, Json{{"type", "mute"}}}, std::pair{audio, pluginTarget(delay, 1)}, // Sync, a switch
			 std::pair{audio, Json{{"type", "strip"}, {"parameter", 4}}}, // the low cut's slope, a stepped choice
			 std::pair{audio, Json{{"type", "volume"}}}})
		run(client, "automationPoint.add",
			{{"lane", idOf(run(client, "automationLane.add", {{"track", track}, {"target", target}}), "lane")},
				{"points", {{{"position", 0}, {"value", 0.0}}, {{"position", 960}, {"value", 0.0}}}}});

	// A file with ramps and fractions there, as from a build before #621 or a changed file.
	Json file = run(client, "project.get");
	REQUIRE(file.at("automation").size() == 5);
	for (Json& lane : file.at("automation"))
		for (Json& point : lane.at("points"))
		{
			point["shape"] = "curve";
			point["curve"] = 0.5;
			point["value"] = lane.at("target").at("type") == "strip" ? 2.6 : 0.6;
		}
	const Project loaded = projectFromJson(file);
	size_t stepped = 0;
	for (const auto& [id, lane] : loaded.automation)
		for (const auto& [pointId, point] : lane.points)
		{
			INFO("lane " << id << " of type " << automationTargetTypeName(lane.target.type));
			if (lane.target.type == AutomationTargetType::Volume)
			{
				CHECK(point.shape == AutomationShape::Curve); // a ramp curve keeps its points
				CHECK(point.value == 0.6);
				continue;
			}
			CHECK(point.shape == AutomationShape::Step);
			CHECK(point.curve == 0.0);
			CHECK(point.value == (lane.target.type == AutomationTargetType::Strip ? 3.0 : 1.0));
			++stepped;
		}
	CHECK(stepped == 8);
}

TEST_CASE("a built-in plugin with a state keeps its state's values for parameters not set, across jumps", "[unit]")
{
	RenderChannel channel;
	RenderSlot slot;
	slot.id = 5;
	slot.plugin = "builtin:gainPan";
	slot.parameters[1] = 0.5; // the pan, set on top of the state
	std::vector<uint8_t> saved;
	for (const auto& [id, value] : {std::pair{uint32_t{0}, -12.0}, std::pair{uint32_t{1}, -1.0}})
	{
		const size_t at = saved.size();
		saved.resize(at + sizeof(id) + sizeof(value));
		std::memcpy(saved.data() + at, &id, sizeof(id));
		std::memcpy(saved.data() + at + sizeof(id), &value, sizeof(value));
	}
	slot.state = std::make_shared<const std::vector<uint8_t>>(saved);
	channel.slots.push_back(slot);
	// The pan's automation reads, the gain has none.
	RenderAutomationLane lane;
	lane.target = {AutomationTargetType::Plugin, 1, 0, 0, 5};
	lane.points = {{0, 0.0, AutomationShape::Step, 0.0}, {1000, 1.0, AutomationShape::Step, 0.0}};
	channel.automation.push_back(lane);

	std::unique_ptr<ChannelAutomation> automation = channelAutomation(channel, 6.0);
	const auto staticValue = [&automation](uint32_t parameter)
	{
		double value = -1000.0; // none
		for (const PlayedLane& played : automation->lanes)
			if (played.target == AutomationTarget{AutomationTargetType::Plugin, parameter, 0, 0, 5})
				value = played.staticValue;
		return value;
	};
	CHECK(staticValue(0) == -12.0); // the state's
	CHECK(staticValue(1) == 0.5);   // the value set
	auto state = std::make_shared<ChannelState>();
	state->automation.setAutomation(std::move(automation));

	auto plugin = std::make_shared<ParameterRecorder>();
	ChannelStrip source(state, nullptr, {}, std::nullopt, false, StripPart::Source, {}, {plugin.get()}, {5});
	SlotStrip strip(state, plugin, 0);
	source.prepareFor(48000.0, 64);
	strip.prepareFor(48000.0, 64);
	const graph::EventBuffer in(graph::g_maxEventsPerBlock);
	graph::EventBuffer out(graph::g_maxEventsPerBlock);
	const auto block = [&](const graph::TransportBlock& transport)
	{
		source.process({nullptr, 0, 64, in, out, transport});
		strip.process({nullptr, 0, 64, in, out, transport});
	};
	// Stopped, playing, a locate, a cycle jump in a block, stopped at another position.
	block({false, 0, nullptr, 0});
	const graph::TransportSegment start{0, 64, 0};
	block({true, 0, &start, 1});
	const graph::TransportSegment next{0, 64, 64};
	block({true, 64, &next, 1});
	const graph::TransportSegment located{0, 64, 5000};
	block({true, 5000, &located, 1});
	const std::array<graph::TransportSegment, 2> cycle{{{0, 32, 1968}, {32, 32, 0}}};
	block({true, 1968, cycle.data(), 2});
	block({false, 3000, nullptr, 0});
	REQUIRE(!plugin->values.empty());
	for (const double value : plugin->values)
		CHECK(value == -12.0);
}

namespace
{
	constexpr uint32_t g_midiBlock = 256;

	// A MIDI track's strip, playing from 0 in blocks of g_midiBlock frames, whose mute lane has steps to 1 (muted) or 0
	// at the samples given, with the other lanes given and the track's sequencer as source, if any.
	class MidiStrip
	{
	public:
		explicit MidiStrip(const std::vector<std::pair<int64_t, double>>& steps,
			const std::vector<RenderAutomationLane>& lanes = {}, std::shared_ptr<graph::Processor> source = nullptr)
			: m_strip(m_state, std::move(source))
		{
			RenderChannel channel;
			channel.type = TrackType::Midi;
			channel.format = ChannelFormat::None;
			RenderAutomationLane lane;
			lane.target = {AutomationTargetType::Mute, 0, 0, 0, 0};
			for (const auto& [position, value] : steps)
				lane.points.push_back({position, value, AutomationShape::Step, 0.0});
			channel.automation.push_back(lane);
			channel.automation.insert(channel.automation.end(), lanes.begin(), lanes.end());
			m_state->automation.setAutomation(channelAutomation(channel, 6.0));
			m_strip.prepareFor(48000.0, g_midiBlock);
		}

		// Plays the channel's automation from the next block on.
		void setChannel(const RenderChannel& channel)
		{
			m_state->automation.setAutomation(channelAutomation(channel, 6.0));
		}

		// The next block plays from position, a jump.
		void locate(int64_t position) { m_position = position; }

		ChannelStrip& strip() { return m_strip; }

		// Plays the next block with events; returns what the strip lets through.
		const graph::EventBuffer& play(const std::vector<graph::ProcessEvent>& events)
		{
			graph::EventBuffer in(graph::g_maxEventsPerBlock);
			for (const graph::ProcessEvent& event : events)
				in.add(event);
			m_out.clear();
			const graph::TransportSegment segment{0, g_midiBlock, m_position};
			m_strip.process({nullptr, 0, g_midiBlock, in, m_out, {true, m_position, &segment, 1}});
			m_position += g_midiBlock;
			return m_out;
		}

	private:
		std::shared_ptr<ChannelState> m_state = std::make_shared<ChannelState>();
		ChannelStrip m_strip;
		graph::EventBuffer m_out{graph::g_maxEventsPerBlock};
		int64_t m_position = 0;
	};

	graph::ProcessEvent midiEvent(
		graph::ProcessEvent::Type type, uint32_t offset, uint8_t channel, uint8_t key, uint32_t value = 0);

	// A controller's value in dropControllers.
	uint32_t droppedValue(size_t channel, size_t number)
	{
		return static_cast<uint32_t>((channel * 1000) + number);
	}

	// Plays two muted blocks: 1000 controllers on MIDI channels 0 to 7, then 100 on channel 8 and channel 0's first
	// again, now 7.
	void dropControllers(MidiStrip& strip)
	{
		using Type = graph::ProcessEvent::Type;
		std::vector<graph::ProcessEvent> events;
		for (size_t channel = 0; channel < 8; ++channel)
			for (size_t number = 0; number < 125; ++number)
				events.push_back(midiEvent(Type::Controller, 0, static_cast<uint8_t>(channel),
					static_cast<uint8_t>(number), droppedValue(channel, number)));
		CHECK(strip.play(events).empty());
		events.clear();
		for (size_t number = 0; number < 100; ++number)
			events.push_back(midiEvent(Type::Controller, 0, 8, static_cast<uint8_t>(number), droppedValue(8, number)));
		events.push_back(midiEvent(Type::Controller, 1, 0, 0, 7));
		CHECK(strip.play(events).empty());
	}

	// Checks that sent has dropControllers' controllers, without channel 8's controller 99 unless withLast, in the
	// order of their latest events, with their latest values: channel 0's first one last.
	void checkDropped(const std::vector<graph::ProcessEvent>& sent, bool withLast)
	{
		std::vector<std::pair<size_t, size_t>> expected; // channel and number
		for (size_t channel = 0; channel < 8; ++channel)
			for (size_t number = channel == 0 ? 1 : 0; number < 125; ++number)
				expected.emplace_back(channel, number);
		for (size_t number = 0; number < (withLast ? 100 : 99); ++number)
			expected.emplace_back(8, number);
		expected.emplace_back(0, 0);
		REQUIRE(sent.size() == expected.size());
		for (size_t i = 0; i < sent.size(); ++i)
		{
			const auto [channel, number] = expected[i];
			INFO("event " << i);
			CHECK(sent[i].type == graph::ProcessEvent::Type::Controller);
			CHECK(sent[i].channel == channel);
			CHECK(sent[i].key == number);
			CHECK(sent[i].value == (i + 1 == sent.size() ? 7 : droppedValue(channel, number)));
		}
	}

	graph::ProcessEvent midiEvent(
		graph::ProcessEvent::Type type, uint32_t offset, uint8_t channel, uint8_t key, uint32_t value)
	{
		graph::ProcessEvent event;
		event.type = type;
		event.offset = offset;
		event.channel = channel;
		event.key = key;
		event.value = value;
		event.velocity = type == graph::ProcessEvent::Type::NoteOn ? 40000 : 0;
		return event;
	}
}

TEST_CASE("a MIDI track's mute ends its notes and releases its pedals also in a full event buffer", "[unit]")
{
	using Type = graph::ProcessEvent::Type;
	MidiStrip strip({{0, 0.0}, {100, 1.0}});
	std::vector<graph::ProcessEvent> events{
		midiEvent(Type::NoteOn, 10, 3, 60), midiEvent(Type::Controller, 20, 3, 64, 0xFFFFFFFFU)};
	while (events.size() < graph::g_maxEventsPerBlock)
		events.push_back(midiEvent(Type::Controller, 50, 0, 1, static_cast<uint32_t>(events.size())));
	const graph::EventBuffer& out = strip.play(events);
	REQUIRE(out.size() == graph::g_maxEventsPerBlock);
	// They take the places of the last controllers.
	CHECK(out[out.size() - 3].type == Type::Controller);
	CHECK(out[out.size() - 3].key == 1);
	const graph::ProcessEvent& end = out[out.size() - 2];
	CHECK(end.type == Type::NoteOff);
	CHECK(end.offset == 100);
	CHECK(end.channel == 3);
	CHECK(end.key == 60);
	const graph::ProcessEvent& up = out[out.size() - 1];
	CHECK(up.type == Type::Controller);
	CHECK(up.offset == 100);
	CHECK(up.channel == 3);
	CHECK(up.key == 64);
	CHECK(up.value == 0);
}

TEST_CASE(
	"unmuting a MIDI track sends what its mute dropped in the order of the latest events, what doesn't fit in the "
	"next block",
	"[unit]")
{
	using Type = graph::ProcessEvent::Type;
	// Muted from the start to 10 frames into the third block.
	MidiStrip strip({{0, 1.0}, {(2 * g_midiBlock) + 10, 0.0}});
	dropControllers(strip);

	// At the unmute's frame, in the order of their latest events, what fits with the block's controller after it;
	// the rest at the start of the next block, but channel 8's controller 99, which that controller replaced.
	std::vector<graph::ProcessEvent> sent;
	const graph::EventBuffer& unmuted = strip.play({midiEvent(Type::Controller, 20, 8, 99, 5)});
	REQUIRE(unmuted.size() == graph::g_maxEventsPerBlock);
	for (size_t i = 0; i + 1 < unmuted.size(); ++i)
	{
		CHECK(unmuted[i].offset == 10);
		sent.push_back(unmuted[i]);
	}
	const graph::ProcessEvent& replacing = unmuted[unmuted.size() - 1];
	CHECK(replacing.offset == 20);
	CHECK(replacing.key == 99);
	CHECK(replacing.value == 5);
	for (const graph::ProcessEvent& event : strip.play({}))
	{
		CHECK(event.offset == 0);
		sent.push_back(event);
	}
	checkDropped(sent, false);
	CHECK(strip.play({}).empty());
}

TEST_CASE(
	"a MIDI track muted again before it sent all that its mute dropped sends the rest once when it unmutes", "[unit]")
{
	// Muted from the start to 10 frames into the third block, again from 200 frames into it to the fifth block.
	MidiStrip strip({{0, 1.0}, {(2 * g_midiBlock) + 10, 0.0}, {(2 * g_midiBlock) + 200, 1.0}, {4 * g_midiBlock, 0.0}});
	dropControllers(strip);
	std::vector<graph::ProcessEvent> sent;
	for (const graph::ProcessEvent& event : strip.play({}))
	{
		CHECK(event.offset == 10);
		sent.push_back(event);
	}
	CHECK(sent.size() == graph::g_maxEventsPerBlock);
	CHECK(strip.play({}).empty());
	for (const graph::ProcessEvent& event : strip.play({}))
	{
		CHECK(event.offset == 0);
		sent.push_back(event);
	}
	checkDropped(sent, true);
	CHECK(strip.play({}).empty());
}

namespace
{
	// The type, offset, MIDI channel, key and value of each event in events that isn't a note.
	using MidiValues = std::vector<std::array<uint32_t, 5>>;

	MidiValues midiValuesOf(const graph::EventBuffer& events)
	{
		MidiValues values;
		for (const graph::ProcessEvent& event : events)
			if (event.type != graph::ProcessEvent::Type::NoteOn && event.type != graph::ProcessEvent::Type::NoteOff)
				values.push_back(
					{static_cast<uint32_t>(event.type), event.offset, event.channel, event.key, event.value});
		return values;
	}

	constexpr auto g_controller = static_cast<uint32_t>(graph::ProcessEvent::Type::Controller);
	constexpr auto g_program = static_cast<uint32_t>(graph::ProcessEvent::Type::Program);
}

TEST_CASE("a MIDI track's mute releases a pedal at any value, and unmuting sets it to that value again", "[unit]")
{
	using Type = graph::ProcessEvent::Type;
	// Muted from 100 to 50 frames into the second block.
	MidiStrip strip({{0, 0.0}, {100, 1.0}, {g_midiBlock + 50, 0.0}});
	constexpr uint32_t half = 0x40000000U; // a continuous sustain pedal's half pedal
	CHECK(midiValuesOf(strip.play(
			  {midiEvent(Type::Controller, 10, 2, 64, half), midiEvent(Type::Controller, 20, 2, 66, 0xFFFFFFFFU)})) ==
		MidiValues{{g_controller, 10, 2, 64, half}, {g_controller, 20, 2, 66, 0xFFFFFFFFU},
			{g_controller, 100, 2, 64, 0}, {g_controller, 100, 2, 66, 0}});
	CHECK(midiValuesOf(strip.play({})) ==
		MidiValues{{g_controller, 50, 2, 64, half}, {g_controller, 50, 2, 66, 0xFFFFFFFFU}});
}

TEST_CASE("unmuting a MIDI track sends a bank select before a program change that came after it, also if a program "
		  "change came before it",
	"[unit]")
{
	using Type = graph::ProcessEvent::Type;
	MidiStrip strip({{0, 1.0}, {g_midiBlock + 10, 0.0}});
	CHECK(strip
			.play({midiEvent(Type::Program, 10, 0, 5, 0xFFFFFFFFU), midiEvent(Type::Controller, 20, 0, 0, 3),
				midiEvent(Type::Program, 30, 0, 6, 0xFFFFFFFFU)})
			.empty());
	CHECK(midiValuesOf(strip.play({})) == MidiValues{{g_controller, 10, 0, 0, 3}, {g_program, 10, 0, 6, 0xFFFFFFFFU}});
}

TEST_CASE("unmuting a MIDI track by its button after a move sends the channel's values before what its mute dropped",
	"[unit]")
{
	using Type = graph::ProcessEvent::Type;
	RenderChannel channel;
	channel.type = TrackType::Midi;
	channel.format = ChannelFormat::None;
	channel.program = 3;
	channel.midiVolume = 100;
	MidiStrip strip({});
	strip.setChannel(channel);
	CHECK(!strip.play({}).empty()); // the channel's values
	channel.muted = true;
	channel.silent = true;
	strip.setChannel(channel);
	CHECK(strip.play({}).empty());
	// After a move while muted, the part's program change and MIDI volume play into the mute.
	strip.locate(10000);
	CHECK(
		strip.play({midiEvent(Type::Program, 5, 0, 9, 0xFFFFFFFFU), midiEvent(Type::Controller, 6, 0, 7, 20)}).empty());
	channel.muted = false;
	channel.silent = false;
	strip.setChannel(channel);
	const MidiValues unmuted = midiValuesOf(strip.play({}));
	// The channel's program and MIDI volume go out again, then what the part left, which wins.
	REQUIRE(unmuted.size() > 2);
	CHECK(unmuted[unmuted.size() - 2] == std::array<uint32_t, 5>{g_program, 0, 0, 9, 0xFFFFFFFFU});
	CHECK(unmuted.back() == std::array<uint32_t, 5>{g_controller, 0, 0, 7, 20});
	const auto channelValue = [&unmuted](uint32_t type, uint32_t key)
	{
		return std::any_of(unmuted.begin(), unmuted.end() - 2,
			[type, key](const std::array<uint32_t, 5>& value) { return value[0] == type && value[3] == key; });
	};
	CHECK(channelValue(g_program, 3));
	CHECK(channelValue(g_controller, 7));
}

TEST_CASE(
	"a value that a MIDI track's mute takes the place of in a full event buffer goes out when it unmutes", "[unit]")
{
	using Type = graph::ProcessEvent::Type;
	// Muted from the start to 10 frames into the second block, again from 100 frames into it to the fourth block.
	MidiStrip strip({{0, 1.0}, {g_midiBlock + 10, 0.0}, {g_midiBlock + 100, 1.0}, {3 * g_midiBlock, 0.0}});
	CHECK(strip.play({midiEvent(Type::Controller, 5, 0, 1, 77)}).empty());
	// Unmuting sends CC 1, and the block's events fill the buffer; the mute's note-off takes the last CC 2's place.
	std::vector<graph::ProcessEvent> events{midiEvent(Type::NoteOn, 20, 3, 60)};
	while (events.size() + 1 < graph::g_maxEventsPerBlock)
		events.push_back(midiEvent(Type::Controller, 50, 0, 2, static_cast<uint32_t>(events.size())));
	const uint32_t last = events.back().value;
	const graph::EventBuffer& full = strip.play(events);
	REQUIRE(full.size() == graph::g_maxEventsPerBlock);
	CHECK(full[0].type == Type::Controller);
	CHECK(full[0].key == 1);
	CHECK(full[0].value == 77);
	CHECK(full[full.size() - 1].type == Type::NoteOff);
	CHECK(full[full.size() - 2].value == last - 1);
	CHECK(strip.play({}).empty());
	CHECK(midiValuesOf(strip.play({})) == MidiValues{{g_controller, 0, 0, 2, last}});
}

TEST_CASE("values whose places a MIDI track's mute ends take in a full event buffer wait in their order, the latest "
		  "of a kind winning",
	"[unit]")
{
	using Type = graph::ProcessEvent::Type;
	// Muted from 150 to 10 frames into the second block. Four notes sound at the mute, whose note-offs take the
	// places of the block's last four values: CC 1 at 10, a bank select, a program change, CC 1 at 90.
	MidiStrip strip({{0, 0.0}, {150, 1.0}, {g_midiBlock + 10, 0.0}});
	std::vector<graph::ProcessEvent> events;
	for (uint8_t pitch = 60; pitch < 64; ++pitch)
		events.push_back(midiEvent(Type::NoteOn, 5, 3, pitch));
	while (events.size() + 4 < graph::g_maxEventsPerBlock)
		events.push_back(midiEvent(Type::Controller, 50, 0, 2, 1));
	events.insert(events.end(),
		{midiEvent(Type::Controller, 100, 0, 1, 10), midiEvent(Type::Controller, 105, 0, 0, 3),
			midiEvent(Type::Program, 110, 0, 6, 0xFFFFFFFFU), midiEvent(Type::Controller, 120, 0, 1, 90)});
	const graph::EventBuffer& full = strip.play(events);
	REQUIRE(full.size() == graph::g_maxEventsPerBlock);
	for (size_t i = full.size() - 4; i < full.size(); ++i)
		CHECK(full[i].type == Type::NoteOff);
	CHECK(midiValuesOf(strip.play({})) ==
		MidiValues{{g_controller, 10, 0, 0, 3}, {g_program, 10, 0, 6, 0xFFFFFFFFU}, {g_controller, 10, 0, 1, 90}});
}

TEST_CASE("a MIDI track of another project on the channel doesn't get what the old one's mute kept", "[unit]")
{
	using Type = graph::ProcessEvent::Type;
	// Muted from 100 to 50 frames into the second block; the pedal goes up at the mute, CC 1 plays into it.
	MidiStrip strip({{0, 0.0}, {100, 1.0}, {g_midiBlock + 50, 0.0}});
	strip.strip().playsProject(1);
	CHECK(midiValuesOf(strip.play(
			  {midiEvent(Type::Controller, 10, 0, 64, 0xFFFFFFFFU), midiEvent(Type::Controller, 150, 0, 1, 5)})) ==
		MidiValues{{g_controller, 10, 0, 64, 0xFFFFFFFFU}, {g_controller, 100, 0, 64, 0}});
	SECTION("the same project")
	{
		CHECK(midiValuesOf(strip.play({})) ==
			MidiValues{{g_controller, 50, 0, 64, 0xFFFFFFFFU}, {g_controller, 50, 0, 1, 5}});
	}
	SECTION("another project")
	{
		strip.strip().playsProject(2);
		CHECK(strip.play({}).empty());
	}
}

TEST_CASE("a MIDI track heard again after a move forgets what its mute kept of the kinds chased for it before it sends "
		  "the rest",
	"[unit]")
{
	using Type = graph::ProcessEvent::Type;
	RenderChannel channel;
	channel.type = TrackType::Midi;
	channel.format = ChannelFormat::None;
	channel.midiVolume = 100;
	MidiStrip strip({});
	auto chaser = std::make_shared<MidiSequencer>();
	MidiChase chase;
	chase.controllers = true;
	chaser->setChase(chase);
	std::vector<graph::ProcessEvent> kinds;
	for (const uint16_t kind : channelChaseKinds(channel))
		kinds.push_back(chaseKindEvent(kind));
	strip.strip().chasedWhenHeard(std::move(kinds), chaser);
	auto chased = std::make_shared<graph::EventBuffer>(graph::g_maxEventsPerBlock);
	strip.strip().chaseFrom(chased, true);
	strip.setChannel(channel);
	CHECK(!strip.play({}).empty());
	channel.muted = true;
	channel.silent = true;
	strip.setChannel(channel);
	CHECK(strip.play({}).empty());
	const bool moved = GENERATE(false, true);
	CAPTURE(moved);
	if (moved)
		strip.locate(10000);
	// Into the mute: CC 7, which the channel sends, CC 1, which the track's own chase has, and CC 2.
	CHECK(strip
			.play({midiEvent(Type::Controller, 5, 0, 7, 1), midiEvent(Type::Controller, 6, 0, 1, 2),
				midiEvent(Type::Controller, 7, 0, 2, 3)})
			.empty());
	channel.muted = false;
	channel.silent = false;
	strip.setChannel(channel);
	if (moved)
		chased->add(midiEvent(Type::Controller, 0, 0, 1, 9)); // where the track is heard again
	const MidiValues unmuted = midiValuesOf(strip.play({}));
	chased->clear();
	if (!moved)
	{
		// Not heard again: everything it kept, and nothing of the channel's.
		CHECK(
			unmuted == MidiValues{{g_controller, 0, 0, 7, 1}, {g_controller, 0, 0, 1, 2}, {g_controller, 0, 0, 2, 3}});
		return;
	}
	// The channel's MIDI volume, then CC 2; the chase sends CC 7 and CC 1 to the instrument, not the strip.
	REQUIRE(!unmuted.empty());
	CHECK(unmuted.back() == std::array<uint32_t, 5>{g_controller, 0, 0, 2, 3});
	CHECK(std::none_of(unmuted.begin(), unmuted.end(),
		[](const std::array<uint32_t, 5>& value) { return value[3] == 1 || (value[3] == 7 && value[4] == 1); }));
	CHECK(std::any_of(
		unmuted.begin(), unmuted.end(), [](const std::array<uint32_t, 5>& value) { return value[3] == 7; }));
}

TEST_CASE(
	"a MIDI track's mute keeps values of kinds that alternate more often than there are kinds, and unmuting sends "
	"each kind once in the order of the latest events",
	"[unit]")
{
	using Type = graph::ProcessEvent::Type;
	// CC 3 once, then CC 1 and CC 2 in turns, count times over blocks of up to 1000 events: around the 2096 kinds the
	// mute keeps, and twice as many.
	const size_t count = GENERATE(size_t{2095}, size_t{2096}, size_t{2097}, size_t{4400});
	CAPTURE(count);
	constexpr size_t perBlock = 1000;
	const size_t muted = 1 + ((count + perBlock - 1) / perBlock); // blocks
	MidiStrip strip({{0, 1.0}, {(static_cast<int64_t>(muted) * g_midiBlock) + 10, 0.0}});
	CHECK(strip.play({midiEvent(Type::Controller, 0, 0, 3, 99)}).empty());
	for (size_t first = 0; first < count; first += perBlock)
	{
		std::vector<graph::ProcessEvent> events;
		for (size_t i = first; i < std::min(count, first + perBlock); ++i)
			events.push_back(midiEvent(Type::Controller, static_cast<uint32_t>((i - first) / 4), 0,
				static_cast<uint8_t>(i % 2 == 0 ? 1 : 2), static_cast<uint32_t>(i)));
		CHECK(strip.play(events).empty());
	}
	const auto last = static_cast<uint32_t>(count - 1);
	const uint32_t latest = last % 2 == 0 ? 1 : 2;
	CHECK(midiValuesOf(strip.play({})) ==
		MidiValues{{g_controller, 10, 0, 3, 99}, {g_controller, 10, 0, 3 - latest, last - 1},
			{g_controller, 10, 0, latest, last}});
	CHECK(strip.play({}).empty());
}

TEST_CASE("what unmuting a MIDI track sends at the start of the next block goes before a new value of its controller "
		  "lane there",
	"[unit]")
{
	// The track's CC 0 lane steps to its highest value at the start of the fourth block, where the rest of what the
	// mute dropped goes out: CC 0, its latest kind, among it.
	RenderAutomationLane lane;
	lane.target = {AutomationTargetType::Controller, 0, 0, 0, 0};
	lane.points = {{0, 0.0, AutomationShape::Step, 0.0}, {int64_t{3} * g_midiBlock, 1.0, AutomationShape::Step, 0.0}};
	MidiStrip strip({{0, 1.0}, {(2 * g_midiBlock) + 10, 0.0}}, {lane});
	dropControllers(strip);
	CHECK(strip.play({}).size() == graph::g_maxEventsPerBlock);
	const MidiValues rest = midiValuesOf(strip.play({}));
	REQUIRE(rest.size() > 2);
	CHECK(rest[rest.size() - 2] == std::array<uint32_t, 5>{g_controller, 0, 0, 0, 7});
	CHECK(rest.back() == std::array<uint32_t, 5>{g_controller, 0, 0, 0, 0xFFFFFFFFU});
}

namespace
{
	// A MIDI track's source that passes on what it receives but CC 7, as a MIDI insert that filters it would.
	class WithoutVolume : public graph::Processor
	{
	public:
		void process(const graph::ProcessContext& context) noexcept override
		{
			for (const graph::ProcessEvent& event : context.eventsIn)
				if (event.type != graph::ProcessEvent::Type::Controller || event.key != 7)
					context.eventsOut.add(event);
		}
	};
}

TEST_CASE("unmuting a MIDI track sends what its mute kept after the channel's values that its source passes on, also "
		  "where its source leaves one out, and before the track's own events at that sample",
	"[unit]")
{
	using Type = graph::ProcessEvent::Type;
	RenderChannel channel;
	channel.type = TrackType::Midi;
	channel.format = ChannelFormat::None;
	channel.program = 3;
	channel.midiVolume = 100;
	channel.midiPan = 64;
	MidiStrip strip({}, {}, std::make_shared<WithoutVolume>());
	strip.setChannel(channel);
	CHECK(!strip.play({}).empty());
	channel.muted = true;
	channel.silent = true;
	strip.setChannel(channel);
	CHECK(strip.play({}).empty());
	// After a move while muted, a pan plays into the mute.
	strip.locate(10000);
	CHECK(strip.play({midiEvent(Type::Controller, 5, 0, 10, 20)}).empty());
	channel.muted = false;
	channel.silent = false;
	strip.setChannel(channel);
	// The channel's program and pan go out again, without the MIDI volume, then the pan the track left, which wins,
	// then the track's MIDI input at that sample, which the source passes on before the channel's values.
	const MidiValues unmuted = midiValuesOf(strip.play({midiEvent(Type::Controller, 0, 0, 11, 33)}));
	REQUIRE(unmuted.size() == 4);
	CHECK(unmuted[0][0] == g_program);
	CHECK(unmuted[0][3] == 3);
	CHECK(unmuted[1][0] == g_controller);
	CHECK(unmuted[1][3] == 10);
	CHECK(unmuted[1][4] != 20);
	CHECK(unmuted[2] == std::array<uint32_t, 5>{g_controller, 0, 0, 10, 20});
	CHECK(unmuted[3] == std::array<uint32_t, 5>{g_controller, 0, 0, 11, 33});
}
