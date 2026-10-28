// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/graph/processor.h"
#include "plugin/events.h"
#include "plugin/extensions.h"
#include "plugin/plugin.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace gnubase::engine::plugins
{
	// The processor of a plugin slot node (graph::NodeKind::PluginSlot): hosts any plugin::Plugin, a built-in one or
	// a remote plugin::remote::RemotePlugin, so the graph doesn't know the difference (docs/plugin-api.md). It
	// activates the plugin when the graph is compiled, and per block passes the node's audio to the plugin's main
	// input port, its events as plugin events and the transport as transport segments, and the main output port back
	// as the node's audio. Other ports get silence. A block the plugin fails (ProcessStatus::Error, which a remote
	// plugin also returns once its host crashed or hung) outputs silence: the plugin is bypassed.
	class PluginSlot final : public graph::Processor
	{
	public:
		// The plugin is initialized.
		explicit PluginSlot(std::unique_ptr<plugin::Plugin> plugin);
		~PluginSlot() override;

		plugin::Plugin& plugin() const { return *m_plugin; }

		// How blocks are processed from now on; offline blocks get a longer deadline in a host process.
		void setProcessMode(plugin::ProcessMode mode) noexcept { m_mode.store(mode, std::memory_order_relaxed); }

		// [any] True while the plugin fails its blocks, so the slot outputs silence.
		bool failed() const noexcept { return m_failed.load(std::memory_order_relaxed); }

		void process(const graph::ProcessContext& context) noexcept override;
		uint32_t latency() const override;

	protected:
		void prepare(double sampleRate, uint32_t maxFrames) override;

	private:
		struct Port
		{
			std::vector<float> samples; // channels × maxFrames
			std::vector<float*> channels;
		};

		void allocate(bool input, uint32_t maxFrames);
		void convertEvents(const graph::ProcessContext& context) noexcept;
		uint32_t convertTransport(const graph::TransportBlock& transport) noexcept;

		std::unique_ptr<plugin::Plugin> m_plugin;
		plugin::AudioPorts* m_ports = nullptr;
		plugin::Latency* m_latency = nullptr;
		bool m_active = false;
		std::atomic<plugin::ProcessMode> m_mode{plugin::ProcessMode::Realtime};
		std::atomic<bool> m_failed{false};

		// Allocated in prepare, used by the audio thread.
		std::vector<Port> m_inputPorts;
		std::vector<Port> m_outputPorts;
		std::vector<plugin::AudioBuffer> m_inputs;
		std::vector<plugin::AudioBuffer> m_outputs;
		uint32_t m_mainInput = 0;
		uint32_t m_mainOutput = 0;
		plugin::EventList m_eventsIn;
		plugin::EventList m_eventsOut;
		std::vector<plugin::TransportSegment> m_transport;
		bool m_started = false;
		int64_t m_steadyTime = 0;
	};
}
