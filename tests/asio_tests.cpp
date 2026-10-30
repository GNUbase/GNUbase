// SPDX-License-Identifier: GPL-3.0-or-later
// The ASIO backend against a fake driver, which behaves like a driver loaded through COM: the backend's own code runs,
// only the hardware is missing.
#include "devicecontract.h"
#include "platform/audio/asiohost.h"

#define NOMINMAX
#include <windows.h>

#include <iasiodrv.h>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace gnubase::audio;

namespace
{
	// Two inputs of 32-bit integers and two outputs of 32-bit floats, 44.1 or 48 kHz, blocks of 64 to 1024 frames in
	// powers of two (to 512 at 44.1 kHz; 256 preferred), two clock sources. Its inputs count frames.
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
			return initFails ? ASIOFalse : ASIOTrue;
		}
		void getDriverName(char* name) override { std::snprintf(name, 32, "Fake"); }
		long getDriverVersion() override { return 1; }
		// Like some drivers, without minding ASIO's limit of 124 characters.
		void getErrorMessage(char* text) override { std::memcpy(text, errorText.c_str(), errorText.size() + 1); }

		ASIOError start() override
		{
			running = true;
			if (!silent)
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
			*maximum = rate == 44100.0 ? 512 : 1024;
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
			if (refuseRates)
				return ASE_NoClock;
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
			std::snprintf(info->name, sizeof(info->name), "%s %ld",
				info->isInput == ASIOTrue ? inputName.c_str() : "Out", info->channel + 1);
			return ASE_OK;
		}

		ASIOError createBuffers(ASIOBufferInfo* infos, long channels, long size, ASIOCallbacks* asioCallbacks) override
		{
			if (size == refusedBlockSize)
				return ASE_HWMalfunction;
			if (flakyCreates > 0)
			{
				--flakyCreates;
				return ASE_HWMalfunction;
			}
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
		bool initFails = false;
		std::string errorText = "fake failure";
		std::string inputName = "In"; // the inputs are this and their number
		bool silent = false;          // starts, but never calls back
		bool refuseRates = false;     // says it runs at 44.1 kHz, but doesn't switch
		long refusedBlockSize = 0;    // allowed, but createBuffers fails
		int flakyCreates = 0;         // createBuffers fails this often first, then works
		double rate = 48000.0;
		long clock = 0;
		long blockSize = 0;
		ASIOCallbacks* callbacks = nullptr;
		std::vector<std::vector<int32_t>> buffers; // two halves per channel, inputs first
		std::thread thread;
	};

	// Drivers by name.
	AsioDrivers fakes(const std::map<std::string, FakeDriver*>& drivers)
	{
		return {[drivers]
			{
				std::vector<std::string> names;
				for (const auto& driver : drivers)
					names.push_back(driver.first);
				return names;
			},
			[drivers](const std::string& name)
			{
				const auto found = drivers.find(name);
				if (found == drivers.end())
					return static_cast<IASIO*>(nullptr);
				found->second->AddRef();
				return static_cast<IASIO*>(found->second);
			}};
	}

	AsioDrivers fake(FakeDriver& driver)
	{
		return fakes({{"Fake ASIO", &driver}});
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
	checkDeviceContract(*backend, true); // as with hardware
	CHECK(driver.references == 0);       // every driver loaded was released
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

TEST_CASE("the ASIO backend lists the block sizes of the sample rate it opens at", "[unit]")
{
	FakeDriver driver;
	const std::unique_ptr<Backend> backend = makeAsioBackend(fake(driver));
	DeviceConfig config;
	config.sampleRate = 44100.0;
	CHECK(backend->open(config)->info().blockSizes.back() == 512);
	config.blockSize = 1024;
	CHECK_THROWS_AS(backend->open(config), DeviceError);
}

TEST_CASE("the ASIO backend stays usable after a driver refuses what it offered", "[unit]")
{
	FakeDriver driver;
	driver.errorText = std::string(200, 'x'); // past ASIO's limit, and not about these failures
	driver.refusedBlockSize = 128;
	driver.refuseRates = true;
	const std::unique_ptr<Backend> backend = makeAsioBackend(fake(driver));

	// A driver that doesn't switch its sample rate lists only the current one.
	std::unique_ptr<Device> device = backend->open({});
	CHECK(device->info().sampleRates == std::vector<double>{48000.0});
	device.reset();
	DeviceConfig config;
	config.sampleRate = 44100.0;
	CHECK_THROWS_AS(backend->open(config), DeviceError);

	config = {};
	config.blockSize = 128;
	try
	{
		backend->open(config);
		FAIL("opened");
	}
	catch (const DeviceError& error)
	{
		CHECK(std::string(error.what()).find("createBuffers") != std::string::npos);
		CHECK(std::string(error.what()).find("xxx") == std::string::npos);
	}

	driver.initFails = true;
	try
	{
		backend->open({});
		FAIL("opened");
	}
	catch (const DeviceError& error)
	{
		CHECK(std::string(error.what()).find(driver.errorText) != std::string::npos); // why init failed
	}
	driver.initFails = false;
	CHECK(driver.references == 0);

	driver.flakyCreates = 1; // e.g. still busy closing the previous device
	device = backend->open({});
	Copy copy;
	device->start(copy);
	device->stop();
	CHECK(copy.blocks > 0);
}

TEST_CASE("the ASIO backend refuses to start a driver that never calls back", "[unit]")
{
	FakeDriver driver;
	driver.silent = true;
	const std::unique_ptr<Backend> backend = makeAsioBackend(fake(driver));
	std::unique_ptr<Device> device = backend->open({});
	Copy copy;
	CHECK_THROWS_AS(device->start(copy), DeviceError);
	CHECK(!device->running());

	driver.silent = false; // e.g. its hardware was plugged in
	device->start(copy);
	CHECK(device->running());
	device.reset();
	CHECK(copy.blocks > 0);
	CHECK(driver.references == 0);
}

TEST_CASE("the ASIO backend converts the driver's strings from the ANSI code page to UTF-8", "[unit]")
{
	if (GetACP() != 1252)
		SKIP("the test's driver strings are in Windows-1252");
	FakeDriver driver;
	driver.inputName = "Entr\xE9"
					   "e"; // "Entrée" in Windows-1252
	driver.errorText = "Pas d'appareil connect\xE9";
	const std::unique_ptr<Backend> backend = makeAsioBackend(fake(driver));
	CHECK(backend->open({})->info().inputs.front() ==
		"Entr\xC3\xA9"
		"e 1"); // in UTF-8

	driver.initFails = true;
	CHECK_THROWS_WITH(backend->open({}), Catch::Matchers::ContainsSubstring("Pas d'appareil connect\xC3\xA9"));
}

TEST_CASE("the ASIO backend lists drivers it couldn't open or start as unavailable, with the reason", "[unit]")
{
	FakeDriver unplugged; // refuses init without its hardware
	unplugged.initFails = true;
	unplugged.errorText = "No device is connected to the PC";
	FakeDriver silent; // starts without its hardware, but never calls back
	silent.silent = true;
	FakeDriver working;
	const std::unique_ptr<Backend> backend =
		makeAsioBackend(fakes({{"Unplugged", &unplugged}, {"Silent", &silent}, {"Working", &working}}));
	const auto reasons = [&backend]
	{
		std::map<std::string, std::string> result;
		for (const DeviceInfo& info : backend->devices())
			result[info.id] = info.unavailableReason;
		return result;
	};
	const auto openAndStart = [&backend](const std::string& name)
	{
		DeviceConfig config;
		config.device = name;
		Copy copy;
		try
		{
			backend->open(config)->start(copy);
		}
		catch (const DeviceError&) // listed as unavailable from now on
		{
		}
	};
	// The backend learns about a driver only by loading it.
	CHECK(reasons() == std::map<std::string, std::string>{{"Silent", ""}, {"Unplugged", ""}, {"Working", ""}});

	for (const char* name : {"Unplugged", "Silent", "Working"})
		openAndStart(name);
	std::map<std::string, std::string> unavailable = reasons();
	CHECK_THAT(unavailable["Unplugged"], Catch::Matchers::ContainsSubstring("No device is connected to the PC"));
	CHECK_THAT(unavailable["Silent"], Catch::Matchers::ContainsSubstring("doesn't process audio"));
	CHECK(unavailable["Working"].empty());

	// Their hardware was plugged in.
	unplugged.initFails = false;
	silent.silent = false;
	openAndStart("Unplugged");
	openAndStart("Silent");
	CHECK(reasons() == std::map<std::string, std::string>{{"Silent", ""}, {"Unplugged", ""}, {"Working", ""}});
	CHECK(unplugged.references + silent.references + working.references == 0);
}
