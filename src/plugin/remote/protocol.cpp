// SPDX-License-Identifier: GPL-3.0-or-later
#include "plugin/remote/protocol.h"

#include <algorithm>
#include <cstring>
#include <thread>

namespace gnubase::plugin::remote
{
	namespace
	{
		uint32_t nextPowerOfTwo(uint32_t value)
		{
			uint32_t result = 1;
			while (result < value)
				result <<= 1;
			return result;
		}

		void write(Writer& writer, const AudioPortInfo& port)
		{
			writer.u32(port.id);
			writer.string(port.name);
			writer.u32(port.channelCount);
			writer.u8(static_cast<uint8_t>(port.layout));
			writer.u64(port.speakers);
			writer.u32(port.flags);
		}

		void read(Reader& reader, AudioPortInfo& port)
		{
			port.id = reader.u32();
			port.name = reader.string();
			port.channelCount = reader.u32();
			port.layout = static_cast<ChannelLayout>(std::min<uint8_t>(reader.u8(), 4));
			port.speakers = reader.u64();
			port.flags = reader.u32();
		}

		void write(Writer& writer, const ParamInfo& param)
		{
			writer.u32(param.id);
			writer.u32(param.flags);
			writer.string(param.name);
			writer.string(param.module);
			writer.f64(param.minimum);
			writer.f64(param.maximum);
			writer.f64(param.defaultValue);
		}

		void read(Reader& reader, ParamInfo& param)
		{
			param.id = reader.u32();
			param.flags = reader.u32();
			param.name = reader.string();
			param.module = reader.string();
			param.minimum = reader.f64();
			param.maximum = reader.f64();
			param.defaultValue = reader.f64();
		}

		void writeStrings(Writer& writer, const std::vector<std::string>& strings)
		{
			writer.u32(static_cast<uint32_t>(strings.size()));
			for (const std::string& string : strings)
				writer.string(string);
		}

		std::vector<std::string> readStrings(Reader& reader)
		{
			std::vector<std::string> strings(reader.count(sizeof(uint32_t)));
			for (std::string& string : strings)
				string = reader.string();
			return strings;
		}

		uint32_t channelsOf(const std::vector<AudioPortInfo>& ports)
		{
			uint32_t channels = 0;
			for (const AudioPortInfo& port : ports)
				channels += port.channelCount;
			return channels;
		}

		// Payload bytes per control ring message, a multiple of 8 so fragments stay aligned.
		uint32_t maxPayload(const ipc::Connection& connection)
		{
			return ((connection.config().controlRingSize / 2) - static_cast<uint32_t>(sizeof(ipc::MessageHeader))) &
				~uint32_t{7};
		}
	}

	ipc::Config audioConfig(uint32_t inputChannels, uint32_t outputChannels, uint32_t maxFrames)
	{
		ipc::Config config;
		config.maxChannels = std::clamp<uint32_t>(std::max(inputChannels, outputChannels), 1, 256);
		config.maxFrames = std::clamp<uint32_t>(maxFrames, 1, 65536);
		config.blockSlots = 2;
		// Two blocks in flight, each with its events, transport segments and sysex bytes.
		const size_t perBlock = (g_maxEvents * (sizeof(Event) + sizeof(ipc::MessageHeader))) +
			(g_maxTransportSegments * (sizeof(TransportSegment) + sizeof(ipc::MessageHeader))) + g_maxSysexBytes +
			g_maxAudioMessage;
		config.eventRingSize = nextPowerOfTwo(static_cast<uint32_t>(2 * perBlock));
		config.controlRingSize = 256; // unused: control messages go over the host's control connection
		return config;
	}

	ipc::Config controlConfig()
	{
		ipc::Config config;
		config.maxChannels = 1;
		config.maxFrames = 1;
		config.blockSlots = 1;
		config.eventRingSize = 256;
		config.controlRingSize = 1 << 20;
		return config;
	}

	void Writer::string(std::string_view value)
	{
		bytes(value.data(), value.size());
	}

	void Writer::bytes(const void* data, size_t size)
	{
		u32(static_cast<uint32_t>(size));
		raw(data, size);
	}

	void Writer::raw(const void* data, size_t size)
	{
		if (size == 0)
			return;
		const auto* bytes = static_cast<const uint8_t*>(data);
		m_data.insert(m_data.end(), bytes, bytes + size);
	}

	bool Reader::raw(void* data, size_t size)
	{
		if (!m_ok || size > remaining())
		{
			m_ok = false;
			std::memset(data, 0, size);
			return false;
		}
		if (size > 0)
			std::memcpy(data, m_data + m_position, size);
		m_position += size;
		return true;
	}

	uint8_t Reader::u8()
	{
		uint8_t value = 0;
		raw(&value, sizeof(value));
		return value;
	}

	uint16_t Reader::u16()
	{
		uint16_t value = 0;
		raw(&value, sizeof(value));
		return value;
	}

	uint32_t Reader::u32()
	{
		uint32_t value = 0;
		raw(&value, sizeof(value));
		return value;
	}

	uint64_t Reader::u64()
	{
		uint64_t value = 0;
		raw(&value, sizeof(value));
		return value;
	}

	int64_t Reader::i64()
	{
		int64_t value = 0;
		raw(&value, sizeof(value));
		return value;
	}

	double Reader::f64()
	{
		double value = 0.0;
		raw(&value, sizeof(value));
		return value;
	}

	uint32_t Reader::count(size_t itemSize)
	{
		const uint32_t value = u32();
		if (!m_ok || (itemSize > 0 && value > remaining() / itemSize))
		{
			m_ok = false;
			return 0;
		}
		return value;
	}

	std::string Reader::string()
	{
		const uint32_t size = count(1);
		std::string value(size, '\0');
		raw(value.data(), size);
		return value;
	}

	std::vector<uint8_t> Reader::bytes()
	{
		const uint32_t size = count(1);
		std::vector<uint8_t> value(size);
		raw(value.data(), size);
		return value;
	}

	PluginInfo describe(Plugin& plugin, bool active)
	{
		PluginInfo info;
		info.descriptor = plugin.descriptor();
		for (const std::string_view id : {AudioPorts::extensionId(), Params::extensionId(), State::extensionId(),
				 Latency::extensionId(), Tail::extensionId()})
		{
			if (plugin.extension(id) != nullptr)
				info.extensions.emplace_back(id);
		}
		if (const auto* ports = extension<AudioPorts>(plugin))
		{
			for (const bool input : {true, false})
			{
				std::vector<AudioPortInfo>& list = input ? info.inputs : info.outputs;
				const uint32_t count = std::min(ports->audioPortCount(input), g_maxPorts);
				for (uint32_t i = 0; i < count; ++i)
				{
					AudioPortInfo port;
					if (ports->audioPortInfo(input, i, port))
						list.push_back(std::move(port));
				}
			}
		}
		if (const auto* params = extension<Params>(plugin))
		{
			const uint32_t count = params->paramCount();
			for (uint32_t i = 0; i < count; ++i)
			{
				ParamInfo param;
				if (!params->paramInfo(i, param))
					continue;
				double value = param.defaultValue;
				params->paramValue(param.id, value);
				info.params.push_back(std::move(param));
				info.values.push_back(value);
			}
		}
		if (const auto* latency = extension<Latency>(plugin); latency != nullptr && active)
			info.latency = latency->latency();
		if (const auto* tail = extension<Tail>(plugin))
			info.tail = tail->tail();
		return info;
	}

	void write(Writer& writer, const PluginInfo& info)
	{
		const Descriptor& descriptor = info.descriptor;
		writer.string(descriptor.id);
		writer.string(descriptor.format);
		writer.string(descriptor.formatId);
		writer.string(descriptor.name);
		writer.string(descriptor.vendor);
		writer.string(descriptor.version);
		writeStrings(writer, descriptor.features);
		writeStrings(writer, info.extensions);
		for (const std::vector<AudioPortInfo>* ports : {&info.inputs, &info.outputs})
		{
			writer.u32(static_cast<uint32_t>(ports->size()));
			for (const AudioPortInfo& port : *ports)
				write(writer, port);
		}
		writer.u32(static_cast<uint32_t>(info.params.size()));
		for (size_t i = 0; i < info.params.size(); ++i)
		{
			write(writer, info.params[i]);
			writer.f64(i < info.values.size() ? info.values[i] : info.params[i].defaultValue);
		}
		writer.u32(info.latency);
		writer.u32(info.tail);
	}

	bool read(Reader& reader, PluginInfo& info)
	{
		Descriptor& descriptor = info.descriptor;
		descriptor.id = reader.string();
		descriptor.format = reader.string();
		descriptor.formatId = reader.string();
		descriptor.name = reader.string();
		descriptor.vendor = reader.string();
		descriptor.version = reader.string();
		descriptor.features = readStrings(reader);
		info.extensions = readStrings(reader);
		for (std::vector<AudioPortInfo>* ports : {&info.inputs, &info.outputs})
		{
			const uint32_t count = reader.count(24);
			if (count > g_maxPorts)
				return false;
			ports->resize(count);
			for (AudioPortInfo& port : *ports)
				read(reader, port);
		}
		const uint32_t params = reader.count(48);
		info.params.resize(params);
		info.values.resize(params);
		for (uint32_t i = 0; i < params; ++i)
		{
			read(reader, info.params[i]);
			info.values[i] = reader.f64();
		}
		info.latency = reader.u32();
		info.tail = reader.u32();
		return reader.ok() && channelsOf(info.inputs) <= 256 && channelsOf(info.outputs) <= 256;
	}

	void write(Writer& writer, const EventList& events)
	{
		writer.u32(static_cast<uint32_t>(events.size()));
		for (const Event& event : events)
		{
			writer.raw(&event, sizeof(event));
			if (event.type == EventType::Sysex)
				writer.bytes(events.bytes(event), event.data[1]);
		}
	}

	bool read(Reader& reader, EventList& events)
	{
		const uint32_t count = reader.count(sizeof(Event));
		for (uint32_t i = 0; i < count; ++i)
		{
			Event event;
			if (!reader.raw(&event, sizeof(event)))
				return false;
			if (event.type == EventType::Sysex)
			{
				const std::vector<uint8_t> bytes = reader.bytes();
				if (!reader.ok() ||
					!events.pushSysex(event.offset, event.port, bytes.data(), static_cast<uint32_t>(bytes.size())))
					return false;
			}
			else if (static_cast<uint8_t>(event.type) > static_cast<uint8_t>(EventType::Sysex) || !events.push(event))
			{
				return false;
			}
		}
		return reader.ok();
	}

	bool sendMessage(ipc::Connection& connection, Message type, const ControlHeader& header,
		const std::vector<uint8_t>& body, std::chrono::nanoseconds timeout)
	{
		std::vector<uint8_t> payload(sizeof(header) + body.size());
		std::memcpy(payload.data(), &header, sizeof(header));
		if (!body.empty())
			std::memcpy(payload.data() + sizeof(header), body.data(), body.size());

		const auto deadline = std::chrono::steady_clock::now() + timeout;
		const uint32_t chunk = maxPayload(connection);
		size_t sent = 0;
		do
		{
			const size_t size = std::min<size_t>(chunk, payload.size() - sent);
			const bool last = sent + size == payload.size();
			const Message partType = last ? type : Message::Fragment;
			// The ring is full until the peer reads; the control path may wait for that.
			while (!connection.sendControl(
				static_cast<uint16_t>(partType), g_messageVersion, payload.data() + sent, static_cast<uint32_t>(size)))
			{
				if (connection.peerState() == ipc::PeerState::Closed || std::chrono::steady_clock::now() >= deadline)
					return false;
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			sent += size;
		} while (sent < payload.size());
		return true;
	}

	MessageReader::MessageReader(ipc::Connection& connection)
		: m_connection(&connection), m_buffer(connection.config().controlRingSize / 2)
	{
	}

	ipc::ReadStatus MessageReader::read(Message& type, ControlHeader& header, std::vector<uint8_t>& body)
	{
		if (m_corrupt)
			return ipc::ReadStatus::Corrupt;
		for (;;)
		{
			ipc::MessageHeader part;
			const ipc::ReadStatus status = m_connection->receiveControl(part, m_buffer.data(), m_buffer.size());
			if (status != ipc::ReadStatus::Ok)
			{
				m_corrupt = status == ipc::ReadStatus::Corrupt;
				return status;
			}
			if (m_pending.size() + part.size > g_maxControlMessage)
			{
				m_corrupt = true;
				return ipc::ReadStatus::Corrupt;
			}
			const auto* bytes = reinterpret_cast<const uint8_t*>(m_buffer.data());
			m_pending.insert(m_pending.end(), bytes, bytes + part.size);
			if (part.type == static_cast<uint16_t>(Message::Fragment))
				continue;
			if (m_pending.size() < sizeof(ControlHeader))
			{
				m_corrupt = true;
				return ipc::ReadStatus::Corrupt;
			}
			type = static_cast<Message>(part.type);
			std::memcpy(&header, m_pending.data(), sizeof(header));
			body.assign(m_pending.begin() + sizeof(header), m_pending.end());
			m_pending.clear();
			return ipc::ReadStatus::Ok;
		}
	}
}
