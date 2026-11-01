// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/graph/processor.h"
#include "plugin/events.h"
#include "plugin/extensions.h"
#include "plugin/plugin.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace gnubase::engine::plugins
{
	// Appends a graph event to a plugin's events, at the offset clamped to the block of `frames`: notes and parameter
	// changes as plugin events, the other MIDI messages as MIDI 1.0 bytes (a program change after its bank select)
	// and SysEx with its bytes copied. Real-time safe.
	void convertEvent(const graph::ProcessEvent& event, uint32_t frames, plugin::EventList& out) noexcept;

	// The processor of a plugin slot node (graph::NodeKind::PluginSlot): hosts any plugin::Plugin, a built-in one or
	// a remote plugin::remote::RemotePlugin, so the graph doesn't know the difference (docs/plugin-api.md). The engine
	// activates the plugin in a job before a graph with the slot is compiled; otherwise compiling the graph does. Per
	// block it passes the node's audio to the plugin's main input port, its events as plugin events and the transport
	// as transport segments, and the main output port back as the node's audio. Other ports get silence. A block the
	// plugin fails (ProcessStatus::Error, which a remote plugin also returns once its host crashed or hung) outputs
	// silence. A bypassed slot processes the plugin and outputs its input.
	//
	// Threads: [main] calls are the plugin's main thread, the plugin job thread in the engine; they may only happen
	// while no graph processes the slot.
	class PluginSlot final : public graph::Processor
	{
	public:
		// The plugin is initialized.
		explicit PluginSlot(std::unique_ptr<plugin::Plugin> plugin);
		~PluginSlot() override;

		plugin::Plugin& plugin() const { return *m_plugin; }

		// [main] Activates the plugin for blocks of up to maxFrames at the sample rate, unless it is active for them
		// already. Returns whether it is active.
		bool activate(double sampleRate, uint32_t maxFrames);
		// [main] Deactivates the plugin; then the slot outputs silence.
		void deactivate();
		// [main] Deactivates and destroys the plugin, e.g. a remote one's instance in its host process.
		void destroy();
		// [any] Whether the plugin is active for blocks of up to maxFrames at the sample rate.
		bool activeFor(double sampleRate, uint32_t maxFrames) const;

		// [main] Loads a state the plugin saved, e.g. from a project file, before it is activated.
		bool loadState(const std::vector<uint8_t>& state);

		// [command thread] The values for the plugin's parameters, by parameter id: from the next block on, those
		// that changed since the last call reach the plugin as parameter changes at the block's start. A parameter
		// that has no value any more goes back to its default; others keep what the plugin has, e.g. from its state.
		// Values of parameters the plugin doesn't have are ignored.
		void setParameters(const std::map<uint32_t, double>& values);

		// [any] From the next block on, the slot outputs its input while the plugin keeps processing, and reports no
		// latency once the graph is compiled again.
		void setBypass(bool bypass) noexcept { m_bypass.store(bypass, std::memory_order_relaxed); }
		bool bypassed() const noexcept { return m_bypass.load(std::memory_order_relaxed); }

		// How blocks are processed: from now on for Process::mode, and for the plugin's own setProcessMode() from the
		// next prepare on, which activates it again. Offline blocks get a longer deadline in a host process.
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
		void addParameterChanges() noexcept;
		void convertEvents(const graph::ProcessContext& context) noexcept;
		uint32_t convertTransport(const graph::TransportBlock& transport) noexcept;

		std::unique_ptr<plugin::Plugin> m_plugin;
		plugin::AudioPorts* m_ports = nullptr;
		plugin::Latency* m_latency = nullptr;
		bool m_active = false;
		std::atomic<double> m_activeRate{0.0}; // what it is active for
		std::atomic<uint32_t> m_activeFrames{0};
		std::atomic<bool> m_bypass{false};

		// Parameter values from setParameters, by the plugin's parameter index.
		std::vector<uint32_t> m_parameterIds;
		std::vector<double> m_defaults;
		std::vector<std::optional<double>> m_set; // command thread: what was set last
		std::unique_ptr<std::atomic<double>[]> m_targets;
		std::unique_ptr<std::atomic<bool>[]> m_changed;
		std::atomic<uint64_t> m_changes{0}; // counts setParameters calls that changed a value
		uint64_t m_seenChanges = 0;         // audio thread
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
		std::vector<float> m_dry; // the input of a bypassed block: g_maxAudioChannels × maxFrames
		uint32_t m_maxFrames = 0;
	};
}
