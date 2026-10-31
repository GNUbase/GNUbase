// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/audio/dummydevice.h"
#include "platform/audio/inputfifo.h"
#include "platform/audio/sampleformat.h"
#ifdef _WIN32
#include "platform/audio/wasapi.h"
#endif
#ifdef __APPLE__
#include "platform/audio/coreaudio.h"
#include "platform/rt/workerpool.h"
#endif
#ifdef __linux__
#include "platform/audio/jack.h"
#include "platform/audio/pipewire.h"
#include "platform/process.h"

#include <dlfcn.h>
#include <jack/jack.h>
#endif
#include "devicecontract.h"
#include "engine/document/document.h"
#include "engine/engine.h"
#include "platform/hosttime.h"
#include "platform/rt/realtime.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace gnubase::audio;

namespace
{
	// Checks every block against the device contract and counts the blocks. Records into memory reserved up front, as
	// the RT checker requires.
	class ContractCallback : public AudioCallback
	{
	public:
		explicit ContractCallback(const DeviceFormat& format) : m_format(format) {}

		void process(const DeviceBlock& block) noexcept override
		{
			bool ok = gnubase::rt::isRealtimeContext() && block.frames >= 1 && block.frames <= m_format.blockSize &&
				block.midiOut.empty();
			if (m_blocks > 0)
				ok = ok && block.position == m_next && block.time >= m_lastTime;
			const gnubase::HostTime now = gnubase::hostTime();
			ok = ok && block.time > now - g_second && block.time < now + g_second; // on the host clock
			for (int c = 0; c < m_format.inputChannels; ++c)
				ok = ok && block.inputs[c] != nullptr;
			for (int c = 0; c < m_format.outputChannels; ++c)
				for (uint32_t i = 0; i < block.frames; ++i)
					ok = ok && block.outputs[c][i] == 0.0f;
			if (!ok)
				m_violations.fetch_add(1, std::memory_order_relaxed);
			m_next = block.position + block.frames;
			m_lastTime = block.time;
			m_blocks.fetch_add(1, std::memory_order_release);
		}

		// Waits until count more blocks have arrived; false after five seconds.
		bool waitFor(size_t count) const
		{
			const size_t target = blocks() + count;
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (blocks() < target)
			{
				if (std::chrono::steady_clock::now() > deadline)
					return false;
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
			return true;
		}

		size_t blocks() const { return m_blocks.load(std::memory_order_acquire); }
		size_t violations() const { return m_violations.load(std::memory_order_relaxed); }

	private:
		DeviceFormat m_format;
		static constexpr gnubase::HostTime g_second = 1000000000;

		uint64_t m_next = 0;
		gnubase::HostTime m_lastTime = 0;
		std::atomic<size_t> m_blocks{0};
		std::atomic<size_t> m_violations{0};
	};

	DeviceConfig config(
		const std::string& device, double sampleRate = 0.0, uint32_t blockSize = 0, const std::string& clockSource = {})
	{
		DeviceConfig result;
		result.device = device;
		result.sampleRate = sampleRate;
		result.blockSize = blockSize;
		result.clockSource = clockSource;
		return result;
	}

	bool contains(const std::vector<double>& values, double value)
	{
		return std::find(values.begin(), values.end(), value) != values.end();
	}

	bool contains(const std::vector<uint32_t>& values, uint32_t value)
	{
		return std::find(values.begin(), values.end(), value) != values.end();
	}

	// Opens and starts a device once, whether it can or not: some backends learn only that way that a device is
	// unavailable. Returns why it can't, or nothing.
	std::string tryDevice(Backend& backend, const std::string& id)
	{
		try
		{
			std::unique_ptr<Device> device = backend.open(config(id));
			ContractCallback callback(device->format());
			device->start(callback);
			device->stop(); // while the callback is still there
			return {};
		}
		catch (const DeviceError& error)
		{
			return error.what();
		}
	}
}

void checkDeviceContract(Backend& backend, bool openFirst)
{
	INFO("backend " << backend.name());
	std::map<std::string, std::string> failures; // per device id, why it didn't open or start
	if (openFirst)
		for (const DeviceInfo& listed : backend.devices())
			failures[listed.id] = tryDevice(backend, listed.id);
	const std::vector<DeviceInfo> devices = backend.devices();
	std::set<std::string> ids;
	for (const DeviceInfo& listed : devices)
	{
		CHECK(!listed.id.empty());
		CHECK(!listed.name.empty());
		CHECK(ids.insert(listed.id).second);
	}
	CHECK_THROWS_AS(backend.open(config("no such device")), DeviceError);
	CHECK_THROWS_AS(backend.open(config("", 1.0)), DeviceError); // no device runs at 1 Hz

	for (const DeviceInfo& listed : devices)
	{
		INFO("device " << listed.id);
		if (!listed.unavailableReason.empty())
		{
			WARN("skipped device " << listed.id << ", which is unavailable: " << listed.unavailableReason);
			continue;
		}
		if (const std::string& failure = failures[listed.id]; !failure.empty())
		{
			FAIL_CHECK("listed as available, but doesn't open or start: " << failure);
			continue; // on to the next device
		}

		// With its defaults the device describes itself completely.
		std::unique_ptr<Device> device = backend.open(config(listed.id));
		const DeviceFormat format = device->format();
		const DeviceInfo info = device->info();
		CHECK(info.id == listed.id);
		CHECK(format.sampleRate > 0.0);
		CHECK(format.blockSize > 0);
		CHECK(info.inputs.size() == static_cast<size_t>(format.inputChannels));
		CHECK(info.outputs.size() == static_cast<size_t>(format.outputChannels));
		CHECK(contains(info.sampleRates, format.sampleRate));
		CHECK(contains(info.blockSizes, format.blockSize));
		CHECK(info.clockSource >= (info.clockSources.empty() ? -1 : 0));
		CHECK(info.clockSource < static_cast<int>(info.clockSources.size()));
		const DeviceLatency latency = device->latency();
		CHECK(latency.input < format.sampleRate); // less than a second
		CHECK(latency.output < format.sampleRate);
		CHECK(!device->running());

		// It runs until stopped, and runs again after a restart, its positions counting on.
		ContractCallback callback(format);
		device->start(callback);
		CHECK(device->running());
		CHECK(callback.waitFor(8));
		device->stop();
		CHECK(!device->running());
		const size_t stoppedAt = callback.blocks();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		CHECK(callback.blocks() == stoppedAt);
		CHECK(device->position() > 0);
		device->start(callback);
		CHECK(callback.waitFor(4));
		CHECK(callback.violations() == 0);
		device.reset(); // stops it

		// Every listed sample rate, block size and clock source opens as asked. Block sizes and clock sources at the
		// rate they were listed for: some drivers allow other sizes at other rates, and opening at a rate may change
		// the device's current one.
		for (const double rate : info.sampleRates)
			CHECK(backend.open(config(listed.id, rate))->format().sampleRate == rate);
		for (const uint32_t size : info.blockSizes)
			CHECK(backend.open(config(listed.id, format.sampleRate, size))->format().blockSize == size);
		for (size_t i = 0; i < info.clockSources.size(); ++i)
			CHECK(backend.open(config(listed.id, format.sampleRate, 0, info.clockSources[i]))->info().clockSource ==
				static_cast<int>(i));
		CHECK_THROWS_AS(backend.open(config(listed.id, 0.0, 0, "no such clock")), DeviceError);
	}
}

namespace
{
	using gnubase::engine::Client;
	using gnubase::engine::Json;

	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const gnubase::engine::Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	gnubase::engine::ObjectId idOf(const Json& result, const char* kind)
	{
		return result.at(kind).at("id").get<gnubase::engine::ObjectId>();
	}
}

int64_t checkRoundTrip(std::vector<std::unique_ptr<Backend>> backends, const Loopback& loopback)
{
	constexpr int64_t burst = 4800;  // frames of noise
	constexpr int64_t window = 8192; // how far off the recording is looked for, each way
	constexpr int64_t start = 24000; // where the burst plays: playback has settled by then
	const std::string backend = loopback.device.value("backend", std::string("dummy"));
	INFO("backend " << backend << ", device \"" << loopback.device.value("device", std::string()) << "\", output "
					<< loopback.output << " into input " << loopback.input);

	gnubase::engine::Engine engine(std::move(backends));
	Client client(engine);
	Json parameters = loopback.device;
	parameters["captureSeconds"] = 4; // the input, recorded where it played
	const Json device = run(client, "audio.setDevice", parameters).at("device");
	REQUIRE(loopback.output < device.at("outputChannels").get<int>());
	REQUIRE(loopback.input < device.at("inputChannels").get<int>());

	// A mono track plays the burst on a bus of the output alone. No track monitors, so nothing feeds back.
	const gnubase::test::TempDir dir;
	gnubase::test::Audio noise;
	noise.sampleRate = static_cast<int>(run(client, "project.get").at("sampleRate").get<double>());
	uint32_t state = 367; // the same noise on every platform: a linear congruential generator, from -0.5 to 0.5
	for (int64_t i = 0; i < burst; ++i)
	{
		state = (state * 1664525U) + 1013904223U;
		noise.samples.push_back((static_cast<float>(state >> 8U) / 16777216.0f) - 0.5f);
	}
	const std::filesystem::path file = dir.path() / "noise.wav";
	gnubase::test::writeWav(file, noise);
	const Json port = run(client, "audio.ports").at("outputs").at(static_cast<size_t>(loopback.output)).at("name");
	const gnubase::engine::ObjectId bus = idOf(
		run(client, "bus.add", {{"direction", "output"}, {"format", "mono"}, {"ports", Json::array({port})}}), "bus");
	const Json track = run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}, {"channels", 1}});
	run(client, "channel.set", {{"channel", track.at("channel").at("id")}, {"input", 0}, {"output", bus}});
	const gnubase::engine::ObjectId media = idOf(run(client, "media.add", {{"path", file.u8string()}}), "media");
	run(client, "audioEvent.add",
		{{"track", idOf(track, "track")}, {"clip", idOf(run(client, "clip.add", {{"media", media}}), "clip")},
			{"position", start}, {"offset", 0}, {"length", burst}});

	// Recorded in real time, once the device runs and knows its latency.
	run(client, "audio.start");
	std::this_thread::sleep_for(std::chrono::milliseconds(500));
	run(client, "transport.record");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	for (;;)
	{
		const Json status = run(client, "audio.get").at("device");
		const int64_t end = start + burst + window + status.at("recordOffset").get<int64_t>() +
			(2 * status.at("blockSize").get<int64_t>());
		if (run(client, "transport.get").at("position").get<int64_t>() > end)
			break;
		REQUIRE(std::chrono::steady_clock::now() < deadline);
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	run(client, "transport.stop");
	const Json stopped = run(client, "audio.stop").at("device");
	const std::vector<float> recorded = run(client, "audio.captureInput",
		{{"channel", loopback.input}, {"start", start - window}, {"frames", burst + (2 * window)}})
											.at("samples")
											.get<std::vector<float>>();
	REQUIRE(recorded.size() == static_cast<size_t>(burst + (2 * window)));

	// The burst is where the recording correlates best with it.
	int64_t offset = 0;
	double best = -1.0;
	for (int64_t lag = -window; lag <= window; ++lag)
	{
		double sum = 0.0;
		for (int64_t i = 0; i < burst; ++i)
			sum += static_cast<double>(recorded[static_cast<size_t>(window + lag + i)]) *
				noise.samples[static_cast<size_t>(i)];
		if (sum > best)
		{
			best = sum;
			offset = lag;
		}
	}
	double energy = 0.0;
	double recordedEnergy = 0.0;
	for (int64_t i = 0; i < burst; ++i)
	{
		energy += static_cast<double>(noise.samples[static_cast<size_t>(i)]) * noise.samples[static_cast<size_t>(i)];
		const double sample = recorded[static_cast<size_t>(window + offset + i)];
		recordedEnergy += sample * sample;
	}
	const double gain = best / energy;
	const double similarity = recordedEnergy > 0.0 ? best / std::sqrt(energy * recordedEnergy) : 0.0;
	const int64_t recordOffset = stopped.at("recordOffset").get<int64_t>();
	const int64_t expected = loopback.inGraph ? stopped.at("blockSize").get<int64_t>() - recordOffset : 0;
	const double rate = stopped.at("sampleRate").get<double>();
	INFO("recorded " << offset << " frames (" << static_cast<double>(offset) * 1000.0 / rate
					 << " ms) later than it played, expected " << expected << " +- " << loopback.tolerance
					 << "; record offset " << recordOffset << ", gain " << gain << ", similarity " << similarity);
	REQUIRE(similarity > 0.9); // the burst came back, and not just noise
	CHECK(gain > 0.5);
	CHECK(gain < 2.0);
	CHECK(std::abs(offset - expected) <= loopback.tolerance);
	WARN(backend << " round trip: recorded " << offset << " frames later than it played, record offset "
				 << recordOffset);
	return offset;
}

TEST_CASE("the dummy backend keeps the device contract", "[unit]")
{
	DummyBackend backend;
	REQUIRE(backend.devices().size() == 1);
	checkDeviceContract(backend);
}

namespace
{
	// A sound card with a cable from each output to the input of the same number and latencies that say so: the dummy
	// device's loopback, behind a backend of its own, so that the engine takes it for hardware.
	class CableBackend : public Backend
	{
	public:
		std::string name() const override { return "cable"; }
		std::vector<DeviceInfo> devices() override { return {open(DeviceConfig{})->info()}; }
		std::unique_ptr<Device> open(const DeviceConfig& /*config*/) override
		{
			auto device = std::make_unique<DummyDevice>(DeviceFormat{48000.0, 64, 4, 4}, 0);
			device->setLatency({100, 157}, true);
			return device;
		}
	};

	std::vector<std::unique_ptr<Backend>> cable()
	{
		std::vector<std::unique_ptr<Backend>> backends;
		backends.push_back(std::make_unique<CableBackend>());
		return backends;
	}
}

TEST_CASE("a project plays and records over a loopback cable, and the round trip says how far off", "[integration]")
{
	Loopback loopback;
	loopback.device = {{"backend", "cable"}};
	loopback.output = 2;
	loopback.input = 2;
	CHECK(checkRoundTrip(cable(), loopback) == 0);
	loopback.device["recordShift"] = 10; // as for a driver that reports its latency 10 frames short
	loopback.tolerance = 10;
	CHECK(checkRoundTrip(cable(), loopback) == 10);
}

// Hidden: plays a project through a hardware loopback and records it back (checkRoundTrip), for a machine with a cable
// from an output to an input, or a virtual loopback device. GNUBASE_LOOPBACK names them as
// <backend>,<device id>,<output>,<input>, with channels from 0 and an empty id for the default device, e.g.
// "asio,<driver>,0,0" or "wasapi-exclusive,,2,0" (the id may hold commas); GNUBASE_LOOPBACK_TOLERANCE allows the
// recording that many frames off (default 64). Run it with each backend: ASIO, WASAPI shared and exclusive, CoreAudio.
TEST_CASE("a project plays and records over a hardware loopback", "[.hardware]")
{
	const char* variable = std::getenv("GNUBASE_LOOPBACK");
	if (variable == nullptr)
		SKIP("GNUBASE_LOOPBACK names no loopback");
	const std::string value = variable;
	const size_t afterBackend = value.find(',');
	const size_t beforeInput = value.rfind(',');
	const size_t beforeOutput =
		beforeInput == std::string::npos || beforeInput == 0 ? std::string::npos : value.rfind(',', beforeInput - 1);
	if (afterBackend == std::string::npos || beforeOutput == std::string::npos || beforeOutput <= afterBackend)
		FAIL("GNUBASE_LOOPBACK must be <backend>,<device id>,<output>,<input>, not " << value);
	Loopback loopback;
	loopback.device = {{"backend", value.substr(0, afterBackend)}};
	if (beforeOutput > afterBackend + 1)
		loopback.device["device"] = value.substr(afterBackend + 1, beforeOutput - afterBackend - 1);
	loopback.output = std::stoi(value.substr(beforeOutput + 1, beforeInput - beforeOutput - 1));
	loopback.input = std::stoi(value.substr(beforeInput + 1));
	const char* tolerance = std::getenv("GNUBASE_LOOPBACK_TOLERANCE");
	loopback.tolerance = tolerance != nullptr ? std::stoll(tolerance) : 64;
	checkRoundTrip(makeBackends(), loopback);
}

// Hidden: opens real devices and plays silence through them. Run it on a machine with audio hardware:
// gnubase-tests "[hardware]". Devices listed as unavailable, such as those of drivers for unplugged hardware, are
// skipped.
TEST_CASE("every hardware backend keeps the device contract", "[.hardware]")
{
	// A section per backend, so one runs on its own: gnubase-tests "[hardware]" -c "<backend name>".
	for (const std::unique_ptr<Backend>& backend : makeBackends())
		if (backend->name() != "dummy")
			DYNAMIC_SECTION(backend->name())
			{
				checkDeviceContract(*backend, true);
			}
}

#ifdef _WIN32
// Only lists devices; the contract suite above opens them ("[hardware]").
TEST_CASE("the WASAPI backends list their devices and refuse unknown ones", "[unit]")
{
	for (const bool exclusive : {false, true})
	{
		const std::unique_ptr<Backend> backend = makeWasapiBackend(exclusive);
		INFO("backend " << backend->name());
		for (const DeviceInfo& device : backend->devices())
		{
			CHECK(!device.outputs.empty());
			CHECK(!device.sampleRates.empty());
			CHECK(device.controlPanel);
		}
		CHECK_THROWS_AS(backend->open(config("no such device")), DeviceError);
	}
}
#endif

#ifdef __APPLE__
// Only lists devices; the contract suite above opens them ("[hardware]").
TEST_CASE("the CoreAudio backend lists its devices and refuses unknown ones", "[unit]")
{
	const std::unique_ptr<Backend> backend = makeCoreAudioBackend();
	CHECK(backend->name() == "coreaudio");
	for (const DeviceInfo& device : backend->devices())
	{
		INFO("device " << device.id);
		CHECK(device.inputs.size() + device.outputs.size() > 0);
		CHECK(!device.sampleRates.empty());
		CHECK(std::is_sorted(device.sampleRates.begin(), device.sampleRates.end()));
		CHECK(!device.blockSizes.empty());
		CHECK(device.clockSources.empty());
	}
	CHECK_THROWS_AS(backend->open(config("no such device")), DeviceError);
}

// Hidden: opens the default output device ("[hardware]").
TEST_CASE("worker threads join the audio workgroup of a CoreAudio device", "[.hardware]")
{
	const std::unique_ptr<Device> device = makeCoreAudioBackend()->open(config(""));
	REQUIRE(device->workgroup() != nullptr);
	const gnubase::rt::WorkerPool pool(
		2, device->format().blockSize / device->format().sampleRate, device->workgroup());
	CHECK(pool.workgroupWorkers() == 2);
	const gnubase::rt::WorkerPool alone(2);
	CHECK(alone.workgroupWorkers() == 0);
}

// Hidden: opens every device at every listed rate ("[hardware]"). GNUbase's own rate change must not make the device
// ask for a reset, and its channels are those it has at the rate it opened at, as the listing then shows them.
TEST_CASE("a CoreAudio device opened at another rate keeps still and has that rate's channels", "[.hardware]")
{
	const std::unique_ptr<Backend> backend = makeCoreAudioBackend();
	for (const DeviceInfo& listed : backend->devices())
		for (const double rate : listed.sampleRates)
		{
			INFO("device " << listed.id << " at " << rate << " Hz");
			std::atomic<int> resets{0};
			const std::unique_ptr<Device> device = backend->open(config(listed.id, rate));
			device->onReset([&resets] { resets.fetch_add(1); });
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			CHECK(resets.load() == 0);
			const std::vector<DeviceInfo> now = backend->devices();
			const auto found = std::find_if(
				now.begin(), now.end(), [&listed](const DeviceInfo& info) { return info.id == listed.id; });
			REQUIRE(found != now.end());
			CHECK(device->format().inputChannels == static_cast<int>(found->inputs.size()));
			CHECK(device->format().outputChannels == static_cast<int>(found->outputs.size()));
			CHECK(device->info().inputs == found->inputs);
			CHECK(device->info().outputs == found->outputs);
		}
}
#endif

#ifdef __linux__
namespace
{
	// The PipeWire backend, or nothing if no PipeWire daemon runs here. GNUBASE_REQUIRE_PIPEWIRE=1 says one must, with
	// a sink: CI starts one with a null sink and a null source (tools/start-pipewire.sh).
	std::unique_ptr<Backend> pipeWireOrSkip()
	{
		std::unique_ptr<Backend> backend = makePipeWireBackend();
		const char* require = std::getenv("GNUBASE_REQUIRE_PIPEWIRE");
		const bool required = require != nullptr && std::string(require) == "1";
		try
		{
			if (backend->devices().empty() && required)
				FAIL("the PipeWire daemon has no sink");
		}
		catch (const DeviceError& error)
		{
			if (required)
				FAIL(error.what());
			return nullptr;
		}
		return backend;
	}

	// Runs one of PipeWire's command line tools (pw-cli, pw-metadata). True if it succeeds.
	bool pw(const std::string& tool, const std::vector<std::string>& arguments)
	{
		gnubase::platform::ChildProcess process =
			gnubase::platform::ChildProcess::start("/usr/bin/" + tool, arguments, "/dev/null");
		return process.wait(std::chrono::seconds(10)) == 0;
	}

	// True once done() holds, false if it doesn't within five seconds.
	bool eventually(const std::function<bool()>& done)
	{
		for (int i = 0; i < 100 && !done(); ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		return done();
	}

	// Removes a node when it goes out of scope, also when a check fails.
	struct NodeRemover
	{
		~NodeRemover() { pw("pw-cli", {"destroy", name}); } // fails if it was removed already

		std::string name;
	};

	// Forces a setting of the graph (clock.force-rate, clock.force-quantum) as another client would, and clears it
	// again when it goes out of scope, also when a check fails.
	class ForcedSetting
	{
	public:
		ForcedSetting(std::string key, uint32_t value) : m_key(std::move(key))
		{
			REQUIRE(pw("pw-metadata", {"-n", "settings", "0", m_key, std::to_string(value)}));
		}
		~ForcedSetting() { pw("pw-metadata", {"-n", "settings", "0", m_key, "0"}); }
		ForcedSetting(const ForcedSetting&) = delete;
		ForcedSetting& operator=(const ForcedSetting&) = delete;

	private:
		std::string m_key;
	};

	// The tests that change the graph need PipeWire's command line tools, which come with the daemon.
	bool havePipeWireTools()
	{
		if (std::filesystem::exists("/usr/bin/pw-cli") && std::filesystem::exists("/usr/bin/pw-metadata"))
			return true;
		WARN("no pw-cli and pw-metadata to change the graph with");
		return false;
	}

	// In the contract's test case, so that the contract never meets the sink that comes and goes when ctest runs
	// tests in parallel. A device asks for a reset when the sink gets other ports, e.g. for another channel count,
	// and opens again with those; then when the sink goes away.
	void checkUnplug(const std::unique_ptr<Backend>& backend)
	{
		if (!havePipeWireTools())
			return;
		const std::string sink = "gnubase-test-unplug";
		REQUIRE(pw("pw-cli",
			{"create-node", "adapter",
				"{ factory.name=support.null-audio-sink node.name=" + sink +
					" media.class=Audio/Sink audio.position=[ FL FR ] object.linger=true }"}));
		const NodeRemover remover{sink};
		const auto listed = [&backend, &sink]
		{
			const std::vector<DeviceInfo> devices = backend->devices();
			return std::any_of(
				devices.begin(), devices.end(), [&sink](const DeviceInfo& device) { return device.id == sink; });
		};
		REQUIRE(eventually(listed)); // with ports, which the session manager adds
		std::unique_ptr<Device> device = backend->open(config(sink));
		CHECK(device->format().outputChannels == 2);
		std::atomic<int> resets{0};
		device->onReset([&resets] { ++resets; });
		REQUIRE(pw("pw-cli",
			{"set-param", sink, "PortConfig",
				"{ direction: Input, mode: dsp, format: { mediaType: audio, mediaSubtype: raw, format: F32P, rate: "
				"48000, "
				"channels: 1, position: [ MONO ] } }"}));
		CHECK(eventually([&resets] { return resets > 0; }));
		device.reset();

		device = backend->open(config(sink));
		CHECK(device->info().outputs == std::vector<std::string>{"MONO"});
		resets = 0;
		device->onReset([&resets] { ++resets; });
		REQUIRE(pw("pw-cli", {"destroy", sink}));
		CHECK(eventually([&resets] { return resets > 0; }));
		device.reset();
		CHECK(!listed());
	}
}

TEST_CASE("the PipeWire backend keeps the device contract and asks for a reset when a sink's ports change or it goes "
		  "away",
	"[integration]")
{
	const std::unique_ptr<Backend> backend = pipeWireOrSkip();
	if (!backend)
		SKIP("no PipeWire daemon runs here");
	checkDeviceContract(*backend);
	checkUnplug(backend);
}

// While another client forces the graph to another rate, a device would only play silence: it asks for a reset. Until
// the force is lifted the listing has the forced rate only, and opening at another one says why it can't; a device
// opened without a rate opens at the forced one. When the force is lifted, the device asks for a reset again.
TEST_CASE("a PipeWire device asks for a reset when another client forces the graph to another rate", "[integration]")
{
	std::unique_ptr<Backend> backend = pipeWireOrSkip();
	if (!backend)
		SKIP("no PipeWire daemon runs here");
	if (!havePipeWireTools())
		return;
	SECTION("the device")
	{
		std::unique_ptr<Device> device = backend->open(config("default"));
		const double rate = device->format().sampleRate;
		const uint32_t other = rate != 44100.0 ? 44100 : 48000;
		std::atomic<int> resets{0};
		device->onReset([&resets] { ++resets; });
		ContractCallback callback(device->format());
		device->start(callback);
		REQUIRE(callback.waitFor(8));
		{
			const ForcedSetting forced("clock.force-rate", other);
			CHECK(eventually([&resets] { return resets > 0; }));
			device.reset();
			CHECK(backend->devices().front().sampleRates == std::vector<double>{static_cast<double>(other)});
			CHECK_THROWS_WITH(backend->open(config("default", rate)), Catch::Matchers::ContainsSubstring("forces"));
			CHECK(backend->open(config("default"))->format().sampleRate == other);
		}
		CHECK(eventually(
			[&backend, rate]
			{
				try
				{
					return backend->open(config("default", rate))->format().sampleRate == rate;
				}
				catch (const DeviceError&)
				{
					return false;
				}
			}));
		CHECK(callback.violations() == 0);
	}
	SECTION("the engine, with the device set as the studio setup sets it")
	{
		std::vector<std::unique_ptr<Backend>> backends;
		backends.push_back(std::move(backend));
		gnubase::engine::Engine engine(std::move(backends));
		Client client(engine);
		// Backend and device only, without a sample rate: the device opens at the graph's rate, the project's.
		run(client, "audio.setDevice", {{"backend", "pipewire"}, {"device", "default"}});
		run(client, "audio.start");
		const auto status = [&client] { return run(client, "audio.get"); };
		const Json before = status().at("device");
		REQUIRE(before.at("sampleRateMismatch") == false);
		const double rate = before.at("sampleRate").get<double>();
		const double other = rate != 44100.0 ? 44100.0 : 48000.0;
		const auto runsAt = [&status](double expected, bool mismatch)
		{
			const Json current = status();
			return current.at("running") == true && current.at("lost").is_null() &&
				current.at("device").at("sampleRate").get<double>() == expected &&
				current.at("device").at("sampleRateMismatch") == mismatch;
		};
		{
			// Like any device that doesn't run at the project's rate, it runs at the forced one meanwhile.
			const ForcedSetting forced("clock.force-rate", static_cast<uint32_t>(other));
			CHECK(eventually([&runsAt, other] { return runsAt(other, true); }));
		}
		CHECK(eventually([&runsAt, rate] { return runsAt(rate, false); }));
	}
}

// The latency counts in quanta: when another client changes the graph's quantum while a device runs, the device
// reports a latency change, and the engine moves the record offset with it once, without restarting the device over
// and over (#386).
TEST_CASE("a running PipeWire device reports a latency change when the graph's quantum changes", "[integration]")
{
	std::unique_ptr<Backend> backend = pipeWireOrSkip();
	if (!backend)
		SKIP("no PipeWire daemon runs here");
	if (!havePipeWireTools())
		return;
	SECTION("the device")
	{
		const std::unique_ptr<Device> device = backend->open(config("default"));
		std::atomic<int> changes{0};
		device->onLatencyChange([&changes] { ++changes; });
		ContractCallback callback(device->format());
		device->start(callback);
		REQUIRE(callback.waitFor(8));
		const DeviceLatency before = device->latency();
		const uint32_t quantum = device->format().blockSize != 256 ? 256 : 512;
		const ForcedSetting forced("clock.force-quantum", quantum);
		CHECK(eventually([&changes] { return changes > 0; }));
		CHECK(callback.waitFor(8));
		const DeviceLatency after = device->latency();
		INFO("latency at block size " << device->format().blockSize << ": " << before.input << " in, " << before.output
									  << " out; at quantum " << quantum << ": " << after.input << " in, "
									  << after.output << " out");
		CHECK(after.output != before.output);
		if (device->format().inputChannels > 0)
			CHECK(after.input != before.input);
		CHECK(callback.violations() == 0);
	}
	SECTION("the engine")
	{
		std::vector<std::unique_ptr<Backend>> backends;
		backends.push_back(std::move(backend));
		gnubase::engine::Engine engine(std::move(backends));
		Client client(engine);
		run(client, "audio.setDevice", {{"backend", "pipewire"}, {"device", "default"}, {"recordShift", 10}});
		run(client, "audio.start");
		const auto status = [&client] { return run(client, "audio.get").at("device"); };
		const auto settled = [&status]
		{
			const Json device = status();
			return device.at("recordOffset").get<int64_t>() ==
				device.at("inputLatency").get<int64_t>() + device.at("outputLatency").get<int64_t>() - 10;
		};
		REQUIRE(eventually(settled));
		const int64_t before = status().at("recordOffset").get<int64_t>();
		const uint32_t quantum = status().at("blockSize").get<uint32_t>() != 256 ? 256 : 512;
		const ForcedSetting forced("clock.force-quantum", quantum);
		CHECK(eventually(
			[&settled, &status, before] { return settled() && status().at("recordOffset").get<int64_t>() != before; }));

		// Settled: no more restarts, each of which would send audio.changed.
		while (client.poll())
		{
		}
		std::this_thread::sleep_for(std::chrono::seconds(1));
		int changes = 0;
		while (const std::optional<gnubase::engine::Message> message = client.poll())
		{
			const auto* event = std::get_if<gnubase::engine::Event>(&*message);
			changes += event != nullptr && event->name == "audio.changed" ? 1 : 0;
		}
		CHECK(changes == 0);
		CHECK(run(client, "audio.get").at("running") == true);
	}
}

// tools/start-pipewire.sh adds the loopback: a null sink of media class Audio/Duplex, a device of its own
// (<node>|<node>) whose capture ports are the monitor ports of its playback ports.
TEST_CASE("a project plays and records over a loopback in the PipeWire graph", "[integration]")
{
	std::unique_ptr<Backend> backend = pipeWireOrSkip();
	if (!backend)
		SKIP("no PipeWire daemon runs here");
	const std::string id = "gnubase-test-loopback|gnubase-test-loopback";
	const std::vector<DeviceInfo> devices = backend->devices();
	if (std::none_of(devices.begin(), devices.end(), [&id](const DeviceInfo& device) { return device.id == id; }))
	{
		const char* require = std::getenv("GNUBASE_REQUIRE_PIPEWIRE");
		if (require != nullptr && std::string(require) == "1")
			FAIL("the PipeWire daemon has no loopback node gnubase-test-loopback (tools/start-pipewire.sh)");
		SKIP("the PipeWire daemon has no loopback node gnubase-test-loopback (tools/start-pipewire.sh)");
	}
	Loopback loopback;
	loopback.device = {{"backend", "pipewire"}, {"device", id}};
	loopback.output = 1;
	loopback.input = 1;
	loopback.inGraph = true;
	std::vector<std::unique_ptr<Backend>> backends;
	backends.push_back(std::move(backend));
	checkRoundTrip(std::move(backends), loopback);
}

TEST_CASE("a PipeWire device counts each xrun its driver's clock adds up as a dropout", "[unit]")
{
	gnubase::audio::XrunCounter xruns;
	CHECK(!xruns.cycle(7, 500)); // the baseline: xruns from before the device ran
	CHECK(!xruns.cycle(7, 500));
	CHECK(xruns.cycle(7, 756));
	CHECK(!xruns.cycle(7, 756));
	CHECK(!xruns.cycle(9, 2000)); // another driver's clock
	CHECK(xruns.cycle(9, 2001));
	xruns.restart(); // stopped and started again
	CHECK(!xruns.cycle(9, 3000));
	CHECK(xruns.cycle(9, 3100));
}

TEST_CASE("the PipeWire backend lists sinks with sources and names their channels", "[integration]")
{
	const std::unique_ptr<Backend> backend = pipeWireOrSkip();
	if (!backend)
		SKIP("no PipeWire daemon runs here");
	const std::vector<DeviceInfo> devices = backend->devices();
	for (const DeviceInfo& device : devices)
	{
		INFO("device " << device.id);
		CHECK(!device.outputs.empty());
		CHECK(std::none_of(
			device.outputs.begin(), device.outputs.end(), [](const std::string& name) { return name.empty(); }));
		CHECK(!device.sampleRates.empty());
		CHECK(device.clockSources.empty());
		if (device.id.find('|') != std::string::npos)
			CHECK(!device.inputs.empty()); // the source's capture channels
		else if (device.id != "default")
			CHECK(device.inputs.empty()); // a sink alone
	}
	CHECK(std::any_of(devices.begin(), devices.end(), [](const DeviceInfo& device) { return device.id == "default"; }));
	CHECK_THROWS_AS(backend->open(config("no such sink|no such source")), DeviceError);
	CHECK_THROWS_AS(backend->open(config("", 0.0, 0, "no such clock")), DeviceError);
}
#endif

#ifdef __linux__
namespace
{
	bool required(const char* variable)
	{
		const char* value = std::getenv(variable);
		return value != nullptr && std::string(value) == "1";
	}

	// A JACK backend on this libjack, or nothing if no JACK server runs for it. With requireVariable set to 1, one
	// must: CI starts jackd with its dummy driver (tools/start-jack.sh) and PipeWire (tools/start-pipewire.sh).
	std::unique_ptr<Backend> jackOrSkip(const std::string& library, const char* requireVariable)
	{
		std::unique_ptr<Backend> backend = makeJackBackend(library);
		size_t count = 0;
		try
		{
			count = backend->devices().size(); // not inside REQUIRE, which would catch the error itself
		}
		catch (const DeviceError& error)
		{
			if (required(requireVariable))
				FAIL(error.what());
			return nullptr;
		}
		REQUIRE(count == 1);
		return backend;
	}

	// PipeWire's libjack, where Debian and Ubuntu (pipewire-jack) or Fedora install it; empty if it isn't there.
	std::string pipeWireJack()
	{
#ifdef __x86_64__
		const std::string debian = "/usr/lib/x86_64-linux-gnu/pipewire-0.3/jack/libjack.so.0";
#elif defined(__aarch64__)
		const std::string debian = "/usr/lib/aarch64-linux-gnu/pipewire-0.3/jack/libjack.so.0";
#else
		const std::string debian;
#endif
		for (const std::string& path : {debian, std::string("/usr/lib64/pipewire-0.3/jack/libjack.so.0")})
			if (!path.empty() && std::filesystem::exists(path))
				return path;
		return {};
	}

	// A JACK client with two capture and two playback ports. Physical, they are a sound card's, as its driver has them:
	// PipeWire marks only hardware ports physical, and in CI it has null sinks only. With a cable, each playback port
	// plays into the capture port of the same number, as a loopback cable would.
	class TestClient
	{
	public:
		TestClient(const std::string& library, const char* name, bool physical, bool cable = false)
		{
			m_library = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
			REQUIRE(m_library != nullptr);
			const auto open = function<decltype(&jack_client_open)>("jack_client_open");
			const auto add = function<decltype(&jack_port_register)>("jack_port_register");
			m_close = function<decltype(&jack_client_close)>("jack_client_close");
			m_bufferSize = function<decltype(&jack_get_buffer_size)>("jack_get_buffer_size");
			m_setBufferSize = function<decltype(&jack_set_buffer_size)>("jack_set_buffer_size");
			m_buffer = function<decltype(&jack_port_get_buffer)>("jack_port_get_buffer");
			m_cycleTimes = function<decltype(&jack_get_cycle_times)>("jack_get_cycle_times");
			m_time = function<decltype(&jack_get_time)>("jack_get_time");
			m_client = open(name, JackNoStartServer, nullptr);
			REQUIRE(m_client != nullptr);
			size_t i = 0;
			for (const char* port : {"capture_1", "capture_2", "playback_1", "playback_2"})
			{
				const unsigned long direction = port[0] == 'c' ? JackPortIsOutput : JackPortIsInput;
				m_ports.at(i) = add(m_client, port, JACK_DEFAULT_AUDIO_TYPE,
					direction | (physical ? JackPortIsPhysical : 0) | JackPortIsTerminal, 0);
				REQUIRE(m_ports.at(i) != nullptr);
				++i;
			}
			if (cable)
				REQUIRE(function<decltype(&jack_set_process_callback)>("jack_set_process_callback")(
							m_client, &TestClient::loop, this) == 0);
			REQUIRE(function<decltype(&jack_activate)>("jack_activate")(m_client) == 0);
		}

		~TestClient() { m_close(m_client); } // removes its ports; the library stays loaded, as the backend's

		TestClient(const TestClient&) = delete;
		TestClient& operator=(const TestClient&) = delete;

		// The server's buffer size, as this client sees it.
		uint32_t bufferSize() const { return m_bufferSize(m_client); }

		// Asks the server for a buffer size, as another application would. 0 if it takes it.
		int setBufferSize(uint32_t frames) const { return m_setBufferSize(m_client, frames); }

		// The start of the server's current cycle on the host clock, or 0 if the server doesn't tell. A cycle is every
		// client's, so this works on another client's process thread too (jackd).
		gnubase::HostTime cycleStart() const
		{
			jack_nframes_t frames = 0;
			jack_time_t current = 0;
			jack_time_t next = 0;
			float period = 0.0f;
			if (m_cycleTimes(m_client, &frames, &current, &next, &period) != 0)
				return 0;
			const gnubase::HostTime now = gnubase::hostTime();
			return now - (static_cast<gnubase::HostTime>(m_time() - current) * 1000);
		}

	private:
		template <typename Function> Function function(const char* name)
		{
			const auto result = reinterpret_cast<Function>(dlsym(m_library, name));
			REQUIRE(result != nullptr);
			return result;
		}

		// The cable, on the server's process thread.
		static int loop(jack_nframes_t frames, void* data)
		{
			const auto& self = *static_cast<TestClient*>(data);
			for (size_t c = 0; c < 2; ++c)
				std::memcpy(self.m_buffer(self.m_ports[c], frames), self.m_buffer(self.m_ports[c + 2], frames),
					frames * sizeof(float));
			return 0;
		}

		void* m_library = nullptr;
		jack_client_t* m_client = nullptr;
		std::array<jack_port_t*, 4> m_ports{}; // capture, then playback
		decltype(&jack_client_close) m_close = nullptr;
		decltype(&jack_get_buffer_size) m_bufferSize = nullptr;
		decltype(&jack_set_buffer_size) m_setBufferSize = nullptr;
		decltype(&jack_port_get_buffer) m_buffer = nullptr;
		decltype(&jack_get_cycle_times) m_cycleTimes = nullptr;
		decltype(&jack_get_time) m_time = nullptr;
	};

	// Counts the blocks, a cycle long, that start within 20 µs of the start of the server's cycle: the two clocks are
	// read at slightly different times, and a thread may be preempted between the two reads now and then.
	class CycleCallback : public AudioCallback
	{
	public:
		explicit CycleCallback(const TestClient& clock) : m_clock(clock) {}

		void process(const DeviceBlock& block) noexcept override
		{
			const gnubase::HostTime start = m_clock.cycleStart();
			if (start == 0)
				return;
			if (std::llabs(block.time - start) < 20000)
				m_close.fetch_add(1);
			m_blocks.fetch_add(1);
		}

		int blocks() const { return m_blocks.load(); }
		int close() const { return m_close.load(); }

	private:
		const TestClient& m_clock;
		std::atomic<int> m_blocks{0};
		std::atomic<int> m_close{0};
	};

	// Sets one of PipeWire's settings, e.g. clock.force-rate. True if it succeeds.
	bool pwSetting(const std::string& key, const std::string& value)
	{
		gnubase::platform::ChildProcess process = gnubase::platform::ChildProcess::start(
			"/usr/bin/pw-metadata", {"-n", "settings", "0", key, value}, "/dev/null");
		return process.wait(std::chrono::seconds(10)) == 0;
	}

	// The graph's forced quantum (PipeWire's setting clock.force-quantum), 0 if none is forced.
	uint32_t forcedQuantum()
	{
		// NOLINTNEXTLINE(bugprone-command-processor): a constant command, whose output ChildProcess can't capture
		FILE* pipe = popen("pw-metadata -n settings 0 clock.force-quantum 2>/dev/null", "r");
		if (pipe == nullptr)
			return 0;
		std::string output;
		std::array<char, 256> chunk{};
		while (fgets(chunk.data(), static_cast<int>(chunk.size()), pipe) != nullptr)
			output += chunk.data();
		pclose(pipe);
		const std::string key = "key:'clock.force-quantum' value:'";
		const size_t at = output.find(key);
		return at == std::string::npos ? 0 : static_cast<uint32_t>(std::strtoul(&output[at + key.size()], nullptr, 10));
	}

	// Takes longer than a cycle once, a few blocks in, so that the server reports an xrun.
	class LateCallback : public AudioCallback
	{
	public:
		explicit LateCallback(double seconds) : m_late(static_cast<gnubase::HostTime>(seconds * 1e9)) {}

		void process(const DeviceBlock& /*block*/) noexcept override
		{
			if (m_blocks.fetch_add(1) != 4)
				return;
			m_lateStarted = true;
			const gnubase::HostTime until = gnubase::hostTime() + m_late;
			while (gnubase::hostTime() < until)
			{
			}
			m_lateFinished = true;
		}

		int blocks() const { return m_blocks.load(); }
		bool lateStarted() const { return m_lateStarted; }
		bool lateFinished() const { return m_lateFinished; }

	private:
		gnubase::HostTime m_late;
		std::atomic<int> m_blocks{0};
		std::atomic<bool> m_lateStarted{false};
		std::atomic<bool> m_lateFinished{false};
	};

	// Waits up to two seconds for done().
	bool waitUntil(const std::function<bool()>& done)
	{
		for (int i = 0; i < 200 && !done(); ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		return done();
	}

	// A running device leaves itself alone, but asks for a reset when another client changes the server's buffer size,
	// and keeps its blocks within its block size meanwhile. PipeWire's JACK layer takes buffer size requests as a hint
	// (clock.force-quantum) that it may not follow, so there only the first part is checked.
	void checkJackResets(Backend& backend, bool bufferSizeChanges)
	{
		const std::unique_ptr<Device> device = backend.open(config("default"));
		std::atomic<int> resets{0};
		device->onReset([&resets] { ++resets; });
		ContractCallback callback(device->format());
		device->start(callback);
		REQUIRE(callback.waitFor(8));
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		CHECK(resets == 0);
		if (bufferSizeChanges)
		{
			const std::vector<uint32_t> sizes = device->info().blockSizes;
			const uint32_t other = sizes.back() != device->format().blockSize ? sizes.back() : sizes.front();
			{
				const std::unique_ptr<Device> changer = backend.open(config("default", 0.0, other));
				CHECK(changer->format().blockSize == other);
				CHECK(waitUntil([&resets] { return resets > 0; }));
				CHECK(callback.waitFor(4));
			}
			CHECK(callback.violations() == 0);
		}
		device->stop(); // before the callback goes
	}

	// A device opened at a block size other than the server's asks the server for it and doesn't reset itself once the
	// server follows, which PipeWire's JACK layer does a few cycles late, not even when the graph changes afterwards:
	// the latency, which may follow the buffer size, is the device's new baseline then. Under PipeWire, where the
	// request forces the graph's quantum, closing leaves none forced (#389).
	void checkJackOwnBufferSize(Backend& backend, const std::string& library, bool pipeWire)
	{
		auto other = std::make_unique<TestClient>(library, "gnubase-test-client", false);
		const uint32_t own = other->bufferSize() != 512 ? 512 : 1024;
		{
			const std::unique_ptr<Device> device = backend.open(config("default", 0.0, own));
			std::atomic<int> resets{0};
			device->onReset([&resets] { ++resets; });
			ContractCallback callback(device->format());
			device->start(callback);
			REQUIRE(callback.waitFor(8));
			REQUIRE(waitUntil([&other, own] { return other->bufferSize() == own; }));
			if (pipeWire)
				CHECK(forcedQuantum() == own);
			std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the device's watcher sees the new size
			other.reset(); // removes the client's ports: the graph changes
			std::this_thread::sleep_for(std::chrono::milliseconds(300));
			CHECK(resets == 0);
			CHECK(callback.violations() == 0);
			device->stop();
		}
		if (pipeWire)
			CHECK(waitUntil([] { return forcedQuantum() == 0; }));
	}

	// Closing gives the server its buffer size back only while it still runs at the size the device asked for: a size
	// another client set meanwhile stays (#371).
	void checkJackOtherClientsBufferSize(Backend& backend, const std::string& library)
	{
		const TestClient other(library, "gnubase-test-client", false);
		const uint32_t found = other.bufferSize();
		const uint32_t own = found != 512 ? 512 : 1024;
		const uint32_t theirs = found != 128 ? 128 : 64;
		{
			const std::unique_ptr<Device> device = backend.open(config("default", 0.0, own));
			REQUIRE(waitUntil([&other, own] { return other.bufferSize() == own; }));
			REQUIRE(other.setBufferSize(theirs) == 0);
			REQUIRE(waitUntil([&other, theirs] { return other.bufferSize() == theirs; }));
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
		CHECK(other.bufferSize() == theirs);
		CHECK(other.setBufferSize(found) == 0);
	}

	// A reset the device finds before it has a handler, as between the engine opening it and reading its info and
	// latency, comes once the handler is set (#371).
	void checkJackResetBeforeHandler(Backend& backend, std::unique_ptr<TestClient> hardware)
	{
		const std::unique_ptr<Device> device = backend.open(config("default"));
		ContractCallback callback(device->format());
		device->start(callback);
		REQUIRE(callback.waitFor(4));
		hardware.reset();
		std::this_thread::sleep_for(std::chrono::milliseconds(300)); // the device's watcher finds the ports gone
		std::atomic<int> resets{0};
		device->onReset([&resets] { ++resets; });
		CHECK(waitUntil([&resets] { return resets > 0; }));
		device->stop();
	}

	// The device asks for a reset when the server's sample rate changes, and plays silence rather than calling the
	// engine at the wrong rate (#389). jackd can't change its rate while it runs; PipeWire can (clock.force-rate).
	void checkJackRateChange(Backend& backend)
	{
		const double rate = backend.devices().front().sampleRates.front();
		{
			const std::unique_ptr<Device> device = backend.open(config("default"));
			std::atomic<int> resets{0};
			device->onReset([&resets] { ++resets; });
			ContractCallback callback(device->format());
			device->start(callback);
			REQUIRE(callback.waitFor(4));
			struct RateRestorer
			{
				~RateRestorer() { pwSetting("clock.force-rate", "0"); }
			} const restorer;
			REQUIRE(pwSetting("clock.force-rate", rate != 44100.0 ? "44100" : "48000"));
			CHECK(waitUntil([&resets] { return resets > 0; }));
			const size_t blocks = callback.blocks();
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			CHECK(callback.blocks() <= blocks + 1); // one may have been under way
			device->stop();
		}
		CHECK(waitUntil([&backend, rate] { return backend.devices().front().sampleRates.front() == rate; }));
	}

	// Blocks are stamped with the start of the server's cycle as its filtered clock has it, not with the time the
	// process callback happens to run at (#402).
	void checkJackCycleTimes(Backend& backend, const std::string& library)
	{
		const TestClient clock(library, "gnubase-test-clock", false);
		const std::unique_ptr<Device> device = backend.open(config("default")); // a block per cycle
		CycleCallback callback(clock);
		device->start(callback);
		CHECK(waitUntil([&callback] { return callback.blocks() >= 50; }));
		device->stop();
		CHECK(callback.close() >= callback.blocks() * 9 / 10); // the callback runs tens of µs late
	}

	// The server reports a cycle the device took too long for as an xrun, which the device counts as an overload.
	void checkJackXruns(Backend& backend)
	{
		const std::unique_ptr<Device> device = backend.open(config("default"));
		const double cycle = device->format().blockSize / device->format().sampleRate;
		LateCallback callback(cycle * 2); // much longer, and jackd drops the client
		device->start(callback);
		CHECK(waitUntil([&device] { return device->overloads() > 0; }));
		device->stop(); // before the callback goes out of scope
	}

	// Stopping while the callback is late returns after that block: jackd2 cancels the process thread on deactivation,
	// which must not unwind through the callback.
	void checkJackStopWhileLate(Backend& backend)
	{
		const std::unique_ptr<Device> device = backend.open(config("default"));
		const double cycle = device->format().blockSize / device->format().sampleRate;
		LateCallback callback(cycle * 2);
		device->start(callback);
		for (int i = 0; i < 2000 && !callback.lateStarted(); ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		REQUIRE(callback.lateStarted());
		device->stop();
		CHECK(callback.lateFinished());
		const int blocks = callback.blocks();
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		CHECK(callback.blocks() == blocks);
	}

	// A device that uses the fake hardware's ports asks for a reset when it is unplugged.
	void checkJackUnplug(Backend& backend, std::unique_ptr<TestClient> hardware)
	{
		const std::unique_ptr<Device> device = backend.open(config("default"));
		const std::vector<std::string> outputs = device->info().outputs;
		CHECK(std::find(outputs.begin(), outputs.end(), "gnubase-test-hardware:playback_2") != outputs.end());
		std::atomic<int> resets{0};
		device->onReset([&resets] { ++resets; });
		ContractCallback callback(device->format());
		device->start(callback);
		REQUIRE(callback.waitFor(4));
		hardware.reset();
		CHECK(waitUntil([&resets] { return resets > 0; }));
		device->stop();
	}

	// Plays a project over a sound card whose outputs are cabled to its inputs: a client with physical ports, which the
	// device connects to as to any sound card. The cable is inside the server's graph.
	void checkJackRoundTrip(std::unique_ptr<Backend> backend, const std::string& library)
	{
		const TestClient card(library, "gnubase-test-cable", true, true);
		const DeviceInfo device = backend->devices().front();
		const auto index = [](const std::vector<std::string>& names, const char* name)
		{
			const auto found = std::find(names.begin(), names.end(), name);
			REQUIRE(found != names.end());
			return static_cast<int>(found - names.begin());
		};
		Loopback loopback;
		loopback.device = {{"backend", "jack"}, {"device", "default"}};
		loopback.output = index(device.outputs, "gnubase-test-cable:playback_2");
		loopback.input = index(device.inputs, "gnubase-test-cable:capture_2");
		loopback.inGraph = true;
		std::vector<std::unique_ptr<Backend>> backends;
		backends.push_back(std::move(backend));
		checkRoundTrip(std::move(backends), loopback);
	}
}

// The tests that add and remove physical ports run in the contract's test case, so that the contract never meets
// ports that come and go when ctest runs tests in parallel.
TEST_CASE("the JACK backend keeps the device contract and plays a project over a loopback with jackd", "[integration]")
{
	std::unique_ptr<Backend> backend = jackOrSkip("libjack.so.0", "GNUBASE_REQUIRE_JACK");
	if (!backend)
		SKIP("no JACK server runs here");
	checkDeviceContract(*backend);
	checkJackResets(*backend, true);
	checkJackOwnBufferSize(*backend, "libjack.so.0", false);
	checkJackOtherClientsBufferSize(*backend, "libjack.so.0");
	checkJackXruns(*backend);
	checkJackStopWhileLate(*backend);
	checkJackCycleTimes(*backend, "libjack.so.0");
	checkJackUnplug(*backend, std::make_unique<TestClient>("libjack.so.0", "gnubase-test-hardware", true));
	checkJackResetBeforeHandler(*backend, std::make_unique<TestClient>("libjack.so.0", "gnubase-test-hardware", true));
	checkJackRoundTrip(std::move(backend), "libjack.so.0");
}

TEST_CASE("the JACK backend keeps the device contract and plays a project over a loopback with PipeWire's JACK layer",
	"[integration]")
{
	// GNUbase's clients ask for buffer sizes the way jack_bufsize does, through the graph's clock.force-quantum, which
	// outlasts the client (#389); by default each client forces its own node's quantum, which goes with it.
	struct GlobalBufferSize
	{
		GlobalBufferSize() { setenv("PIPEWIRE_PROPS", "{ jack.global-buffer-size = true }", 1); }
		~GlobalBufferSize() { unsetenv("PIPEWIRE_PROPS"); }
		GlobalBufferSize(const GlobalBufferSize&) = delete;
		GlobalBufferSize& operator=(const GlobalBufferSize&) = delete;
	} const globalBufferSize;
	const std::string library = pipeWireJack();
	if (library.empty() && required("GNUBASE_REQUIRE_PIPEWIRE"))
		FAIL("PipeWire's libjack isn't installed (pipewire-jack)");
	std::unique_ptr<Backend> backend = library.empty() ? nullptr : jackOrSkip(library, "GNUBASE_REQUIRE_PIPEWIRE");
	if (!backend)
		SKIP("no PipeWire daemon with its JACK layer runs here");
	auto hardware = std::make_unique<TestClient>(library, "gnubase-test-hardware", true);
	checkDeviceContract(*backend);
	checkJackResets(*backend, false);
	checkJackOwnBufferSize(*backend, library, true);
	checkJackRateChange(*backend);
	checkJackUnplug(*backend, std::move(hardware));
	checkJackResetBeforeHandler(*backend, std::make_unique<TestClient>(library, "gnubase-test-hardware", true));
	checkJackRoundTrip(std::move(backend), library);
}

TEST_CASE("the JACK backend lists the server's physical ports and refuses what the server doesn't do", "[integration]")
{
	const std::unique_ptr<Backend> backend = jackOrSkip("libjack.so.0", "GNUBASE_REQUIRE_JACK");
	if (!backend)
		SKIP("no JACK server runs here");
	const DeviceInfo device = backend->devices().front();
	CHECK(device.id == "default");
	CHECK(!device.outputs.empty()); // e.g. system:playback_1
	CHECK(device.sampleRates.size() == 1);
	CHECK(device.clockSources.empty());
	const double rate = device.sampleRates.front();
	CHECK_THROWS_AS(backend->open(config("default", rate * 2)), DeviceError);
	CHECK_THROWS_AS(backend->open(config("default", rate, 16384)), DeviceError);
}

TEST_CASE("the JACK backend reports a missing libjack as a device error", "[unit]")
{
	const std::unique_ptr<Backend> backend = makeJackBackend("libjack-that-isnt-there.so");
	CHECK(backend->name() == "jack");
	CHECK_THROWS_AS(backend->devices(), DeviceError);
	CHECK_THROWS_AS(backend->open(config("")), DeviceError);
}
#endif

TEST_CASE("the backends of this build come with the dummy backend last", "[unit]")
{
	const std::vector<std::unique_ptr<Backend>> backends = makeBackends();
	REQUIRE(!backends.empty());
	CHECK(backends.back()->name() == "dummy");
#ifdef __linux__
	CHECK(backends.front()->name() == "pipewire");
	CHECK(backends[1]->name() == "jack");
#endif
	std::set<std::string> names;
	for (const std::unique_ptr<Backend>& backend : backends)
		CHECK(names.insert(backend->name()).second);
}

TEST_CASE("the dummy device names its channels and keeps the format it was given", "[unit]")
{
	DeviceFormat format;
	format.sampleRate = 22050.0;
	format.blockSize = 100;
	format.inputChannels = 1;
	format.outputChannels = 3;
	const DummyDevice device(format, 0);
	const DeviceInfo info = device.info();
	CHECK(info.id == "dummy");
	CHECK(info.inputs == std::vector<std::string>{"Input 1"});
	CHECK(info.outputs == std::vector<std::string>{"Output 1", "Output 2", "Output 3"});
	CHECK(contains(info.sampleRates, 22050.0));
	CHECK(contains(info.blockSizes, 100U));
	CHECK(info.clockSources.empty());
	CHECK(!info.controlPanel);
	CHECK(device.workgroup() == nullptr); // only CoreAudio devices have one
}

TEST_CASE("a device passes its driver's reset requests on", "[unit]")
{
	DummyDevice device(DeviceFormat{}, 0);
	CHECK(!device.requestReset()); // nobody listens: nothing happens, and the driver learns so
	int resets = 0;
	device.onReset([&resets] { ++resets; });
	CHECK(device.requestReset());
	CHECK(resets == 1);
}

// Drivers ask for resets on threads of their own, e.g. WASAPI's endpoint notifications, also while the engine sets the
// handler (#316). Each handler owns memory that replacing it frees, so a race shows under the thread sanitizer.
TEST_CASE("a device takes a new reset handler while its driver asks for resets", "[unit]")
{
	DummyDevice device(DeviceFormat{}, 0);
	std::atomic<int> resets{0};
	std::atomic<bool> done{false};
	std::thread driver(
		[&device, &done]
		{
			while (!done.load())
				device.requestReset();
		});
	while (resets.load() < 1000)
	{
		auto counter = std::make_shared<std::atomic<int>*>(&resets);
		device.onReset([counter] { ++**counter; });
	}
	done = true;
	driver.join();
	CHECK(resets.load() >= 1000);
}

TEST_CASE("driver samples convert to float and back in every sample format", "[unit]")
{
	const std::array<float, 6> left{0.0f, 0.5f, -0.5f, 1.0f, -1.0f, 0.25f};
	const std::array<float, 6> right{-0.25f, 0.75f, 2.0f, -2.0f, 0.125f, 0.0f}; // out of range: clipped
	const std::array<const float*, 2> planar{left.data(), right.data()};
	constexpr uint32_t frames = 6;
	for (const SampleFormat format :
		{SampleFormat::Float32, SampleFormat::Int16, SampleFormat::Int24, SampleFormat::Int32})
	{
		INFO("format " << static_cast<int>(format));
		std::vector<uint8_t> interleaved(bytesPerSample(format) * 2 * left.size());
		fromFloat(planar.data(), 2, frames, format, interleaved.data());
		std::array<float, 6> backLeft{};
		std::array<float, 6> backRight{};
		const std::array<float*, 2> back{backLeft.data(), backRight.data()};
		toFloat(interleaved.data(), format, 2, frames, back.data());
		const bool integer = format != SampleFormat::Float32;
		for (size_t i = 0; i < left.size(); ++i)
		{
			// Integer formats end just below 1.0.
			const float expectedLeft = integer ? std::clamp(left[i], -1.0f, 0.99997f) : left[i];
			const float expectedRight = integer ? std::clamp(right[i], -1.0f, 0.99997f) : right[i];
			CHECK(std::abs(backLeft[i] - expectedLeft) < 0.0001f);
			CHECK(std::abs(backRight[i] - expectedRight) < 0.0001f);
		}
	}

	// Int24 is packed little-endian: 0.5 is 0x400000.
	const float half = 0.5f;
	const float* channel = &half;
	std::array<uint8_t, 3> packed{};
	fromFloat(&channel, 1, 1, SampleFormat::Int24, packed.data());
	CHECK(packed == std::array<uint8_t, 3>{0x00, 0x00, 0x40});
}

namespace
{
	// A mono input FIFO whose samples count the frames written, so drops and repeats show.
	struct FifoRun
	{
		FifoRun(uint32_t target, uint32_t slack) : fifo(1, 8192, target, slack) {}

		void write(uint32_t frames)
		{
			std::vector<float> samples(frames);
			for (float& sample : samples)
				sample = static_cast<float>(++written);
			const std::vector<const float*> channels{samples.data()};
			fifo.write(channels.data(), frames);
		}

		std::vector<float> read(uint32_t frames)
		{
			std::vector<float> samples(frames);
			const std::vector<float*> channels{samples.data()};
			fifo.read(channels.data(), frames);
			return samples;
		}

		InputFifo fifo;
		uint64_t written = 0;
	};
}

TEST_CASE("the input FIFO starts once its target is buffered and pads an underrun with silence", "[unit]")
{
	FifoRun run(256, 8192); // no drift correction
	run.write(200);
	CHECK(run.read(128) == std::vector<float>(128, 0.0f)); // not filled up yet
	run.write(100);
	const std::vector<float> first = run.read(128);
	CHECK(first.front() == 1.0f);
	CHECK(first.back() == 128.0f);
	CHECK(run.read(128).front() == 129.0f);
	const std::vector<float> last = run.read(128); // 44 frames left
	CHECK(last[43] == 300.0f);
	CHECK(last[44] == 0.0f);
	CHECK(run.fifo.underruns() == 1);
	CHECK(run.fifo.overruns() == 0);
	run.write(8193); // one more than fits
	CHECK(run.fifo.overruns() == 1);
}

TEST_CASE("the input FIFO follows a faster or slower input clock without running dry or over", "[unit]")
{
	for (const uint32_t extra : {481U, 479U}) // one block in ten: about 200 ppm of drift, more than real clocks have
	{
		INFO(extra << " frames in every tenth block");
		FifoRun run(960, 32);
		float previous = 0.0f;
		bool inOrder = true; // each frame follows the one before, or repeats it, or skips one
		for (int block = 0; block < 20000; ++block)
		{
			run.write(block % 10 == 0 ? extra : 480);
			for (const float sample : run.read(480))
			{
				const float step = sample - previous;
				inOrder = inOrder && (previous == 0.0f || step == 0.0f || step == 1.0f || step == 2.0f);
				previous = sample;
			}
		}
		CHECK(inOrder);
		CHECK(run.fifo.underruns() == 0);
		CHECK(run.fifo.overruns() == 0);
		run.write(480);
		CHECK(run.fifo.size() > 960 - 64); // before the next read: around the target
		CHECK(run.fifo.size() < 960 + 64);
		CHECK((extra > 480 ? run.fifo.drops() : run.fifo.repeats()) > 0);
	}
}
