// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>

// The UI-engine binding (#50): user actions go to the engine as commands, and the UI's state follows the engine's
// events and, for values that change faster than the UI draws, what it reads once a frame. docs/features/
// ui-engine-binding.md has the reasoning.
namespace gnubase::ui
{
	class EngineBinding;

	// A handler that the binding calls while the subscription exists. Subscriptions must not outlive their binding.
	class Subscription
	{
	public:
		Subscription(Subscription&& other) noexcept;
		Subscription& operator=(Subscription&& other) noexcept;
		Subscription(const Subscription&) = delete;
		Subscription& operator=(const Subscription&) = delete;
		~Subscription();

	private:
		friend class EngineBinding;

		Subscription(EngineBinding& binding, uint64_t id) : m_binding(&binding), m_id(id) {}
		void end() noexcept;

		EngineBinding* m_binding = nullptr;
		uint64_t m_id = 0;
	};

	// The UI's connection to the engine, used on the UI thread only. It goes through an engine client named "ui",
	// so the UI can do nothing that MCP and Lua can't, and its commands show in the command log (engine.commandLog).
	class EngineBinding
	{
	public:
		// woken is called on the engine's thread when results or events start to wait for update(), once until
		// update() runs, e.g. Shell::wake. It must be quick, must not block and must not call the engine or the
		// binding.
		explicit EngineBinding(engine::Engine& engine, std::function<void()> woken = {});

		// Sends a command. done runs in update() with its result; without done, a failure is logged.
		void send(std::string command, engine::Json parameters = engine::Json::object(),
			std::function<void(const engine::Result&)> done = {});

		// Sends document commands ({"command", "parameters"} each) as a step of a value widget's gesture (the
		// "gesture" parameter of its change event, ui/widgets.h): a history.transaction named description whose merge
		// key is what the gesture changes (e.g. "channel 5 volume") and the gesture, so that a drag, wheel turns or a
		// held arrow key are one undo step. With gesture 0 the commands are a step of their own.
		// The steps of one thing go one at a time (#716): while one is in flight the next waits, and a newer step of
		// the same gesture replaces the one waiting, so that a fast drag sends its newest value whenever the engine
		// has answered instead of piling up commands. done runs for every step sent; a step replaced before it was
		// sent is dropped with its done.
		void sendStep(int gesture, const std::string& what, std::string description, engine::Json commands,
			std::function<void(const engine::Result&)> done = {});

		// Whether steps of what (sendStep) are in flight or waiting. Until they aren't, the engine's values for what
		// are older than the control that sent them, so a view leaves the control as it is (#716); the engine's
		// events for the last step, and its value then, come after.
		bool sending(const std::string& what) const { return m_steps.count(what) != 0; }

		// How many commands of that name were sent and their results not handed on yet, e.g. for tests of how many
		// queries a view keeps open.
		size_t waiting(const std::string& command) const;

		// Calls handler in update() with every event of that name while the subscription exists.
		[[nodiscard]] Subscription on(std::string event, std::function<void(const engine::Event&)> handler);

		// Calls handler in update() with the result of every command of that name sent through the binding, after
		// the sender's done, while the subscription exists: e.g. to show what a command run from a menu did.
		[[nodiscard]] Subscription onResult(std::string command, std::function<void(const engine::Result&)> handler);

		// Calls apply in update() with what read returns, the first time and whenever it changed since: for values
		// that change faster than the UI draws, such as the playback position and meters, which the UI reads once a
		// frame instead of getting an event for every change.
		template <typename T>
		[[nodiscard]] Subscription watch(std::function<T()> read, std::function<void(const T&)> apply)
		{
			return follow(
				[read = std::move(read), apply = std::move(apply), last = std::optional<T>()]() mutable
				{
					T value = read();
					if (last && *last == value)
						return;
					last = value;
					apply(value);
				});
		}

		// Hands the results and events that arrived to their handlers, in order, then reads the watched values. The
		// shell calls it in every step (Shell::eachStep). Returns whether it watches values, so that the shell keeps
		// stepping once a frame while it does.
		bool update();

	private:
		friend class Subscription;

		struct Handler
		{
			std::string event;
			std::function<void(const engine::Event&)> call;
		};

		struct Waiting
		{
			std::string command;
			std::function<void(const engine::Result&)> done;
		};

		// A step of sendStep, until it is sent.
		struct Step
		{
			std::string merge;
			std::string description;
			engine::Json commands;
			std::function<void(const engine::Result&)> done;
		};

		// The steps of one thing: whether one is in flight and its done, and those that wait for it.
		struct Steps
		{
			bool inFlight = false;
			std::function<void(const engine::Result&)> done;
			std::deque<Step> waiting;
		};

		Subscription follow(std::function<void()> read);
		void end(uint64_t id) noexcept;
		void answer(const engine::Result& result);
		void dispatch(const engine::Event& event);
		void sendNextStep(const std::string& what); // or forgets what if no step waits

		std::atomic<bool> m_woken{false}; // woken was called and update() hasn't run since
		engine::Client m_client;
		std::map<engine::RequestId, Waiting> m_waiting; // commands sent, until their results arrive
		std::map<uint64_t, Handler> m_handlers;
		std::map<uint64_t, Waiting> m_resultHandlers; // onResult's, by subscription
		std::map<uint64_t, std::function<void()>> m_watches;
		std::map<std::string, Steps> m_steps; // sendStep's, while one is in flight
		uint64_t m_nextId = 1;
	};
}
