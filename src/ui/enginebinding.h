// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"

#include <RmlUi/Core/Element.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
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

		// Sends a command. done runs in update() with its result; without done, a failure is logged. A command that
		// isn't a query sends the steps that wait (sendStep) first, so that it comes after them, as the user did them.
		// Returns its request id, which its events carry.
		engine::RequestId send(std::string command, engine::Json parameters = engine::Json::object(),
			std::function<void(const engine::Result&)> done = {});

		// Sends a command, like send, for a view that asks again and again, e.g. once a second: done's run doesn't
		// make every window draw, so done asks for a draw itself when it changed what an element shows
		// (Rml::Context::RequestNextUpdate(0)), and an idle UI stays idle. onResult's handlers still draw every window.
		void sendQuietly(std::string command, engine::Json parameters, std::function<void(const engine::Result&)> done);

		// Sends a query, like send, that what the user does next waits for (afterAwaited): e.g. snap.zeroCrossings,
		// which moves a selection range's edges before an edit of the range runs (#1086). settle waits for its result,
		// and for what its done sends, as for a command that isn't a query.
		engine::RequestId sendAwaited(
			std::string command, engine::Json parameters, std::function<void(const engine::Result&)> done);

		// Runs run now, or while queries sent with sendAwaited are out, once they are answered, after their dones: what
		// the user does goes in the order they did it, e.g. Delete right after a range is drawn, and an undo after it.
		// What a held one runs itself runs at once, unless an awaited query is out again.
		void afterAwaited(std::function<void()> run);
		// Whether afterAwaited holds anything.
		bool holding() const { return !m_held.empty(); }
		// Runs what afterAwaited holds now, without waiting for the answers: e.g. once awaitHeld timed out.
		void releaseHeld();
		// Hands on results and events, as update() does, until afterAwaited holds nothing, for at most timeout; unlike
		// settle, it doesn't wait for other commands out, such as those that the held ones send. Returns false if
		// something is still held then.
		bool awaitHeld(std::chrono::milliseconds timeout);

		// Sends document commands ({"command", "parameters"} each) as a step of a value widget's gesture (the
		// "gesture" parameter of its change event, ui/widgets.h): a history.transaction named description whose merge
		// key is what the gesture changes (e.g. "channel 5 volume") and the gesture, so that a drag, wheel turns or a
		// held arrow key are one undo step. With gesture 0 the commands are a step of their own.
		// The steps of one thing go one at a time (#716): while one is in flight the next waits, and a newer step of
		// the same gesture replaces the one waiting, so that a fast drag sends its newest value whenever the engine
		// has answered instead of piling up commands. done runs for every step sent; a step replaced before it was
		// sent is dropped with its done.
		// shown, unless null, is what the control shows once the engine took the step, kept with the record of what's
		// steps for shown(): an object's members are merged into what the record keeps (a JSON merge patch), e.g. one
		// per event that the step changes, anything else replaces it.
		// merge, unless empty, names the merge key instead of what, for steps whose record what is shared by things
		// that undo apart, such as the info line's fields for each selection of events.
		void sendStep(int gesture, const std::string& what, std::string description, engine::Json commands,
			std::function<void(const engine::Result&)> done = {}, const engine::Json& shown = nullptr,
			const std::string& merge = {});

		// Whether steps of what (sendStep) are in flight or waiting. Until they aren't, the engine's values for what
		// are older than the control that sent them, so a view leaves the control as it is (#716); the engine's
		// events for the last step, and its value then, come after.
		bool sending(const std::string& what) const;

		// How many results and events update() has handed on, counting the one being handed on: a view that shows
		// what an answer said later, rather than in its handler, keeps this with it to ask ahead().
		uint64_t handed() const { return m_handed; }

		// Whether the control that sends the steps of what is ahead of the engine's answer that update() handed on as
		// handed() returned read: a step of it is on its way, or the last one that the engine took was answered after
		// that answer. A refused step changes nothing, so it doesn't count. Once what's last step is answered and a
		// project read (project.get) answered after it was handed on, such as the one after its history.changed, and no
		// other is out, its record goes away in the next update(), so it is ahead of no answer from then on; a failed
		// read doesn't count. So records go only while some view reads the project, as the event display does: a
		// binding without one keeps them for the session, and a view that asks about another query's answer may find
		// the record gone before that query answers again.
		bool ahead(const std::string& what, uint64_t read) const;

		// What the steps of what showed (sendStep's shown) while the control is ahead of read, else null: a view shows
		// it instead of the older value that read has, also after it showed another channel or event in the same
		// control meanwhile (#1421). It goes with the record, so a session keeps none for every control it touched
		// (#1471). Valid until the next sendStep or update().
		const engine::Json* shown(const std::string& what, uint64_t read) const;

		// A name for one view to begin the whats of its steps with, e.g. "mixer 3 ", a new one for each call. Views
		// that show the same value, such as two mixers and the inspector, send their steps under names of their own,
		// so only the view whose control sends leaves that control as it is, and the others show each step the engine
		// took (#716).
		std::string stepOwner(const std::string& view) { return view + " " + std::to_string(++m_owners) + " "; }

		// How many commands of that name were sent and their results not handed on yet, e.g. for tests of how many
		// queries a view keeps open.
		size_t waiting(const std::string& command) const;

		// Sends the steps that wait (sendStep) and hands on results and events, as update() does, until no command that
		// isn't a query, nor a query sent with sendAwaited, is out, including those that the dones send: e.g. so that
		// what a user script ran through the UI is in the script's undo step (docs/features/scripts.md). Returns false
		// if they are still out after the timeout.
		bool settle(std::chrono::milliseconds timeout);

		// Calls handler in update() with every event of that name while the subscription exists.
		[[nodiscard]] Subscription on(std::string event, std::function<void(const engine::Event&)> handler);

		// Calls handler in update() with the result of every command of that name sent through the binding, after
		// the sender's done, while the subscription exists: e.g. to show what a command run from a menu did.
		[[nodiscard]] Subscription onResult(std::string command, std::function<void(const engine::Result&)> handler);

		// Calls apply in update() with what read returns, the first time and whenever it changed since: for values
		// that change faster than the UI draws, such as the playback position and meters, which the UI reads once a
		// frame instead of getting an event for every change. shows is the element that shows the value: after apply,
		// the window it is in draws in that step, and no other window does for it. An apply that returns bool tells
		// whether it changed what an element shows, and the window draws only then: e.g. a time display whose text
		// changes less often than the position it shows. With null nothing draws for it, so apply must ask for a draw
		// itself if it changes an element (Rml::Context::RequestNextUpdate(0)).
		template <typename T, typename Apply>
		[[nodiscard]] Subscription watch(Rml::Element* shows, std::function<T()> read, Apply apply)
		{
			using Returns = std::invoke_result_t<Apply&, const T&>;
			static_assert(std::is_void_v<Returns> || std::is_same_v<Returns, bool>, "apply returns nothing or bool");
			return follow(
				[shows = shows != nullptr ? shows->GetObserverPtr() : Rml::ObserverPtr<Rml::Element>(),
					read = std::move(read), apply = std::move(apply), last = std::optional<T>()]() mutable
				{
					T value = read();
					if (last && *last == value)
						return;
					last = value;
					if constexpr (std::is_same_v<Returns, bool>)
					{
						if (apply(std::as_const(value)))
							redraw(shows.get());
					}
					else
					{
						apply(std::as_const(value));
						redraw(shows.get());
					}
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
			bool quiet;   // done draws what it changed itself (sendQuietly)
			bool awaited; // settle waits for it (sendAwaited)
		};

		// A step of sendStep, until it is sent.
		struct Step
		{
			std::string merge;
			std::string description;
			engine::Json commands;
			std::function<void(const engine::Result&)> done;
		};

		// The steps of one thing: the dones of those in flight, in order, those that wait, and when the engine last
		// took one. A record with nothing in flight or waiting goes away in the update() after a project read answered
		// after taken, once no other is out.
		struct Steps
		{
			std::deque<std::function<void(const engine::Result&)>> inFlight;
			std::deque<Step> waiting;
			uint64_t taken = 0; // m_handed when its result was handed on
			engine::Json shown; // sendStep's, merged
		};

		Subscription follow(std::function<void()> read);
		void end(uint64_t id) noexcept;
		// Each returns whether handlers took the message.
		bool take(const engine::Message& message);
		bool answer(const engine::Result& result);
		bool dispatch(const engine::Event& event);
		// Draws the element's window in the shell's step (Rml::Context::RequestNextUpdate).
		static void redraw(Rml::Element* shows);
		void sendWaitingSteps(const std::string& before);
		engine::RequestId transmit(std::string command, engine::Json parameters,
			std::function<void(const engine::Result&)> done, bool quiet, bool awaited = false);
		void sendNextStep(const std::string& what);
		void answerStep(const std::string& what, const engine::Result& result);
		// Hands on results and events until done returns true, for at most timeout; returns done().
		bool handOnUntil(std::chrono::milliseconds timeout, const std::function<bool()>& done);
		// Runs what afterAwaited holds while no awaited query is out (all of it with force); returns whether any ran.
		bool runHeld(bool force);

		std::atomic<bool> m_woken{false}; // woken was called and update() hasn't run since
		engine::Client m_client;
		std::function<void()> m_handled;
		std::map<engine::RequestId, Waiting> m_waiting; // commands sent, until their results arrive
		std::map<uint64_t, Handler> m_handlers;
		std::map<uint64_t, Waiting> m_resultHandlers; // onResult's, by subscription
		std::map<uint64_t, std::function<void()>> m_watches;
		std::map<std::string, Steps> m_steps;     // sendStep's, by what they change; update() erases drained entries
		std::set<std::string> m_queries;          // the engine's commands that only read
		size_t m_awaited = 0;                     // queries sent with sendAwaited and not answered yet
		std::deque<std::function<void()>> m_held; // afterAwaited's, in order
		bool m_runningHeld = false;               // runHeld runs one
		uint64_t m_handed = 0;
		uint64_t m_projectRead = 0; // m_handed when the last successful project.get's answer was handed on
		uint64_t m_owners = 0;      // stepOwner's
		uint64_t m_nextId = 1;
	};
}
