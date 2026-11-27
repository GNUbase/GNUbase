// SPDX-License-Identifier: GPL-3.0-or-later
// The project window's status line (#407, docs/features/status-line.md) on the dummy device: the record format with a
// sample rate mismatch, a silent and a lost audio device, as they follow audio.changed; the remaining record time
// (#104).
#include "uiapp.h"

#include "engine/engine.h"
#include "engine/media/diskspace.h"
#include "platform/audio/dummydevice.h"
#include "testutils/testutils.h"
#include "ui/automation.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <vector>

using namespace gnubase;
using Catch::Matchers::ContainsSubstring;
using engine::Json;

namespace
{
	Json element(test::UiApp& app, const std::string& selector)
	{
		const Json found = app.ui().dom(std::nullopt, selector, 0).at("elements");
		REQUIRE(found.size() == 1);
		return found[0];
	}

	bool has(test::UiApp& app, const std::string& selector, const std::string& name)
	{
		const Json names = element(app, selector).value("classes", Json::array());
		return std::find(names.begin(), names.end(), name) != names.end();
	}

	std::string text(test::UiApp& app, const std::string& selector)
	{
		return element(app, selector).value("text", std::string());
	}

	// A driver whose clock another client may hold, so that its device runs but plays silence, as PipeWire's does
	// (#422).
	struct HeldDriver
	{
		std::atomic<bool> silent{false};
		std::atomic<audio::DummyDevice*> device{nullptr}; // the device opened last
	};

	class HeldDevice : public audio::DummyDevice
	{
	public:
		HeldDevice(const audio::DeviceFormat& format, const HeldDriver& driver)
			: DummyDevice(format, 0), m_driver(driver)
		{
		}

		std::string silenceReason() const override
		{
			return m_driver.silent ? "another client holds the clock at 44100 Hz" : "";
		}

	private:
		const HeldDriver& m_driver;
	};

	// The dummy device, as backend "held".
	class HeldBackend : public audio::DummyBackend
	{
	public:
		explicit HeldBackend(std::shared_ptr<HeldDriver> driver) : m_driver(std::move(driver)) {}

		std::string name() const override { return "held"; }

		std::unique_ptr<audio::Device> open(const audio::DeviceConfig& config) override
		{
			auto device = std::make_unique<HeldDevice>(DummyBackend::open(config)->format(), *m_driver);
			m_driver->device = device.get();
			return device;
		}

	private:
		std::shared_ptr<HeldDriver> m_driver;
	};
}

TEST_CASE("the status line highlights a sample rate mismatch and shows a lost audio device", "[ui]")
{
	test::UiApp app({1000, 700});
	app.appUi().showProject();
	CHECK(app.waitUntil([&] { return text(app, "#record-format") == "Record: 48 kHz - 24 bit"; }));
	CHECK_FALSE(has(app, "#record-format", "mismatch"));
	CHECK(has(app, "#lost-device", "hidden"));

	// The dummy device doesn't run at 16 kHz, so it stays at 48 kHz and plays the project faster.
	app.call("project.set", {{"sampleRate", 16000.0}});
	CHECK(app.waitUntil([&] { return has(app, "#record-format", "mismatch"); }));
	CHECK(text(app, "#record-format") == "Record: 16 kHz - 24 bit");
	app.call("project.set", {{"sampleRate", 44100.0}}); // it does at 44.1 kHz
	CHECK(app.waitUntil([&] { return !has(app, "#record-format", "mismatch"); }));
	CHECK(text(app, "#record-format") == "Record: 44.1 kHz - 24 bit");

	// Unplugged: shown by its name, with the reason and its id as the tooltip, until it is plugged in again.
	app.call("audio.simulateDeviceLoss", {{"connected", false}});
	CHECK(app.waitUntil([&] { return !has(app, "#lost-device", "hidden"); }));
	CHECK(text(app, "#lost-device") == "Audio Device Lost: Dummy device");
	const std::string title = element(app, "#lost-device").at("attributes").at("title").get<std::string>();
	CHECK_THAT(title, ContainsSubstring("unplugged"));
	CHECK_THAT(title, ContainsSubstring("(dummy)"));
	app.call("audio.simulateDeviceLoss", {{"connected", true}});
	CHECK(app.waitUntil([&] { return has(app, "#lost-device", "hidden"); }, std::chrono::seconds(10)));
}

TEST_CASE("the status line says why a running audio device plays silence", "[ui]")
{
	const auto driver = std::make_shared<HeldDriver>();
	std::vector<std::unique_ptr<audio::Backend>> backends;
	backends.push_back(std::make_unique<HeldBackend>(driver));
	backends.push_back(std::make_unique<audio::DummyBackend>());
	test::UiApp app({1000, 700}, midi::makeBackends(), std::move(backends));
	app.appUi().showProject();
	app.call("audio.setDevice", {{"backend", "held"}, {"start", true}});
	CHECK(app.waitUntil([&] { return text(app, "#record-format") == "Record: 48 kHz - 24 bit"; }));
	CHECK(has(app, "#silent-device", "hidden"));

	// The device reports the silence and its end as a latency change, on which the engine sends audio.changed.
	driver->silent = true;
	driver->device.load()->reportLatencyChange();
	CHECK(app.waitUntil([&] { return !has(app, "#silent-device", "hidden"); }));
	CHECK(text(app, "#silent-device") == "Audio Device Silent: Dummy device");
	CHECK(element(app, "#silent-device").at("attributes").at("title") == "another client holds the clock at 44100 Hz");
	CHECK(has(app, "#lost-device", "hidden"));
	driver->silent = false;
	driver->device.load()->reportLatencyChange();
	CHECK(app.waitUntil([&] { return has(app, "#silent-device", "hidden"); }));
	app.call("audio.stop");
}

TEST_CASE(
	"recording.remainingTime divides the free disk space by what the armed tracks record a second", "[integration]")
{
	const test::TempDir dir;
	engine::Engine engine;
	engine::Client client(engine);
	const auto run = [&client](const std::string& command, const Json& parameters = Json::object())
	{
		const engine::Result result = client.call(command, parameters);
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	};
	run("project.save", {{"path", (dir.path() / "time.dawproject").u8string()}});
	Json time = run("recording.remainingTime");
	CHECK(time.at("seconds").is_null()); // nothing armed
	CHECK(time.at("channels") == 0);
	CHECK(time.at("availableBytes").get<double>() > 0.0);
	CHECK(std::filesystem::u8path(time.at("folder").get<std::string>()) == dir.path() / "Audio");

	// A mono and a stereo track, 24 bit at 48 kHz: 9 bytes a frame. A track without an input bus doesn't record.
	std::vector<Json> channels;
	for (const int count : {1, 2, 2})
	{
		const Json added = run("track.add", {{"type", "audio"}, {"channels", count}});
		channels.push_back(added.at("channel").at("id"));
		run("channel.set", {{"channel", channels.back()}, {"recordEnable", true}});
	}
	run("channel.set", {{"channel", channels.back()}, {"input", 0}});
	run("project.set", {{"sampleRate", 48000.0}, {"recordBitDepth", "int24"}});
	time = run("recording.remainingTime");
	CHECK(time.at("channels") == 3);
	CHECK(time.at("seconds").get<double>() == Catch::Approx(time.at("availableBytes").get<double>() / (48000.0 * 9)));
	run("project.set", {{"recordBitDepth", "float64"}});
	time = run("recording.remainingTime");
	CHECK(time.at("seconds").get<double>() == Catch::Approx(time.at("availableBytes").get<double>() / (48000.0 * 24)));
}

TEST_CASE(
	"the disk space is measured off the caller's thread, so a disk that hangs doesn't hold it up (#645)", "[unit]")
{
	using namespace std::chrono_literals;
	// A disk that hangs until it is let go. Shared, since a measurement's thread may outlive the test.
	struct Disk
	{
		std::mutex mutex;
		std::condition_variable changed;
		bool hangs = true;
		int measured = 0;
	};
	const auto disk = std::make_shared<Disk>();
	engine::DiskSpace space(
		[disk](const std::filesystem::path& folder)
		{
			std::unique_lock lock(disk->mutex);
			++disk->measured;
			disk->changed.notify_all();
			disk->changed.wait(lock, [&disk] { return !disk->hangs; });
			return static_cast<uintmax_t>(folder == "a" ? 1000 : 2000);
		});
	const auto start = std::chrono::steady_clock::now();
	CHECK(!space.measure("a", 20ms).has_value()); // none yet
	CHECK(!space.measure("a", 20ms).has_value()); // nor a second measurement while the first one hangs
	CHECK(std::chrono::steady_clock::now() - start < 5s);
	{
		// The measurement's thread may not have started yet on a busy machine: wait for it, then check there is one.
		std::unique_lock lock(disk->mutex);
		CHECK(disk->changed.wait_for(lock, 10s, [&disk] { return disk->measured != 0; }));
		CHECK(disk->measured == 1);
		disk->hangs = false;
	}
	disk->changed.notify_all();
	// The hanging one ends; meanwhile a question doesn't wait.
	std::optional<uintmax_t> available;
	const auto until = std::chrono::steady_clock::now() + 10s;
	while (!available && std::chrono::steady_clock::now() < until)
	{
		available = space.measure("a", 100ms);
		std::this_thread::sleep_for(10ms);
	}
	CHECK(available == uintmax_t{1000});
	// Once the folder has a measurement, a slow next one doesn't make the question wait: it answers with the last one.
	{
		const std::scoped_lock lock(disk->mutex);
		disk->hangs = true;
	}
	const auto slow = std::chrono::steady_clock::now();
	CHECK(space.measure("a", 5s) == uintmax_t{1000});
	CHECK(std::chrono::steady_clock::now() - slow < 2s);
	{
		const std::scoped_lock lock(disk->mutex);
		disk->hangs = false;
	}
	disk->changed.notify_all();
	// Another folder waits for a measurement of its own, once the slow one has ended.
	available.reset();
	const auto other = std::chrono::steady_clock::now() + 10s;
	while (!available && std::chrono::steady_clock::now() < other)
	{
		available = space.measure("b", 5s);
		std::this_thread::sleep_for(10ms);
	}
	CHECK(available == uintmax_t{2000});
}

TEST_CASE("the status line shows the remaining record time of the record-enabled tracks", "[ui]")
{
	test::UiApp app({1000, 700});
	app.appUi().showProject();
	// Hidden, as by default, the status line doesn't ask: each question measures the disk (#645).
	const auto hiddenUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
	while (std::chrono::steady_clock::now() < hiddenUntil)
		app.shell().step(std::chrono::milliseconds(20));
	CHECK(text(app, "#record-time").empty());
	app.appUi().docking().setVisible("project", "statusLine", true);
	CHECK(app.waitUntil([&] { return text(app, "#record-time") == "Max. Record Time: -"; }));
	Rml::Element* recordTime = nullptr;
	Rml::Context& context = app.main().context();
	for (int i = 0; i < context.GetNumDocuments() && recordTime == nullptr; ++i)
		recordTime = context.GetDocument(i)->GetElementById("record-time");
	REQUIRE(recordTime != nullptr);
	const Json added = app.call("track.add", {{"type", "audio"}, {"channels", 1}});
	app.call("channel.set", {{"channel", added.at("channel").at("id")}, {"recordEnable", true}});

	// Hours and minutes, or minutes and seconds under an hour; it follows within a second or so. With no input, read
	// from the element itself (the UI tools are posted work, which draws every window): the project window draws the
	// new text in the step it changes in, though the status line asks for it quietly (#715).
	bool changed = false;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!changed && std::chrono::steady_clock::now() < deadline)
	{
		const std::string before = recordTime->GetInnerRML();
		const uint64_t frames = app.main().frames();
		app.shell().step(std::chrono::milliseconds(20));
		if (recordTime->GetInnerRML() != before)
		{
			changed = true;
			CHECK(app.main().frames() == frames + 1);
		}
	}
	REQUIRE(changed);
	const std::regex shown(R"(Max\. Record Time: (\d+ h \d\d min|\d+ min \d\d s))");
	CHECK(std::regex_match(text(app, "#record-time"), shown));
}

TEST_CASE(
	"Studio > More Options > Max. Record Time shows the remaining record time in a window of its own (#645)", "[ui]")
{
	test::UiApp app({1000, 700});
	app.appUi().showProject();
	CHECK(app.ui()
			  .dom(std::nullopt, "#studio-menu menuitem[command=\"window.maxRecordTime\"]", 0)
			  .at("elements")
			  .size() == 1);
	// The window whose document is max-record-time.rml, or none.
	const auto window = [&app]() -> std::optional<ui::WindowId>
	{
		const Json windows = app.ui().windows();
		for (const Json& open : windows.at("windows"))
			for (const Json& document : open.at("documents"))
				if (document.at("source").get<std::string>().find("max-record-time.rml") != std::string::npos)
					return open.at("id").get<ui::WindowId>();
		return std::nullopt;
	};
	const auto shown = [&app, &window]
	{
		const std::optional<ui::WindowId> id = window();
		if (!id)
			return std::string();
		const Json found = app.ui().dom(id, "#record-time", 0).at("elements");
		return found.size() == 1 ? found[0].value("text", std::string()) : std::string();
	};
	app.keys().run("window.maxRecordTime", Json::object(), app.main(), nullptr);
	REQUIRE(app.waitUntil([&] { return shown() == "Max. Record Time: -"; })); // no track would record
	const Json added = app.call("track.add", {{"type", "audio"}, {"channels", 1}});
	app.call("channel.set", {{"channel", added.at("channel").at("id")}, {"recordEnable", true}});
	const std::regex time(R"(Max\. Record Time: (\d+ h \d\d min|\d+ min \d\d s))");
	CHECK(app.waitUntil([&] { return std::regex_match(shown(), time); }, std::chrono::seconds(10)));
	app.keys().run("window.maxRecordTime", Json::object(), app.main(), nullptr);
	CHECK(app.waitUntil([&] { return !window(); }));
}
