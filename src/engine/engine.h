// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/schema.h"
#include "platform/audio/device.h"
#include "platform/midi/port.h"
#include "platform/rt/snapshotpublisher.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace gnubase::engine
{
	class AudioEngine;
	class Jobs;
	class ProjectModel;

	namespace plugins
	{
		class HostManager;
	}
	struct RenderSnapshot;

	namespace peaks
	{
		class PeakCache;
	}

	namespace settings
	{
		class Store;
	}

	using RequestId = uint64_t;

	// GNUbase version, e.g. "0.1.0".
	const char* version();

	// What a command does and takes. MCP tools and Lua bindings are generated from it.
	struct CommandInfo
	{
		std::string name; // "area.action", e.g. "transport.play"
		int version = 1;  // raised when the parameters or the result change incompatibly
		std::string description;
		Json parameters = Json{{"type", "object"}, {"additionalProperties", false}}; // JSON schema
		Json result = Json::object();                                                // JSON schema
		bool query = false; // only reads, never changes the project
	};

	// A change announced by a command. Every client receives it.
	struct Event
	{
		std::string name;      // e.g. "track.changed"
		Json data;             // what changed: object ids and properties
		RequestId request = 0; // the command that caused it
	};

	// The outcome of a command, sent to the client that sent it.
	struct Result
	{
		RequestId request = 0;
		Json value;        // what the command returned; null if it failed
		std::string error; // why it failed; empty if it succeeded

		bool ok() const { return error.empty(); }
	};

	using Message = std::variant<Result, Event>;

	// Thrown by a command handler to fail the command with a message for the client.
	class CommandError : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};

	class Engine;

	// What a command handler gets while it runs on the command thread.
	class CommandContext
	{
	public:
		// A command run inside another one, as in a transaction, has the outer command's context as outer.
		explicit CommandContext(const Json& parameters, CommandContext* outer = nullptr)
			: m_parameters(parameters), m_outer(outer)
		{
		}

		// Checked against the command's parameter schema.
		const Json& parameters() const { return m_parameters; }

		// Announces a change. Clients receive it before the command's result, and not at all if the command fails.
		// Inside another command, the outer command announces it.
		void emit(std::string name, Json data = Json::object())
		{
			if (m_outer != nullptr)
				m_outer->emit(std::move(name), std::move(data));
			else
				m_events.push_back({std::move(name), std::move(data)});
		}

		// Runs another command as part of this one and returns its result, e.g. for a marker command that moves the
		// transport. The parameters and the result are checked against the command's schemas; the command's events
		// are announced with this command's, and if it fails, this one fails with the reason. Commands that change
		// the project record their own undo step, so call only commands outside the document this way.
		Json call(const std::string& command, const Json& parameters = Json::object());

	private:
		friend class Engine;

		const Json& m_parameters;
		CommandContext* m_outer = nullptr;
		Engine* m_engine = nullptr; // set on the outermost context
		std::vector<Event> m_events;
	};

	using CommandHandler = std::function<Json(CommandContext& context)>;

	namespace detail
	{
		struct Mailbox;
	}

	// The only way into the engine. Commands run one at a time on the engine's command thread; clients (UI, MCP
	// server, Lua, tests) send them and receive results and events through a Client. Commands, results and events are
	// JSON, so the same API works in-process, over MCP and from Lua. Meters and the playback position do not go
	// through here: the audio thread writes them into lock-free buffers that clients read once per frame.
	class Engine
	{
	public:
		// The audio engine plays through devices of these backends and takes MIDI from the ports of these MIDI
		// backends. The user settings are read from and saved to the settings folder (files::settingsDirectory() is
		// the user's); without one, they are kept in memory only, as in tests.
		explicit Engine(std::vector<std::unique_ptr<audio::Backend>> backends = audio::makeBackends(),
			const std::filesystem::path& settingsDirectory = {},
			std::vector<std::unique_ptr<gnubase::midi::Backend>> midiBackends = gnubase::midi::makeBackends());
		~Engine();
		Engine(const Engine&) = delete;
		Engine& operator=(const Engine&) = delete;

		// Throws std::logic_error if the name is taken.
		void registerCommand(CommandInfo info, CommandHandler handler);

		// Queues a command from the engine's own threads, e.g. when an audio driver asks for a reset. Its events
		// reach every client; its result goes nowhere, and a failure is logged. Not for audio threads.
		void post(std::string command, Json parameters = Json::object());

		// The same, and done gets the command's result on the command thread, such as when a background job changes
		// the project with a command. Once the engine stops, done gets an error instead, for commands that didn't run.
		// done must be quick and must not call the engine.
		void post(std::string command, Json parameters, std::function<void(Result result)> done);

		// Announces an event from one of the engine's own threads, such as a background job's progress. Every client
		// receives it. Not for audio threads.
		void broadcast(std::string name, Json data = Json::object());

		// Every registered command, sorted by name. The engine.commands query returns the same as JSON.
		std::vector<CommandInfo> commands() const;

		// The project's render snapshots, published after every command that changes the project. The audio thread
		// is their only reader.
		rt::SnapshotPublisher<RenderSnapshot>& renderSnapshots();

		// The plugin host processes and the plugins that run in them.
		plugins::HostManager& pluginHosts();

		// The folder of the user settings; empty when they are kept in memory only. Features whose user settings
		// have files of their own, such as the quantize presets, keep them there.
		const std::filesystem::path& settingsDirectory() const { return m_settingsDirectory; }

		// The project cursor in samples, where the audio thread last moved it or a locate puts it, read without a
		// command and without waiting: for the UI to read once a frame while playing. Not for audio threads.
		int64_t playbackPosition() const;

	private:
		friend class Client;
		friend class CommandContext;
		friend class Listener;

		struct Command
		{
			CommandInfo info;
			CommandHandler handler;
		};

		struct Request
		{
			RequestId id = 0;
			std::string command;
			Json parameters;
			std::shared_ptr<detail::Mailbox> sender;
			std::function<void(Result result)> done; // for a posted command that wants its result
		};

		void run();
		void execute(Request& request);
		std::shared_ptr<const Command> find(const std::string& name) const; // throws CommandError if unknown
		Json runInside(const std::string& command, const Json& parameters, CommandContext& outer);

		const std::filesystem::path m_settingsDirectory;
		mutable std::mutex m_mutex; // guards everything below except the thread
		std::map<std::string, std::shared_ptr<const Command>> m_commands;
		std::deque<Request> m_requests;
		std::vector<std::shared_ptr<detail::Mailbox>> m_clients;
		std::deque<Json> m_commandLog; // the newest commands that aren't queries; only the command thread uses it
		RequestId m_nextRequest = 1;
		bool m_stopping = false;
		std::condition_variable m_wake;
		std::unique_ptr<Jobs> m_jobs;                    // background work such as backups
		std::unique_ptr<plugins::HostManager> m_plugins; // outlives the audio engine, whose graphs may hold plugins
		std::unique_ptr<ProjectModel> m_project;         // only the command thread touches the document
		std::unique_ptr<AudioEngine> m_audio;            // plays the project; only the command thread changes it
		std::unique_ptr<settings::Store> m_settings;     // the user settings; only the command thread uses them
		std::unique_ptr<peaks::PeakCache> m_peaks;       // waveform overviews of the project's media files
		std::thread m_thread;
	};

	// A connection to the engine. Receives the results of its own commands and every event, in order.
	// Clients must not outlive their engine.
	class Client
	{
	public:
		// The name tells the command log who sent a command: "ui", "mcp", "lua" and so on. arrived is called on the
		// thread that queues a message, whenever it queued one, e.g. to wake the UI thread; it must be quick, must not
		// block and must not call the engine.
		explicit Client(Engine& engine, std::string name = "client", std::function<void()> arrived = {});
		~Client();
		Client(const Client&) = delete;
		Client& operator=(const Client&) = delete;

		// Queues a command; its result arrives later as a message with the returned request id.
		RequestId send(std::string command, Json parameters = Json::object());

		// The next message, if there is one.
		std::optional<Message> poll();

		// The next message, waiting up to timeout for it.
		std::optional<Message> wait(std::chrono::milliseconds timeout);

		// Sends a command and waits for its result. Other messages stay queued for poll and wait.
		Result call(std::string command, Json parameters = Json::object(),
			std::chrono::milliseconds timeout = std::chrono::seconds(10));

	private:
		Engine& m_engine;
		std::shared_ptr<detail::Mailbox> m_mailbox;
	};

	// Calls a function for every event while it exists, instead of queueing events like a client: on the thread that
	// sends the event, before the command's result reaches its client. The function must be quick, must not block and
	// must not call the engine. For servers that hand events on to clients of their own, such as MCP sessions.
	// Listeners must not outlive their engine.
	class Listener
	{
	public:
		Listener(Engine& engine, std::function<void(const Event&)> function);
		~Listener();
		Listener(const Listener&) = delete;
		Listener& operator=(const Listener&) = delete;

	private:
		Engine& m_engine;
		std::shared_ptr<detail::Mailbox> m_mailbox;
	};
}
