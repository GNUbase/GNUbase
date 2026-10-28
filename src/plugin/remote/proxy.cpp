// SPDX-License-Identifier: GPL-3.0-or-later
#include "plugin/remote/proxy.h"

#include "platform/log.h"
#include "platform/rt/realtime.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <thread>

namespace gnubase::plugin::remote
{
	namespace
	{
		constexpr auto g_version = g_messageVersion;
		constexpr std::chrono::nanoseconds g_slice = std::chrono::milliseconds(5);

		uint32_t channelsOf(const std::vector<AudioPortInfo>& ports)
		{
			uint32_t channels = 0;
			for (const AudioPortInfo& port : ports)
				channels += port.channelCount;
			return channels;
		}

		void silence(const Process& process) noexcept
		{
			for (uint32_t port = 0; port < process.audioOutputCount; ++port)
			{
				AudioBuffer& buffer = process.audioOutputs[port];
				for (uint32_t c = 0; c < buffer.channelCount; ++c)
					std::fill(buffer.channels[c], buffer.channels[c] + process.frames, 0.0f);
				buffer.constantMask = ~uint64_t{0};
			}
		}

		bool isPerNote(const Event& event)
		{
			return event.noteId != -1 || event.key != -1;
		}
	}

	const char* toString(Failure failure)
	{
		switch (failure)
		{
		case Failure::None:
			return "none";
		case Failure::Deadline:
			return "deadline";
		case Failure::HostGone:
			return "host gone";
		case Failure::Protocol:
			return "protocol";
		}
		return "unknown";
	}

	RemotePlugin::RemotePlugin(
		std::string id, std::string path, std::shared_ptr<HostConnection> host, Deadlines deadlines)
		: m_id(std::move(id)), m_path(std::move(path)), m_deadlines(deadlines), m_binding(std::make_unique<Binding>()),
		  m_message(g_maxAudioMessage)
	{
		m_binding->host = std::move(host);
		m_info.descriptor.id = m_id;
	}

	RemotePlugin::~RemotePlugin()
	{
		publish(nullptr);
		try
		{
			release(*m_binding);
		}
		catch (...) // NOLINT(bugprone-empty-catch): a destructor can't report it; only out of memory gets here
		{
			// The host process destroys the instance when it ends.
		}
		try
		{
			if (m_onDestroy)
				m_onDestroy();
		}
		catch (...) // NOLINT(bugprone-empty-catch): a destructor can't report it; only out of memory gets here
		{
			// The manager's entry is left behind.
		}
	}

	HostConnection::Reply RemotePlugin::call(Binding& binding, Message type, const std::vector<uint8_t>& body) const
	{
		if (!binding.host)
			return {false, "no plugin host", {}};
		return binding.host->request(type, binding.instance, body, m_deadlines.control);
	}

	bool RemotePlugin::create(Binding& binding, PluginInfo& info, std::string& error)
	{
		if (!binding.host)
		{
			error = "no plugin host";
			return false;
		}
		binding.instance = binding.host->newInstance();
		Writer writer;
		writer.string(m_id);
		writer.string(m_path);
		const HostConnection::Reply reply = call(binding, Message::Create, writer.data());
		if (!reply.ok)
		{
			error = reply.error;
			return false;
		}
		binding.created = true;
		Reader reader(reply.body);
		if (!read(reader, info))
		{
			error = "the plugin host sent invalid plugin info";
			return false;
		}
		return true;
	}

	bool RemotePlugin::init()
	{
		rt::assertNotRealtime("RemotePlugin::init");
		PluginInfo info;
		std::string error;
		if (!create(*m_binding, info, error))
		{
			logging::warning("Cannot create the plugin {}: {}", m_id, error);
			return false;
		}
		setInfo(std::move(info));
		return true;
	}

	bool RemotePlugin::activateIn(Binding& binding, PluginInfo& info, std::string& error)
	{
		try
		{
			binding.audio = ipc::Connection::create(audioConfig(m_inputChannels, m_outputChannels, m_maxFrames));
		}
		catch (const std::exception& exception)
		{
			error = exception.what();
			return false;
		}
		Writer writer;
		writer.f64(m_sampleRate);
		writer.u32(m_minFrames);
		writer.u32(m_maxFrames);
		writer.string(binding.audio.name());
		const HostConnection::Reply reply = call(binding, Message::Activate, writer.data());
		Reader reader(reply.body);
		if (!reply.ok || !read(reader, info))
		{
			error = reply.ok ? "the plugin host sent invalid plugin info" : reply.error;
			binding.audio = ipc::Connection();
			return false;
		}
		binding.audio.waitForPeer(std::chrono::nanoseconds::zero()); // the host opened it: its name can go
		binding.active = true;
		return true;
	}

	bool RemotePlugin::activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames)
	{
		rt::assertNotRealtime("RemotePlugin::activate");
		if (!m_binding->created || m_binding->active || failure() != Failure::None)
			return false;
		m_sampleRate = sampleRate;
		m_minFrames = minFrames;
		m_maxFrames = maxFrames;
		PluginInfo info;
		std::string error;
		if (!activateIn(*m_binding, info, error))
		{
			logging::warning("Cannot activate the plugin {}: {}", m_id, error);
			return false;
		}
		m_info.latency = info.latency;
		m_info.tail = info.tail;
		m_flags = 0;
		m_processing = false;
		publish(m_binding.get());
		return true;
	}

	void RemotePlugin::deactivate()
	{
		rt::assertNotRealtime("RemotePlugin::deactivate");
		if (!m_binding->active)
			return;
		publish(nullptr);
		m_binding->audio.close();
		if (failure() == Failure::None)
			call(*m_binding, Message::Deactivate, {});
		m_binding->audio = ipc::Connection();
		m_binding->active = false;
	}

	void RemotePlugin::release(Binding& binding)
	{
		if (!binding.created)
			return;
		// A failed plugin's host is gone or hangs: it goes when its process ends.
		const bool reachable = failure() == Failure::None;
		if (binding.active)
		{
			binding.audio.close();
			if (reachable)
				call(binding, Message::Deactivate, {});
			binding.audio = ipc::Connection();
			binding.active = false;
		}
		if (reachable)
			call(binding, Message::Destroy, {});
		binding.created = false;
	}

	void RemotePlugin::publish(Binding* binding)
	{
		m_live.store(binding);
		// The audio thread set m_busy before it read m_live: once it is clear, no block uses the old binding.
		while (m_busy.load())
			std::this_thread::sleep_for(std::chrono::microseconds(50));
	}

	void RemotePlugin::setInfo(PluginInfo info)
	{
		m_info = std::move(info);
		m_info.descriptor.id = m_id;
		m_inputChannels = channelsOf(m_info.inputs);
		m_outputChannels = channelsOf(m_info.outputs);
		m_paramIndex.clear();
		for (uint32_t i = 0; i < m_info.params.size(); ++i)
			m_paramIndex.emplace_back(m_info.params[i].id, i);
		std::sort(m_paramIndex.begin(), m_paramIndex.end());
		m_values = std::make_unique<std::atomic<double>[]>(m_info.params.size());
		setValues(m_info.values);
	}

	void RemotePlugin::setValues(const std::vector<double>& values)
	{
		const size_t count = std::min(values.size(), m_info.params.size());
		for (size_t i = 0; i < count; ++i)
			m_values[i].store(values[i], std::memory_order_relaxed);
	}

	void RemotePlugin::mirror(const Event& event) noexcept
	{
		if (event.type != EventType::ParamValue || isPerNote(event))
			return;
		const auto found =
			std::lower_bound(m_paramIndex.begin(), m_paramIndex.end(), std::pair<uint32_t, uint32_t>(event.param, 0));
		if (found != m_paramIndex.end() && found->first == event.param)
			m_values[found->second].store(event.value, std::memory_order_relaxed);
	}

	bool RemotePlugin::has(std::string_view extension) const
	{
		return std::find(m_info.extensions.begin(), m_info.extensions.end(), extension) != m_info.extensions.end();
	}

	void* RemotePlugin::extension(std::string_view id)
	{
		if (!has(id))
			return nullptr;
		return provide<AudioPorts, Params, State, Latency, Tail>(this, id);
	}

	void RemotePlugin::fail(Failure failure) noexcept
	{
		Failure expected = Failure::None;
		m_failure.compare_exchange_strong(expected, failure, std::memory_order_acq_rel);
	}

	bool RemotePlugin::startProcessing() noexcept
	{
		m_processing.store(true, std::memory_order_relaxed);
		m_flags = (m_flags & ~uint32_t{Stop}) | Start;
		return failure() == Failure::None;
	}

	void RemotePlugin::stopProcessing() noexcept
	{
		m_processing.store(false, std::memory_order_relaxed);
		m_flags = (m_flags & ~uint32_t{Start}) | Stop;
	}

	void RemotePlugin::reset() noexcept
	{
		m_flags |= Reset;
	}

	ProcessStatus RemotePlugin::process(const Process& process) noexcept
	{
		ProcessStatus status = ProcessStatus::Error;
		if (failure() == Failure::None)
		{
			m_busy.store(true);
			Binding* binding = m_live.load();
			if (binding != nullptr && binding->host->exited())
				fail(Failure::HostGone);
			else if (binding != nullptr && process.frames <= m_maxFrames)
				status = exchange(*binding, &process, m_flags, process.eventsIn, process.eventsOut);
			m_busy.store(false, std::memory_order_release);
		}
		if (status == ProcessStatus::Error)
			silence(process);
		return status;
	}

	void RemotePlugin::flushParams(const EventList& in, EventList& out) noexcept
	{
		if (failure() != Failure::None)
			return;
		if (m_processing.load(std::memory_order_relaxed))
		{
			// [audio] A block without frames.
			m_busy.store(true);
			if (Binding* binding = m_live.load(); binding != nullptr)
				exchange(*binding, nullptr, m_flags, in, out);
			m_busy.store(false, std::memory_order_release);
			return;
		}
		// [main]
		try
		{
			for (const Event& event : in)
				mirror(event);
			Writer writer;
			write(writer, in);
			const size_t before = out.size();
			const HostConnection::Reply reply = call(*m_binding, Message::FlushParams, writer.data());
			Reader reader(reply.body);
			if (reply.ok && read(reader, out))
			{
				for (size_t i = before; i < out.size(); ++i)
					mirror(out[i]);
			}
		}
		catch (const std::exception& error)
		{
			logging::error("Flushing the parameters of {} failed: {}", m_id, error.what());
		}
	}

	bool RemotePlugin::sendEvents(ipc::Connection& audio, const EventList& events, uint32_t& count) noexcept
	{
		for (const Event& event : events)
		{
			mirror(event);
			if (!audio.sendEvent(static_cast<uint16_t>(AudioMessage::Event), g_version, &event, sizeof(event)))
				return false;
			++count;
			if (event.type == EventType::Sysex)
			{
				if (!audio.sendEvent(
						static_cast<uint16_t>(AudioMessage::SysexBytes), g_version, events.bytes(event), event.data[1]))
					return false;
				++count;
			}
		}
		return true;
	}

	ProcessStatus RemotePlugin::exchange(
		Binding& binding, const Process* process, uint32_t flags, const EventList& in, EventList& out) noexcept
	{
		ipc::Connection& audio = binding.audio;
		ipc::AudioBlock block;
		if (!audio.beginBlock(block)) // the host still holds every slot: it stopped answering
		{
			fail(Failure::Deadline);
			return ProcessStatus::Error;
		}

		// The block's messages: its info with the inputs' constant masks, the transport, the events.
		ProcessInfo info;
		info.flags = process != nullptr ? flags : flags | Flush;
		if (process != nullptr)
		{
			info.steadyTime = process->steadyTime;
			info.mode = static_cast<uint8_t>(process->mode);
			info.portCount = static_cast<uint8_t>(std::min<uint32_t>(process->audioInputCount, g_maxPorts));
		}
		std::memcpy(m_message.data(), &info, sizeof(info));
		for (uint32_t port = 0; port < info.portCount; ++port)
			std::memcpy(m_message.data() + sizeof(info) + (port * sizeof(uint64_t)),
				&process->audioInputs[port].constantMask, sizeof(uint64_t));
		uint32_t count = 1;
		bool sent = audio.sendEvent(static_cast<uint16_t>(AudioMessage::ProcessInfo), g_version, m_message.data(),
			static_cast<uint32_t>(sizeof(info) + (info.portCount * sizeof(uint64_t))));
		const uint32_t segments =
			process != nullptr ? std::min<uint32_t>(process->transportCount, g_maxTransportSegments) : 0;
		for (uint32_t i = 0; sent && i < segments; ++i, ++count)
			sent = audio.sendEvent(static_cast<uint16_t>(AudioMessage::Transport), g_version, &process->transport[i],
				sizeof(TransportSegment));
		sent = sent && sendEvents(audio, in, count);
		if (!sent) // the host doesn't read its event ring
		{
			fail(Failure::Protocol);
			return ProcessStatus::Error;
		}

		// The inputs, port after port, each with the channels the plugin reported for it.
		const uint32_t frames = process != nullptr ? process->frames : 0;
		uint32_t channel = 0;
		for (uint32_t port = 0; port < m_info.inputs.size(); ++port)
		{
			const AudioBuffer* buffer =
				process != nullptr && port < process->audioInputCount ? &process->audioInputs[port] : nullptr;
			for (uint32_t c = 0; c < m_info.inputs[port].channelCount; ++c, ++channel)
			{
				float* samples = block.channel(channel);
				if (buffer != nullptr && c < buffer->channelCount)
					std::memcpy(samples, buffer->channels[c], frames * sizeof(float));
				else
					std::fill(samples, samples + frames, 0.0f);
			}
		}
		block.sequence = ++m_sequence;
		block.position =
			process != nullptr && process->transportCount > 0 ? process->transport[0].transport.position : 0;
		block.frames = frames;
		block.channels = m_inputChannels;
		block.eventCount = count;
		audio.sendBlock(block);

		// The answer, until the deadline.
		const bool offline = process != nullptr && process->mode == ProcessMode::Offline;
		const auto deadline = std::chrono::steady_clock::now() + (offline ? m_deadlines.offline : m_deadlines.realtime);
		for (;;)
		{
			// In slices, so a host that crashed meanwhile is noticed long before the deadline.
			const auto remaining =
				std::max(deadline - std::chrono::steady_clock::now(), std::chrono::steady_clock::duration::zero());
			const ipc::WaitResult wait = audio.waitForBlock(std::min<std::chrono::nanoseconds>(remaining, g_slice));
			if (wait == ipc::WaitResult::Timeout && remaining > g_slice)
			{
				if (binding.host->exited())
				{
					fail(Failure::HostGone);
					return ProcessStatus::Error;
				}
				continue;
			}
			ipc::AudioBlock reply;
			const ipc::ReadStatus read = audio.receiveBlock(reply);
			if (read == ipc::ReadStatus::Corrupt)
			{
				fail(Failure::Protocol);
				return ProcessStatus::Error;
			}
			if (read == ipc::ReadStatus::Empty)
			{
				if (wait == ipc::WaitResult::PeerClosed)
				{
					fail(Failure::HostGone);
					return ProcessStatus::Error;
				}
				if (wait == ipc::WaitResult::Timeout || std::chrono::steady_clock::now() >= deadline)
				{
					fail(Failure::Deadline);
					return ProcessStatus::Error;
				}
				continue;
			}
			// An answer to an older block can only be left over from a protocol error; it is skipped.
			const bool mine = reply.sequence == block.sequence;
			ProcessStatus status = ProcessStatus::Error;
			const bool valid = readReply(audio, reply, mine ? process : nullptr, mine ? &out : nullptr, status);
			audio.releaseBlock();
			if (!valid)
			{
				fail(Failure::Protocol);
				return ProcessStatus::Error;
			}
			if (mine)
			{
				m_flags = 0;
				return status;
			}
		}
	}

	bool RemotePlugin::readReply(ipc::Connection& audio, const ipc::AudioBlock& reply, const Process* process,
		EventList* out, ProcessStatus& status) noexcept
	{
		status = ProcessStatus::Error;
		Event sysex;
		bool pendingSysex = false;
		for (uint32_t i = 0; i < reply.eventCount; ++i)
		{
			ipc::MessageHeader header;
			if (audio.receiveEvent(header, m_message.data(), m_message.size()) != ipc::ReadStatus::Ok)
				return false;
			switch (static_cast<AudioMessage>(header.type))
			{
			case AudioMessage::Result:
			{
				if (header.size < sizeof(ProcessResult))
					return false;
				ProcessResult result;
				std::memcpy(&result, m_message.data(), sizeof(result));
				if (result.status <= static_cast<uint8_t>(ProcessStatus::Sleep))
					status = static_cast<ProcessStatus>(result.status);
				const uint32_t masks = std::min<uint32_t>(
					result.portCount, static_cast<uint32_t>((header.size - sizeof(result)) / sizeof(uint64_t)));
				for (uint32_t port = 0; process != nullptr && port < masks && port < process->audioOutputCount; ++port)
					std::memcpy(&process->audioOutputs[port].constantMask,
						m_message.data() + sizeof(result) + (port * sizeof(uint64_t)), sizeof(uint64_t));
				break;
			}
			case AudioMessage::Event:
			{
				if (header.size != sizeof(Event))
					return false;
				Event event;
				std::memcpy(&event, m_message.data(), sizeof(event));
				if (static_cast<uint8_t>(event.type) > static_cast<uint8_t>(EventType::Sysex))
					return false;
				pendingSysex = event.type == EventType::Sysex;
				if (pendingSysex)
				{
					sysex = event;
				}
				else if (out != nullptr)
				{
					mirror(event);
					out->push(event);
				}
				break;
			}
			case AudioMessage::SysexBytes:
				if (pendingSysex && out != nullptr)
					out->pushSysex(
						sysex.offset, sysex.port, reinterpret_cast<const uint8_t*>(m_message.data()), header.size);
				pendingSysex = false;
				break;
			default: // from a newer minor version
				break;
			}
		}
		if (process == nullptr)
			return true;

		// The outputs, port after port, as the inputs.
		if (reply.frames != process->frames || reply.channels != m_outputChannels)
			return false;
		uint32_t channel = 0;
		for (uint32_t port = 0; port < m_info.outputs.size(); ++port)
		{
			const AudioBuffer* buffer = port < process->audioOutputCount ? &process->audioOutputs[port] : nullptr;
			for (uint32_t c = 0; c < m_info.outputs[port].channelCount; ++c, ++channel)
			{
				if (buffer != nullptr && c < buffer->channelCount)
					std::memcpy(buffer->channels[c], reply.channel(channel), process->frames * sizeof(float));
			}
			for (uint32_t c = m_info.outputs[port].channelCount; buffer != nullptr && c < buffer->channelCount; ++c)
				std::fill(buffer->channels[c], buffer->channels[c] + process->frames, 0.0f);
		}
		return true;
	}

	uint32_t RemotePlugin::audioPortCount(bool input) const
	{
		return static_cast<uint32_t>(input ? m_info.inputs.size() : m_info.outputs.size());
	}

	bool RemotePlugin::audioPortInfo(bool input, uint32_t index, AudioPortInfo& info) const
	{
		const std::vector<AudioPortInfo>& ports = input ? m_info.inputs : m_info.outputs;
		if (index >= ports.size())
			return false;
		info = ports[index];
		return true;
	}

	bool RemotePlugin::paramInfo(uint32_t index, ParamInfo& info) const
	{
		if (index >= m_info.params.size())
			return false;
		info = m_info.params[index];
		return true;
	}

	bool RemotePlugin::paramValue(uint32_t param, double& value) const
	{
		const auto found =
			std::lower_bound(m_paramIndex.begin(), m_paramIndex.end(), std::pair<uint32_t, uint32_t>(param, 0));
		if (found == m_paramIndex.end() || found->first != param)
			return false;
		value = m_values[found->second].load(std::memory_order_relaxed);
		return true;
	}

	bool RemotePlugin::paramValueToText(uint32_t param, double value, std::string& text) const
	{
		Writer writer;
		writer.u32(param);
		writer.f64(value);
		const HostConnection::Reply reply = call(*m_binding, Message::ParamToText, writer.data());
		Reader reader(reply.body);
		if (!reply.ok)
			return false;
		text = reader.string();
		return reader.ok();
	}

	bool RemotePlugin::paramTextToValue(uint32_t param, std::string_view text, double& value) const
	{
		Writer writer;
		writer.u32(param);
		writer.string(text);
		const HostConnection::Reply reply = call(*m_binding, Message::TextToParam, writer.data());
		Reader reader(reply.body);
		if (!reply.ok)
			return false;
		value = reader.f64();
		return reader.ok();
	}

	bool RemotePlugin::saveState(std::vector<uint8_t>& data, StateContext context)
	{
		Writer writer;
		writer.u8(static_cast<uint8_t>(context));
		const HostConnection::Reply reply = call(*m_binding, Message::SaveState, writer.data());
		Reader reader(reply.body);
		if (!reply.ok)
			return false;
		data = reader.bytes();
		return reader.ok();
	}

	bool RemotePlugin::loadState(const uint8_t* data, size_t size, StateContext context)
	{
		Writer writer;
		writer.u8(static_cast<uint8_t>(context));
		writer.bytes(data, size);
		const HostConnection::Reply reply = call(*m_binding, Message::LoadState, writer.data());
		if (!reply.ok)
			return false;
		Reader reader(reply.body);
		std::vector<double> values(reader.count(sizeof(double)));
		for (double& value : values)
			value = reader.f64();
		setValues(values);
		return reader.ok();
	}

	bool RemotePlugin::moveTo(std::shared_ptr<HostConnection> host, std::string& error)
	{
		rt::assertNotRealtime("RemotePlugin::moveTo");
		if (failure() != Failure::None)
		{
			error = std::string("the plugin failed: ") + toString(failure());
			return false;
		}
		if (host == m_binding->host)
			return true;
		if (!m_binding->created)
		{
			m_binding->host = std::move(host);
			return true;
		}

		std::vector<uint8_t> state;
		const bool hasState = has(State::extensionId());
		if (hasState && !saveState(state, StateContext::Project))
		{
			error = "cannot save the plugin's state";
			return false;
		}
		auto binding = std::make_unique<Binding>();
		binding->host = std::move(host);
		PluginInfo info;
		bool ok = create(*binding, info, error);
		if (ok &&
			(info.params.size() != m_info.params.size() || channelsOf(info.inputs) != m_inputChannels ||
				channelsOf(info.outputs) != m_outputChannels))
		{
			error = "the plugin has other ports or parameters in the other host";
			ok = false;
		}
		std::vector<double> values = info.values;
		if (ok && hasState)
		{
			Writer writer;
			writer.u8(static_cast<uint8_t>(StateContext::Project));
			writer.bytes(state.data(), state.size());
			const HostConnection::Reply reply = call(*binding, Message::LoadState, writer.data());
			Reader reader(reply.body);
			values.resize(reader.count(sizeof(double)));
			for (double& value : values)
				value = reader.f64();
			ok = reply.ok && reader.ok();
			if (!ok)
				error = reply.ok ? "the plugin host sent invalid values" : "cannot load the state: " + reply.error;
		}
		PluginInfo activeInfo;
		if (ok && m_binding->active)
			ok = activateIn(*binding, activeInfo, error);
		if (!ok)
		{
			release(*binding);
			return false;
		}

		// Take over between two blocks.
		const std::unique_ptr<Binding> old = std::move(m_binding);
		m_binding = std::move(binding);
		if (old->active)
			publish(m_binding.get());
		setValues(values);
		release(*old);
		return true;
	}
}
