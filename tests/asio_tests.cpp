// SPDX-License-Identifier: GPL-3.0-or-later
// The ASIO backend against a fake driver, which behaves like a driver loaded through COM: the backend's own code runs,
// only the hardware is missing.
#include "devicecontract.h"
#include "platform/audio/asiohost.h"

#define NOMINMAX
#include <windows.h>

#include <iasiodrv.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace gnubase::audio;

namespace
{
	// Two inputs of 32-bit integers and two outputs of 32-bit floats, 44.1 or 48 kHz, blocks of 64 to 1024 frames in
	// powers of two (256 preferred), two clock sources. Its inputs count frames.
	class FakeDriver : public IASIO
	{
	public:
		~FakeDriver() { FakeDriver::stop(); }

		// The test owns the driver; references are only counted.
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID /*id*/, void** object) override
		{
			*object = nullptr;
			return E_NOINTERFACE;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
		ULONG STDMETHODCALLTYPE Release() override { return --references; }

		ASIOBool init(void* /*window*/) override
		{
			rate = 48000.0;
			clock = 0;
			return ASIOTrue;
		}
		void getDriverName(char* name) override { std::snprintf(name, 32, "Fake"); }
		long getDriverVersion() override { return 1; }
		void getErrorMessage(char* text) override { std::snprintf(text, 124, "fake failure"); }

		ASIOError start() override
		{
			running = true;
			thread = std::thread([this] { run(); });
			return ASE_OK;
		}

		ASIOError stop() override
		{
			if (!thread.joinable())
				return ASE_OK;
			running = false;
			thread.join();
			return ASE_OK;
		}

		ASIOError getChannels(long* inputs, long* outputs) override
		{
			*inputs = 2;
			*outputs = 2;
			return ASE_OK;
		}

		ASIOError getLatencies(long* input, long* output) override
		{
			*input = 64;
			*output = 96;
			return ASE_OK;
		}

		ASIOError getBufferSize(long* minimum, long* maximum, long* preferred, long* granularity) override
		{
			*minimum = 64;
			*maximum = 1024;
			*preferred = 256;
			*granularity = -1;
			return ASE_OK;
		}

		ASIOError canSampleRate(ASIOSampleRate sampleRate) override
		{
			return sampleRate == 44100.0 || sampleRate == 48000.0 ? ASE_OK : ASE_NoClock;
		}

		ASIOError getSampleRate(ASIOSampleRate* sampleRate) override
		{
			*sampleRate = rate;
			return ASE_OK;
		}

		ASIOError setSampleRate(ASIOSampleRate sampleRate) override
		{
			rate = sampleRate;
			return ASE_OK;
		}

		ASIOError getClockSources(ASIOClockSource* clocks, long* count) override
		{
			const char* const names[] = {"Internal", "ADAT"};
			*count = std::min(*count, 2L);
			for (long i = 0; i < *count; ++i)
			{
				clocks[i] = {};
				clocks[i].index = i;
				clocks[i].isCurrentSource = i == clock ? ASIOTrue : ASIOFalse;
				std::snprintf(clocks[i].name, sizeof(clocks[i].name), "%s", names[i]);
			}
			return ASE_OK;
		}

		ASIOError setClockSource(long reference) override
		{
			clock = reference;
			return ASE_OK;
		}

		ASIOError getSamplePosition(ASIOSamples* /*position*/, ASIOTimeStamp* /*time*/) override { return ASE_OK; }

		ASIOError getChannelInfo(ASIOChannelInfo* info) override
		{
			info->isActive = ASIOTrue;
			info->channelGroup = 0;
			info->type = info->isInput == ASIOTrue ? sampleType : ASIOSTFloat32LSB;
			std::snprintf(
				info->name, sizeof(info->name), "%s %ld", info->isInput == ASIOTrue ? "In" : "Out", info->channel + 1);
			return ASE_OK;
		}

		ASIOError createBuffers(ASIOBufferInfo* infos, long channels, long size, ASIOCallbacks* asioCallbacks) override
		{
			buffers.assign(static_cast<size_t>(channels) * 2, std::vector<int32_t>(static_cast<size_t>(size)));
			for (long c = 0; c < channels; ++c)
				for (size_t half = 0; half < 2; ++half)
					infos[c].buffers[half] = buffers[(static_cast<size_t>(c) * 2) + half].data();
			blockSize = size;
			callbacks = asioCallbacks;
			return ASE_OK;
		}

		ASIOError disposeBuffers() override
		{
			buffers.clear();
			return ASE_OK;
		}

		ASIOError controlPanel() override
		{
			++panels;
			return ASE_OK;
		}

		ASIOError future(long /*selector*/, void* /*opt*/) override { return ASE_NotPresent; }

		ASIOError outputReady() override
		{
			++readies;
			return ASE_OK;
		}

		// A buffer switch every block duration. Input channel c of frame f holds (f % 1000 + 1) * (c + 1) * 65536.
		void run()
		{
			ASIOTime time{};
			long index = 0;
			int32_t frame = 0;
			auto next = std::chrono::steady_clock::now();
			while (running)
			{
				for (size_t c = 0; c < 2; ++c)
				{
					std::vector<int32_t>& buffer = buffers[(c * 2) + static_cast<size_t>(index)];
					for (size_t i = 0; i < buffer.size(); ++i)
						buffer[i] =
							(((frame + static_cast<int32_t>(i)) % 1000) + 1) * static_cast<int32_t>(c + 1) * 65536;
				}
				callbacks->bufferSwitchTimeInfo(&time, index, ASIOFalse);
				frame += blockSize;
				index ^= 1;
				next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
					std::chrono::duration<double>(blockSize / rate));
				std::this_thread::sleep_until(next);
			}
		}

		std::atomic<long> references{0};
		std::atomic<int> panels{0};
		std::atomic<int> readies{0};
		std::atomic<bool> running{false};
		ASIOSampleType sampleType = ASIOSTInt32LSB;
		double rate = 48000.0;
		long clock = 0;
		long blockSize = 0;
		ASIOCallbacks* callbacks = nullptr;
		std::vector<std::vector<int32_t>> buffers; // two halves per channel, inputs first
		std::thread thread;
	};

	AsioDrivers fake(FakeDriver& driver)
	{
		return {[] { return std::vector<std::string>{"Fake ASIO"}; },
			[&driver](const std::string& name)
			{
				if (name != "Fake ASIO")
					return static_cast<IASIO*>(nullptr);
				driver.AddRef();
				return static_cast<IASIO*>(&driver);
			}};
	}

	// Copies the inputs to the outputs and remembers the first block.
	class Copy : public AudioCallback
	{
	public:
		void process(const DeviceBlock& block) noexcept override
		{
			for (int c = 0; c < 2; ++c)
				std::memcpy(block.outputs[c], block.inputs[c], block.frames * sizeof(float));
			if (blocks.load() == 0)
				first = block.inputs[1][0];
			++blocks;
		}

		std::atomic<int> blocks{0};
		float first = 0.0f;
	};
}

TEST_CASE("the ASIO backend keeps the device contract with a driver", "[unit]")
{
	FakeDriver driver;
	const std::unique_ptr<Backend> backend = makeAsioBackend(fake(driver));
	checkDeviceContract(*backend);
	CHECK(driver.references == 0); // every driver loaded was released
}

TEST_CASE("the ASIO backend converts the driver's samples and passes its requests on", "[unit]")
{
	FakeDriver driver;
	const std::unique_ptr<Backend> backend = makeAsioBackend(fake(driver));
	std::unique_ptr<Device> device = backend->open({});
	CHECK(device->info().inputs == std::vector<std::string>{"In 1", "In 2"});
	CHECK(device->info().clockSources == std::vector<std::string>{"Internal", "ADAT"});
	CHECK(device->latency().input == 64);
	CHECK(device->latency().output == 96);
	CHECK_THROWS_AS(backend->open({}), DeviceError); // one driver at a time

	Copy copy;
	device->start(copy);
	while (copy.blocks < 4)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	device->stop();
	CHECK(copy.first == 2.0f * 65536.0f / 2147483648.0f); // the second input's first frame
	CHECK(driver.readies > 0);
	// The outputs went back to the driver as floats: the second output (buffers 6 and 7) holds the second input.
	for (size_t half = 0; half < 2; ++half)
	{
		float sample = 0.0f;
		std::memcpy(&sample, driver.buffers[6 + half].data(), sizeof(sample));
		CHECK(sample > 0.0f);
	}

	int resets = 0;
	device->onReset([&resets] { ++resets; });
	CHECK(driver.callbacks->asioMessage(kAsioSelectorSupported, kAsioResetRequest, nullptr, nullptr) == 1);
	CHECK(driver.callbacks->asioMessage(kAsioEngineVersion, 0, nullptr, nullptr) == 2);
	driver.callbacks->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
	driver.callbacks->asioMessage(kAsioBufferSizeChange, 0, nullptr, nullptr);
	driver.callbacks->sampleRateDidChange(44100.0);
	CHECK(resets == 3);

	device->openControlPanel();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (driver.panels == 0 && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	CHECK(driver.panels == 1);

	device.reset();
	CHECK(driver.references == 0);
}

TEST_CASE("the ASIO backend refuses sample types it doesn't convert", "[unit]")
{
	FakeDriver driver;
	driver.sampleType = ASIOSTFloat64LSB;
	const std::unique_ptr<Backend> backend = makeAsioBackend(fake(driver));
	try
	{
		backend->open({});
		FAIL("opened");
	}
	catch (const DeviceError& error)
	{
		CHECK(std::string(error.what()).find("sample type") != std::string::npos);
	}
	CHECK(driver.references == 0);
}
