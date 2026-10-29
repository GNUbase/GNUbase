// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/plugins/pluginslot.h"

#include "engine/graph/graph.h"
#include "engine/midi/midivalues.h"
#include "platform/rt/realtime.h"

#include <algorithm>
#include <cstring>

namespace gnubase::engine::plugins
{
	namespace
	{
		constexpr size_t g_maxTransportSegments = 64;
		constexpr uint32_t g_maxBank = 16383; // MIDI's bank select: 14 bits
		constexpr uint32_t g_bankSelect = 0;
		constexpr uint32_t g_bankSelectLsb = 32;

		void silence(const graph::ProcessContext& context) noexcept
		{
			for (int c = 0; c < context.channels; ++c)
				std::fill(context.audio[c], context.audio[c] + context.frames, 0.0f);
		}
	}

	PluginSlot::PluginSlot(std::unique_ptr<plugin::Plugin> plugin)
		: m_plugin(std::move(plugin)), m_eventsIn(graph::g_maxEventsPerBlock, 65536),
		  m_eventsOut(graph::g_maxEventsPerBlock, 65536), m_transport(g_maxTransportSegments)
	{
		m_ports = plugin::extension<plugin::AudioPorts>(*m_plugin);
		m_latency = plugin::extension<plugin::Latency>(*m_plugin);
	}

	PluginSlot::~PluginSlot()
	{
		if (!m_active)
			return;
		// No graph processes the slot any more.
		if (m_started)
			m_plugin->stopProcessing();
		m_plugin->deactivate();
	}

	void PluginSlot::allocate(bool input, uint32_t maxFrames)
	{
		std::vector<Port>& ports = input ? m_inputPorts : m_outputPorts;
		std::vector<plugin::AudioBuffer>& buffers = input ? m_inputs : m_outputs;
		uint32_t& main = input ? m_mainInput : m_mainOutput;
		const uint32_t count = m_ports != nullptr ? m_ports->audioPortCount(input) : 0;
		ports.assign(count, Port{});
		buffers.assign(count, plugin::AudioBuffer{});
		main = 0;
		bool foundMain = false;
		for (uint32_t i = 0; i < count; ++i)
		{
			plugin::AudioPortInfo info;
			m_ports->audioPortInfo(input, i, info);
			Port& port = ports[i];
			port.samples.assign(size_t{info.channelCount} * maxFrames, 0.0f);
			port.channels.resize(info.channelCount);
			for (uint32_t c = 0; c < info.channelCount; ++c)
				port.channels[c] = port.samples.data() + (size_t{c} * maxFrames);
			buffers[i].channels = port.channels.data();
			buffers[i].channelCount = info.channelCount;
			if (!foundMain && (info.flags & plugin::AudioPortInfo::Main) != 0)
			{
				main = i;
				foundMain = true;
			}
		}
	}

	void PluginSlot::prepare(double sampleRate, uint32_t maxFrames)
	{
		rt::assertNotRealtime("PluginSlot::prepare");
		if (m_active)
		{
			if (m_started)
				m_plugin->stopProcessing();
			m_plugin->deactivate();
		}
		m_started = false;
		m_plugin->setProcessMode(m_mode.load(std::memory_order_relaxed));
		m_active = m_plugin->activate(sampleRate, 1, maxFrames);
		// The ports as activate left them: a plugin may change them while inactive, and a remote one reads them again.
		allocate(true, maxFrames);
		allocate(false, maxFrames);
	}

	uint32_t PluginSlot::latency() const
	{
		return m_active && m_latency != nullptr ? m_latency->latency() : 0;
	}

	void convertEvent(const graph::ProcessEvent& event, uint32_t frames, plugin::EventList& out) noexcept
	{
		using Type = graph::ProcessEvent::Type;
		plugin::Event converted;
		converted.offset = std::min(event.offset, frames - 1);
		converted.port = 0;
		converted.channel = event.channel;
		converted.key = event.key;
		// A MIDI 1.0 message of up to three bytes, on the event's channel.
		const auto midi1 = [&](uint32_t status, uint32_t data1, uint32_t data2 = 0)
		{
			converted.type = plugin::EventType::Midi;
			converted.key = -1;
			converted.data[0] = (status | (event.channel & 0x0FU)) | ((data1 & 0x7FU) << 8) | ((data2 & 0x7FU) << 16);
			out.push(converted);
		};
		switch (event.type)
		{
		case Type::NoteOn:
		case Type::NoteOff:
			converted.type = event.type == Type::NoteOn ? plugin::EventType::NoteOn : plugin::EventType::NoteOff;
			converted.value = event.velocity / 65535.0;
			out.push(converted);
			break;
		case Type::Controller:
			midi1(0xB0U, event.key, midi::controllerToMidi1(event.value));
			break;
		case Type::PitchBend:
		{
			const uint16_t bend = midi::pitchBendToMidi1(event.value);
			midi1(0xE0U, bend & 0x7FU, bend >> 7U);
			break;
		}
		case Type::ChannelPressure:
			midi1(0xD0U, midi::controllerToMidi1(event.value));
			break;
		case Type::PolyPressure:
			midi1(0xA0U, event.key, midi::controllerToMidi1(event.value));
			break;
		case Type::Program:
			if (event.value <= g_maxBank)
			{
				midi1(0xB0U, g_bankSelect, event.value >> 7U);
				midi1(0xB0U, g_bankSelectLsb, event.value);
			}
			midi1(0xC0U, event.key);
			break;
		case Type::SysEx:
			out.pushSysex(converted.offset, 0, event.data, event.data != nullptr ? event.size : 0);
			break;
		case Type::Parameter:
			converted.type = plugin::EventType::ParamValue;
			converted.port = -1;
			converted.channel = -1;
			converted.key = -1;
			converted.param = event.parameter;
			converted.value = event.parameterValue;
			out.push(converted);
			break;
		}
	}

	void PluginSlot::convertEvents(const graph::ProcessContext& context) noexcept
	{
		m_eventsIn.clear();
		for (const graph::ProcessEvent& event : context.eventsIn)
			convertEvent(event, context.frames, m_eventsIn);
	}

	uint32_t PluginSlot::convertTransport(const graph::TransportBlock& transport) noexcept
	{
		if (!transport.playing || transport.segmentCount == 0)
		{
			m_transport[0] = plugin::TransportSegment{};
			m_transport[0].transport.position = transport.position;
			return 1;
		}
		const auto count = static_cast<uint32_t>(std::min(transport.segmentCount, m_transport.size()));
		for (uint32_t i = 0; i < count; ++i)
		{
			const graph::TransportSegment& segment = transport.segments[i];
			plugin::TransportSegment& converted = m_transport[i];
			converted = plugin::TransportSegment{};
			converted.offset = segment.offset;
			converted.transport.flags = plugin::Transport::Playing;
			converted.transport.position = segment.position;
		}
		return count;
	}

	void PluginSlot::process(const graph::ProcessContext& context) noexcept
	{
		if (!m_active || context.frames == 0)
		{
			silence(context);
			return;
		}
		if (!m_started)
			m_started = m_plugin->startProcessing();

		// The node's audio goes to the main input; a mono node feeds every channel of it.
		for (uint32_t port = 0; port < m_inputs.size(); ++port)
		{
			plugin::AudioBuffer& buffer = m_inputs[port];
			buffer.constantMask = 0;
			for (uint32_t c = 0; c < buffer.channelCount; ++c)
			{
				float* samples = m_inputPorts[port].channels[c];
				const int source = context.channels == 1 ? 0 : static_cast<int>(c);
				if (port == m_mainInput && source < context.channels)
					std::memcpy(samples, context.audio[source], context.frames * sizeof(float));
				else
					std::fill(samples, samples + context.frames, 0.0f);
			}
		}
		convertEvents(context);
		m_eventsOut.clear();
		const plugin::Process process{context.frames, m_steadyTime, m_mode.load(std::memory_order_relaxed),
			m_inputs.data(), static_cast<uint32_t>(m_inputs.size()), m_outputs.data(),
			static_cast<uint32_t>(m_outputs.size()), m_eventsIn, m_eventsOut, m_transport.data(),
			convertTransport(context.transport)};
		const plugin::ProcessStatus status = m_started ? m_plugin->process(process) : plugin::ProcessStatus::Error;
		m_steadyTime += context.frames;

		const bool failed = status == plugin::ProcessStatus::Error || m_outputs.empty();
		m_failed.store(failed, std::memory_order_relaxed);
		if (failed)
		{
			silence(context);
			return;
		}
		// The main output becomes the node's audio; a mono output feeds every channel of the node.
		const plugin::AudioBuffer& output = m_outputs[m_mainOutput];
		for (int c = 0; c < context.channels; ++c)
		{
			const uint32_t source = output.channelCount == 1 ? 0 : static_cast<uint32_t>(c);
			if (source < output.channelCount)
				std::memcpy(context.audio[c], output.channels[source], context.frames * sizeof(float));
			else
				std::fill(context.audio[c], context.audio[c] + context.frames, 0.0f);
		}
		for (const plugin::Event& event : m_eventsOut)
		{
			if (event.type != plugin::EventType::NoteOn && event.type != plugin::EventType::NoteOff)
				continue;
			graph::ProcessEvent converted;
			converted.offset = event.offset;
			converted.type = event.type == plugin::EventType::NoteOn ? graph::ProcessEvent::Type::NoteOn
																	 : graph::ProcessEvent::Type::NoteOff;
			converted.channel = static_cast<uint8_t>(std::clamp<int>(event.channel, 0, 15));
			converted.key = static_cast<uint8_t>(std::clamp<int>(event.key, 0, 127));
			converted.velocity = static_cast<uint16_t>(std::clamp(event.value, 0.0, 1.0) * 65535.0);
			context.eventsOut.add(converted);
		}
	}
}
