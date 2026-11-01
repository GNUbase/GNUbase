// SPDX-License-Identifier: GPL-3.0-or-later
#include "fakertkit.h"

#include <dbus/dbus.h>
#include <poll.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace gnubase::test
{
	namespace
	{
		constexpr const char* g_rtkit = "org.freedesktop.RealtimeKit1";

		struct State
		{
			FakeRtkit::Mode mode = FakeRtkit::Mode::Grants;
			DBusServer* server = nullptr;
			std::vector<DBusWatch*> watches; // the server's, for its listening socket
			std::vector<DBusConnection*> connections;
			mutable std::mutex mutex;
			std::vector<FakeRtkit::Request> requests;
			std::atomic<bool> stop{false};
			std::thread thread;
			std::optional<std::string> previousAddress;

			void serve();
		};

		dbus_bool_t addWatch(DBusWatch* watch, void* impl)
		{
			static_cast<State*>(impl)->watches.push_back(watch);
			return TRUE;
		}

		void removeWatch(DBusWatch* watch, void* impl)
		{
			auto& watches = static_cast<State*>(impl)->watches;
			watches.erase(std::remove(watches.begin(), watches.end(), watch), watches.end());
		}

		void toggleWatch(DBusWatch* /*watch*/, void* /*impl*/)
		{
			// serve polls the enabled watches only
		}

		// Appends value to reply as a variant of type ("i" or "x").
		template <typename Value> void appendVariant(DBusMessage* reply, int type, const char* signature, Value value)
		{
			DBusMessageIter iter{};
			DBusMessageIter variant{};
			dbus_message_iter_init_append(reply, &iter);
			dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, signature, &variant);
			dbus_message_iter_append_basic(&variant, type, &value);
			dbus_message_iter_close_container(&iter, &variant);
		}

		DBusMessage* answer(State& impl, DBusMessage* message)
		{
			const bool rtkit = impl.mode != FakeRtkit::Mode::Missing;
			if (dbus_message_is_method_call(message, DBUS_INTERFACE_DBUS, "Hello") != 0)
			{
				DBusMessage* reply = dbus_message_new_method_return(message);
				const char* name = ":1.1";
				dbus_message_append_args(reply, DBUS_TYPE_STRING, &name, DBUS_TYPE_INVALID);
				return reply;
			}
			if (rtkit && dbus_message_is_method_call(message, DBUS_INTERFACE_PROPERTIES, "Get") != 0)
			{
				const char* interface = nullptr;
				const char* property = nullptr;
				dbus_message_get_args(
					message, nullptr, DBUS_TYPE_STRING, &interface, DBUS_TYPE_STRING, &property, DBUS_TYPE_INVALID);
				const std::string name = property != nullptr ? property : "";
				if (name != "MaxRealtimePriority" && name != "RTTimeUSecMax")
					return dbus_message_new_error(message, DBUS_ERROR_UNKNOWN_PROPERTY, "no such property");
				DBusMessage* reply = dbus_message_new_method_return(message);
				if (name == "MaxRealtimePriority")
					appendVariant(reply, DBUS_TYPE_INT32, "i", dbus_int32_t{FakeRtkit::g_maxPriority});
				else
					appendVariant(reply, DBUS_TYPE_INT64, "x", dbus_int64_t{FakeRtkit::g_rttimeMax});
				return reply;
			}
			if (rtkit && dbus_message_is_method_call(message, g_rtkit, "MakeThreadRealtime") != 0)
			{
				dbus_uint64_t thread = 0;
				dbus_uint32_t priority = 0;
				dbus_message_get_args(
					message, nullptr, DBUS_TYPE_UINT64, &thread, DBUS_TYPE_UINT32, &priority, DBUS_TYPE_INVALID);
				{
					const std::scoped_lock lock(impl.mutex);
					impl.requests.push_back({thread, priority});
				}
				if (impl.mode == FakeRtkit::Mode::Refuses)
					return dbus_message_new_error(message, DBUS_ERROR_ACCESS_DENIED, "refused");
				return dbus_message_new_method_return(message);
			}
			return dbus_message_new_error(message, DBUS_ERROR_SERVICE_UNKNOWN, "no such service");
		}

		DBusHandlerResult handle(DBusConnection* connection, DBusMessage* message, void* impl)
		{
			if (dbus_message_get_type(message) != DBUS_MESSAGE_TYPE_METHOD_CALL)
				return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
			DBusMessage* reply = answer(*static_cast<State*>(impl), message);
			dbus_connection_send(connection, reply, nullptr);
			dbus_message_unref(reply);
			return DBUS_HANDLER_RESULT_HANDLED;
		}

		void accept(DBusServer* /*server*/, DBusConnection* connection, void* impl)
		{
			dbus_connection_ref(connection);
			dbus_connection_add_filter(connection, handle, impl, nullptr);
			static_cast<State*>(impl)->connections.push_back(connection);
		}

		void setAddress(const char* address)
		{
			if (address != nullptr)
				setenv("DBUS_SYSTEM_BUS_ADDRESS", address, 1);
			else
				unsetenv("DBUS_SYSTEM_BUS_ADDRESS");
		}
	}

	void State::serve()
	{
		while (!stop.load())
		{
			std::vector<pollfd> fds;
			std::vector<DBusWatch*> polled;
			for (DBusWatch* watch : watches)
				if (dbus_watch_get_enabled(watch) != 0)
				{
					fds.push_back({dbus_watch_get_unix_fd(watch), POLLIN, 0});
					polled.push_back(watch);
				}
			poll(fds.data(), fds.size(), 1);
			for (size_t i = 0; i < fds.size(); ++i)
				if ((fds[i].revents & POLLIN) != 0)
					dbus_watch_handle(polled[i], DBUS_WATCH_READABLE);
			for (auto connection = connections.begin(); connection != connections.end();)
			{
				if (dbus_connection_read_write_dispatch(*connection, 0) != 0)
				{
					++connection;
					continue;
				}
				dbus_connection_close(*connection); // the client hung up
				dbus_connection_unref(*connection);
				connection = connections.erase(connection);
			}
		}
	}

	struct FakeRtkit::Impl : State
	{
	};

	FakeRtkit::FakeRtkit(Mode mode) : m_impl(std::make_unique<Impl>())
	{
		m_impl->mode = mode;
		if (const char* previous = std::getenv("DBUS_SYSTEM_BUS_ADDRESS"))
			m_impl->previousAddress = previous;
		if (mode == Mode::NoBus)
		{
			setAddress("unix:path=/nonexistent/gnubase-test/system_bus_socket");
			return;
		}
		DBusError error;
		dbus_error_init(&error);
		m_impl->server = dbus_server_listen("unix:tmpdir=/tmp", &error);
		if (m_impl->server == nullptr)
		{
			const std::string message = error.message != nullptr ? error.message : "unknown error";
			dbus_error_free(&error);
			throw std::runtime_error("the fake rtkit can't listen: " + message);
		}
		State* const state = m_impl.get(); // what the callbacks get
		dbus_server_set_watch_functions(m_impl->server, addWatch, removeWatch, toggleWatch, state, nullptr);
		dbus_server_set_new_connection_function(m_impl->server, accept, state, nullptr);
		char* address = dbus_server_get_address(m_impl->server);
		setAddress(address);
		dbus_free(address);
		m_impl->thread = std::thread([impl = m_impl.get()] { impl->serve(); });
	}

	FakeRtkit::~FakeRtkit()
	{
		if (m_impl->server != nullptr)
		{
			m_impl->stop.store(true);
			m_impl->thread.join();
			for (DBusConnection* connection : m_impl->connections)
			{
				dbus_connection_close(connection);
				dbus_connection_unref(connection);
			}
			dbus_server_disconnect(m_impl->server);
			dbus_server_unref(m_impl->server);
		}
		setAddress(m_impl->previousAddress ? m_impl->previousAddress->c_str() : nullptr);
	}

	std::vector<FakeRtkit::Request> FakeRtkit::requests() const
	{
		const std::scoped_lock lock(m_impl->mutex);
		return m_impl->requests;
	}
}
