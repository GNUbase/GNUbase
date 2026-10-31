// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/midi/jackmidi.h"

#include "platform/jacklibrary.h"
#include "platform/log.h"
#include "platform/rt/mpmcqueue.h"
#include "platform/rt/realtime.h"

#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace gnubase::midi
{
	namespace
	{
		constexpr size_t g_maxPorts = 256;       // open at once, per direction
		constexpr size_t g_queueCapacity = 1024; // messages an output holds until they are due

		struct Scheduled
		{
			audio::MidiMessage message;
			HostTime time = 0;
		};

		// A port of GNUbase's client. The process thread uses it while it is active and says so in busy: closing
		// clears active and waits until busy is clear, both sequentially consistent, so one of them sees the other.
		struct Slot
		{
			std::atomic<bool> active{false};
			std::atomic<bool> busy{false};
			jack_port_t* port = nullptr;
			Receiver receiver;                               // an input's
			std::unique_ptr<rt::MpmcQueue<Scheduled>> queue; // an output's
			Scheduled pending;                               // an output's next message, not due yet
			bool hasPending = false;
		};

		// GNUbase's client "gnubase-midi" (or "gnubase-midi-01" and so on, if the name is taken) and its ports. It is
		// active from the start, so connections and the process callback work at once.
		class Session
		{
		public:
			explicit Session(const JackLibrary& jack) : m_jack(jack), m_inputs(g_maxPorts), m_outputs(g_maxPorts)
			{
				m_client = jack.clientOpen("gnubase-midi", JackNoStartServer, nullptr);
				if (m_client == nullptr)
					throw std::runtime_error("JACK: no server to connect to");
				m_prefix = std::string(jack.getClientName(m_client)) + ":";
				jack.setProcessCallback(m_client, &Session::onProcess, this);
				jack.setPortRegistrationCallback(m_client, &Session::onPortRegistration, this);
				jack.onInfoShutdown(m_client, &Session::onShutdown, this);
				if (jack.activate(m_client) != 0)
				{
					jack.clientClose(m_client);
					throw std::runtime_error("JACK: cannot activate the MIDI client");
				}
			}

			// Closing deactivates first, which returns once the server is out of the process callback for good.
			~Session() { m_jack.clientClose(m_client); }

			Session(const Session&) = delete;
			Session& operator=(const Session&) = delete;

			bool shutDown() const { return m_shutdown; }

			// True once after ports were registered or unregistered, or the server went away.
			bool takeChange() { return m_changed.exchange(false); }

			// Other clients' MIDI outputs are GNUbase's inputs, their inputs its outputs.
			std::vector<PortInfo> ports() const
			{
				std::vector<PortInfo> result;
				for (const bool input : {true, false})
				{
					const char** names = m_jack.getPorts(
						m_client, nullptr, JACK_DEFAULT_MIDI_TYPE, input ? JackPortIsOutput : JackPortIsInput);
					for (size_t i = 0; names != nullptr && names[i] != nullptr; ++i)
						if (std::string(names[i]).rfind(m_prefix, 0) != 0)
							result.push_back({names[i], names[i], input, false});
					if (names != nullptr)
						m_jack.free(static_cast<void*>(names));
				}
				return result;
			}

			// Registers a port and connects it to the port with this id. Returns its slot.
			size_t open(const std::string& id, bool input, Receiver receiver)
			{
				const std::vector<PortInfo> listed = ports();
				if (std::none_of(listed.begin(), listed.end(),
						[&](const PortInfo& port) { return port.id == id && port.input == input; }))
					throw std::runtime_error(std::string(input ? "no MIDI input " : "no MIDI output ") + id);

				const std::scoped_lock lock(m_mutex);
				std::vector<Slot>& slots = input ? m_inputs : m_outputs;
				const auto vacant =
					std::find_if(slots.begin(), slots.end(), [](const Slot& slot) { return slot.port == nullptr; });
				if (vacant == slots.end())
					throw std::runtime_error("JACK: " + std::to_string(g_maxPorts) + " MIDI ports are open");
				const std::string name = (input ? "in_" : "out_") + std::to_string(++m_registered);
				jack_port_t* port = m_jack.portRegister(
					m_client, name.c_str(), JACK_DEFAULT_MIDI_TYPE, input ? JackPortIsInput : JackPortIsOutput, 0);
				if (port == nullptr)
					throw std::runtime_error("JACK: cannot register the MIDI port " + name);
				Slot& slot = *vacant;
				slot.port = port;
				slot.receiver = std::move(receiver);
				if (!input)
				{
					slot.queue = std::make_unique<rt::MpmcQueue<Scheduled>>(g_queueCapacity);
					slot.hasPending = false;
				}
				const auto index = static_cast<size_t>(vacant - slots.begin());
				std::atomic<size_t>& used = input ? m_inputsUsed : m_outputsUsed;
				used.store(std::max(used.load(), index + 1));
				slot.active = true;

				const std::string own = m_jack.portName(port);
				const int error = input ? m_jack.connect(m_client, id.c_str(), own.c_str())
										: m_jack.connect(m_client, own.c_str(), id.c_str());
				if (error != 0 && error != EEXIST)
				{
					close(slot);
					throw std::runtime_error("JACK: cannot connect to " + id);
				}
				return index;
			}

			void close(bool input, size_t index)
			{
				const std::scoped_lock lock(m_mutex);
				close((input ? m_inputs : m_outputs)[index]);
			}

			// Not on the process thread. Dropped if the output holds too many messages that aren't due yet.
			void send(size_t index, const Scheduled& scheduled) { m_outputs[index].queue->tryPush(scheduled); }

		private:
			void close(Slot& slot)
			{
				slot.active = false;
				while (slot.busy)
					std::this_thread::yield();
				m_jack.portUnregister(m_client, slot.port); // which disconnects it
				slot.port = nullptr;
				slot.receiver = nullptr;
				slot.queue.reset();
			}

			// jackd2 stops the process thread with an asynchronous cancel when the client closes; as in the audio
			// backend, the callback runs with cancellation disabled, a flag in the thread.
			static int onProcess(jack_nframes_t frames, void* data)
			{
				int state = 0;
				pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &state);
				static_cast<Session*>(data)->process(frames);
				pthread_setcancelstate(state, nullptr);
				return 0;
			}

			void process(uint32_t frames) noexcept
			{
				const rt::ScopedRealtimeContext realtime;
				const HostTime start = hostTime();
				const double rate = m_jack.getSampleRate(m_client);
				if (frames == 0 || rate <= 0.0)
					return;
				// What arrived during the cycle before: its frames span the time from that cycle's start to this one's.
				const HostTime previous =
					m_lastStart != 0 ? m_lastStart : start - static_cast<HostTime>(frames * 1e9 / rate);
				m_lastStart = start;
				const size_t inputs = m_inputsUsed.load();
				for (size_t i = 0; i < inputs; ++i)
				{
					Slot& slot = m_inputs[i];
					slot.busy = true;
					if (slot.active)
						receive(slot, frames, previous, start);
					slot.busy = false;
				}
				const size_t outputs = m_outputsUsed.load();
				for (size_t i = 0; i < outputs; ++i)
				{
					Slot& slot = m_outputs[i];
					slot.busy = true;
					if (slot.active)
						transmit(slot, frames, start, rate);
					slot.busy = false;
				}
			}

			void receive(const Slot& slot, uint32_t frames, HostTime previous, HostTime start) const
			{
				void* buffer = m_jack.portGetBuffer(slot.port, frames);
				const uint32_t count = m_jack.midiGetEventCount(buffer);
				for (uint32_t i = 0; i < count; ++i)
				{
					jack_midi_event_t event{};
					if (m_jack.midiEventGet(&event, buffer, i) != 0 || event.size == 0 ||
						static_cast<size_t>(messageSize(event.buffer[0])) != event.size)
						continue; // SysEx and broken messages
					audio::MidiMessage message;
					message.size = static_cast<uint8_t>(event.size);
					std::copy_n(event.buffer, event.size, message.bytes.begin());
					slot.receiver(message, previous + ((start - previous) * event.time / frames));
				}
			}

			// Messages due before this cycle go to its start; those due after it wait, and so do the ones behind them.
			void transmit(Slot& slot, uint32_t frames, HostTime start, double rate) const
			{
				void* buffer = m_jack.portGetBuffer(slot.port, frames);
				m_jack.midiClearBuffer(buffer);
				uint32_t last = 0;
				while (slot.hasPending || slot.queue->tryPop(slot.pending))
				{
					slot.hasPending = true;
					const double offset = static_cast<double>(slot.pending.time - start) * rate / 1e9;
					if (offset >= frames)
						return;
					last = std::max(last, offset > 0.0 ? static_cast<uint32_t>(offset) : 0U);
					m_jack.midiEventWrite(buffer, last, slot.pending.message.bytes.data(), slot.pending.message.size);
					slot.hasPending = false;
				}
			}

			// The callbacks below may come on the process thread (JACK 1 has no other), so they only take notes.

			static void onPortRegistration(jack_port_id_t /*port*/, int /*registered*/, void* data)
			{
				static_cast<Session*>(data)->m_changed = true;
			}

			static void onShutdown(jack_status_t /*code*/, const char* /*reason*/, void* data)
			{
				auto& self = *static_cast<Session*>(data);
				self.m_shutdown = true;
				self.m_changed = true;
			}

			const JackLibrary& m_jack;
			jack_client_t* m_client = nullptr;
			std::string m_prefix; // of the client's port names
			std::mutex m_mutex;   // guards opening and closing
			int m_registered = 0; // ports so far, for their names
			std::vector<Slot> m_inputs;
			std::vector<Slot> m_outputs;
			std::atomic<size_t> m_inputsUsed{0}; // slots the process thread looks at
			std::atomic<size_t> m_outputsUsed{0};
			HostTime m_lastStart = 0; // process thread
			std::atomic<bool> m_changed{false};
			std::atomic<bool> m_shutdown{false};
		};

		class JackMidiInput : public Input
		{
		public:
			JackMidiInput(std::shared_ptr<Session> session, size_t slot) : m_session(std::move(session)), m_slot(slot)
			{
			}
			~JackMidiInput() override { m_session->close(true, m_slot); }

			JackMidiInput(const JackMidiInput&) = delete;
			JackMidiInput& operator=(const JackMidiInput&) = delete;

		private:
			std::shared_ptr<Session> m_session;
			size_t m_slot;
		};

		class JackMidiOutput : public Output
		{
		public:
			JackMidiOutput(std::shared_ptr<Session> session, size_t slot) : m_session(std::move(session)), m_slot(slot)
			{
			}
			~JackMidiOutput() override { m_session->close(false, m_slot); }

			JackMidiOutput(const JackMidiOutput&) = delete;
			JackMidiOutput& operator=(const JackMidiOutput&) = delete;

			void send(const audio::MidiMessage& message, HostTime time) override
			{
				if (messageSize(message.bytes[0]) != 0)
					m_session->send(m_slot, {message, time});
			}

		private:
			std::shared_ptr<Session> m_session;
			size_t m_slot;
		};

		// A session lasts until the server goes away; open ports keep theirs, and the next listing starts another.
		class JackMidiBackend : public Backend
		{
		public:
			explicit JackMidiBackend(std::string library) : m_library(std::move(library)) {}

			~JackMidiBackend() override
			{
				m_watching = false;
				if (m_watcher.joinable())
					m_watcher.join();
			}

			JackMidiBackend(const JackMidiBackend&) = delete;
			JackMidiBackend& operator=(const JackMidiBackend&) = delete;

			std::string name() const override { return "jack"; }

			std::vector<PortInfo> ports() override
			{
				const std::shared_ptr<Session> current = session();
				return current ? current->ports() : std::vector<PortInfo>{};
			}

			std::unique_ptr<Input> openInput(const std::string& id, Receiver receiver) override
			{
				std::shared_ptr<Session> current = session();
				if (!current)
					throw std::runtime_error("no MIDI input " + id + ": no JACK server runs");
				const size_t slot = current->open(id, true, std::move(receiver));
				return std::make_unique<JackMidiInput>(std::move(current), slot);
			}

			std::unique_ptr<Output> openOutput(const std::string& id) override
			{
				std::shared_ptr<Session> current = session();
				if (!current)
					throw std::runtime_error("no MIDI output " + id + ": no JACK server runs");
				const size_t slot = current->open(id, false, nullptr);
				return std::make_unique<JackMidiOutput>(std::move(current), slot);
			}

		private:
			// The running session, or null without libjack or a server: JACK is optional, so that isn't an error.
			std::shared_ptr<Session> session()
			{
				const std::scoped_lock lock(m_mutex);
				if (m_session && !m_session->shutDown())
					return m_session;
				m_session.reset();
				try
				{
					m_session = std::make_shared<Session>(JackLibrary::get(m_library));
				}
				catch (const std::exception& error)
				{
					logging::debug("No JACK MIDI ports: {}", error.what());
					return nullptr;
				}
				if (!m_watcher.joinable())
					m_watcher = std::thread([this] { watch(); });
				return m_session;
			}

			// Reports what the session's callbacks noted, from a thread that may call into the engine.
			void watch()
			{
				while (m_watching)
				{
					std::this_thread::sleep_for(std::chrono::milliseconds(20));
					bool changed = false;
					{
						const std::scoped_lock lock(m_mutex);
						changed = m_session && m_session->takeChange();
					}
					if (changed)
						portsChanged();
				}
			}

			std::string m_library;
			std::mutex m_mutex; // guards m_session
			std::shared_ptr<Session> m_session;
			std::atomic<bool> m_watching{true};
			std::thread m_watcher;
		};
	}

	std::unique_ptr<Backend> makeJackMidiBackend(std::string library)
	{
		return std::make_unique<JackMidiBackend>(std::move(library));
	}
}
