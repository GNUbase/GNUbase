// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/enginebinding.h"

#include "platform/log.h"

#include <RmlUi/Core/Context.h>

#include <algorithm>
#include <utility>
#include <variant>
#include <vector>

namespace gnubase::ui
{
	Subscription::Subscription(Subscription&& other) noexcept
		: m_binding(std::exchange(other.m_binding, nullptr)), m_id(other.m_id)
	{
	}

	Subscription& Subscription::operator=(Subscription&& other) noexcept
	{
		if (this != &other)
		{
			end();
			m_binding = std::exchange(other.m_binding, nullptr);
			m_id = other.m_id;
		}
		return *this;
	}

	Subscription::~Subscription()
	{
		end();
	}

	void Subscription::end() noexcept
	{
		if (m_binding != nullptr)
			m_binding->end(m_id);
		m_binding = nullptr;
	}

	EngineBinding::EngineBinding(engine::Engine& engine, std::function<void()> woken, std::function<void()> handled)
		: m_client(engine, "ui",
			  [this, woken = std::move(woken)]
			  {
				  if (woken && !m_woken.exchange(true))
					  woken();
			  }),
		  m_handled(std::move(handled))
	{
		for (const engine::CommandInfo& info : engine.commands())
			if (info.query)
				m_queries.insert(info.name);
	}

	engine::RequestId EngineBinding::send(
		std::string command, engine::Json parameters, std::function<void(const engine::Result&)> done)
	{
		sendWaitingSteps(command);
		return transmit(std::move(command), std::move(parameters), std::move(done), false);
	}

	void EngineBinding::sendQuietly(
		std::string command, engine::Json parameters, std::function<void(const engine::Result&)> done)
	{
		sendWaitingSteps(command);
		transmit(std::move(command), std::move(parameters), std::move(done), true);
	}

	void EngineBinding::sendWaitingSteps(const std::string& before)
	{
		// A command that may change something goes after the steps the user made before it, e.g. an undo right after a
		// drag undoes all of the drag.
		if (m_queries.count(before) == 0)
			for (auto& [what, steps] : m_steps)
				while (!steps.waiting.empty())
					sendNextStep(what);
	}

	engine::RequestId EngineBinding::transmit(
		std::string command, engine::Json parameters, std::function<void(const engine::Result&)> done, bool quiet)
	{
		std::string name = command;
		const engine::RequestId id = m_client.send(std::move(command), std::move(parameters));
		m_waiting.emplace(id, Waiting{std::move(name), std::move(done), quiet});
		return id;
	}

	void EngineBinding::sendStep(int gesture, const std::string& what, std::string description, engine::Json commands,
		std::function<void(const engine::Result&)> done)
	{
		Step step{gesture != 0 ? what + " " + std::to_string(gesture) : std::string(), std::move(description),
			std::move(commands), std::move(done)};
		Steps& steps = m_steps[what];
		if (!step.merge.empty() && !steps.waiting.empty() && steps.waiting.back().merge == step.merge)
			steps.waiting.back() = std::move(step); // the newer value of the same gesture
		else
			steps.waiting.push_back(std::move(step));
		if (steps.inFlight.empty())
			sendNextStep(what);
	}

	void EngineBinding::sendNextStep(const std::string& what)
	{
		const auto entry = m_steps.find(what);
		Steps& steps = entry->second;
		Step step = std::move(steps.waiting.front());
		steps.waiting.pop_front();
		steps.inFlight.push_back(std::move(step.done));
		// The map's entries stay for the binding's life, so the key outlives the answer.
		const std::string* key = &entry->first;
		transmit(
			"history.transaction",
			{{"description", std::move(step.description)}, {"merge", std::move(step.merge)},
				{"commands", std::move(step.commands)}},
			[this, key](const engine::Result& result) { answerStep(*key, result); }, false);
	}

	void EngineBinding::answerStep(const std::string& what, const engine::Result& result)
	{
		std::function<void(const engine::Result&)> done;
		if (const auto steps = m_steps.find(what); steps != m_steps.end() && !steps->second.inFlight.empty())
		{
			done = std::move(steps->second.inFlight.front()); // results come in the order the steps went
			steps->second.inFlight.pop_front();
			if (result.ok())
				steps->second.taken = m_handed;
		}
		if (done)
			done(result);
		else if (!result.ok())
			logging::warning("history.transaction from the UI failed: {}", result.error);
		// done may have sent the next step itself.
		if (const auto steps = m_steps.find(what);
			steps != m_steps.end() && steps->second.inFlight.empty() && !steps->second.waiting.empty())
			sendNextStep(what);
	}

	bool EngineBinding::sending(const std::string& what) const
	{
		const auto steps = m_steps.find(what);
		return steps != m_steps.end() && (!steps->second.inFlight.empty() || !steps->second.waiting.empty());
	}

	bool EngineBinding::ahead(const std::string& what, uint64_t read) const
	{
		const auto steps = m_steps.find(what);
		return sending(what) || (steps != m_steps.end() && steps->second.taken > read);
	}

	size_t EngineBinding::waiting(const std::string& command) const
	{
		return static_cast<size_t>(std::count_if(m_waiting.begin(), m_waiting.end(),
			[&command](const auto& waiting) { return waiting.second.command == command; }));
	}

	Subscription EngineBinding::on(std::string event, std::function<void(const engine::Event&)> handler)
	{
		const uint64_t id = m_nextId++;
		m_handlers.emplace(id, Handler{std::move(event), std::move(handler)});
		return {*this, id};
	}

	Subscription EngineBinding::onResult(std::string command, std::function<void(const engine::Result&)> handler)
	{
		const uint64_t id = m_nextId++;
		m_resultHandlers.emplace(id, Waiting{std::move(command), std::move(handler), false});
		return {*this, id};
	}

	Subscription EngineBinding::follow(std::function<void()> read)
	{
		const uint64_t id = m_nextId++;
		m_watches.emplace(id, std::move(read));
		return {*this, id};
	}

	void EngineBinding::end(uint64_t id) noexcept
	{
		m_handlers.erase(id);
		m_resultHandlers.erase(id);
		m_watches.erase(id);
	}

	bool EngineBinding::update()
	{
		m_woken = false; // before taking the messages, so one that arrives meanwhile wakes the UI again
		bool handled = false;
		while (std::optional<engine::Message> message = m_client.poll())
		{
			if (const auto* result = std::get_if<engine::Result>(&*message))
				handled = answer(*result) || handled;
			else
				handled = dispatch(std::get<engine::Event>(*message)) || handled;
		}
		if (handled && m_handled)
			m_handled();
		// Each function is taken out while it runs, since it may end its own subscription or others.
		std::vector<uint64_t> ids;
		ids.reserve(m_watches.size());
		for (const auto& [id, read] : m_watches)
			ids.push_back(id);
		for (const uint64_t id : ids)
		{
			auto found = m_watches.find(id);
			if (found == m_watches.end())
				continue;
			std::function<void()> read = std::move(found->second);
			read();
			if (found = m_watches.find(id); found != m_watches.end())
				found->second = std::move(read);
		}
		return !m_watches.empty();
	}

	bool EngineBinding::answer(const engine::Result& result)
	{
		++m_handed;
		const auto found = m_waiting.find(result.request);
		if (found == m_waiting.end())
			return false;
		const Waiting waiting = std::move(found->second);
		m_waiting.erase(found);
		if (waiting.done)
			waiting.done(result);
		else if (!result.ok())
			logging::warning("{} from the UI failed: {}", waiting.command, result.error);
		// As in dispatch, each handler is taken out while it runs.
		std::vector<uint64_t> ids;
		for (const auto& [id, handler] : m_resultHandlers)
			if (handler.command == waiting.command)
				ids.push_back(id);
		const bool handled = (waiting.done && !waiting.quiet) || !ids.empty();
		for (const uint64_t id : ids)
		{
			auto handler = m_resultHandlers.find(id);
			if (handler == m_resultHandlers.end())
				continue;
			std::function<void(const engine::Result&)> call = std::move(handler->second.done);
			call(result);
			if (handler = m_resultHandlers.find(id); handler != m_resultHandlers.end())
				handler->second.done = std::move(call);
		}
		return handled;
	}

	bool EngineBinding::dispatch(const engine::Event& event)
	{
		++m_handed;
		std::vector<uint64_t> ids;
		for (const auto& [id, handler] : m_handlers)
			if (handler.event == event.name)
				ids.push_back(id);
		for (const uint64_t id : ids)
		{
			auto found = m_handlers.find(id);
			if (found == m_handlers.end())
				continue;
			std::function<void(const engine::Event&)> call = std::move(found->second.call);
			call(event);
			if (found = m_handlers.find(id); found != m_handlers.end())
				found->second.call = std::move(call);
		}
		return !ids.empty();
	}

	void EngineBinding::redraw(Rml::Element* shows)
	{
		if (shows == nullptr)
			return;
		if (Rml::Context* context = shows->GetContext())
			context->RequestNextUpdate(0);
	}
}
