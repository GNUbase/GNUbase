// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ipc/connection.h"
#include "plugin/extensions.h"
#include "plugin/plugin.h"
#include "plugin/remote/protocol.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace gnubase::plugin::remote
{
	// The host process's side of one plugin instance (docs/plugin-api.md, "Split across the engine and the host
	// process"): it drives the plugin, which a format adapter created, from the engine's messages. Main-thread
	// requests come from the host process's control loop (handle); blocks arrive on the instance's audio connection
	// and are processed on a real-time thread of the stub's own while the plugin is active.
	//
	// It is also the plugin's plugin::Host with the host extensions HostAudioPorts, HostNotePorts, HostParams,
	// HostState, HostLatency and HostTail, and passes what the plugin asks of the engine on: as notifications (idle)
	// or as flags of the next block's result. Nothing from the engine is trusted: requests and blocks are checked
	// against what the plugin was activated with before the plugin sees them.
	class PluginStub final : public Host,
							 public HostAudioPorts,
							 public HostNotePorts,
							 public HostParams,
							 public HostState,
							 public HostLatency,
							 public HostTail
	{
	public:
		PluginStub() = default;
		~PluginStub() override;
		PluginStub(const PluginStub&) = delete;
		PluginStub& operator=(const PluginStub&) = delete;
		PluginStub(PluginStub&&) = delete;
		PluginStub& operator=(PluginStub&&) = delete;

		// [main] The plugin, created with this stub as its host and initialized.
		void setPlugin(std::unique_ptr<Plugin> plugin) { m_plugin = std::move(plugin); }
		Plugin* plugin() const { return m_plugin.get(); }

		// [main] Handles a request for this instance: Activate, Deactivate, SaveState, LoadState, ParamToText,
		// TextToParam, FlushParams, Describe and SetProcessMode. Writes what it returns into `reply`, or returns false
		// with the reason.
		bool handle(Message type, Reader& request, Writer& reply, std::string& error);

		// [main] Calls Plugin::onMainThread if the plugin asked for it, and returns what the plugin asked of the engine
		// since the last call, each with its detail.
		std::vector<std::pair<Notification, uint32_t>> idle();

		// [main] Ends the audio thread. False if it doesn't end in time: the plugin hangs in process(), and the stub
		// must not be destroyed. The host process then exits without destroying it.
		bool stopAudio(std::chrono::milliseconds timeout);

		// Host
		void requestRestart() override { notify(Notification::RequestRestart); }
		void requestProcess() override { m_resultFlags.fetch_or(RequestProcess, std::memory_order_relaxed); }
		void requestCallback() override { m_callback.store(true, std::memory_order_release); }
		void* extension(std::string_view id) override
		{
			return provide<HostAudioPorts, HostNotePorts, HostParams, HostState, HostLatency, HostTail>(this, id);
		}

		// Host extensions
		void rescanAudioPorts() override { notify(Notification::RescanAudioPorts); }
		void rescanNotePorts(uint32_t what) override { notify(Notification::RescanNotePorts, what); }
		void rescanParams(uint32_t what) override { notify(Notification::RescanParams, what); }
		void requestParamFlush() override { m_resultFlags.fetch_or(RequestParamFlush, std::memory_order_relaxed); }
		void markStateDirty() override { notify(Notification::StateDirty); }
		void latencyChanged() override { notify(Notification::LatencyChanged); }
		void tailChanged() override { notify(Notification::TailChanged); }

	private:
		void notify(Notification notification, uint32_t detail = 0) noexcept;
		bool activate(Reader& request, Writer& reply, std::string& error);
		void runAudio() noexcept;
		bool processBlock(const ipc::AudioBlock& input) noexcept;
		bool readBlock(const ipc::AudioBlock& input, ProcessInfo& info, uint32_t& segments) noexcept;
		bool validBlock(const ipc::AudioBlock& input, const ProcessInfo& info, uint32_t segments) const noexcept;

		std::unique_ptr<Plugin> m_plugin;
		bool m_active = false;
		uint32_t m_minFrames = 0;
		uint32_t m_maxFrames = 0;
		std::atomic<bool> m_callback{false};
		// What the plugin asked of the engine: a bit per Notification, and the rescans' flags.
		std::atomic<uint32_t> m_notifications{0};
		std::atomic<uint32_t> m_paramRescan{0};
		std::atomic<uint32_t> m_notePortRescan{0};
		std::atomic<uint32_t> m_resultFlags{0}; // ResultFlags for the next block's result

		// While active: the audio connection, the ports, and what the audio thread works with, allocated here.
		ipc::Connection m_audio;
		std::vector<uint32_t> m_inputPorts; // channels per port
		std::vector<uint32_t> m_outputPorts;
		uint32_t m_inputChannels = 0;
		uint32_t m_outputChannels = 0;
		std::vector<AudioBuffer> m_inputs;
		std::vector<AudioBuffer> m_outputs;
		std::vector<float*> m_inputPointers;
		std::vector<float*> m_outputPointers;
		EventList m_in{g_maxEvents, g_maxSysexBytes};
		EventList m_out{g_maxEvents, g_maxSysexBytes};
		std::array<TransportSegment, g_maxTransportSegments> m_transport{};
		std::vector<std::byte> m_message = std::vector<std::byte>(g_maxAudioMessage);
		// Set by the audio thread between startProcessing and stopProcessing; the main thread reads it to refuse
		// flushParams meanwhile.
		std::atomic<bool> m_processing{false};

		std::thread m_thread;
		std::atomic<bool> m_stop{false};
		std::mutex m_mutex;
		std::condition_variable m_stopped;
		bool m_done = true; // the audio thread has ended; guarded by m_mutex
	};
}
