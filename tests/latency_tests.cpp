// SPDX-License-Identifier: GPL-3.0-or-later
// I/O latency compensation and the monitoring path (#44): the dummy device's latency and loopback, recorded input
// placed where it was played, and direct monitoring.
#include "engine/audio/transport.h"
#include "engine/document/document.h"
#include "engine/engine.h"
#include "platform/audio/dummydevice.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
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
using gnubase::engine::ObjectId;
using gnubase::engine::Result;

namespace
{
	DeviceFormat format(uint32_t blockSize)
	{
		DeviceFormat result;
		result.blockSize = blockSize;
		return result;
	}

	// Outputs frame f as f + 1 on the first output and -(f + 1) on the second, and keeps the inputs it gets, in
	// memory reserved up front.
	class Count : public AudioCallback
	{
	public:
		explicit Count(size_t frames) : inputs(frames) {}

		void process(const DeviceBlock& block) noexcept override
		{
			for (uint32_t i = 0; i < block.frames; ++i)
			{
				const uint64_t frame = block.position + i;
				block.outputs[0][i] = static_cast<float>(frame + 1);
				block.outputs[1][i] = -static_cast<float>(frame + 1);
				if (frame < inputs.size())
					inputs[frame] = {block.inputs[0][i], block.inputs[1][i]};
			}
		}

		std::vector<std::array<float, 2>> inputs;
	};

	class Silence : public AudioCallback
	{
	public:
		void process(const DeviceBlock& /*block*/) noexcept override {}
	};

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

	ObjectId idOf(const Json& result, const char* kind)
	{
		return result.at(kind).at("id").get<ObjectId>();
	}

	// Frame f of channel c of a ramp file: (f + 1) / 65536, negated on the second channel; exact in float.
	float ramp(int64_t frame, int channel)
	{
		const float value = static_cast<float>(frame + 1) / 65536.0f;
		return channel == 0 ? value : -value;
	}

	std::string writeRamp(const gnubase::test::TempDir& dir, int64_t frames)
	{
		gnubase::test::Audio audio;
		audio.channels = 2;
		for (int64_t f = 0; f < frames; ++f)
			for (int c = 0; c < 2; ++c)
				audio.samples.push_back(ramp(f, c));
		const std::filesystem::path file = dir.path() / "ramp.wav";
		gnubase::test::writeWav(file, audio);
		return file.u8string();
	}

	// A stereo audio track that plays the ramp file from position, without an input: it would hear the loopback.
	void addRamp(Client& client, const std::string& path, int64_t position, int64_t length)
	{
		const Json track = run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}, {"channels", 2}});
		run(client, "channel.set", {{"channel", track.at("channel").at("id")}, {"input", 0}});
		const ObjectId media = idOf(run(client, "media.add", {{"path", path}}), "media");
		const ObjectId clip = idOf(run(client, "clip.add", {{"media", media}}), "clip");
		run(client, "audioEvent.add",
			{{"track", idOf(track, "track")}, {"clip", clip}, {"position", position}, {"offset", 0},
				{"length", length}});
	}

	std::vector<float> recorded(Client& client, int channel, int64_t start, int64_t frames)
	{
		return run(client, "audio.captureInput", {{"channel", channel}, {"start", start}, {"frames", frames}})
			.at("samples")
			.get<std::vector<float>>();
	}

	std::vector<float> captured(Client& client, int channel, int64_t frames)
	{
		return run(client, "audio.capture", {{"channel", channel}, {"start", 0}, {"frames", frames}})
			.at("samples")
			.get<std::vector<float>>();
	}

	// How many of frames frames of what an input channel recorded from start differ from the ramp.
	size_t rampMismatches(Client& client, int channel, int64_t start, int64_t frames)
	{
		const std::vector<float> samples = recorded(client, channel, start, frames);
		REQUIRE(samples.size() == static_cast<size_t>(frames));
		size_t mismatches = 0;
		for (int64_t f = 0; f < frames; ++f)
			if (samples[static_cast<size_t>(f)] != ramp(f, channel))
				++mismatches;
		return mismatches;
	}

	bool silent(const std::vector<float>& samples)
	{
		return std::all_of(samples.begin(), samples.end(), [](float sample) { return sample == 0.0f; });
	}

	// The dummy device, but its driver can't monitor directly.
	class PlainDevice : public DummyDevice
	{
	public:
		PlainDevice() : DummyDevice(DeviceFormat{}, 0) {}

		DeviceInfo info() const override
		{
			DeviceInfo info = DummyDevice::info();
			info.id = "plain";
			info.canMonitorDirectly = false;
			return info;
		}
	};

	class PlainBackend : public Backend
	{
	public:
		std::string name() const override { return "plain"; }
		std::vector<DeviceInfo> devices() override { return {PlainDevice().info()}; }
		std::unique_ptr<Device> open(const DeviceConfig& /*config*/) override
		{
			return std::make_unique<PlainDevice>();
		}
	};

	// A device that learns its latency after it opens, as PipeWire does from its ports' latency parameters: it
	// reports a block each way until learn() tells it more and reports the change.
	class SettlingDevice : public DummyDevice
	{
	public:
		SettlingDevice() : DummyDevice(DeviceFormat{48000.0, 64, 2, 2}, 0) {}

		DeviceInfo info() const override
		{
			DeviceInfo info = DummyDevice::info();
			info.id = "settling";
			return info;
		}

		DeviceLatency latency() const override { return {m_input.load(), m_output.load()}; }

		void learn(DeviceLatency latency)
		{
			m_input = latency.input;
			m_output = latency.output;
			reportLatencyChange();
		}

	private:
		std::atomic<uint32_t> m_input{64};
		std::atomic<uint32_t> m_output{64};
	};

	class SettlingBackend : public Backend
	{
	public:
		explicit SettlingBackend(std::atomic<SettlingDevice*>& opened) : m_opened(opened) {}
		std::string name() const override { return "settling"; }
		std::vector<DeviceInfo> devices() override { return {SettlingDevice().info()}; }
		std::unique_ptr<Device> open(const DeviceConfig& /*config*/) override
		{
			auto device = std::make_unique<SettlingDevice>();
			m_opened = device.get();
			return device;
		}

	private:
		std::atomic<SettlingDevice*>& m_opened;
	};

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
}

TEST_CASE("the dummy device reports its latency and loops its outputs back a round trip later", "[unit]")
{
	DummyDevice device(format(64), 1000);
	CHECK(device.latency().input == 0);
	device.setLatency({100, 60}, true);
	CHECK(device.latency().input == 100);
	CHECK(device.latency().output == 60);

	Count count(1000);
	device.render(count, 1000);
	CHECK(count.inputs[159] == std::array<float, 2>{0.0f, 0.0f});
	CHECK(count.inputs[160] == std::array<float, 2>{1.0f, -1.0f}); // what it output at frame 0
	CHECK(count.inputs[999] == std::array<float, 2>{840.0f, -840.0f});
	CHECK(device.captured(0, 0, 1) == std::vector<float>{1.0f}); // the capture is what the callback output

	// Without loopback the latency is only reported.
	DummyDevice reporting(format(64), 0);
	reporting.setLatency({100, 60});
	Count none(256);
	reporting.render(none, 256);
	CHECK(none.inputs[200] == std::array<float, 2>{0.0f, 0.0f});
}

TEST_CASE("the dummy device can't loop back sooner than a block later", "[unit]")
{
	DummyDevice device(format(64), 0);
	CHECK_THROWS_AS(device.setLatency({32, 31}, true), DeviceError); // the outputs aren't there yet
	device.setLatency({32, 32}, true);
	Count count(256);
	device.render(count, 256);
	CHECK(count.inputs[64] == std::array<float, 2>{1.0f, -1.0f});
}

TEST_CASE("the dummy device monitors its inputs directly, without latency", "[unit]")
{
	DummyDevice device(format(64), 1000);
	CHECK(device.info().canMonitorDirectly);
	device.setInput(
		[](float* const* inputs, int channels, uint32_t frames, uint64_t /*position*/)
		{
			for (int c = 0; c < channels; ++c)
				for (uint32_t i = 0; i < frames; ++i)
					inputs[c][i] = c == 0 ? 0.5f : 0.25f;
		});
	device.monitorInput({0, 1, true, 0.5, -1.0}); // the first input on the second output only, at half the level
	device.monitorInput({1, 0, true, 1.0, 0.0});  // the second input centered on the first two outputs
	Silence silence;
	device.render(silence, 64);
	CHECK(device.captured(0, 10, 1) == std::vector<float>{0.125f});
	CHECK(device.captured(1, 10, 1) == std::vector<float>{0.25f + 0.125f});

	device.monitorInput({0, 1, false, 0.5, -1.0});
	device.render(silence, 64);
	CHECK(device.captured(1, 70, 1) == std::vector<float>{0.125f});
}

TEST_CASE("played positions follow the transport through cycle jumps and stops", "[unit]")
{
	gnubase::engine::PlayedPositions played(16);
	const std::array<gnubase::engine::graph::TransportSegment, 2> segments{{{0, 3, 100}, {3, 5, 10}}};
	gnubase::engine::graph::TransportBlock jump;
	jump.playing = true;
	jump.position = 100;
	jump.segments = segments.data();
	jump.segmentCount = segments.size();
	played.add(0, 8, jump);                                     // plays 100 to 102, then jumps to 10
	played.add(8, 4, gnubase::engine::graph::TransportBlock{}); // stopped
	CHECK(played.at(0) == 100);
	CHECK(played.at(2) == 102);
	CHECK(played.at(3) == 10);
	CHECK(played.at(7) == 14);
	CHECK(played.at(8) == -1);
	CHECK(played.at(11) == -1);
	CHECK(played.at(12) == -1); // not played yet
	CHECK(played.at(-1) == -1);

	const std::array<gnubase::engine::graph::TransportSegment, 1> on{{{0, 8, 50}}};
	gnubase::engine::graph::TransportBlock playing;
	playing.playing = true;
	playing.position = 50;
	playing.segments = on.data();
	playing.segmentCount = on.size();
	played.add(12, 8, playing);
	CHECK(played.at(3) == -1); // more than 16 frames ago
	CHECK(played.at(4) == 11);
	CHECK(played.at(19) == 57);
}

TEST_CASE("a loopback through the dummy device records sample-aligned", "[integration]")
{
	int64_t shift = 0;
	SECTION("where the device's latencies say")
	{
	}
	SECTION("shifted for a driver that reports them wrong")
	{
		shift = 10;
	}

	const gnubase::test::TempDir dir;
	Engine engine;
	Client client(engine);
	Json device = {
		{"blockSize", 64}, {"captureSeconds", 1}, {"inputLatency", 100}, {"outputLatency", 157}, {"loopback", true}};
	if (shift != 0)
		device["recordShift"] = shift;
	const Json status = run(client, "audio.setDevice", device);
	CHECK(status.at("device").at("inputLatency") == 100);
	CHECK(status.at("device").at("outputLatency") == 157);
	CHECK(status.at("device").at("recordShift") == shift);
	CHECK(status.at("device").at("recordOffset") == 257 - shift);
	CHECK(run(client, "audio.get").at("device").at("recordOffset") == 257 - shift);

	addRamp(client, writeRamp(dir, 500), 1000, 500);
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 4800}});

	// The ramp left the outputs at 1000 and came back 257 frames later; it is recorded where it played.
	const std::vector<float> output = captured(client, 0, 1001);
	CHECK(output[999] == 0.0f);
	CHECK(output[1000] == ramp(0, 0));
	const int64_t start = 1000 + shift;
	CHECK(rampMismatches(client, 0, start, 500) == 0);
	CHECK(rampMismatches(client, 1, start, 500) == 0);
	CHECK(recorded(client, 0, start - 1, 1) == std::vector<float>{0.0f});
	CHECK(recorded(client, 1, start + 500, 1) == std::vector<float>{0.0f});
}

TEST_CASE("recorded input lines up across cycle jumps", "[integration]")
{
	const gnubase::test::TempDir dir;
	Engine engine;
	Client client(engine);
	run(client, "audio.setDevice",
		{{"blockSize", 64}, {"captureSeconds", 1}, {"inputLatency", 100}, {"outputLatency", 157}, {"loopback", true}});
	addRamp(client, writeRamp(dir, 100), 1900, 100); // ends at the right locator
	run(client, "transport.set", {{"left", 500}, {"right", 2000}, {"cycle", true}});
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 6000}}); // the way in and two laps

	// The ramp came back after playback had jumped to the left locator: it still goes where it played, not 257
	// frames before where playback was when it came back.
	CHECK(rampMismatches(client, 0, 1900, 100) == 0);
	CHECK(silent(recorded(client, 0, 400, 100)));
}

TEST_CASE(
	"only the transport's playing frames are recorded, and only from the dummy device while it stands", "[integration]")
{
	Engine engine;
	Client client(engine);
	CHECK_THAT(reject(client, "audio.captureInput", {{"channel", 0}, {"start", 0}, {"frames", 1}}),
		ContainsSubstring("no audio device"));
	run(client, "audio.setDevice",
		{{"blockSize", 64}, {"captureSeconds", 1},
			{"input", {{"processor", "sine"}, {"parameters", {{"Level", 0.5}}}}}});
	run(client, "audio.render", {{"frames", 640}}); // stopped: nothing is recorded
	CHECK(silent(recorded(client, 0, 0, 640)));
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 640}});
	const std::vector<float> samples = recorded(client, 1, 0, 640);
	CHECK(!silent(samples));
	CHECK(recorded(client, 1, 47990, 100).size() == 10); // cut to the capture's length
	CHECK_THAT(reject(client, "audio.captureInput", {{"channel", 2}, {"start", 0}, {"frames", 1}}),
		ContainsSubstring("no input channel 2"));
	run(client, "audio.start");
	CHECK_THAT(reject(client, "audio.captureInput", {{"channel", 0}, {"start", 0}, {"frames", 1}}),
		ContainsSubstring("running"));
	run(client, "audio.stop");

	CHECK_THAT(reject(client, "audio.setDevice", {{"blockSize", 256}, {"inputLatency", 100}, {"loopback", true}}),
		ContainsSubstring("at least a block"));
	CHECK_THAT(reject(client, "audio.setDevice", {{"loopback", true}, {"input", {{"processor", "sine"}}}}),
		ContainsSubstring("loopback"));
}

TEST_CASE("direct monitoring routes audio tracks' inputs through the device instead of the graph", "[integration]")
{
	// The same sine on both inputs, through a stereo track at -6 dB: through the graph, or through the device.
	const auto output = [](bool direct)
	{
		Engine engine;
		Client client(engine);
		const Json status = run(client, "audio.setDevice",
			{{"blockSize", 64}, {"captureSeconds", 1}, {"directMonitoring", direct},
				{"input", {{"processor", "sine"}, {"parameters", {{"Level", 0.5}}}}}});
		CHECK(status.at("device").at("canMonitorDirectly") == true);
		CHECK(status.at("device").at("directMonitoring") == direct);
		const Json track = run(client, "track.add", {{"type", "audio"}});
		run(client, "channel.set",
			{{"channel", track.at("channel").at("id")}, {"volume", 20.0 * std::log10(0.5)}, {"monitor", true}});
		run(client, "audio.render", {{"frames", 640}});
		return std::array<std::vector<float>, 2>{captured(client, 0, 640), captured(client, 1, 640)};
	};
	const std::array<std::vector<float>, 2> graph = output(false);
	const std::array<std::vector<float>, 2> device = output(true);
	REQUIRE(device[0].size() == 640);
	CHECK(!silent(device[0]));
	for (size_t c = 0; c < 2; ++c)
		for (size_t i = 320; i < 640; ++i)                            // after the graph's fader has glided to -6 dB
			REQUIRE_THAT(device[c][i], WithinAbs(graph[c][i], 1e-6)); // once, not twice
}

TEST_CASE(
	"direct monitoring pans a mono input with its track and leaves tracks into groups to the graph", "[integration]")
{
	Engine engine;
	Client client(engine);
	run(client, "audio.setDevice",
		{{"blockSize", 64}, {"captureSeconds", 1}, {"directMonitoring", true},
			{"input", {{"processor", "sine"}, {"parameters", {{"Level", 0.5}}}}}});
	const ObjectId mono =
		idOf(run(client, "bus.add", {{"direction", "input"}, {"format", "mono"}, {"ports", {"Input 1"}}}), "bus");
	const Json track = run(client, "track.add", {{"type", "audio"}, {"channels", 1}});
	run(client, "channel.set",
		{{"channel", track.at("channel").at("id")}, {"input", mono}, {"pan", -1.0}, {"monitor", true}});
	run(client, "audio.render", {{"frames", 640}});
	CHECK(!silent(captured(client, 0, 640)));
	CHECK(silent(captured(client, 1, 640)));

	// A group isn't on the device: a track that plays into one is monitored through the graph, and panned there.
	const Json group = run(client, "track.add", {{"type", "group"}});
	run(client, "channel.set",
		{{"channel", track.at("channel").at("id")}, {"output", group.at("channel").at("id")}, {"pan", 1.0}});
	run(client, "audio.render", {{"frames", 640}});
	const std::vector<float> left = run(client, "audio.capture", {{"channel", 0}, {"start", 640}, {"frames", 640}})
										.at("samples")
										.get<std::vector<float>>();
	const std::vector<float> right = run(client, "audio.capture", {{"channel", 1}, {"start", 640}, {"frames", 640}})
										 .at("samples")
										 .get<std::vector<float>>();
	CHECK(silent(std::vector<float>(left.begin() + 320, left.end()))); // after the pan's glide
	CHECK(!silent(right));
}

TEST_CASE("direct monitoring needs a driver that can do it", "[integration]")
{
	std::vector<std::unique_ptr<Backend>> backends;
	backends.push_back(std::make_unique<PlainBackend>());
	backends.push_back(std::make_unique<DummyBackend>());
	Engine engine(std::move(backends));
	Client client(engine);
	CHECK(run(client, "audio.devices").at("backends")[0].at("devices")[0].at("canMonitorDirectly") == false);
	CHECK_THAT(reject(client, "audio.setDevice", {{"backend", "plain"}, {"directMonitoring", true}}),
		ContainsSubstring("can't monitor directly"));
	CHECK(run(client, "audio.get").at("device").is_null());
	CHECK_THAT(reject(client, "audio.setDevice", {{"backend", "plain"}, {"loopback", true}}),
		ContainsSubstring("only the dummy device"));
	CHECK(
		run(client, "audio.setDevice", {{"backend", "plain"}, {"recordShift", -20}}).at("device").at("recordOffset") ==
		20);
}

TEST_CASE("the record offset follows latency a device reports after it opens", "[integration]")
{
	std::atomic<SettlingDevice*> opened{nullptr};
	std::vector<std::unique_ptr<Backend>> backends;
	backends.push_back(std::make_unique<SettlingBackend>(opened));
	backends.push_back(std::make_unique<DummyBackend>());
	Engine engine(std::move(backends));
	Client client(engine);
	CHECK_THAT(reject(client, "audio.updateLatency"), ContainsSubstring("no audio device"));
	const Json device = run(client, "audio.setDevice", {{"backend", "settling"}, {"recordShift", 10}}).at("device");
	CHECK(device.at("recordOffset") == 118); // a block each way
	run(client, "audio.start");

	// The driver learns its latency while the device runs: the engine reads it again and keeps the device running.
	opened.load()->learn({100, 157});
	CHECK(waitForEvent(client, "audio.changed",
		[](const Json& data) { return data.at("device").at("recordOffset") == 247 && data.at("running") == true; }));
	const Json status = run(client, "audio.get");
	CHECK(status.at("device").at("inputLatency") == 100);
	CHECK(status.at("device").at("outputLatency") == 157);
	CHECK(status.at("running") == true);

	// Nothing changed: the same device, stopped, which keeps its position.
	const Json stopped = run(client, "audio.stop");
	const Json same = run(client, "audio.updateLatency");
	CHECK(same.at("device").at("recordOffset") == 247);
	CHECK(same.at("position") == stopped.at("position"));
	CHECK_THAT(reject(client, "audio.updateLatency", {{"inputLatency", 100}}), ContainsSubstring("dummy device"));
}

TEST_CASE("a loopback records sample-aligned after the dummy device's latency changes", "[integration]")
{
	const gnubase::test::TempDir dir;
	Engine engine;
	Client client(engine);
	run(client, "audio.setDevice",
		{{"blockSize", 64}, {"captureSeconds", 1}, {"inputLatency", 32}, {"outputLatency", 32}, {"loopback", true}});
	run(client, "audio.render", {{"frames", 640}});

	// As a driver whose latency settles after it opens: the loopback's round trip grows, and so does the offset.
	const Json status = run(client, "audio.updateLatency", {{"inputLatency", 100}, {"outputLatency", 157}});
	CHECK(status.at("device").at("inputLatency") == 100);
	CHECK(status.at("device").at("outputLatency") == 157);
	CHECK(status.at("device").at("recordOffset") == 257);
	CHECK(status.at("position") == 640);                                      // the same device
	CHECK(run(client, "audio.reset").at("device").at("recordOffset") == 257); // which a reset keeps

	addRamp(client, writeRamp(dir, 500), 1000, 500);
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 4800}});
	CHECK(rampMismatches(client, 0, 1000, 500) == 0);
	CHECK(rampMismatches(client, 1, 1000, 500) == 0);

	CHECK_THAT(reject(client, "audio.updateLatency", {{"inputLatency", 10}, {"outputLatency", 10}}),
		ContainsSubstring("at least a block"));
	CHECK_THAT(reject(client, "audio.updateLatency", {{"inputLatency", 70000}}), ContainsSubstring("65536"));
	run(client, "audio.start");
	CHECK_THAT(reject(client, "audio.updateLatency", {{"inputLatency", 200}}), ContainsSubstring("running"));
	run(client, "audio.stop");
	CHECK(run(client, "audio.get").at("device").at("inputLatency") == 100); // the rejected ones changed nothing
}
