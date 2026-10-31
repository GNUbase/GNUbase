// SPDX-License-Identifier: GPL-3.0-or-later
// The studio setup dialog (#79, docs/features/studio-setup.md) in the app's UI, on the dummy device and a dummy MIDI
// port, driven with ui::Automation as MCP's UI tools drive it: what it shows comes from the engine, and what the user
// changes in it reaches the engine.
#include "uiapp.h"

#include "platform/audio/dummydevice.h"
#include "platform/midi/dummyport.h"
#include "ui/automation.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <memory>
#include <optional>
#include <string>
#include <utility>
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
		app.ui().key(std::nullopt, "Ctrl+A");
		app.ui().text(std::nullopt, text);
	}

	// Opens a drop-down list, picks the option that many above the selected one with the arrow key, and closes it.
	// Its options aren't in the DOM that the UI tools see.
	void choose(test::UiApp& app, const std::string& select, int up)
	{
		click(app, select);
		for (int i = 0; i < up; ++i)
			app.ui().key(std::nullopt, "Up");
		click(app, select);
	}

	// The text of the dialog's drop-down list's selected option.
	std::string selected(test::UiApp& app, const std::string& id)
	{
		Rml::Element* element = nullptr;
		for (int i = 0; element == nullptr && i < app.main().context().GetNumDocuments(); ++i)
			element = app.main().context().GetDocument(i)->GetElementById(id);
		auto* select = dynamic_cast<Rml::ElementFormControlSelect*>(element);
		REQUIRE(select != nullptr);
		const Rml::Element* option = select->GetOption(select->GetSelection());
		REQUIRE(option != nullptr);
		return option->GetInnerRML();
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
	choose(app, "#sample-rate", 1); // 44100, before 48000
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
	CHECK(elements(app, "#audio-ports input.show-as")[0].at("value") == "Mic");

	// The ports' Reset shows the system names and every port again, and OK sends them.
	click(app, "#reset-audio-ports");
	click(app, "#ok");
	CHECK(closed(app));
	CHECK(app.call("audio.ports").at("inputs")[0].at("showAs") == "Input 1");
	CHECK(app.call("audio.ports").at("outputs")[3].at("visible") == true);
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
	CHECK(app.call("engine.commandLog").at("commands").back().at("command") == "audio.reset");
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
	openDialog(app);
	CHECK(selected(app, "driver") == "dummy - Dummy device");
	choose(app, "#driver", 1); // the fake interface, listed before the dummy device
	CHECK(selected(app, "driver") == "fake - Fake interface");
	click(app, "#apply");
	CHECK(open(app));
	CHECK(lastRun(app, "audio.setDevice") == Json{{"backend", "fake"}, {"device", "fake-1"}});
	CHECK(app.call("audio.get").at("device").at("id") == "fake-1");
	CHECK(element(app, "#devices-device").at("text") == "Fake interface");
	CHECK(element(app, "#studio-setup-message").value("text", "").empty());
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
