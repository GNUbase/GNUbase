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
#include "platform/hosttime.h"
#include "platform/rt/realtime.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
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

TEST_CASE("the dummy backend keeps the device contract", "[unit]")
{
	DummyBackend backend;
	REQUIRE(backend.devices().size() == 1);
	checkDeviceContract(backend);
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

	// Runs PipeWire's command line tool. True if it succeeds.
	bool pwCli(const std::vector<std::string>& arguments)
	{
		gnubase::platform::ChildProcess process =
			gnubase::platform::ChildProcess::start("/usr/bin/pw-cli", arguments, "/dev/null");
		return process.wait(std::chrono::seconds(10)) == 0;
	}

	// Removes a node when it goes out of scope, also when a check fails.
	struct NodeRemover
	{
		~NodeRemover() { pwCli({"destroy", name}); } // fails if it was removed already

		std::string name;
	};

	// In the contract's test case, so that the contract never meets the sink that comes and goes when ctest runs
	// tests in parallel.
	void checkUnplug(const std::unique_ptr<Backend>& backend)
	{
		if (!std::filesystem::exists("/usr/bin/pw-cli"))
		{
			WARN("no pw-cli to add and remove a sink with");
			return;
		}
		const std::string sink = "gnubase-test-unplug";
		REQUIRE(pwCli({"create-node", "adapter",
			"{ factory.name=support.null-audio-sink node.name=" + sink +
				" media.class=Audio/Sink audio.position=[ FL FR ] object.linger=true }"}));
		const NodeRemover remover{sink};
		const auto listed = [&backend, &sink]
		{
			const std::vector<DeviceInfo> devices = backend->devices();
			return std::any_of(
				devices.begin(), devices.end(), [&sink](const DeviceInfo& device) { return device.id == sink; });
		};
		for (int i = 0; i < 100 && !listed(); ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		REQUIRE(listed()); // with ports, which the session manager adds
		std::unique_ptr<Device> device = backend->open(config(sink));
		std::atomic<int> resets{0};
		device->onReset([&resets] { ++resets; });
		REQUIRE(pwCli({"destroy", sink}));
		for (int i = 0; i < 100 && resets == 0; ++i)
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		CHECK(resets > 0);
		device.reset();
		CHECK(!listed());
	}
}

TEST_CASE("the PipeWire backend keeps the device contract and asks for a reset when a sink goes away", "[integration]")
{
	const std::unique_ptr<Backend> backend = pipeWireOrSkip();
	if (!backend)
		SKIP("no PipeWire daemon runs here");
	checkDeviceContract(*backend);
	checkUnplug(backend);
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
	// PipeWire marks only hardware ports physical, and in CI it has null sinks only.
	class TestClient
	{
	public:
		TestClient(const std::string& library, const char* name, bool physical)
		{
			m_library = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
			REQUIRE(m_library != nullptr);
			const auto open = function<decltype(&jack_client_open)>("jack_client_open");
			const auto add = function<decltype(&jack_port_register)>("jack_port_register");
			m_close = function<decltype(&jack_client_close)>("jack_client_close");
			m_bufferSize = function<decltype(&jack_get_buffer_size)>("jack_get_buffer_size");
			m_client = open(name, JackNoStartServer, nullptr);
			REQUIRE(m_client != nullptr);
			for (const char* port : {"capture_1", "capture_2", "playback_1", "playback_2"})
			{
				const unsigned long direction = port[0] == 'c' ? JackPortIsOutput : JackPortIsInput;
				REQUIRE(add(m_client, port, JACK_DEFAULT_AUDIO_TYPE,
							direction | (physical ? JackPortIsPhysical : 0) | JackPortIsTerminal, 0) != nullptr);
			}
			REQUIRE(function<decltype(&jack_activate)>("jack_activate")(m_client) == 0);
		}

		~TestClient() { m_close(m_client); } // removes its ports; the library stays loaded, as the backend's

		TestClient(const TestClient&) = delete;
		TestClient& operator=(const TestClient&) = delete;

		// The server's buffer size, as this client sees it.
		uint32_t bufferSize() const { return m_bufferSize(m_client); }

	private:
		template <typename Function> Function function(const char* name)
		{
			const auto result = reinterpret_cast<Function>(dlsym(m_library, name));
			REQUIRE(result != nullptr);
			return result;
		}

		void* m_library = nullptr;
		jack_client_t* m_client = nullptr;
		decltype(&jack_client_close) m_close = nullptr;
		decltype(&jack_get_buffer_size) m_bufferSize = nullptr;
	};

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
		if (!bufferSizeChanges)
			return;

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

	// A device opened at a block size other than the server's asks the server for it and doesn't reset itself once the
	// server follows, which PipeWire's JACK layer does a few cycles late, not even when the graph changes afterwards:
	// the latency, which may follow the buffer size, is the device's new baseline then.
	void checkJackOwnBufferSize(Backend& backend, const std::string& library)
	{
		auto other = std::make_unique<TestClient>(library, "gnubase-test-client", false);
		const uint32_t own = other->bufferSize() != 512 ? 512 : 1024;
		const std::unique_ptr<Device> device = backend.open(config("default", 0.0, own));
		std::atomic<int> resets{0};
		device->onReset([&resets] { ++resets; });
		ContractCallback callback(device->format());
		device->start(callback);
		REQUIRE(callback.waitFor(8));
		REQUIRE(waitUntil([&other, own] { return other->bufferSize() == own; }));
		std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the device's watcher sees the new size
		other.reset();                                               // removes the client's ports: the graph changes
		std::this_thread::sleep_for(std::chrono::milliseconds(300));
		CHECK(resets == 0);
		CHECK(callback.violations() == 0);
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
	}
}

TEST_CASE("the JACK backend keeps the device contract with jackd", "[integration]")
{
	const std::unique_ptr<Backend> backend = jackOrSkip("libjack.so.0", "GNUBASE_REQUIRE_JACK");
	if (!backend)
		SKIP("no JACK server runs here");
	checkDeviceContract(*backend);
	checkJackResets(*backend, true);
	checkJackOwnBufferSize(*backend, "libjack.so.0");
	checkJackXruns(*backend);
	checkJackStopWhileLate(*backend);
	checkJackUnplug(*backend, std::make_unique<TestClient>("libjack.so.0", "gnubase-test-hardware", true));
}

TEST_CASE("the JACK backend keeps the device contract with PipeWire's JACK layer", "[integration]")
{
	const std::string library = pipeWireJack();
	if (library.empty() && required("GNUBASE_REQUIRE_PIPEWIRE"))
		FAIL("PipeWire's libjack isn't installed (pipewire-jack)");
	const std::unique_ptr<Backend> backend =
		library.empty() ? nullptr : jackOrSkip(library, "GNUBASE_REQUIRE_PIPEWIRE");
	if (!backend)
		SKIP("no PipeWire daemon with its JACK layer runs here");
	auto hardware = std::make_unique<TestClient>(library, "gnubase-test-hardware", true);
	checkDeviceContract(*backend);
	checkJackResets(*backend, false);
	checkJackOwnBufferSize(*backend, library);
	checkJackUnplug(*backend, std::move(hardware));
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
	device.requestReset(); // nobody listens: nothing happens
	int resets = 0;
	device.onReset([&resets] { ++resets; });
	device.requestReset();
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
