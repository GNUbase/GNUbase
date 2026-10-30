// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/enginebinding.h"

#include "platform/log.h"

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

	EngineBinding::EngineBinding(engine::Engine& engine, std::function<void()> woken)
		: m_client(engine, "ui", std::move(woken))
	{
	}

	void EngineBinding::send(
		std::string command, engine::Json parameters, std::function<void(const engine::Result&)> done)
	{
		std::string name = command;
		const engine::RequestId id = m_client.send(std::move(command), std::move(parameters));
		m_waiting.emplace(id, Waiting{std::move(name), std::move(done)});
	}

	Subscription EngineBinding::on(std::string event, std::function<void(const engine::Event&)> handler)
	{
		const uint64_t id = m_nextId++;
		m_handlers.emplace(id, Handler{std::move(event), std::move(handler)});
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
		m_watches.erase(id);
	}

	bool EngineBinding::update()
	{
		while (std::optional<engine::Message> message = m_client.poll())
		{
			if (const auto* result = std::get_if<engine::Result>(&*message))
				answer(*result);
			else
				dispatch(std::get<engine::Event>(*message));
		}
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

	void EngineBinding::answer(const engine::Result& result)
	{
		const auto found = m_waiting.find(result.request);
		if (found == m_waiting.end())
			return;
		const Waiting waiting = std::move(found->second);
		m_waiting.erase(found);
		if (waiting.done)
			waiting.done(result);
		else if (!result.ok())
			logging::warning("{} from the UI failed: {}", waiting.command, result.error);
	}

	void EngineBinding::dispatch(const engine::Event& event)
	{
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
	}
}
