// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/audio/dummydevice.h"
#include "platform/rt/realtime.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
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
				ok = ok && block.position == m_next;
			for (int c = 0; c < m_format.inputChannels; ++c)
				ok = ok && block.inputs[c] != nullptr;
			for (int c = 0; c < m_format.outputChannels; ++c)
				for (uint32_t i = 0; i < block.frames; ++i)
					ok = ok && block.outputs[c][i] == 0.0f;
			if (!ok)
				m_violations.fetch_add(1, std::memory_order_relaxed);
			m_next = block.position + block.frames;
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
		uint64_t m_next = 0;
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

	// What every backend promises, for each of its devices: the same suite runs against the dummy backend in CI and
	// against the hardware backends locally.
	void checkContract(Backend& backend)
	{
		INFO("backend " << backend.name());
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

			// Every listed sample rate, block size and clock source opens as asked.
			for (const double rate : info.sampleRates)
				CHECK(backend.open(config(listed.id, rate))->format().sampleRate == rate);
			for (const uint32_t size : info.blockSizes)
				CHECK(backend.open(config(listed.id, 0.0, size))->format().blockSize == size);
			for (size_t i = 0; i < info.clockSources.size(); ++i)
				CHECK(backend.open(config(listed.id, 0.0, 0, info.clockSources[i]))->info().clockSource ==
					static_cast<int>(i));
			CHECK_THROWS_AS(backend.open(config(listed.id, 0.0, 0, "no such clock")), DeviceError);
		}
	}
}

TEST_CASE("the dummy backend keeps the device contract", "[unit]")
{
	DummyBackend backend;
	REQUIRE(backend.devices().size() == 1);
	checkContract(backend);
}

// Hidden: opens real devices and plays silence through them. Run it on a machine with audio hardware:
// gnubase-tests "[hardware]"
TEST_CASE("every hardware backend keeps the device contract", "[.hardware]")
{
	for (const std::unique_ptr<Backend>& backend : makeBackends())
		if (backend->name() != "dummy")
			checkContract(*backend);
}

TEST_CASE("the backends of this build come with the dummy backend last", "[unit]")
{
	const std::vector<std::unique_ptr<Backend>> backends = makeBackends();
	REQUIRE(!backends.empty());
	CHECK(backends.back()->name() == "dummy");
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
