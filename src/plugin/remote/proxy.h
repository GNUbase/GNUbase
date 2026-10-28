// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ipc/connection.h"
#include "plugin/extensions.h"
#include "plugin/plugin.h"
#include "plugin/remote/hostconnection.h"
#include "plugin/remote/protocol.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gnubase::plugin::remote
{
	// How long the proxy waits for its host process.
	struct Deadlines
	{
		std::chrono::nanoseconds realtime{std::chrono::milliseconds(100)}; // per block, for realtime and prefetch
		std::chrono::nanoseconds offline{std::chrono::seconds(10)};        // per block, for offline blocks
		std::chrono::milliseconds control{std::chrono::seconds(10)};       // per main-thread call
	};

	// Why a remote plugin stopped working. It is bypassed from then on: process() outputs silence and returns
	// ProcessStatus::Error without contacting its host. Recovering it is #65.
	enum class Failure : uint8_t
	{
		None,
		Deadline, // a block was not answered in time: the host hangs or crashed
		HostGone, // the host process exited or closed the connection
		Protocol, // the host broke the protocol
	};

	const char* toString(Failure failure);

	// The engine's side of a plugin that runs in a plugin host process (docs/plugin-api.md, "Split across the engine
	// and the host process"). It implements plugin::Plugin by sending each call to its instance in the host:
	// main-thread calls as requests over the host's control connection, blocks over an audio connection of its own,
	// created when it is activated. It caches what the host reports (ports, parameter infos, latency, tail) and mirrors
	// every parameter value, so reading one never waits for the host.
	//
	// Extensions: AudioPorts, Params, State, Latency and Tail, for the ids the remote plugin has. The others come with
	// their tickets (#69 editors, #70 parameters, #72 presets).
	class RemotePlugin final : public Plugin,
							   public AudioPorts,
							   public Params,
							   public State,
							   public Latency,
							   public Tail
	{
	public:
		// The plugin `id` in the file `path` (empty for plugins built into the host), to create in `host` on init().
		RemotePlugin(std::string id, std::string path, std::shared_ptr<HostConnection> host, Deadlines deadlines);
		~RemotePlugin() override;
		RemotePlugin(const RemotePlugin&) = delete;
		RemotePlugin& operator=(const RemotePlugin&) = delete;

		// Plugin
		const Descriptor& descriptor() const override { return m_info.descriptor; }
		bool init() override;
		bool activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames) override;
		void deactivate() override;
		bool startProcessing() noexcept override;
		void stopProcessing() noexcept override;
		void reset() noexcept override;
		ProcessStatus process(const Process& process) noexcept override;
		void* extension(std::string_view id) override;

		// AudioPorts
		uint32_t audioPortCount(bool input) const override;
		bool audioPortInfo(bool input, uint32_t index, AudioPortInfo& info) const override;

		// Params
		uint32_t paramCount() const override { return static_cast<uint32_t>(m_info.params.size()); }
		bool paramInfo(uint32_t index, ParamInfo& info) const override;
		bool paramValue(uint32_t param, double& value) const override;
		bool paramValueToText(uint32_t param, double value, std::string& text) const override;
		bool paramTextToValue(uint32_t param, std::string_view text, double& value) const override;
		void flushParams(const EventList& in, EventList& out) noexcept override;

		// State
		bool saveState(std::vector<uint8_t>& data, StateContext context) override;
		bool loadState(const uint8_t* data, size_t size, StateContext context) override;

		// Latency, Tail
		uint32_t latency() const override { return m_info.latency; }
		uint32_t tail() const noexcept override { return m_info.tail; }

		// [main] The host process the instance runs in.
		const std::shared_ptr<HostConnection>& host() const { return m_binding->host; }
		const std::string& path() const { return m_path; }

		// [main] Moves the instance to another host process with its state, e.g. because the hosting mode changed: it
		// is created there, activated if it is active, gets the saved state, and takes over between two blocks, so it
		// may be processing meanwhile. The old instance is destroyed. On failure it stays where it is and `error`
		// says why.
		bool moveTo(std::shared_ptr<HostConnection> host, std::string& error);

		// [any]
		Failure failure() const noexcept { return m_failure.load(std::memory_order_acquire); }
		// [any] Bypasses the plugin for good; the first failure is kept.
		void fail(Failure failure) noexcept;

		// [main] Called when the proxy is destroyed, e.g. to unregister it.
		void onDestroy(std::function<void()> callback) { m_onDestroy = std::move(callback); }

	private:
		// One instance in a host process: where blocks go. Swapped as a whole when the instance moves.
		struct Binding
		{
			std::shared_ptr<HostConnection> host;
			uint32_t instance = 0;
			bool created = false;
			bool active = false;
			ipc::Connection audio; // while active
		};

		HostConnection::Reply call(Binding& binding, Message type, const std::vector<uint8_t>& body) const;
		bool create(Binding& binding, PluginInfo& info, std::string& error);
		bool activateIn(Binding& binding, PluginInfo& info, std::string& error);
		void release(Binding& binding);
		void publish(Binding* binding);
		void setInfo(PluginInfo info);
		void setValues(const std::vector<double>& values);
		void mirror(const Event& event) noexcept;
		bool has(std::string_view extension) const;
		ProcessStatus exchange(
			Binding& binding, const Process* process, uint32_t flags, const EventList& in, EventList& out) noexcept;
		bool sendEvents(ipc::Connection& audio, const EventList& events, uint32_t& count) noexcept;
		bool readReply(ipc::Connection& audio, const ipc::AudioBlock& reply, const Process* process, EventList* out,
			ProcessStatus& status) noexcept;

		std::string m_id;
		std::string m_path;
		Deadlines m_deadlines;
		PluginInfo m_info;
		uint32_t m_inputChannels = 0;
		uint32_t m_outputChannels = 0;
		double m_sampleRate = 0.0;
		uint32_t m_minFrames = 0;
		uint32_t m_maxFrames = 0;
		std::function<void()> m_onDestroy;

		// The parameter mirror: ids sorted, with their index in m_info.params; values written by the audio thread and
		// read anywhere.
		std::vector<std::pair<uint32_t, uint32_t>> m_paramIndex;
		std::unique_ptr<std::atomic<double>[]> m_values;

		// The binding the main thread owns, and the one the audio thread uses: m_busy is set while it does, so the
		// main thread knows when an old binding is free.
		std::unique_ptr<Binding> m_binding;
		std::atomic<Binding*> m_live{nullptr};
		std::atomic<bool> m_busy{false};

		// Audio thread.
		uint64_t m_sequence = 0;
		uint32_t m_flags = 0; // ProcessFlags for the next block
		std::atomic<bool> m_processing{false};
		std::vector<std::byte> m_message; // one audio message, sent or read back

		std::atomic<Failure> m_failure{Failure::None};
	};
}
