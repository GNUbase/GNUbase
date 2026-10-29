// SPDX-License-Identifier: GPL-3.0-or-later
#include "pluginhost/host.h"

#include "platform/log.h"
#include "platform/process.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string_view>
#include <utility>

namespace gnubase::pluginhost
{
	namespace
	{
		using namespace std::chrono_literals;
		using namespace plugin::remote;

		constexpr auto g_poll = 20ms;
		constexpr auto g_watch = 50ms;
		constexpr auto g_sendTimeout = 10s;
		constexpr auto g_stopTimeout = 2000ms;
	}

	PluginHost::PluginHost(ipc::Connection control, Formats formats)
		: m_control(std::move(control)), m_writer(m_control), m_formats(std::move(formats))
	{
		m_watchdog = std::thread([this, engine = m_control.peerProcessId()] { watchEngine(engine); });
	}

	PluginHost::~PluginHost()
	{
		releaseStubs();
		{
			const std::scoped_lock lock(m_watchMutex);
			m_watchStop = true;
		}
		m_watchWake.notify_all();
		m_watchdog.join();
	}

	void PluginHost::watchEngine(uint32_t engine)
	{
		// The main loop notices the engine's end too, but a plugin may block the main thread for good: then only
		// this thread can end the process. Linux has the kernel end it (Connection::open), Windows the engine's job
		// object; macOS has neither.
		const platform::ProcessWatch watch(engine);
		std::unique_lock lock(m_watchMutex);
		while (!m_watchWake.wait_for(lock, g_watch, [this] { return m_watchStop; }))
		{
			if (watch.gone())
			{
				logging::warning("The engine is gone; exiting");
				logging::flush();
				std::_Exit(0);
			}
		}
	}

	int PluginHost::run()
	{
		MessageReader reader(m_control);
		ControlMessage message;
		while (m_exitCode < 0)
		{
			const ipc::WaitResult wait = m_control.waitForControl(g_poll);
			ipc::ReadStatus status = ipc::ReadStatus::Empty;
			while (m_exitCode < 0 && (status = reader.read(message)) == ipc::ReadStatus::Ok)
				handle(message);
			if (status == ipc::ReadStatus::Corrupt)
			{
				logging::error("The engine broke the protocol");
				m_exitCode = 3;
				break;
			}

			// What the plugins asked of the engine.
			for (auto& [instance, stub] : m_stubs)
			{
				for (const auto& [notification, detail] : stub->idle())
				{
					Writer body;
					body.u32(static_cast<uint32_t>(notification));
					body.u32(detail);
					send(Message::Notify, ControlHeader{0, instance}, body.data());
				}
			}

			// The connection reports PeerClosed when the engine closed it and when the engine's process is gone.
			if (wait == ipc::WaitResult::PeerClosed)
			{
				logging::info("The engine closed the connection or is gone");
				m_exitCode = 0;
			}
		}
		if (!releaseStubs())
		{
			// A plugin hangs in process() on its audio thread, which can't be joined: leave without cleaning up.
			logging::error("A plugin hangs; exiting at once");
			logging::flush();
			std::_Exit(m_exitCode);
		}
		return m_exitCode;
	}

	bool PluginHost::releaseStubs()
	{
		bool released = true;
		for (auto& [instance, stub] : m_stubs)
		{
			if (!stub->stopAudio(g_stopTimeout))
			{
				// Its plugin runs on in process(): it must not be destroyed. The process ends right after.
				[[maybe_unused]] const PluginStub* const hung = stub.release();
				logging::error("Plugin instance {} hangs", instance);
				released = false;
			}
		}
		m_stubs.clear();
		return released;
	}

	void PluginHost::send(Message type, const ControlHeader& header, const std::vector<uint8_t>& body)
	{
		if (m_writer.send(type, header, body, g_sendTimeout) != SendResult::Sent)
			logging::warning("Cannot send a message to the engine");
	}

	void PluginHost::reply(
		const ControlHeader& header, bool ok, const std::string& error, const std::vector<uint8_t>& body)
	{
		Writer message;
		message.u8(ok ? 1 : 0);
		message.string(ok ? std::string() : error);
		message.raw(body.data(), ok ? body.size() : 0);
		const SendResult result = m_writer.send(Message::Reply, header, message.data(), g_sendTimeout);
		if (result == SendResult::TooLarge)
		{
			// E.g. a plugin state over the limit: the engine gets an error instead of waiting for nothing.
			logging::error("A reply of {} bytes is too large", message.data().size());
			reply(header, false,
				"the reply is too large: " + std::to_string(message.data().size()) + " bytes, at most " +
					std::to_string(m_writer.maxMessage() - sizeof(ControlHeader)),
				{});
		}
		else if (result != SendResult::Sent)
		{
			logging::warning("Cannot send a reply to the engine");
		}
	}

	void PluginHost::handle(const ControlMessage& message)
	{
		const ControlHeader& header = message.header;
		if (message.tooLarge)
		{
			reply(header, false, "the request is too large", {});
			return;
		}
		Reader request(message.body);
		Writer body;
		std::string error;
		bool ok = false;
		try
		{
			if (!m_greeted)
			{
				// The handshake comes first: the versions of the messages and of the plugin API.
				const uint32_t protocol = request.u32();
				const uint32_t api = request.u32();
				if (message.type != Message::Hello || !request.ok())
				{
					error = "expected the handshake";
				}
				else if (protocol != g_protocolVersion || api != plugin::g_apiVersion)
				{
					error = "the plugin host speaks protocol " + std::to_string(g_protocolVersion) +
						" and plugin API " + std::to_string(plugin::g_apiVersion) + ", not " +
						std::to_string(protocol) + " and " + std::to_string(api);
				}
				else
				{
					body.u32(g_protocolVersion);
					body.u32(plugin::g_apiVersion);
					body.u32(platform::currentProcessId());
					ok = true;
					m_greeted = true;
				}
				if (!ok)
				{
					logging::error("Refusing the engine: {}", error);
					m_exitCode = 4;
				}
			}
			else if (message.type == Message::Create)
			{
				ok = create(header.instance, request, body, error);
			}
			else if (message.type == Message::Destroy)
			{
				ok = destroy(header.instance, error);
			}
			else if (const auto found = m_stubs.find(header.instance); found != m_stubs.end())
			{
				ok = found->second->handle(message.type, request, body, error);
			}
			else
			{
				error = "no plugin instance " + std::to_string(header.instance);
			}
		}
		catch (const std::exception& exception)
		{
			ok = false;
			error = exception.what();
		}
		reply(header, ok, error, body.data());
	}

	bool PluginHost::create(uint32_t instance, Reader& request, Writer& reply, std::string& error)
	{
		const std::string id = request.string();
		const std::string path = request.string();
		if (!request.ok() || instance == 0 || m_stubs.count(instance) != 0)
		{
			error = "invalid request";
			return false;
		}
		auto stub = std::make_unique<PluginStub>();
		std::unique_ptr<plugin::Plugin> plugin = m_formats.create(id, path, *stub);
		if (!plugin->init())
		{
			error = "the plugin " + id + " failed to initialize";
			return false;
		}
		write(reply, describe(*plugin, false));
		stub->setPlugin(std::move(plugin));
		m_stubs[instance] = std::move(stub);
		logging::info("Created plugin {} as instance {}", id, instance);
		return true;
	}

	bool PluginHost::destroy(uint32_t instance, std::string& error)
	{
		const auto found = m_stubs.find(instance);
		if (found == m_stubs.end())
			return true; // e.g. one whose Create failed or came after the engine gave up on it
		if (!found->second->stopAudio(g_stopTimeout))
		{
			error = "the plugin hangs in process()";
			return false;
		}
		m_stubs.erase(found);
		logging::info("Destroyed instance {}", instance);
		return true;
	}

	int runPluginHost(int argc, char** argv, Formats formats)
	{
		std::string connection;
		std::string logFile = "gnubase-pluginhost.log";
		for (int i = 1; i + 1 < argc; i += 2)
		{
			const std::string_view option = argv[i];
			if (option == "--connect")
				connection = argv[i + 1];
			else if (option == "--log")
				logFile = argv[i + 1];
		}
		if (connection.empty())
		{
			std::fprintf(stderr, "usage: gnubase-pluginhost --connect <connection> [--log <file name>]\n");
			return 2;
		}

		try
		{
			const logging::Session logSession(logging::defaultDirectory() / logFile);
			ipc::Connection control;
			try
			{
				control = ipc::Connection::open(connection);
			}
			catch (const ipc::Error& error)
			{
				logging::error("{}", std::string_view(error.what()));
				return 3;
			}
			PluginHost host(std::move(control), std::move(formats));
			return host.run();
		}
		catch (const std::exception& error)
		{
			std::fprintf(stderr, "GNUbase plugin host: %s\n", error.what());
			return 1;
		}
	}
}
