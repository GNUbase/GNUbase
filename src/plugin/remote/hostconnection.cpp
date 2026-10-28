// SPDX-License-Identifier: GPL-3.0-or-later
#include "plugin/remote/hostconnection.h"

#include "platform/log.h"
#include "platform/rt/realtime.h"

#include <exception>
#include <utility>

namespace gnubase::plugin::remote
{
	namespace
	{
		using namespace std::chrono_literals;

		constexpr auto g_poll = 20ms;
		constexpr auto g_exitWait = 2000ms;
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
			host->m_process = platform::ChildProcess::start(options.executable, arguments);
		}
		catch (const std::exception& error)
		{
			throw LaunchError(std::string("Cannot start the plugin host: ") + error.what());
		}
		host->m_processId = host->m_process.id();

		// Wait for the host to open the connection, unless it exits first, e.g. because its IPC version differs.
		const auto deadline = std::chrono::steady_clock::now() + options.timeout;
		while (!host->m_control.waitForPeer(g_poll))
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
		const std::scoped_lock lock(m_processMutex);
		if (!m_process.wait(g_exitWait))
			logging::warning("Plugin host {} did not exit; ending it", m_processId);
		// ChildProcess kills a process that still runs.
	}

	bool HostConnection::checkExited()
	{
		rt::assertNotRealtime("HostConnection::checkExited");
		{
			const std::scoped_lock lock(m_processMutex);
			if (!m_process.running())
				m_exited = true;
		}
		return exited();
	}

	void HostConnection::kill()
	{
		const std::scoped_lock lock(m_processMutex);
		m_process.kill();
		m_exited = true;
	}

	void HostConnection::onNotification(std::function<void(uint32_t instance, Notification notification)> handler)
	{
		const std::scoped_lock lock(m_mutex);
		m_notify = std::move(handler);
	}

	HostConnection::Reply HostConnection::request(
		Message type, uint32_t instance, const std::vector<uint8_t>& body, std::chrono::milliseconds timeout)
	{
		rt::assertNotRealtime("HostConnection::request");
		Pending pending;
		ControlHeader header;
		header.instance = instance;
		{
			const std::scoped_lock lock(m_mutex);
			if (exited())
				return {false, "the plugin host is gone", {}};
			header.request = m_nextRequest++;
			if (header.request == 0)
				header.request = m_nextRequest++;
			m_pending[header.request] = &pending;
		}
		bool sent = false;
		{
			const std::scoped_lock lock(m_sendMutex);
			sent = sendMessage(m_control, type, header, body, timeout);
		}

		std::unique_lock lock(m_mutex);
		if (sent)
			m_replied.wait_for(lock, timeout, [&] { return pending.done; });
		m_pending.erase(header.request);
		if (pending.done)
			return std::move(pending.reply);
		return {false, sent ? "the plugin host did not answer in time" : "the plugin host does not read requests", {}};
	}

	void HostConnection::run()
	{
		MessageReader reader(m_control);
		Message type = Message::Reply;
		ControlHeader header;
		std::vector<uint8_t> body;
		while (!m_stopping)
		{
			const ipc::WaitResult wait = m_control.waitForControl(g_poll);
			ipc::ReadStatus status = ipc::ReadStatus::Empty;
			while ((status = reader.read(type, header, body)) == ipc::ReadStatus::Ok)
			{
				if (type == Message::Reply)
				{
					Reader replyReader(body);
					Reply reply;
					reply.ok = replyReader.u8() != 0;
					reply.error = replyReader.string();
					if (!replyReader.ok())
						reply = {false, "the plugin host sent an invalid reply", {}};
					else
						reply.body.assign(
							body.end() - static_cast<std::ptrdiff_t>(replyReader.remaining()), body.end());
					const std::scoped_lock lock(m_mutex);
					if (const auto found = m_pending.find(header.request); found != m_pending.end())
					{
						found->second->reply = std::move(reply);
						found->second->done = true;
						m_replied.notify_all();
					}
				}
				else if (type == Message::Notify)
				{
					Reader notifyReader(body);
					const uint32_t bit = notifyReader.u32();
					std::function<void(uint32_t, Notification)> notify;
					{
						const std::scoped_lock lock(m_mutex);
						notify = m_notify;
					}
					// Notifications of a newer minor version are ignored.
					if (notifyReader.ok() && bit <= static_cast<uint32_t>(Notification::LatencyChanged) && notify)
						notify(header.instance, static_cast<Notification>(bit));
				}
				// Other types are from a newer minor version: ignored.
			}
			if (status == ipc::ReadStatus::Corrupt)
			{
				logging::error("Plugin host {} broke the protocol; ending it", m_processId);
				kill();
			}
			if (wait == ipc::WaitResult::PeerClosed || checkExited())
				break;
		}
		finish();
	}

	void HostConnection::finish()
	{
		if (!m_stopping)
		{
			{
				const std::scoped_lock lock(m_processMutex);
				m_process.wait(0ms);
			}
			logging::warning("Plugin host {} is gone", m_processId);
		}
		m_exited = true;
		const std::scoped_lock lock(m_mutex);
		for (auto& [id, pending] : m_pending)
		{
			pending->reply = {false, "the plugin host is gone", {}};
			pending->done = true;
		}
		m_replied.notify_all();
	}
}
