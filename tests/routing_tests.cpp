// SPDX-License-Identifier: GPL-3.0-or-later
// Sends to group and effect channels, and routing that never closes a loop (#108, docs/features/routing.md).
#include "engine/document/document.h"
#include "engine/engine.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
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

	double gain(double decibels)
	{
		return std::pow(10.0, decibels / 20.0);
	}

	ObjectId channelOf(const Json& added)
	{
		return added.at("channel").at("id").get<ObjectId>();
	}

	Json channel(Client& client, ObjectId id)
	{
		const Json project = run(client, "project.get");
		for (const Json& item : project.at("channels"))
			if (item.at("id") == id)
				return item;
		FAIL("no channel " << id);
	}

	Json sends(ObjectId channel, Json items)
	{
		return {{"channel", channel}, {"sends", std::move(items)}};
	}

	// An audio track that passes a sine from the dummy device's input to the stereo output, and an effect channel on
	// the same output, so the output shows what a send adds.
	class Sends
	{
	public:
		Sends()
		{
			run(client, "audio.setDevice",
				{{"blockSize", 64}, {"inputChannels", 2}, {"outputChannels", 2}, {"captureSeconds", 1},
					{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 441.0}, {"Level", 0.5}}}}}});
			track = channelOf(run(client, "track.add", {{"type", "audio"}}));
			effect = channelOf(run(client, "track.add", {{"type", "effect"}}));
		}

		void set(Json parameters)
		{
			parameters["channel"] = track;
			run(client, "channel.set", std::move(parameters));
		}

		std::vector<float> output(int64_t frames)
		{
			return run(client, "audio.capture", {{"channel", 0}, {"start", 0}, {"frames", frames}})
				.at("samples")
				.get<std::vector<float>>();
		}

		Engine engine;
		Client client{engine};
		ObjectId track = 0;
		ObjectId effect = 0;
	};

	// The sine as the track alone passes it on.
	const std::vector<float>& sine()
	{
		static const std::vector<float> samples = []
		{
			Sends mixer;
			run(mixer.client, "audio.render", {{"frames", 1000}});
			return mixer.output(1000);
		}();
		return samples;
	}

	void checkGain(const std::vector<float>& output, const std::function<double(size_t)>& gainAt)
	{
		size_t wrong = 0;
		size_t first = 0;
		for (size_t i = 0; i < output.size(); ++i)
			if (std::abs(output[i] - (sine()[i] * gainAt(i))) > 1e-6 && wrong++ == 0)
				first = i;
		INFO(wrong << " wrong frames, the first at " << first << ": " << output[first] << " instead of "
				   << sine()[first] * gainAt(first));
		CHECK(wrong == 0);
	}

	void checkGain(const std::vector<float>& output, double constant)
	{
		checkGain(output, [constant](size_t) { return constant; });
	}
}

TEST_CASE("channel.set sets a channel's sends, refuses what can't be a destination and undoes", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId audio = channelOf(run(client, "track.add", {{"type", "audio"}}));
	const ObjectId effect = channelOf(run(client, "track.add", {{"type", "effect"}}));
	const ObjectId group = channelOf(run(client, "track.add", {{"type", "group"}}));
	const ObjectId midi = channelOf(run(client, "track.add", {{"type", "midi"}}));
	const ObjectId folder = channelOf(run(client, "track.add", {{"type", "folder"}, {"groupChannel", true}}));

	// Eight sends, off and without a destination; MIDI channels have none.
	const Json fresh = channel(client, audio).at("sends");
	REQUIRE(fresh.size() == 8);
	CHECK(fresh[0] == Json({{"destination", 0}, {"on", false}, {"level", 0.0}, {"preFader", false}}));
	CHECK(channel(client, midi).at("sends") == Json::array());

	// Each send changes only what it has.
	const Json set = run(client, "channel.set",
		sends(audio,
			{{{"send", 1}, {"destination", effect}, {"on", true}, {"level", -6.0}},
				{{"send", 8}, {"destination", group}, {"preFader", true}}}))
						 .at("channel")
						 .at("sends");
	CHECK(set[0] == Json({{"destination", effect}, {"on", true}, {"level", -6.0}, {"preFader", false}}));
	CHECK(set[7] == Json({{"destination", group}, {"on", false}, {"level", 0.0}, {"preFader", true}}));
	run(client, "channel.set", sends(audio, {{{"send", 1}, {"level", -12.0}}}));
	CHECK(channel(client, audio).at("sends")[0].at("destination") == effect);
	CHECK(channel(client, audio).at("sends")[0].at("level") == -12.0);
	// A folder's group channel takes sends too; the level stops at the fader's ends.
	run(client, "channel.set", sends(audio, {{{"send", 2}, {"destination", folder}, {"level", 20.0}}}));
	CHECK(channel(client, audio).at("sends")[1].at("level") == 6.0);
	run(client, "channel.set", sends(audio, {{{"send", 2}, {"level", -300.0}}}));
	CHECK(channel(client, audio).at("sends")[1].at("level") == -144.0);

	CHECK_THAT(reject(client, "channel.set", sends(audio, {{{"send", 9}}})), ContainsSubstring("send"));
	CHECK_THAT(reject(client, "channel.set", sends(audio, {{{"send", 0}}})), ContainsSubstring("send"));
	CHECK_THAT(reject(client, "channel.set", sends(midi, {{{"send", 1}, {"on", true}}})), ContainsSubstring("MIDI"));
	CHECK_THAT(reject(client, "channel.set", sends(effect, {{{"send", 1}, {"destination", audio}}})),
		ContainsSubstring("group or effect"));
	const ObjectId output = run(client, "project.get").at("busses")[1].at("id").get<ObjectId>();
	CHECK_THAT(reject(client, "channel.set", sends(audio, {{{"send", 1}, {"destination", output}}})),
		ContainsSubstring("group or effect"));
	CHECK_THAT(reject(client, "channel.set", sends(effect, {{{"send", 1}, {"destination", effect}}})),
		ContainsSubstring("loop"));

	// Undo takes the last change back.
	run(client, "history.undo");
	CHECK(channel(client, audio).at("sends")[1].at("level") == 6.0);
}

TEST_CASE("outputs and sends never close a loop, through nested groups and effect channels", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId first = channelOf(run(client, "track.add", {{"type", "group"}}));
	const ObjectId second = channelOf(run(client, "track.add", {{"type", "group"}}));
	const ObjectId effect = channelOf(run(client, "track.add", {{"type", "effect"}}));
	const ObjectId other = channelOf(run(client, "track.add", {{"type", "effect"}}));

	// Nested groups: first plays into second, which sends to the effect channel, which sends to the other one.
	run(client, "channel.set", {{"channel", first}, {"output", second}});
	run(client, "channel.set", sends(second, {{{"send", 1}, {"destination", effect}}}));
	run(client, "channel.set", sends(effect, {{{"send", 1}, {"destination", other}}}));

	// Back to any of them through an output or a send, on or off, would be a loop.
	CHECK_THAT(reject(client, "channel.set", {{"channel", second}, {"output", first}}), ContainsSubstring("loop"));
	CHECK_THAT(reject(client, "channel.set", sends(second, {{{"send", 2}, {"destination", first}}})),
		ContainsSubstring("loop"));
	CHECK_THAT(reject(client, "channel.set", sends(other, {{{"send", 1}, {"destination", first}}})),
		ContainsSubstring("loop"));
	CHECK_THAT(reject(client, "channel.set", sends(other, {{{"send", 1}, {"destination", effect}}})),
		ContainsSubstring("loop"));
	CHECK_THAT(reject(client, "channel.set", {{"channel", other}, {"output", first}}), ContainsSubstring("loop"));
	// Elsewhere is fine: the other effect channel into the first group's sibling, a send skipping a step.
	run(client, "channel.set", sends(first, {{{"send", 1}, {"destination", other}}}));

	// The processing graph has a node per send with a destination, and no cycle.
	const Json graph = run(client, "graph.get");
	size_t count = 0;
	for (const Json& node : graph.at("nodes"))
		count += node.at("kind") == "send" ? 1 : 0;
	CHECK(count == 3);
	CHECK(graph.at("order").size() == graph.at("nodes").size());
}

TEST_CASE("a send's destination track going clears it; duplicates send to the copies of their destinations", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId audio = channelOf(run(client, "track.add", {{"type", "audio"}}));
	const Json effectTrack = run(client, "track.add", {{"type", "effect"}});
	const ObjectId effect = channelOf(effectTrack);
	const Json folder = run(client, "track.add", {{"type", "folder"}, {"groupChannel", true}});
	const ObjectId inside =
		channelOf(run(client, "track.add", {{"type", "audio"}, {"parent", folder.at("track").at("id")}}));
	run(client, "channel.set", sends(audio, {{{"send", 1}, {"destination", effect}, {"on", true}}}));
	run(client, "channel.set", sends(inside, {{{"send", 1}, {"destination", channelOf(folder)}}}));

	// A copy of the folder: the track inside sends to the copy's group channel.
	const Json copy = run(client, "track.duplicate", {{"track", folder.at("track").at("id")}}).at("track");
	const Json project = run(client, "project.get");
	ObjectId copiedInside = 0;
	for (const Json& track : project.at("tracks"))
		if (track.at("parent") == copy.at("id"))
			copiedInside = track.at("channel").get<ObjectId>();
	REQUIRE(copiedInside != 0);
	CHECK(channel(client, copiedInside).at("sends")[0].at("destination") == copy.at("channel"));

	run(client, "track.remove", {{"track", effectTrack.at("track").at("id")}});
	CHECK(channel(client, audio).at("sends")[0].at("destination") == 0);
	CHECK(channel(client, audio).at("sends")[0].at("on") == true);
	run(client, "track.set", {{"track", folder.at("track").at("id")}, {"groupChannel", false}});
	CHECK(channel(client, inside).at("sends")[0].at("destination") == 0);
	run(client, "history.undo");
	run(client, "history.undo");
	CHECK(channel(client, audio).at("sends")[0].at("destination") == effect);
}

TEST_CASE("send nodes take a channel's output after its fader, or before it from its strip", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId audio = channelOf(run(client, "track.add", {{"type", "audio"}}));
	const ObjectId effect = channelOf(run(client, "track.add", {{"type", "effect"}}));
	run(client, "channel.set",
		sends(audio,
			{{{"send", 1}, {"destination", effect}}, {{"send", 2}, {"destination", effect}, {"preFader", true}}}));

	const Json graph = run(client, "graph.get");
	const Json& nodes = graph.at("nodes");
	REQUIRE(nodes.size() == 6); // in, out, the track's channel, the effect channel, two sends
	CHECK(nodes[4].at("kind") == "send");
	CHECK(nodes[4].at("object") == audio);
	CHECK(nodes[4].at("audioInputs") == Json::array({2}));
	CHECK(nodes[5].at("audioInputs") == Json::array());
	CHECK(nodes[5].at("eventInputs") == Json::array({2})); // only after the channel, which hands it the audio
	CHECK(nodes[3].at("audioInputs") == Json::array({4, 5}));
	CHECK(nodes[1].at("audioInputs") == Json::array({2, 3}));
	CHECK(graph.at("order") == Json::array({0, 2, 4, 5, 3, 1}));
}

TEST_CASE(
	"sends add a share of a channel to an effect channel, after or before its fader and its mute", "[integration]")
{
	// A post-fader send at -6 dB: the effect channel adds the channel's output at -6 dB.
	Sends post;
	post.set(
		{{"volume", -20.0}, {"sends", {{{"send", 1}, {"destination", post.effect}, {"on", true}, {"level", -6.0}}}}});
	run(post.client, "audio.render", {{"frames", 1000}});
	checkGain(post.output(1000), gain(-20.0) * (1.0 + gain(-6.0)));

	// Before the fader the send doesn't follow it.
	Sends pre;
	pre.set({{"volume", -20.0},
		{"sends", {{{"send", 1}, {"destination", pre.effect}, {"on", true}, {"level", -6.0}, {"preFader", true}}}}});
	run(pre.client, "audio.render", {{"frames", 1000}});
	checkGain(pre.output(1000), gain(-20.0) + gain(-6.0));

	// Muted, only the pre-fader send plays; an effect channel's fader scales what reaches it.
	Sends muted;
	muted.set({{"mute", true},
		{"sends",
			{{{"send", 1}, {"destination", muted.effect}, {"on", true}, {"level", -6.0}, {"preFader", true}},
				{{"send", 2}, {"destination", muted.effect}, {"on", true}}}}});
	run(muted.client, "channel.set", {{"channel", muted.effect}, {"volume", -6.0}});
	run(muted.client, "audio.render", {{"frames", 1000}});
	checkGain(muted.output(1000), gain(-12.0));

	// A send that is off adds nothing, a second one to the same destination adds as much again.
	Sends two;
	two.set({{"sends",
		{{{"send", 1}, {"destination", two.effect}}, {{"send", 2}, {"destination", two.effect}, {"on", true}},
			{{"send", 3}, {"destination", two.effect}, {"on", true}}}}});
	run(two.client, "audio.render", {{"frames", 1000}});
	checkGain(two.output(1000), 3.0);
}

TEST_CASE("a send switched on or set to another level glides there over a block", "[integration]")
{
	Sends mixer;
	mixer.set({{"sends", {{{"send", 1}, {"destination", mixer.effect}, {"level", -6.0}}}}});
	run(mixer.client, "audio.render", {{"frames", 128}});
	mixer.set({{"sends", {{{"send", 1}, {"on", true}}}}});
	run(mixer.client, "audio.render", {{"frames", 128}});
	mixer.set({{"sends", {{{"send", 1}, {"level", 0.0}}}}});
	run(mixer.client, "audio.render", {{"frames", 128}});
	const double half = gain(-6.0);
	checkGain(mixer.output(384),
		[half](size_t i)
		{
			const double x = std::min(1.0, static_cast<double>((i % 128) + 1) / 64.0); // through a block's glide
			if (i < 128)
				return 1.0;
			if (i < 256)
				return 1.0 + (half * x);
			return 1.0 + half + ((1.0 - half) * x);
		});
}

TEST_CASE("soloing a channel keeps the effect channels it sends to heard", "[integration]")
{
	Sends mixer;
	const ObjectId other = channelOf(run(mixer.client, "track.add", {{"type", "audio"}}));
	mixer.set({{"solo", true}, {"sends", {{{"send", 1}, {"destination", mixer.effect}, {"on", true}}}}});
	// The other track's pre-fader send stays silent while it isn't heard.
	run(mixer.client, "channel.set",
		sends(other, {{{"send", 1}, {"destination", mixer.effect}, {"on", true}, {"preFader", true}}}));
	run(mixer.client, "audio.render", {{"frames", 1000}});
	checkGain(mixer.output(1000), 2.0);
}
