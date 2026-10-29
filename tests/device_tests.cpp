// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/audio/dummydevice.h"
#include "platform/audio/inputfifo.h"
#include "platform/audio/sampleformat.h"
#ifdef _WIN32
#include "platform/audio/wasapi.h"
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

	// Whether a listed device opens and starts. A driver can be installed for hardware that isn't plugged in.
	bool available(Backend& backend, const DeviceInfo& listed)
	{
		try
		{
			std::unique_ptr<Device> device = backend.open(config(listed.id));
			ContractCallback callback(device->format());
			device->start(callback);
			device->stop(); // while the callback is still there
			return true;
		}
		catch (const DeviceError& error)
		{
			WARN("skipped device " << listed.id << ", which doesn't start: " << error.what());
			return false;
		}
	}
}

void checkDeviceContract(Backend& backend, bool skipUnavailable)
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
		if (skipUnavailable && !available(backend, listed))
			continue;

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
// gnubase-tests "[hardware]". Devices that don't start, such as those of drivers for unplugged hardware, are skipped.
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
