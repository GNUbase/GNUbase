// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/graph/events.h"
#include "engine/graph/graph.h"
#include "engine/graph/processor.h"
#include "engine/processors/builtin.h"
#include "plugin/extensions.h"
#include "plugin/plugin.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// GNUbase's built-in processors as built-in plugins: the plugin API (docs/plugin-api.md) on top of the built-in
// registry, so a plugin slot hosts the mixing effects like any other plugin, inside the engine.
namespace gnubase::engine::processors
{
	// One built-in processor behind the plugin API: a main audio input (none for instruments) and output with the
	// same channels, the processor's parameters with sample-accurate changes, notes, its latency and tail, and the
	// parameter values as its state.
	class BuiltinPlugin final : public plugin::Plugin,
								public plugin::AudioPorts,
								public plugin::Params,
								public plugin::State,
								public plugin::Latency,
								public plugin::Tail
	{
	public:
		static constexpr size_t g_maxTransportSegments = 16;

		BuiltinPlugin(const BuiltinInfo& info, plugin::Host& host, uint32_t channels);

		const plugin::Descriptor& descriptor() const override { return m_descriptor; }
		bool init() override;
		bool activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames) override;
		void deactivate() override {}
		bool startProcessing() noexcept override { return true; }
		void stopProcessing() noexcept override {}
		void reset() noexcept override;
		plugin::ProcessStatus process(const plugin::Process& process) noexcept override;
		void* extension(std::string_view id) override;

		// AudioPorts
		uint32_t audioPortCount(bool input) const override;
		bool audioPortInfo(bool input, uint32_t index, plugin::AudioPortInfo& info) const override;

		// Params
		uint32_t paramCount() const override;
		bool paramInfo(uint32_t index, plugin::ParamInfo& info) const override;
		bool paramValue(uint32_t param, double& value) const override;
		bool paramValueToText(uint32_t param, double value, std::string& text) const override;
		bool paramTextToValue(uint32_t param, std::string_view text, double& value) const override;
		void flushParams(const plugin::EventList& in, plugin::EventList& out) noexcept override;

		// State: the parameter values, as pairs of id and value.
		bool saveState(std::vector<uint8_t>& data, plugin::StateContext context) override;
		bool loadState(const uint8_t* data, size_t size, plugin::StateContext context) override;

		// Latency and Tail, from activate on.
		uint32_t latency() const override;
		uint32_t tail() const noexcept override;

	private:
		int indexOf(uint32_t param) const;
		void setValue(uint32_t param, double value) noexcept;
		void applyPending() noexcept;
		void addParameter(uint32_t offset, uint32_t param, double value) noexcept;
		void translate(const plugin::Event& event) noexcept;
		graph::TransportBlock convert(const plugin::Process& process) noexcept;

		const BuiltinInfo& m_info;
		plugin::Host& m_host;
		uint32_t m_channels;
		uint32_t m_maxFrames = 0; // from activate
		plugin::Descriptor m_descriptor;
		std::shared_ptr<graph::Processor> m_processor;
		std::vector<std::atomic<double>> m_values; // written while processing, read by paramValue on the main thread
		std::vector<std::atomic<bool>> m_pending;  // values the processor hasn't got yet
		graph::EventBuffer m_events{graph::g_maxEventsPerBlock};
		graph::EventBuffer m_eventsOut{graph::g_maxEventsPerBlock};
		std::vector<graph::TransportSegment> m_segments;
	};

	// The built-in processors as a plugin factory: ids "builtin:<processor id>", ports with the given channels.
	class BuiltinFactory final : public plugin::Factory
	{
	public:
		explicit BuiltinFactory(uint32_t channels = 2);

		size_t count() const override { return m_descriptors.size(); }
		const plugin::Descriptor& descriptor(size_t index) const override { return m_descriptors[index]; }
		std::unique_ptr<plugin::Plugin> create(std::string_view id, plugin::Host& host) override;

	private:
		uint32_t m_channels;
		std::vector<plugin::Descriptor> m_descriptors;
	};
}
