// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/processors/builtinplugin.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>

namespace gnubase::engine::processors
{
	namespace
	{
		plugin::Descriptor describe(const BuiltinInfo& info)
		{
			plugin::Descriptor descriptor;
			descriptor.id = "builtin:" + info.id;
			descriptor.format = "builtin";
			descriptor.formatId = info.id;
			descriptor.name = info.name;
			descriptor.vendor = "GNUbase";
			descriptor.version = GNUBASE_VERSION;
			descriptor.features = {info.instrument ? "instrument" : "audio-effect"};
			return descriptor;
		}

		plugin::ChannelLayout layoutOf(uint32_t channels)
		{
			switch (channels)
			{
			case 1:
				return plugin::ChannelLayout::Mono;
			case 2:
				return plugin::ChannelLayout::Stereo;
			default:
				return plugin::ChannelLayout::Other; // more channels, in no particular order
			}
		}
	}

	BuiltinPlugin::BuiltinPlugin(const BuiltinInfo& info, plugin::Host& host, uint32_t channels)
		: m_info(info), m_host(host), m_channels(channels), m_descriptor(describe(info)),
		  m_values(info.parameters.size()), m_pending(info.parameters.size())
	{
	}

	bool BuiltinPlugin::init()
	{
		m_processor = createBuiltin(m_info.id);
		for (size_t i = 0; i < m_info.parameters.size(); ++i)
		{
			m_values[i] = m_info.parameters[i].defaultValue;
			m_pending[i] = false;
		}
		return true;
	}

	bool BuiltinPlugin::activate(double sampleRate, uint32_t /*minFrames*/, uint32_t maxFrames)
	{
		if (!m_processor)
			return false;
		m_processor->prepareFor(sampleRate, maxFrames);
		m_maxFrames = maxFrames;
		m_segments.resize(g_maxTransportSegments);
		// Values set while inactive reach the processor with the first block.
		for (std::atomic<bool>& pending : m_pending)
			pending = true;
		return true;
	}

	void BuiltinPlugin::reset() noexcept
	{
		if (m_processor)
			(*m_processor).reset();
	}

	plugin::ProcessStatus BuiltinPlugin::process(const plugin::Process& process) noexcept
	{
		const uint32_t inputs = m_info.instrument ? 0 : 1;
		if (!m_processor || process.audioInputCount != inputs || process.audioOutputCount != 1 ||
			process.audioOutputs[0].channelCount != m_channels ||
			(inputs == 1 && process.audioInputs[0].channelCount != m_channels) || process.transportCount == 0 ||
			process.frames > m_maxFrames)
			return plugin::ProcessStatus::Error;

		// graph::Processor processes in place, in the plugin's output.
		const plugin::AudioBuffer& out = process.audioOutputs[0];
		for (uint32_t c = 0; c < m_channels; ++c)
		{
			if (inputs == 1)
				std::memcpy(out.channels[c], process.audioInputs[0].channels[c], process.frames * sizeof(float));
			else
				std::fill(out.channels[c], out.channels[c] + process.frames, 0.0f);
		}

		m_events.clear();
		m_eventsOut.clear();
		applyPending();
		for (const plugin::Event& event : process.eventsIn)
			translate(event);

		const graph::TransportBlock transport = convert(process);
		m_processor->process(
			{out.channels, static_cast<int>(m_channels), process.frames, m_events, m_eventsOut, transport});
		return plugin::ProcessStatus::Continue;
	}

	void* BuiltinPlugin::extension(std::string_view id)
	{
		return plugin::provide<plugin::AudioPorts, plugin::Params, plugin::State, plugin::Latency, plugin::Tail>(
			this, id);
	}

	uint32_t BuiltinPlugin::audioPortCount(bool input) const
	{
		return input && m_info.instrument ? 0 : 1;
	}

	bool BuiltinPlugin::audioPortInfo(bool input, uint32_t index, plugin::AudioPortInfo& info) const
	{
		if (index >= audioPortCount(input))
			return false;
		info.id = 0;
		info.name = input ? "Input" : "Output";
		info.channelCount = m_channels;
		info.layout = layoutOf(m_channels);
		info.flags = plugin::AudioPortInfo::Main;
		return true;
	}

	uint32_t BuiltinPlugin::paramCount() const
	{
		return static_cast<uint32_t>(m_info.parameters.size());
	}

	bool BuiltinPlugin::paramInfo(uint32_t index, plugin::ParamInfo& info) const
	{
		if (index >= paramCount())
			return false;
		const ParameterInfo& parameter = m_info.parameters[index];
		info = {}; // the host may pass one it used before
		info.id = parameter.id;
		info.flags = plugin::ParamInfo::Automatable;
		if (parameter.stepped)
		{
			// One step for each whole number from the minimum to the maximum.
			info.flags |= plugin::ParamInfo::Stepped;
			info.stepCount = static_cast<uint32_t>(std::lround(parameter.maximum - parameter.minimum));
		}
		if (!parameter.options.empty())
			info.flags |= plugin::ParamInfo::Enum;
		info.name = parameter.name;
		info.minimum = parameter.minimum;
		info.maximum = parameter.maximum;
		info.defaultValue = parameter.defaultValue;
		return true;
	}

	bool BuiltinPlugin::paramValue(uint32_t param, double& value) const
	{
		const int index = indexOf(param);
		if (index < 0)
			return false;
		value = m_values[static_cast<size_t>(index)];
		return true;
	}

	bool BuiltinPlugin::paramValueToText(uint32_t param, double value, std::string& text) const
	{
		const int index = indexOf(param);
		if (index < 0)
			return false;
		text = valueToText(m_info.parameters[static_cast<size_t>(index)], value);
		return true;
	}

	bool BuiltinPlugin::paramTextToValue(uint32_t param, std::string_view text, double& value) const
	{
		const int index = indexOf(param);
		if (index < 0)
			return false;
		const std::optional<double> parsed = textToValue(m_info.parameters[static_cast<size_t>(index)], text);
		if (!parsed.has_value())
			return false;
		value = *parsed;
		return true;
	}

	void BuiltinPlugin::flushParams(const plugin::EventList& in, plugin::EventList& /*out*/) noexcept
	{
		for (const plugin::Event& event : in)
			if (event.space == plugin::g_coreEventSpace && event.type == plugin::EventType::ParamValue)
				setValue(event.param, event.value);
	}

	bool BuiltinPlugin::saveState(std::vector<uint8_t>& data, plugin::StateContext /*context*/)
	{
		data.clear();
		for (size_t i = 0; i < m_info.parameters.size(); ++i)
		{
			const uint32_t id = m_info.parameters[i].id;
			const double value = m_values[i];
			const size_t at = data.size();
			data.resize(at + sizeof(id) + sizeof(value));
			std::memcpy(data.data() + at, &id, sizeof(id));
			std::memcpy(data.data() + at + sizeof(id), &value, sizeof(value));
		}
		return true;
	}

	bool BuiltinPlugin::loadState(const uint8_t* data, size_t size, plugin::StateContext /*context*/)
	{
		constexpr size_t entry = sizeof(uint32_t) + sizeof(double);
		if (size % entry != 0)
			return false;
		for (size_t at = 0; at < size; at += entry)
		{
			uint32_t id = 0;
			double value = 0.0;
			std::memcpy(&id, data + at, sizeof(id));
			std::memcpy(&value, data + at + sizeof(id), sizeof(value));
			setValue(id, value);
		}
		if (auto* hostParams = plugin::extension<plugin::HostParams>(m_host))
			hostParams->rescanParams(plugin::HostParams::Values);
		return true;
	}

	uint32_t BuiltinPlugin::latency() const
	{
		return m_processor ? m_processor->latency() : 0;
	}

	uint32_t BuiltinPlugin::tail() const noexcept
	{
		// UINT32_MAX, the processor's endless, is g_infiniteTail.
		return m_processor ? m_processor->tail() : 0;
	}

	int BuiltinPlugin::indexOf(uint32_t param) const
	{
		for (size_t i = 0; i < m_info.parameters.size(); ++i)
			if (m_info.parameters[i].id == param)
				return static_cast<int>(i);
		return -1;
	}

	void BuiltinPlugin::setValue(uint32_t param, double value) noexcept
	{
		const int index = indexOf(param);
		if (index < 0)
			return;
		m_values[static_cast<size_t>(index)] = value;
		m_pending[static_cast<size_t>(index)] = true;
	}

	void BuiltinPlugin::applyPending() noexcept
	{
		for (size_t i = 0; i < m_pending.size(); ++i)
			if (m_pending[i].exchange(false))
				addParameter(0, m_info.parameters[i].id, m_values[i]);
	}

	void BuiltinPlugin::addParameter(uint32_t offset, uint32_t param, double value) noexcept
	{
		graph::ProcessEvent event;
		event.offset = offset;
		event.type = graph::ProcessEvent::Type::Parameter;
		event.parameter = param;
		event.parameterValue = value;
		m_events.add(event);
	}

	void BuiltinPlugin::translate(const plugin::Event& event) noexcept
	{
		// Types are numbered per namespace: an extension's event can have the number of a core type.
		if (event.space != plugin::g_coreEventSpace)
			return;
		if (event.type == plugin::EventType::ParamValue)
		{
			const int index = indexOf(event.param);
			if (index < 0)
				return;
			m_values[static_cast<size_t>(index)] = event.value;
			addParameter(event.offset, event.param, event.value);
		}
		else if ((event.type == plugin::EventType::NoteOn || event.type == plugin::EventType::NoteOff) &&
			event.key >= 0)
		{
			graph::ProcessEvent note;
			note.offset = event.offset;
			note.type = event.type == plugin::EventType::NoteOn ? graph::ProcessEvent::Type::NoteOn
																: graph::ProcessEvent::Type::NoteOff;
			note.channel = static_cast<uint8_t>(std::max<int16_t>(event.channel, 0));
			note.key = static_cast<uint8_t>(event.key);
			note.velocity = static_cast<uint16_t>(std::lround(std::clamp(event.value, 0.0, 1.0) * 65535.0));
			m_events.add(note);
		}
	}

	graph::TransportBlock BuiltinPlugin::convert(const plugin::Process& process) noexcept
	{
		const plugin::Transport& first = process.transport[0].transport;
		graph::TransportBlock block;
		block.playing = (first.flags & plugin::Transport::Playing) != 0;
		block.position = first.position;
		if ((first.flags & plugin::Transport::TempoValid) != 0 && first.tempo > 0.0)
			block.tempo = first.tempo;
		if (!block.playing)
			return block;
		const size_t count = std::min<size_t>(process.transportCount, m_segments.size());
		for (size_t i = 0; i < count; ++i)
		{
			const uint32_t begin = process.transport[i].offset;
			const uint32_t end = i + 1 < count ? process.transport[i + 1].offset : process.frames;
			m_segments[i] = {begin, end - begin, process.transport[i].transport.position};
		}
		block.segments = m_segments.data();
		block.segmentCount = count;
		return block;
	}

	BuiltinFactory::BuiltinFactory(uint32_t channels) : m_channels(channels)
	{
		for (const BuiltinInfo& info : builtins())
			m_descriptors.push_back(describe(info));
	}

	std::unique_ptr<plugin::Plugin> BuiltinFactory::create(std::string_view id, plugin::Host& host)
	{
		for (const BuiltinInfo& info : builtins())
			if (id == "builtin:" + info.id)
				return std::make_unique<BuiltinPlugin>(info, host, m_channels);
		return nullptr;
	}
}
