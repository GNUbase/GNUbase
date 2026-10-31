// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/engine.h"
#include "platform/audio/dummydevice.h"
#include "platform/hosttime.h"
#include "platform/midi/dummyport.h"
#include "platform/rt/realtime.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace gnubase::audio;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using gnubase::engine::Client;
using gnubase::engine::Engine;
using gnubase::engine::Event;
using gnubase::engine::Json;
using gnubase::engine::Message;
using gnubase::engine::Result;

namespace
{
	// Records every block and outputs input + position, so the output depends on inputs, blocks and positions.
	// Records into memory reserved up front, as the RT checker requires.
	class Recorder : public AudioCallback
	{
	public:
		struct Block
		{
			uint32_t frames = 0;
			uint64_t position = 0;
			std::array<MidiMessage, 8> midi{};
			size_t midiCount = 0;
		};

		void process(const DeviceBlock& block) noexcept override
		{
			realtime = realtime && gnubase::rt::isRealtimeContext();
			Block recorded{block.frames, block.position};
			for (const MidiMessage& message : block.midiIn)
				if (recorded.midiCount < recorded.midi.size())
					recorded.midi[recorded.midiCount++] = message;
			if (count < blocks.size())
				blocks[count++] = recorded;
			for (uint32_t i = 0; i < block.frames; ++i)
			{
				block.outputs[0][i] = block.inputs[0][i] + static_cast<float>(block.position + i);
				block.outputs[1][i] = -block.outputs[0][i];
			}
			for (const MidiMessage& message : block.midiIn)
			{
				MidiMessage echo = message;
				echo.bytes[2] = 1;
				block.midiOut.add(echo);
			}
		}

		std::array<Block, 1024> blocks{};
		size_t count = 0;
		bool realtime = true;
	};

	DeviceFormat format(uint32_t blockSize)
	{
		DeviceFormat result;
		result.blockSize = blockSize;
		return result;
	}

	MidiMessage noteOn(uint8_t pitch)
	{
		MidiMessage message;
		message.size = 3;
		message.bytes = {0x90, pitch, 100};
		return message;
	}

	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	std::string reject(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " succeeded: " << result.value.dump());
		REQUIRE(!result.ok());
		return result.error;
	}

	std::vector<float> capture(Client& client, int channel, size_t frames)
	{
		return run(client, "audio.capture", {{"channel", channel}, {"start", 0}, {"frames", frames}})
			.at("samples")
			.get<std::vector<float>>();
	}

	// What the fake backend's driver does, shared with the test.
	struct FakeDriver
	{
		std::atomic<uint32_t> preferredBlockSize{256};
		std::atomic<int> controlPanels{0};
		std::atomic<DummyDevice*> device{nullptr}; // the device opened last
		std::atomic<bool> connected{true};         // fake-1's hardware; opening fails without it
		std::atomic<bool> failStart{false};        // starting fails, as with a driver that doesn't process audio
	};

	DeviceInfo fakeInfo()
	{
		DeviceInfo info;
		info.id = "fake-1";
		info.name = "Fake interface";
		info.inputs = {"Mic", "Line"};
		info.outputs = {"Main L", "Main R"};
		info.sampleRates = {44100.0, 48000.0};
		info.blockSizes = {128, 256};
		info.clockSources = {"Internal", "Word clock"};
		info.clockSource = 0;
		info.controlPanel = true;
		return info;
	}

	// A device that describes itself like hardware and otherwise runs like the dummy device.
	class FakeDevice : public DummyDevice
	{
	public:
		FakeDevice(const DeviceFormat& format, int clockSource, FakeDriver& driver)
			: DummyDevice(format, 0), m_clockSource(clockSource), m_driver(driver)
		{
		}

		DeviceInfo info() const override
		{
			DeviceInfo info = fakeInfo();
			info.clockSource = m_clockSource;
			return info;
		}

		DeviceLatency latency() const override { return {format().blockSize, 2 * format().blockSize}; }
		void openControlPanel() override { ++m_driver.controlPanels; }

		void start(AudioCallback& callback) override
		{
			if (m_driver.failStart)
				throw DeviceError("the driver starts, but doesn't process audio");
			DummyDevice::start(callback);
		}

	private:
		int m_clockSource;
		FakeDriver& m_driver;
	};

	// A backend with one device, fake-1, whose driver can change its preferred block size and ask for a reset. It also
	// lists fake-2, whose hardware isn't connected.
	class FakeBackend : public Backend
	{
	public:
		explicit FakeBackend(std::shared_ptr<FakeDriver> driver) : m_driver(std::move(driver)) {}

		std::string name() const override { return "fake"; }

		std::vector<DeviceInfo> devices() override
		{
			DeviceInfo unplugged;
			unplugged.id = "fake-2";
			unplugged.name = "Unplugged interface";
			unplugged.unavailableReason = "not connected";
			return {fakeInfo(), unplugged};
		}

		std::unique_ptr<Device> open(const DeviceConfig& config) override
		{
			const DeviceInfo info = fakeInfo();
			if (!config.device.empty() && config.device != info.id)
				throw DeviceError("no device " + config.device);
			if (!m_driver->connected)
				throw DeviceError("fake-1 is not connected");
			DeviceFormat format;
			format.sampleRate = config.sampleRate == 0.0 ? 48000.0 : config.sampleRate;
			format.blockSize = config.blockSize == 0 ? m_driver->preferredBlockSize.load() : config.blockSize;
			if (std::find(info.sampleRates.begin(), info.sampleRates.end(), format.sampleRate) ==
					info.sampleRates.end() ||
				std::find(info.blockSizes.begin(), info.blockSizes.end(), format.blockSize) == info.blockSizes.end())
				throw DeviceError("the fake interface doesn't run at that sample rate or block size");
			int clockSource = 0;
			if (!config.clockSource.empty())
			{
				const auto found = std::find(info.clockSources.begin(), info.clockSources.end(), config.clockSource);
				if (found == info.clockSources.end())
					throw DeviceError("no clock source " + config.clockSource);
				clockSource = static_cast<int>(found - info.clockSources.begin());
			}
			auto device = std::make_unique<FakeDevice>(format, clockSource, *m_driver);
			m_driver->device = device.get();
			return device;
		}

	private:
		std::shared_ptr<FakeDriver> m_driver;
	};

	std::vector<std::unique_ptr<Backend>> fakeBackends(const std::shared_ptr<FakeDriver>& driver)
	{
		std::vector<std::unique_ptr<Backend>> backends;
		backends.push_back(std::make_unique<FakeBackend>(driver));
		backends.push_back(std::make_unique<DummyBackend>());
		return backends;
	}

	// Waits for an event whose data matches; false after five seconds.
	template <typename Match> bool waitForEvent(Client& client, const std::string& name, Match match)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < deadline)
		{
			const std::optional<Message> message = client.wait(std::chrono::milliseconds(50));
			const auto* event = message ? std::get_if<Event>(&*message) : nullptr;
			if (event != nullptr && event->name == name && match(event->data))
				return true;
		}
		return false;
	}

	// A project whose output depends on routing: the input through an audio track and a group to the output, and a
	// second audio track straight to the output, both monitoring it.
	void buildProject(Client& client)
	{
		const Json group = run(client, "track.add", {{"type", "group"}});
		const Json first = run(client, "track.add", {{"type", "audio"}});
		const Json second = run(client, "track.add", {{"type", "audio"}});
		run(client, "channel.set",
			{{"channel", first.at("channel").at("id")}, {"output", group.at("channel").at("id")}, {"monitor", true}});
		run(client, "channel.set", {{"channel", second.at("channel").at("id")}, {"monitor", true}});
	}
}

TEST_CASE("the dummy device renders offline in blocks, from its input to its capture", "[unit]")
{
	DummyDevice device(format(64), 1000);
	device.setInput(
		[](float* const* inputs, int channels, uint32_t frames, uint64_t /*position*/)
		{
			for (int c = 0; c < channels; ++c)
				for (uint32_t i = 0; i < frames; ++i)
					inputs[c][i] = 0.5f;
		});
	Recorder recorder;
	device.render(recorder, 150);

	REQUIRE(recorder.count == 3);
	CHECK(recorder.blocks[0].frames == 64);
	CHECK(recorder.blocks[1].position == 64);
	CHECK(recorder.blocks[2].frames == 22);
	CHECK(recorder.blocks[2].position == 128);
	CHECK(recorder.realtime);
	CHECK(device.position() == 150);
	CHECK(device.capturedFrames() == 150);

	const std::vector<float> left = device.captured(0, 140, 100); // cut to what was captured
	REQUIRE(left.size() == 10);
	CHECK(left[0] == 140.5f);
	CHECK(device.captured(1, 149, 1) == std::vector<float>{-149.5f});
}

TEST_CASE("the dummy device delivers injected MIDI at its frame and captures MIDI output", "[unit]")
{
	DummyDevice device(format(64), 1000);
	device.injectMidi(70, noteOn(60));
	device.injectMidi(5, noteOn(62)); // injected out of order
	device.injectMidi(70, noteOn(64));
	Recorder recorder;
	device.render(recorder, 128);

	REQUIRE(recorder.blocks[0].midiCount == 1);
	CHECK(recorder.blocks[0].midi[0].offset == 5);
	REQUIRE(recorder.blocks[1].midiCount == 2);
	CHECK(recorder.blocks[1].midi[0].offset == 6);
	CHECK(recorder.blocks[1].midi[0].bytes[1] == 60);
	CHECK(recorder.blocks[1].midi[1].bytes[1] == 64);

	const std::vector<TimedMidi> captured = device.capturedMidi();
	REQUIRE(captured.size() == 3);
	CHECK(captured[0].position == 5);
	CHECK(captured[1].position == 70);
	CHECK(captured[2].message.bytes == std::array<uint8_t, 3>{0x90, 64, 1});
}

TEST_CASE("the dummy device keeps what fits in its capture and counts the rest", "[unit]")
{
	DummyDevice device(format(64), 100);
	Recorder recorder;
	device.render(recorder, 256);
	CHECK(device.capturedFrames() == 100);
	CHECK(device.droppedFrames() == 156);
	CHECK(device.position() == 256);
}

TEST_CASE("simulated real time takes as long as the audio and renders the same as offline", "[integration]")
{
	constexpr uint64_t frames = 9600; // 200 ms
	DummyDevice offline(format(128), frames);
	Recorder offlineRecorder;
	offline.render(offlineRecorder, frames);

	DummyDevice device(format(128), frames);
	Recorder recorder;
	const auto started = std::chrono::steady_clock::now();
	device.start(recorder);
	CHECK(device.running());
	while (device.position() < frames)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	const auto elapsed = std::chrono::steady_clock::now() - started;
	device.stop();
	// Not CHECK_FALSE: Catch2's ContinueOnFailure | FalseTest trips clang-analyzer's EnumCastOutOfRange check
	CHECK(!device.running());

	CHECK(elapsed >= std::chrono::milliseconds(180));
	CHECK(recorder.realtime);
	CHECK(device.captured(0, 0, frames) == offline.captured(0, 0, frames));
	CHECK(device.captured(1, 0, frames) == offline.captured(1, 0, frames));
}

TEST_CASE("the same project renders identically offline and in simulated real time", "[integration]")
{
	constexpr int frames = 24000;
	const Json device = {{"sampleRate", 48000}, {"blockSize", 128}, {"captureSeconds", 1},
		{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 997.0}, {"Level", 0.5}}}}}};

	Engine offlineEngine;
	Client offline(offlineEngine);
	buildProject(offline);
	run(offline, "audio.setDevice", device);
	CHECK(run(offline, "audio.render", {{"frames", frames}}).at("position") == frames);

	Engine realtimeEngine;
	Client realtime(realtimeEngine);
	buildProject(realtime);
	run(realtime, "audio.setDevice", device);
	CHECK(run(realtime, "audio.start").at("running") == true);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (run(realtime, "audio.get").at("position").get<int64_t>() < frames &&
		std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	CHECK(run(realtime, "audio.stop").at("running") == false);

	for (int channel = 0; channel < 2; ++channel)
	{
		const std::vector<float> expected = capture(offline, channel, frames);
		REQUIRE(expected.size() == frames);
		// Two paths from the input reach the output: twice the input.
		CHECK_THAT(expected[1], WithinAbs(2.0 * 0.5 * std::sin(2.0 * 3.14159265358979323846 * 997.0 / 48000.0), 1e-6));
		CHECK(capture(realtime, channel, frames) == expected);
	}
}

TEST_CASE("busses take consecutive device channels, and channels no bus reaches stay silent", "[unit]")
{
	Engine engine;
	Client client(engine);
	const Json track = run(client, "track.add", {{"type", "audio"}});
	run(client, "channel.set", {{"channel", track.at("channel").at("id")}, {"monitor", true}});
	run(client, "audio.setDevice",
		{{"blockSize", 64}, {"inputChannels", 1}, {"outputChannels", 4}, {"captureSeconds", 0.01},
			{"input", {{"processor", "impulse"}, {"parameters", {{"Interval", 0.001}}}}}});
	run(client, "audio.render", {{"frames", 64}});

	// The mono device input feeds the left channel of the stereo input bus; the right one is silent.
	const std::vector<float> left = capture(client, 0, 64);
	CHECK(left[0] == 1.0f);
	CHECK(left[48] == 1.0f);
	CHECK(left[1] == 0.0f);
	CHECK(capture(client, 1, 64) == std::vector<float>(64, 0.0f));
	CHECK(capture(client, 2, 64) == std::vector<float>(64, 0.0f));
	CHECK(capture(client, 3, 64) == std::vector<float>(64, 0.0f));
}

TEST_CASE("audio.get reports the device, and audio commands check what they can do", "[unit]")
{
	Engine engine;
	Client client(engine);
	Json status = run(client, "audio.get");
	CHECK(status.at("device").is_null());
	CHECK_THAT(reject(client, "audio.render", {{"frames", 10}}), ContainsSubstring("no audio device"));
	CHECK_THAT(reject(client, "audio.start"), ContainsSubstring("no audio device"));

	CHECK_THAT(reject(client, "audio.setDevice", {{"blockSize", 0}}), ContainsSubstring("blockSize"));
	CHECK_THAT(reject(client, "audio.setDevice", {{"input", {{"processor", "testSynth"}}}}),
		ContainsSubstring("not a generator"));
	CHECK_THAT(reject(client, "audio.setDevice", {{"input", {{"processor", "sine"}, {"parameters", {{"Pitch", 1}}}}}}),
		ContainsSubstring("no parameter Pitch"));

	status = run(client, "audio.setDevice", {{"sampleRate", 44100}, {"blockSize", 32}, {"outputChannels", 6}});
	const Json& device = status.at("device");
	CHECK(device.at("backend") == "dummy");
	CHECK(device.at("id") == "dummy");
	CHECK(device.at("sampleRate") == 44100.0);
	CHECK(device.at("blockSize") == 32);
	CHECK(device.at("inputChannels") == 2);
	CHECK(device.at("outputChannels") == 6);
	CHECK(device.at("inputs") == Json{"Input 1", "Input 2"});
	CHECK(device.at("outputs").size() == 6);
	CHECK(device.at("inputLatency") == 0);
	CHECK(device.at("outputLatency") == 0);
	CHECK(device.at("clockSource").is_null());
	CHECK(device.at("controlPanel") == false);
	CHECK(status.at("running") == false);
	CHECK(status.at("position") == 0);

	run(client, "audio.start");
	CHECK_THAT(reject(client, "audio.start"), ContainsSubstring("already running"));
	CHECK_THAT(reject(client, "audio.render", {{"frames", 10}}), ContainsSubstring("running"));
	run(client, "track.add", {{"type", "audio"}}); // the graph is swapped in while it runs
	run(client, "audio.stop");

	CHECK_THAT(reject(client, "audio.capture", {{"channel", 6}, {"start", 0}, {"frames", 1}}),
		ContainsSubstring("no output channel 6"));
}

TEST_CASE("audio.devices lists every backend's devices", "[unit]")
{
	Engine engine(fakeBackends(std::make_shared<FakeDriver>()));
	Client client(engine);
	const Json backends = run(client, "audio.devices").at("backends");
	REQUIRE(backends.size() == 2);
	CHECK(backends[0].at("name") == "fake");
	const Json& devices = backends[0].at("devices");
	REQUIRE(devices.size() == 2);
	CHECK(devices[0] ==
		Json{{"id", "fake-1"}, {"name", "Fake interface"}, {"inputs", {"Mic", "Line"}},
			{"outputs", {"Main L", "Main R"}}, {"sampleRates", {44100.0, 48000.0}}, {"blockSizes", {128, 256}},
			{"clockSources", {"Internal", "Word clock"}}, {"clockSource", "Internal"}, {"controlPanel", true},
			{"available", true}, {"unavailableReason", nullptr}, {"canMonitorDirectly", false}});
	CHECK(devices[1].at("id") == "fake-2");
	CHECK(devices[1].at("available") == false);
	CHECK(devices[1].at("unavailableReason") == "not connected");
	CHECK(backends[1].at("name") == "dummy");
	CHECK(backends[1].at("devices")[0].at("id") == "dummy");
}

TEST_CASE(
	"audio.setDevice opens a backend's device with the sample rate, block size and clock source asked for", "[unit]")
{
	Engine engine(fakeBackends(std::make_shared<FakeDriver>()));
	Client client(engine);
	Json device = run(client, "audio.setDevice", {{"backend", "fake"}}).at("device");
	CHECK(device.at("backend") == "fake");
	CHECK(device.at("id") == "fake-1");
	CHECK(device.at("name") == "Fake interface");
	CHECK(device.at("sampleRate") == 48000.0);
	CHECK(device.at("blockSize") == 256); // the driver's preferred size
	CHECK(device.at("inputChannels") == 2);
	CHECK(device.at("outputs") == Json{"Main L", "Main R"});
	CHECK(device.at("inputLatency") == 256);
	CHECK(device.at("outputLatency") == 512);
	CHECK(device.at("clockSource") == "Internal");
	CHECK(device.at("controlPanel") == true);

	device = run(client, "audio.setDevice",
		{{"backend", "fake"}, {"device", "fake-1"}, {"sampleRate", 44100}, {"blockSize", 128},
			{"clockSource", "Word clock"}})
				 .at("device");
	CHECK(device.at("sampleRate") == 44100.0);
	CHECK(device.at("blockSize") == 128);
	CHECK(device.at("clockSource") == "Word clock");

	CHECK_THAT(reject(client, "audio.setDevice", {{"backend", "nope"}}), ContainsSubstring("no audio backend nope"));
	CHECK_THAT(reject(client, "audio.setDevice", {{"backend", "fake"}, {"inputChannels", 1}}),
		ContainsSubstring("only the dummy device"));
	CHECK_THAT(reject(client, "audio.setDevice", {{"clockSource", "Internal"}}), ContainsSubstring("clock source"));
	CHECK_THAT(reject(client, "audio.setDevice", {{"device", "fake-1"}}), ContainsSubstring("no device fake-1"));
	CHECK(run(client, "audio.get").at("device").at("blockSize") == 128); // checked before anything changed

	// The audio device of another backend is closed before the new one opens; if that fails, there is none.
	CHECK_THAT(reject(client, "audio.setDevice", {{"backend", "fake"}, {"sampleRate", 96000}}),
		ContainsSubstring("sample rate"));
	CHECK(run(client, "audio.get").at("device").is_null());

	run(client, "audio.setDevice", {{"backend", "fake"}});
	CHECK_THAT(reject(client, "audio.render", {{"frames", 10}}), ContainsSubstring("dummy device"));
	CHECK_THAT(reject(client, "audio.capture", {{"channel", 0}, {"start", 0}, {"frames", 1}}),
		ContainsSubstring("dummy device"));
}

TEST_CASE("audio.controlPanel opens the driver's control panel", "[unit]")
{
	const auto driver = std::make_shared<FakeDriver>();
	Engine engine(fakeBackends(driver));
	Client client(engine);
	CHECK_THAT(reject(client, "audio.controlPanel"), ContainsSubstring("no audio device"));
	run(client, "audio.setDevice");
	CHECK_THAT(reject(client, "audio.controlPanel"), ContainsSubstring("no control panel"));
	run(client, "audio.setDevice", {{"backend", "fake"}});
	run(client, "audio.controlPanel");
	CHECK(driver->controlPanels == 1);
}

TEST_CASE("a driver's reset request reopens its device with the new settings and keeps it running", "[integration]")
{
	const auto driver = std::make_shared<FakeDriver>();
	Engine engine(fakeBackends(driver));
	Client client(engine);
	run(client, "track.add", {{"type", "audio"}});
	run(client, "audio.setDevice", {{"backend", "fake"}});
	run(client, "audio.start");

	// The user picks a smaller buffer in the driver's control panel, and the driver asks to be reset.
	driver->preferredBlockSize = 128;
	driver->device.load()->requestReset();
	CHECK(waitForEvent(client, "audio.changed",
		[](const Json& data) { return data.at("device").at("blockSize") == 128 && data.at("running") == true; }));
	const Json status = run(client, "audio.get");
	CHECK(status.at("device").at("blockSize") == 128);
	CHECK(status.at("running") == true);

	// audio.reset does the same, and a stopped device stays stopped.
	run(client, "audio.stop");
	driver->preferredBlockSize = 256;
	const Json reset = run(client, "audio.reset");
	CHECK(reset.at("device").at("blockSize") == 256);
	CHECK(reset.at("running") == false);
}

TEST_CASE("audio.reset keeps the dummy device's settings", "[unit]")
{
	Engine engine;
	Client client(engine);
	CHECK_THAT(reject(client, "audio.reset"), ContainsSubstring("no audio device"));
	const Json track = run(client, "track.add", {{"type", "audio"}});
	run(client, "channel.set", {{"channel", track.at("channel").at("id")}, {"monitor", true}}); // hears the input
	run(client, "audio.setDevice", {{"blockSize", 64}, {"outputChannels", 4}, {"input", {{"processor", "sine"}}}});
	run(client, "audio.render", {{"frames", 64}});
	const Json status = run(client, "audio.reset");
	CHECK(status.at("device").at("blockSize") == 64);
	CHECK(status.at("device").at("outputChannels") == 4);
	CHECK(status.at("position") == 0); // a new device
	run(client, "audio.render", {{"frames", 64}});
	CHECK(capture(client, 0, 64) != std::vector<float>(64, 0.0f)); // with its input
}

TEST_CASE("audio.set changes the device's settings and keeps the others", "[unit]")
{
	Engine engine;
	Client client(engine);
	CHECK_THAT(reject(client, "audio.set", {{"blockSize", 128}}), ContainsSubstring("no audio device"));
	run(client, "audio.setDevice", {{"blockSize", 64}, {"outputChannels", 4}, {"outputLatency", 300}});
	Json device = run(client, "audio.set", {{"sampleRate", 44100}, {"blockSize", 128}}).at("device");
	CHECK(device.at("sampleRate") == 44100.0);
	CHECK(device.at("blockSize") == 128);
	CHECK(device.at("outputChannels") == 4); // the dummy device's own settings stay
	CHECK(device.at("outputLatency") == 300);

	// A running device runs on with the new settings.
	run(client, "audio.start");
	const Json status = run(client, "audio.set", {{"recordShift", 10}});
	CHECK(status.at("running") == true);
	CHECK(status.at("device").at("recordShift") == 10);
	CHECK(status.at("device").at("blockSize") == 128);
	run(client, "audio.stop");

	// Settings the device can't take leave it as it was.
	CHECK_THAT(reject(client, "audio.set", {{"blockSize", 0}}), ContainsSubstring("blockSize"));
	CHECK(run(client, "audio.get").at("device").at("blockSize") == 128);
	CHECK_THAT(reject(client, "audio.set", {{"backend", "dummy"}}), ContainsSubstring("backend"));
}

TEST_CASE("audio.set opens a driver's device again with the old settings if it can't take the new", "[unit]")
{
	Engine engine(fakeBackends(std::make_shared<FakeDriver>()));
	Client client(engine);
	run(client, "audio.setDevice", {{"backend", "fake"}, {"sampleRate", 44100}});
	Json device = run(client, "audio.set", {{"clockSource", "Word clock"}}).at("device");
	CHECK(device.at("id") == "fake-1");
	CHECK(device.at("sampleRate") == 44100.0);
	CHECK(device.at("clockSource") == "Word clock");

	CHECK_THAT(reject(client, "audio.set", {{"sampleRate", 96000}}), ContainsSubstring("sample rate"));
	device = run(client, "audio.get").at("device");
	REQUIRE(device.is_object());
	CHECK(device.at("sampleRate") == 44100.0);
	CHECK(device.at("clockSource") == "Word clock");
}

TEST_CASE("a failed audio.set or audio.setDevice still announces the device it reopened or closed", "[unit]")
{
	const auto driver = std::make_shared<FakeDriver>();
	Engine engine(fakeBackends(driver));
	Client client(engine);
	run(client, "audio.setDevice", {{"backend", "fake"}, {"sampleRate", 44100}});
	while (client.wait(std::chrono::milliseconds(0))) // audio.setDevice's own audio.changed
		;

	// Opened again with the old settings: the device is back at position 0, and clients hear of it.
	reject(client, "audio.set", {{"sampleRate", 96000}});
	CHECK(waitForEvent(client, "audio.changed", [](const Json& data)
		{ return data.at("device").is_object() && data.at("device").at("sampleRate") == 44100.0; }));

	// Neither the new nor the old settings open it: there is no device.
	driver->connected = false;
	reject(client, "audio.set", {{"blockSize", 128}});
	CHECK(waitForEvent(client, "audio.changed", [](const Json& data) { return data.at("device").is_null(); }));
	CHECK(run(client, "audio.get").at("device").is_null());

	// audio.setDevice closes the old device before it opens the new one.
	run(client, "audio.setDevice");
	reject(client, "audio.setDevice", {{"backend", "fake"}});
	CHECK(waitForEvent(client, "audio.changed", [](const Json& data) { return data.at("device").is_null(); }));
}

TEST_CASE("a reset or a returning device that opens but fails to start still announces the device", "[integration]")
{
	const auto driver = std::make_shared<FakeDriver>();
	Engine engine(fakeBackends(driver));
	Client client(engine);
	run(client, "audio.setDevice", {{"backend", "fake"}});
	run(client, "audio.start");
	while (client.wait(std::chrono::milliseconds(0))) // the commands' own audio.changed
		;
	const auto openAndStopped = [](const Json& data)
	{ return data.at("device").is_object() && data.at("running") == false && data.at("lost").is_null(); };

	// audio.reset opens the device again, then starting it fails.
	driver->failStart = true;
	reject(client, "audio.reset");
	CHECK(waitForEvent(client, "audio.changed", openAndStopped));
	driver->failStart = false;
	run(client, "audio.start");

	// The running device goes away and returns, then starting it fails: the automatic try still announces it.
	driver->connected = false;
	driver->device.load()->requestReset();
	CHECK(waitForEvent(client, "audio.changed", [](const Json& data) { return !data.at("lost").is_null(); }));
	CHECK(run(client, "audio.get").at("lost").at("restart") == true);
	driver->failStart = true;
	driver->connected = true;
	CHECK(waitForEvent(client, "audio.changed", openAndStopped));
	CHECK(openAndStopped(run(client, "audio.get")));
}

TEST_CASE("audio.setPort renames and hides the device's ports, per device", "[unit]")
{
	Engine engine;
	Client client(engine);
	CHECK_THAT(
		reject(client, "audio.setPort", {{"input", true}, {"name", "Input 1"}}), ContainsSubstring("no audio device"));
	run(client, "audio.setDevice");
	Json ports = run(client, "audio.ports");
	CHECK(ports.at("inputs")[0].at("showAs") == "Input 1");
	CHECK(ports.at("inputs")[0].at("visible") == true);

	const Json port =
		run(client, "audio.setPort", {{"input", true}, {"name", "Input 1"}, {"showAs", "Mic"}, {"visible", false}});
	CHECK(port.at("name") == "Input 1");
	CHECK(port.at("showAs") == "Mic");
	CHECK(port.at("visible") == false);
	CHECK(waitForEvent(
		client, "audio.portsChanged", [](const Json& data) { return data.at("inputs")[0].at("showAs") == "Mic"; }));
	ports = run(client, "audio.ports");
	CHECK(ports.at("inputs")[0].at("showAs") == "Mic");
	CHECK(ports.at("inputs")[1].at("showAs") == "Input 2");
	CHECK(ports.at("outputs")[0].at("showAs") == "Output 1"); // outputs are other ports

	CHECK_THAT(reject(client, "audio.setPort", {{"input", false}, {"name", "Input 1"}}),
		ContainsSubstring("no output port Input 1"));

	// The device keeps them when it opens again, also with other settings.
	run(client, "audio.setDevice", {{"inputChannels", 4}});
	ports = run(client, "audio.ports");
	CHECK(ports.at("inputs")[0].at("showAs") == "Mic");
	CHECK(ports.at("inputs")[0].at("visible") == false);

	// An empty name shows the system name again.
	CHECK(
		run(client, "audio.setPort", {{"input", true}, {"name", "Input 1"}, {"showAs", ""}}).at("showAs") == "Input 1");
}

namespace
{
	// Waits until the transport has played past a position; false after five seconds.
	bool waitForTransport(Client& client, int64_t past)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (run(client, "transport.get").at("position").get<int64_t>() <= past)
		{
			if (std::chrono::steady_clock::now() > deadline)
				return false;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		return true;
	}
}

TEST_CASE("a device unplugged during playback leaves the engine running and plays on once it returns", "[integration]")
{
	Engine engine;
	Client client(engine);
	CHECK_THAT(reject(client, "audio.simulateDeviceLoss", {{"connected", false}}), ContainsSubstring("dummy device"));
	run(client, "track.add", {{"type", "audio"}});
	run(client, "audio.setDevice", {{"blockSize", 64}});
	run(client, "transport.play");
	run(client, "audio.start");
	CHECK(waitForTransport(client, 0));

	// The hardware goes away: the driver asks for a reset, which fails, and the engine waits for the device.
	CHECK(run(client, "audio.simulateDeviceLoss", {{"connected", false}}).at("lost").is_null()); // not yet
	CHECK(waitForEvent(client, "audio.changed", [](const Json& data) { return !data.at("lost").is_null(); }));
	Json status = run(client, "audio.get");
	CHECK(status.at("device").is_null());
	CHECK(status.at("running") == false);
	CHECK(status.at("lost").at("backend") == "dummy");
	CHECK(status.at("lost").at("restart") == true);
	CHECK_THAT(status.at("lost").at("reason").get<std::string>(), ContainsSubstring("unplugged"));
	const Json transport = run(client, "transport.get");
	CHECK(transport.at("playing") == true);                                // the engine goes on
	CHECK(run(client, "audio.reset").at("lost").at("backend") == "dummy"); // still gone, without an error
	CHECK_THAT(reject(client, "audio.setDevice"), ContainsSubstring("unplugged"));
	CHECK(run(client, "audio.get").at("lost").at("restart") == true); // a failed audio.setDevice changes nothing

	// Plugged in again: the engine opens it on its own, with its settings, and plays on from where it was.
	run(client, "audio.simulateDeviceLoss", {{"connected", true}});
	CHECK(waitForEvent(client, "audio.changed",
		[](const Json& data) { return data.at("lost").is_null() && data.at("running") == true; }));
	status = run(client, "audio.get");
	CHECK(status.at("device").at("blockSize") == 64);
	CHECK(waitForTransport(client, transport.at("position").get<int64_t>()));
	run(client, "audio.stop");
}

TEST_CASE("a backend's device that went away is opened again with its settings once it returns", "[integration]")
{
	const auto driver = std::make_shared<FakeDriver>();
	Engine engine(fakeBackends(driver));
	Client client(engine);
	run(client, "audio.setDevice", {{"backend", "fake"}, {"blockSize", 128}});
	CHECK_THAT(reject(client, "audio.simulateDeviceLoss", {{"connected", false}}), ContainsSubstring("dummy device"));

	driver->connected = false;
	driver->device.load()->requestReset(); // as drivers do when their device goes away
	CHECK(waitForEvent(client, "audio.changed", [](const Json& data) { return !data.at("lost").is_null(); }));
	const Json lost = run(client, "audio.get").at("lost");
	CHECK(lost.at("backend") == "fake");
	CHECK(lost.at("restart") == false); // it was stopped
	CHECK_THAT(lost.at("reason").get<std::string>(), ContainsSubstring("not connected"));

	driver->connected = true;
	CHECK(waitForEvent(client, "audio.changed",
		[](const Json& data) { return data.at("lost").is_null() && !data.at("device").is_null(); }));
	const Json status = run(client, "audio.get");
	CHECK(status.at("device").at("blockSize") == 128);
	CHECK(status.at("running") == false);

	// Picking another device ends the wait for the lost one.
	driver->connected = false;
	driver->device.load()->requestReset();
	CHECK(waitForEvent(client, "audio.changed", [](const Json& data) { return !data.at("lost").is_null(); }));
	CHECK(run(client, "audio.setDevice").at("lost").is_null());
	driver->connected = true;
	std::this_thread::sleep_for(std::chrono::seconds(1));
	CHECK(run(client, "audio.get").at("device").at("backend") == "dummy");
}

TEST_CASE("the unplugged dummy device plugs in again after another backend replaced it", "[integration]")
{
	const auto driver = std::make_shared<FakeDriver>();
	Engine engine(fakeBackends(driver));
	Client client(engine);
	run(client, "audio.setDevice");
	run(client, "audio.simulateDeviceLoss", {{"connected", false}});
	CHECK(waitForEvent(client, "audio.changed", [](const Json& data) { return !data.at("lost").is_null(); }));
	driver->connected = false;
	reject(client, "audio.setDevice", {{"backend", "fake"}}); // closes the lost dummy device, then fails
	CHECK(run(client, "audio.get").at("lost").is_null());
	run(client, "audio.simulateDeviceLoss", {{"connected", true}});
	CHECK(run(client, "audio.setDevice").at("device").at("backend") == "dummy");
}

TEST_CASE("tries to open a lost device again run one at a time and stay out of the command log", "[integration]")
{
	Engine engine;
	Client client(engine);
	run(client, "audio.setDevice");
	run(client, "audio.simulateDeviceLoss", {{"connected", false}});
	CHECK(waitForEvent(client, "audio.changed", [](const Json& data) { return !data.at("lost").is_null(); }));

	// Tries that find the device still gone leave the log to what clients did.
	std::this_thread::sleep_for(std::chrono::milliseconds(1200));
	Json log = run(client, "engine.commandLog").at("commands");
	CHECK(std::count_if(log.begin(), log.end(), [](const Json& entry) { return entry.at("client") == "engine"; }) ==
		1); // the driver's audio.reset that lost the device

	// The command thread is busy for over a second and the device returns meanwhile: tries don't queue up behind
	// it, so the device opens once, announced once, instead of every further try resetting it again.
	engine.post("audio.get", Json::object(),
		[](const Result&) { std::this_thread::sleep_for(std::chrono::milliseconds(1200)); }); // a long command
	run(client, "audio.simulateDeviceLoss", {{"connected", true}});
	int changes = 0;
	const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
	while (std::chrono::steady_clock::now() < until)
	{
		const std::optional<Message> message = client.wait(std::chrono::milliseconds(50));
		const auto* event = message ? std::get_if<Event>(&*message) : nullptr;
		if (event != nullptr && event->name == "audio.changed")
		{
			CHECK(event->data.at("device").is_object());
			++changes;
		}
	}
	CHECK(changes == 1);
	log = run(client, "engine.commandLog").at("commands");
	CHECK(std::count_if(log.begin(), log.end(), [](const Json& entry)
			  { return entry.at("command") == "audio.reconnect"; }) == 1); // the try that opened it
}

TEST_CASE("audio.stop and audio.start while a device is lost decide whether it starts again", "[integration]")
{
	Engine engine;
	Client client(engine);
	run(client, "audio.setDevice");
	run(client, "audio.start");
	run(client, "audio.simulateDeviceLoss", {{"connected", false}});
	CHECK(waitForEvent(client, "audio.changed", [](const Json& data) { return !data.at("lost").is_null(); }));

	// Stopped while it is gone: it returns stopped.
	CHECK(run(client, "audio.stop").at("lost").at("restart") == false);
	CHECK(waitForEvent(client, "audio.changed",
		[](const Json& data) { return !data.at("lost").is_null() && data.at("lost").at("restart") == false; }));
	run(client, "audio.simulateDeviceLoss", {{"connected", true}});
	CHECK(waitForEvent(client, "audio.changed", [](const Json& data) { return data.at("device").is_object(); }));
	CHECK(run(client, "audio.get").at("running") == false);

	// Started while it is gone: it returns running.
	run(client, "audio.simulateDeviceLoss", {{"connected", false}});
	CHECK(waitForEvent(client, "audio.changed", [](const Json& data) { return !data.at("lost").is_null(); }));
	CHECK(run(client, "audio.start").at("lost").at("restart") == true);
	CHECK(waitForEvent(client, "audio.changed",
		[](const Json& data) { return !data.at("lost").is_null() && data.at("lost").at("restart") == true; }));
	run(client, "audio.simulateDeviceLoss", {{"connected", true}});
	CHECK(waitForEvent(client, "audio.changed", [](const Json& data) { return data.at("device").is_object(); }));
	CHECK(run(client, "audio.get").at("running") == true);
	run(client, "audio.stop");
}

TEST_CASE("a device lost while the project's sample rate changes returns at the new rate", "[integration]")
{
	const auto driver = std::make_shared<FakeDriver>();
	Engine engine(fakeBackends(driver));
	Client client(engine);
	run(client, "audio.setDevice", {{"backend", "fake"}, {"sampleRate", 48000.0}});
	driver->connected = false;
	driver->device.load()->requestReset();
	CHECK(waitForEvent(client, "audio.changed", [](const Json& data) { return !data.at("lost").is_null(); }));
	run(client, "project.set", {{"sampleRate", 44100.0}});
	driver->connected = true;
	CHECK(waitForEvent(client, "audio.changed",
		[](const Json& data) { return data.at("lost").is_null() && !data.at("device").is_null(); }));
	const Json device = run(client, "audio.get").at("device");
	CHECK(device.at("sampleRate") == 44100.0);
	CHECK(device.at("sampleRateMismatch") == false);
}

TEST_CASE("the device follows the project's sample rate where it can and reports a mismatch where it can't", "[unit]")
{
	Engine engine(fakeBackends(std::make_shared<FakeDriver>()));
	Client client(engine);
	run(client, "audio.setDevice", {{"backend", "fake"}});
	run(client, "audio.start");
	Json device = run(client, "audio.get").at("device");
	CHECK(device.at("sampleRate") == 48000.0);
	CHECK(device.at("projectSampleRate") == 48000.0);
	CHECK(device.at("sampleRateMismatch") == false);

	// A new project sample rate that the device supports: the device changes to it and keeps running.
	run(client, "project.set", {{"sampleRate", 44100.0}});
	Json status = run(client, "audio.get");
	CHECK(status.at("device").at("sampleRate") == 44100.0);
	CHECK(status.at("device").at("sampleRateMismatch") == false);
	CHECK(status.at("running") == true);

	// One it doesn't: the device stays, and plays faster or slower.
	run(client, "project.set", {{"sampleRate", 96000.0}});
	device = run(client, "audio.get").at("device");
	CHECK(device.at("sampleRate") == 44100.0);
	CHECK(device.at("projectSampleRate") == 96000.0);
	CHECK(device.at("sampleRateMismatch") == true);
	run(client, "audio.stop");

	// A device opened without a sample rate runs at the project's; one asked for wins until the project's changes.
	run(client, "project.set", {{"sampleRate", 44100.0}});
	CHECK(run(client, "audio.setDevice", {{"backend", "fake"}}).at("device").at("sampleRate") == 44100.0);
	device = run(client, "audio.setDevice", {{"backend", "fake"}, {"sampleRate", 48000.0}}).at("device");
	CHECK(device.at("sampleRate") == 48000.0);
	CHECK(device.at("sampleRateMismatch") == true);
	run(client, "project.set", {{"name", "Song"}});
	CHECK(run(client, "audio.get").at("device").at("sampleRate") == 48000.0);

	// The dummy device lists the usual rates only.
	run(client, "project.set", {{"sampleRate", 16000.0}});
	device = run(client, "audio.setDevice").at("device");
	CHECK(device.at("sampleRate") == 48000.0);
	CHECK(device.at("sampleRateMismatch") == true);
	run(client, "project.set", {{"sampleRate", 96000.0}});
	CHECK(run(client, "audio.get").at("device").at("sampleRate") == 96000.0);
}

namespace
{
	std::vector<std::unique_ptr<gnubase::midi::Backend>> midiBackends(
		std::unique_ptr<gnubase::midi::DummyBackend> backend)
	{
		std::vector<std::unique_ptr<gnubase::midi::Backend>> backends;
		backends.push_back(std::move(backend));
		return backends;
	}
}

TEST_CASE("midi.ports lists the MIDI ports and keeps their settings while they are unplugged", "[integration]")
{
	auto dummy = std::make_unique<gnubase::midi::DummyBackend>();
	gnubase::midi::DummyBackend& backend = *dummy;
	backend.addLoopback("Keys");
	Engine engine(makeBackends(), {}, midiBackends(std::move(dummy)));
	Client client(engine);
	Json ports = run(client, "midi.ports").at("ports");
	REQUIRE(ports.size() == 2);
	CHECK(ports[0] ==
		Json{{"backend", "dummy"}, {"id", "Keys"}, {"input", true}, {"name", "Keys"}, {"showAs", "Keys"},
			{"visible", true}, {"inAllInputs", true}, {"present", true}, {"open", true}, {"midi2", false}});
	CHECK(ports[1].at("input") == false);
	CHECK(ports[1].at("open") == false); // outputs open once something sends to them

	const Json keys =
		run(client, "midi.setPort", {{"id", "Keys"}, {"input", true}, {"showAs", "My keys"}, {"inAllInputs", false}});
	CHECK(keys.at("showAs") == "My keys");
	CHECK(keys.at("inAllInputs") == false);
	CHECK_THAT(reject(client, "midi.setPort", {{"id", "Keys"}, {"input", false}, {"inAllInputs", true}}),
		ContainsSubstring("only inputs"));
	CHECK_THAT(
		reject(client, "midi.setPort", {{"id", "Drums"}, {"input", true}}), ContainsSubstring("no MIDI input Drums"));

	// Unplugged: the backend tells the engine, which lists the port as not present and keeps its settings.
	backend.removeLoopback("Keys");
	CHECK(waitForEvent(
		client, "midi.changed", [](const Json& data) { return data.at("ports")[0].at("present") == false; }));
	ports = run(client, "midi.ports").at("ports");
	CHECK(ports[0].at("open") == false);
	CHECK(ports[0].at("showAs") == "My keys");

	// Plugged in again: open again, with its settings.
	backend.addLoopback("Keys");
	CHECK(
		waitForEvent(client, "midi.changed", [](const Json& data) { return data.at("ports")[0].at("open") == true; }));
	CHECK(run(client, "midi.ports").at("ports")[0].at("inAllInputs") == false);

	// Hidden inputs are closed.
	CHECK(run(client, "midi.setPort", {{"id", "Keys"}, {"input", true}, {"visible", false}}).at("open") == false);
	CHECK(run(client, "midi.refresh").at("ports").size() == 2);
}

TEST_CASE("MIDI input reaches the monitor at its frame position once the device processes blocks", "[unit]")
{
	auto dummy = std::make_unique<gnubase::midi::DummyBackend>();
	gnubase::midi::DummyBackend& backend = *dummy;
	backend.addLoopback("Keys");
	Engine engine(makeBackends(), {}, midiBackends(std::move(dummy)));
	Client client(engine);
	CHECK(run(client, "midi.monitor").at("received").empty()); // the first MIDI command opens the ports
	run(client, "audio.setDevice", {{"blockSize", 64}});

	constexpr gnubase::HostTime second = 1000000000;
	const auto keys = backend.openOutput("Keys");
	keys->send(noteOn(60), gnubase::hostTime() - second);          // long ago: the next block's first frame
	keys->send(noteOn(62), gnubase::hostTime() + (3600 * second)); // in an hour
	CHECK(run(client, "midi.monitor").at("received").empty());     // no block yet

	run(client, "audio.render", {{"frames", 128}});
	const Json monitor = run(client, "midi.monitor");
	CHECK(monitor.at("received") ==
		Json::array({{{"backend", "dummy"}, {"id", "Keys"}, {"position", 0}, {"bytes", {0x90, 60, 100}}}}));
	CHECK(monitor.at("dropped") == 0);
	CHECK(run(client, "midi.monitor").at("received").empty()); // each message once

	// start drops what was received so far, for a fresh start.
	keys->send(noteOn(64), gnubase::hostTime() - second);
	run(client, "audio.render", {{"frames", 64}});
	CHECK(run(client, "midi.monitor", {{"start", true}}) == Json{{"received", Json::array()}, {"dropped", 0}});
	CHECK(run(client, "midi.monitor").at("received").empty());
}
