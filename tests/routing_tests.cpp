// SPDX-License-Identifier: GPL-3.0-or-later
// Sends to group and effect channels, and routing that never closes a loop (#108, docs/features/routing.md).
#include "engine/document/document.h"
#include "engine/engine.h"
#include "testutils/heldjob.h"
#include "testutils/ticks.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;
using gnubase::test::ticks;

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
	// the same output, so the output shows what a send adds. The track is stereo unless format says otherwise.
	class Sends
	{
	public:
		explicit Sends(int blockSize = 64, const std::string& format = {})
		{
			run(client, "audio.setDevice",
				{{"blockSize", blockSize}, {"inputChannels", 2}, {"outputChannels", 2}, {"captureSeconds", 1},
					{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 441.0}, {"Level", 0.5}}}}}});
			Json parameters = {{"type", "audio"}};
			if (!format.empty())
				parameters["format"] = format;
			const Json added = run(client, "track.add", parameters);
			track = channelOf(added);
			trackId = added.at("track").at("id").get<ObjectId>();
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
		ObjectId track = 0; // the audio track's channel
		ObjectId trackId = 0;
		ObjectId effect = 0;
	};

	ObjectId trackOf(const Json& added)
	{
		return added.at("track").at("id").get<ObjectId>();
	}

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

	// The strips whose pre-fader tap mute automation mutes: a stereo channel's; that of a channel with a plugin slot,
	// whose fader runs in a node of its own (StripPart::Fader); and a mono channel's, whose panner places it on the
	// stereo output.
	enum class Strip : uint8_t
	{
		Stereo,
		Slot,
		Mono,
	};

	const char* name(Strip strip)
	{
		if (strip == Strip::Slot)
			return "with a plugin slot";
		return strip == Strip::Mono ? "mono" : "stereo";
	}

	// The main output while the track, a channel of that strip that monitors its input, plays from the start, after
	// setUp.
	std::vector<float> played(int blockSize, Strip strip, const std::function<void(Sends&)>& setUp, int64_t frames)
	{
		Sends mixer(blockSize, strip == Strip::Mono ? "mono" : "");
		if (strip == Strip::Slot)
		{
			const Json slot =
				run(mixer.client, "plugin.insert", {{"channel", mixer.track}, {"plugin", "builtin:gainPan"}})
					.at("slot")
					.at("id");
			gnubase::test::waitForPlugin(mixer.client, slot);
		}
		mixer.set({{"monitor", true}});
		setUp(mixer);
		run(mixer.client, "transport.play");
		run(mixer.client, "audio.render", {{"frames", frames}});
		return mixer.output(frames);
	}

	// A pre-fader send at -6 dB to the effect channel.
	void sendPreFader(Sends& mixer)
	{
		mixer.set({{"sends",
			{{{"send", 1}, {"destination", mixer.effect}, {"on", true}, {"level", -6.0}, {"preFader", true}}}}});
	}

	// Mute automation of the track: the points' positions in samples, with their values.
	void automateMute(Sends& mixer, const std::vector<std::pair<int64_t, double>>& points)
	{
		const ObjectId lane =
			run(mixer.client, "automationLane.add", {{"track", mixer.trackId}, {"target", {{"type", "mute"}}}})
				.at("lane")
				.at("id")
				.get<ObjectId>();
		Json items = Json::array();
		for (const auto& [position, value] : points)
			items.push_back({{"position", ticks(position)}, {"value", value}});
		run(mixer.client, "automationPoint.add", {{"lane", lane}, {"points", items}});
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

TEST_CASE("a send that is on glides out of its old destination and into a new one, and in from silence when it gets "
		  "one again",
	"[integration]")
{
	// A second effect channel at -6 dB tells the two destinations apart on the output.
	Sends mixer;
	const ObjectId other = channelOf(run(mixer.client, "track.add", {{"type", "effect"}}));
	run(mixer.client, "channel.set", {{"channel", other}, {"volume", -6.0}});
	mixer.set({{"sends", {{{"send", 1}, {"destination", mixer.effect}, {"on", true}, {"level", 0.0}}}}});
	run(mixer.client, "audio.render", {{"frames", 128}});
	mixer.set({{"sends", {{{"send", 1}, {"destination", other}}}}});
	run(mixer.client, "audio.render", {{"frames", 128}});
	mixer.set({{"sends", {{{"send", 1}, {"destination", 0}}}}});
	run(mixer.client, "audio.render", {{"frames", 128}});
	mixer.set({{"sends", {{{"send", 1}, {"destination", other}}}}});
	run(mixer.client, "audio.render", {{"frames", 128}});
	const double half = gain(-6.0);
	checkGain(mixer.output(512),
		[half](size_t i)
		{
			const double x = std::min(1.0, static_cast<double>((i % 128) + 1) / 64.0); // through a block's glide
			if (i < 128)
				return 2.0;
			if (i < 256)
				return 1.0 + (1.0 - x) + (half * x);
			if (i < 384)
				return 1.0 + (half * (1.0 - x));
			return 1.0 + (half * x);
		});
}

TEST_CASE("a send gliding out of a destination that now plays into its channel stops at once", "[integration]")
{
	// No block between the changes: the send to the other effect channel would still glide out of it.
	Sends mixer;
	const ObjectId other = channelOf(run(mixer.client, "track.add", {{"type", "effect"}}));
	run(mixer.client, "channel.set", sends(mixer.effect, {{{"send", 1}, {"destination", other}, {"on", true}}}));
	run(mixer.client, "audio.render", {{"frames", 128}});
	run(mixer.client, "channel.set", sends(mixer.effect, {{{"send", 1}, {"destination", 0}}}));
	run(mixer.client, "channel.set", {{"channel", other}, {"output", mixer.effect}});
	mixer.set({{"output", other}});
	run(mixer.client, "audio.render", {{"frames", 128}});
	checkGain(mixer.output(256), 1.0); // the track alone, then through both effect channels
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

TEST_CASE("a lower maximum brings send levels above it down, and one undo takes them back up", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId audio = channelOf(run(client, "track.add", {{"type", "audio"}}));
	const ObjectId effect = channelOf(run(client, "track.add", {{"type", "effect"}}));
	run(client, "project.set", {{"maxVolume", 12.0}});
	run(client, "channel.set",
		sends(audio, {{{"send", 1}, {"destination", effect}, {"level", 12.0}}, {{"send", 2}, {"level", -3.0}}}));
	CHECK(channel(client, audio).at("sends")[0].at("level") == 12.0);

	run(client, "project.set", {{"maxVolume", 6.0}});
	CHECK(channel(client, audio).at("sends")[0].at("level") == 6.0);
	CHECK(channel(client, audio).at("sends")[1].at("level") == -3.0);

	run(client, "history.undo");
	CHECK(run(client, "project.get").at("maxVolume") == 12.0);
	CHECK(channel(client, audio).at("sends")[0].at("level") == 12.0);
}

TEST_CASE("outputs go to effect channels too, but never in a loop", "[integration]")
{
	Sends mixer;
	const ObjectId other = channelOf(run(mixer.client, "track.add", {{"type", "effect"}}));
	// The track plays only through the effect channel, which passes it on to the main output.
	mixer.set({{"output", mixer.effect}});
	CHECK(channel(mixer.client, mixer.track).at("output") == mixer.effect);
	run(mixer.client, "audio.render", {{"frames", 1000}});
	checkGain(mixer.output(1000), 1.0);

	// An effect channel that plays into another one can't be sent to from it, and an audio channel is no output.
	run(mixer.client, "channel.set", {{"channel", mixer.effect}, {"output", other}});
	CHECK_THAT(reject(mixer.client, "channel.set", sends(other, {{{"send", 1}, {"destination", mixer.effect}}})),
		ContainsSubstring("loop"));
	CHECK_THAT(reject(mixer.client, "channel.set", {{"channel", other}, {"output", mixer.track}}),
		ContainsSubstring("output must be"));
}

TEST_CASE("Mute Pre-Send when Mute: the user setting mutes pre-fader sends with their channel", "[integration]")
{
	Sends mixer;
	run(mixer.client, "settings.set", {{"key", "audio.mutePreSends"}, {"value", true}});
	mixer.set({{"mute", true},
		{"sends", {{{"send", 1}, {"destination", mixer.effect}, {"on", true}, {"level", -6.0}, {"preFader", true}}}}});
	run(mixer.client, "audio.render", {{"frames", 1000}});
	checkGain(mixer.output(1000), 0.0);

	// Unmuted, the pre-fader send plays again.
	Sends unmuted;
	run(unmuted.client, "settings.set", {{"key", "audio.mutePreSends"}, {"value", true}});
	unmuted.set({{"sends",
		{{{"send", 1}, {"destination", unmuted.effect}, {"on", true}, {"level", -6.0}, {"preFader", true}}}}});
	run(unmuted.client, "audio.render", {{"frames", 1000}});
	checkGain(unmuted.output(1000), 1.0 + gain(-6.0));
}

TEST_CASE("Mute Pre-Send when Mute: mute automation mutes pre-fader sends from its sample on", "[integration]")
{
	const int blockSize = GENERATE(64, 100);
	const bool mutePreSends = GENERATE(true, false);
	const Strip strip = GENERATE(Strip::Stereo, Strip::Slot, Strip::Mono);
	INFO("block size " << blockSize << ", " << name(strip) << (mutePreSends ? ", Mute Pre-Send when Mute" : ""));
	const std::vector<float> output = played(
		blockSize, strip,
		[mutePreSends](Sends& mixer)
		{
			run(mixer.client, "settings.set", {{"key", "audio.mutePreSends"}, {"value", mutePreSends}});
			sendPreFader(mixer);
			automateMute(mixer, {{0, 0.0}, {1000, 1.0}, {3000, 0.0}});
		},
		4000);

	// The track alone, as it plays its input, and with its send.
	const std::vector<float> alone = played(blockSize, strip, [](Sends&) {}, 4000);
	const std::vector<float> sent = played(blockSize, strip, sendPreFader, 4000);

	// Mute glides over 5 ms (240 frames) from the point's sample on; with the setting, the pre-fader send with it.
	const auto audible = [](size_t position)
	{
		if (position < 1000)
			return 1.0;
		if (position < 3000)
			return std::max(0.0, 1.0 - (static_cast<double>(position - 1000) / 240.0));
		return std::min(1.0, static_cast<double>(position - 3000) / 240.0);
	};
	size_t wrong = 0;
	size_t first = 0;
	for (size_t i = 0; i < output.size(); ++i)
	{
		const double send = sent[i] - alone[i];
		const double expected = (alone[i] * audible(i)) + (send * (mutePreSends ? audible(i) : 1.0));
		if (std::abs(output[i] - expected) > 1e-6 && wrong++ == 0)
			first = i;
	}
	INFO(wrong << " wrong frames, the first at " << first);
	CHECK(wrong == 0);
}

TEST_CASE(
	"Mute Pre-Send when Mute switched on or off for a muted channel glides its pre-fader sends there", "[integration]")
{
	Sends mixer;
	mixer.set({{"mute", true},
		{"sends", {{{"send", 1}, {"destination", mixer.effect}, {"on", true}, {"level", -6.0}, {"preFader", true}}}}});
	run(mixer.client, "audio.render", {{"frames", 128}});
	run(mixer.client, "settings.set", {{"key", "audio.mutePreSends"}, {"value", true}});
	run(mixer.client, "audio.render", {{"frames", 128}});
	run(mixer.client, "settings.set", {{"key", "audio.mutePreSends"}, {"value", false}});
	run(mixer.client, "audio.render", {{"frames", 128}});
	const double half = gain(-6.0);
	checkGain(mixer.output(384),
		[half](size_t i)
		{
			const double x = std::min(1.0, static_cast<double>((i % 128) + 1) / 64.0); // through a block's glide
			if (i < 128)
				return half;
			if (i < 256)
				return half * (1.0 - x);
			return half * x;
		});
}

TEST_CASE("Mute Pre-Send when Mute switched on or off under mute automation glides the pre-fader sends there",
	"[integration]")
{
	const int blockSize = GENERATE(64, 100);
	INFO("block size " << blockSize);
	const auto block = static_cast<int64_t>(blockSize);
	Sends mixer(blockSize);
	mixer.set({{"monitor", true}});
	sendPreFader(mixer);
	automateMute(mixer, {{0, 1.0}});
	run(mixer.client, "transport.play");
	// Muted by then, its 5 ms glide (240 frames) over.
	run(mixer.client, "audio.render", {{"frames", 6 * block}});
	run(mixer.client, "settings.set", {{"key", "audio.mutePreSends"}, {"value", true}});
	run(mixer.client, "audio.render", {{"frames", 2 * block}});
	run(mixer.client, "settings.set", {{"key", "audio.mutePreSends"}, {"value", false}});
	run(mixer.client, "audio.render", {{"frames", 2 * block}});
	const std::vector<float> output = mixer.output(10 * block);

	// The track's input, as it passes it on alone. The send glides out over the first block of the new graph, and in
	// again over the first block of the next one.
	const std::vector<float> input = played(blockSize, Strip::Stereo, [](Sends&) {}, 10 * block);
	const double half = gain(-6.0);
	const auto expected = [half, block](size_t i)
	{
		const auto frame = static_cast<int64_t>(i);
		const double x = static_cast<double>((frame % block) + 1) / static_cast<double>(block);
		switch (frame / block)
		{
		case 6:
			return half * (1.0 - x);
		case 7:
			return 0.0;
		case 8:
			return half * x;
		default:
			return half;
		}
	};
	size_t wrong = 0;
	size_t first = 0;
	for (auto i = static_cast<size_t>(6 * block); i < output.size(); ++i)
		if (std::abs(output[i] - (input[i] * expected(i))) > 1e-6 && wrong++ == 0)
			first = i;
	INFO(wrong << " wrong frames, the first at " << first << ": " << output[first] << " instead of "
			   << input[first] * expected(first));
	CHECK(wrong == 0);
}

TEST_CASE("a send wider than its destination is converted like an output", "[integration]")
{
	// A stereo track's pre-fader send into a mono effect channel: L and R merge at -3.01 dB each, and the mono
	// channel is panned onto the stereo output at the center.
	Sends mixer;
	const ObjectId mono = channelOf(run(mixer.client, "track.add", {{"type", "effect"}, {"format", "mono"}}));
	mixer.set({{"output", 0}, {"sends", {{{"send", 1}, {"destination", mono}, {"on", true}, {"preFader", true}}}}});
	const Json graph = run(mixer.client, "graph.get");
	const auto send = std::find_if(
		graph.at("nodes").begin(), graph.at("nodes").end(), [](const Json& node) { return node.at("kind") == "send"; });
	REQUIRE(send != graph.at("nodes").end());
	CHECK(send->at("format") == "stereo");
	run(mixer.client, "audio.render", {{"frames", 1000}});
	const std::vector<float> output = mixer.output(1000);
	const double merge = 0.70794578438413791; // -3.01 dB, and the -3 dB pan law's center
	size_t wrong = 0;
	for (size_t i = 0; i < output.size(); ++i)
		wrong += std::abs(output[i] - (sine()[i] * 2.0 * merge * merge)) > 1e-5 ? 1 : 0;
	CHECK(wrong == 0);
}

TEST_CASE("a new group or effect track for selected tracks takes their outputs or sends", "[unit]")
{
	Engine engine;
	Client client(engine);
	const Json first = run(client, "track.add", {{"type", "audio"}});
	const Json second = run(client, "track.add", {{"type", "audio"}});
	const Json midi = run(client, "track.add", {{"type", "midi"}});
	const ObjectId old = channelOf(run(client, "track.add", {{"type", "effect"}}));
	const Json ids = {first.at("track").at("id"), second.at("track").at("id")};

	// Group Track to Selected Tracks: both play into the new group; one undo takes it all back.
	const ObjectId group = channelOf(run(client, "track.add", {{"type", "group"}, {"tracks", ids}}));
	CHECK(channel(client, channelOf(first)).at("output") == group);
	CHECK(channel(client, channelOf(second)).at("output") == group);
	run(client, "history.undo");
	CHECK(channel(client, channelOf(first)).at("output") != group);

	// Effect Track to Selected Tracks: the first free send of each, on, after the fader, at the default send level.
	run(client, "settings.set", {{"key", "audio.defaultSendLevel"}, {"value", -10.0}});
	run(client, "channel.set", sends(channelOf(first), {{{"send", 1}, {"destination", old}}}));
	const ObjectId effect = channelOf(run(client, "track.add", {{"type", "effect"}, {"tracks", ids}}));
	const Json expected = {{"destination", effect}, {"on", true}, {"level", -10.0}, {"preFader", false}};
	CHECK(channel(client, channelOf(first)).at("sends")[1] == expected);
	CHECK(channel(client, channelOf(second)).at("sends")[0] == expected);

	// Add Effect Track to Send: that send, whatever it had.
	const ObjectId third = channelOf(run(
		client, "track.add", {{"type", "effect"}, {"tracks", Json::array({first.at("track").at("id")})}, {"send", 1}}));
	CHECK(channel(client, channelOf(first)).at("sends")[0].at("destination") == third);

	// A track without an audio channel is left out, and the group is added all the same.
	const Json one = Json::array({midi.at("track").at("id")});
	CHECK(run(client, "track.add", {{"type", "group"}, {"tracks", one}}).at("track").at("type") == "group");
	CHECK_THAT(reject(client, "track.add", {{"type", "group"}, {"tracks", Json::array({999999})}}),
		ContainsSubstring("no track 999999"));
	CHECK_THAT(reject(client, "track.add", {{"type", "audio"}, {"tracks", ids}}), ContainsSubstring("take tracks"));
	CHECK_THAT(reject(client, "track.add", {{"type", "group"}, {"tracks", ids}, {"send", 1}}),
		ContainsSubstring("only effect tracks"));
	CHECK_THAT(
		reject(client, "track.add", {{"type", "effect"}, {"tracks", ids}, {"send", 9}}), ContainsSubstring("send"));
	CHECK_THAT(reject(client, "track.add", {{"type", "effect"}, {"send", 1}}), ContainsSubstring("send needs tracks"));
}

TEST_CASE("group and effect tracks for selected tracks leave out the tracks that would close a loop", "[unit]")
{
	Engine engine;
	Client client(engine);
	const Json audio = run(client, "track.add", {{"type", "audio"}});
	const ObjectId main = channel(client, channelOf(audio)).at("output").get<ObjectId>();

	// A new group in the project's Group folder plays into the folder's group channel, which can't play into it, so
	// with the folder selected, only the audio track plays into the new group (reference manual p. 546).
	const Json folder = run(client, "track.add", {{"type", "folder"}, {"name", "Group"}, {"groupChannel", true}});
	const Json both = {trackOf(folder), trackOf(audio)};
	const Json added = run(client, "track.add", {{"type", "group"}, {"projectFolder", true}, {"tracks", both}});
	CHECK(added.at("track").at("parent") == trackOf(folder));
	CHECK(channel(client, channelOf(added)).at("output") == channelOf(folder));
	CHECK(channel(client, channelOf(folder)).at("output") == main);
	CHECK(channel(client, channelOf(audio)).at("output") == channelOf(added));
	run(client, "history.undo");
	CHECK(channel(client, channelOf(audio)).at("output") == main);

	// Likewise, an effect channel that the new one plays into keeps its sends, and the audio track sends to it
	// (p. 554), also with send.
	const Json effect = run(client, "track.add", {{"type", "effect"}});
	const Json selection = {trackOf(effect), trackOf(audio)};
	for (const Json& parameters : {Json::object(), Json{{"send", 2}}})
	{
		Json add = parameters;
		add.update({{"type", "effect"}, {"output", channelOf(effect)}, {"tracks", selection}});
		const ObjectId sent = channelOf(run(client, "track.add", add));
		const size_t send = parameters.empty() ? 0 : 1;
		CHECK(channel(client, channelOf(audio)).at("sends")[send].at("destination") == sent);
		const Json kept = channel(client, channelOf(effect)).at("sends");
		CHECK(std::all_of(kept.begin(), kept.end(), [](const Json& item) { return item.at("destination") == 0; }));
		run(client, "history.undo");
	}
}

TEST_CASE(
	"group and effect tracks for selected tracks route those that can be routed and leave out the others", "[unit]")
{
	Engine engine;
	Client client(engine);
	const Json audio = run(client, "track.add", {{"type", "audio"}});
	const Json full = run(client, "track.add", {{"type", "audio"}});
	const Json midi = run(client, "track.add", {{"type", "midi"}});
	const Json marker = run(client, "track.add", {{"type", "marker"}});
	const Json folder = run(client, "track.add", {{"type", "folder"}});
	const Json inside = run(client, "track.add", {{"type", "audio"}, {"parent", trackOf(folder)}});
	const Json grouped = run(client, "track.add", {{"type", "folder"}, {"groupChannel", true}});
	const ObjectId old = channelOf(run(client, "track.add", {{"type", "effect"}}));
	Json all = Json::array();
	for (int send = 1; send <= 8; ++send)
		all.push_back({{"send", send}, {"destination", old}});
	run(client, "channel.set", sends(channelOf(full), all));
	const Json selection = {
		trackOf(audio), trackOf(full), trackOf(midi), trackOf(marker), trackOf(folder), trackOf(grouped)};
	const ObjectId main = channel(client, channelOf(inside)).at("output").get<ObjectId>();

	// The audio channels play into the group, the folder's group channel too; a folder without one stands for none of
	// the tracks inside it, which only play into the group when they are selected themselves.
	const ObjectId group = channelOf(run(client, "track.add", {{"type", "group"}, {"tracks", selection}}));
	CHECK(channel(client, channelOf(audio)).at("output") == group);
	CHECK(channel(client, channelOf(full)).at("output") == group);
	CHECK(channel(client, channelOf(grouped)).at("output") == group);
	CHECK(channel(client, channelOf(inside)).at("output") == main);

	// With send, the tracks without an audio channel are left out too.
	const ObjectId third =
		channelOf(run(client, "track.add", {{"type", "effect"}, {"tracks", selection}, {"send", 3}}));
	CHECK(channel(client, channelOf(audio)).at("sends")[2].at("destination") == third);
	CHECK(channel(client, channelOf(midi)).at("sends") == Json::array());
	run(client, "history.undo");

	// Those with a free send send to the effect channel; one without stays as it is. One undo takes it all back.
	const ObjectId effect = channelOf(run(client, "track.add", {{"type", "effect"}, {"tracks", selection}}));
	CHECK(channel(client, channelOf(audio)).at("sends")[0].at("destination") == effect);
	CHECK(channel(client, channelOf(grouped)).at("sends")[0].at("destination") == effect);
	const Json fullSends = channel(client, channelOf(full)).at("sends");
	CHECK(std::all_of(
		fullSends.begin(), fullSends.end(), [old](const Json& send) { return send.at("destination") == old; }));
	run(client, "history.undo");
	CHECK(channel(client, channelOf(audio)).at("sends")[0] ==
		Json{{"destination", 0}, {"on", false}, {"level", 0.0}, {"preFader", false}});
	CHECK(channel(client, channelOf(grouped)).at("sends")[0].at("destination") == 0);
	const Json channels = run(client, "project.get").at("channels");
	CHECK(
		std::none_of(channels.begin(), channels.end(), [effect](const Json& item) { return item.at("id") == effect; }));
}

TEST_CASE("group and effect tracks go into the project's Group and FX folders", "[unit]")
{
	Engine engine;
	Client client(engine);
	const Json group = run(client, "track.add", {{"type", "group"}, {"projectFolder", true}});
	const Json again = run(client, "track.add", {{"type", "group"}, {"projectFolder", true}});
	const Json audio = run(client, "track.add", {{"type", "audio"}});
	const Json effect = run(client, "track.add",
		{{"type", "effect"}, {"projectFolder", true}, {"tracks", Json::array({audio.at("track").at("id")})}});
	CHECK(channel(client, channelOf(audio)).at("sends")[0].at("destination") == channelOf(effect));

	const Json project = run(client, "project.get");
	std::vector<std::pair<std::string, size_t>> folders; // the top-level folders and how many tracks they have
	for (const Json& track : project.at("tracks"))
		if (track.at("type") == "folder" && track.at("parent") == 0)
			folders.emplace_back(track.at("name").get<std::string>(),
				std::count_if(project.at("tracks").begin(), project.at("tracks").end(),
					[&track](const Json& inside) { return inside.at("parent") == track.at("id"); }));
	CHECK(folders == std::vector<std::pair<std::string, size_t>>{{"Group", 2}, {"FX", 1}});
	CHECK(group.at("track").at("parent") == again.at("track").at("parent"));
	CHECK(effect.at("track").at("parent") != group.at("track").at("parent"));

	CHECK_THAT(reject(client, "track.add", {{"type", "audio"}, {"projectFolder", true}}),
		ContainsSubstring("only group, effect and VCA"));
	CHECK_THAT(reject(client, "track.add",
				   {{"type", "group"}, {"projectFolder", true}, {"parent", group.at("track").at("parent")}}),
		ContainsSubstring("other parent"));
	// One undo takes the track and the folder made for it.
	run(client, "history.undo");
	const Json undone = run(client, "project.get").at("tracks");
	CHECK(std::none_of(undone.begin(), undone.end(), [](const Json& track) { return track.at("name") == "FX"; }));
}
