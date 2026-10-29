// SPDX-License-Identifier: GPL-3.0-or-later
#include "plugin/remote/proxy.h"

#include "platform/log.h"
#include "platform/rt/realtime.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <thread>

namespace gnubase::plugin::remote
{
	namespace
	{
		constexpr auto g_version = g_messageVersion;
		// The audio thread waits for an answer in slices this long, so a host that crashed or was marked hung
		// meanwhile fails the block at once instead of at its deadline.
		constexpr std::chrono::nanoseconds g_slice = std::chrono::milliseconds(1);

		uint32_t channelsOf(const std::vector<AudioPortInfo>& ports)
		{
			uint32_t channels = 0;
			for (const AudioPortInfo& port : ports)
				channels += port.channelCount;
			return channels;
		}

		std::vector<uint32_t> channelsPerPort(const std::vector<AudioPortInfo>& ports)
		{
			std::vector<uint32_t> channels;
			channels.reserve(ports.size());
			for (const AudioPortInfo& port : ports)
				channels.push_back(port.channelCount);
			return channels;
		}

		void silence(const Process& process) noexcept
		{
			for (uint32_t port = 0; port < process.audioOutputCount; ++port)
			{
				AudioBuffer& buffer = process.audioOutputs[port];
				for (uint32_t c = 0; c < buffer.channelCount; ++c)
					std::fill(buffer.channels[c], buffer.channels[c] + process.frames, 0.0f);
				buffer.constantMask = ~uint64_t{0};
			}
		}

		bool isCoreEvent(const Event& event)
		{
			return event.space == g_coreEventSpace;
		}

		bool isPerNote(const Event& event)
		{
			return event.noteId != -1 || event.key != -1;
		}

		bool sameParams(const std::vector<ParamInfo>& a, const std::vector<ParamInfo>& b)
		{
			return std::equal(a.begin(), a.end(), b.begin(), b.end(),
				[](const ParamInfo& x, const ParamInfo& y) { return x.id == y.id; });
		}

		// Clears the dirty marks of a move when it ends, however it ends.
		class Tracking
		{
		public:
			explicit Tracking(std::atomic<bool>& tracking) : m_tracking(tracking) { m_tracking = true; }
			~Tracking() { m_tracking = false; }
			Tracking(const Tracking&) = delete;
			Tracking& operator=(const Tracking&) = delete;
			Tracking(Tracking&&) = delete;
			Tracking& operator=(Tracking&&) = delete;

		private:
			std::atomic<bool>& m_tracking;
		};
	}

	std::chrono::nanoseconds Deadlines::forBlock(uint32_t frames, double sampleRate, bool offlineBlock) const noexcept
	{
		if (offlineBlock)
			return offline;
		if (sampleRate <= 0.0)
			return realtimeMaximum;
		const std::chrono::duration<double> period(static_cast<double>(frames) / sampleRate * realtimeBlocks);
		const auto deadline = std::chrono::duration_cast<std::chrono::nanoseconds>(period);
		return std::max(realtimeMinimum, std::min(deadline, realtimeMaximum));
	}

	const char* toString(Failure failure)
	{
		switch (failure)
		{
		case Failure::None:
			return "none";
		case Failure::Deadline:
			return "deadline";
		case Failure::HostGone:
			return "host gone";
		case Failure::Protocol:
			return "protocol";
		}
		return "unknown";
	}

	RemotePlugin::RemotePlugin(
		std::string id, std::string path, std::shared_ptr<HostConnection> host, Deadlines deadlines, Host* engineHost)
		: m_id(std::move(id)), m_path(std::move(path)), m_deadlines(deadlines), m_host(engineHost),
		  m_binding(std::make_shared<Binding>()), m_message(g_maxAudioMessage)
	{
		m_binding->host = std::move(host);
		m_descriptor.id = m_id;
		if (m_host != nullptr)
		{
			m_hostParams = plugin::extension<HostParams>(*m_host);
			m_hostAudioPorts = plugin::extension<HostAudioPorts>(*m_host);
			m_hostNotePorts = plugin::extension<HostNotePorts>(*m_host);
			m_hostState = plugin::extension<HostState>(*m_host);
			m_hostLatency = plugin::extension<HostLatency>(*m_host);
			m_hostTail = plugin::extension<HostTail>(*m_host);
		}
	}

	RemotePlugin::~RemotePlugin()
	{
		try
		{
			if (m_onDestroy)
				m_onDestroy();
		}
		catch (...) // NOLINT(bugprone-empty-catch): a destructor can't report it; only out of memory gets here
		{
			// The manager's entry is left behind.
		}
		const std::scoped_lock calls(m_callMutex);
		publish(nullptr);
		try
		{
			release(*binding());
		}
		catch (...) // NOLINT(bugprone-empty-catch): a destructor can't report it; only out of memory gets here
		{
			// The host process destroys the instance when it ends.
		}
	}

	std::shared_ptr<RemotePlugin::Binding> RemotePlugin::binding() const
	{
		const std::scoped_lock lock(m_mutex);
		return m_binding;
	}

	std::shared_ptr<HostConnection> RemotePlugin::host() const
	{
		return binding()->host;
	}

	HostConnection::Reply RemotePlugin::call(
		Binding& binding, Message type, const std::vector<uint8_t>& body, std::chrono::milliseconds timeout)
	{
		if (!binding.host)
			return {false, "no plugin host", {}, false};
		return binding.host->request(type, binding.instance, body, timeout);
	}

	bool RemotePlugin::create(Binding& binding, PluginInfo& info, std::string& error)
	{
		if (!binding.host)
		{
			error = "no plugin host";
			return false;
		}
		binding.instance = binding.host->newInstance();
		binding.host->listen(
			binding.instance, [this](Notification notification, uint32_t detail) { notified(notification, detail); });
		Writer writer;
		writer.string(m_id);
		writer.string(m_path);
		const HostConnection::Reply reply = call(binding, Message::Create, writer.data(), m_deadlines.job);
		if (!reply.ok)
		{
			// A Create that the host carries out after the engine gave up on it would leave an orphan behind.
			if (reply.late)
				binding.host->post(Message::Destroy, binding.instance, {}, m_deadlines.query);
			binding.host->listen(binding.instance, {});
			error = reply.error;
			return false;
		}
		binding.created = true;
		Reader reader(reply.body);
		if (!read(reader, info))
		{
			error = "the plugin host sent invalid plugin info";
			return false;
		}
		return true;
	}

	bool RemotePlugin::describeIn(Binding& binding, PluginInfo& info, std::string& error) const
	{
		const HostConnection::Reply reply = call(binding, Message::Describe, {}, m_deadlines.job);
		Reader reader(reply.body);
		if (!reply.ok || !read(reader, info))
		{
			error = reply.ok ? "the plugin host sent invalid plugin info" : reply.error;
			return false;
		}
		return true;
	}

	bool RemotePlugin::init()
	{
		rt::assertNotRealtime("RemotePlugin::init");
		const std::scoped_lock calls(m_callMutex);
		const std::shared_ptr<Binding> current = binding();
		PluginInfo info;
		std::string error;
		if (!create(*current, info, error))
		{
			logging::warning("Cannot create the plugin {}: {}", m_id, error);
			return false;
		}
		const std::scoped_lock lock(m_mutex);
		m_descriptor = info.descriptor;
		m_descriptor.id = m_id;
		m_info = info;
		m_tail = info.tail;
		setParams(info);
		return true;
	}

	bool RemotePlugin::setProcessMode(ProcessMode mode)
	{
		rt::assertNotRealtime("RemotePlugin::setProcessMode");
		const std::scoped_lock calls(m_callMutex);
		const std::shared_ptr<Binding> current = binding();
		if (current->active)
			return false;
		if (current->created)
		{
			Writer writer;
			writer.u8(static_cast<uint8_t>(mode));
			if (!call(*current, Message::SetProcessMode, writer.data(), m_deadlines.query).ok)
				return false;
		}
		m_mode = mode;
		return true;
	}

	bool RemotePlugin::activateIn(Binding& binding, PluginInfo& info, std::string& error)
	{
		// The ports as they are now: a state or the plugin itself may have changed them since they were cached.
		PluginInfo current;
		if (!describeIn(binding, current, error))
			return false;
		const uint32_t inputChannels = channelsOf(current.inputs);
		const uint32_t outputChannels = channelsOf(current.outputs);
		try
		{
			binding.audio = ipc::Connection::create(audioConfig(inputChannels, outputChannels, m_maxFrames));
		}
		catch (const std::exception& exception)
		{
			error = exception.what(); // e.g. no room for the shared memory
			return false;
		}
		Writer writer;
		writer.f64(m_sampleRate);
		writer.u32(m_minFrames);
		writer.u32(m_maxFrames);
		writer.string(binding.audio.name());
		const HostConnection::Reply reply = call(binding, Message::Activate, writer.data(), m_deadlines.job);
		Reader reader(reply.body);
		if (!reply.ok || !read(reader, info))
		{
			if (reply.late) // it may still activate: deactivate it again
				binding.host->post(Message::Deactivate, binding.instance, {}, m_deadlines.query);
			error = reply.ok ? "the plugin host sent invalid plugin info" : reply.error;
			binding.audio = ipc::Connection();
			return false;
		}
		binding.audio.waitForPeer(std::chrono::nanoseconds::zero()); // the host opened it: its name can go
		if (channelsOf(info.inputs) != inputChannels || channelsOf(info.outputs) != outputChannels)
		{
			call(binding, Message::Deactivate, {}, m_deadlines.job);
			binding.audio = ipc::Connection();
			error = "the plugin changed its ports while it was activated";
			return false;
		}
		binding.inputPorts = channelsPerPort(info.inputs);
		binding.outputPorts = channelsPerPort(info.outputs);
		binding.inputChannels = inputChannels;
		binding.outputChannels = outputChannels;
		binding.started = false;
		binding.active = true;
		return true;
	}

	bool RemotePlugin::activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames)
	{
		rt::assertNotRealtime("RemotePlugin::activate");
		const std::scoped_lock calls(m_callMutex);
		const std::shared_ptr<Binding> current = binding();
		if (!current->created || current->active || failure() != Failure::None)
			return false;
		m_sampleRate = sampleRate;
		m_minFrames = minFrames;
		m_maxFrames = maxFrames;
		PluginInfo info;
		std::string error;
		if (!activateIn(*current, info, error))
		{
			logging::warning("Cannot activate the plugin {}: {}", m_id, error);
			return false;
		}
		applyInfo(info, false);
		m_flags = 0;
		m_processing = false;
		publish(current.get());
		return true;
	}

	void RemotePlugin::deactivate()
	{
		rt::assertNotRealtime("RemotePlugin::deactivate");
		const std::scoped_lock calls(m_callMutex);
		const std::shared_ptr<Binding> current = binding();
		if (!current->active)
			return;
		publish(nullptr);
		m_processing = false;
		current->audio.close();
		if (failure() == Failure::None)
			call(*current, Message::Deactivate, {}, m_deadlines.job);
		current->audio = ipc::Connection();
		current->active = false;
		current->started = false;
	}

	void RemotePlugin::release(Binding& binding)
	{
		if (!binding.created)
			return;
		// A failed plugin's host is gone or hangs: it goes when its process ends.
		const bool reachable = failure() == Failure::None && !binding.host->exited();
		if (binding.active)
		{
			binding.audio.close();
			if (reachable)
				call(binding, Message::Deactivate, {}, m_deadlines.job);
			binding.audio = ipc::Connection();
			binding.active = false;
		}
		if (reachable)
			call(binding, Message::Destroy, {}, m_deadlines.job);
		binding.host->listen(binding.instance, {});
		binding.created = false;
	}

	void RemotePlugin::publish(Binding* binding)
	{
		m_live.store(binding);
		// The audio thread set m_busy before it read m_live: once it is clear, no block uses the old binding.
		while (m_busy.load())
			std::this_thread::sleep_for(std::chrono::microseconds(50));
	}

	void RemotePlugin::onNotification(std::function<void()> callback)
	{
		const std::scoped_lock lock(m_mutex);
		m_onNotification = std::move(callback);
	}

	void RemotePlugin::notified(Notification notification, uint32_t detail)
	{
		if (notification == Notification::RequestRestart)
		{
			if (m_host != nullptr)
				m_host->requestRestart(); // [any]
			return;
		}
		if (notification == Notification::RescanParams)
			m_paramRescan.fetch_or(detail, std::memory_order_acq_rel);
		else if (notification == Notification::RescanNotePorts)
			m_notePortRescan.fetch_or(detail, std::memory_order_acq_rel);
		m_notifications.fetch_or(uint32_t{1} << static_cast<uint32_t>(notification), std::memory_order_acq_rel);
		std::function<void()> callback;
		{
			const std::scoped_lock lock(m_mutex);
			callback = m_onNotification;
		}
		if (callback)
			callback();
	}

	bool RemotePlugin::refresh()
	{
		rt::assertNotRealtime("RemotePlugin::refresh");
		const std::scoped_lock calls(m_callMutex);
		const uint32_t bits = m_notifications.exchange(0, std::memory_order_acq_rel);
		const uint32_t paramRescan = m_paramRescan.exchange(0, std::memory_order_acq_rel);
		const uint32_t notePortRescan = m_notePortRescan.exchange(0, std::memory_order_acq_rel);
		const auto has = [bits](Notification notification)
		{ return (bits & (uint32_t{1} << static_cast<uint32_t>(notification))) != 0; };
		if (bits == 0)
			return true;
		bool ok = true;
		if ((has(Notification::RescanParams) || has(Notification::RescanAudioPorts) ||
				has(Notification::LatencyChanged) || has(Notification::TailChanged)) &&
			failure() == Failure::None)
		{
			const std::shared_ptr<Binding> current = binding();
			PluginInfo info;
			std::string error;
			ok = current->created && describeIn(*current, info, error);
			if (ok)
				applyInfo(info, false);
			else
				logging::warning("Cannot fetch what the plugin {} changed: {}", m_id, error);
		}
		// On to the engine.
		if (has(Notification::RescanParams) && m_hostParams != nullptr)
			m_hostParams->rescanParams(paramRescan);
		if (has(Notification::RescanAudioPorts) && m_hostAudioPorts != nullptr)
			m_hostAudioPorts->rescanAudioPorts();
		if (has(Notification::RescanNotePorts) && m_hostNotePorts != nullptr)
			m_hostNotePorts->rescanNotePorts(notePortRescan);
		if (has(Notification::LatencyChanged) && m_hostLatency != nullptr)
			m_hostLatency->latencyChanged();
		if (has(Notification::TailChanged) && m_hostTail != nullptr)
			m_hostTail->tailChanged();
		if (has(Notification::StateDirty) && m_hostState != nullptr)
			m_hostState->markStateDirty();
		return ok;
	}

	void RemotePlugin::setParams(const PluginInfo& info)
	{
		m_info.params = info.params;
		m_paramIndex.clear();
		for (uint32_t i = 0; i < m_info.params.size(); ++i)
			m_paramIndex.emplace_back(m_info.params[i].id, i);
		std::sort(m_paramIndex.begin(), m_paramIndex.end());
		m_values = std::make_unique<std::atomic<double>[]>(m_info.params.size());
		m_dirty = std::make_unique<std::atomic<bool>[]>(m_info.params.size());
		for (size_t i = 0; i < m_info.params.size(); ++i)
			m_values[i].store(i < info.values.size() ? info.values[i] : m_info.params[i].defaultValue);
	}

	void RemotePlugin::applyInfo(PluginInfo info, bool keepDirtyValues)
	{
		const std::scoped_lock lock(m_mutex);
		const bool active = m_binding->active;
		m_info.extensions = std::move(info.extensions);
		if (active)
		{
			m_info.latency = info.latency; // only valid while active
		}
		else
		{
			// Ports only change while inactive; an active plugin asks for a restart, and activate reads them again.
			m_info.inputs = std::move(info.inputs);
			m_info.outputs = std::move(info.outputs);
		}
		m_tail = info.tail;
		if (sameParams(m_info.params, info.params))
		{
			for (size_t i = 0; i < info.params.size(); ++i)
			{
				m_info.params[i] = info.params[i];
				// A move keeps the mirror's newer values: they reach the new instance with its first block.
				if (i < info.values.size() && !(keepDirtyValues && m_dirty[i].load()))
					m_values[i].store(info.values[i], std::memory_order_relaxed);
			}
		}
		else if (!active)
		{
			setParams(info);
		}
		else
		{
			logging::warning(
				"The plugin {} changed its parameters while active; ignored until it is deactivated", m_id);
		}
	}

	void RemotePlugin::mirror(const Event& event, bool markDirty) noexcept
	{
		if (!isCoreEvent(event) || event.type != EventType::ParamValue || isPerNote(event))
			return;
		const auto found =
			std::lower_bound(m_paramIndex.begin(), m_paramIndex.end(), std::pair<uint32_t, uint32_t>(event.param, 0));
		if (found == m_paramIndex.end() || found->first != event.param)
			return;
		m_values[found->second].store(event.value, std::memory_order_relaxed);
		if (markDirty || m_tracking.load(std::memory_order_relaxed))
			m_dirty[found->second].store(true, std::memory_order_relaxed);
	}

	bool RemotePlugin::has(std::string_view extension) const
	{
		const std::scoped_lock lock(m_mutex);
		return std::find(m_info.extensions.begin(), m_info.extensions.end(), extension) != m_info.extensions.end();
	}

	void* RemotePlugin::extension(std::string_view id)
	{
		if (!has(id))
			return nullptr;
		return provide<AudioPorts, Params, State, Latency, Tail>(this, id);
	}

	void RemotePlugin::fail(Failure failure) noexcept
	{
		Failure expected = Failure::None;
		m_failure.compare_exchange_strong(expected, failure, std::memory_order_acq_rel);
	}

	bool RemotePlugin::startProcessing() noexcept
	{
		if (failure() != Failure::None)
			return false;
		bool started = false;
		m_busy.store(true);
		if (Binding* live = m_live.load(); live != nullptr)
		{
			// Only a plugin that refused to start isn't started. If Start came too late, or the host couldn't take
			// it yet, the next block sends it again first; the host ignores a Start while it processes.
			bool refused = false;
			if (!live->started)
			{
				const Exchanged start = exchange(*live, nullptr, Start, m_noEvents, nullptr);
				live->started = start.status != ProcessStatus::Error;
				refused = start.sent && !start.late && !live->started;
			}
			started = !refused && failure() == Failure::None;
		}
		m_busy.store(false, std::memory_order_release);
		m_processing.store(started, std::memory_order_release);
		return started;
	}

	void RemotePlugin::stopProcessing() noexcept
	{
		m_busy.store(true);
		// A Start that came too late may still have started it.
		if (Binding* live = m_live.load();
			live != nullptr && (live->started || m_processing.load(std::memory_order_relaxed)))
		{
			if (failure() == Failure::None)
				exchange(*live, nullptr, Stop, m_noEvents, nullptr);
			live->started = false;
		}
		m_busy.store(false, std::memory_order_release);
		m_processing.store(false, std::memory_order_release);
	}

	void RemotePlugin::reset() noexcept
	{
		m_flags |= Reset;
	}

	ProcessStatus RemotePlugin::process(const Process& process) noexcept
	{
		ProcessStatus status = ProcessStatus::Error;
		if (failure() == Failure::None && m_processing.load(std::memory_order_relaxed))
		{
			m_busy.store(true);
			if (Binding* live = m_live.load(); live != nullptr)
			{
				// After a move, the new instance starts with its first block. A Start that isn't answered in time is
				// sent again with the next block, and this one is silence, so a callback waits one deadline at most.
				if (!live->started)
					live->started = exchange(*live, nullptr, Start, m_noEvents, nullptr).status != ProcessStatus::Error;
				if (live->started && process.frames <= m_maxFrames)
				{
					const Exchanged block = exchange(*live, &process, m_flags, process.eventsIn, &process.eventsOut);
					status = block.status;
					// A block the host never got lost its notes and audio: the next one resets the plugin, so no
					// note hangs.
					m_flags = block.sent ? 0 : m_flags | Reset;
				}
			}
			m_busy.store(false, std::memory_order_release);
		}
		if (status == ProcessStatus::Error)
			silence(process);
		return status;
	}

	void RemotePlugin::flushParams(const EventList& in, EventList& out) noexcept
	{
		if (failure() != Failure::None)
			return;
		if (m_processing.load(std::memory_order_acquire))
		{
			// [audio] A block without frames.
			m_busy.store(true);
			if (Binding* live = m_live.load(); live != nullptr && live->started)
				exchange(*live, nullptr, Flush, in, &out);
			m_busy.store(false, std::memory_order_release);
			return;
		}
		// [main]
		try
		{
			{
				const std::scoped_lock lock(m_mutex);
				for (const Event& event : in)
					mirror(event);
			}
			Writer writer;
			write(writer, in);
			const std::shared_ptr<Binding> current = binding();
			const HostConnection::Reply reply = call(*current, Message::FlushParams, writer.data(), m_deadlines.query);
			Reader reader(reply.body);
			const size_t before = out.size();
			if (reply.ok && read(reader, out))
			{
				const std::scoped_lock lock(m_mutex);
				for (size_t i = before; i < out.size(); ++i)
					mirror(out[i]);
			}
		}
		catch (const std::exception& error)
		{
			logging::error("Flushing the parameters of {} failed: {}", m_id, error.what());
		}
	}

	bool RemotePlugin::resync(Binding& binding, const EventList* in, uint32_t& count) noexcept
	{
		// The parameters that changed while the instance moved, at the block's start, with the mirror's values.
		if (!binding.resync.exchange(false, std::memory_order_acq_rel))
			return true;
		(void)in;
		for (const auto& [id, index] : m_paramIndex)
		{
			if (!m_dirty[index].exchange(false, std::memory_order_relaxed))
				continue;
			Event event;
			event.type = EventType::ParamValue;
			event.param = id;
			event.value = m_values[index].load(std::memory_order_relaxed);
			if (!binding.audio.sendEvent(static_cast<uint16_t>(AudioMessage::Event), g_version, &event, sizeof(event)))
				return false;
			++count;
		}
		return true;
	}

	bool RemotePlugin::flushResync(Binding& binding)
	{
		if (!binding.resync.exchange(false, std::memory_order_acq_rel))
			return true; // the audio thread sent them
		EventList events(m_paramIndex.size() + 1, 0, 0);
		{
			const std::scoped_lock lock(m_mutex);
			for (const auto& [id, index] : m_paramIndex)
			{
				if (!m_dirty[index].exchange(false, std::memory_order_relaxed))
					continue;
				Event event;
				event.type = EventType::ParamValue;
				event.param = id;
				event.value = m_values[index].load(std::memory_order_relaxed);
				events.push(event);
			}
		}
		if (events.empty())
			return true;
		Writer writer;
		write(writer, events);
		if (call(binding, Message::FlushParams, writer.data(), m_deadlines.query).ok)
			return true;
		// It started processing meanwhile: its first block takes them.
		const std::scoped_lock lock(m_mutex);
		for (const Event& event : events)
		{
			const auto found = std::lower_bound(
				m_paramIndex.begin(), m_paramIndex.end(), std::pair<uint32_t, uint32_t>(event.param, 0));
			m_dirty[found->second].store(true, std::memory_order_relaxed);
		}
		binding.resync = true;
		return false;
	}

	bool RemotePlugin::sendEvents(ipc::Connection& audio, const EventList& events, uint32_t& count) noexcept
	{
		for (const Event& event : events)
		{
			mirror(event);
			if (!audio.sendEvent(static_cast<uint16_t>(AudioMessage::Event), g_version, &event, sizeof(event)))
				return false;
			++count;
			if (isCoreEvent(event) && event.type == EventType::Sysex)
			{
				if (!audio.sendEvent(
						static_cast<uint16_t>(AudioMessage::SysexBytes), g_version, events.bytes(event), event.data[1]))
					return false;
				++count;
			}
		}
		return true;
	}

	void RemotePlugin::missed(Binding& binding, std::chrono::steady_clock::time_point sent) noexcept
	{
		// A late block is silence for this plugin only. A plugin late for longer than Deadlines::hang hangs: no new
		// plugins go to its host, and the host's other plugins fail at their next block. A pause in processing that
		// long, e.g. while stopped, starts a new run of late blocks.
		if (!m_late || sent - m_lastSent >= m_deadlines.hang)
			m_lateSince = sent;
		m_late = true;
		m_lastSent = sent;
		if (std::chrono::steady_clock::now() - m_lateSince >= m_deadlines.hang)
		{
			fail(Failure::Deadline);
			binding.host->markHung();
		}
	}

	void RemotePlugin::keep(Binding& binding, const EventList& in) noexcept
	{
		// A block the host never got: its parameter changes go with the next block that it gets, with the mirror's
		// values, and its other events are lost.
		for (const Event& event : in)
			mirror(event, true);
		binding.resync.store(true, std::memory_order_release);
	}

	RemotePlugin::Exchanged RemotePlugin::exchange(
		Binding& binding, const Process* process, uint32_t flags, const EventList& in, EventList* out) noexcept
	{
		Exchanged result;
		// A host that is gone or hangs fails its plugins at once, without waiting for a deadline per plugin.
		if (binding.host->exited())
		{
			fail(Failure::HostGone);
			return result;
		}
		if (binding.host->hung())
		{
			fail(Failure::Deadline);
			return result;
		}
		const auto sentAt = std::chrono::steady_clock::now();
		ipc::Connection& audio = binding.audio;
		if (m_late)
		{
			// Answers that came too late are dropped before the next block goes out: the host can't answer a block
			// while the answer ring is full of them, and would drop it unprocessed.
			const ipc::WaitResult drained = audio.waitForAnswer(m_sequence + 1, std::chrono::nanoseconds::zero());
			if (drained == ipc::WaitResult::PeerClosed)
			{
				fail(Failure::HostGone);
				return result;
			}
			if (drained == ipc::WaitResult::Ready) // an answer to a block that was never sent
			{
				fail(Failure::Protocol);
				return result;
			}
		}
		ipc::AudioBlock block;
		if (!audio.beginBlock(block)) // the host still has the two blocks before: it is far behind, or hangs
		{
			keep(binding, in);
			missed(binding, sentAt);
			return result;
		}

		// The block's messages: its info with the inputs' constant masks, the transport, the events.
		ProcessInfo info;
		info.flags = flags;
		if (process != nullptr)
		{
			info.steadyTime = process->steadyTime;
			info.mode = static_cast<uint8_t>(process->mode);
			info.portCount = static_cast<uint8_t>(std::min<uint32_t>(process->audioInputCount, g_maxPorts));
			info.transportCount =
				static_cast<uint16_t>(std::min<uint32_t>(process->transportCount, g_maxTransportSegments));
		}
		// The parameters a move left behind go first; a block without frames then flushes them.
		const bool resyncing = binding.resync.load(std::memory_order_acquire);
		if (process == nullptr && resyncing)
			info.flags |= Flush;
		std::memcpy(m_message.data(), &info, sizeof(info));
		for (uint32_t port = 0; port < info.portCount; ++port)
			std::memcpy(m_message.data() + sizeof(info) + (port * sizeof(uint64_t)),
				&process->audioInputs[port].constantMask, sizeof(uint64_t));
		uint32_t count = 1;
		bool sent = audio.sendEvent(static_cast<uint16_t>(AudioMessage::ProcessInfo), g_version, m_message.data(),
			static_cast<uint32_t>(sizeof(info) + (info.portCount * sizeof(uint64_t))));
		for (uint32_t i = 0; sent && i < info.transportCount; ++i, ++count)
			sent = audio.sendEvent(static_cast<uint16_t>(AudioMessage::Transport), g_version, &process->transport[i],
				sizeof(TransportSegment));
		sent = sent && resync(binding, &in, count) && sendEvents(audio, in, count);
		if (!sent) // the host doesn't read its event ring
		{
			fail(Failure::Protocol);
			return result;
		}

		// The inputs, port after port, each with the channels the instance was activated with.
		const uint32_t frames = process != nullptr ? process->frames : 0;
		uint32_t channel = 0;
		for (uint32_t port = 0; port < binding.inputPorts.size(); ++port)
		{
			const AudioBuffer* buffer =
				process != nullptr && port < process->audioInputCount ? &process->audioInputs[port] : nullptr;
			for (uint32_t c = 0; c < binding.inputPorts[port]; ++c, ++channel)
			{
				float* samples = block.channel(channel);
				if (buffer != nullptr && c < buffer->channelCount)
					std::memcpy(samples, buffer->channels[c], frames * sizeof(float));
				else
					std::fill(samples, samples + frames, 0.0f);
			}
		}
		block.sequence = ++m_sequence;
		block.position =
			process != nullptr && process->transportCount > 0 ? process->transport[0].transport.position : 0;
		block.frames = frames;
		block.channels = binding.inputChannels;
		block.eventCount = count;
		audio.sendBlock(block);
		result.sent = true;

		// The answer, until the deadline: a number of block periods, or the offline deadline.
		const bool offline =
			m_mode == ProcessMode::Offline || (process != nullptr && process->mode == ProcessMode::Offline);
		const auto deadline =
			sentAt + m_deadlines.forBlock(process != nullptr ? frames : m_maxFrames, m_sampleRate, offline);
		for (;;)
		{
			const auto remaining =
				std::max(deadline - std::chrono::steady_clock::now(), std::chrono::steady_clock::duration::zero());
			const ipc::WaitResult wait =
				audio.waitForAnswer(block.sequence, std::min<std::chrono::nanoseconds>(remaining, g_slice));
			if (wait == ipc::WaitResult::Ready)
				break;
			if (wait == ipc::WaitResult::PeerClosed || binding.host->exited())
			{
				fail(Failure::HostGone);
				return result;
			}
			if (binding.host->hung())
			{
				fail(Failure::Deadline);
				return result;
			}
			if (remaining <= g_slice)
			{
				// Silence for this block. Its answer, when it comes, is dropped by the next block's wait, so it never
				// mixes into a later block; the output events it carries are lost.
				result.late = true;
				missed(binding, sentAt);
				return result;
			}
		}
		m_late = false;
		m_lastSent = sentAt;
		ipc::AudioBlock reply;
		if (audio.receiveBlock(reply) != ipc::ReadStatus::Ok)
		{
			fail(Failure::Protocol);
			return result;
		}
		const bool valid = readReply(binding, reply, process, out, result.status);
		audio.releaseBlock();
		if (!valid)
		{
			fail(Failure::Protocol);
			result.status = ProcessStatus::Error;
		}
		return result;
	}

	bool RemotePlugin::readReply(Binding& binding, const ipc::AudioBlock& reply, const Process* process, EventList* out,
		ProcessStatus& status) noexcept
	{
		status = ProcessStatus::Error;
		Event sysex;
		bool pendingSysex = false;
		for (uint32_t i = 0; i < reply.eventCount; ++i)
		{
			ipc::MessageHeader header;
			if (binding.audio.receiveEvent(header, m_message.data(), m_message.size()) != ipc::ReadStatus::Ok)
				return false;
			switch (static_cast<AudioMessage>(header.type))
			{
			case AudioMessage::Result:
			{
				if (header.size < sizeof(ProcessResult))
					return false;
				ProcessResult result;
				std::memcpy(&result, m_message.data(), sizeof(result));
				if (result.status <= static_cast<uint8_t>(ProcessStatus::Sleep))
					status = static_cast<ProcessStatus>(result.status);
				const uint32_t masks = std::min<uint32_t>(
					result.portCount, static_cast<uint32_t>((header.size - sizeof(result)) / sizeof(uint64_t)));
				for (uint32_t port = 0; process != nullptr && port < masks && port < process->audioOutputCount; ++port)
					std::memcpy(&process->audioOutputs[port].constantMask,
						m_message.data() + sizeof(result) + (port * sizeof(uint64_t)), sizeof(uint64_t));
				if (result.droppedEvents > 0)
					logging::warning("The plugin {} lost {} output events", m_id, result.droppedEvents);
				if ((result.flags & RequestProcess) != 0 && m_host != nullptr)
					m_host->requestProcess();
				if ((result.flags & RequestParamFlush) != 0 && m_hostParams != nullptr)
					m_hostParams->requestParamFlush();
				break;
			}
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
				{
					sysex = event;
				}
				else
				{
					mirror(event);
					if (out != nullptr)
						out->push(event);
				}
				break;
			}
			case AudioMessage::SysexBytes:
				if (pendingSysex && out != nullptr)
					out->pushSysex(
						sysex.offset, sysex.port, reinterpret_cast<const uint8_t*>(m_message.data()), header.size);
				pendingSysex = false;
				break;
			default: // from a newer minor version
				break;
			}
		}
		if (process == nullptr)
			return reply.frames == 0;

		// The outputs, port after port, as the inputs.
		if (reply.frames != process->frames || reply.channels != binding.outputChannels)
			return false;
		uint32_t channel = 0;
		for (uint32_t port = 0; port < binding.outputPorts.size(); ++port)
		{
			const AudioBuffer* buffer = port < process->audioOutputCount ? &process->audioOutputs[port] : nullptr;
			for (uint32_t c = 0; c < binding.outputPorts[port]; ++c, ++channel)
			{
				if (buffer != nullptr && c < buffer->channelCount)
					std::memcpy(buffer->channels[c], reply.channel(channel), process->frames * sizeof(float));
			}
			for (uint32_t c = binding.outputPorts[port]; buffer != nullptr && c < buffer->channelCount; ++c)
				std::fill(buffer->channels[c], buffer->channels[c] + process->frames, 0.0f);
		}
		return true;
	}

	uint32_t RemotePlugin::audioPortCount(bool input) const
	{
		const std::scoped_lock lock(m_mutex);
		return static_cast<uint32_t>(input ? m_info.inputs.size() : m_info.outputs.size());
	}

	bool RemotePlugin::audioPortInfo(bool input, uint32_t index, AudioPortInfo& info) const
	{
		const std::scoped_lock lock(m_mutex);
		const std::vector<AudioPortInfo>& ports = input ? m_info.inputs : m_info.outputs;
		if (index >= ports.size())
			return false;
		info = ports[index];
		return true;
	}

	uint32_t RemotePlugin::paramCount() const
	{
		const std::scoped_lock lock(m_mutex);
		return static_cast<uint32_t>(m_info.params.size());
	}

	bool RemotePlugin::paramInfo(uint32_t index, ParamInfo& info) const
	{
		const std::scoped_lock lock(m_mutex);
		if (index >= m_info.params.size())
			return false;
		info = m_info.params[index];
		return true;
	}

	bool RemotePlugin::paramValue(uint32_t param, double& value) const
	{
		const std::scoped_lock lock(m_mutex);
		const auto found =
			std::lower_bound(m_paramIndex.begin(), m_paramIndex.end(), std::pair<uint32_t, uint32_t>(param, 0));
		if (found == m_paramIndex.end() || found->first != param)
			return false;
		value = m_values[found->second].load(std::memory_order_relaxed);
		return true;
	}

	uint32_t RemotePlugin::latency() const
	{
		const std::scoped_lock lock(m_mutex);
		return m_info.latency;
	}

	bool RemotePlugin::paramValueToText(uint32_t param, double value, std::string& text) const
	{
		Writer writer;
		writer.u32(param);
		writer.f64(value);
		const HostConnection::Reply reply = call(*binding(), Message::ParamToText, writer.data(), m_deadlines.query);
		Reader reader(reply.body);
		if (!reply.ok)
			return false;
		text = reader.string();
		return reader.ok();
	}

	bool RemotePlugin::paramTextToValue(uint32_t param, std::string_view text, double& value) const
	{
		Writer writer;
		writer.u32(param);
		writer.string(text);
		const HostConnection::Reply reply = call(*binding(), Message::TextToParam, writer.data(), m_deadlines.query);
		Reader reader(reply.body);
		if (!reply.ok)
			return false;
		value = reader.f64();
		return reader.ok();
	}

	bool RemotePlugin::saveState(std::vector<uint8_t>& data, StateContext context)
	{
		rt::assertNotRealtime("RemotePlugin::saveState");
		const std::scoped_lock calls(m_callMutex);
		Writer writer;
		writer.u8(static_cast<uint8_t>(context));
		const HostConnection::Reply reply = call(*binding(), Message::SaveState, writer.data(), m_deadlines.job);
		if (!reply.ok)
		{
			logging::warning("Cannot save the state of {}: {}", m_id, reply.error);
			return false;
		}
		Reader reader(reply.body);
		data = reader.bytes();
		return reader.ok();
	}

	bool RemotePlugin::loadIn(Binding& binding, const std::vector<uint8_t>& state, PluginInfo& info, std::string& error)
	{
		Writer writer;
		writer.u8(static_cast<uint8_t>(StateContext::Project));
		writer.bytes(state.data(), state.size());
		const HostConnection::Reply reply = call(binding, Message::LoadState, writer.data(), m_deadlines.job);
		Reader reader(reply.body);
		if (!reply.ok || !read(reader, info))
		{
			error = reply.ok ? "the plugin host sent invalid plugin info" : "cannot load the state: " + reply.error;
			return false;
		}
		return true;
	}

	bool RemotePlugin::loadState(const uint8_t* data, size_t size, StateContext context)
	{
		rt::assertNotRealtime("RemotePlugin::loadState");
		const std::scoped_lock calls(m_callMutex);
		Writer writer;
		writer.u8(static_cast<uint8_t>(context));
		writer.bytes(data, size);
		const HostConnection::Reply reply = call(*binding(), Message::LoadState, writer.data(), m_deadlines.job);
		PluginInfo info;
		Reader reader(reply.body);
		if (!reply.ok || !read(reader, info))
		{
			logging::warning("Cannot load the state of {}: {}", m_id, reply.ok ? "invalid plugin info" : reply.error);
			return false;
		}
		applyInfo(std::move(info), false);
		return true;
	}

	bool RemotePlugin::moveTo(std::shared_ptr<HostConnection> host, std::string& error)
	{
		rt::assertNotRealtime("RemotePlugin::moveTo");
		const std::scoped_lock calls(m_callMutex);
		if (failure() != Failure::None)
		{
			error = std::string("the plugin failed: ") + toString(failure());
			return false;
		}
		const std::shared_ptr<Binding> old = binding();
		if (host == old->host)
			return true;
		if (!old->created)
		{
			const std::scoped_lock lock(m_mutex);
			old->host = std::move(host);
			return true;
		}

		// From the state's saving on, every parameter that changes is marked, so the new instance gets it.
		{
			const std::scoped_lock lock(m_mutex);
			for (size_t i = 0; i < m_paramIndex.size(); ++i)
				m_dirty[i] = false;
		}
		const Tracking tracking(m_tracking);
		std::vector<uint8_t> state;
		const bool hasState = has(State::extensionId());
		if (hasState)
		{
			Writer writer;
			writer.u8(static_cast<uint8_t>(StateContext::Project));
			const HostConnection::Reply reply = call(*old, Message::SaveState, writer.data(), m_deadlines.job);
			Reader reader(reply.body);
			if (reply.ok)
				state = reader.bytes();
			if (!reply.ok || !reader.ok())
			{
				error = "cannot save the plugin's state: " + (reply.ok ? std::string("invalid reply") : reply.error);
				return false;
			}
		}
		auto fresh = std::make_shared<Binding>();
		fresh->host = std::move(host);
		PluginInfo info;
		bool ok = create(*fresh, info, error);
		if (ok && hasState)
			ok = loadIn(*fresh, state, info, error);
		if (ok)
		{
			const std::scoped_lock lock(m_mutex);
			if (!sameParams(info.params, m_info.params) || channelsOf(info.inputs) != channelsOf(m_info.inputs) ||
				channelsOf(info.outputs) != channelsOf(m_info.outputs))
			{
				error = "the plugin has other ports or parameters in the other host";
				ok = false;
			}
		}
		if (ok && m_mode != ProcessMode::Realtime)
		{
			Writer writer;
			writer.u8(static_cast<uint8_t>(m_mode));
			const HostConnection::Reply reply = call(*fresh, Message::SetProcessMode, writer.data(), m_deadlines.job);
			ok = reply.ok;
			if (!ok)
				error = "cannot set the process mode: " + reply.error;
		}
		PluginInfo activeInfo;
		if (ok && old->active)
		{
			ok = activateIn(*fresh, activeInfo, error) && activeInfo.inputs.size() == old->inputPorts.size() &&
				activeInfo.outputs.size() == old->outputPorts.size();
			if (!ok && error.empty())
				error = "the plugin has other ports in the other host";
		}
		if (!ok)
		{
			release(*fresh);
			return false;
		}

		// Take over between two blocks. The parameters that changed since the state was saved go with the new
		// instance's first block, or below if it doesn't process.
		fresh->resync = true;
		{
			const std::scoped_lock lock(m_mutex);
			m_binding = fresh;
		}
		if (old->active)
			publish(fresh.get());
		// An active instance reports its latency only once activated.
		applyInfo(old->active ? std::move(activeInfo) : std::move(info), true);
		if (!m_processing.load(std::memory_order_acquire))
			flushResync(*fresh);
		release(*old);
		return true;
	}
}
