// SPDX-License-Identifier: GPL-3.0-or-later
#include "plugin/remote/stub.h"

#include "platform/log.h"
#include "platform/rt/realtime.h"
#include "platform/thread.h"

#include <algorithm>
#include <cstring>
#include <exception>

namespace gnubase::plugin::remote
{
	namespace
	{
		using namespace std::chrono_literals;

		constexpr auto g_version = g_messageVersion;
	}

	PluginStub::~PluginStub()
	{
		if (m_active)
		{
			stopAudio(2000ms);
			m_plugin->deactivate();
		}
	}

	void PluginStub::notify(Notification notification) noexcept
	{
		m_notifications.fetch_or(uint32_t{1} << static_cast<uint32_t>(notification), std::memory_order_acq_rel);
	}

	uint32_t PluginStub::idle()
	{
		if (m_callback.exchange(false, std::memory_order_acq_rel) && m_plugin)
			m_plugin->onMainThread();
		return m_notifications.exchange(0, std::memory_order_acq_rel);
	}

	bool PluginStub::handle(Message type, Reader& request, Writer& reply, std::string& error)
	{
		if (!m_plugin)
		{
			error = "no plugin";
			return false;
		}
		Plugin& plugin = *m_plugin;
		switch (type)
		{
		case Message::Activate:
			return activate(request, reply, error);
		case Message::Deactivate:
			if (!m_active)
				return true;
			if (!stopAudio(2000ms))
			{
				error = "the plugin hangs in process()";
				return false;
			}
			m_audio = ipc::Connection();
			plugin.deactivate();
			m_active = false;
			return true;
		case Message::SaveState:
		{
			auto* state = gnubase::plugin::extension<State>(plugin);
			const auto context = static_cast<StateContext>(std::min<uint8_t>(request.u8(), 2));
			std::vector<uint8_t> data;
			if (state == nullptr || !request.ok() || !state->saveState(data, context))
			{
				error = "the plugin cannot save its state";
				return false;
			}
			reply.bytes(data.data(), data.size());
			return true;
		}
		case Message::LoadState:
		{
			auto* state = gnubase::plugin::extension<State>(plugin);
			const auto context = static_cast<StateContext>(std::min<uint8_t>(request.u8(), 2));
			const std::vector<uint8_t> data = request.bytes();
			if (state == nullptr || !request.ok() || !state->loadState(data.data(), data.size(), context))
			{
				error = "the plugin cannot load the state";
				return false;
			}
			const PluginInfo info = describe(plugin, m_active);
			reply.u32(static_cast<uint32_t>(info.values.size()));
			for (const double value : info.values)
				reply.f64(value);
			return true;
		}
		case Message::ParamToText:
		{
			const auto* params = gnubase::plugin::extension<Params>(plugin);
			const uint32_t param = request.u32();
			const double value = request.f64();
			std::string text;
			if (params == nullptr || !request.ok() || !params->paramValueToText(param, value, text))
			{
				error = "no text for the value";
				return false;
			}
			reply.string(text);
			return true;
		}
		case Message::TextToParam:
		{
			const auto* params = gnubase::plugin::extension<Params>(plugin);
			const uint32_t param = request.u32();
			const std::string text = request.string();
			double value = 0.0;
			if (params == nullptr || !request.ok() || !params->paramTextToValue(param, text, value))
			{
				error = "no value for the text";
				return false;
			}
			reply.f64(value);
			return true;
		}
		case Message::FlushParams:
		{
			auto* params = gnubase::plugin::extension<Params>(plugin);
			EventList in(g_maxEvents, g_maxSysexBytes);
			EventList out(g_maxEvents, g_maxSysexBytes);
			if (params == nullptr || !read(request, in))
			{
				error = "the plugin has no parameters, or the events are invalid";
				return false;
			}
			params->flushParams(in, out);
			write(reply, out);
			return true;
		}
		default:
			error = "unknown request " + std::to_string(static_cast<int>(type));
			return false;
		}
	}

	bool PluginStub::activate(Reader& request, Writer& reply, std::string& error)
	{
		const double sampleRate = request.f64();
		const uint32_t minFrames = request.u32();
		const uint32_t maxFrames = request.u32();
		const std::string name = request.string();
		if (!request.ok() || m_active)
		{
			error = m_active ? "the plugin is active" : "invalid request";
			return false;
		}
		if (!m_plugin->activate(sampleRate, minFrames, maxFrames))
		{
			error = "the plugin cannot be activated";
			return false;
		}
		const PluginInfo info = describe(*m_plugin, true);
		try
		{
			m_audio = ipc::Connection::open(name);
		}
		catch (const std::exception& exception)
		{
			m_plugin->deactivate();
			error = exception.what();
			return false;
		}
		m_inputPorts.clear();
		m_outputPorts.clear();
		for (const AudioPortInfo& port : info.inputs)
			m_inputPorts.push_back(port.channelCount);
		for (const AudioPortInfo& port : info.outputs)
			m_outputPorts.push_back(port.channelCount);
		m_inputChannels = 0;
		for (const uint32_t channels : m_inputPorts)
			m_inputChannels += channels;
		m_outputChannels = 0;
		for (const uint32_t channels : m_outputPorts)
			m_outputChannels += channels;
		if (m_inputChannels > m_audio.config().maxChannels || m_outputChannels > m_audio.config().maxChannels ||
			maxFrames > m_audio.config().maxFrames)
		{
			m_audio = ipc::Connection();
			m_plugin->deactivate();
			error = "the audio connection is too small for the plugin";
			return false;
		}
		m_inputs.assign(m_inputPorts.size(), AudioBuffer{});
		m_outputs.assign(m_outputPorts.size(), AudioBuffer{});
		m_inputPointers.assign(m_inputChannels, nullptr);
		m_outputPointers.assign(m_outputChannels, nullptr);
		m_processing = false;
		m_stop = false;
		m_done = false;
		m_active = true;
		m_thread = std::thread([this] { runAudio(); });
		write(reply, info);
		return true;
	}

	bool PluginStub::stopAudio(std::chrono::milliseconds timeout)
	{
		if (!m_thread.joinable())
			return true;
		m_stop = true;
		{
			std::unique_lock lock(m_mutex);
			if (!m_stopped.wait_for(lock, timeout, [this] { return m_done; }))
				return false;
		}
		m_thread.join();
		return true;
	}

	void PluginStub::runAudio() noexcept
	{
		if (!platform::setRealtimePriority())
			logging::info("The plugin host's audio thread runs without real-time priority");
		{
			const rt::ScopedRealtimeContext realtime;
			while (!m_stop.load(std::memory_order_acquire))
			{
				const ipc::WaitResult wait = m_audio.waitForBlock(std::chrono::milliseconds(20));
				ipc::AudioBlock input;
				ipc::ReadStatus status = ipc::ReadStatus::Empty;
				bool valid = true;
				while (valid && (status = m_audio.receiveBlock(input)) == ipc::ReadStatus::Ok)
					valid = processBlock(input);
				if (!valid || status == ipc::ReadStatus::Corrupt || wait == ipc::WaitResult::PeerClosed)
					break;
			}
			if (m_processing)
				m_plugin->stopProcessing();
			m_processing = false;
		}
		const std::scoped_lock lock(m_mutex);
		m_done = true;
		m_stopped.notify_all();
	}

	bool PluginStub::processBlock(const ipc::AudioBlock& input) noexcept
	{
		m_in.clear();
		m_out.clear();
		ProcessInfo info;
		uint32_t segments = 0;
		Event sysex;
		bool pendingSysex = false;
		for (auto& buffer : m_inputs)
			buffer.constantMask = 0;
		for (uint32_t i = 0; i < input.eventCount; ++i)
		{
			ipc::MessageHeader header;
			if (m_audio.receiveEvent(header, m_message.data(), m_message.size()) != ipc::ReadStatus::Ok)
				return false;
			switch (static_cast<AudioMessage>(header.type))
			{
			case AudioMessage::ProcessInfo:
			{
				if (header.size < sizeof(info))
					return false;
				std::memcpy(&info, m_message.data(), sizeof(info));
				const auto masks = std::min<size_t>(
					{info.portCount, m_inputs.size(), (header.size - sizeof(info)) / sizeof(uint64_t)});
				for (size_t port = 0; port < masks; ++port)
					std::memcpy(&m_inputs[port].constantMask,
						m_message.data() + sizeof(info) + (port * sizeof(uint64_t)), sizeof(uint64_t));
				break;
			}
			case AudioMessage::Transport:
				if (header.size != sizeof(TransportSegment))
					return false;
				if (segments < m_transport.size())
					std::memcpy(&m_transport[segments++], m_message.data(), sizeof(TransportSegment));
				break;
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
					sysex = event;
				else
					m_in.push(event);
				break;
			}
			case AudioMessage::SysexBytes:
				if (pendingSysex)
					m_in.pushSysex(
						sysex.offset, sysex.port, reinterpret_cast<const uint8_t*>(m_message.data()), header.size);
				pendingSysex = false;
				break;
			default: // from a newer minor version
				break;
			}
		}

		if ((info.flags & Stop) != 0 && m_processing)
		{
			m_plugin->stopProcessing();
			m_processing = false;
		}
		if ((info.flags & Reset) != 0)
			(*m_plugin).reset(); // the plugin's reset, not the pointer's

		ipc::AudioBlock output;
		if (!m_audio.beginBlock(output)) // the engine hasn't read the last answers: it gave up on them
		{
			m_audio.releaseBlock();
			return true;
		}
		ProcessStatus status = ProcessStatus::Continue;
		if (input.frames == 0)
		{
			if ((info.flags & Flush) != 0)
			{
				if (auto* params = gnubase::plugin::extension<Params>(*m_plugin))
					params->flushParams(m_in, m_out);
			}
		}
		else
		{
			if (!m_processing)
				m_processing = m_plugin->startProcessing();
			if (!m_processing || input.channels != m_inputChannels)
			{
				status = ProcessStatus::Error;
			}
			else
			{
				uint32_t channel = 0;
				for (size_t port = 0; port < m_inputs.size(); ++port)
				{
					m_inputs[port].channels = m_inputPointers.data() + channel;
					m_inputs[port].channelCount = m_inputPorts[port];
					for (uint32_t c = 0; c < m_inputPorts[port]; ++c, ++channel)
						m_inputPointers[channel] = input.channel(channel);
				}
				channel = 0;
				for (size_t port = 0; port < m_outputs.size(); ++port)
				{
					m_outputs[port].channels = m_outputPointers.data() + channel;
					m_outputs[port].channelCount = m_outputPorts[port];
					m_outputs[port].constantMask = 0;
					for (uint32_t c = 0; c < m_outputPorts[port]; ++c, ++channel)
						m_outputPointers[channel] = output.channel(channel);
				}
				if (segments == 0) // stopped: the transport at the block's position
				{
					m_transport[0] = TransportSegment{};
					m_transport[0].transport.position = input.position;
					segments = 1;
				}
				const Process process{input.frames, info.steadyTime,
					static_cast<ProcessMode>(std::min<uint8_t>(info.mode, 2)), m_inputs.data(),
					static_cast<uint32_t>(m_inputs.size()), m_outputs.data(), static_cast<uint32_t>(m_outputs.size()),
					m_in, m_out, m_transport.data(), segments};
				status = m_plugin->process(process);
			}
		}

		// The answer: the result with the outputs' constant masks, the output events, then the block.
		ProcessResult result;
		result.status = static_cast<uint8_t>(status);
		result.portCount = static_cast<uint8_t>(std::min<size_t>(m_outputs.size(), g_maxPorts));
		std::memcpy(m_message.data(), &result, sizeof(result));
		for (uint32_t port = 0; port < result.portCount; ++port)
			std::memcpy(m_message.data() + sizeof(result) + (port * sizeof(uint64_t)), &m_outputs[port].constantMask,
				sizeof(uint64_t));
		uint32_t count = 0;
		bool sent = m_audio.sendEvent(static_cast<uint16_t>(AudioMessage::Result), g_version, m_message.data(),
			static_cast<uint32_t>(sizeof(result) + (result.portCount * sizeof(uint64_t))));
		count += sent ? 1 : 0;
		// Events that don't fit into the ring are dropped, as from a full EventList.
		for (const Event& event : m_out)
		{
			if (!sent)
				break;
			sent = m_audio.sendEvent(static_cast<uint16_t>(AudioMessage::Event), g_version, &event, sizeof(event));
			count += sent ? 1 : 0;
			if (sent && event.type == EventType::Sysex)
			{
				const uint32_t size = std::min<uint32_t>(event.data[1], g_maxAudioMessage);
				sent = m_audio.sendEvent(
					static_cast<uint16_t>(AudioMessage::SysexBytes), g_version, m_out.bytes(event), size);
				count += sent ? 1 : 0;
			}
		}
		output.sequence = input.sequence;
		output.position = input.position;
		output.frames = input.frames;
		output.channels = m_outputChannels;
		output.eventCount = count;
		m_audio.releaseBlock();
		m_audio.sendBlock(output);
		return true;
	}
}
