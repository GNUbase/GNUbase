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
	inline constexpr uint32_t g_protocolVersion = 1;
	inline constexpr uint16_t g_messageVersion = 1;

	// Control messages. Every one starts with a ControlHeader.
	// NOLINTNEXTLINE(performance-enum-size): the type field of IPC messages has 16 bits
	enum class Message : uint16_t
	{
		// Engine to host, each answered with a Reply.
		Hello = 1,        // protocol and API version; Reply: the host's versions and process id
		Create = 2,       // plugin id, file path; Reply: PluginInfo after init()
		Activate = 3,     // sample rate, min and max frames, the audio connection's name; Reply: PluginInfo
		Deactivate = 4,   // Reply: nothing
		Destroy = 5,      // Reply: nothing
		SaveState = 6,    // StateContext; Reply: the bytes
		LoadState = 7,    // StateContext, the bytes; Reply: the parameter values
		ParamToText = 8,  // parameter id, value; Reply: the text
		TextToParam = 9,  // parameter id, text; Reply: the value
		FlushParams = 10, // events; Reply: the output events
						  // Host to engine.
		Reply = 100,      // ok, error message, then what the request returns
		Notify = 101,     // Notification
						  // A part of a message too large for the ring; the message's last part has its own type.
		Fragment = 200,
	};

	// What a plugin asks of the engine, from its host process.
	enum class Notification : uint8_t
	{
		RequestRestart = 1,
		StateDirty = 2,
		RescanParams = 3,
		LatencyChanged = 4,
	};

	struct ControlHeader
	{
		uint32_t request = 0;  // answered by the Reply with the same id; 0 for notifications
		uint32_t instance = 0; // the plugin instance in the host process; 0 for the host itself
	};

	static_assert(sizeof(ControlHeader) == 8);

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

	// Flags of ProcessInfo.
	enum ProcessFlags : uint8_t
	{
		Start = 1, // call startProcessing first, after a stopProcessing
		Stop = 2,  // call stopProcessing; a block of 0 frames does nothing else
		Reset = 4, // call reset first
		Flush = 8, // a block of 0 frames: call Params::flushParams instead of process
	};

	struct ProcessInfo
	{
		int64_t steadyTime = -1;
		uint32_t flags = 0; // ProcessFlags
		uint8_t mode = 0;   // ProcessMode
		uint8_t portCount = 0;
		uint16_t reserved = 0;
	};

	struct ProcessResult
	{
		uint8_t status = 0; // ProcessStatus
		uint8_t portCount = 0;
		uint16_t reserved = 0;
		uint32_t flags = 0;
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

	// What the engine caches of a plugin instance: its descriptor, the extensions it has, and what they report.
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

	// Sends a control message of any size: messages the ring can't hold go out in fragments. Waits for room in the
	// ring until the timeout passes; returns false then, or if the peer closed.
	bool sendMessage(ipc::Connection& connection, Message type, const ControlHeader& header,
		const std::vector<uint8_t>& body, std::chrono::nanoseconds timeout);

	// Reads complete control messages from a connection, putting fragments together.
	class MessageReader
	{
	public:
		explicit MessageReader(ipc::Connection& connection);

		// The next complete message. Corrupt if the peer broke the protocol: impossible sizes, a message without its
		// header, more than g_maxControlMessage bytes. Stays so.
		ipc::ReadStatus read(Message& type, ControlHeader& header, std::vector<uint8_t>& body);

	private:
		ipc::Connection* m_connection = nullptr;
		std::vector<std::byte> m_buffer;
		std::vector<uint8_t> m_pending; // fragments so far
		bool m_corrupt = false;
	};

	// The largest control message, fragments put together: room for large plugin states.
	inline constexpr size_t g_maxControlMessage = size_t{256} << 20;
}
