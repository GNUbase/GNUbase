// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ipc/connection.h"
#include "plugin/events.h"
#include "plugin/extensions.h"
#include "plugin/plugin.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// The messages between the engine and a plugin host process, on top of the IPC transport (docs/architecture.md,
// "Plugin hosting and IPC"). Each host process has one control connection: the engine creates it, starts the host
// with its name, and every main-thread call to the host's plugins goes over it as a request with an id and the
// instance it is for. Each active plugin instance has its own audio connection for its blocks.
namespace gnubase::plugin::remote
{
	// The version of these messages. Hosts refuse an engine with another version in the handshake; the IPC transport
	// checks its own version when the host opens the connection.
	inline constexpr uint32_t g_protocolVersion = 2;
	inline constexpr uint16_t g_messageVersion = 1;

	// Control messages. Every one starts with a ControlHeader.
	// NOLINTNEXTLINE(performance-enum-size): the type field of IPC messages has 16 bits
	enum class Message : uint16_t
	{
		// Engine to host, each answered with a Reply.
		Hello = 1,           // protocol and API version; Reply: the host's versions and process id
		Create = 2,          // plugin id, file path; Reply: PluginInfo after init()
		Activate = 3,        // sample rate, min and max frames, the audio connection's name; Reply: PluginInfo
		Deactivate = 4,      // Reply: nothing
		Destroy = 5,         // Reply: nothing; an unknown instance is fine, e.g. one whose Create failed
		SaveState = 6,       // StateContext; Reply: the bytes
		LoadState = 7,       // StateContext, the bytes; Reply: PluginInfo, which the state may have changed
		ParamToText = 8,     // parameter id, value; Reply: the text
		TextToParam = 9,     // parameter id, text; Reply: the value
		FlushParams = 10,    // events; Reply: the output events. Refused while the instance processes
		Describe = 11,       // Reply: PluginInfo as it is now, e.g. after the plugin asked for a rescan
		SetProcessMode = 12, // ProcessMode; Reply: nothing. Only while inactive
							 // Host to engine.
		Reply = 100, // ok, error message, then what the request returns
		Notify =
			101, // Notification, then its detail
				 // A part of a message larger than a ring message; the message's last part has the message's own type.
		Fragment = 200,
	};

	// What a plugin asks of the engine, from its host process: its calls of Host::requestRestart and of the host
	// extensions. A Notify message carries one with a detail, the flags of the rescan where there are any.
	enum class Notification : uint8_t
	{
		RequestRestart = 1,
		StateDirty = 2,       // HostState::markStateDirty
		RescanParams = 3,     // HostParams::rescanParams; detail: HostParams::Rescan flags
		LatencyChanged = 4,   // HostLatency::latencyChanged
		TailChanged = 5,      // HostTail::tailChanged
		RescanAudioPorts = 6, // HostAudioPorts::rescanAudioPorts
		RescanNotePorts = 7,  // HostNotePorts::rescanNotePorts; detail: HostNotePorts::Rescan flags
	};

	inline constexpr uint32_t g_lastNotification = static_cast<uint32_t>(Notification::RescanNotePorts);

	struct ControlHeader
	{
		uint32_t request = 0;  // answered by the Reply with the same id; 0 for notifications
		uint32_t instance = 0; // the plugin instance in the host process; 0 for the host itself
	};

	static_assert(sizeof(ControlHeader) == 8);

	// Starts every part of a control message in the ring. A sender that gives up in the middle of a message (the peer
	// doesn't read in time) leaves its first parts in the ring; the receiver drops them when the next message starts,
	// so the stream never gets out of step.
	struct PartHeader
	{
		uint32_t message = 0; // counts up per message and sender
		uint32_t index = 0;   // of the part within the message, from 0
	};

	static_assert(sizeof(PartHeader) == 8);

	// The messages of a block in the audio connection's event ring, before the block. The block header's event count
	// counts all of them.
	// NOLINTNEXTLINE(performance-enum-size): the type field of IPC messages has 16 bits
	enum class AudioMessage : uint16_t
	{
		ProcessInfo = 1, // engine to host, first: a ProcessInfo, then a constant mask per input port
		Transport = 2,   // engine to host: a TransportSegment
		Event = 3,       // both ways: a plugin::Event
		SysexBytes = 4,  // both ways: the bytes of the Sysex event right before it
		Result = 5,      // host to engine, first: a ProcessResult, then a constant mask per output port
	};

	// Flags of ProcessInfo. The proxy sends Start and Stop in blocks of 0 frames of their own, right when the engine
	// calls startProcessing and stopProcessing, and waits for the answer, whose status says whether it worked.
	enum ProcessFlags : uint8_t
	{
		Start = 1, // call startProcessing
		Stop = 2,  // call stopProcessing
		Reset = 4, // call reset first
		Flush = 8, // a block of 0 frames: call Params::flushParams instead of process
	};

	// Flags of ProcessResult: what the plugin asked for while processing.
	enum ResultFlags : uint8_t
	{
		RequestProcess = 1,    // Host::requestProcess
		RequestParamFlush = 2, // HostParams::requestParamFlush
	};

	struct ProcessInfo
	{
		int64_t steadyTime = -1;
		uint32_t flags = 0; // ProcessFlags
		uint8_t mode = 0;   // ProcessMode
		uint8_t portCount = 0;
		uint16_t transportCount = 0; // Transport messages of the block; at least 1 when it has frames
	};

	struct ProcessResult
	{
		uint8_t status = 0; // ProcessStatus
		uint8_t portCount = 0;
		uint16_t droppedEvents = 0; // output events the host couldn't pass on, saturating
		uint32_t flags = 0;         // ResultFlags
	};

	static_assert(sizeof(ProcessInfo) == 16 && sizeof(ProcessResult) == 8);
	static_assert(std::is_trivially_copyable_v<TransportSegment> && sizeof(TransportSegment) == 72);

	// The most audio ports a remote plugin has per direction, and the largest audio message.
	inline constexpr uint32_t g_maxPorts = 64;
	inline constexpr size_t g_maxAudioMessage = 65536;
	// Capacities of the event lists on both sides of the audio path.
	inline constexpr size_t g_maxEvents = 2048;
	inline constexpr size_t g_maxSysexBytes = 65536;
	inline constexpr size_t g_maxTransportSegments = 64;

	// The audio connection of an instance with these ports and block size.
	ipc::Config audioConfig(uint32_t inputChannels, uint32_t outputChannels, uint32_t maxFrames);

	// The control connection of a host process.
	ipc::Config controlConfig();

	// Little-endian encoding of control message bodies.
	class Writer
	{
	public:
		void u8(uint8_t value) { m_data.push_back(value); }
		void u16(uint16_t value) { raw(&value, sizeof(value)); }
		void u32(uint32_t value) { raw(&value, sizeof(value)); }
		void u64(uint64_t value) { raw(&value, sizeof(value)); }
		void i64(int64_t value) { raw(&value, sizeof(value)); }
		void f64(double value) { raw(&value, sizeof(value)); }
		void string(std::string_view value);
		void bytes(const void* data, size_t size); // with its size first
		void raw(const void* data, size_t size);

		const std::vector<uint8_t>& data() const { return m_data; }
		std::vector<uint8_t>& data() { return m_data; }

	private:
		std::vector<uint8_t> m_data;
	};

	// Decodes what Writer wrote. Nothing read from the peer is trusted: reading past the end, or a size that doesn't
	// fit, returns zeros and makes ok() false from then on.
	class Reader
	{
	public:
		Reader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}
		explicit Reader(const std::vector<uint8_t>& data) : Reader(data.data(), data.size()) {}

		uint8_t u8();
		uint16_t u16();
		uint32_t u32();
		uint64_t u64();
		int64_t i64();
		double f64();
		std::string string();
		std::vector<uint8_t> bytes();
		bool raw(void* data, size_t size);

		// A count of items that each take at least `itemSize` bytes; 0 and not ok if they can't fit.
		uint32_t count(size_t itemSize);

		bool ok() const { return m_ok; }
		size_t remaining() const { return m_size - m_position; }

	private:
		const uint8_t* m_data = nullptr;
		size_t m_size = 0;
		size_t m_position = 0;
		bool m_ok = true;
	};

	// What the engine caches of a plugin instance: its descriptor, the extensions it has, and what they report. The
	// latency is only valid while the instance is active.
	struct PluginInfo
	{
		Descriptor descriptor;
		std::vector<std::string> extensions;
		std::vector<AudioPortInfo> inputs;
		std::vector<AudioPortInfo> outputs;
		std::vector<ParamInfo> params;
		std::vector<double> values; // per parameter, in the order of params
		uint32_t latency = 0;
		uint32_t tail = 0;
	};

	// [main] Collects a plugin's info through its extensions. `active` reads the latency, which is only valid then.
	PluginInfo describe(Plugin& plugin, bool active);

	void write(Writer& writer, const PluginInfo& info);
	bool read(Reader& reader, PluginInfo& info);

	void write(Writer& writer, const EventList& events);
	// Appends the events to the list; false if they don't fit or are invalid.
	bool read(Reader& reader, EventList& events);

	// The largest control message, its ControlHeader and parts put together: room for large plugin states. Senders
	// check it before they send anything, so a larger message is refused with an error instead of reaching the peer.
	inline constexpr size_t g_maxControlMessage = size_t{256} << 20;

	enum class SendResult : uint8_t
	{
		Sent,
		TooLarge, // over the size limit: nothing was sent
		Timeout,  // the peer didn't make room in time; parts that went out are dropped by the receiver
		Closed,   // the peer closed the connection
	};

	// Sends control messages of any size up to a limit: a message larger than a ring message goes out in parts, each
	// with a PartHeader. One sender per connection and direction at a time; not real-time.
	class MessageWriter
	{
	public:
		explicit MessageWriter(ipc::Connection& connection, size_t maxMessage = g_maxControlMessage);

		// Waits for room in the ring until the timeout passes.
		SendResult send(Message type, const ControlHeader& header, const std::vector<uint8_t>& body,
			std::chrono::nanoseconds timeout);

		size_t maxMessage() const { return m_maxMessage; }

	private:
		ipc::Connection* m_connection = nullptr;
		size_t m_maxMessage = 0;
		uint32_t m_nextMessage = 1;
	};

	// A complete control message.
	struct ControlMessage
	{
		Message type = Message::Reply;
		ControlHeader header;
		std::vector<uint8_t> body;
		// It was larger than the receiver's limit: only its type and header arrived, so the receiver can answer with
		// an error instead of letting the sender wait for nothing.
		bool tooLarge = false;
	};

	// Reads complete control messages from a connection, putting their parts together. Parts of a message whose
	// sender gave up are dropped. Not real-time.
	class MessageReader
	{
	public:
		explicit MessageReader(ipc::Connection& connection, size_t maxMessage = g_maxControlMessage);

		// The next complete message. Corrupt if the peer broke the protocol: a part without its PartHeader, or a
		// message without its ControlHeader. Stays so.
		ipc::ReadStatus read(ControlMessage& message);

		// Parts dropped so far because their message was not completed.
		size_t droppedParts() const { return m_droppedParts; }

	private:
		ipc::Connection* m_connection = nullptr;
		size_t m_maxMessage = 0;
		std::vector<std::byte> m_buffer;
		std::vector<uint8_t> m_pending; // the current message's parts so far, without their PartHeaders
		uint32_t m_message = 0;         // the current message
		uint32_t m_nextIndex = 0;       // the part it continues with
		bool m_inMessage = false;
		bool m_tooLarge = false;
		size_t m_droppedParts = 0;
		bool m_corrupt = false;
	};
}
