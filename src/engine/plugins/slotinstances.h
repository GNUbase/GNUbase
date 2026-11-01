// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/rendersnapshot.h"
#include "engine/plugins/pluginslot.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace gnubase::engine::plugins
{
	class HostManager;

	// The plugins of the project's slots (#282), for the audio engine's graphs. Making, activating, deactivating and
	// destroying a plugin are jobs on the plugin job thread (HostManager::run), never on the command or audio
	// thread: until its plugin is active for the device's sample rate and block size, a slot's node has no processor
	// and passes its audio on. A plugin that a graph may still process is deactivated only once the audio thread runs
	// a graph without it.
	class SlotInstances
	{
	public:
		// A slot of the render snapshot, on a channel with that many audio channels.
		struct Wanted
		{
			const RenderSlot* slot = nullptr;
			uint32_t channels = 2;
		};

		// changed is called on any thread when a plugin became ready or failed, or asked to be restarted; the engine
		// then builds its graph again (update). retired(generation) tells whether the audio thread can no longer
		// process a graph older than that generation.
		SlotInstances(
			HostManager& jobs, std::function<void()> changed, std::function<bool(uint64_t generation)> retired);
		// Destroys the plugins on the job thread and waits for it; no graph may process them any more.
		~SlotInstances();
		SlotInstances(const SlotInstances&) = delete;
		SlotInstances& operator=(const SlotInstances&) = delete;
		SlotInstances(SlotInstances&&) = delete;
		SlotInstances& operator=(SlotInstances&&) = delete;

		// [command thread] Brings the plugins in line with the slots for a device at the sample rate and block size:
		// makes and activates new ones, passes on parameter values and bypass, and marks those that must leave the
		// graph before they are deactivated, reactivated or destroyed. Call retire once the graph without them is
		// published.
		void update(const std::vector<Wanted>& slots, double sampleRate, uint32_t maxFrames);
		// [command thread] The processor for a slot's node, or none while its plugin isn't ready.
		std::shared_ptr<PluginSlot> processor(uint64_t slot) const;
		// [command thread] "loading", "active", "inactive", "failed" (it couldn't be made or activated) or
		// "unavailable" (this build has no such plugin); "" for no slot.
		std::string status(uint64_t slot) const;
		// [command thread] The graph of this generation, now published, has none of the plugins update marked: they
		// are deactivated, activated again or destroyed as soon as the audio thread runs it.
		void retire(uint64_t generation);

	private:
		struct Instance;
		struct Shared;
		class EngineHost;

		void start(const std::shared_ptr<Instance>& instance, const Wanted& wanted);
		void post(std::function<void()> job);
		// [any] Runs work as a job once the audio thread can't process a graph older than generation any more, or the
		// engine closes. It waits in jobs queued again and again, so other jobs run meanwhile.
		static void whenRetired(const std::shared_ptr<Shared>& shared, uint64_t generation, std::function<void()> work);

		std::shared_ptr<Shared> m_shared;
		std::map<uint64_t, std::shared_ptr<Instance>> m_instances;
		std::vector<std::shared_ptr<Instance>> m_leaving; // to retire: they leave or change
		double m_sampleRate = 0.0;
		uint32_t m_maxFrames = 0;
	};
}
