// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/engine.h"

#include "engine/audio/audioengine.h"
#include "engine/document/projectmodel.h"
#include "engine/jobs/jobs.h"
#include "engine/peaks/peakcache.h"
#include "engine/plugins/hostmanager.h"
#include "engine/processors/builtin.h"
#include "engine/settings/usersettings.h"
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
			std::function<void(const Event&)> listener; // a Listener's mailbox hands events to it instead
			std::string name;                           // the client's, for the command log
			std::function<void()> onArrival;            // called after a message was queued

			void post(Message message)
			{
				{
					// Under the lock, so a Listener that stops waits for a call that runs.
					const std::scoped_lock lock(mutex);
					if (listener)
					{
						if (const auto* event = std::get_if<Event>(&message))
							listener(*event);
						return;
					}
					messages.push_back(std::move(message));
					if (onArrival)
						onArrival(); // under the lock too, so a client that goes away waits for a call that runs
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
		// How many commands engine.commandLog keeps.
		constexpr size_t g_commandLogSize = 256;

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

	Engine::Engine(std::vector<std::unique_ptr<audio::Backend>> backends,
		const std::filesystem::path& settingsDirectory,
		std::vector<std::unique_ptr<gnubase::midi::Backend>> midiBackends)
		: m_settingsDirectory(settingsDirectory)
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
		registerCommand(
			{"engine.commandLog", 1,
				"The last 256 commands that ran, oldest first, without queries: what the UI and other "
				"clients did. Each with its request id (events name it too), the client that sent it (ui, "
				"mcp, lua, app for the command line, engine for the engine's own commands), the command, "
				"its parameters, and the error if it failed. Commands the engine ran on its own that changed "
				"nothing, such as tries to open a lost audio device again, are left out.",
				CommandInfo{}.parameters, Json::parse(R"({"type": "object", "properties": {"commands":
								{"type": "array", "items": {"type": "object", "properties": {"request": {"type":
								"integer"}, "client": {"type": "string"}, "command": {"type": "string"}, "parameters":
								{}, "error": {"type": "string"}}, "required": ["request", "client", "command",
								"parameters"]}}}, "required": ["commands"]})"),
				true},
			[this](CommandContext&) { return Json{{"commands", Json(m_commandLog)}}; });
		processors::registerCommands(*this);
		m_plugins = std::make_unique<plugins::HostManager>();
		m_plugins->registerCommands(*this);
		m_plugins->onReport([this] { post("pluginhost.check"); });
		m_jobs = std::make_unique<Jobs>(*this);
		m_jobs->registerCommands(*this);
		m_project = std::make_unique<ProjectModel>();
		m_project->registerCommands(*this, *m_jobs);
		m_settings = std::make_unique<settings::Store>(settings::userFormat(), settingsDirectory);
		settings::registerCommands(*this, *m_settings, [this] { applySettings(); });
		m_audio = std::make_unique<AudioEngine>(
			m_project->snapshots(), *m_settings, std::move(backends), std::move(midiBackends));
		m_audio->registerCommands(*this);
		m_peaks = std::make_unique<peaks::PeakCache>(
			[this](const std::string& name, const Json& data) { broadcast(name, data); });
		m_peaks->registerCommands(*this);
		applySettings();
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
		m_thread.join(); // fails the commands that didn't run, which jobs may wait for
		m_peaks.reset(); // stops its worker while the clients can still be reached
		m_jobs.reset();  // no command starts a job anymore; cancels the running ones and waits for them
	}

	void Engine::applySettings()
	{
		MidiChase chase;
		chase.notes = m_settings->get(settings::g_midiChaseNotes);
		chase.controllers = m_settings->get(settings::g_midiChaseControllers);
		chase.programs = m_settings->get(settings::g_midiChasePrograms);
		chase.pitchBend = m_settings->get(settings::g_midiChasePitchBend);
		chase.beyondParts = m_settings->get(settings::g_midiChaseBeyondParts);
		m_audio->setChase(chase);
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
			m_requests.push_back({m_nextRequest++, std::move(command), std::move(parameters), nullptr, nullptr});
		}
		m_wake.notify_one();
	}

	void Engine::post(std::string command, Json parameters, std::function<void(Result result)> done)
	{
		rt::assertNotRealtime("engine::Engine::post");
		RequestId id = 0;
		{
			const std::scoped_lock lock(m_mutex);
			id = m_nextRequest++;
			if (!m_stopping)
			{
				m_requests.push_back({id, std::move(command), std::move(parameters), nullptr, std::move(done)});
				m_wake.notify_one();
				return;
			}
		}
		done({id, nullptr, "the engine stopped before " + command + " ran"});
	}

	void Engine::broadcast(std::string name, Json data)
	{
		rt::assertNotRealtime("engine::Engine::broadcast");
		std::vector<std::shared_ptr<detail::Mailbox>> clients;
		{
			const std::scoped_lock lock(m_mutex);
			clients = m_clients;
		}
		const Event event{std::move(name), std::move(data), 0};
		for (const auto& client : clients)
			client->post(event);
	}

	rt::SnapshotPublisher<RenderSnapshot>& Engine::renderSnapshots()
	{
		return m_project->snapshots();
	}

	plugins::HostManager& Engine::pluginHosts()
	{
		return *m_plugins;
	}

	int64_t Engine::playbackPosition() const
	{
		return m_audio->position();
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
			std::deque<Request> left;
			bool stopping = false;
			{
				std::unique_lock lock(m_mutex);
				m_wake.wait(lock, [this] { return m_stopping || !m_requests.empty(); });
				stopping = m_stopping;
				if (stopping)
				{
					left.swap(m_requests); // from here on, post fails at once
				}
				else
				{
					request = std::move(m_requests.front());
					m_requests.pop_front();
				}
			}
			if (stopping)
			{
				// Jobs may wait for commands they posted; they get an error.
				for (const Request& waiting : left)
					if (waiting.done)
						waiting.done({waiting.id, nullptr, "the engine stopped before " + waiting.command + " ran"});
				return;
			}
			execute(request);
		}
	}

	std::shared_ptr<const Engine::Command> Engine::find(const std::string& name) const
	{
		const std::scoped_lock lock(m_mutex);
		const auto found = m_commands.find(name);
		if (found == m_commands.end())
			throw CommandError("unknown command " + name);
		return found->second;
	}

	Json Engine::runInside(const std::string& command, const Json& parameters, CommandContext& outer)
	{
		try
		{
			const std::shared_ptr<const Command> inner = find(command);
			if (const std::string problem = validate(parameters, inner->info.parameters); !problem.empty())
				throw CommandError(problem);
			CommandContext context(parameters, &outer);
			Json value = inner->handler(context);
			if (const std::string problem = validate(value, inner->info.result, "result"); !problem.empty())
				throw CommandError("returned an invalid result: " + problem);
			return value;
		}
		catch (const std::exception& error)
		{
			throw CommandError(command + ": " + error.what());
		}
	}

	Json CommandContext::call(const std::string& command, const Json& parameters)
	{
		if (m_outer != nullptr)
			return m_outer->call(command, parameters);
		GNUBASE_ASSERT(m_engine != nullptr);
		if (m_engine == nullptr)
			throw CommandError("no engine runs this command");
		return m_engine->runInside(command, parameters, *this);
	}

	void Engine::execute(Request& request)
	{
		CommandContext context(request.parameters);
		context.m_engine = this;
		Result result;
		result.request = request.id;
		std::vector<std::shared_ptr<detail::Mailbox>> clients;
		bool query = false;
		try
		{
			const std::shared_ptr<const Command> command = find(request.command);
			query = command->info.query;
			if (const std::string problem = validate(request.parameters, command->info.parameters); !problem.empty())
				throw CommandError(problem);
			Json value = command->handler(context);
			m_audio->sync(); // a changed project gets a new processing graph
			if (const std::string problem = validate(value, command->info.result, "result"); !problem.empty())
				throw CommandError(request.command + " returned an invalid result: " + problem);
			result.value = std::move(value);
		}
		catch (const std::exception& error)
		{
			result.error = error.what();
			context.m_events.clear(); // a failed command changes nothing
		}
		if (!query && !context.m_unlogged)
		{
			Json entry = {{"request", request.id}, {"client", request.sender ? request.sender->name : "engine"},
				{"command", request.command}, {"parameters", request.parameters}};
			if (!result.ok())
				entry["error"] = result.error;
			m_commandLog.push_back(std::move(entry));
			if (m_commandLog.size() > g_commandLogSize)
				m_commandLog.pop_front();
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
		// After the command's own events, so media.added reaches clients before the peak worker's media.peaksReady.
		try
		{
			m_peaks->sync(m_project->snapshots().latest().media);
		}
		catch (const std::exception& error)
		{
			logging::error("The peaks of the media files are not updated: {}", error.what());
		}
		if (request.sender)
			request.sender->post(std::move(result));
		else if (request.done)
			request.done(std::move(result));
		else if (!result.ok())
			logging::error("{} failed: {}", request.command, result.error);
	}

	Client::Client(Engine& engine, std::string name, std::function<void()> arrived)
		: m_engine(engine), m_mailbox(std::make_shared<detail::Mailbox>())
	{
		m_mailbox->name = std::move(name);
		m_mailbox->onArrival = std::move(arrived);
		const std::scoped_lock lock(m_engine.m_mutex);
		m_engine.m_clients.push_back(m_mailbox);
	}

	Client::~Client()
	{
		{
			const std::scoped_lock lock(m_engine.m_mutex);
			auto& clients = m_engine.m_clients;
			clients.erase(std::remove(clients.begin(), clients.end(), m_mailbox), clients.end());
		}
		// The command thread may still hold the mailbox; from here on it calls nobody.
		const std::scoped_lock lock(m_mailbox->mutex);
		m_mailbox->onArrival = nullptr;
	}

	Listener::Listener(Engine& engine, std::function<void(const Event&)> function)
		: m_engine(engine), m_mailbox(std::make_shared<detail::Mailbox>())
	{
		m_mailbox->listener = std::move(function);
		const std::scoped_lock lock(m_engine.m_mutex);
		m_engine.m_clients.push_back(m_mailbox);
	}

	Listener::~Listener()
	{
		{
			const std::scoped_lock lock(m_engine.m_mutex);
			auto& clients = m_engine.m_clients;
			clients.erase(std::remove(clients.begin(), clients.end(), m_mailbox), clients.end());
		}
		// The command thread may still hold the mailbox; from here on it only queues what nobody reads.
		const std::scoped_lock lock(m_mailbox->mutex);
		m_mailbox->listener = nullptr;
	}

	RequestId Client::send(std::string command, Json parameters)
	{
		rt::assertNotRealtime("engine::Client::send");
		RequestId id = 0;
		{
			const std::scoped_lock lock(m_engine.m_mutex);
			id = m_engine.m_nextRequest++;
			m_engine.m_requests.push_back({id, std::move(command), std::move(parameters), m_mailbox, nullptr});
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
