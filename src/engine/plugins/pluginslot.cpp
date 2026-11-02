// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/plugins/pluginslot.h"

#include "engine/document/document.h"
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
		if (auto* params = plugin::extension<plugin::Params>(*m_plugin))
		{
			const uint32_t count = params->paramCount();
			for (uint32_t i = 0; i < count; ++i)
			{
				plugin::ParamInfo info;
				if (!params->paramInfo(i, info))
					continue;
				m_parameterIds.push_back(info.id);
				m_defaults.push_back(info.defaultValue);
			}
		}
		m_set.resize(m_parameterIds.size());
		m_targets = std::make_unique<std::atomic<double>[]>(m_parameterIds.size());
		m_changed = std::make_unique<std::atomic<bool>[]>(m_parameterIds.size());
		for (size_t i = 0; i < m_parameterIds.size(); ++i)
		{
			m_targets[i].store(0.0, std::memory_order_relaxed);
			m_changed[i].store(false, std::memory_order_relaxed);
		}
	}

	PluginSlot::~PluginSlot()
	{
		// No graph processes the slot any more.
		deactivate();
	}

	void PluginSlot::deactivate()
	{
		rt::assertNotRealtime("PluginSlot::deactivate");
		if (!m_active)
			return;
		if (m_started)
			m_plugin->stopProcessing();
		m_plugin->deactivate();
		m_active = false;
		m_started = false;
		m_activeRate.store(0.0, std::memory_order_relaxed);
		m_activeFrames.store(0, std::memory_order_relaxed);
	}

	void PluginSlot::destroy()
	{
		if (!m_plugin)
			return;
		deactivate();
		m_plugin = nullptr;
		m_ports = nullptr;
		m_latency = nullptr;
	}

	bool PluginSlot::activeFor(double sampleRate, uint32_t maxFrames) const
	{
		return m_activeRate.load(std::memory_order_relaxed) == sampleRate &&
			m_activeFrames.load(std::memory_order_relaxed) == maxFrames;
	}

	bool PluginSlot::loadState(const std::vector<uint8_t>& state)
	{
		rt::assertNotRealtime("PluginSlot::loadState");
		auto* extension = m_plugin ? plugin::extension<plugin::State>(*m_plugin) : nullptr;
		return extension != nullptr && extension->loadState(state.data(), state.size(), plugin::StateContext::Project);
	}

	void PluginSlot::setParameters(const std::map<uint32_t, double>& values)
	{
		bool changed = false;
		for (size_t i = 0; i < m_parameterIds.size(); ++i)
		{
			const auto value = values.find(m_parameterIds[i]);
			std::optional<double> wanted;
			if (value != values.end())
				wanted = value->second;
			if (wanted == m_set[i])
				continue;
			// A value that went goes back to the default.
			m_targets[i].store(wanted.value_or(m_defaults[i]), std::memory_order_relaxed);
			m_changed[i].store(true, std::memory_order_release);
			m_set[i] = wanted;
			changed = true;
		}
		if (changed)
			m_changes.fetch_add(1, std::memory_order_release);
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
		activate(sampleRate, maxFrames);
	}

	bool PluginSlot::activate(double sampleRate, uint32_t maxFrames)
	{
		rt::assertNotRealtime("PluginSlot::activate");
		if (!m_plugin)
			return false;
		if (m_active && activeFor(sampleRate, maxFrames))
			return true;
		deactivate();
		m_plugin->setProcessMode(m_mode.load(std::memory_order_relaxed));
		m_active = m_plugin->activate(sampleRate, 1, maxFrames);
		// The ports as activate left them: a plugin may change them while inactive, and a remote one reads them again.
		allocate(true, maxFrames);
		allocate(false, maxFrames);
		m_delay = m_active && m_latency != nullptr ? m_latency->latency() : 0;
		m_dryLength = m_delay + maxFrames;
		m_dry.assign(size_t{g_maxAudioChannels} * m_dryLength, 0.0f);
		m_dryWrite = 0;
		m_maxFrames = maxFrames;
		m_bypassed = bypassed();
		if (m_active)
		{
			m_activeRate.store(sampleRate, std::memory_order_relaxed);
			m_activeFrames.store(maxFrames, std::memory_order_relaxed);
		}
		return m_active;
	}

	uint32_t PluginSlot::latency() const
	{
		// Bypassed too: the input is delayed by as much.
		return m_active ? m_delay : 0;
	}

	void PluginSlot::addParameterChanges() noexcept
	{
		const uint64_t changes = m_changes.load(std::memory_order_acquire);
		if (changes == m_seenChanges)
			return;
		// At most half a block's events, so the block's own events have room; the rest follow in the next blocks.
		size_t room = graph::g_maxEventsPerBlock / 2;
		for (size_t i = 0; i < m_parameterIds.size(); ++i)
		{
			if (room == 0)
				return; // m_seenChanges stays behind: the next block looks again
			if (!m_changed[i].exchange(false, std::memory_order_acquire))
				continue;
			plugin::Event event;
			event.type = plugin::EventType::ParamValue;
			event.port = -1;
			event.channel = -1;
			event.key = -1;
			event.param = m_parameterIds[i];
			event.value = m_targets[i].load(std::memory_order_relaxed);
			if (!m_eventsIn.push(event))
			{
				m_changed[i].store(true, std::memory_order_relaxed);
				return;
			}
			--room;
		}
		m_seenChanges = changes;
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
		addParameterChanges(); // at offset 0, before the block's own events
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
		const bool bypass = bypassed();
		const bool crossfade = bypass != m_bypassed;
		m_bypassed = bypass;
		const int dryChannels = std::min(context.channels, g_maxAudioChannels);
		const bool fits = context.frames <= m_maxFrames;
		const bool dry = (bypass || crossfade) && fits;
		// Every block's input goes into the ring, so a bypass that starts has the delayed input at hand.
		const uint32_t written = m_dryWrite;
		if (fits)
		{
			for (int c = 0; c < dryChannels; ++c)
			{
				float* ring = m_dry.data() + (static_cast<size_t>(c) * m_dryLength);
				const uint32_t first = std::min(context.frames, m_dryLength - written);
				std::memcpy(ring + written, context.audio[c], first * sizeof(float));
				std::memcpy(ring, context.audio[c] + first, (context.frames - first) * sizeof(float));
			}
			m_dryWrite = (written + context.frames) % m_dryLength;
		}

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
			silence(context);
		else
			output(context, bypass);
		if (!dry)
			return;
		// The delayed input instead, or a crossfade to or from it over the block.
		const uint32_t start = (written + m_dryLength - m_delay) % m_dryLength;
		for (int c = 0; c < dryChannels; ++c)
		{
			const float* ring = m_dry.data() + (static_cast<size_t>(c) * m_dryLength);
			float* samples = context.audio[c];
			uint32_t read = start;
			for (uint32_t i = 0; i < context.frames; ++i)
			{
				const float input = ring[read];
				read = read + 1 == m_dryLength ? 0 : read + 1;
				if (!crossfade)
				{
					samples[i] = input;
					continue;
				}
				const float t = static_cast<float>(i + 1) / static_cast<float>(context.frames);
				samples[i] += (bypass ? t : 1.0f - t) * (input - samples[i]);
			}
		}
	}

	void PluginSlot::output(const graph::ProcessContext& context, bool bypass) noexcept
	{
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
		if (bypass)
			return; // nor its notes
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
