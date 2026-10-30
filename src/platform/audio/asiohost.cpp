// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/audio/asiohost.h"

#include "platform/audio/sampleformat.h"
#include "platform/hosttime.h"
#include "platform/log.h"
#include "platform/rt/denormals.h"
#include "platform/rt/realtime.h"
#include "platform/winutf8.h"

#include <objbase.h>

#include <iasiodrv.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace gnubase::audio
{
	namespace
	{
		constexpr std::array<double, 6> g_sampleRates{44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0};
		constexpr size_t g_maxBlockSizes = 64; // listed for drivers with a fine granularity
		constexpr std::chrono::seconds g_firstBufferTimeout{2};
		constexpr int g_createBuffersTries = 3;
		constexpr std::chrono::milliseconds g_createBuffersRetryDelay{50};

		// Per driver name: whether the driver changes its sample rate when asked (see AsioDevice::open).
		using RateSwitching = std::map<std::string, bool>;
		// Per driver name: why it can't be used, as its last open or start showed (see AsioBackend::devices).
		using Unavailable = std::map<std::string, std::string>;

		using Handle = std::unique_ptr<std::remove_pointer_t<HANDLE>, decltype(&CloseHandle)>;

		// Runs functions on a thread of its own, in a COM single-threaded apartment that handles window messages.
		class DriverThread
		{
		public:
			DriverThread() : m_thread([this] { loop(); }) {}

			~DriverThread()
			{
				m_quit = true;
				SetEvent(m_wake.get());
				m_thread.join();
			}

			DriverThread(const DriverThread&) = delete;
			DriverThread& operator=(const DriverThread&) = delete;

			// Runs a function on the thread and returns its result, or throws its exception.
			template <typename Function> auto call(Function function) -> decltype(function())
			{
				std::packaged_task<decltype(function())()> task(std::move(function));
				auto result = task.get_future();
				post([&task] { task(); });
				return result.get();
			}

			// Runs a function on the thread later.
			void post(std::function<void()> function)
			{
				{
					const std::scoped_lock lock(m_mutex);
					m_tasks.push_back(std::move(function));
				}
				SetEvent(m_wake.get());
			}

		private:
			void loop()
			{
				const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
				HANDLE wake = m_wake.get();
				while (!m_quit)
				{
					MsgWaitForMultipleObjectsEx(1, &wake, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
					MSG message{};
					while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE)
					{
						TranslateMessage(&message);
						DispatchMessageW(&message);
					}
					runTasks();
				}
				if (SUCCEEDED(com))
					CoUninitialize();
			}

			void runTasks()
			{
				for (;;)
				{
					std::function<void()> task;
					{
						const std::scoped_lock lock(m_mutex);
						if (m_tasks.empty())
							return;
						task = std::move(m_tasks.front());
						m_tasks.pop_front();
					}
					task();
				}
			}

			Handle m_wake{CreateEventW(nullptr, FALSE, FALSE, nullptr), &CloseHandle};
			std::mutex m_mutex; // guards m_tasks
			std::deque<std::function<void()>> m_tasks;
			std::atomic<bool> m_quit{false};
			std::thread m_thread; // last: it uses everything above
		};

		SampleFormat formatOf(ASIOSampleType type, const std::string& channel)
		{
			switch (type)
			{
			case ASIOSTInt16LSB:
				return SampleFormat::Int16;
			case ASIOSTInt24LSB:
				return SampleFormat::Int24;
			case ASIOSTInt32LSB:
				return SampleFormat::Int32;
			case ASIOSTFloat32LSB:
				return SampleFormat::Float32;
			default:
				throw DeviceError(
					"ASIO: " + channel + " has a sample type GNUbase doesn't take yet (" + std::to_string(type) + ")");
			}
		}

		// The block sizes a driver allows: from minimum to maximum in steps of granularity, or powers of two if that
		// is -1, and the preferred one.
		std::vector<uint32_t> blockSizes(long minimum, long maximum, long preferred, long granularity)
		{
			std::vector<uint32_t> sizes;
			if (granularity == -1)
			{
				for (long size = 1; size <= maximum; size *= 2)
					if (size >= minimum)
						sizes.push_back(static_cast<uint32_t>(size));
			}
			else if (granularity > 0)
			{
				for (long size = minimum; size <= maximum && sizes.size() < g_maxBlockSizes; size += granularity)
					sizes.push_back(static_cast<uint32_t>(size));
			}
			const auto preferredSize = static_cast<uint32_t>(preferred);
			if (preferred > 0 && std::find(sizes.begin(), sizes.end(), preferredSize) == sizes.end())
				sizes.insert(std::lower_bound(sizes.begin(), sizes.end(), preferredSize), preferredSize);
			return sizes;
		}

		// A driver's string, which is in the ANSI code page, in UTF-8.
		std::string text(const char* chars, size_t size)
		{
			return utf8(wide({chars, strnlen(chars, size)}, CP_ACP).c_str());
		}

		void bufferSwitch(long index, ASIOBool directProcess);
		ASIOTime* bufferSwitchTimeInfo(ASIOTime* time, long index, ASIOBool directProcess);
		void sampleRateDidChange(ASIOSampleRate rate);
		long asioMessage(long selector, long value, void* message, double* opt);

		class AsioDevice;

		// The open device: ASIO's callbacks have no context.
		std::atomic<AsioDevice*> g_open{nullptr};

		class AsioDevice : public Device
		{
		public:
			AsioDevice(const AsioDrivers& drivers, const std::string& name, const DeviceConfig& config,
				RateSwitching& switching, std::shared_ptr<Unavailable> unavailable)
				: m_unavailable(std::move(unavailable))
			{
				AsioDevice* none = nullptr;
				if (!g_open.compare_exchange_strong(none, this))
					throw DeviceError("ASIO: another driver is open, and only one can be");
				try
				{
					m_thread.call([&] { open(drivers, name, config, switching); });
				}
				catch (...)
				{
					m_thread.call([this] { close(); });
					g_open = nullptr;
					throw;
				}
			}

			~AsioDevice() override
			{
				try
				{
					AsioDevice::stop(); // not virtually: a derived device would be gone already
					m_thread.call([this] { close(); });
				}
				catch (const std::exception& error)
				{
					logging::error("Closing the ASIO driver {} failed: {}", m_info.name, error.what());
				}
				g_open = nullptr;
			}

			AsioDevice(const AsioDevice&) = delete;
			AsioDevice& operator=(const AsioDevice&) = delete;

			const DeviceFormat& format() const override { return m_format; }
			DeviceInfo info() const override { return m_info; }
			DeviceLatency latency() const override { return m_latency; }
			bool running() const override { return m_running; }
			uint64_t position() const override { return m_position.load(std::memory_order_acquire); }

			void start(AudioCallback& callback) override
			{
				if (m_running)
					throw std::logic_error("the ASIO device is already running");
				m_callback.store(&callback, std::memory_order_release);
				const uint64_t position = m_position.load(std::memory_order_acquire);
				m_thread.call([this] { check(m_driver->start(), "start"); });
				// Some drivers start without their hardware, e.g. when it isn't plugged in, and never call back.
				const auto deadline = std::chrono::steady_clock::now() + g_firstBufferTimeout;
				while (m_position.load(std::memory_order_acquire) == position)
				{
					if (std::chrono::steady_clock::now() > deadline)
					{
						m_thread.call([this] { m_driver->stop(); });
						m_callback.store(nullptr, std::memory_order_release);
						throw unavailable(m_info.name, "the driver starts, but doesn't process audio");
					}
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				}
				m_unavailable->erase(m_info.name);
				m_running = true;
			}

			void stop() override
			{
				if (!m_running)
					return;
				m_thread.call([this] { m_driver->stop(); });
				m_running = false;
			}

			// Without waiting: some drivers show their control panel as a modal dialog.
			void openControlPanel() override
			{
				m_thread.post([this] { m_driver->controlPanel(); });
			}

			void monitorInput(const InputMonitor& monitor) override
			{
				if (!m_info.canMonitorDirectly)
					return;
				// Gains go up to +12 dB, taken as 4; pan goes from all left to all right of a stereo pair.
				constexpr double maximum = 0x7fffffff;
				ASIOInputMonitor parameters{};
				parameters.input = monitor.input;
				parameters.output = monitor.output;
				parameters.gain = static_cast<long>(std::llround(std::clamp(monitor.gain / 4.0, 0.0, 1.0) * maximum));
				parameters.state = monitor.on ? ASIOTrue : ASIOFalse;
				parameters.pan =
					static_cast<long>(std::llround(std::clamp((monitor.pan + 1.0) / 2.0, 0.0, 1.0) * maximum));
				m_thread.call([this, &parameters] { m_driver->future(kAsioSetInputMonitor, &parameters); });
			}

			// The driver's thread, once per buffer.
			void process(long index) noexcept
			{
				AudioCallback* callback = m_callback.load(std::memory_order_acquire);
				if (callback == nullptr)
					return;
				const rt::ScopedRealtimeContext realtime;
				const rt::ScopedDenormalsDisabled denormals;
				const HostTime time = hostTime();
				const auto half = static_cast<size_t>(index & 1);
				const uint32_t frames = m_format.blockSize;
				for (size_t c = 0; c < m_inputs.size(); ++c)
					toFloat(m_buffers[c].buffers[half], m_formats[c], 1, frames, &m_inputs[c]);
				std::fill(m_outputSamples.begin(), m_outputSamples.end(), 0.0f);
				m_midiIn.clear();
				m_midiOut.clear();
				const uint64_t position = m_position.load(std::memory_order_relaxed);
				callback->process({m_inputs.data(), m_outputs.data(), frames, position, time, m_midiIn, m_midiOut});
				for (size_t c = 0; c < m_outputs.size(); ++c)
				{
					const size_t channel = m_inputs.size() + c;
					fromFloat(&m_outputs[c], 1, frames, m_formats[channel], m_buffers[channel].buffers[half]);
				}
				m_position.store(position + frames, std::memory_order_release);
				if (m_outputReady)
					m_driver->outputReady();
			}

			// From the driver's callbacks, on whatever thread the driver uses.
			// ponytail: the engine queues the reset with a lock and an allocation, which a driver's audio thread
			// shouldn't do; drivers only ask when audio is interrupted anyway. Hand it to the driver thread if that
			// ever glitches.
			void driverReset() const { requestReset(); }

		private:
			// The driver thread.
			void open(const AsioDrivers& drivers, const std::string& name, const DeviceConfig& config,
				RateSwitching& switching)
			{
				m_driver = drivers.create(name);
				if (m_driver == nullptr)
					throw unavailable(name, "the driver doesn't load");
				if (m_driver->init(GetDesktopWindow()) != ASIOTrue)
					throw unavailable(name, "the driver doesn't start: " + errorMessage());
				m_unavailable->erase(name);
				m_info.id = name;
				m_info.name = name;
				m_info.controlPanel = true;

				long inputs = 0;
				long outputs = 0;
				check(m_driver->getChannels(&inputs, &outputs), "getChannels");

				ASIOSampleRate current = 0.0;
				check(m_driver->getSampleRate(&current), "getSampleRate");
				std::vector<double> others; // the other rates it says it runs at
				for (const double rate : g_sampleRates)
					if (rate != current && m_driver->canSampleRate(rate) == ASE_OK)
						others.push_back(rate);
				// Some drivers, like those following another program's clock, claim rates they then refuse. Whether
				// a driver switches is tried once per driver, as switching can take a while.
				// ponytail: one rate stands for all; try each if a driver turns up that refuses only some.
				auto switches = switching.find(name);
				if (switches == switching.end() && !others.empty())
				{
					const bool switched = m_driver->setSampleRate(others.front()) == ASE_OK;
					if (switched)
						m_driver->setSampleRate(current);
					switches = switching.emplace(name, switched).first;
				}
				m_info.sampleRates = {current};
				if (switches != switching.end() && switches->second)
				{
					m_info.sampleRates.insert(m_info.sampleRates.end(), others.begin(), others.end());
					std::sort(m_info.sampleRates.begin(), m_info.sampleRates.end());
				}
				const double rate = config.sampleRate != 0.0 ? config.sampleRate : current;
				if (std::find(m_info.sampleRates.begin(), m_info.sampleRates.end(), rate) == m_info.sampleRates.end())
					throw DeviceError("ASIO: " + name + " doesn't run at " + std::to_string(std::lround(rate)) + " Hz");
				if (rate != current)
					check(m_driver->setSampleRate(rate), "setSampleRate");

				// At the new rate: some drivers allow other sizes at other rates.
				long minimum = 0;
				long maximum = 0;
				long preferred = 0;
				long granularity = 0;
				check(m_driver->getBufferSize(&minimum, &maximum, &preferred, &granularity), "getBufferSize");
				m_info.blockSizes = blockSizes(minimum, maximum, preferred, granularity);

				std::array<ASIOClockSource, 32> clocks{};
				long count = static_cast<long>(clocks.size());
				if (m_driver->getClockSources(clocks.data(), &count) == ASE_OK)
					for (long i = 0; i < std::min(count, static_cast<long>(clocks.size())); ++i)
					{
						const ASIOClockSource& clock = clocks[static_cast<size_t>(i)];
						m_info.clockSources.push_back(text(clock.name, sizeof(clock.name)));
						if (clock.isCurrentSource == ASIOTrue)
							m_info.clockSource = static_cast<int>(i);
					}
				if (!config.clockSource.empty())
				{
					const auto found =
						std::find(m_info.clockSources.begin(), m_info.clockSources.end(), config.clockSource);
					if (found == m_info.clockSources.end())
						throw DeviceError("ASIO: " + name + " has no clock source " + config.clockSource);
					const auto index = static_cast<size_t>(found - m_info.clockSources.begin());
					check(m_driver->setClockSource(clocks[index].index), "setClockSource");
					m_info.clockSource = static_cast<int>(index);
				}

				const uint32_t blockSize = config.blockSize != 0 ? config.blockSize : static_cast<uint32_t>(preferred);
				if (std::find(m_info.blockSizes.begin(), m_info.blockSizes.end(), blockSize) == m_info.blockSizes.end())
					throw DeviceError(
						"ASIO: " + name + " doesn't take blocks of " + std::to_string(blockSize) + " frames");

				// Every channel, inputs first.
				for (long c = 0; c < inputs + outputs; ++c)
				{
					const bool input = c < inputs;
					ASIOChannelInfo channel{};
					channel.channel = input ? c : c - inputs;
					channel.isInput = input ? ASIOTrue : ASIOFalse;
					check(m_driver->getChannelInfo(&channel), "getChannelInfo");
					std::string channelName = text(channel.name, sizeof(channel.name));
					m_formats.push_back(formatOf(channel.type, channelName));
					(input ? m_info.inputs : m_info.outputs).push_back(std::move(channelName));
					m_buffers.push_back({channel.isInput, channel.channel, {nullptr, nullptr}});
				}
				m_callbacks = {&bufferSwitch, &sampleRateDidChange, &asioMessage, &bufferSwitchTimeInfo};
				// Some drivers refuse now and then, e.g. right after the previous device closed.
				ASIOError created = ASE_OK;
				for (int tries = 0; tries < g_createBuffersTries; ++tries)
				{
					if (tries > 0)
						std::this_thread::sleep_for(g_createBuffersRetryDelay);
					created = m_driver->createBuffers(
						m_buffers.data(), inputs + outputs, static_cast<long>(blockSize), &m_callbacks);
					if (created == ASE_OK)
						break;
				}
				check(created, "createBuffers");
				m_buffersCreated = true;
				long inputLatency = 0;
				long outputLatency = 0;
				if (m_driver->getLatencies(&inputLatency, &outputLatency) == ASE_OK)
					m_latency = {static_cast<uint32_t>(std::max(inputLatency, 0L)),
						static_cast<uint32_t>(std::max(outputLatency, 0L))};
				m_outputReady = m_driver->outputReady() == ASE_OK;
				m_info.canMonitorDirectly = m_driver->future(kAsioCanInputMonitor, nullptr) == ASE_SUCCESS;

				m_format.sampleRate = rate;
				m_format.blockSize = blockSize;
				m_format.inputChannels = static_cast<int>(inputs);
				m_format.outputChannels = static_cast<int>(outputs);
				m_inputSamples.resize(static_cast<size_t>(inputs) * blockSize);
				m_outputSamples.resize(static_cast<size_t>(outputs) * blockSize);
				for (long c = 0; c < inputs; ++c)
					m_inputs.push_back(m_inputSamples.data() + (static_cast<size_t>(c) * blockSize));
				for (long c = 0; c < outputs; ++c)
					m_outputs.push_back(m_outputSamples.data() + (static_cast<size_t>(c) * blockSize));
			}

			// The driver thread.
			void close()
			{
				if (m_driver == nullptr)
					return;
				if (m_buffersCreated)
					m_driver->disposeBuffers();
				m_driver->Release();
				m_driver = nullptr;
			}

			// Why init failed. Other calls leave the message as it was, or garbage.
			std::string errorMessage() const
			{
				// ASIO allows 124 characters, but some drivers write more.
				// ponytail: a driver writing past 4 KiB still overruns the stack; only a guard page would stop that.
				std::array<char, 4096> message{};
				m_driver->getErrorMessage(message.data());
				return text(message.data(), message.size());
			}

			// Remembers why the driver can't be used, and returns the error to throw.
			DeviceError unavailable(const std::string& name, const std::string& reason) const
			{
				(*m_unavailable)[name] = reason;
				return DeviceError("ASIO: " + name + ": " + reason);
			}

			void check(ASIOError error, const char* what) const
			{
				if (error != ASE_OK)
					throw DeviceError("ASIO: " + m_info.name + ": " + what + " failed (" + std::to_string(error) + ")");
			}

			std::shared_ptr<Unavailable> m_unavailable;
			DriverThread m_thread;
			IASIO* m_driver = nullptr; // used on m_thread, and by process on the driver's audio thread
			ASIOCallbacks m_callbacks{};
			std::vector<ASIOBufferInfo> m_buffers; // inputs, then outputs
			std::vector<SampleFormat> m_formats;   // per buffer
			bool m_buffersCreated = false;
			bool m_outputReady = false;
			DeviceFormat m_format;
			DeviceInfo m_info;
			DeviceLatency m_latency;
			bool m_running = false;

			std::atomic<AudioCallback*> m_callback{nullptr};
			std::vector<float> m_inputSamples;
			std::vector<float> m_outputSamples;
			std::vector<float*> m_inputs;
			std::vector<float*> m_outputs;
			MidiBuffer m_midiIn;
			MidiBuffer m_midiOut;
			std::atomic<uint64_t> m_position{0};
		};

		void bufferSwitch(long index, ASIOBool /*directProcess*/)
		{
			if (AsioDevice* device = g_open.load(std::memory_order_acquire); device != nullptr)
				device->process(index);
		}

		ASIOTime* bufferSwitchTimeInfo(ASIOTime* time, long index, ASIOBool directProcess)
		{
			bufferSwitch(index, directProcess);
			return time;
		}

		void sampleRateDidChange(ASIOSampleRate /*rate*/)
		{
			if (const AsioDevice* device = g_open.load(std::memory_order_acquire); device != nullptr)
				device->driverReset();
		}

		long asioMessage(long selector, long value, void* /*message*/, double* /*opt*/)
		{
			switch (selector)
			{
			case kAsioSelectorSupported:
				return value == kAsioEngineVersion || value == kAsioResetRequest || value == kAsioBufferSizeChange ||
						value == kAsioResyncRequest || value == kAsioLatenciesChanged ||
						value == kAsioSupportsTimeInfo || value == kAsioSupportsInputMonitor
					? 1
					: 0;
			case kAsioEngineVersion:
				return 2;
			case kAsioResetRequest:
			case kAsioBufferSizeChange:
			case kAsioResyncRequest:
			case kAsioLatenciesChanged: // opening the driver again reads them
				if (const AsioDevice* device = g_open.load(std::memory_order_acquire); device != nullptr)
					device->driverReset();
				return 1;
			case kAsioSupportsTimeInfo:
			case kAsioSupportsInputMonitor:
				return 1;
			default:
				return 0;
			}
		}

		std::vector<std::string> installedNames()
		{
			HKEY key = nullptr;
			if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &key) != ERROR_SUCCESS)
				return {};
			std::vector<std::string> names;
			std::array<wchar_t, 256> name{};
			for (DWORD i = 0;; ++i)
			{
				auto size = static_cast<DWORD>(name.size());
				if (RegEnumKeyExW(key, i, name.data(), &size, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
					break;
				names.push_back(utf8(name.data()));
			}
			RegCloseKey(key);
			return names;
		}

		IASIO* createInstalled(const std::string& name)
		{
			std::array<wchar_t, 64> classId{};
			auto bytes = static_cast<DWORD>(classId.size() * sizeof(wchar_t));
			const std::wstring path = L"SOFTWARE\\ASIO\\" + wide(name);
			if (RegGetValueW(HKEY_LOCAL_MACHINE, path.c_str(), L"CLSID", RRF_RT_REG_SZ, nullptr, classId.data(),
					&bytes) != ERROR_SUCCESS)
				return nullptr;
			CLSID id{};
			if (FAILED(CLSIDFromString(classId.data(), &id)))
				return nullptr;
			IASIO* driver = nullptr;
			// ASIO drivers take their class id as interface id.
			if (FAILED(CoCreateInstance(id, nullptr, CLSCTX_INPROC_SERVER, id, reinterpret_cast<void**>(&driver))))
				return nullptr;
			return driver;
		}

		class AsioBackend : public Backend
		{
		public:
			explicit AsioBackend(AsioDrivers drivers) : m_drivers(std::move(drivers))
			{
				if (!m_drivers.list)
					m_drivers = {&installedNames, &createInstalled};
			}

			std::string name() const override { return "asio"; }

			// Names only: a driver tells more once it is loaded, and loading one would close the open one. Drivers are
			// unavailable if they didn't load or start the last time they were opened or started.
			std::vector<DeviceInfo> devices() override
			{
				std::vector<DeviceInfo> devices;
				for (const std::string& driver : m_drivers.list())
				{
					DeviceInfo info;
					info.id = driver;
					info.name = driver;
					info.controlPanel = true;
					if (const auto found = m_unavailable->find(driver); found != m_unavailable->end())
						info.unavailableReason = found->second;
					devices.push_back(std::move(info));
				}
				return devices;
			}

			std::unique_ptr<Device> open(const DeviceConfig& config) override
			{
				const std::vector<std::string> names = m_drivers.list();
				if (names.empty())
					throw DeviceError("there is no ASIO driver");
				const std::string& name = config.device.empty() ? names.front() : config.device;
				if (std::find(names.begin(), names.end(), name) == names.end())
					throw DeviceError("no ASIO driver " + name);
				return std::make_unique<AsioDevice>(m_drivers, name, config, m_switching, m_unavailable);
			}

		private:
			AsioDrivers m_drivers;
			RateSwitching m_switching;
			std::shared_ptr<Unavailable> m_unavailable = std::make_shared<Unavailable>(); // shared with its devices
		};
	}

	std::unique_ptr<Backend> makeAsioBackend(AsioDrivers drivers)
	{
		return std::make_unique<AsioBackend>(std::move(drivers));
	}
}
