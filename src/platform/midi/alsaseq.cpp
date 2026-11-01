// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/midi/alsaseq.h"

#include <alsa/asoundlib.h>
#include <poll.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <map>
#include <memory>
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
		constexpr unsigned g_readable = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
		constexpr unsigned g_writable = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;
		constexpr unsigned g_portType = SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION;

		std::runtime_error failure(const std::string& what, int error)
		{
			return std::runtime_error("ALSA: " + what + ": " + snd_strerror(error));
		}

		// An exported port of another client, as the sequencer lists it now.
		struct Listed
		{
			std::string id;
			std::string name;
			int client = 0;
			int port = 0;
			bool readable = false; // an input of GNUbase's
			bool writable = false; // an output
		};

		std::vector<Listed> list(snd_seq_t* seq)
		{
			std::vector<Listed> ports;
			std::map<std::string, int> named; // ports so far per id
			const int self = snd_seq_client_id(seq);
			snd_seq_client_info_t* clientInfo = nullptr;
			snd_seq_port_info_t* portInfo = nullptr;
			if (snd_seq_client_info_malloc(&clientInfo) < 0)
				throw std::bad_alloc();
			const std::unique_ptr<snd_seq_client_info_t, decltype(&snd_seq_client_info_free)> clientOwner(
				clientInfo, &snd_seq_client_info_free);
			if (snd_seq_port_info_malloc(&portInfo) < 0)
				throw std::bad_alloc();
			const std::unique_ptr<snd_seq_port_info_t, decltype(&snd_seq_port_info_free)> portOwner(
				portInfo, &snd_seq_port_info_free);

			snd_seq_client_info_set_client(clientInfo, -1);
			while (snd_seq_query_next_client(seq, clientInfo) >= 0)
			{
				const int number = snd_seq_client_info_get_client(clientInfo);
				if (number == SND_SEQ_CLIENT_SYSTEM || number == self)
					continue;
				const std::string clientName = snd_seq_client_info_get_name(clientInfo);
				snd_seq_port_info_set_client(portInfo, number);
				snd_seq_port_info_set_port(portInfo, -1);
				while (snd_seq_query_next_port(seq, portInfo) >= 0)
				{
					const unsigned caps = snd_seq_port_info_get_capability(portInfo);
					const bool readable = (caps & g_readable) == g_readable;
					const bool writable = (caps & g_writable) == g_writable;
					if ((caps & SND_SEQ_PORT_CAP_NO_EXPORT) != 0 || (!readable && !writable))
						continue;
					std::string name = snd_seq_port_info_get_name(portInfo);
					std::string id = clientName + ':';
					id += name;
					const int count = ++named[id];
					if (count > 1)
						id += " #" + std::to_string(count);
					ports.push_back({std::move(id), std::move(name), number, snd_seq_port_info_get_port(portInfo),
						readable, writable});
				}
			}
			return ports;
		}

		class AlsaSeqBackend;

		class AlsaSeqInput : public Input
		{
		public:
			AlsaSeqInput(AlsaSeqBackend& backend, int port) : m_backend(backend), m_port(port) {}
			~AlsaSeqInput() override;

			AlsaSeqInput(const AlsaSeqInput&) = delete;
			AlsaSeqInput& operator=(const AlsaSeqInput&) = delete;

		private:
			AlsaSeqBackend& m_backend;
			int m_port;
		};

		class AlsaSeqOutput : public Output
		{
		public:
			AlsaSeqOutput(AlsaSeqBackend& backend, int port) : m_backend(backend), m_port(port) {}
			~AlsaSeqOutput() override;

			AlsaSeqOutput(const AlsaSeqOutput&) = delete;
			AlsaSeqOutput& operator=(const AlsaSeqOutput&) = delete;

			void send(const audio::MidiMessage& message, HostTime time) override;

		private:
			AlsaSeqBackend& m_backend;
			int m_port;
		};

		// The sequencer client, its ports and the thread that reads what arrives at them.
		class AlsaSeqBackend : public Backend
		{
		public:
			AlsaSeqBackend() = default;

			~AlsaSeqBackend() override
			{
				if (m_seq == nullptr)
					return;
				m_stopping = true;
				{
					// Wakes the reader with an event to itself.
					const std::scoped_lock lock(m_mutex);
					snd_seq_event_t event;
					snd_seq_ev_clear(&event);
					event.type = SND_SEQ_EVENT_USR0;
					snd_seq_ev_set_source(&event, m_announcements);
					snd_seq_ev_set_dest(&event, snd_seq_client_id(m_seq), m_announcements);
					snd_seq_ev_set_direct(&event);
					snd_seq_event_output_direct(m_seq, &event);
				}
				m_reader.join();
				snd_midi_event_free(m_decoder);
				snd_midi_event_free(m_encoder);
				snd_seq_close(m_seq);
			}

			AlsaSeqBackend(const AlsaSeqBackend&) = delete;
			AlsaSeqBackend& operator=(const AlsaSeqBackend&) = delete;

			std::string name() const override { return "alsa"; }

			std::vector<PortInfo> ports() override
			{
				const std::scoped_lock lock(m_mutex);
				start();
				std::vector<PortInfo> ports;
				const std::vector<Listed> listed = list(m_seq);
				for (const bool input : {true, false})
					for (const Listed& port : listed)
						if (input ? port.readable : port.writable)
							ports.push_back({port.id, port.name, input, false});
				return ports;
			}

			std::unique_ptr<Input> openInput(const std::string& id, Receiver receiver) override
			{
				const std::scoped_lock lock(m_mutex);
				start();
				const Listed source = find(id, true);
				const int port = createPort("GNUbase in", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE);
				{
					const std::scoped_lock receiving(m_receiversMutex);
					m_receivers[port] = std::move(receiver);
				}
				if (const int error = snd_seq_connect_from(m_seq, port, source.client, source.port); error < 0)
				{
					closeInput(port);
					throw failure("cannot connect the MIDI input " + id, error);
				}
				return std::make_unique<AlsaSeqInput>(*this, port);
			}

			std::unique_ptr<Output> openOutput(const std::string& id) override
			{
				const std::scoped_lock lock(m_mutex);
				start();
				const Listed destination = find(id, false);
				const int port = createPort("GNUbase out", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ);
				if (const int error = snd_seq_connect_to(m_seq, port, destination.client, destination.port); error < 0)
				{
					snd_seq_delete_simple_port(m_seq, port);
					throw failure("cannot connect the MIDI output " + id, error);
				}
				return std::make_unique<AlsaSeqOutput>(*this, port);
			}

			// Deleting the port ends its subscription; the reader calls no receiver after it is gone.
			void closeInput(int port)
			{
				{
					const std::scoped_lock lock(m_mutex);
					snd_seq_delete_simple_port(m_seq, port);
				}
				const std::scoped_lock receiving(m_receiversMutex);
				m_receivers.erase(port);
			}

			void closeOutput(int port)
			{
				const std::scoped_lock lock(m_mutex);
				snd_seq_delete_simple_port(m_seq, port);
			}

			// To the port's subscriber, now or through the queue.
			void send(int port, const audio::MidiMessage& message, HostTime time)
			{
				const int size = messageSize(message.bytes[0]);
				if (size == 0)
					return;
				const std::scoped_lock lock(m_mutex);
				snd_seq_event_t event;
				snd_seq_ev_clear(&event);
				snd_midi_event_reset_encode(m_encoder);
				if (snd_midi_event_encode(m_encoder, message.bytes.data(), size, &event) != size ||
					event.type == SND_SEQ_EVENT_NONE)
					return;
				snd_seq_ev_set_source(&event, port);
				snd_seq_ev_set_subs(&event);
				const HostTime ahead = time - hostTime();
				if (ahead <= 0)
				{
					snd_seq_ev_set_direct(&event);
				}
				else
				{
					const snd_seq_real_time_t delay{
						static_cast<unsigned>(ahead / 1000000000), static_cast<unsigned>(ahead % 1000000000)};
					snd_seq_ev_schedule_real(&event, m_queue, 1, &delay); // relative to the queue's now
				}
				snd_seq_event_output_direct(m_seq, &event);
			}

		private:
			// Opens the client, once: its port for the sequencer's announcements, its queue, its reader.
			void start()
			{
				if (m_seq != nullptr)
					return;
				snd_seq_t* seq = nullptr;
				if (const int error = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK); error < 0)
					throw failure("cannot open the sequencer", error);
				try
				{
					snd_seq_set_client_name(seq, "GNUbase");
					m_announcements = snd_seq_create_simple_port(seq, "GNUbase announcements",
						SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_NO_EXPORT, SND_SEQ_PORT_TYPE_APPLICATION);
					if (m_announcements < 0)
						throw failure("cannot create a port", m_announcements);
					if (const int error = snd_seq_connect_from(
							seq, m_announcements, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE);
						error < 0)
						throw failure("cannot follow the sequencer's announcements", error);
					m_queue = snd_seq_alloc_named_queue(seq, "GNUbase");
					if (m_queue < 0)
						throw failure("cannot allocate a queue", m_queue);
					snd_seq_start_queue(seq, m_queue, nullptr);
					snd_seq_drain_output(seq);
					if (snd_midi_event_new(16, &m_decoder) < 0 || snd_midi_event_new(16, &m_encoder) < 0)
						throw std::bad_alloc();
					snd_midi_event_no_status(m_decoder, 1);
				}
				catch (...)
				{
					for (snd_midi_event_t** coder : {&m_decoder, &m_encoder})
						if (*coder != nullptr)
						{
							snd_midi_event_free(*coder);
							*coder = nullptr;
						}
					snd_seq_close(seq);
					throw;
				}
				m_seq = seq;
				m_reader = std::thread([this] { read(); });
			}

			int createPort(const char* name, unsigned caps)
			{
				const int port = snd_seq_create_simple_port(m_seq, name, caps, g_portType);
				if (port < 0)
					throw failure("cannot create a port", port);
				return port;
			}

			Listed find(const std::string& id, bool input) const
			{
				for (Listed& port : list(m_seq))
					if (port.id == id && (input ? port.readable : port.writable))
						return std::move(port);
				throw std::runtime_error(std::string(input ? "no MIDI input " : "no MIDI output ") + id);
			}

			// The reader thread: input to the receivers, announcements of other clients' ports as changes.
			void read()
			{
				const int self = snd_seq_client_id(m_seq);
				std::vector<pollfd> descriptors(static_cast<size_t>(snd_seq_poll_descriptors_count(m_seq, POLLIN)));
				snd_seq_poll_descriptors(m_seq, descriptors.data(), static_cast<unsigned>(descriptors.size()), POLLIN);
				while (!m_stopping)
				{
					poll(descriptors.data(), descriptors.size(), 1000);
					bool changed = false;
					snd_seq_event_t* event = nullptr;
					while (!m_stopping && snd_seq_event_input(m_seq, &event) >= 0 && event != nullptr)
					{
						switch (event->type)
						{
						case SND_SEQ_EVENT_CLIENT_START:
						case SND_SEQ_EVENT_CLIENT_EXIT:
						case SND_SEQ_EVENT_CLIENT_CHANGE:
						case SND_SEQ_EVENT_PORT_START:
						case SND_SEQ_EVENT_PORT_EXIT:
						case SND_SEQ_EVENT_PORT_CHANGE:
							changed = changed || event->data.addr.client != self;
							break;
						default:
							receive(*event, hostTime());
						}
					}
					if (changed)
						portsChanged();
				}
			}

			void receive(const snd_seq_event_t& event, HostTime time)
			{
				std::array<unsigned char, 16> bytes{};
				const long size = snd_midi_event_decode(m_decoder, bytes.data(), bytes.size(), &event);
				if (size < 1 || size != messageSize(bytes[0]))
					return; // SysEx and what isn't MIDI
				audio::MidiMessage message;
				message.size = static_cast<uint8_t>(size);
				std::copy_n(bytes.begin(), message.size, message.bytes.begin());
				const std::scoped_lock lock(m_receiversMutex);
				if (const auto receiver = m_receivers.find(event.dest.port); receiver != m_receivers.end())
					receiver->second(message, time);
			}

			std::mutex m_mutex; // guards the client's use outside the reader
			snd_seq_t* m_seq = nullptr;
			int m_announcements = -1;
			int m_queue = -1;
			snd_midi_event_t* m_encoder = nullptr; // guarded by m_mutex
			snd_midi_event_t* m_decoder = nullptr; // the reader's
			std::mutex m_receiversMutex;           // guards m_receivers; receivers are called with it held
			std::map<int, Receiver> m_receivers;   // by the client's port
			std::atomic<bool> m_stopping{false};
			std::thread m_reader;
		};

		AlsaSeqInput::~AlsaSeqInput()
		{
			m_backend.closeInput(m_port);
		}

		AlsaSeqOutput::~AlsaSeqOutput()
		{
			m_backend.closeOutput(m_port);
		}

		void AlsaSeqOutput::send(const audio::MidiMessage& message, HostTime time)
		{
			m_backend.send(m_port, message, time);
		}
	}

	std::unique_ptr<Backend> makeAlsaSeqBackend()
	{
		return std::make_unique<AlsaSeqBackend>();
	}
}
