// SPDX-License-Identifier: GPL-3.0-or-later
#include "plugin/remote/hostconnection.h"

#include "platform/log.h"
#include "platform/rt/realtime.h"

#include <algorithm>
#include <exception>
#include <optional>
#include <string>
#include <utility>

namespace gnubase::plugin::remote
{
	namespace
	{
		using namespace std::chrono_literals;

		constexpr auto g_poll = 5ms;
		constexpr auto g_exitWait = 2000ms;
		constexpr auto g_killWait = 2000ms;
	}

	std::shared_ptr<HostConnection> HostConnection::launch(const LaunchOptions& options)
	{
		rt::assertNotRealtime("HostConnection::launch");
		std::shared_ptr<HostConnection> host(new HostConnection());
		try
		{
			host->m_control = ipc::Connection::create(controlConfig());
			std::vector<std::string> arguments{"--connect", host->m_control.name()};
			if (!options.logFile.empty())
				arguments.insert(arguments.end(), {"--log", options.logFile});
			host->m_process = platform::ChildProcess::start(options.executable, arguments, options.errorOutput);
		}
		catch (const std::exception& error)
		{
			throw LaunchError(std::string("Cannot start the plugin host: ") + error.what());
		}
		host->m_processId = host->m_process.id();
		host->m_logFile = options.logFile;
		host->m_writer = std::make_unique<MessageWriter>(host->m_control);

		// Wait for the host to open the connection, unless it exits first, e.g. because its IPC version differs.
		const auto deadline = std::chrono::steady_clock::now() + options.timeout;
		while (!host->m_control.waitForPeer(20ms))
		{
			if (!host->m_process.running())
				throw LaunchError("The plugin host exited before it connected (exit code " +
					std::to_string(host->m_process.wait(0ms).value_or(-1)) + ")");
			if (std::chrono::steady_clock::now() >= deadline)
				throw LaunchError("The plugin host did not connect in time"); // the destructor kills it
		}
		host->m_thread = std::thread([raw = host.get()] { raw->run(); });

		Writer hello;
		hello.u32(options.protocolVersion);
		hello.u32(options.apiVersion);
		const Reply reply = host->request(Message::Hello, 0, hello.data(), options.timeout);
		if (!reply.ok)
			throw LaunchError("The plugin host refused the connection: " + reply.error);
		Reader reader(reply.body);
		const uint32_t protocol = reader.u32();
		const uint32_t api = reader.u32();
		if (!reader.ok() || protocol != g_protocolVersion || api != g_apiVersion)
			throw LaunchError("The plugin host speaks another protocol version");
		logging::info("Plugin host {} started", host->m_processId);
		return host;
	}

	HostConnection::~HostConnection()
	{
		m_stopping = true;
		m_control.close(); // the host exits when it sees this
		if (m_thread.joinable())
			m_thread.join();
		// The plugin control thread is gone: the process is this thread's now.
		if (!m_process.wait(g_exitWait))
			logging::warning("Plugin host {} did not exit; ending it", m_processId);
		// ChildProcess kills a process that still runs, waiting a bounded time.
	}

	void HostConnection::kill()
	{
		rt::assertNotRealtime("HostConnection::kill");
		m_killRequested = true;
		if (!m_thread.joinable())
			return;
		const auto deadline = std::chrono::steady_clock::now() + g_killWait;
		while (!exited() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(1ms);
	}

	void HostConnection::listen(uint32_t instance, Listener listener)
	{
		rt::assertNotRealtime("HostConnection::listen");
		const std::scoped_lock lock(m_dispatchMutex);
		if (listener)
			m_listeners[instance] = std::move(listener);
		else
			m_listeners.erase(instance);
	}

	std::chrono::steady_clock::duration HostConnection::silence() const noexcept
	{
		const int64_t since = m_silentSince.load(std::memory_order_acquire);
		if (since == 0)
			return std::chrono::steady_clock::duration::zero();
		return std::chrono::steady_clock::now().time_since_epoch() - std::chrono::steady_clock::duration(since);
	}

	bool HostConnection::send(Message type, uint32_t instance, uint32_t request, const std::vector<uint8_t>& body,
		std::chrono::steady_clock::time_point deadline, std::string& error)
	{
		ControlHeader header;
		header.request = request;
		header.instance = instance;
		// A large message, e.g. a state, may take its whole timeout to go out: a quick request waits for it only
		// until its own deadline.
		std::unique_lock lock(m_sendMutex, deadline);
		if (!lock.owns_lock())
		{
			error = "the plugin host is still busy receiving another request";
			return false;
		}
		const auto left =
			std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
		const SendResult result = m_writer->send(type, header, body, std::max(left, std::chrono::milliseconds(0)));
		lock.unlock();
		switch (result)
		{
		case SendResult::Sent:
			return true;
		case SendResult::TooLarge:
			error = "the request is too large: " + std::to_string(body.size()) + " bytes, at most " +
				std::to_string(m_writer->maxMessage() - sizeof(ControlHeader));
			return false;
		case SendResult::Timeout:
			error = "the plugin host does not read requests";
			return false;
		case SendResult::Closed:
			error = "the plugin host is gone";
			return false;
		}
		return false;
	}

	HostConnection::Reply HostConnection::request(
		Message type, uint32_t instance, const std::vector<uint8_t>& body, std::chrono::milliseconds timeout)
	{
		rt::assertNotRealtime("HostConnection::request");
		// One deadline for sending the request and for its reply.
		const auto sentAt = std::chrono::steady_clock::now();
		const auto deadline = sentAt + timeout;
		Pending pending;
		uint32_t id = 0;
		{
			const std::scoped_lock lock(m_mutex);
			if (exited())
				return {false, "the plugin host is gone", {}, false};
			id = m_nextRequest++;
			if (id == 0)
				id = m_nextRequest++;
			m_pending[id] = &pending;
		}
		std::string error;
		const bool sent = send(type, instance, id, body, deadline, error);

		std::unique_lock lock(m_mutex);
		if (sent)
			m_replied.wait_until(lock, deadline, [&] { return pending.done; });
		m_pending.erase(id);
		if (pending.done)
			return std::move(pending.reply);
		if (!sent)
			return {false, error, {}, false};
		// The host is silent from this request on, unless it was already, until it answers any request. A reply that
		// comes later finds no request and is dropped.
		int64_t none = 0;
		m_silentSince.compare_exchange_strong(none, sentAt.time_since_epoch().count(), std::memory_order_acq_rel);
		return {false, "the plugin host did not answer in time", {}, true};
	}

	bool HostConnection::post(
		Message type, uint32_t instance, const std::vector<uint8_t>& body, std::chrono::milliseconds timeout)
	{
		rt::assertNotRealtime("HostConnection::post");
		uint32_t id = 0;
		{
			const std::scoped_lock lock(m_mutex);
			if (exited())
				return false;
			id = m_nextRequest++;
			if (id == 0)
				id = m_nextRequest++;
		}
		std::string error;
		return send(type, instance, id, body, std::chrono::steady_clock::now() + timeout, error);
	}

	void HostConnection::received(const ControlMessage& message)
	{
		if (message.type == Message::Reply)
		{
			Reply reply;
			if (message.tooLarge)
			{
				reply.error = "the reply of the plugin host is too large";
			}
			else
			{
				Reader replyReader(message.body);
				reply.ok = replyReader.u8() != 0;
				reply.error = replyReader.string();
				if (!replyReader.ok())
					reply = {false, "the plugin host sent an invalid reply", {}, false};
				else
					reply.body.assign(
						message.body.end() - static_cast<std::ptrdiff_t>(replyReader.remaining()), message.body.end());
			}
			const std::scoped_lock lock(m_mutex); // so a request that times out meanwhile doesn't make it silent again
			m_silentSince.store(0, std::memory_order_release); // a late reply too: the main thread is back
			if (const auto found = m_pending.find(message.header.request); found != m_pending.end())
			{
				found->second->reply = std::move(reply);
				found->second->done = true;
				m_replied.notify_all();
			}
		}
		else if (message.type == Message::Notify && !message.tooLarge)
		{
			Reader notifyReader(message.body);
			const uint32_t notification = notifyReader.u32();
			const uint32_t detail = notifyReader.u32();
			// Notifications of a newer minor version are ignored.
			if (!notifyReader.ok() || notification < 1 || notification > g_lastNotification)
				return;
			const std::scoped_lock lock(m_dispatchMutex);
			if (const auto found = m_listeners.find(message.header.instance); found != m_listeners.end())
				found->second(static_cast<Notification>(notification), detail);
		}
		// Other types are from a newer minor version: ignored.
	}

	void HostConnection::run()
	{
		MessageReader reader(m_control);
		ControlMessage message;
		while (!m_stopping)
		{
			const ipc::WaitResult wait = m_control.waitForControl(g_poll);
			ipc::ReadStatus status = ipc::ReadStatus::Empty;
			while ((status = reader.read(message)) == ipc::ReadStatus::Ok)
				received(message);
			if (status == ipc::ReadStatus::Corrupt)
			{
				logging::error("Plugin host {} broke the protocol; ending it", m_processId);
				m_killRequested = true;
			}
			if (m_killRequested)
			{
				m_process.kill();
				break;
			}
			if (wait == ipc::WaitResult::PeerClosed || !m_process.running())
				break;
		}
		finish();
	}

	void HostConnection::finish()
	{
		if (!m_stopping)
		{
			const std::optional<int> code = m_process.wait(0ms);
			logging::warning("Plugin host {} is gone (exit code {})", m_processId, code.value_or(-1));
		}
		m_exited = true;
		const std::scoped_lock lock(m_mutex);
		for (auto& [id, pending] : m_pending)
		{
			pending->reply = {false, "the plugin host is gone", {}, false};
			pending->done = true;
		}
		m_replied.notify_all();
	}
}
