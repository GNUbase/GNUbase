// SPDX-License-Identifier: GPL-3.0-or-later
#include "plugin/remote/stub.h"

#include "platform/log.h"
#include "platform/rt/realtime.h"
#include "platform/thread.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>

namespace gnubase::plugin::remote
{
	namespace
	{
		using namespace std::chrono_literals;

		constexpr auto g_version = g_messageVersion;
		constexpr double g_maxSampleRate = 10'000'000.0;

		bool isCoreEvent(const Event& event)
		{
			return event.space == g_coreEventSpace;
		}
	}

	PluginStub::~PluginStub()
	{
		if (m_active)
		{
			stopAudio(2000ms);
			m_plugin->deactivate();
		}
	}

	void PluginStub::notify(Notification notification, uint32_t detail) noexcept
	{
		if (notification == Notification::RescanParams)
			m_paramRescan.fetch_or(detail, std::memory_order_acq_rel);
		else if (notification == Notification::RescanNotePorts)
			m_notePortRescan.fetch_or(detail, std::memory_order_acq_rel);
		m_notifications.fetch_or(uint32_t{1} << static_cast<uint32_t>(notification), std::memory_order_acq_rel);
	}

	std::vector<std::pair<Notification, uint32_t>> PluginStub::idle()
	{
		if (m_callback.exchange(false, std::memory_order_acq_rel) && m_plugin)
			m_plugin->onMainThread();
		std::vector<std::pair<Notification, uint32_t>> notifications;
		const uint32_t bits = m_notifications.exchange(0, std::memory_order_acq_rel);
		for (uint32_t bit = 1; bit <= g_lastNotification; ++bit)
		{
			if ((bits & (uint32_t{1} << bit)) == 0)
				continue;
			const auto notification = static_cast<Notification>(bit);
			uint32_t detail = 0;
			if (notification == Notification::RescanParams)
				detail = m_paramRescan.exchange(0, std::memory_order_acq_rel);
			else if (notification == Notification::RescanNotePorts)
				detail = m_notePortRescan.exchange(0, std::memory_order_acq_rel);
			notifications.emplace_back(notification, detail);
		}
		return notifications;
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
		case Message::SetProcessMode:
		{
			const uint8_t mode = request.u8();
			if (!request.ok() || mode > static_cast<uint8_t>(ProcessMode::Offline) || m_active)
			{
				error = m_active ? "the plugin is active" : "invalid request";
				return false;
			}
			if (!plugin.setProcessMode(static_cast<ProcessMode>(mode)))
			{
				error = "the plugin refused the process mode";
				return false;
			}
			return true;
		}
		case Message::Describe:
			write(reply, describe(plugin, m_active));
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
			// A state may change the parameters' values and, while inactive, the ports and the parameters themselves.
			write(reply, describe(plugin, m_active));
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
			// The audio thread flushes while the plugin processes (Params::flushParams); never both at once.
			if (m_processing.load(std::memory_order_acquire))
			{
				error = "the plugin is processing";
				return false;
			}
			auto* params = gnubase::plugin::extension<Params>(plugin);
			EventList in(g_maxEvents, g_maxSysexBytes);
			EventList out(g_maxEvents, g_maxSysexBytes);
			if (params == nullptr || !read(request, in))
			{
				error = "the plugin has no parameters, or the events are invalid";
				return false;
			}
			for (const Event& event : in)
			{
				if (event.offset != 0)
				{
					error = "the events of a flush are at offset 0";
					return false;
				}
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
		if (!std::isfinite(sampleRate) || sampleRate <= 0.0 || sampleRate > g_maxSampleRate || minFrames == 0 ||
			minFrames > maxFrames)
		{
			error = "invalid sample rate or block sizes";
			return false;
		}
		try
		{
			m_audio = ipc::Connection::open(name);
		}
		catch (const std::exception& exception)
		{
			error = exception.what();
			return false;
		}
		if (maxFrames > m_audio.config().maxFrames)
		{
			m_audio = ipc::Connection();
			error = "the audio connection is too small for the block size";
			return false;
		}
		if (!m_plugin->activate(sampleRate, minFrames, maxFrames))
		{
			m_audio = ipc::Connection();
			error = "the plugin cannot be activated";
			return false;
		}
		const PluginInfo info = describe(*m_plugin, true);
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
		if (m_inputChannels > m_audio.config().maxChannels || m_outputChannels > m_audio.config().maxChannels)
		{
			m_audio = ipc::Connection();
			m_plugin->deactivate();
			error = "the audio connection is too small for the plugin's ports";
			return false;
		}
		m_minFrames = minFrames;
		m_maxFrames = maxFrames;
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
			if (m_processing.load(std::memory_order_relaxed))
				m_plugin->stopProcessing();
			m_processing.store(false, std::memory_order_release);
		}
		const std::scoped_lock lock(m_mutex);
		m_done = true;
		m_stopped.notify_all();
	}

	bool PluginStub::readBlock(const ipc::AudioBlock& input, ProcessInfo& info, uint32_t& segments) noexcept
	{
		m_in.clear();
		m_out.clear();
		segments = 0;
		Event sysex;
		bool pendingSysex = false;
		bool valid = true;
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
					std::memcpy(&m_transport[segments], m_message.data(), sizeof(TransportSegment));
				++segments;
				break;
			case AudioMessage::Event:
			{
				if (header.size != sizeof(Event))
					return false;
				Event event;
				std::memcpy(&event, m_message.data(), sizeof(event));
				if (isCoreEvent(event) && static_cast<uint8_t>(event.type) > static_cast<uint8_t>(EventType::Sysex))
					return false;
				pendingSysex = isCoreEvent(event) && event.type == EventType::Sysex;
				if (pendingSysex)
					sysex = event;
				else
					valid = m_in.push(event) && valid; // refused: out of order or too many
				break;
			}
			case AudioMessage::SysexBytes:
				if (pendingSysex)
					valid = m_in.pushSysex(sysex.offset, sysex.port, reinterpret_cast<const uint8_t*>(m_message.data()),
								header.size) &&
						valid;
				pendingSysex = false;
				break;
			default: // from a newer minor version
				break;
			}
		}
		return valid;
	}

	bool PluginStub::validBlock(const ipc::AudioBlock& input, const ProcessInfo& info, uint32_t segments) const noexcept
	{
		// The engine is trusted no more than the plugin is: a block must fit what the plugin was activated with.
		if (input.frames > m_maxFrames || segments != info.transportCount || segments > m_transport.size())
			return false;
		const uint32_t frames = input.frames;
		for (const Event& event : m_in)
		{
			if (event.offset >= std::max<uint32_t>(frames, 1))
				return false;
		}
		if (frames == 0)
			return true;
		if (input.channels != m_inputChannels || segments == 0 || m_transport[0].offset != 0)
			return false;
		for (uint32_t i = 1; i < segments; ++i)
		{
			if (m_transport[i].offset <= m_transport[i - 1].offset || m_transport[i].offset >= frames)
				return false;
		}
		return true;
	}

	bool PluginStub::processBlock(const ipc::AudioBlock& input) noexcept
	{
		ProcessInfo info;
		uint32_t segments = 0;
		if (!readBlock(input, info, segments))
			return false; // the event ring is out of step with the blocks: the engine broke the protocol
		const bool valid = validBlock(input, info, segments);
		if (!valid)
			logging::error("The engine sent an invalid block of {} frames", input.frames);

		ipc::AudioBlock output;
		if (!m_audio.beginBlock(output)) // the engine hasn't read the last answers: it gave up on them
		{
			m_audio.releaseBlock();
			return true;
		}
		ProcessStatus status = ProcessStatus::Continue;
		bool processing = m_processing.load(std::memory_order_relaxed);
		if (!valid)
		{
			status = ProcessStatus::Error;
		}
		else
		{
			if ((info.flags & Stop) != 0 && processing)
			{
				m_plugin->stopProcessing();
				processing = false;
				m_processing.store(false, std::memory_order_release);
			}
			if ((info.flags & Reset) != 0)
				(*m_plugin).reset(); // the plugin's reset, not the pointer's
			if ((info.flags & Start) != 0 && !processing)
			{
				processing = m_plugin->startProcessing();
				m_processing.store(processing, std::memory_order_release);
				status = processing ? ProcessStatus::Continue : ProcessStatus::Error;
			}
		}
		if (valid && input.frames == 0)
		{
			if ((info.flags & Flush) != 0)
			{
				if (auto* params = gnubase::plugin::extension<Params>(*m_plugin))
					params->flushParams(m_in, m_out);
			}
		}
		else if (valid && !processing)
		{
			status = ProcessStatus::Error; // blocks only come between Start and Stop
		}
		else if (valid)
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
			const Process process{input.frames, info.steadyTime,
				static_cast<ProcessMode>(std::min<uint8_t>(info.mode, 2)), m_inputs.data(),
				static_cast<uint32_t>(m_inputs.size()), m_outputs.data(), static_cast<uint32_t>(m_outputs.size()), m_in,
				m_out, m_transport.data(), segments};
			status = m_plugin->process(process);
		}

		// The answer: the result with the outputs' constant masks, the output events, then the block.
		ProcessResult result;
		result.status = static_cast<uint8_t>(status);
		result.portCount = static_cast<uint8_t>(std::min<size_t>(m_outputs.size(), g_maxPorts));
		result.flags = m_resultFlags.exchange(0, std::memory_order_relaxed);
		// The events the plugin's output list refused. The event ring has room for two blocks of a full list, so the
		// events that went into the list fit.
		result.droppedEvents = static_cast<uint16_t>(std::min<size_t>(m_out.dropped(), 0xffff));
		const size_t resultSize = sizeof(result) + (result.portCount * sizeof(uint64_t));
		uint32_t count = 1;
		std::memcpy(m_message.data(), &result, sizeof(result));
		for (uint32_t port = 0; port < result.portCount; ++port)
			std::memcpy(m_message.data() + sizeof(result) + (port * sizeof(uint64_t)), &m_outputs[port].constantMask,
				sizeof(uint64_t));
		bool sent = m_audio.sendEvent(static_cast<uint16_t>(AudioMessage::Result), g_version, m_message.data(),
			static_cast<uint32_t>(resultSize));
		if (!sent)
			count = 0;
		for (const Event& event : m_out)
		{
			if (!sent)
				break;
			sent = m_audio.sendEvent(static_cast<uint16_t>(AudioMessage::Event), g_version, &event, sizeof(event));
			count += sent ? 1 : 0;
			if (sent && isCoreEvent(event) && event.type == EventType::Sysex)
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
