// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/plugins/slotinstances.h"

#include "engine/plugins/hostmanager.h"
#include "engine/processors/builtinplugin.h"
#include "platform/log.h"
#include "platform/rt/realtime.h"

#include <chrono>
#include <exception>
#include <future>
#include <map>
#include <set>
#include <string_view>
#include <thread>
#include <utility>

namespace gnubase::engine::plugins
{
	namespace
	{
		// How often a job checks whether the audio thread left the graphs that still have a plugin.
		constexpr auto g_retirePoll = std::chrono::milliseconds(1);

		enum class Action : uint8_t
		{
			Deactivate,
			Reactivate,
			Destroy,
		};
	}

	namespace
	{
		struct Made
		{
			std::shared_ptr<PluginSlot> processor;
			std::string status;
		};

		// The values set for a slot's parameters that its plugin gets through PluginSlot::setParameters. A built-in
		// plugin has an automation lane for every parameter, which sends the values set as its static values
		// (channelAutomation), so they take that one way, in step with its automation.
		const std::map<uint32_t, double>& setParameters(const RenderSlot& slot)
		{
			static const std::map<uint32_t, double> none;
			return processors::findBuiltinPlugin(slot.plugin) != nullptr ? none : slot.parameters;
		}

		std::unique_ptr<plugin::Plugin> makeBuiltin(const std::string& plugin, uint32_t channels, plugin::Host& host)
		{
			return processors::BuiltinFactory(channels).create(plugin, host);
		}

		// [plugin job thread] Makes a slot's plugin and activates it if the slot wants it active.
		Made make(const SlotInstances::Factory& factory, plugin::Host& host, const RenderSlot& wanted,
			uint32_t channels, double sampleRate, uint32_t maxFrames)
		{
			std::unique_ptr<plugin::Plugin> plugin = factory(wanted.plugin, channels, host);
			if (!plugin)
				return {nullptr, "unavailable"};
			if (!plugin->init())
			{
				logging::warning("Plugin slot {}: {} doesn't initialize", wanted.id, wanted.plugin);
				return {nullptr, "failed"};
			}
			auto processor = std::make_shared<PluginSlot>(std::move(plugin));
			if (wanted.state && !processor->loadState(*wanted.state))
				logging::warning("Plugin slot {}: {} doesn't take its saved state", wanted.id, wanted.plugin);
			processor->setParameters(setParameters(wanted));
			processor->setBypass(wanted.bypass);
			if (!wanted.active)
				return {std::move(processor), "inactive"};
			const bool active = sampleRate > 0.0 && processor->activate(sampleRate, maxFrames);
			return {std::move(processor), active ? "active" : "failed"};
		}
	}

	struct SlotInstances::Shared
	{
		HostManager& jobs;
		std::function<void()> changed;
		std::function<bool(uint64_t)> retired;
		Factory factory;
		std::atomic<bool> closing{false};
		std::atomic<int> retiring{0}; // whenRetired jobs that haven't run their work

		Shared(
			HostManager& manager, std::function<void()> onChange, std::function<bool(uint64_t)> isRetired, Factory make)
			: jobs(manager), changed(std::move(onChange)), retired(std::move(isRetired)),
			  factory(make ? std::move(make) : Factory(makeBuiltin))
		{
		}
	};

	// What a slot's plugin asks of the engine. A restart takes the plugin out of the graph, deactivates it and
	// activates it again.
	class SlotInstances::EngineHost final : public plugin::Host
	{
	public:
		EngineHost(std::shared_ptr<Shared> shared, std::shared_ptr<std::atomic<bool>> restart)
			: m_shared(std::move(shared)), m_restart(std::move(restart))
		{
		}

		void requestRestart() override
		{
			m_restart->store(true);
			// Posting a command isn't real-time safe; a request from an audio thread waits for the next graph build.
			// Plugins in host processes ask on the IPC thread.
			if (!m_shared->closing && !rt::isRealtimeContext())
				m_shared->changed();
		}
		void requestProcess() override {}
		void requestCallback() override {}
		void* extension(std::string_view /*id*/) override { return nullptr; }

	private:
		std::shared_ptr<Shared> m_shared;
		std::shared_ptr<std::atomic<bool>> m_restart;
	};

	struct SlotInstances::Instance
	{
		std::string plugin;
		uint32_t channels = 2;
		std::shared_ptr<const std::vector<uint8_t>> state; // what it was made with
		std::shared_ptr<std::atomic<bool>> restart = std::make_shared<std::atomic<bool>>(false);
		std::shared_ptr<EngineHost> host; // the plugin's, outlives it

		// Set by jobs, read by the command thread.
		mutable std::mutex mutex;
		std::shared_ptr<PluginSlot> processor;
		std::string status = "loading";

		// Command thread.
		bool inGraph = false;
		bool leaving = false;            // waits for retire
		Action action = Action::Destroy; // what retire does with it
		double triedRate = 0.0;          // what it was last activated for: a plugin that failed isn't tried again
		uint32_t triedFrames = 0;

		void set(Made made)
		{
			const std::scoped_lock lock(mutex);
			processor = std::move(made.processor);
			status = std::move(made.status);
		}
		void setStatus(std::string value)
		{
			const std::scoped_lock lock(mutex);
			status = std::move(value);
		}
		std::shared_ptr<PluginSlot> currentProcessor() const
		{
			const std::scoped_lock lock(mutex);
			return processor;
		}
		// [plugin job thread] Does what retire marked it for.
		void apply(Action what, double sampleRate, uint32_t maxFrames)
		{
			const std::shared_ptr<PluginSlot> current = currentProcessor();
			if (!current)
				return;
			try
			{
				switch (what)
				{
				case Action::Destroy:
					current->destroy();
					set({nullptr, "unavailable"});
					break;
				case Action::Deactivate:
					current->deactivate();
					set({current, "inactive"});
					break;
				case Action::Reactivate:
					current->deactivate();
					set({current, sampleRate > 0.0 && current->activate(sampleRate, maxFrames) ? "active" : "failed"});
					break;
				}
			}
			catch (const std::exception& error)
			{
				logging::warning("Plugin slot: changing its plugin failed: {}", error.what());
				set({current, "failed"});
			}
		}
	};

	SlotInstances::SlotInstances(HostManager& jobs, std::function<void()> changed,
		std::function<bool(uint64_t generation)> retired, Factory factory)
		: m_shared(std::make_shared<Shared>(jobs, std::move(changed), std::move(retired), std::move(factory)))
	{
	}

	SlotInstances::~SlotInstances()
	{
		m_shared->closing = true;
		try
		{
			std::vector<std::shared_ptr<Instance>> instances = m_leaving;
			for (const auto& [slot, instance] : m_instances)
				instances.push_back(instance);
			for (const std::shared_ptr<Instance>& instance : instances)
				post(
					[instance]
					{
						const std::shared_ptr<PluginSlot> processor = instance->currentProcessor();
						if (processor)
							processor->destroy();
					});
			// Done once the jobs before an empty one are, and the retiring ones that queued themselves again.
			do
				m_shared->jobs.run([] {}).get();
			while (m_shared->retiring > 0);
		}
		catch (const std::exception& error)
		{
			logging::warning("Plugin slots: destroying their plugins failed: {}", error.what());
		}
	}

	void SlotInstances::post(std::function<void()> job)
	{
		// The future isn't needed: a job reports through its instance.
		static_cast<void>(m_shared->jobs.run(std::move(job)));
	}

	void SlotInstances::start(const std::shared_ptr<Instance>& instance, const Wanted& wanted)
	{
		const std::shared_ptr<Shared> shared = m_shared;
		const double sampleRate = m_sampleRate;
		const uint32_t maxFrames = m_maxFrames;
		post(
			[shared, instance, slot = std::make_shared<const RenderSlot>(*wanted.slot), channels = wanted.channels,
				sampleRate, maxFrames]
			{
				if (shared->closing)
					return;
				try
				{
					instance->set(make(shared->factory, *instance->host, *slot, channels, sampleRate, maxFrames));
				}
				catch (const std::exception& error)
				{
					logging::warning("Plugin slot {}: {} failed: {}", slot->id, slot->plugin, error.what());
					instance->set({nullptr, "failed"});
				}
				if (!shared->closing)
					shared->changed();
			});
	}

	void SlotInstances::update(const std::vector<Wanted>& slots, double sampleRate, uint32_t maxFrames)
	{
		m_sampleRate = sampleRate;
		m_maxFrames = maxFrames;
		std::set<uint64_t> wanted;
		for (const Wanted& want : slots)
		{
			const RenderSlot& slot = *want.slot;
			wanted.insert(slot.id);
			std::shared_ptr<Instance>& instance = m_instances[slot.id];
			if (instance && instance->leaving)
				continue; // retire comes first
			if (instance &&
				(instance->plugin != slot.plugin || instance->channels != want.channels ||
					instance->state != slot.state))
			{
				// Another plugin, the channel's format changed or another state, e.g. from another file: a new
				// instance.
				instance->action = Action::Destroy;
				m_leaving.push_back(instance);
				instance.reset();
			}
			if (!instance)
			{
				instance = std::make_shared<Instance>();
				instance->plugin = slot.plugin;
				instance->channels = want.channels;
				instance->state = slot.state;
				instance->host = std::make_shared<EngineHost>(m_shared, instance->restart);
				instance->triedRate = sampleRate;
				instance->triedFrames = maxFrames;
				start(instance, want);
				continue;
			}

			std::shared_ptr<PluginSlot> processor;
			std::string status;
			{
				const std::scoped_lock lock(instance->mutex);
				processor = instance->processor;
				status = instance->status;
			}
			if (status == "loading" || !processor)
				continue; // a job is on it, or there is no plugin
			processor->setParameters(setParameters(slot));
			processor->setBypass(slot.bypass);
			const bool restart = instance->restart->exchange(false);
			const bool ready = status == "active" && processor->activeFor(sampleRate, maxFrames) && !restart;
			if (slot.active && ready)
			{
				instance->inGraph = true;
				continue;
			}
			if (!slot.active && status == "inactive" && !instance->inGraph)
				continue;
			if (slot.active && status == "failed" && !restart && instance->triedRate == sampleRate &&
				instance->triedFrames == maxFrames)
				continue;
			instance->action = slot.active ? Action::Reactivate : Action::Deactivate;
			if (slot.active)
			{
				instance->triedRate = sampleRate;
				instance->triedFrames = maxFrames;
			}
			// Out of the graph first, if it is in one: retire acts once the audio thread runs a graph without it.
			instance->inGraph = false;
			instance->leaving = true;
			m_leaving.push_back(instance);
		}
		for (auto instance = m_instances.begin(); instance != m_instances.end();)
		{
			if (wanted.count(instance->first) != 0)
			{
				++instance;
				continue;
			}
			instance->second->action = Action::Destroy;
			instance->second->inGraph = false;
			m_leaving.push_back(instance->second);
			instance = m_instances.erase(instance);
		}
	}

	std::shared_ptr<PluginSlot> SlotInstances::processor(uint64_t slot) const
	{
		const auto found = m_instances.find(slot);
		if (found == m_instances.end() || !found->second->inGraph)
			return nullptr;
		const std::scoped_lock lock(found->second->mutex);
		return found->second->processor;
	}

	void SlotInstances::setProcessMode(plugin::ProcessMode mode) const
	{
		for (const auto& [slot, instance] : m_instances)
			if (const std::shared_ptr<PluginSlot> processor = instance->currentProcessor())
				processor->setProcessMode(mode);
	}

	std::string SlotInstances::status(uint64_t slot) const
	{
		const auto found = m_instances.find(slot);
		if (found == m_instances.end())
			return {};
		const std::scoped_lock lock(found->second->mutex);
		return found->second->status;
	}

	void SlotInstances::whenRetired(
		const std::shared_ptr<Shared>& shared, uint64_t generation, std::function<void()> work)
	{
		static_cast<void>(shared->jobs.run(
			[shared, generation, work = std::move(work)]() mutable
			{
				if (shared->closing || shared->retired(generation))
				{
					work();
					--shared->retiring;
					return;
				}
				// Later, after the jobs queued meanwhile.
				std::this_thread::sleep_for(g_retirePoll);
				whenRetired(shared, generation, std::move(work));
			}));
	}

	void SlotInstances::retire(uint64_t generation)
	{
		if (m_leaving.empty())
			return;
		struct Leaving
		{
			std::shared_ptr<Instance> instance;
			Action action;
		};
		std::vector<Leaving> leaving;
		leaving.reserve(m_leaving.size());
		for (const std::shared_ptr<Instance>& instance : m_leaving)
		{
			instance->leaving = false;
			if (instance->action != Action::Destroy)
				instance->setStatus("loading");
			leaving.push_back({instance, instance->action});
		}
		m_leaving.clear();

		const std::shared_ptr<Shared> shared = m_shared;
		const double sampleRate = m_sampleRate;
		const uint32_t maxFrames = m_maxFrames;
		++shared->retiring;
		whenRetired(shared, generation,
			[shared, leaving = std::move(leaving), sampleRate, maxFrames]
			{
				for (const Leaving& item : leaving)
					item.instance->apply(item.action, sampleRate, maxFrames);
				if (!shared->closing)
					shared->changed();
			});
	}

}
