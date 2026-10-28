// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/engine.h"

#include "engine/audio/audioengine.h"
#include "engine/document/projectmodel.h"
#include "engine/peaks/peakcache.h"
#include "engine/processors/builtin.h"
#include "platform/log.h"
#include "platform/rt/realtime.h"

#include <algorithm>
#include <utility>

namespace gnubase::engine
{
	namespace detail
	{
		// Where the engine posts a client's messages.
		struct Mailbox
		{
			std::mutex mutex;
			std::condition_variable arrived;
			std::deque<Message> messages;

			void post(Message message)
			{
				{
					const std::scoped_lock lock(mutex);
					messages.push_back(std::move(message));
				}
				arrived.notify_all();
			}

			// Removes and returns the first message that matches, waiting up to timeout for one.
			template <typename Match> std::optional<Message> take(std::chrono::milliseconds timeout, Match match)
			{
				std::unique_lock lock(mutex);
				std::optional<Message> found;
				arrived.wait_for(lock, timeout,
					[&]
					{
						const auto it = std::find_if(messages.begin(), messages.end(), match);
						if (it == messages.end())
							return false;
						found = std::move(*it);
						messages.erase(it);
						return true;
					});
				return found;
			}
		};
	}

	namespace
	{
		Json toJson(const CommandInfo& info)
		{
			return {{"name", info.name}, {"version", info.version}, {"description", info.description},
				{"parameters", info.parameters}, {"result", info.result}, {"query", info.query}};
		}
	}

	const char* version()
	{
		return GNUBASE_VERSION;
	}

	Engine::Engine(std::vector<std::unique_ptr<audio::Backend>> backends)
	{
		registerCommand({"engine.version", 1, "The GNUbase version.", CommandInfo{}.parameters,
							Json::parse(R"({"type": "object", "properties": {"version": {"type": "string"}},
								"required": ["version"]})"),
							true},
			[](CommandContext&) { return Json{{"version", version()}}; });
		registerCommand({"engine.commands", 1, "Every command with its documentation, parameters and result.",
							CommandInfo{}.parameters,
							Json::parse(R"({"type": "object", "properties": {"commands": {"type": "array"}},
								"required": ["commands"]})"),
							true},
			[this](CommandContext&)
			{
				Json list = Json::array();
				for (const CommandInfo& info : commands())
					list.push_back(toJson(info));
				return Json{{"commands", list}};
			});
		processors::registerCommands(*this);
		m_project = std::make_unique<ProjectModel>();
		m_project->registerCommands(*this);
		m_audio = std::make_unique<AudioEngine>(m_project->snapshots(), std::move(backends));
		m_audio->registerCommands(*this);
		m_peaks = std::make_unique<peaks::PeakCache>(
			[this](const std::string& name, const Json& data) { broadcast({name, data, 0}); });
		m_peaks->registerCommands(*this);
		m_thread = std::thread([this] { run(); });
	}

	Engine::~Engine()
	{
		{
			const std::scoped_lock lock(m_mutex);
			GNUBASE_ASSERT(m_clients.empty());
			m_stopping = true;
		}
		m_wake.notify_all();
		m_thread.join();
		m_peaks.reset(); // stops its worker while the clients can still be reached
	}

	void Engine::registerCommand(CommandInfo info, CommandHandler handler)
	{
		rt::assertNotRealtime("engine::Engine::registerCommand");
		const std::scoped_lock lock(m_mutex);
		const std::string name = info.name;
		auto command = std::make_shared<const Command>(Command{std::move(info), std::move(handler)});
		if (!m_commands.emplace(name, std::move(command)).second)
			throw std::logic_error("command " + name + " is already registered");
	}

	void Engine::post(std::string command, Json parameters)
	{
		rt::assertNotRealtime("engine::Engine::post");
		{
			const std::scoped_lock lock(m_mutex);
			m_requests.push_back({m_nextRequest++, std::move(command), std::move(parameters), nullptr});
		}
		m_wake.notify_one();
	}

	rt::SnapshotPublisher<RenderSnapshot>& Engine::renderSnapshots()
	{
		return m_project->snapshots();
	}

	std::vector<CommandInfo> Engine::commands() const
	{
		const std::scoped_lock lock(m_mutex);
		std::vector<CommandInfo> list;
		list.reserve(m_commands.size());
		for (const auto& entry : m_commands)
			list.push_back(entry.second->info);
		return list;
	}

	void Engine::run()
	{
		for (;;)
		{
			Request request;
			{
				std::unique_lock lock(m_mutex);
				m_wake.wait(lock, [this] { return m_stopping || !m_requests.empty(); });
				if (m_stopping)
					return;
				request = std::move(m_requests.front());
				m_requests.pop_front();
			}
			execute(request);
		}
	}

	void Engine::execute(Request& request)
	{
		CommandContext context(request.parameters);
		Result result;
		result.request = request.id;
		std::vector<std::shared_ptr<detail::Mailbox>> clients;
		try
		{
			std::shared_ptr<const Command> command;
			{
				const std::scoped_lock lock(m_mutex);
				if (const auto found = m_commands.find(request.command); found != m_commands.end())
					command = found->second;
			}
			if (!command)
				throw CommandError("unknown command " + request.command);
			if (const std::string problem = validate(request.parameters, command->info.parameters); !problem.empty())
				throw CommandError(problem);
			Json value = command->handler(context);
			m_audio->sync(); // a changed project gets a new processing graph
			m_peaks->sync(m_project->snapshots().latest().media);
			if (const std::string problem = validate(value, command->info.result, "result"); !problem.empty())
				throw CommandError(request.command + " returned an invalid result: " + problem);
			result.value = std::move(value);
		}
		catch (const std::exception& error)
		{
			result.error = error.what();
			context.m_events.clear(); // a failed command changes nothing
		}

		{
			const std::scoped_lock lock(m_mutex);
			clients = m_clients;
		}
		for (Event& event : context.m_events)
		{
			event.request = request.id;
			for (const auto& client : clients)
				client->post(event);
		}
		if (request.sender)
			request.sender->post(std::move(result));
		else if (!result.ok())
			logging::error("{} failed: {}", request.command, result.error);
	}

	void Engine::broadcast(Event event)
	{
		std::vector<std::shared_ptr<detail::Mailbox>> clients;
		{
			const std::scoped_lock lock(m_mutex);
			clients = m_clients;
		}
		for (const auto& client : clients)
			client->post(event);
	}

	Client::Client(Engine& engine) : m_engine(engine), m_mailbox(std::make_shared<detail::Mailbox>())
	{
		const std::scoped_lock lock(m_engine.m_mutex);
		m_engine.m_clients.push_back(m_mailbox);
	}

	Client::~Client()
	{
		const std::scoped_lock lock(m_engine.m_mutex);
		auto& clients = m_engine.m_clients;
		clients.erase(std::remove(clients.begin(), clients.end(), m_mailbox), clients.end());
	}

	RequestId Client::send(std::string command, Json parameters)
	{
		rt::assertNotRealtime("engine::Client::send");
		RequestId id = 0;
		{
			const std::scoped_lock lock(m_engine.m_mutex);
			id = m_engine.m_nextRequest++;
			m_engine.m_requests.push_back({id, std::move(command), std::move(parameters), m_mailbox});
		}
		m_engine.m_wake.notify_one();
		return id;
	}

	std::optional<Message> Client::poll()
	{
		return wait(std::chrono::milliseconds(0));
	}

	std::optional<Message> Client::wait(std::chrono::milliseconds timeout)
	{
		rt::assertNotRealtime("engine::Client::wait");
		return m_mailbox->take(timeout, [](const Message&) { return true; });
	}

	Result Client::call(std::string command, Json parameters, std::chrono::milliseconds timeout)
	{
		const std::string name = command;
		const RequestId id = send(std::move(command), std::move(parameters));
		std::optional<Message> message = m_mailbox->take(timeout,
			[id](const Message& candidate)
			{
				const auto* result = std::get_if<Result>(&candidate);
				return result != nullptr && result->request == id;
			});
		if (!message)
			return {id, nullptr, name + " got no result within " + std::to_string(timeout.count()) + " ms"};
		return std::get<Result>(std::move(*message));
	}
}
