// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"

#include <RmlUi/Core/Element.h>

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
		// binding. handled is called in update(), once, when results or events reached handlers there (a command's
		// done, onResult's or on's handlers): they may have changed what any window shows, so the app draws every
		// window then (Shell::requestRender).
		explicit EngineBinding(
			engine::Engine& engine, std::function<void()> woken = {}, std::function<void()> handled = {});

		// Sends a command. done runs in update() with its result; without done, a failure is logged.
		void send(std::string command, engine::Json parameters = engine::Json::object(),
			std::function<void(const engine::Result&)> done = {});

		// Sends a command, like send, for a view that asks again and again, e.g. once a second: done's run doesn't
		// make every window draw, so done asks for a draw itself when it changed what an element shows
		// (Rml::Context::RequestNextUpdate(0)), and an idle UI stays idle. onResult's handlers still draw every window.
		void sendQuietly(std::string command, engine::Json parameters, std::function<void(const engine::Result&)> done);

		// Sends document commands ({"command", "parameters"} each) as a step of a value widget's gesture (the
		// "gesture" parameter of its change event, ui/widgets.h): a history.transaction named description whose merge
		// key is what the gesture changes (e.g. "channel 5 volume") and the gesture, so that a drag, wheel turns or a
		// held arrow key are one undo step. With gesture 0 the commands are a step of their own.
		void sendStep(int gesture, const std::string& what, std::string description, engine::Json commands,
			std::function<void(const engine::Result&)> done = {});

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
		// frame instead of getting an event for every change. shows is the element that shows the value: after apply,
		// the window it is in draws in that step, and no other window does for it. With null nothing draws for it, so
		// apply must ask for a draw itself if it changes an element (Rml::Context::RequestNextUpdate(0)).
		template <typename T> [[nodiscard]] Subscription watch(
			Rml::Element* shows, std::function<T()> read, std::function<void(const T&)> apply)
		{
			return follow(
				[shows = shows != nullptr ? shows->GetObserverPtr() : Rml::ObserverPtr<Rml::Element>(),
					read = std::move(read), apply = std::move(apply), last = std::optional<T>()]() mutable
				{
					T value = read();
					if (last && *last == value)
						return;
					last = value;
					apply(value);
					redraw(shows.get());
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
			bool quiet; // done draws what it changed itself (sendQuietly)
		};

		Subscription follow(std::function<void()> read);
		void end(uint64_t id) noexcept;
		// Each returns whether handlers took the message.
		bool answer(const engine::Result& result);
		bool dispatch(const engine::Event& event);
		// Draws the element's window in the shell's step (Rml::Context::RequestNextUpdate).
		static void redraw(Rml::Element* shows);

		std::atomic<bool> m_woken{false}; // woken was called and update() hasn't run since
		engine::Client m_client;
		std::function<void()> m_handled;
		std::map<engine::RequestId, Waiting> m_waiting; // commands sent, until their results arrive
		std::map<uint64_t, Handler> m_handlers;
		std::map<uint64_t, Waiting> m_resultHandlers; // onResult's, by subscription
		std::map<uint64_t, std::function<void()>> m_watches;
		uint64_t m_nextId = 1;
	};
}
