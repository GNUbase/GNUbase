// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/engine.h"
#include "platform/audio/dummydevice.h"
#include "platform/rt/realtime.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

using namespace gnubase::audio;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using gnubase::engine::Client;
using gnubase::engine::Engine;
using gnubase::engine::Json;
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
	CHECK_FALSE(device.running());

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
	CHECK(status.at("device") ==
		Json{{"type", "dummy"}, {"sampleRate", 44100.0}, {"blockSize", 32}, {"inputChannels", 2},
			{"outputChannels", 6}});
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
