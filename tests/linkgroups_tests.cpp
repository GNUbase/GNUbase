// SPDX-License-Identifier: GPL-3.0-or-later
// Link groups and VCA faders (#110, docs/features/link-groups.md).
#include "engine/document/document.h"
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace gnubase;
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

	ObjectId add(Client& client, const std::string& type)
	{
		return run(client, "track.add", {{"type", type}, {"timeBase", "linear"}})
			.at("channel")
			.at("id")
			.get<ObjectId>();
	}

	Json channelJson(Client& client, ObjectId id)
	{
		const Json project = run(client, "project.get");
		for (const Json& item : project.at("channels"))
			if (item.at("id") == id)
				return item;
		FAIL("no channel " << id);
	}

	// The last event of that name the client received, or null. Takes every event waiting.
	Json lastEvent(Client& client, const std::string& name)
	{
		Json found;
		while (const auto message = client.poll())
			if (const auto* event = std::get_if<Event>(&*message); event != nullptr && event->name == name)
				found = event->data;
		return found;
	}

	Json groups(Client& client)
	{
		return run(client, "project.get").at("linkGroups");
	}

	ObjectId groupOf(const Json& added)
	{
		return added.at("group").at("id").get<ObjectId>();
	}

	ObjectId trackOf(Client& client, ObjectId channel)
	{
		const Json project = run(client, "project.get");
		for (const Json& item : project.at("channels"))
			if (item.at("id") == channel)
				return item.at("track").get<ObjectId>();
		FAIL("no channel " << channel);
	}

	// Audio tracks that pass a sine from the dummy device's input to the stereo output, so the output shows their
	// faders and the VCA faders that control them.
	class Mixer
	{
	public:
		explicit Mixer(int blockSize = 64)
		{
			run(client, "audio.setDevice",
				{{"blockSize", blockSize}, {"inputChannels", 2}, {"outputChannels", 2}, {"captureSeconds", 1},
					{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 441.0}, {"Level", 0.5}}}}}});
			track = add(client, "audio");
			run(client, "channel.set", {{"channel", track}, {"monitor", true}});
		}

		void automate(ObjectId channelId, const Json& points)
		{
			const ObjectId lane = run(
				client, "automationLane.add", {{"track", trackOf(client, channelId)}, {"target", {{"type", "volume"}}}})
									  .at("lane")
									  .at("id")
									  .get<ObjectId>();
			run(client, "automationPoint.add", {{"lane", lane}, {"points", points}});
		}

		std::vector<float> render(int64_t frames)
		{
			run(client, "audio.render", {{"frames", frames}});
			return run(client, "audio.capture", {{"channel", 0}, {"start", 0}, {"frames", frames}})
				.at("samples")
				.get<std::vector<float>>();
		}

		Engine engine;
		Client client{engine};
		ObjectId track = 0;
	};

	// The output of the track at 0 dB without VCA faders, playing.
	const std::vector<float>& sine()
	{
		static const std::vector<float> samples = []
		{
			Mixer mixer;
			run(mixer.client, "transport.play");
			return mixer.render(5000);
		}();
		return samples;
	}

	void checkGain(const std::vector<float>& output, const std::function<double(int64_t)>& gainAt, double tolerance)
	{
		size_t wrong = 0;
		size_t first = 0;
		for (size_t i = 0; i < output.size(); ++i)
			if (std::abs(output[i] - (sine()[i] * gainAt(static_cast<int64_t>(i)))) > tolerance && wrong++ == 0)
				first = i;
		INFO(wrong << " wrong frames, the first at " << first << ": " << output[first] << " instead of "
				   << sine()[first] * gainAt(static_cast<int64_t>(first)));
		CHECK(wrong == 0);
	}

	// A straight ramp of the gain from a dB value at a position to another at a later one, level before and after.
	double ramp(int64_t at, int64_t from, double fromDecibels, int64_t to, double toDecibels)
	{
		const double x = std::clamp(static_cast<double>(at - from) / static_cast<double>(to - from), 0.0, 1.0);
		return gain(fromDecibels) + ((gain(toDecibels) - gain(fromDecibels)) * x);
	}
}

TEST_CASE("linked channels pass on the settings their group links, unless linked is false, in one undo step", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId a = add(client, "audio");
	const ObjectId b = add(client, "audio");
	const ObjectId c = add(client, "instrument");
	const ObjectId other = add(client, "audio");
	run(client, "channel.set", {{"channel", b}, {"volume", -10.0}, {"pan", 0.5}});

	const Json added = run(client, "linkGroup.add", {{"channels", {a, b, c}}, {"pan", true}});
	const ObjectId group = groupOf(added);
	CHECK(added.at("group") ==
		Json({{"id", group}, {"name", "Link Group 1"}, {"channels", {a, b, c}}, {"volume", true}, {"pan", true},
			{"mute", false}, {"record", false}, {"eq", false}, {"dynamics", false}, {"sends", true}, {"vca", 0}}));
	CHECK(groups(client).size() == 1);

	// The fader and the pan move by as much as the channel's, and stop at their ends.
	run(client, "channel.set", {{"channel", a}, {"volume", -4.0}, {"pan", 0.75}});
	CHECK(channelJson(client, b).at("volume") == -14.0);
	CHECK(channelJson(client, b).at("pan") == 1.0);
	CHECK(channelJson(client, c).at("volume") == -4.0);
	CHECK(channelJson(client, other).at("volume") == 0.0);
	// The group doesn't link mute; one undo takes every fader back.
	run(client, "channel.set", {{"channel", a}, {"mute", true}});
	CHECK(channelJson(client, b).at("mute") == false);
	run(client, "history.undo");
	run(client, "history.undo");
	CHECK(channelJson(client, a).at("volume") == 0.0);
	CHECK(channelJson(client, b).at("volume") == -10.0);
	CHECK(channelJson(client, b).at("pan") == 0.5);

	// Mute, solo and listen, record enable and monitor go on as they are; exclusive solo leaves the group soloed.
	run(client, "linkGroup.set", {{"group", group}, {"mute", true}, {"record", true}, {"name", "Drums"}});
	CHECK(groups(client).at(0).at("name") == "Drums");
	run(client, "channel.set", {{"channel", other}, {"solo", true}});
	run(client, "channel.set", {{"channel", b}, {"solo", true}, {"exclusive", true}, {"listen", true}});
	for (const ObjectId id : {a, b, c})
		CHECK(channelJson(client, id).at("solo") == true);
	CHECK(channelJson(client, other).at("solo") == false);
	CHECK(channelJson(client, a).at("listen") == true);
	run(client, "channel.set", {{"channel", c}, {"recordEnable", true}});
	CHECK(channelJson(client, a).at("recordEnable") == true);

	// linked false changes only the channel (Sus, reference manual p. 460).
	run(client, "channel.set", {{"channel", a}, {"volume", -20.0}, {"mute", true}, {"linked", false}});
	CHECK(channelJson(client, b).at("volume") == -10.0);
	CHECK(channelJson(client, b).at("mute") == false);

	// A channel is in one group at most: a new group takes it from the old one.
	const ObjectId second = groupOf(run(client, "linkGroup.add", {{"channels", {c, other}}}));
	CHECK(groups(client).at(0).at("channels") == Json({a, b}));
	CHECK(groups(client).at(1).at("name") == "Link Group 2");
	run(client, "channel.set", {{"channel", other}, {"volume", -3.0}});
	CHECK(channelJson(client, c).at("volume") == -3.0);

	// Removing a track takes its channel out of its group; unlinking removes the group, and undo brings it back.
	run(client, "track.remove", {{"track", trackOf(client, other)}});
	CHECK(groups(client).at(1).at("channels") == Json({c}));
	run(client, "linkGroup.remove", {{"group", second}});
	CHECK(groups(client).size() == 1);
	run(client, "history.undo");
	CHECK(groups(client).size() == 2);

	CHECK_THAT(reject(client, "linkGroup.add", {{"channels", Json::array()}}), ContainsSubstring("needs a channel"));
	CHECK_THAT(reject(client, "linkGroup.add", {{"channels", {a, a}}}), ContainsSubstring("twice"));
	CHECK_THAT(reject(client, "linkGroup.add", {{"channels", {999}}}), ContainsSubstring("no channel 999"));
	CHECK_THAT(reject(client, "linkGroup.set", {{"group", 999}}), ContainsSubstring("no link group 999"));
	CHECK_THAT(reject(client, "linkGroup.add", {{"channels", {a}}, {"vca", b}}), ContainsSubstring("VCA track"));
}

TEST_CASE("VCA faders control one link group each, pass their buttons on and never control themselves", "[unit]")
{
	Engine engine;
	Client client(engine);
	const Json vcaTrack = run(client, "track.add", {{"type", "vca"}});
	CHECK(vcaTrack.at("track").at("name") == "VCA 01");
	const ObjectId vca = vcaTrack.at("channel").at("id").get<ObjectId>();
	CHECK(vcaTrack.at("channel").at("format") == "none");
	CHECK(vcaTrack.at("channel").at("output") == 0);
	const ObjectId main = add(client, "vca");
	const ObjectId a = add(client, "audio");
	const ObjectId b = add(client, "audio");
	const ObjectId c = add(client, "midi");

	// A VCA fader has a fader and buttons, but no pan, routing or audio.
	run(client, "channel.set", {{"channel", vca}, {"volume", -10.0}});
	CHECK_THAT(reject(client, "channel.set", {{"channel", vca}, {"pan", 0.5}}), ContainsSubstring("no pan"));
	CHECK_THAT(reject(client, "channel.set", {{"channel", vca}, {"output", 0}}), ContainsSubstring("no input"));
	CHECK_THAT(reject(client, "track.add", {{"type", "vca"}, {"format", "mono"}}), ContainsSubstring("no audio"));
	CHECK_THAT(reject(client, "channel.set", {{"channel", vca}, {"format", "stereo"}}), ContainsSubstring("no audio"));
	CHECK_THAT(reject(client, "channel.set", {{"channel", vca}, {"channels", 2}}), ContainsSubstring("no audio"));
	const Json parameters = run(client, "parameter.list", {{"channel", vca}}).at("parameters");
	REQUIRE(parameters.size() == 1);
	CHECK(parameters[0].at("target") == Json({{"type", "volume"}}));

	// With a VCA fader, the group doesn't link the volume (reference manual p. 517).
	const ObjectId group = groupOf(run(client, "linkGroup.add", {{"channels", {a, b, c}}, {"vca", vca}}));
	CHECK(groups(client).at(0).at("volume") == false);
	CHECK_THAT(reject(client, "linkGroup.set", {{"group", group}, {"volume", true}}),
		ContainsSubstring("doesn't link the volume"));
	run(client, "channel.set", {{"channel", a}, {"volume", -6.0}});
	CHECK(channelJson(client, b).at("volume") == 0.0);

	// Its buttons go on to the group's channels, those that have them, and through nested VCA faders.
	const ObjectId top = groupOf(run(client, "linkGroup.add", {{"channels", {vca}}, {"vca", main}}));
	run(client, "channel.set", {{"channel", main}, {"mute", true}, {"solo", true}, {"listen", true}});
	for (const ObjectId id : {vca, a, b, c})
	{
		CHECK(channelJson(client, id).at("mute") == true);
		CHECK(channelJson(client, id).at("solo") == true);
	}
	CHECK(channelJson(client, a).at("listen") == true);
	CHECK(channelJson(client, c).at("listen") == false); // a MIDI channel has no listen
	run(client, "channel.set", {{"channel", vca}, {"recordEnable", true}, {"monitor", true}});
	CHECK(channelJson(client, b).at("recordEnable") == true);
	CHECK(channelJson(client, c).at("monitor") == true);
	// One undo step: the VCA fader's change and what it passed on.
	run(client, "history.undo");
	CHECK(channelJson(client, b).at("recordEnable") == false);

	// No loops, nor a VCA fader in the group it controls.
	CHECK_THAT(reject(client, "linkGroup.set", {{"group", group}, {"channels", {a, main}}}),
		ContainsSubstring("would control itself"));
	CHECK_THAT(reject(client, "linkGroup.set", {{"group", group}, {"channels", {a, vca}}}),
		ContainsSubstring("can't control a link group it is in"));

	// A VCA fader assigned to another group leaves the one it controlled, whose volume link comes back.
	const ObjectId other = groupOf(run(client, "linkGroup.add", {{"channels", {a}}, {"vca", main}}));
	CHECK(groups(client).at(1).at("vca") == 0);
	CHECK(groups(client).at(1).at("volume") == true);
	CHECK(groups(client).at(1).at("id") == top);
	CHECK(groups(client).at(0).at("channels") == Json({b, c})); // a went to the new group
	run(client, "linkGroup.set", {{"group", other}, {"vca", 0}});
	CHECK(groups(client).at(2).at("volume") == true);

	// Removing a VCA track leaves its group without a VCA fader; the group it was the only channel of goes.
	run(client, "track.remove", {{"track", trackOf(client, vca)}});
	REQUIRE(groups(client).size() == 2);
	CHECK(groups(client).at(0).at("vca") == 0);
	CHECK(groups(client).at(1).at("id") == other);
}

TEST_CASE("removing a VCA track turns its group's volume link back on, in one undo step", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId vca = add(client, "vca");
	const ObjectId a = add(client, "audio");
	run(client, "linkGroup.add", {{"channels", {a}}, {"vca", vca}});
	CHECK(groups(client).at(0).at("volume") == false);

	// As when the group loses its VCA fader through linkGroup.set (reference manual p. 518).
	run(client, "track.remove", {{"track", trackOf(client, vca)}});
	CHECK(groups(client).at(0).at("vca") == 0);
	CHECK(groups(client).at(0).at("volume") == true);
	CHECK(lastEvent(client, "linkGroup.changed").at("group").at("volume") == true);
	run(client, "history.undo");
	CHECK(groups(client).at(0).at("vca") == vca);
	CHECK(groups(client).at(0).at("volume") == false);
}

TEST_CASE("a link group that loses all its channels goes, and its VCA fader stays", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId vca = add(client, "vca");
	const ObjectId a = add(client, "audio");
	const ObjectId b = add(client, "audio");
	const ObjectId c = add(client, "audio");

	// Its channels move to another group, as linkGroup.add refuses a group without channels.
	const ObjectId first = groupOf(run(client, "linkGroup.add", {{"channels", {a, b}}, {"vca", vca}}));
	lastEvent(client, "linkGroup.removed");
	const ObjectId second = groupOf(run(client, "linkGroup.add", {{"channels", {a, b}}}));
	REQUIRE(groups(client).size() == 1);
	CHECK(groups(client).at(0).at("id") == second);
	CHECK(lastEvent(client, "linkGroup.removed") == Json({{"group", first}}));
	// The VCA fader controls nothing now and can take another group.
	run(client, "linkGroup.set", {{"group", second}, {"vca", vca}});
	CHECK(groups(client).at(0).at("vca") == vca);
	run(client, "history.undo");
	run(client, "history.undo");
	REQUIRE(groups(client).size() == 1);
	CHECK(groups(client).at(0).at("id") == first);
	CHECK(groups(client).at(0).at("channels") == Json({a, b}));
	CHECK(groups(client).at(0).at("vca") == vca);

	// linkGroup.set taking a group's last channel.
	const ObjectId third = groupOf(run(client, "linkGroup.add", {{"channels", {c}}}));
	run(client, "linkGroup.set", {{"group", first}, {"channels", {a, b, c}}});
	REQUIRE(groups(client).size() == 1);
	CHECK(lastEvent(client, "linkGroup.removed") == Json({{"group", third}}));
	run(client, "history.undo");
	REQUIRE(groups(client).size() == 2);

	// Removing the tracks of its channels.
	run(client, "track.remove", {{"track", trackOf(client, c)}});
	REQUIRE(groups(client).size() == 1);
	CHECK(groups(client).at(0).at("id") == first);
	CHECK(lastEvent(client, "linkGroup.removed") == Json({{"group", third}}));
	run(client, "track.remove", {{"track", trackOf(client, a)}});
	run(client, "track.remove", {{"track", trackOf(client, b)}});
	CHECK(groups(client).empty());
	CHECK(channelJson(client, vca).at("id") == vca);
	run(client, "history.undo"); // b's track comes back, and the group with it
	REQUIRE(groups(client).size() == 1);
	CHECK(groups(client).at(0).at("channels") == Json({b}));
	CHECK(groups(client).at(0).at("vca") == vca);
}

TEST_CASE("new link groups get a name no other group has", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId a = add(client, "audio");
	const ObjectId b = add(client, "audio");
	const ObjectId c = add(client, "audio");
	const ObjectId d = add(client, "audio");
	const ObjectId first = groupOf(run(client, "linkGroup.add", {{"channels", {a}}}));
	run(client, "linkGroup.add", {{"channels", {b}}});
	run(client, "linkGroup.remove", {{"group", first}});

	// With one group, the next one would be Link Group 2, which that one is called.
	CHECK(run(client, "linkGroup.add", {{"channels", {c}}}).at("group").at("name") == "Link Group 3");
	// A name the user gave counts too.
	run(client, "linkGroup.set", {{"group", groups(client).at(0).at("id")}, {"name", "Link Group 4"}});
	CHECK(run(client, "linkGroup.add", {{"channels", {d}}}).at("group").at("name") == "Link Group 5");
	// A given name stays as it is, even one that is taken.
	CHECK(run(client, "linkGroup.add", {{"channels", {a}}, {"name", "Link Group 3"}}).at("group").at("name") ==
		"Link Group 3");
}

TEST_CASE("link groups and VCA tracks are saved and loaded with the project", "[integration]")
{
	const test::TempDir dir;
	const std::string file = (dir.path() / "linked.dawproject").u8string();
	Engine engine;
	Client client(engine);
	const ObjectId vca = add(client, "vca");
	const ObjectId a = add(client, "audio");
	const ObjectId b = add(client, "group");
	run(client, "channel.set", {{"channel", vca}, {"volume", -4.5}});
	run(client, "linkGroup.add", {{"channels", {a, b}}, {"vca", vca}, {"name", "Strings"}, {"mute", true}});
	run(client, "channel.set", {{"channel", vca}, {"monitor", true}, {"recordEnable", true}, {"listen", true}});
	const Json vcaBefore = channelJson(client, vca);
	const Json before = run(client, "project.get");
	run(client, "project.save", {{"path", file}});
	run(client, "project.new");
	run(client, "project.load", {{"path", file}});
	const Json after = run(client, "project.get");
	CHECK(after.at("linkGroups") == before.at("linkGroups"));
	CHECK(after.at("tracks").at(0).at("type") == "vca");
	CHECK(channelJson(client, vca).at("volume") == -4.5);
	CHECK(channelJson(client, vca) == vcaBefore); // its buttons too
	CHECK(channelJson(client, a).at("monitor") == true);
}

TEST_CASE("a VCA fader adds its volume to its channels', through nested VCA faders", "[integration]")
{
	Mixer mixer;
	const ObjectId vca = add(mixer.client, "vca");
	const ObjectId nested = add(mixer.client, "vca");
	run(mixer.client, "linkGroup.add", {{"channels", {mixer.track}}, {"vca", nested}});
	run(mixer.client, "linkGroup.add", {{"channels", {nested}}, {"vca", vca}});
	// The reference manual's example (p. 518): the channel at -3 dB, the nested VCA fader at -10 dB, the main one
	// from 0 to +4 dB: the channel plays at -9 dB.
	run(mixer.client, "channel.set", {{"channel", mixer.track}, {"volume", -3.0}});
	run(mixer.client, "channel.set", {{"channel", nested}, {"volume", -10.0}});
	run(mixer.client, "channel.set", {{"channel", vca}, {"volume", 4.0}});
	CHECK(channelJson(mixer.client, mixer.track).at("volume") == -3.0); // the channel's own fader stays
	run(mixer.client, "transport.play");
	checkGain(mixer.render(2000), [](int64_t) { return gain(-9.0); }, 1e-6);
}

TEST_CASE("VCA fader automation combines with the channels' own automation", "[integration]")
{
	SECTION("a VCA ramp over a static channel fader")
	{
		Mixer mixer;
		const ObjectId vca = add(mixer.client, "vca");
		run(mixer.client, "linkGroup.add", {{"channels", {mixer.track}}, {"vca", vca}});
		run(mixer.client, "channel.set", {{"channel", mixer.track}, {"volume", -6.0}});
		mixer.automate(vca,
			{{{"position", 500}, {"value", 0.0}}, {{"position", 2500}, {"value", -12.0}, {"shape", "step"}},
				{{"position", 3000}, {"value", 3.0}}});
		run(mixer.client, "transport.play");
		checkGain(
			mixer.render(4000),
			[](int64_t at)
			{
				if (at >= 3000)
					return gain(-6.0) * gain(3.0);
				return gain(-6.0) * (at < 2500 ? ramp(at, 500, 0.0, 2500, -12.0) : gain(-12.0));
			},
			1e-6);
	}
	SECTION("a VCA ramp over a channel's own ramp")
	{
		Mixer mixer(100);
		const ObjectId vca = add(mixer.client, "vca");
		run(mixer.client, "linkGroup.add", {{"channels", {mixer.track}}, {"vca", vca}});
		run(mixer.client, "channel.set", {{"channel", vca}, {"volume", -20.0}}); // automation overrides it
		mixer.automate(vca, {{{"position", 0}, {"value", 0.0}}, {{"position", 4000}, {"value", -12.0}}});
		mixer.automate(mixer.track, {{{"position", 1000}, {"value", 0.0}}, {{"position", 3000}, {"value", -20.0}}});
		run(mixer.client, "transport.play");
		// Where both move, the product plays in straight pieces: close to the exact one.
		checkGain(
			mixer.render(4500),
			[](int64_t at) { return ramp(at, 0, 0.0, 4000, -12.0) * ramp(at, 1000, 0.0, 3000, -20.0); }, 1e-3);
	}
	SECTION("virgin territory: the gaps of both play their static volumes")
	{
		Mixer mixer;
		const ObjectId vca = add(mixer.client, "vca");
		run(mixer.client, "linkGroup.add", {{"channels", {mixer.track}}, {"vca", vca}});
		run(mixer.client, "automation.set", {{"virginTerritory", true}});
		run(mixer.client, "channel.set", {{"channel", vca}, {"volume", -6.0}});
		mixer.automate(vca, {{{"position", 1000}, {"value", 0.0}}, {{"position", 2000}, {"value", 0.0}}});
		mixer.automate(mixer.track, {{{"position", 500}, {"value", -3.0}}, {{"position", 3500}, {"value", -3.0}}});
		run(mixer.client, "transport.play");
		checkGain(
			mixer.render(4000),
			[](int64_t at)
			{
				const double own = at < 500 || at >= 3500 ? 0.0 : -3.0;
				const double fader = at < 1000 || at >= 2000 ? -6.0 : 0.0;
				return gain(own + fader);
			},
			1e-6);
	}
}
