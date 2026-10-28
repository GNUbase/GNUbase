// SPDX-License-Identifier: GPL-3.0-or-later
#include "pluginhost/host.h"

#include "platform/log.h"
#include "platform/process.h"

#include <chrono>
#include <cstdlib>
#include <exception>
#include <utility>

namespace gnubase::pluginhost
{
	namespace
	{
		using namespace std::chrono_literals;
		using namespace plugin::remote;

		constexpr auto g_poll = 20ms;
		constexpr auto g_engineCheck = 200ms;
		constexpr auto g_sendTimeout = 10s;
		constexpr auto g_stopTimeout = 2000ms;
	}

	PluginHost::PluginHost(ipc::Connection control, Formats formats)
		: m_control(std::move(control)), m_formats(std::move(formats))
	{
	}

	PluginHost::~PluginHost()
	{
		releaseStubs();
	}

	int PluginHost::run()
	{
		MessageReader reader(m_control);
		const uint32_t engine = m_control.peerProcessId();
		auto lastCheck = std::chrono::steady_clock::now();
		Message type = Message::Hello;
		ControlHeader header;
		std::vector<uint8_t> body;
		while (m_exitCode < 0)
		{
			const ipc::WaitResult wait = m_control.waitForControl(g_poll);
			ipc::ReadStatus status = ipc::ReadStatus::Empty;
			while (m_exitCode < 0 && (status = reader.read(type, header, body)) == ipc::ReadStatus::Ok)
				handle(type, header, body);
			if (status == ipc::ReadStatus::Corrupt)
			{
				logging::error("The engine broke the protocol");
				m_exitCode = 3;
				break;
			}

			// What the plugins asked of the engine.
			for (auto& [instance, stub] : m_stubs)
			{
				const uint32_t bits = stub->idle();
				for (uint32_t bit = 0; bit < 32; ++bit)
				{
					if ((bits & (uint32_t{1} << bit)) == 0)
						continue;
					Writer notification;
					notification.u32(bit);
					send(Message::Notify, ControlHeader{0, instance}, notification.data());
				}
			}

			if (wait == ipc::WaitResult::PeerClosed)
			{
				logging::info("The engine closed the connection");
				m_exitCode = 0;
			}
			else if (std::chrono::steady_clock::now() - lastCheck >= g_engineCheck)
			{
				lastCheck = std::chrono::steady_clock::now();
				if (!platform::processExists(engine))
				{
					logging::warning("The engine is gone");
					m_exitCode = 0;
				}
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
		if (!sendMessage(m_control, type, header, body, g_sendTimeout))
			logging::warning("Cannot send a message to the engine");
	}

	void PluginHost::handle(Message type, const ControlHeader& header, const std::vector<uint8_t>& body)
	{
		Reader request(body);
		Writer reply;
		std::string error;
		bool ok = false;
		try
		{
			if (!m_greeted)
			{
				// The handshake comes first: the versions of the messages and of the plugin API.
				const uint32_t protocol = request.u32();
				const uint32_t api = request.u32();
				if (type != Message::Hello || !request.ok())
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
					reply.u32(g_protocolVersion);
					reply.u32(plugin::g_apiVersion);
					reply.u32(platform::currentProcessId());
					ok = true;
					m_greeted = true;
				}
				if (!ok)
				{
					logging::error("Refusing the engine: {}", error);
					m_exitCode = 4;
				}
			}
			else if (type == Message::Create)
			{
				ok = create(header.instance, request, reply, error);
			}
			else if (type == Message::Destroy)
			{
				ok = destroy(header.instance, error);
			}
			else if (const auto found = m_stubs.find(header.instance); found != m_stubs.end())
			{
				ok = found->second->handle(type, request, reply, error);
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

		Writer message;
		message.u8(ok ? 1 : 0);
		message.string(ok ? std::string() : error);
		message.raw(reply.data().data(), ok ? reply.data().size() : 0);
		send(Message::Reply, header, message.data());
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
			return true;
		if (!found->second->stopAudio(g_stopTimeout))
		{
			error = "the plugin hangs in process()";
			return false;
		}
		m_stubs.erase(found);
		return true;
	}
}
