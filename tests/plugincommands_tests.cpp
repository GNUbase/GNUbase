// SPDX-License-Identifier: GPL-3.0-or-later
// Plugin slots in the document (#282): plugin.insert, plugin.remove, plugin.move, plugin.set, plugin.list and
// plugin.get, the parameters of the plugins in them, and how they are saved.
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/processors/gainpan.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>
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

	ObjectId insert(Client& client, ObjectId channel, const char* plugin, Json index = nullptr)
	{
		Json parameters = {{"channel", channel}, {"plugin", plugin}};
		if (!index.is_null())
			parameters["index"] = std::move(index);
		return run(client, "plugin.insert", parameters).at("slot").at("id").get<ObjectId>();
	}

	// The ids of a channel's slots, from top to bottom.
	std::vector<ObjectId> slotIds(Client& client, ObjectId channel)
	{
		std::vector<ObjectId> result;
		const Json slots = run(client, "plugin.list", {{"channel", channel}}).at("slots");
		for (const Json& slot : slots)
			result.push_back(slot.at("id").get<ObjectId>());
		return result;
	}

	Json pluginTarget(ObjectId slot, uint32_t parameter)
	{
		return {{"type", "plugin"}, {"slot", slot}, {"parameter", parameter}};
	}

	// The data of the events since the last call, by name.
	std::map<std::string, std::vector<Json>> events(Client& client)
	{
		std::map<std::string, std::vector<Json>> found;
		while (const auto message = client.poll())
			if (const auto* event = std::get_if<Event>(&*message))
				found[event->name].push_back(event->data);
		return found;
	}

	std::vector<std::string> keys(const Json& object)
	{
		std::vector<std::string> result;
		for (const auto& item : object.items())
			result.push_back(item.key());
		return result;
	}
}

TEST_CASE("plugin.insert puts a plugin into a channel's slot and moves the slots below down", "[unit]")
{
	Engine engine;
	Client client(engine);
	const TestTrack audio = addTrack(client, "audio");

	const Json first = run(client, "plugin.insert", {{"channel", audio.channel}, {"plugin", "builtin:eq"}}).at("slot");
	CHECK(first.at("channel") == audio.channel);
	CHECK(first.at("index") == 0);
	CHECK(first.at("plugin") == "builtin:eq");
	CHECK(first.at("name") == "Parametric EQ");
	CHECK(first.at("bypass") == false);
	CHECK(first.at("active") == true);
	const ObjectId eq = first.at("id").get<ObjectId>();
	const ObjectId gate = insert(client, audio.channel, "builtin:gate");
	const ObjectId limiter = insert(client, audio.channel, "builtin:limiter", 1);
	CHECK(slotIds(client, audio.channel) == std::vector<ObjectId>{eq, limiter, gate});
	CHECK(run(client, "plugin.get", {{"slot", gate}}).at("slot").at("index") == 2);
	CHECK(run(client, "project.get").at("channels").at(0).at("slots") == Json::array({eq, limiter, gate}));
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Insert plugin");

	// Undo takes the plugin out again, redo puts it back with the same id.
	run(client, "history.undo");
	CHECK(slotIds(client, audio.channel) == std::vector<ObjectId>{eq, gate});
	run(client, "history.redo");
	CHECK(slotIds(client, audio.channel) == std::vector<ObjectId>{eq, limiter, gate});

	// Group, effect and instrument channels have slots too; MIDI channels have none.
	for (const char* type : {"group", "effect", "instrument"})
		insert(client, addTrack(client, type).channel, "builtin:eq");
	const TestTrack midi = addTrack(client, "midi");

	// Wrong arguments change nothing.
	const Json before = run(client, "project.get");
	const auto refused = [&](const Json& arguments, const std::string& expected)
	{ CHECK_THAT(reject(client, "plugin.insert", arguments), ContainsSubstring(expected)); };
	refused({{"channel", midi.channel}, {"plugin", "builtin:eq"}}, "MIDI");
	refused({{"channel", 9999}, {"plugin", "builtin:eq"}}, "9999");
	refused({{"channel", audio.channel}, {"plugin", "builtin:nothing"}}, "builtin:nothing");
	refused({{"channel", audio.channel}, {"plugin", "eq"}}, "eq");
	refused({{"channel", audio.channel}, {"plugin", "builtin:testSynth"}}, "instrument");
	refused({{"channel", audio.channel}, {"plugin", "builtin:eq"}, {"index", 4}}, "index");
	refused({{"channel", audio.channel}, {"plugin", "builtin:eq"}, {"index", -1}}, "index");
	// Other formats come with the plugin scan; until then, no plugin of theirs is known.
	refused({{"channel", audio.channel}, {"plugin", "vst3:0123456789ABCDEF0123456789ABCDEF"}}, "vst3:");
	CHECK(run(client, "project.get") == before);

	// A channel has at most 16 slots.
	for (int i = 3; i < 16; ++i)
		insert(client, audio.channel, "builtin:gainPan");
	CHECK_THAT(reject(client, "plugin.insert", {{"channel", audio.channel}, {"plugin", "builtin:eq"}}),
		ContainsSubstring("16"));
}

TEST_CASE("plugin.move and plugin.remove rearrange slots within and between channels", "[unit]")
{
	Engine engine;
	Client client(engine);
	const TestTrack audio = addTrack(client, "audio");
	const TestTrack group = addTrack(client, "group");
	const ObjectId a = insert(client, audio.channel, "builtin:eq");
	const ObjectId b = insert(client, audio.channel, "builtin:gate");
	const ObjectId c = insert(client, audio.channel, "builtin:limiter");

	// The index counts the slots without the moved one, as track.move's does.
	run(client, "plugin.move", {{"slot", a}, {"index", 2}});
	CHECK(slotIds(client, audio.channel) == std::vector<ObjectId>{b, c, a});
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Move plugin");
	run(client, "plugin.move", {{"slot", c}, {"channel", group.channel}, {"index", 0}});
	CHECK(slotIds(client, audio.channel) == std::vector<ObjectId>{b, a});
	CHECK(slotIds(client, group.channel) == std::vector<ObjectId>{c});
	CHECK(run(client, "plugin.get", {{"slot", c}}).at("slot").at("channel") == group.channel);
	run(client, "history.undo");
	CHECK(slotIds(client, audio.channel) == std::vector<ObjectId>{b, c, a});
	CHECK(slotIds(client, group.channel).empty());

	run(client, "plugin.remove", {{"slot", b}});
	CHECK(slotIds(client, audio.channel) == std::vector<ObjectId>{c, a});
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Remove plugin");
	CHECK_THAT(reject(client, "plugin.get", {{"slot", b}}), ContainsSubstring("no slot"));
	run(client, "history.undo");
	CHECK(slotIds(client, audio.channel) == std::vector<ObjectId>{b, c, a});

	const Json before = run(client, "project.get");
	CHECK_THAT(reject(client, "plugin.move", {{"slot", a}, {"index", 3}}), ContainsSubstring("index"));
	CHECK_THAT(reject(client, "plugin.move", {{"slot", a}, {"channel", group.channel}, {"index", 1}}),
		ContainsSubstring("index"));
	const TestTrack midi = addTrack(client, "midi");
	const Json withMidi = run(client, "project.get");
	CHECK_THAT(reject(client, "plugin.move", {{"slot", a}, {"channel", midi.channel}, {"index", 0}}),
		ContainsSubstring("MIDI"));
	CHECK_THAT(reject(client, "plugin.remove", {{"slot", 9999}}), ContainsSubstring("9999"));
	CHECK(run(client, "project.get") == withMidi);
	run(client, "track.remove", {{"track", midi.track}});
	CHECK(run(client, "project.get").at("slots") == before.at("slots"));
}

TEST_CASE("plugin.set bypasses or deactivates a slot's plugin as one undo step", "[unit]")
{
	Engine engine;
	Client client(engine);
	const TestTrack audio = addTrack(client, "audio");
	const ObjectId eq = insert(client, audio.channel, "builtin:eq");

	Json slot = run(client, "plugin.set", {{"slot", eq}, {"bypass", true}}).at("slot");
	CHECK(slot.at("bypass") == true);
	CHECK(slot.at("active") == true);
	CHECK(run(client, "history.get").at("steps").back().at("action") == "Change plugin");
	slot = run(client, "plugin.set", {{"slot", eq}, {"active", false}}).at("slot");
	CHECK(slot.at("active") == false);
	while (client.poll())
	{
	}
	run(client, "history.undo");
	CHECK(run(client, "plugin.get", {{"slot", eq}}).at("slot").at("active") == true);
	// Undo announces the slot as plugin.set does.
	std::optional<Json> announced;
	while (const auto message = client.poll())
		if (const auto* event = std::get_if<Event>(&*message); event != nullptr && event->name == "plugin.changed")
			announced = event->data.at("slot");
	REQUIRE(announced.has_value());
	const Json changed = announced.value_or(Json());
	CHECK(changed.at("id") == eq);
	CHECK(changed.at("active") == true);
	CHECK(changed.at("index") == 0);
	CHECK_THAT(reject(client, "plugin.set", {{"slot", 9999}, {"bypass", true}}), ContainsSubstring("9999"));
}

TEST_CASE("the parameters of a slot's plugin are a channel's parameters", "[unit]")
{
	Engine engine;
	Client client(engine);
	const TestTrack audio = addTrack(client, "audio");
	const ObjectId gain = insert(client, audio.channel, "builtin:gainPan");
	const Json target = pluginTarget(gain, processors::GainPan::Gain);

	bool listed = false;
	const Json parameters = run(client, "parameter.list", {{"channel", audio.channel}}).at("parameters");
	for (const Json& parameter : parameters)
		if (parameter.at("target") == target)
		{
			listed = true;
			CHECK(parameter.at("name") == "Gain");
			CHECK(parameter.at("value") == 0.0);
		}
	CHECK(listed);

	const Json changed =
		run(client, "parameter.set", {{"channel", audio.channel}, {"target", target}, {"text", "-6 dB"}});
	CHECK(changed.at("parameter").at("value") == -6.0);
	CHECK(run(client, "plugin.get", {{"slot", gain}}).at("slot").at("parameters") ==
		Json::array({{{"parameter", processors::GainPan::Gain}, {"value", -6.0}}}));
	run(client, "history.undo");
	CHECK(run(client, "parameter.get", {{"channel", audio.channel}, {"target", target}}).at("parameter").at("value") ==
		0.0);

	// A slot of another channel, an unknown parameter, and automation, which comes with #70.
	const TestTrack other = addTrack(client, "audio");
	CHECK_THAT(
		reject(client, "parameter.get", {{"channel", other.channel}, {"target", target}}), ContainsSubstring("slot"));
	CHECK_THAT(reject(client, "parameter.get", {{"channel", audio.channel}, {"target", pluginTarget(gain, 77)}}),
		ContainsSubstring("77"));
	CHECK(!client.call("automationLane.add", {{"track", audio.track}, {"target", target}}).ok());
}

TEST_CASE("removing or duplicating a track removes or copies its slots", "[unit]")
{
	Engine engine;
	Client client(engine);
	const TestTrack audio = addTrack(client, "audio");
	const ObjectId gain = insert(client, audio.channel, "builtin:gainPan");
	run(client, "parameter.set",
		{{"channel", audio.channel}, {"target", pluginTarget(gain, processors::GainPan::Gain)}, {"value", -3.0}});
	run(client, "plugin.set", {{"slot", gain}, {"bypass", true}});

	const Json copy = run(client, "track.duplicate", {{"track", audio.track}}).at("track");
	const ObjectId copyChannel = copy.at("channel").get<ObjectId>();
	const Json copied = run(client, "plugin.list", {{"channel", copyChannel}}).at("slots");
	REQUIRE(copied.size() == 1);
	CHECK(copied[0].at("id") != gain);
	CHECK(copied[0].at("channel") == copyChannel);
	CHECK(copied[0].at("bypass") == true);
	CHECK(copied[0].at("parameters") == run(client, "plugin.get", {{"slot", gain}}).at("slot").at("parameters"));

	run(client, "track.remove", {{"track", audio.track}});
	CHECK_THAT(reject(client, "plugin.get", {{"slot", gain}}), ContainsSubstring("no slot"));
	CHECK(run(client, "project.get").at("slots").size() == 1);
	run(client, "history.undo");
	CHECK(run(client, "plugin.get", {{"slot", gain}}).at("slot").at("bypass") == true);
}

TEST_CASE("slots are saved with their plugin's state, and files from before them or newer builds load", "[unit]")
{
	Engine engine;
	Client client(engine);
	const TestTrack audio = addTrack(client, "audio");
	const ObjectId gain = insert(client, audio.channel, "builtin:gainPan");
	run(client, "parameter.set",
		{{"channel", audio.channel}, {"target", pluginTarget(gain, processors::GainPan::Gain)}, {"value", -3.0}});
	const Json saved = run(client, "project.get");
	CHECK(toJson(projectFromJson(saved)) == saved);

	const gnubase::test::TempDir dir;
	const std::string file = (dir.path() / "slots.dawproject").u8string();
	run(client, "project.save", {{"path", file}});
	run(client, "project.new");
	run(client, "project.load", {{"path", file}});
	CHECK(run(client, "project.get").at("slots") == saved.at("slots"));

	// A state from a file (here one of another application's plugin) is kept as it is, base64 in the project.
	Json withState = saved;
	withState["slots"][0]["plugin"] = "vst3:0123456789ABCDEF0123456789ABCDEF";
	withState["slots"][0]["state"] = "AAEC/w==";
	const Project loaded = projectFromJson(withState);
	const Slot& slot = loaded.slots.at(gain);
	REQUIRE(slot.state != nullptr);
	CHECK(*slot.state == std::vector<uint8_t>{0, 1, 2, 255});
	CHECK(toJson(loaded) == withState);

	// A file from before slots, and a plugin this build doesn't know, which stays in its slot.
	Json old = saved;
	old.erase("slots");
	old["channels"][0].erase("slots");
	CHECK(projectFromJson(old).slots.empty());
	Json unknown = saved;
	unknown["slots"][0]["plugin"] = "future:plugin";
	CHECK(projectFromJson(unknown).slots.at(gain).plugin == "future:plugin");
	unknown["slots"][0]["plugin"] = "builtin:future";
	CHECK(toJson(projectFromJson(unknown)) == unknown); // with its parameter values

	const auto refused = [&saved](const std::function<void(Json&)>& change)
	{
		Json bad = saved;
		change(bad);
		CHECK_THROWS_AS(projectFromJson(bad), std::invalid_argument);
	};
	refused([](Json& p) { p["slots"][0]["channel"] = 9999; });
	refused([](Json& p) { p["channels"][0]["slots"] = Json::array({9999}); });
	refused([](Json& p) { p["channels"][0]["slots"].push_back(p["channels"][0]["slots"][0]); });
	refused([](Json& p) { p["slots"][0]["plugin"] = "noFormat"; });
	refused([](Json& p) { p["slots"][0]["state"] = "not base64!"; });
	refused([](Json& p) { p["slots"][0]["parameters"][0]["value"] = std::numeric_limits<double>::infinity(); });
	refused([](Json& p) { p["slots"][0]["plugin"] = "builtin:testSynth"; }); // an instrument
	refused([](Json& p) { p["slots"].push_back(p["slots"][0]); });           // the same slot twice
	refused(
		[](Json& p)
		{
			Json extra = p["slots"][0];
			extra["id"] = 9000;
			p["slots"].push_back(extra); // a slot no channel lists
		});
	refused(
		[](Json& p)
		{
			// 17 slots
			for (int i = 0; i < 16; ++i)
			{
				Json extra = p["slots"][0];
				extra["id"] = 9000 + i;
				p["slots"].push_back(extra);
				p["channels"][0]["slots"].push_back(9000 + i);
			}
		});
	Json sixteen = saved; // the most a channel has
	for (int i = 0; i < 15; ++i)
	{
		Json extra = sixteen["slots"][0];
		extra["id"] = 9000 + i;
		sixteen["slots"].push_back(extra);
		sixteen["channels"][0]["slots"].push_back(9000 + i);
	}
	CHECK(projectFromJson(sixteen).channels.at(audio.channel).slots.size() == 16);
}

TEST_CASE("plugin states of any length are saved as base64, and texts that aren't base64 are refused", "[unit]")
{
	Engine engine;
	Client client(engine);
	const TestTrack audio = addTrack(client, "audio");
	insert(client, audio.channel, "builtin:gainPan");
	const Json saved = run(client, "project.get");

	const std::vector<std::string> texts{"", "AA==", "AAE=", "AAEC", "AAECAw==", "AAECAwQ="};
	for (size_t length = 0; length < texts.size(); ++length)
	{
		Json withState = saved;
		withState["slots"][0]["state"] = texts[length];
		const Project loaded = projectFromJson(withState);
		const Slot& slot = loaded.slots.begin()->second;
		REQUIRE(slot.state != nullptr);
		std::vector<uint8_t> expected(length);
		for (size_t i = 0; i < length; ++i)
			expected[i] = static_cast<uint8_t>(i);
		CHECK(*slot.state == expected);
		CHECK(toJson(loaded) == withState);
	}
	for (const char* text : {"A", "AA", "AAA", "AA=", "A===", "=AAA", "AA=A", "AA==AA==", "AB==", "AAF=", "AA\nA"})
	{
		INFO(text);
		Json bad = saved;
		bad["slots"][0]["state"] = text;
		CHECK_THROWS_AS(projectFromJson(bad), std::invalid_argument);
	}
}

TEST_CASE("the plugin.* commands, parameter.set, undo and redo announce slots alike", "[unit]")
{
	Engine engine;
	Client client(engine);
	const TestTrack audio = addTrack(client, "audio");
	events(client);

	const ObjectId gain = insert(client, audio.channel, "builtin:gainPan");
	std::map<std::string, std::vector<Json>> announced = events(client);
	REQUIRE(announced["plugin.added"].size() == 1);
	CHECK(announced["plugin.added"][0].at("slot") == run(client, "plugin.get", {{"slot", gain}}).at("slot"));
	CHECK(announced["channel.changed"].size() == 1);

	// plugin.changed has the slot as plugin.get describes it, whatever changed it.
	run(client, "plugin.set", {{"slot", gain}, {"bypass", true}});
	const Json set = events(client)["plugin.changed"].at(0).at("slot");
	CHECK(set == run(client, "plugin.get", {{"slot", gain}}).at("slot"));
	run(client, "parameter.set",
		{{"channel", audio.channel}, {"target", pluginTarget(gain, processors::GainPan::Gain)}, {"value", -6.0}});
	const Json parameter = events(client)["plugin.changed"].at(0).at("slot");
	CHECK(keys(parameter) == keys(set));
	CHECK(parameter == run(client, "plugin.get", {{"slot", gain}}).at("slot"));
	for (const char* command : {"history.undo", "history.undo", "history.redo", "history.redo"})
	{
		INFO(command);
		run(client, command);
		const std::vector<Json> changed = events(client)["plugin.changed"];
		REQUIRE(changed.size() == 1);
		CHECK(keys(changed[0].at("slot")) == keys(set));
		CHECK(changed[0].at("slot") == run(client, "plugin.get", {{"slot", gain}}).at("slot"));
	}

	// Removing it, and undo and redo of that.
	run(client, "plugin.remove", {{"slot", gain}});
	announced = events(client);
	REQUIRE(announced["plugin.removed"].size() == 1);
	CHECK(announced["plugin.removed"][0].at("slot") == gain);
	CHECK(announced["channel.changed"].size() == 1);
	run(client, "history.undo");
	announced = events(client);
	REQUIRE(announced["plugin.added"].size() == 1);
	CHECK(announced["plugin.added"][0].at("slot") == run(client, "plugin.get", {{"slot", gain}}).at("slot"));
	run(client, "history.redo");
	CHECK(events(client)["plugin.removed"].size() == 1);

	// The slots of a track that goes or comes back are in its channel's events.
	run(client, "history.undo");
	run(client, "track.remove", {{"track", audio.track}});
	announced = events(client);
	CHECK(announced["plugin.removed"].empty());
	run(client, "history.undo");
	announced = events(client);
	CHECK(announced["plugin.added"].empty());
	CHECK(!announced["channel.added"].empty());
}
