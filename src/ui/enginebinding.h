// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"

#include <atomic>
#include <cstdint>
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

		// Calls handler in update() with every event of that name while the subscription exists.
		[[nodiscard]] Subscription on(std::string event, std::function<void(const engine::Event&)> handler);

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

		Subscription follow(std::function<void()> read);
		void end(uint64_t id) noexcept;
		void answer(const engine::Result& result);
		void dispatch(const engine::Event& event);

		std::atomic<bool> m_woken{false}; // woken was called and update() hasn't run since
		engine::Client m_client;
		std::map<engine::RequestId, Waiting> m_waiting; // commands sent, until their results arrive
		std::map<uint64_t, Handler> m_handlers;
		std::map<uint64_t, std::function<void()>> m_watches;
		uint64_t m_nextId = 1;
	};
}
