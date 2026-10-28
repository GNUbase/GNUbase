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

	private:
		int m_clockSource;
		FakeDriver& m_driver;
	};

	// A backend with one device, fake-1, whose driver can change its preferred block size and ask for a reset.
	class FakeBackend : public Backend
	{
	public:
		explicit FakeBackend(std::shared_ptr<FakeDriver> driver) : m_driver(std::move(driver)) {}

		std::string name() const override { return "fake"; }
		std::vector<DeviceInfo> devices() override { return {fakeInfo()}; }

		std::unique_ptr<Device> open(const DeviceConfig& config) override
		{
			const DeviceInfo info = fakeInfo();
			if (!config.device.empty() && config.device != info.id)
				throw DeviceError("no device " + config.device);
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
	// second audio track straight to the output.
	void buildProject(Client& client)
	{
		const Json group = run(client, "track.add", {{"type", "group"}});
		const Json first = run(client, "track.add", {{"type", "audio"}});
		run(client, "track.add", {{"type", "audio"}});
		run(client, "channel.set",
			{{"channel", first.at("channel").at("id")}, {"output", group.at("channel").at("id")}});
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
	run(client, "track.add", {{"type", "audio"}});
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
	CHECK(backends[0].at("devices") ==
		Json::array({{{"id", "fake-1"}, {"name", "Fake interface"}, {"inputs", {"Mic", "Line"}},
			{"outputs", {"Main L", "Main R"}}, {"sampleRates", {44100.0, 48000.0}}, {"blockSizes", {128, 256}},
			{"clockSources", {"Internal", "Word clock"}}, {"clockSource", "Internal"}, {"controlPanel", true}}}));
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
	run(client, "track.add", {{"type", "audio"}}); // plays the input
	run(client, "audio.setDevice", {{"blockSize", 64}, {"outputChannels", 4}, {"input", {{"processor", "sine"}}}});
	run(client, "audio.render", {{"frames", 64}});
	const Json status = run(client, "audio.reset");
	CHECK(status.at("device").at("blockSize") == 64);
	CHECK(status.at("device").at("outputChannels") == 4);
	CHECK(status.at("position") == 0); // a new device
	run(client, "audio.render", {{"frames", 64}});
	CHECK(capture(client, 0, 64) != std::vector<float>(64, 0.0f)); // with its input
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
	Engine engine(makeBackends(), midiBackends(std::move(dummy)));
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
	Engine engine(makeBackends(), midiBackends(std::move(dummy)));
	Client client(engine);
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
}
