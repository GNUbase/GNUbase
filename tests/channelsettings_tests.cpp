// SPDX-License-Identifier: GPL-3.0-or-later
// The channel settings window (#112, docs/features/channel-settings.md): copying and pasting a channel's settings and
// link groups that link them in the engine, and the window in the app, driven with ui::Automation as MCP's UI tools
// drive it.
#include "engine/engine.h"
#include "ui/automation.h"
#include "uiapp.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace gnubase;
using Catch::Matchers::ContainsSubstring;
using engine::Client;
using engine::Json;
using engine::ObjectId;
using engine::Result;

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

	ObjectId add(Client& client, const std::string& type)
	{
		return run(client, "track.add", {{"type", type}}).at("channel").at("id").get<ObjectId>();
	}

	Json channelJson(Client& client, ObjectId id)
	{
		const Json project = run(client, "project.get");
		for (const Json& item : project.at("channels"))
			if (item.at("id") == id)
				return item;
		FAIL("no channel " << id);
	}

	// The plugins in a channel's slots, from top to bottom.
	std::vector<std::string> plugins(Client& client, ObjectId channel)
	{
		std::vector<std::string> result;
		const Json slots = run(client, "plugin.list", {{"channel", channel}}).at("slots");
		for (const Json& slot : slots)
			result.push_back(slot.at("plugin").get<std::string>());
		return result;
	}

	// What channel settings copy and paste, from a channel's JSON.
	Json settings(const Json& channel)
	{
		return {{"pre", channel.at("pre")}, {"eq", channel.at("eq")}, {"strip", channel.at("strip")},
			{"sends", channel.at("sends")}};
	}
}

TEST_CASE(
	"channel.copySettings and channel.pasteSettings copy the sections, sends and inserts in one undo step", "[unit]")
{
	engine::Engine engine;
	Client client(engine);
	const ObjectId source = add(client, "audio");
	const ObjectId target = add(client, "audio");
	const ObjectId other = add(client, "instrument");
	const ObjectId effect = add(client, "effect");
	const ObjectId midi = add(client, "midi");
	CHECK_THAT(reject(client, "channel.pasteSettings", {{"channels", {target}}}), ContainsSubstring("nothing"));

	run(client, "channel.set",
		{{"channel", source}, {"volume", -12.0}, {"pre", {{"gain", 6.0}, {"phase", true}}},
			{"eq", {{"bands", {{{"band", 2}, {"on", true}, {"gain", -4.5}}}}}},
			{"strip", {{"compressor", {{"on", true}, {"ratio", 8.0}}}}},
			{"sends", {{{"send", 1}, {"destination", effect}, {"on", true}, {"level", -3.0}}}}});
	run(client, "plugin.insert", {{"channel", source}, {"plugin", "builtin:eq"}});
	run(client, "plugin.insert", {{"channel", source}, {"plugin", "builtin:gainPan"}});
	run(client, "plugin.insert", {{"channel", target}, {"plugin", "builtin:limiter"}});
	run(client, "channel.copySettings", {{"channel", source}});
	CHECK_THAT(reject(client, "channel.pasteSettings", {{"channels", Json::array()}}),
		ContainsSubstring("parameters.channels must have at least 1 item"));
	// What is pasted is what was copied: later changes of the source don't count.
	run(client, "channel.set", {{"channel", source}, {"pre", {{"gain", -6.0}}}});

	const Json pasted = run(client, "channel.pasteSettings", {{"channels", {target, other}}});
	CHECK(pasted.at("channels") == Json({target, other}));
	for (const ObjectId id : {target, other})
	{
		const Json channel = channelJson(client, id);
		CHECK(channel.at("pre").at("gain") == 6.0);
		CHECK(channel.at("pre").at("phase") == true);
		CHECK(channel.at("eq").at("bands").at(1).at("gain") == -4.5);
		CHECK(channel.at("strip").at("compressor").at("ratio") == 8.0);
		CHECK(channel.at("sends").at(0).at("destination") == effect);
		CHECK(channel.at("sends").at(0).at("level") == -3.0);
		// The fader stays the channel's own.
		CHECK(channel.at("volume") == 0.0);
		// The inserts are copies of the source's, in their order, instead of the channel's own.
		CHECK(plugins(client, id) == std::vector<std::string>{"builtin:eq", "builtin:gainPan"});
	}
	CHECK(channelJson(client, target).at("slots") != channelJson(client, source).at("slots"));

	// One undo step takes it all back.
	run(client, "history.undo");
	CHECK(channelJson(client, target).at("pre").at("gain") == 0.0);
	CHECK(plugins(client, target) == std::vector<std::string>{"builtin:limiter"});
	CHECK(plugins(client, other).empty());

	// A send can't go to the channel itself: pasted onto the effect channel, it keeps its settings without the
	// destination.
	run(client, "channel.pasteSettings", {{"channels", {effect}}});
	CHECK(channelJson(client, effect).at("sends").at(0).at("destination") == 0);
	CHECK(channelJson(client, effect).at("sends").at(0).at("level") == -3.0);
	CHECK(settings(channelJson(client, effect)).at("eq") == settings(channelJson(client, source)).at("eq"));

	// MIDI channels have no such settings.
	CHECK_THAT(reject(client, "channel.copySettings", {{"channel", midi}}), ContainsSubstring("MIDI"));
	CHECK_THAT(reject(client, "channel.pasteSettings", {{"channels", {target, midi}}}), ContainsSubstring("MIDI"));
	CHECK_THAT(reject(client, "channel.pasteSettings", {{"channels", {12345}}}), ContainsSubstring("no channel"));
}

TEST_CASE("channel.pasteSettings removes the replaced plugins' automation and drops sends that would loop or that "
		  "another project copied",
	"[unit]")
{
	engine::Engine engine;
	Client client(engine);
	const ObjectId source = add(client, "audio");
	const Json target = run(client, "track.add", {{"type", "audio"}});
	const auto targetChannel = target.at("channel").at("id").get<ObjectId>();
	const ObjectId first = add(client, "effect");
	const ObjectId second = add(client, "effect");

	// The automation of the plugins a paste replaces goes with them, announced as plugin.remove announces it.
	const ObjectId slot = run(client, "plugin.insert", {{"channel", targetChannel}, {"plugin", "builtin:gainPan"}})
							  .at("slot")
							  .at("id")
							  .get<ObjectId>();
	const ObjectId lane = run(client, "automationLane.add",
		{{"track", target.at("track").at("id")}, {"target", {{"type", "plugin"}, {"slot", slot}, {"parameter", 0}}}})
							  .at("lane")
							  .at("id")
							  .get<ObjectId>();
	while (client.poll())
	{
	}
	run(client, "channel.copySettings", {{"channel", source}});
	run(client, "channel.pasteSettings", {{"channels", {targetChannel}}});
	std::vector<std::string> names;
	while (const auto message = client.poll())
		if (const auto* event = std::get_if<engine::Event>(&*message); event != nullptr)
		{
			names.push_back(event->name);
			if (event->name == "automationLane.removed")
				CHECK(event->data.at("lane") == lane);
		}
	CHECK(std::count(names.begin(), names.end(), "automationLane.removed") == 1);
	CHECK(std::find(names.begin(), names.end(), "automationLane.removed") <
		std::find(names.begin(), names.end(), "plugin.removed"));
	CHECK(run(client, "project.get").at("automation").empty());

	// A send to the first effect channel, pasted onto the second one, which the first one sends to, would close a
	// loop through the first: it keeps its level without the destination.
	run(client, "channel.set", {{"channel", first}, {"sends", {{{"send", 1}, {"destination", second}}}}});
	run(client, "channel.set",
		{{"channel", source}, {"sends", {{{"send", 1}, {"destination", first}, {"level", -6.0}}}}});
	run(client, "channel.copySettings", {{"channel", source}});
	run(client, "channel.pasteSettings", {{"channels", {targetChannel, second}}});
	CHECK(channelJson(client, targetChannel).at("sends").at(0).at("destination") == first);
	CHECK(channelJson(client, second).at("sends").at(0).at("destination") == 0);
	CHECK(channelJson(client, second).at("sends").at(0).at("level") == -6.0);

	// Ids start again in another project, so a destination copied in one project means nothing in the next: the
	// sends lose it there, even where the new project has a channel of that id, and a level above the new project's
	// fader top comes down to it; the rest is pasted.
	run(client, "project.set", {{"maxVolume", 12.0}});
	run(client, "channel.set", {{"channel", source}, {"sends", {{{"send", 1}, {"level", 10.0}}}}});
	run(client, "channel.copySettings", {{"channel", source}});
	run(client, "project.new");
	const ObjectId other = add(client, "audio");
	add(client, "effect");
	CHECK(add(client, "effect") == first);
	run(client, "channel.pasteSettings", {{"channels", {other}}});
	CHECK(channelJson(client, other).at("sends").at(0).at("destination") == 0);
	CHECK(channelJson(client, other).at("sends").at(0).at("level") == 6.0);
}

TEST_CASE("link groups link the equalizers, the dynamics and the sends", "[unit]")
{
	engine::Engine engine;
	Client client(engine);
	const ObjectId a = add(client, "audio");
	const ObjectId b = add(client, "audio");
	const ObjectId effect = add(client, "effect");

	// By default a link group links the sends (reference manual p. 458), not the equalizers and the dynamics.
	const Json group = run(client, "linkGroup.add", {{"channels", {a, b, effect}}}).at("group");
	CHECK(group.at("sends") == true);
	CHECK(group.at("eq") == false);
	CHECK(group.at("dynamics") == false);
	run(client, "channel.set",
		{{"channel", a}, {"eq", {{"bands", {{{"band", 1}, {"on", true}}}}}}, {"strip", {{"gate", {{"on", true}}}}}});
	CHECK(channelJson(client, b).at("eq").at("bands").at(0).at("on") == false);
	CHECK(channelJson(client, b).at("strip").at("gate").at("on") == false);

	// A send changes on the linked channels in the same undo step; the effect channel can't send to itself, so it
	// keeps its own.
	run(client, "channel.set", {{"channel", a}, {"sends", {{{"send", 2}, {"destination", effect}, {"on", true}}}}});
	CHECK(channelJson(client, b).at("sends").at(1).at("destination") == effect);
	CHECK(channelJson(client, b).at("sends").at(1).at("on") == true);
	CHECK(channelJson(client, effect).at("sends").at(1).at("destination") == 0);
	run(client, "history.undo");
	CHECK(channelJson(client, b).at("sends").at(1).at("destination") == 0);

	// Nor does a send that would close a loop through another channel: the second effect channel sends to the effect
	// channel, so the effect channel can't send to it.
	const ObjectId second = add(client, "effect");
	run(client, "channel.set", {{"channel", second}, {"sends", {{{"send", 1}, {"destination", effect}}}}});
	run(client, "channel.set", {{"channel", a}, {"sends", {{{"send", 4}, {"destination", second}, {"on", true}}}}});
	CHECK(channelJson(client, b).at("sends").at(3).at("destination") == second);
	CHECK(channelJson(client, effect).at("sends").at(3).at("destination") == 0);
	CHECK(channelJson(client, effect).at("sends").at(3).at("on") == false);

	// The equalizers and the dynamics: what changed on one channel changes on the others, the rest stays theirs.
	run(client, "linkGroup.set", {{"group", group.at("id")}, {"eq", true}, {"dynamics", true}, {"sends", false}});
	run(client, "channel.set", {{"channel", b}, {"eq", {{"bands", {{{"band", 3}, {"gain", 5.0}}}}}}});
	run(client, "channel.set", {{"channel", a}, {"strip", {{"compressor", {{"threshold", -30.0}}}}}});
	for (const ObjectId id : {a, b, effect})
	{
		CHECK(channelJson(client, id).at("eq").at("bands").at(2).at("gain") == 5.0);
		CHECK(channelJson(client, id).at("strip").at("compressor").at("threshold") == -30.0);
	}
	CHECK(channelJson(client, a).at("eq").at("bands").at(0).at("on") == true);
	CHECK(channelJson(client, b).at("eq").at("bands").at(0).at("on") == false);
	run(client, "channel.set", {{"channel", a}, {"sends", {{{"send", 3}, {"on", true}}}}});
	CHECK(channelJson(client, b).at("sends").at(2).at("on") == false);
	// linked false changes only the channel.
	run(client, "channel.set", {{"channel", a}, {"eq", {{"bypass", true}}}, {"linked", false}});
	CHECK(channelJson(client, b).at("eq").at("bypass") == false);
}

namespace
{
	using Window = std::optional<ui::WindowId>;

	Json elements(test::UiApp& app, Window window, const std::string& selector)
	{
		return app.ui().dom(window, selector, 0).at("elements");
	}

	Json element(test::UiApp& app, Window window, const std::string& selector)
	{
		const Json found = elements(app, window, selector);
		REQUIRE(found.size() == 1);
		return found.at(0);
	}

	void click(test::UiApp& app, Window window, const std::string& selector)
	{
		app.ui().click(window, {selector, 0, std::nullopt}, ui::MouseButton::Left, 1, {});
		app.shell().step(std::chrono::milliseconds(0));
	}

	bool has(const Json& element, const std::string& name)
	{
		const Json names = element.value("classes", Json::array());
		return std::find(names.begin(), names.end(), name) != names.end();
	}

	std::vector<uint64_t> strips(test::UiApp& app, Window window)
	{
		std::vector<uint64_t> ids;
		for (const Json& strip : elements(app, window, ".mixer-channel"))
			ids.push_back(std::stoull(strip.at("attributes").at("channel").get<std::string>()));
		return ids;
	}

	struct Added
	{
		uint64_t track = 0;
		uint64_t channel = 0;
	};

	Added addTrack(test::UiApp& app, const std::string& type, const std::string& name)
	{
		const Json added = app.call("track.add", {{"type", type}, {"name", name}});
		return {added.at("track").at("id").get<uint64_t>(), added.at("channel").at("id").get<uint64_t>()};
	}

	Json channelJson(test::UiApp& app, uint64_t id)
	{
		const Json channels = app.call("project.get").at("channels");
		for (const Json& item : channels)
			if (item.at("id") == id)
				return item;
		FAIL("no channel " << id);
	}

	std::vector<uint64_t> selection(test::UiApp& app)
	{
		return app.appUi().trackList()->selection();
	}

	// Picks a function of the window's Functions menu.
	void function(test::UiApp& app, Window window, const std::string& name)
	{
		click(app, window, "#settings-functions");
		click(app, window, "#settings-functions-menu menuitem[function=" + name + "]");
	}

	// Selects tracks once the track list shows them.
	void select(test::UiApp& app, uint64_t track)
	{
		REQUIRE(app.waitUntil(
			[&]
			{
				app.appUi().trackList()->select({track});
				return selection(app) == std::vector<uint64_t>{track};
			}));
	}
}

TEST_CASE("the channel settings window shows a channel with its sections, goes through the channels and follows the "
		  "track selection",
	"[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	const Added guitar = addTrack(app, "audio", "Guitar");
	const Added keys = addTrack(app, "midi", "Keys");
	const Added bass = addTrack(app, "audio", "Bass");
	const Added drums = addTrack(app, "group", "Drums Bus");

	// Edit Channel Settings on a mixer strip (reference manual p. 500) opens the window for that channel and selects
	// its track.
	app.keys().run("window.mixer", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&] { return app.appUi().mixer(1) != nullptr; }));
	const Window mixer = app.appUi().mixerWindow(1);
	const std::string edit = ".mixer-channel[channel=" + std::to_string(guitar.channel) + "] button.mixer-edit";
	REQUIRE(app.waitUntil([&] { return elements(app, mixer, edit).size() == 1; }));
	click(app, mixer, edit);
	REQUIRE(app.waitUntil([&] { return app.appUi().channelSettings() != nullptr; }));
	const Window window = app.appUi().channelSettingsWindow();
	REQUIRE(app.waitUntil([&] { return strips(app, window) == std::vector<uint64_t>{guitar.channel}; }));
	CHECK(element(app, window, ".settings-name").at("text") == "Guitar");
	CHECK(selection(app) == std::vector<uint64_t>{guitar.track});
	// Every section of the channel shows, but for the pictures and the notepad.
	for (const char* section : {"pre", "eq", "strip", "sends", "fader"})
		CHECK(!has(element(app, window, std::string("[section=") + section + "]"), "hidden"));
	for (const char* section : {"picture", "notes"})
		CHECK(has(element(app, window, std::string("[section=") + section + "]"), "hidden"));
	CHECK(elements(app, window, "button.mixer-edit").empty());

	// The controls send channel.set.
	click(app, window, "[param=\"eq.bands.1.on\"]");
	REQUIRE(app.waitUntil([&] { return channelJson(app, guitar.channel).at("eq").at("bands")[0].at("on") == true; }));

	// The previous and the next mixer channel, which the track list selects too.
	click(app, window, "#settings-next");
	REQUIRE(app.waitUntil([&] { return strips(app, window) == std::vector<uint64_t>{keys.channel}; }));
	CHECK(selection(app) == std::vector<uint64_t>{keys.track});
	click(app, window, "#settings-next");
	click(app, window, "#settings-next");
	REQUIRE(app.waitUntil([&] { return strips(app, window) == std::vector<uint64_t>{drums.channel}; }));
	CHECK(element(app, window, "#settings-next").at("attributes").contains("disabled"));
	click(app, window, "#settings-previous");
	REQUIRE(app.waitUntil([&] { return strips(app, window) == std::vector<uint64_t>{bass.channel}; }));

	// Go to Output: a channel that plays into a group channel.
	app.call("channel.set", {{"channel", bass.channel}, {"output", drums.channel}});
	REQUIRE(app.waitUntil([&] { return element(app, window, "#settings-output").at("text") == "Drums Bus"; }));
	click(app, window, "#settings-output");
	REQUIRE(app.waitUntil([&] { return strips(app, window) == std::vector<uint64_t>{drums.channel}; }));

	// It follows the track selection, unless Follow is off.
	app.appUi().trackList()->select({guitar.track});
	REQUIRE(app.waitUntil([&] { return strips(app, window) == std::vector<uint64_t>{guitar.channel}; }));
	CHECK(element(app, window, "#settings-functions-menu menuitem[function=follow]")
			.at("attributes")
			.contains("checked"));
	function(app, window, "follow");
	app.appUi().trackList()->select({bass.track});
	app.shell().step(std::chrono::milliseconds(0));
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(strips(app, window) == std::vector<uint64_t>{guitar.channel});
	click(app, window, "#settings-next");
	REQUIRE(app.waitUntil([&] { return strips(app, window) == std::vector<uint64_t>{keys.channel}; }));
	CHECK(selection(app) == std::vector<uint64_t>{bass.track});

	// Edit Channel Settings as a UI command shows the first selected track's channel.
	app.keys().run("window.channelSettings", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&] { return strips(app, window) == std::vector<uint64_t>{bass.channel}; }));

	// A removed track's channel leaves the window empty.
	app.call("track.remove", {{"track", bass.track}});
	REQUIRE(app.waitUntil([&] { return strips(app, window).empty(); }));
}

TEST_CASE("the channel settings window copies and pastes channel settings and adds an effect track to a send", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	const Added guitar = addTrack(app, "audio", "Guitar");
	const Added bass = addTrack(app, "audio", "Bass");
	const Added drums = addTrack(app, "audio", "Drums");
	app.call("channel.set",
		{{"channel", guitar.channel}, {"eq", {{"bands", {{{"band", 2}, {"on", true}, {"gain", 6.0}}}}}}});
	REQUIRE(app.appUi().openChannelSettings(guitar.track) != nullptr);
	const Window window = app.appUi().channelSettingsWindow();
	REQUIRE(app.waitUntil([&] { return strips(app, window) == std::vector<uint64_t>{guitar.channel}; }));

	// Copy Channel Settings, then Paste Channel Settings onto the next channel (reference manual p. 461).
	function(app, window, "copy");
	click(app, window, "#settings-next");
	REQUIRE(app.waitUntil([&] { return strips(app, window) == std::vector<uint64_t>{bass.channel}; }));
	function(app, window, "paste");
	REQUIRE(app.waitUntil([&] { return channelJson(app, bass.channel).at("eq").at("bands")[1].at("gain") == 6.0; }));
	CHECK(channelJson(app, bass.channel).at("eq").at("bands")[1].at("on") == true);

	// Add Effect Track to Send (p. 554): the add track dialog's effect page, whose track the send goes to, below the
	// track whose send it is.
	click(app, window, "[param=\"sends.1.destination\"]");
	app.ui().key(window, "Down");
	click(app, window, "[param=\"sends.1.destination\"]");
	REQUIRE(app.waitUntil([&] { return !elements(app, window, "#add-track-dialog").empty(); }));
	CHECK(element(app, window, "#track-types").at("attributes").at("value") == "effect");
	click(app, window, "#add-track-confirm");
	REQUIRE(app.waitUntil([&] { return elements(app, window, "#add-track-dialog").empty(); }));
	const Json project = app.call("project.get");
	REQUIRE(project.at("tracks").size() == 4);
	const Json& effect = project.at("tracks")[2];
	CHECK(effect.at("type") == "effect");
	CHECK(project.at("tracks")[3].at("id") == drums.track);
	CHECK(channelJson(app, bass.channel).at("sends")[0].at("destination") == effect.at("channel"));
	CHECK(channelJson(app, bass.channel).at("sends")[0].at("on") == true);
	CHECK(channelJson(app, guitar.channel).at("sends")[0].at("destination") == 0);
	// The drop-down list shows the send's destination again.
	REQUIRE(app.waitUntil(
		[&]
		{
			return element(app, window, "[param=\"sends.1.destination\"]").at("attributes").at("value") ==
				std::to_string(effect.at("channel").get<uint64_t>());
		}));

	// With the add track dialog open in the project window, the entry brings that one to the front as it is, one at a
	// time (openOnce): no second dialog, its page and the name typed stay, and the send keeps its destination.
	app.keys().run("project.addTrack", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&] { return elements(app, std::nullopt, "#add-track-dialog").size() == 1; }));
	click(app, std::nullopt, "#track-name");
	app.ui().text(std::nullopt, "Hall");
	click(app, window, "[param=\"sends.2.destination\"]");
	app.ui().key(window, "Down"); // the effect track, which the send then goes to
	app.ui().key(window, "Down"); // Add Effect Track to Send
	click(app, window, "[param=\"sends.2.destination\"]");
	app.shell().step(std::chrono::milliseconds(0)); // the request, posted later in the step
	CHECK(elements(app, window, "#add-track-dialog").empty());
	CHECK(elements(app, std::nullopt, "#add-track-dialog").size() == 1);
	CHECK(element(app, std::nullopt, "#track-types").at("attributes").at("value") == "audio");
	CHECK(element(app, std::nullopt, "#track-name").at("attributes").at("value") == "Hall");
	click(app, std::nullopt, "#add-track-cancel");
	REQUIRE(app.waitUntil([&] { return elements(app, std::nullopt, "#add-track-dialog").empty(); }));
	CHECK(app.call("project.get").at("tracks").size() == 4);
	CHECK(channelJson(app, bass.channel).at("sends")[1].at("destination") == effect.at("channel"));
}

TEST_CASE("the inspector's Edit Channel Settings opens the channel settings window for its track", "[ui]")
{
	test::UiApp app({1200, 700});
	app.appUi().showProject();
	addTrack(app, "audio", "Guitar");
	const Added bass = addTrack(app, "audio", "Bass");
	app.appUi().docking().open("inspector");
	select(app, bass.track);
	REQUIRE(app.waitUntil([&] { return elements(app, std::nullopt, ".inspector-edit").size() == 1; }));
	click(app, std::nullopt, ".inspector-edit");
	REQUIRE(app.waitUntil([&] { return app.appUi().channelSettings() != nullptr; }));
	REQUIRE(app.waitUntil(
		[&] { return strips(app, app.appUi().channelSettingsWindow()) == std::vector<uint64_t>{bass.channel}; }));
}
