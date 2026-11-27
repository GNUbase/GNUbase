// SPDX-License-Identifier: GPL-3.0-or-later
// The studio setup dialog (#79, docs/features/studio-setup.md) in the app's UI, on the dummy device and a dummy MIDI
// port, driven with ui::Automation as MCP's UI tools drive it: what it shows comes from the engine, and what the user
// changes in it reaches the engine.
#include "uiapp.h"

#include "platform/audio/dummydevice.h"
#include "platform/hosttime.h"
#include "platform/midi/dummyport.h"
#include "ui/automation.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace gnubase;
using Catch::Matchers::ContainsSubstring;
using engine::Json;

namespace
{
	// A dummy MIDI backend with a keyboard plugged in: its input and output Keys.
	std::vector<std::unique_ptr<midi::Backend>> keyboard()
	{
		auto backend = std::make_unique<midi::DummyBackend>();
		backend->addLoopback("Keys");
		std::vector<std::unique_ptr<midi::Backend>> backends;
		backends.push_back(std::move(backend));
		return backends;
	}

	// Two keyboards: Keys and Pads.
	std::vector<std::unique_ptr<midi::Backend>> keyboards()
	{
		auto backend = std::make_unique<midi::DummyBackend>();
		backend->addLoopback("Keys");
		backend->addLoopback("Pads");
		std::vector<std::unique_ptr<midi::Backend>> backends;
		backends.push_back(std::move(backend));
		return backends;
	}

	// An audio interface without hardware, fake-1: it runs like the dummy device and lists two clock sources, none of
	// them current, as some ASIO drivers report them.
	audio::DeviceInfo fakeInfo()
	{
		audio::DeviceInfo info;
		info.id = "fake-1";
		info.name = "Fake interface";
		info.inputs = {"In 1", "In 2"};
		info.outputs = {"Out 1", "Out 2"};
		info.sampleRates = {48000.0};
		info.blockSizes = {256};
		info.clockSources = {"Internal", "Word clock"};
		return info;
	}

	class FakeDevice : public audio::DummyDevice
	{
	public:
		FakeDevice() : DummyDevice(audio::DeviceFormat{}, 0) {}
		audio::DeviceInfo info() const override { return fakeInfo(); }
	};

	class FakeBackend : public audio::Backend
	{
	public:
		std::string name() const override { return "fake"; }
		std::vector<audio::DeviceInfo> devices() override { return {fakeInfo()}; }

		std::unique_ptr<audio::Device> open(const audio::DeviceConfig& config) override
		{
			if (!config.device.empty() && config.device != "fake-1")
				throw audio::DeviceError("no device " + config.device);
			return std::make_unique<FakeDevice>();
		}
	};

	// The fake interface's driver before the dummy device's.
	std::vector<std::unique_ptr<audio::Backend>> fakeInterface()
	{
		std::vector<std::unique_ptr<audio::Backend>> backends;
		backends.push_back(std::make_unique<FakeBackend>());
		backends.push_back(std::make_unique<audio::DummyBackend>());
		return backends;
	}

	// The parameters the engine last ran the command with, or null if it didn't.
	Json lastRun(test::UiApp& app, const std::string& command)
	{
		Json parameters;
		const Json log = app.call("engine.commandLog");
		for (const Json& entry : log.at("commands"))
			if (entry.at("command") == command)
				parameters = entry.at("parameters");
		return parameters;
	}

	Json elements(test::UiApp& app, const std::string& selector)
	{
		return app.ui().dom(std::nullopt, selector, 0).at("elements");
	}

	Json element(test::UiApp& app, const std::string& selector)
	{
		const Json found = elements(app, selector);
		INFO(selector);
		REQUIRE(found.size() == 1);
		return found[0];
	}

	void click(test::UiApp& app, const std::string& selector, int index = 0)
	{
		app.ui().click(std::nullopt, {selector, index, std::nullopt}, ui::MouseButton::Left, 1, {});
		app.shell().step(std::chrono::milliseconds(0));
	}

	// Replaces the text of a text field.
	void type(test::UiApp& app, const std::string& selector, int index, const std::string& text)
	{
		click(app, selector, index);
		app.ui().key(std::nullopt, "Mod+A");
		app.ui().text(std::nullopt, text);
	}

	// Opens a drop-down list, picks the option that many above the selected one with the arrow key (below it if
	// negative), and closes it.
	void choose(test::UiApp& app, const std::string& select, int up)
	{
		click(app, select);
		for (int i = 0; i < std::abs(up); ++i)
			app.ui().key(std::nullopt, up > 0 ? "Up" : "Down");
		click(app, select);
	}

	// The text of the dialog's drop-down list's selected option.
	std::string selected(test::UiApp& app, const std::string& id)
	{
		return test::selectedOption(app, "#" + id);
	}

	bool open(test::UiApp& app)
	{
		return !elements(app, "#studio-setup").empty();
	}

	// Opens the dialog through its UI command, as the main menu does.
	void openDialog(test::UiApp& app)
	{
		app.keys().run("window.studioSetup", Json::object(), app.main(), nullptr);
		REQUIRE(app.waitUntil([&app] { return open(app); }));
		app.shell().step(std::chrono::milliseconds(0));
	}

	bool closed(test::UiApp& app)
	{
		return app.waitUntil([&app] { return !open(app); });
	}

	// The MIDI port, or null.
	Json port(const Json& ports, const std::string& id, bool input)
	{
		for (const Json& entry : ports.at("ports"))
			if (entry.at("id") == id && entry.at("input") == input)
				return entry;
		return nullptr;
	}
}

TEST_CASE("the studio setup dialog shows the audio and MIDI devices and changes them through the engine", "[ui]")
{
	test::UiApp app({900, 640}, keyboard());
	app.call("audio.setDevice", {{"outputChannels", 4}});
	openDialog(app);

	// The audio system page: the dummy device's driver, picked in the drivers of every backend.
	CHECK(element(app, "#page-system").at("visible") == true);
	CHECK(selected(app, "driver") == "dummy - Dummy device");
	CHECK(element(app, "#devices-device").at("text") == "Dummy device");

	// The device page: its settings and its ports.
	click(app, "#devices-device");
	CHECK(element(app, "#page-system").at("visible") == false);
	CHECK(element(app, "#page-device").at("visible") == true);
	CHECK(element(app, "#sample-rate").at("value") == "48000");
	CHECK(element(app, "#block-size").at("value") == "256");
	CHECK(element(app, "#control-panel").at("attributes").contains("disabled")); // the dummy driver has none
	CHECK(elements(app, "#audio-ports tr").size() == 6);
	CHECK(!element(app, "#externally-clocked").at("attributes").contains("checked"));
	choose(app, "#sample-rate", 1); // 44100, before 48000
	click(app, "#externally-clocked");
	type(app, "#audio-ports input.show-as", 0, "Mic");
	click(app, "#audio-ports input.visible", 5); // hides Output 4

	// The MIDI page: the keyboard's input and output.
	click(app, "#devices-midi");
	REQUIRE(elements(app, "#midi-ports tr").size() == 2);
	type(app, "#midi-ports input.show-as", 0, "My keys");
	click(app, "#midi-ports input.all-inputs");

	click(app, "#ok");
	CHECK(closed(app));
	const Json device = app.call("audio.get").at("device");
	CHECK(device.at("sampleRate") == 44100.0);
	CHECK(device.at("externallyClocked") == true);
	CHECK(device.at("outputChannels") == 4); // audio.set keeps the dummy device's own settings
	const Json ports = app.call("audio.ports");
	CHECK(ports.at("inputs")[0].at("showAs") == "Mic");
	CHECK(ports.at("inputs")[1].at("showAs") == "Input 2");
	CHECK(ports.at("outputs")[3].at("visible") == false);
	CHECK(ports.at("outputs")[2].at("visible") == true);
	const Json midiPorts = app.call("midi.ports");
	CHECK(port(midiPorts, "Keys", true).at("showAs") == "My keys");
	CHECK(port(midiPorts, "Keys", true).at("inAllInputs") == false);
	CHECK(port(midiPorts, "Keys", false).at("showAs") == "Keys");

	// It opens again with what the engine has now.
	openDialog(app);
	click(app, "#devices-device");
	CHECK(element(app, "#sample-rate").at("value") == "44100");
	CHECK(element(app, "#externally-clocked").at("attributes").contains("checked"));
	CHECK(elements(app, "#audio-ports input.show-as")[0].at("value") == "Mic");

	// The ports' Reset shows the system names and every port again, and OK sends them.
	click(app, "#reset-audio-ports");
	click(app, "#ok");
	CHECK(closed(app));
	CHECK(app.call("audio.ports").at("inputs")[0].at("showAs") == "Input 1");
	CHECK(app.call("audio.ports").at("outputs")[3].at("visible") == true);
}

TEST_CASE("the studio setup dialog's Note Expression Input page sets up a MIDI input's MPE input", "[ui]")
{
	test::UiApp app({900, 640}, keyboard());
	openDialog(app);
	click(app, "#devices-mpe");
	CHECK(element(app, "#page-mpe").at("visible") == true);
	CHECK(element(app, "#page-midi").at("visible") == false);
	// The MIDI inputs, the keyboard's: no MPE input yet. First Channel is only for channel rotation.
	CHECK(selected(app, "mpe-input") == "Keys");
	const std::string fields = ".mpe-port.shown ";
	CHECK(element(app, fields + "select.mpe-mode").at("value") == "off");
	CHECK(element(app, fields + "select.mpe-first-channel").at("attributes").contains("disabled"));
	choose(app, fields + "select.mpe-mode", -3); // Channel Rotation, below Off and the two zones
	CHECK_FALSE(element(app, fields + "select.mpe-first-channel").at("attributes").contains("disabled"));
	choose(app, fields + "select.mpe-first-channel", -1);
	click(app, fields + "input.mpe-tuning");
	choose(app, fields + "select.mpe-initial-y", -2); // Center (64)

	// A pitch range out of range keeps the dialog open with the message, and sends nothing.
	type(app, fields + "input.mpe-pitch-range", 0, "200");
	click(app, "#ok");
	CHECK(open(app));
	CHECK_THAT(element(app, "#studio-setup-message").at("text").get<std::string>(), ContainsSubstring("Pitch Range"));
	CHECK(port(app.call("midi.ports"), "Keys", true).at("mpe").at("mode") == "off");

	click(app, "#devices-mpe");
	type(app, fields + "input.mpe-pitch-range", 0, "24");
	choose(app, fields + "select.mpe-mode", -3);
	choose(app, fields + "select.mpe-first-channel", -1);
	click(app, fields + "input.mpe-tuning");
	choose(app, fields + "select.mpe-initial-y", -2);
	click(app, "#ok");
	CHECK(closed(app));
	CHECK(port(app.call("midi.ports"), "Keys", true).at("mpe") == Json::parse(R"({"mode": "rotation", "channels": 15,
		"upperChannels": 7, "firstChannel": 2, "pitchRange": 24, "tuning": false, "initialX": "off", "initialY": "center",
		"xMessage": "pitchBend", "pressureMessage": "aftertouch", "yMessage": "cc74", "xExpression": "none",
		"pressureExpression": "pressure", "yExpression": "brightness"})"));
}

TEST_CASE("the Note Expression Input page sets up both MPE zones, each with its member channels", "[ui]")
{
	test::UiApp app({900, 640}, keyboard());
	openDialog(app);
	click(app, "#devices-mpe");
	// Upper Zone Channels is only for both zones (#1675).
	const std::string fields = ".mpe-port.shown ";
	CHECK(element(app, fields + "select.mpe-upper-channels").at("attributes").contains("disabled"));
	choose(app, fields + "select.mpe-mode", -4); // Both Zones, the last
	CHECK(element(app, fields + "select.mpe-mode").at("value") == "both");
	CHECK_FALSE(element(app, fields + "select.mpe-upper-channels").at("attributes").contains("disabled"));
	CHECK(element(app, fields + "select.mpe-first-channel").at("attributes").contains("disabled"));
	choose(app, fields + "select.mpe-upper-channels", -2); // 9, two below the 7 it starts with
	click(app, "#apply");
	// Used Channels is the lower zone's, which gets the rest of 14: the page shows it after Apply.
	const Json mpe = port(app.call("midi.ports"), "Keys", true).at("mpe");
	CHECK(mpe.at("mode") == "both");
	CHECK(mpe.at("upperChannels") == 9);
	CHECK(mpe.at("channels") == 5);
	CHECK(element(app, fields + "select.mpe-channels").at("value") == "5");
	CHECK(element(app, fields + "select.mpe-upper-channels").at("value") == "9");
	choose(app, fields + "select.mpe-mode", 1); // Channel Rotation
	CHECK(element(app, fields + "select.mpe-upper-channels").at("attributes").contains("disabled"));
	CHECK_FALSE(element(app, fields + "select.mpe-first-channel").at("attributes").contains("disabled"));
}

TEST_CASE("the Note Expression Input page sets each dimension's MIDI Message and Note Expression Assignment", "[ui]")
{
	test::UiApp app({900, 640}, keyboard());
	openDialog(app);
	click(app, "#devices-mpe");
	const std::string fields = ".mpe-port.shown ";
	CHECK(element(app, fields + "select.mpe-pressure-message").at("value") == "aftertouch");
	CHECK(element(app, fields + "select.mpe-y-message").at("value") == "cc74");
	// With Use for Tuning, X plays tuning: its assignment can't change.
	CHECK(element(app, fields + "select.mpe-x-expression").at("value") == "tuning");
	CHECK(element(app, fields + "select.mpe-x-expression").at("attributes").contains("disabled"));
	choose(app, fields + "select.mpe-pressure-message", -13); // CC 11, after Aftertouch, Pitchbend and CC 0 to 10
	CHECK(element(app, fields + "select.mpe-pressure-message").at("value") == "cc11");
	choose(app, fields + "select.mpe-y-expression", 5); // Volume, five above Brightness
	CHECK(element(app, fields + "select.mpe-y-expression").at("value") == "volume");
	choose(app, fields + "select.mpe-pressure-expression", -1); // MIDI PolyPressure, below Pressure (#1829)
	CHECK(element(app, fields + "select.mpe-pressure-expression").at("value") == "polyPressure");
	// Off, X plays none and can be another expression; Tuning turns it on again.
	click(app, fields + "input.mpe-tuning");
	CHECK(element(app, fields + "select.mpe-x-expression").at("value") == "none");
	CHECK_FALSE(element(app, fields + "select.mpe-x-expression").at("attributes").contains("disabled"));
	choose(app, fields + "select.mpe-x-expression", -2); // Pan, below None and Volume
	CHECK(element(app, fields + "select.mpe-x-expression").at("value") == "pan");
	click(app, "#apply");
	Json mpe = port(app.call("midi.ports"), "Keys", true).at("mpe");
	CHECK(mpe.at("pressureMessage") == "cc11");
	CHECK(mpe.at("yExpression") == "volume");
	CHECK(mpe.at("pressureExpression") == "polyPressure");
	CHECK(mpe.at("tuning") == false);
	CHECK(mpe.at("xExpression") == "pan");
	choose(app, fields + "select.mpe-x-expression", -1); // Tuning, below Pan
	CHECK(element(app, fields + "input.mpe-tuning").at("attributes").contains("checked"));
	CHECK(element(app, fields + "select.mpe-x-expression").at("attributes").contains("disabled"));
	click(app, "#ok");
	CHECK(closed(app));
	// X keeps Pan as its own assignment under Use for Tuning (#1749), which it plays again with Use for Tuning off.
	mpe = port(app.call("midi.ports"), "Keys", true).at("mpe");
	CHECK(mpe.at("tuning") == true);
	CHECK(mpe.at("xExpression") == "pan");
	openDialog(app);
	click(app, "#devices-mpe");
	CHECK(element(app, fields + "select.mpe-x-expression").at("value") == "tuning");
	click(app, fields + "input.mpe-tuning");
	CHECK(element(app, fields + "select.mpe-x-expression").at("value") == "pan");
	click(app, "#ok");
	CHECK(closed(app));
	mpe = port(app.call("midi.ports"), "Keys", true).at("mpe");
	CHECK(mpe.at("tuning") == false);
	CHECK(mpe.at("xExpression") == "pan");
}

TEST_CASE("a detected MPE device has a page, and the MIDI Activity display shows its input", "[ui]")
{
	auto backend = std::make_unique<midi::DummyBackend>();
	backend->addLoopback("Keys");
	backend->addLoopback("LinnStrument MIDI");
	midi::DummyBackend& dummy = *backend;
	std::vector<std::unique_ptr<midi::Backend>> backends;
	backends.push_back(std::move(backend));
	test::UiApp app({900, 640}, std::move(backends));
	openDialog(app);
	// Its page, named after the device, is the Note Expression Input page with its input, with its preset.
	const Json item = element(app, ".devices-mpe-device");
	CHECK(item.at("text") == "LinnStrument");
	click(app, ".devices-mpe-device");
	CHECK(element(app, "#page-mpe").at("visible") == true);
	CHECK(selected(app, "mpe-input") == "LinnStrument MIDI");
	const std::string fields = ".mpe-port.shown ";
	CHECK(element(app, fields + ".mpe-device").at("text") == "LinnStrument");
	CHECK(element(app, fields + "select.mpe-mode").at("value") == "lower");

	// The device plays: its messages show, the newest last, as midi.activity announces them.
	const std::unique_ptr<midi::Output> device = dummy.openOutput("LinnStrument MIDI");
	for (const std::vector<uint8_t>& bytes :
		std::vector<std::vector<uint8_t>>{{0x91, 60, 100}, {0xE1, 0, 64}, {0xB1, 74, 30}})
	{
		audio::MidiMessage message;
		message.size = static_cast<uint8_t>(bytes.size());
		std::copy(bytes.begin(), bytes.end(), message.bytes.begin());
		device->send(message, hostTime() - 1000000000);
	}
	// Offline the engine doesn't run midi.activity itself.
	if (!app.call("audio.get").at("running").get<bool>())
	{
		app.call("audio.render", {{"frames", 128}});
		app.call("midi.activity");
	}
	const std::string activity = fields + ".mpe-activity";
	CHECK(app.waitUntil([&app, &activity] { return elements(app, activity + " .mpe-message").size() == 3; }));
	const Json lines = elements(app, activity + " .mpe-message");
	REQUIRE(lines.size() == 3);
	CHECK(lines[0].at("text") == "Ch 2 Note On 60 100");
	CHECK(lines[1].at("text") == "Ch 2 Pitchbend 8192");
	CHECK(lines[2].at("text") == "Ch 2 CC 74 30");
	// Another input's activity shows on its own fields.
	choose(app, "#mpe-input", 1);
	CHECK(selected(app, "mpe-input") == "Keys");
	CHECK(elements(app, activity + " .mpe-message").empty());

	// Closed, the page stops listening (midi.activityListen), but not for a client that listens too, as an MCP
	// session or a script may (#1832); once that stops, input is no activity (#1749).
	app.call("midi.activityListen", {{"on", true}});
	click(app, "#cancel");
	CHECK(closed(app));
	engine::Client watcher(app.engine(), "watcher");
	// Whether midi.activity announced input: offline the test runs it, as the engine doesn't.
	const auto announced = [&](uint8_t pitch)
	{
		audio::MidiMessage message;
		message.size = 3;
		message.bytes = {0x91, pitch, 100};
		device->send(message, hostTime() - 1000000000);
		if (!app.call("audio.get").at("running").get<bool>())
		{
			app.call("audio.render", {{"frames", 128}});
			app.call("midi.activity");
		}
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
		while (std::chrono::steady_clock::now() < deadline)
			if (const std::optional<engine::Message> next = watcher.wait(std::chrono::milliseconds(50)))
				if (const auto* event = std::get_if<engine::Event>(&*next);
					event != nullptr && event->name == "midi.activity")
					return true;
		return false;
	};
	CHECK(announced(62));
	app.call("midi.activityListen", {{"on", false}});
	CHECK_FALSE(announced(64));
}

TEST_CASE("the Note Expression Input page keeps each MIDI input's edits, and its input after Apply", "[ui]")
{
	test::UiApp app({900, 640}, keyboards());
	openDialog(app);
	click(app, "#devices-mpe");
	CHECK(selected(app, "mpe-input") == "Keys");
	const std::string fields = ".mpe-port.shown ";
	choose(app, fields + "select.mpe-mode", -1);     // MPE Mode, Lower Zone
	choose(app, fields + "select.mpe-channels", -1); // 16, one more than a zone has
	choose(app, "#mpe-input", -1);
	CHECK(selected(app, "mpe-input") == "Pads");
	CHECK(element(app, fields + "select.mpe-mode").at("value") == "off");
	choose(app, fields + "select.mpe-mode", -2); // MPE Mode, Upper Zone
	// Back on Keys, its edit is still there.
	choose(app, "#mpe-input", 1);
	CHECK(selected(app, "mpe-input") == "Keys");
	CHECK(element(app, fields + "select.mpe-mode").at("value") == "lower");
	choose(app, "#mpe-input", -1);

	// Apply sends both; the page shows what the engine has, still of the input selected.
	click(app, "#apply");
	CHECK(open(app));
	CHECK(port(app.call("midi.ports"), "Keys", true).at("mpe").at("mode") == "lower");
	CHECK(port(app.call("midi.ports"), "Pads", true).at("mpe").at("mode") == "upper");
	CHECK(selected(app, "mpe-input") == "Pads");
	CHECK(element(app, fields + "select.mpe-mode").at("value") == "upper");
	// The zone got the 15 member channels it can have.
	CHECK(port(app.call("midi.ports"), "Keys", true).at("mpe").at("channels") == 15);
	choose(app, "#mpe-input", 1);
	CHECK(element(app, fields + "select.mpe-channels").at("value") == "15");
}

TEST_CASE("the studio setup dialog's Cancel drops the changes, Apply sends them and stays open", "[ui]")
{
	test::UiApp app({900, 640}, keyboard());
	openDialog(app);
	click(app, "#devices-device");
	choose(app, "#block-size", 3); // 32, three before 256
	click(app, "#cancel");
	CHECK(closed(app));
	CHECK(app.call("audio.get").at("device").at("blockSize") == 256);

	// Multi-processing changes while the device runs: the dialog stops it for that and starts it again.
	app.call("audio.start");
	const bool multiProcessing = app.call("audio.get").at("processing").at("multiProcessing").get<bool>();
	openDialog(app);
	click(app, "#multi-processing");
	click(app, "#apply");
	CHECK(open(app));
	Json status = app.call("audio.get");
	CHECK(status.at("processing").at("multiProcessing") == !multiProcessing);
	CHECK(status.at("running") == true);
	CHECK(element(app, "#studio-setup-message").value("text", "").empty());

	// What the engine refuses stays in the dialog, with the message.
	type(app, "#record-shift", 0, "soon");
	click(app, "#ok");
	CHECK(open(app));
	CHECK_THAT(element(app, "#studio-setup-message").value("text", ""), ContainsSubstring("Record Shift"));

	// Reset opens the device again, through the engine.
	click(app, "#reset-device");
	REQUIRE(app.waitUntil([&app] { return !lastRun(app, "audio.reset").is_null(); }));
	app.ui().key(std::nullopt, "Escape");
	CHECK(closed(app));
	app.call("audio.stop");
}

TEST_CASE("the studio setup dialog keeps a sample rate and buffer size that the driver's standard lists lack", "[ui]")
{
	test::UiApp app({900, 640}, keyboard());
	app.call("audio.setDevice", {{"sampleRate", 22050}, {"blockSize", 100}});
	openDialog(app);
	click(app, "#devices-device");
	CHECK(element(app, "#sample-rate").at("value") == "22050");
	CHECK(element(app, "#block-size").at("value") == "100");

	// Something else changes, and OK leaves the device as it is.
	click(app, "#devices-midi");
	type(app, "#midi-ports input.show-as", 0, "My keys");
	click(app, "#ok");
	CHECK(closed(app));
	const Json device = app.call("audio.get").at("device");
	CHECK(device.at("sampleRate") == 22050.0);
	CHECK(device.at("blockSize") == 100);
	CHECK(lastRun(app, "audio.set").is_null());
}

TEST_CASE("the studio setup dialog switches the audio driver through the engine", "[ui]")
{
	test::UiApp app({900, 640}, keyboard(), fakeInterface());
	app.call("audio.start");
	openDialog(app);
	CHECK(selected(app, "driver") == "dummy - Dummy device");
	choose(app, "#driver", 1); // the fake interface, listed before the dummy device
	CHECK(selected(app, "driver") == "fake - Fake interface");
	click(app, "#apply");
	CHECK(open(app));
	// The new driver runs as the old one did, and the app opens it again when it starts.
	CHECK(lastRun(app, "audio.setDevice") == Json{{"backend", "fake"}, {"device", "fake-1"}, {"remember", true}});
	const Json status = app.call("audio.get");
	CHECK(status.at("device").at("id") == "fake-1");
	CHECK(status.at("running") == true);
	CHECK(app.call("settings.get", {{"key", "audio.backend"}}).at("value") == "fake");
	CHECK(app.call("settings.get", {{"key", "audio.device"}}).at("value") == "fake-1");
	CHECK(element(app, "#devices-device").at("text") == "Fake interface");
	CHECK(element(app, "#studio-setup-message").value("text", "").empty());
	app.call("audio.stop");
}

TEST_CASE("the studio setup dialog says why the device chosen last can't be used while the dummy device plays", "[ui]")
{
	test::UiApp app({900, 640}, keyboard(), fakeInterface());
	app.call("audio.setDevice", {{"backend", "fake"}, {"device", "fake-9"}, {"fallBackToDummy", true}});
	openDialog(app);
	CHECK(selected(app, "driver") == "dummy - Dummy device");
	CHECK(element(app, "#studio-setup-message").value("text", "") ==
		"Cannot use fake - fake-9: no device fake-9; the dummy device plays instead");

	// Another driver that opens ends it.
	choose(app, "#driver", 1);
	click(app, "#apply");
	CHECK(app.call("audio.get").at("failed").is_null());
	CHECK(element(app, "#studio-setup-message").value("text", "").empty());
}

TEST_CASE(
	"the studio setup dialog's Apply makes the fallback that plays for a device that failed the user's choice", "[ui]")
{
	test::UiApp app({900, 640}, keyboard(), fakeInterface());
	app.call(
		"audio.setDevice", {{"backend", "fake"}, {"device", "fake-9"}, {"blockSize", 128}, {"fallBackToDummy", true}});
	openDialog(app);
	CHECK(selected(app, "driver") == "dummy - Dummy device");

	// Apply and OK without the driver list touched leave the user's choice for the next start.
	const Json fellBack = lastRun(app, "audio.setDevice");
	click(app, "#apply");
	CHECK(lastRun(app, "audio.setDevice") == fellBack);
	CHECK(app.call("audio.get").at("failed").is_object());
	CHECK(app.call("settings.get", {{"key", "audio.backend"}}).at("value").get<std::string>().empty());
	click(app, "#ok");
	CHECK(closed(app));
	CHECK(lastRun(app, "audio.setDevice") == fellBack);
	CHECK(app.call("audio.get").at("failed").is_object());
	CHECK(app.call("settings.get", {{"key", "audio.backend"}}).at("value").get<std::string>().empty());
	openDialog(app);

	// The driver that plays picked in the list: Apply saves it, at the block size it plays with.
	choose(app, "#driver", 1);
	choose(app, "#driver", -1);
	CHECK(selected(app, "driver") == "dummy - Dummy device");
	click(app, "#apply");
	CHECK(open(app));
	CHECK(lastRun(app, "audio.setDevice") == Json{{"backend", "dummy"}, {"device", "dummy"}, {"remember", true}});
	const Json status = app.call("audio.get");
	CHECK(status.at("failed").is_null());
	CHECK(status.at("device").at("blockSize") == 128);
	CHECK(app.call("settings.get", {{"key", "audio.backend"}}).at("value") == "dummy");
	CHECK(element(app, "#studio-setup-message").value("text", "").empty());

	// With the fallback device default, the message names the backend's default device that plays instead.
	app.call("settings.set", {{"key", "audio.fallbackDevice"}, {"value", "default"}});
	app.call("audio.setDevice", {{"backend", "fake"}, {"device", "fake-9"}, {"fallBackToDummy", true}});
	click(app, "#cancel");
	openDialog(app);
	CHECK(element(app, "#studio-setup-message").value("text", "") ==
		"Cannot use fake - fake-9: no device fake-9; fake - Fake interface plays instead");
	click(app, "#cancel");
}

TEST_CASE("the studio setup dialog leaves the clock source alone when the device reports none as current", "[ui]")
{
	test::UiApp app({900, 640}, keyboard(), fakeInterface());
	app.call("audio.setDevice", {{"backend", "fake"}, {"device", "fake-1"}});
	REQUIRE(app.call("audio.get").at("device").at("clockSource").is_null());
	openDialog(app);
	click(app, "#devices-device");
	CHECK(selected(app, "clock-source").empty());
	click(app, "#ok");
	CHECK(closed(app));
	CHECK(lastRun(app, "audio.set").is_null());

	// A clock source the user picks is sent.
	openDialog(app);
	click(app, "#devices-device");
	click(app, "#clock-source");
	app.ui().key(std::nullopt, "Down");
	click(app, "#clock-source");
	CHECK(selected(app, "clock-source") == "Internal");
	click(app, "#ok");
	CHECK(closed(app));
	CHECK(lastRun(app, "audio.set") == Json{{"clockSource", "Internal"}});
}

TEST_CASE("the studio setup command brings the open dialog to the front instead of opening another", "[ui]")
{
	test::UiApp app({900, 640}, keyboard());
	openDialog(app);
	click(app, "#devices-device");
	type(app, "#audio-ports input.show-as", 0, "Mic");

	// MCP and Lua can run it while the dialog is open; the dialog keeps what the user changed in it.
	app.keys().run("window.studioSetup", Json::object(), app.main(), nullptr);
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(elements(app, "#studio-setup").size() == 1);
	CHECK(elements(app, "#audio-ports input.show-as")[0].at("value") == "Mic");
	click(app, "#ok");
	CHECK(closed(app));
	CHECK(app.call("audio.ports").at("inputs")[0].at("showAs") == "Mic");

	// Once closed, it opens again.
	openDialog(app);
	CHECK(elements(app, "#studio-setup").size() == 1);
}
