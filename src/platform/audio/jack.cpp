// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/audio/jack.h"

#include "platform/hosttime.h"
#include "platform/jacklibrary.h"
#include "platform/log.h"
#include "platform/rt/denormals.h"
#include "platform/rt/realtime.h"

#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace gnubase::audio
{
	namespace
	{
		constexpr std::array<uint32_t, 7> g_blockSizes{32, 64, 128, 256, 512, 1024, 2048};
		constexpr uint32_t g_maxBlockSize = 8192;

		// A connection to the server as client "gnubase" (or "gnubase-2" and so on, if that name is taken).
		class Client
		{
		public:
			explicit Client(const JackLibrary& jack) : m_jack(jack)
			{
				m_client = jack.clientOpen("gnubase", JackNoStartServer, nullptr);
				if (m_client == nullptr)
					throw DeviceError("JACK: no server to connect to");
			}

			~Client() { m_jack.clientClose(m_client); }

			Client(const Client&) = delete;
			Client& operator=(const Client&) = delete;

			jack_client_t* get() const { return m_client; }

			// The physical audio ports of this direction: playback ports take input, capture ports give output.
			std::vector<std::string> physicalPorts(bool playback) const
			{
				std::vector<std::string> names;
				const char** ports = m_jack.getPorts(m_client, nullptr, JACK_DEFAULT_AUDIO_TYPE,
					JackPortIsPhysical | (playback ? JackPortIsInput : JackPortIsOutput));
				for (size_t i = 0; ports != nullptr && ports[i] != nullptr; ++i)
					names.emplace_back(ports[i]);
				if (ports != nullptr)
					m_jack.free(static_cast<void*>(ports));
				return names;
			}

		private:
			const JackLibrary& m_jack;
			jack_client_t* m_client = nullptr;
		};

		DeviceInfo makeInfo(const JackLibrary& jack, const Client& client)
		{
			DeviceInfo info;
			info.id = "default";
			info.name = "JACK system ports";
			info.inputs = client.physicalPorts(false);
			info.outputs = client.physicalPorts(true);
			info.sampleRates = {static_cast<double>(jack.getSampleRate(client.get()))};
			info.blockSizes = {g_blockSizes.begin(), g_blockSizes.end()};
			const uint32_t current = jack.getBufferSize(client.get());
			if (std::find(info.blockSizes.begin(), info.blockSizes.end(), current) == info.blockSizes.end())
			{
				info.blockSizes.push_back(current);
				std::sort(info.blockSizes.begin(), info.blockSizes.end());
			}
			return info;
		}

		class JackDevice : public Device
		{
		public:
			JackDevice(const JackLibrary& jack, std::unique_ptr<Client> client, DeviceInfo info, uint32_t blockSize)
				: m_jack(jack), m_client(std::move(client)), m_info(std::move(info))
			{
				jack_client_t* const handle = m_client->get();
				m_format.sampleRate = m_info.sampleRates.front();
				m_format.inputChannels = static_cast<int>(m_info.inputs.size());
				m_format.outputChannels = static_cast<int>(m_info.outputs.size());
				m_previousBufferSize = jack.getBufferSize(handle);
				m_format.blockSize = blockSize != 0 ? blockSize : m_previousBufferSize;
				if (std::find(m_info.blockSizes.begin(), m_info.blockSizes.end(), m_format.blockSize) ==
					m_info.blockSizes.end())
				{
					m_info.blockSizes.push_back(m_format.blockSize);
					std::sort(m_info.blockSizes.begin(), m_info.blockSizes.end());
				}

				const auto registerPorts = [&jack, handle](const char* prefix, size_t count, unsigned long flags)
				{
					std::vector<jack_port_t*> ports;
					for (size_t c = 0; c < count; ++c)
					{
						const std::string name = prefix + std::to_string(c + 1);
						ports.push_back(jack.portRegister(handle, name.c_str(), JACK_DEFAULT_AUDIO_TYPE, flags, 0));
						if (ports.back() == nullptr)
							throw DeviceError("JACK: cannot register the port " + name);
					}
					return ports;
				};
				m_inputPorts = registerPorts("in_", m_info.inputs.size(), JackPortIsInput | JackPortIsTerminal);
				m_outputPorts = registerPorts("out_", m_info.outputs.size(), JackPortIsOutput | JackPortIsTerminal);
				m_inputBuffers.resize(m_inputPorts.size());
				m_outputBuffers.resize(m_outputPorts.size());
				m_inputs.resize(m_inputPorts.size());
				m_outputs.resize(m_outputPorts.size());

				jack.setProcessCallback(handle, &JackDevice::onProcess, this);
				jack.setXrunCallback(handle, &JackDevice::onXrun, this);
				jack.setSampleRateCallback(handle, &JackDevice::onSampleRate, this);
				jack.setLatencyCallback(handle, &JackDevice::onLatency, this);
				jack.setPortRegistrationCallback(handle, &JackDevice::onPortRegistration, this);
				jack.onInfoShutdown(handle, &JackDevice::onShutdown, this);

				// The server's buffer size is everybody's: GNUbase asks for its block size only if it was given one.
				m_requestedBufferSize = m_previousBufferSize;
				if (blockSize != 0 && blockSize != m_previousBufferSize)
				{
					m_requestedBufferSize = blockSize;
					if (jack.setBufferSize(handle, blockSize) != 0)
						logging::warning("The JACK server doesn't take a buffer size of {} frames", blockSize);
				}
				m_openedBufferSize = jack.getBufferSize(handle);
				m_openedLatency = JackDevice::latency(); // not virtually, as in the destructor
				m_watcher = std::thread([this] { watch(); });
				++g_jackAudioDevicesOpen;
			}

			~JackDevice() override
			{
				--g_jackAudioDevicesOpen;
				JackDevice::stop(); // not virtually: a derived device would be gone already
				m_watching = false;
				m_watcher.join();
				if (m_requestedBufferSize != m_previousBufferSize)
					m_jack.setBufferSize(m_client->get(), m_previousBufferSize); // as GNUbase found it
				m_client.reset(); // before the members its callbacks use go
			}

			JackDevice(const JackDevice&) = delete;
			JackDevice& operator=(const JackDevice&) = delete;

			const DeviceFormat& format() const override { return m_format; }
			DeviceInfo info() const override { return m_info; }

			// What the server says about its physical ports, at least a buffer, for each direction with channels.
			DeviceLatency latency() const override
			{
				const auto worst = [this](const std::vector<std::string>& names, jack_latency_callback_mode_t mode)
				{
					uint32_t frames = 0;
					for (const std::string& name : names)
						if (jack_port_t* port = m_jack.portByName(m_client->get(), name.c_str()); port != nullptr)
						{
							jack_latency_range_t range{};
							m_jack.portGetLatencyRange(port, mode, &range);
							frames = std::max(frames, range.max);
						}
					return frames != 0 ? frames : m_jack.getBufferSize(m_client->get());
				};
				DeviceLatency result;
				if (!m_info.inputs.empty())
					result.input = worst(m_info.inputs, JackCaptureLatency);
				if (!m_info.outputs.empty())
					result.output = worst(m_info.outputs, JackPlaybackLatency);
				return result;
			}

			bool running() const override { return m_running; }
			uint64_t position() const override { return m_position.load(std::memory_order_acquire); }

			void start(AudioCallback& callback) override
			{
				if (m_running)
					throw std::logic_error("the JACK device is already running");
				m_callback.store(&callback, std::memory_order_release);
				if (m_jack.activate(m_client->get()) != 0)
				{
					m_callback.store(nullptr);
					throw DeviceError("JACK: cannot activate the client");
				}
				m_running = true;
				// Output n to physical playback port n, physical capture port n to input n.
				for (size_t c = 0; c < m_outputPorts.size(); ++c)
					connect(m_jack.portName(m_outputPorts[c]), m_info.outputs[c]);
				for (size_t c = 0; c < m_inputPorts.size(); ++c)
					connect(m_info.inputs[c], m_jack.portName(m_inputPorts[c]));
			}

			// Deactivating returns once the server is out of the process callback for good, and disconnects the ports.
			void stop() override
			{
				if (!m_running)
					return;
				m_jack.deactivate(m_client->get());
				m_callback.store(nullptr);
				m_running = false;
			}

		private:
			// PipeWire hides the ports of a node while it is suspended, as for a moment when the graph follows the
			// device's buffer size request on activation: such ports come back within a few cycles.
			void connect(const std::string& from, const std::string& to)
			{
				for (int attempt = 0; attempt < 50; ++attempt)
				{
					const int error = m_jack.connect(m_client->get(), from.c_str(), to.c_str());
					if (error == 0 || error == EEXIST)
						return;
					std::this_thread::sleep_for(std::chrono::milliseconds(10));
				}
				stop();
				throw DeviceError("JACK: cannot connect " + from + " to " + to);
			}

			// jackd2 stops the process thread with an asynchronous pthread_cancel when the client is deactivated. A
			// cancel inside the noexcept callback would end GNUbase, so it waits until the callback has returned:
			// setting the cancel state is a flag in the thread, no system call.
			static int onProcess(jack_nframes_t frames, void* data)
			{
				int state = 0;
				pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &state);
				static_cast<JackDevice*>(data)->process(frames);
				pthread_setcancelstate(state, nullptr);
				return 0;
			}

			void process(uint32_t frames) noexcept
			{
				for (size_t c = 0; c < m_inputPorts.size(); ++c)
					m_inputBuffers[c] = static_cast<const float*>(m_jack.portGetBuffer(m_inputPorts[c], frames));
				for (size_t c = 0; c < m_outputPorts.size(); ++c)
				{
					m_outputBuffers[c] = static_cast<float*>(m_jack.portGetBuffer(m_outputPorts[c], frames));
					std::fill_n(m_outputBuffers[c], frames, 0.0f);
				}
				AudioCallback* callback = m_callback.load(std::memory_order_acquire);
				// At another sample rate the device plays silence rather than at the wrong speed, until it is reset.
				if (callback == nullptr || m_rateChanged.load(std::memory_order_relaxed))
					return;

				const rt::ScopedRealtimeContext realtime;
				const rt::ScopedDenormalsDisabled denormals;
				const HostTime started = hostTime();
				for (uint32_t done = 0; done < frames;)
				{
					const uint32_t block = std::min(frames - done, m_format.blockSize);
					for (size_t c = 0; c < m_inputBuffers.size(); ++c)
						m_inputs[c] = m_inputBuffers[c] + done;
					for (size_t c = 0; c < m_outputBuffers.size(); ++c)
						m_outputs[c] = m_outputBuffers[c] + done;
					m_midiIn.clear();
					m_midiOut.clear();
					const uint64_t position = m_position.load(std::memory_order_relaxed);
					const HostTime time = std::max(
						started + static_cast<HostTime>(std::llround(done * 1e9 / m_format.sampleRate)), m_lastTime);
					m_lastTime = time;
					callback->process({m_inputs.data(), m_outputs.data(), block, position, time, m_midiIn, m_midiOut});
					m_position.store(position + block, std::memory_order_release);
					done += block;
				}
			}

			// The callbacks below may come on the process thread (JACK 1 has no other), so they only take notes, which
			// the watcher thread acts on.

			static int onXrun(void* data)
			{
				static_cast<JackDevice*>(data)->reportOverload();
				return 0;
			}

			static int onSampleRate(jack_nframes_t rate, void* data)
			{
				auto& self = *static_cast<JackDevice*>(data);
				if (static_cast<double>(rate) != self.m_format.sampleRate)
					self.m_rateChanged = true;
				return 0;
			}

			// GNUbase's ports are terminal: its outputs carry no latency of its inputs, so there is nothing to set.
			static void onLatency(jack_latency_callback_mode_t /*mode*/, void* data)
			{
				static_cast<JackDevice*>(data)->m_graphChanged = true;
			}

			static void onPortRegistration(jack_port_id_t /*port*/, int registered, void* data)
			{
				if (registered == 0)
					static_cast<JackDevice*>(data)->m_graphChanged = true;
			}

			// Like a signal handler: nothing but a note, not even a log entry.
			static void onShutdown(jack_status_t /*code*/, const char* /*reason*/, void* data)
			{
				static_cast<JackDevice*>(data)->m_shutdown = true;
			}

			// Asks for a reset once, when the server goes away, changes its sample rate or buffer size, a physical port
			// goes away or the latency changes.
			void watch()
			{
				while (m_watching)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
					const char* reason = whyReset();
					if (reason == nullptr)
						continue;
					logging::info("The JACK device asks for a reset: {}", reason);
					requestReset();
					return;
				}
			}

			const char* whyReset()
			{
				if (m_shutdown)
					return "the server went away";
				// Polled too, as PipeWire calls the callbacks only once the graph runs at a new rate or size. It says 0
				// while the client is inactive.
				const uint32_t rate = m_jack.getSampleRate(m_client->get());
				if (m_rateChanged || (rate != 0 && static_cast<double>(rate) != m_format.sampleRate))
					return "the sample rate changed";
				const uint32_t size = m_jack.getBufferSize(m_client->get());
				if (size != 0 && size != m_openedBufferSize)
				{
					if (size != m_requestedBufferSize)
						return "the buffer size changed";
					// The device's own request took effect late (PipeWire), and the latency may follow the buffer.
					m_openedBufferSize = size;
					m_openedLatency = latency();
				}
				if (!m_graphChanged.exchange(false))
					return nullptr;
				for (const std::vector<std::string>* names : {&m_info.inputs, &m_info.outputs})
					for (const std::string& name : *names)
						if (m_jack.portByName(m_client->get(), name.c_str()) == nullptr)
							return "a physical port went away";
				const DeviceLatency now = latency();
				if (now.input != m_openedLatency.input || now.output != m_openedLatency.output)
					return "the latency changed";
				return nullptr;
			}

			const JackLibrary& m_jack;
			std::unique_ptr<Client> m_client;
			DeviceInfo m_info;
			DeviceFormat m_format;
			std::vector<jack_port_t*> m_inputPorts;
			std::vector<jack_port_t*> m_outputPorts;
			uint32_t m_previousBufferSize = 0;  // the server's when the device opened
			uint32_t m_requestedBufferSize = 0; // what the device asked the server for
			uint32_t m_openedBufferSize = 0;    // the server's once it had
			DeviceLatency m_openedLatency;
			bool m_running = false;

			// Audio thread memory, allocated when opened.
			std::vector<const float*> m_inputBuffers;
			std::vector<float*> m_outputBuffers;
			std::vector<const float*> m_inputs;
			std::vector<float*> m_outputs;
			MidiBuffer m_midiIn;
			MidiBuffer m_midiOut;
			HostTime m_lastTime = 0;

			std::atomic<AudioCallback*> m_callback{nullptr};
			std::atomic<uint64_t> m_position{0};
			std::atomic<bool> m_rateChanged{false};
			std::atomic<bool> m_graphChanged{false};
			std::atomic<bool> m_shutdown{false};
			std::atomic<bool> m_watching{true};
			std::thread m_watcher;
		};

		class JackBackend : public Backend
		{
		public:
			explicit JackBackend(std::string library) : m_library(std::move(library)) {}

			std::string name() const override { return "jack"; }

			std::vector<DeviceInfo> devices() override
			{
				const JackLibrary& jack = JackLibrary::get(m_library);
				const Client client(jack);
				return {makeInfo(jack, client)};
			}

			std::unique_ptr<Device> open(const DeviceConfig& config) override
			{
				if (!config.device.empty() && config.device != "default")
					throw DeviceError("no device " + config.device);
				if (!config.clockSource.empty())
					throw DeviceError("JACK devices have no clock sources");
				if (config.blockSize > g_maxBlockSize)
					throw DeviceError("JACK blocks have at most 8192 frames");
				const JackLibrary& jack = JackLibrary::get(m_library);
				auto client = std::make_unique<Client>(jack);
				DeviceInfo info = makeInfo(jack, *client);
				if (config.sampleRate != 0.0 && config.sampleRate != info.sampleRates.front())
					throw DeviceError("the JACK server runs at " +
						std::to_string(std::lround(info.sampleRates.front())) + " Hz, not at " +
						std::to_string(std::lround(config.sampleRate)) + " Hz");
				return std::make_unique<JackDevice>(jack, std::move(client), std::move(info), config.blockSize);
			}

		private:
			std::string m_library;
		};
	}

	std::unique_ptr<Backend> makeJackBackend(std::string library)
	{
		return std::make_unique<JackBackend>(std::move(library));
	}
}
