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
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace gnubase::plugin::remote
{
	// How long the proxy waits for its host process.
	struct Deadlines
	{
		// Per block in real time and prefetch: this many block periods, within the bounds. A host that misses it
		// already made the device miss its own deadline; the watchdog (#65) tunes these. A block that misses its
		// deadline is silence for that plugin and that block only.
		double realtimeBlocks = 2.0;
		std::chrono::nanoseconds realtimeMinimum{std::chrono::milliseconds(2)};
		std::chrono::nanoseconds realtimeMaximum{std::chrono::milliseconds(100)};
		std::chrono::nanoseconds offline{std::chrono::seconds(10)}; // per offline block
		// A plugin that answers no block in time for this long, counted from the first late block's sending, hangs:
		// it fails, and so does its host. A shorter run of late blocks is only silence.
		std::chrono::nanoseconds hang{std::chrono::milliseconds(250)};
		// Quick main-thread calls, which the command thread may make: value texts, flushing parameters, process mode.
		std::chrono::milliseconds query{std::chrono::seconds(2)};
		// Slow main-thread calls, which run as jobs (engine::plugins::HostManager): create, activate, deactivate,
		// states, rescans, destroy.
		std::chrono::milliseconds job{std::chrono::seconds(30)};

		// The deadline of a block of `frames` frames.
		std::chrono::nanoseconds forBlock(uint32_t frames, double sampleRate, bool offline) const noexcept;
	};

	// Why a remote plugin stopped working. It is bypassed from then on: process() outputs silence and returns
	// ProcessStatus::Error without contacting its host. Recovering it is #65.
	enum class Failure : uint8_t
	{
		None,
		Deadline, // no block was answered in time for Deadlines::hang, or its host hangs: another plugin of it did so
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
	// Threads: the audio-thread calls are real-time safe. The main-thread calls may come from several threads but
	// never hold a lock while they wait for the host: quick ones (value texts, flushing parameters) from the command
	// thread, slow ones (init, activate, deactivate, states, refresh, moveTo) from a job thread, one at a time.
	//
	// What the plugin asks of the engine comes from its host as notifications on the plugin control thread: a restart
	// request goes to the engine's plugin::Host at once, everything else waits for refresh(), which fetches what
	// changed and passes it on to the engine's host extensions.
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
		// The plugin `id` in the file `path`, to create in `host` on init(). `engineHost` is what the plugin asks of
		// the engine, or nullptr; it must outlive the proxy.
		RemotePlugin(std::string id, std::string path, std::shared_ptr<HostConnection> host, Deadlines deadlines,
			Host* engineHost = nullptr);
		~RemotePlugin() override;
		RemotePlugin(const RemotePlugin&) = delete;
		RemotePlugin& operator=(const RemotePlugin&) = delete;
		RemotePlugin(RemotePlugin&&) = delete;
		RemotePlugin& operator=(RemotePlugin&&) = delete;

		// Plugin
		const Descriptor& descriptor() const override { return m_descriptor; }
		bool init() override;
		bool setProcessMode(ProcessMode mode) override;
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
		uint32_t paramCount() const override;
		bool paramInfo(uint32_t index, ParamInfo& info) const override;
		bool paramValue(uint32_t param, double& value) const override;
		bool paramValueToText(uint32_t param, double value, std::string& text) const override;
		bool paramTextToValue(uint32_t param, std::string_view text, double& value) const override;
		void flushParams(const EventList& in, EventList& out) noexcept override;

		// State
		bool saveState(std::vector<uint8_t>& data, StateContext context) override;
		bool loadState(const uint8_t* data, size_t size, StateContext context) override;

		// Latency, Tail
		uint32_t latency() const override;
		uint32_t tail() const noexcept override { return m_tail.load(std::memory_order_relaxed); }

		// [main] The host process the instance runs in.
		std::shared_ptr<HostConnection> host() const;
		const std::string& path() const { return m_path; }

		// [main] Moves the instance to another host process with its state, e.g. because the hosting mode changed: it
		// is created there, activated if it is active, gets the saved state, and takes over between two blocks, so it
		// may be processing meanwhile. Parameter changes that reach the old instance after its state was saved, in
		// events or from the plugin itself, are passed on to the new one with its first block, and the mirror keeps
		// them. The new instance starts without the old one's audio history, so delay lines and reverb tails start
		// again from silence. The old instance is destroyed. On failure it stays where it is and `error` says why.
		bool moveTo(std::shared_ptr<HostConnection> host, std::string& error);

		// [main] Fetches what the plugin's notifications said changed (parameters, ports, latency, tail, state) and
		// passes the notifications on to the engine's host extensions. Returns false if the host doesn't answer.
		bool refresh();

		// [main] Called on the plugin control thread when the plugin has notifications for refresh(), e.g. to run it
		// as a job. Must be quick and must not wait for the host.
		void onNotification(std::function<void()> callback);

		// [any]
		Failure failure() const noexcept { return m_failure.load(std::memory_order_acquire); }
		// [any] Bypasses the plugin for good; the first failure is kept.
		void fail(Failure failure) noexcept;

		// [main] Called when the proxy is destroyed, before it releases its instance, e.g. to unregister it.
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
			// While active: channels per port, as the instance was activated with.
			std::vector<uint32_t> inputPorts;
			std::vector<uint32_t> outputPorts;
			uint32_t inputChannels = 0;
			uint32_t outputChannels = 0;
			bool started = false;            // audio thread: the instance processes (Start sent)
			std::atomic<bool> resync{false}; // the dirty parameters still go to it (moveTo)
		};

		std::shared_ptr<Binding> binding() const;
		static HostConnection::Reply call(
			Binding& binding, Message type, const std::vector<uint8_t>& body, std::chrono::milliseconds timeout);
		bool create(Binding& binding, PluginInfo& info, std::string& error);
		bool describeIn(Binding& binding, PluginInfo& info, std::string& error) const;
		bool activateIn(Binding& binding, PluginInfo& info, std::string& error);
		bool loadIn(Binding& binding, const std::vector<uint8_t>& state, PluginInfo& info, std::string& error);
		void release(Binding& binding);
		void publish(Binding* binding);
		void notified(Notification notification, uint32_t detail);
		void applyInfo(PluginInfo info, bool keepValues);
		void setParams(const PluginInfo& info);
		void mirror(const Event& event, bool markDirty = false) noexcept;
		bool has(std::string_view extension) const;
		bool resync(Binding& binding, const EventList* in, uint32_t& count) noexcept;
		bool flushResync(Binding& binding);
		// What became of a block: the plugin's status, Error unless it was answered in time; whether it went to the
		// host, and whether it went but its answer came too late, which the next block's wait drops.
		struct Exchanged
		{
			ProcessStatus status = ProcessStatus::Error;
			bool sent = false;
			bool late = false;
		};
		Exchanged exchange(
			Binding& binding, const Process* process, uint32_t flags, const EventList& in, EventList* out) noexcept;
		void missed(Binding& binding, std::chrono::steady_clock::time_point sent) noexcept;
		void keep(Binding& binding, const EventList& in) noexcept;
		bool sendEvents(ipc::Connection& audio, const EventList& events, uint32_t& count) noexcept;
		bool readReply(Binding& binding, const ipc::AudioBlock& reply, const Process* process, EventList* out,
			ProcessStatus& status) noexcept;

		const std::string m_id;
		const std::string m_path;
		const Deadlines m_deadlines;
		Host* const m_host;
		// The engine's host extensions, looked up once.
		HostParams* m_hostParams = nullptr;
		HostAudioPorts* m_hostAudioPorts = nullptr;
		HostNotePorts* m_hostNotePorts = nullptr;
		HostState* m_hostState = nullptr;
		HostLatency* m_hostLatency = nullptr;
		HostTail* m_hostTail = nullptr;
		std::function<void()> m_onDestroy;

		// Serializes the slow main-thread calls, which are never meant to overlap.
		std::mutex m_callMutex;
		// Guards the binding pointer, the cached info and the parameter index; never held while waiting for the host.
		mutable std::mutex m_mutex;
		std::shared_ptr<Binding> m_binding;
		Descriptor m_descriptor;
		PluginInfo m_info;
		double m_sampleRate = 0.0;
		uint32_t m_minFrames = 0;
		uint32_t m_maxFrames = 0;
		ProcessMode m_mode = ProcessMode::Realtime;
		std::atomic<uint32_t> m_tail{0};
		std::function<void()> m_onNotification;

		// The parameter mirror: ids sorted, with their index in m_info.params; values written by the audio thread and
		// read anywhere. While a move runs, every parameter that changes is marked dirty.
		std::vector<std::pair<uint32_t, uint32_t>> m_paramIndex;
		std::unique_ptr<std::atomic<double>[]> m_values;
		std::unique_ptr<std::atomic<bool>[]> m_dirty;
		std::atomic<bool> m_tracking{false};

		// Notifications that wait for refresh(): a bit per Notification, and the rescans' flags.
		std::atomic<uint32_t> m_notifications{0};
		std::atomic<uint32_t> m_paramRescan{0};
		std::atomic<uint32_t> m_notePortRescan{0};

		// The binding the audio thread uses: m_busy is set while it does, so the main thread knows when an old
		// binding is free.
		std::atomic<Binding*> m_live{nullptr};
		std::atomic<bool> m_busy{false};

		// Audio thread.
		uint64_t m_sequence = 0;
		uint32_t m_flags = 0; // ProcessFlags for the next block: Reset
		bool m_late = false;  // the last block was late, or never sent: late answers may wait
		std::chrono::steady_clock::time_point m_lateSince; // when the first block of that run of late ones was sent
		std::chrono::steady_clock::time_point m_lastSent;  // when the last block was sent
		std::atomic<bool> m_processing{false};             // between startProcessing and stopProcessing
		std::vector<std::byte> m_message;                  // one audio message, sent or read back
		const EventList m_noEvents;                        // the events of blocks without frames

		std::atomic<Failure> m_failure{Failure::None};
	};
}
