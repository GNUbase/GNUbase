// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform/midi/coremidi.h"

#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <future>
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
		mach_timebase_info_data_t timebase()
		{
			static const mach_timebase_info_data_t info = []
			{
				mach_timebase_info_data_t result{};
				mach_timebase_info(&result);
				return result;
			}();
			return info;
		}

		// CoreMIDI stamps with mach absolute time, which stops while the Mac sleeps and host time doesn't: conversions
		// go by both clocks' now. 0 means now.
		HostTime fromMach(MIDITimeStamp stamp)
		{
			if (stamp == 0)
				return hostTime();
			const auto ticks = static_cast<int64_t>(stamp) - static_cast<int64_t>(mach_absolute_time());
			return hostTime() + (ticks * timebase().numer / timebase().denom);
		}

		MIDITimeStamp toMach(HostTime time)
		{
			const HostTime ahead = time - hostTime();
			if (ahead <= 0)
				return 0;
			return mach_absolute_time() + static_cast<MIDITimeStamp>(ahead * timebase().denom / timebase().numer);
		}

		std::runtime_error failure(const std::string& what, OSStatus result)
		{
			return std::runtime_error(what + ": CoreMIDI error " + std::to_string(result));
		}

		std::string stringProperty(MIDIObjectRef object, CFStringRef property)
		{
			CFStringRef value = nullptr;
			if (MIDIObjectGetStringProperty(object, property, &value) != noErr || value == nullptr)
				return {};
			std::string text(static_cast<size_t>(
								 CFStringGetMaximumSizeForEncoding(CFStringGetLength(value), kCFStringEncodingUTF8)) +
					1,
				'\0');
			if (!CFStringGetCString(value, text.data(), static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8))
				text.clear();
			CFRelease(value);
			text.resize(std::strlen(text.c_str()));
			return text;
		}

		struct Endpoint
		{
			MIDIEndpointRef ref = 0;
			PortInfo info;
		};

		// Sources are inputs, destinations outputs.
		std::vector<Endpoint> list(bool input)
		{
			std::vector<Endpoint> endpoints;
			const ItemCount count = input ? MIDIGetNumberOfSources() : MIDIGetNumberOfDestinations();
			for (ItemCount i = 0; i < count; ++i)
			{
				const MIDIEndpointRef ref = input ? MIDIGetSource(i) : MIDIGetDestination(i);
				if (ref == 0)
					continue;
				SInt32 id = 0;
				MIDIObjectGetIntegerProperty(ref, kMIDIPropertyUniqueID, &id);
				std::string name = stringProperty(ref, kMIDIPropertyDisplayName);
				if (name.empty())
					name = stringProperty(ref, kMIDIPropertyName);
				SInt32 protocol = 0;
				MIDIObjectGetIntegerProperty(ref, kMIDIPropertyProtocolID, &protocol);
				endpoints.push_back({ref,
					{std::to_string(id), name.empty() ? std::to_string(id) : name, input,
						protocol == kMIDIProtocol_2_0}});
			}
			return endpoints;
		}

		MIDIEndpointRef find(const std::string& id, bool input)
		{
			for (const Endpoint& endpoint : list(input))
				if (endpoint.info.id == id)
					return endpoint.ref;
			throw std::runtime_error(std::string(input ? "no MIDI input " : "no MIDI output ") + id);
		}

		// Words in a Universal MIDI Packet, by message type.
		constexpr std::array<UInt32, 16> g_packetWords{1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};

		class CoreMidiInput : public Input
		{
		public:
			CoreMidiInput(MIDIClientRef client, MIDIEndpointRef source, Receiver receiver)
				: m_receiver(std::move(receiver))
			{
				OSStatus result = MIDIInputPortCreateWithProtocol(client, CFSTR("GNUbase input"), kMIDIProtocol_1_0,
					&m_port, ^(const MIDIEventList* list, void* /*refCon*/) {
					  received(*list);
					});
				if (result != noErr)
					throw failure("cannot open the MIDI input", result);
				result = MIDIPortConnectSource(m_port, source, nullptr);
				if (result != noErr)
				{
					MIDIPortDispose(m_port);
					throw failure("cannot connect the MIDI input", result);
				}
			}

			// Once the port is disposed, CoreMIDI doesn't call its receive block any more.
			~CoreMidiInput() override { MIDIPortDispose(m_port); }

			CoreMidiInput(const CoreMidiInput&) = delete;
			CoreMidiInput& operator=(const CoreMidiInput&) = delete;

		private:
			// CoreMIDI's thread. In the MIDI 1.0 protocol, channel messages are type 2 and system messages type 1;
			// SysEx (type 3) isn't carried.
			void received(const MIDIEventList& list) const
			{
				const MIDIEventPacket* packet = list.packet;
				for (UInt32 p = 0; p < list.numPackets; ++p, packet = MIDIEventPacketNext(packet))
				{
					const HostTime time = fromMach(packet->timeStamp);
					for (UInt32 w = 0; w < packet->wordCount; w += g_packetWords[packet->words[w] >> 28U])
					{
						const UInt32 word = packet->words[w];
						const UInt32 type = word >> 28U;
						if (type != 1 && type != 2)
							continue;
						audio::MidiMessage message;
						message.bytes = {static_cast<uint8_t>(word >> 16U), static_cast<uint8_t>((word >> 8U) & 0x7fU),
							static_cast<uint8_t>(word & 0x7fU)};
						const int size = messageSize(message.bytes[0]);
						if (size == 0)
							continue;
						message.size = static_cast<uint8_t>(size);
						for (size_t i = message.size; i < message.bytes.size(); ++i)
							message.bytes[i] = 0;
						m_receiver(message, time);
					}
				}
			}

			MIDIPortRef m_port = 0;
			Receiver m_receiver;
		};

		// CoreMIDI schedules what is sent for later.
		class CoreMidiOutput : public Output
		{
		public:
			CoreMidiOutput(MIDIClientRef client, MIDIEndpointRef destination) : m_destination(destination)
			{
				const OSStatus result = MIDIOutputPortCreate(client, CFSTR("GNUbase output"), &m_port);
				if (result != noErr)
					throw failure("cannot open the MIDI output", result);
			}

			~CoreMidiOutput() override { MIDIPortDispose(m_port); }

			CoreMidiOutput(const CoreMidiOutput&) = delete;
			CoreMidiOutput& operator=(const CoreMidiOutput&) = delete;

			void send(const audio::MidiMessage& message, HostTime time) override
			{
				const uint8_t status = message.bytes[0];
				if (messageSize(status) == 0)
					return;
				const UInt32 word = ((status >= 0xf0 ? 1U : 2U) << 28U) | (UInt32{status} << 16U) |
					(UInt32{message.bytes[1]} << 8U) | message.bytes[2];
				MIDIEventList list{};
				MIDIEventListAdd(
					&list, sizeof(list), MIDIEventListInit(&list, kMIDIProtocol_1_0), toMach(time), 1, &word);
				MIDISendEventList(m_port, m_destination, &list);
			}

		private:
			MIDIPortRef m_port = 0;
			MIDIEndpointRef m_destination = 0;
		};

		class CoreMidiBackend;

		// One CoreMIDI client for the process, since CoreMIDI advises against disposing clients. It starts with the
		// first listing or port, so an engine that doesn't use MIDI leaves CoreMIDI alone. Its notifications arrive on
		// the run loop of a thread of its own, which runs until the process ends.
		class Client
		{
		public:
			static Client& instance()
			{
				static Client client;
				return client;
			}

			MIDIClientRef ref()
			{
				std::call_once(m_started, [this] { start(); });
				if (m_client == 0)
					throw failure("cannot start CoreMIDI", m_status);
				return m_client;
			}

			void add(CoreMidiBackend* backend)
			{
				const std::scoped_lock lock(m_mutex);
				m_backends.push_back(backend);
			}

			void remove(CoreMidiBackend* backend)
			{
				const std::scoped_lock lock(m_mutex);
				m_backends.erase(std::remove(m_backends.begin(), m_backends.end(), backend), m_backends.end());
			}

			Client(const Client&) = delete;
			Client& operator=(const Client&) = delete;

		private:
			Client() = default;

			~Client()
			{
				if (!m_thread.joinable())
					return;
				m_stopping = true;
				CFRunLoopStop(m_loop);
				m_thread.join();
			}

			void start()
			{
				std::promise<void> ready;
				std::future<void> started = ready.get_future();
				m_thread = std::thread(
					[this, &ready]
					{
						m_loop = CFRunLoopGetCurrent();
						m_status = MIDIClientCreateWithBlock(
							CFSTR("GNUbase"), &m_client, ^(const MIDINotification* notification) {
							  notified(*notification);
							});
						if (m_status != noErr)
							m_client = 0;
						// A timer that never fires keeps the run loop running even without CoreMIDI's sources.
						const CFRunLoopTimerRef timer =
							CFRunLoopTimerCreateWithHandler(nullptr, CFAbsoluteTimeGetCurrent() + 1e9, 1e9, 0, 0,
								^(CFRunLoopTimerRef /*timer*/){
								});
						CFRunLoopAddTimer(m_loop, timer, kCFRunLoopDefaultMode);
						ready.set_value();
						while (!m_stopping)
							CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
						CFRunLoopTimerInvalidate(timer);
						CFRelease(timer);
					});
				started.wait();
			}

			void notified(const MIDINotification& notification);

			std::once_flag m_started;
			MIDIClientRef m_client = 0;
			OSStatus m_status = noErr;
			CFRunLoopRef m_loop = nullptr;
			std::atomic<bool> m_stopping{false};
			std::thread m_thread;
			std::mutex m_mutex; // guards m_backends
			std::vector<CoreMidiBackend*> m_backends;
		};

		class CoreMidiBackend : public Backend
		{
		public:
			CoreMidiBackend() { Client::instance().add(this); }
			~CoreMidiBackend() override { Client::instance().remove(this); }

			CoreMidiBackend(const CoreMidiBackend&) = delete;
			CoreMidiBackend& operator=(const CoreMidiBackend&) = delete;

			std::string name() const override { return "coremidi"; }

			std::vector<PortInfo> ports() override
			{
				Client::instance().ref();
				std::vector<PortInfo> ports;
				for (const bool input : {true, false})
					for (Endpoint& endpoint : list(input))
						ports.push_back(std::move(endpoint.info));
				return ports;
			}

			std::unique_ptr<Input> openInput(const std::string& id, Receiver receiver) override
			{
				const MIDIClientRef client = Client::instance().ref();
				return std::make_unique<CoreMidiInput>(client, find(id, true), std::move(receiver));
			}

			std::unique_ptr<Output> openOutput(const std::string& id) override
			{
				const MIDIClientRef client = Client::instance().ref();
				return std::make_unique<CoreMidiOutput>(client, find(id, false));
			}

			void changed() const { portsChanged(); }
		};

		// The client's thread. One setup change comes after the additions and removals it consists of.
		void Client::notified(const MIDINotification& notification)
		{
			if (notification.messageID != kMIDIMsgSetupChanged)
				return;
			const std::scoped_lock lock(m_mutex);
			for (const CoreMidiBackend* backend : m_backends)
				backend->changed();
		}
	}

	std::unique_ptr<Backend> makeCoreMidiBackend()
	{
		return std::make_unique<CoreMidiBackend>();
	}
}
