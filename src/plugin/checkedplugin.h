// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "plugin/extensions.h"
#include "plugin/plugin.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

// A Plugin that checks every call against the thread tags and the lifecycle of plugin.h before passing it on. Hosts
// wrap plugins in it in debug builds: the engine around every plugin of a slot, the plugin host process around every
// adapter (docs/plugin-api.md, "Threading rules"). A call on the wrong thread or in the wrong state is reported and
// not passed on: it returns false, ProcessStatus::Error or nullptr instead.
namespace gnubase::plugin
{
	class CheckedPlugin final : public Plugin
	{
	public:
		enum class Lifecycle : uint8_t
		{
			Created,
			Inactive,
			Active,
			Processing,
		};

		// Called for every violation, on the thread of the call; it must be real-time safe. The message is a literal.
		using ViolationHandler = void (*)(const char* message, void* context) noexcept;

		// `threads` says which thread a call is on; the host's ThreadCheck. It must outlive the wrapper.
		CheckedPlugin(std::unique_ptr<Plugin> plugin, const ThreadCheck& threads, ViolationHandler handler = nullptr,
			void* context = nullptr)
			: m_plugin(std::move(plugin)), m_threads(threads), m_handler(handler), m_context(context)
		{
		}

		~CheckedPlugin() override
		{
			if (!m_threads.isMainThread())
				violation("Plugin destroyed: not on the main thread");
			else if (m_lifecycle != Lifecycle::Created && m_lifecycle != Lifecycle::Inactive)
				violation("Plugin destroyed: still active");
		}

		CheckedPlugin(const CheckedPlugin&) = delete;
		CheckedPlugin& operator=(const CheckedPlugin&) = delete;
		CheckedPlugin(CheckedPlugin&&) = delete;
		CheckedPlugin& operator=(CheckedPlugin&&) = delete;

		Lifecycle lifecycle() const noexcept { return m_lifecycle; }
		// The violations so far, and the message of the last one (nullptr if there was none).
		uint32_t violations() const noexcept { return m_violations; }
		const char* lastViolation() const noexcept { return m_lastViolation; }
		Plugin& wrapped() noexcept { return *m_plugin; }

		const Descriptor& descriptor() const override { return m_plugin->descriptor(); }

		bool init() override
		{
			if (!onMain("Plugin::init: not on the main thread") ||
				!in(Lifecycle::Created, "Plugin::init: called twice"))
				return false;
			if (!m_plugin->init())
				return false;
			m_lifecycle = Lifecycle::Inactive;
			return true;
		}

		bool setProcessMode(ProcessMode mode) override
		{
			if (!onMain("Plugin::setProcessMode: not on the main thread") ||
				!in(Lifecycle::Inactive, "Plugin::setProcessMode: not inactive"))
				return false;
			return m_plugin->setProcessMode(mode);
		}

		bool activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames) override
		{
			if (!onMain("Plugin::activate: not on the main thread") ||
				!in(Lifecycle::Inactive, "Plugin::activate: not inactive"))
				return false;
			if (minFrames > maxFrames || maxFrames == 0)
			{
				violation("Plugin::activate: no valid block size");
				return false;
			}
			if (!m_plugin->activate(sampleRate, minFrames, maxFrames))
				return false;
			m_lifecycle = Lifecycle::Active;
			return true;
		}

		void deactivate() override
		{
			if (!onMain("Plugin::deactivate: not on the main thread") ||
				!in(Lifecycle::Active, "Plugin::deactivate: not active, or still processing"))
				return;
			m_plugin->deactivate();
			m_lifecycle = Lifecycle::Inactive;
		}

		bool startProcessing() noexcept override
		{
			if (!onAudio("Plugin::startProcessing: not on an audio thread") ||
				!in(Lifecycle::Active, "Plugin::startProcessing: not active, or already processing"))
				return false;
			if (!m_plugin->startProcessing())
				return false;
			m_lifecycle = Lifecycle::Processing;
			return true;
		}

		void stopProcessing() noexcept override
		{
			if (!onAudio("Plugin::stopProcessing: not on an audio thread") ||
				!in(Lifecycle::Processing, "Plugin::stopProcessing: not processing"))
				return;
			m_plugin->stopProcessing();
			m_lifecycle = Lifecycle::Active;
		}

		void reset() noexcept override
		{
			if (!onAudio("Plugin::reset: not on an audio thread"))
				return;
			const Lifecycle lifecycle = m_lifecycle;
			if (lifecycle != Lifecycle::Active && lifecycle != Lifecycle::Processing)
			{
				violation("Plugin::reset: not active");
				return;
			}
			(*m_plugin).reset(); // the plugin's reset, not the pointer's
		}

		ProcessStatus process(const Process& process) noexcept override
		{
			if (!onAudio("Plugin::process: not on an audio thread") ||
				!in(Lifecycle::Processing, "Plugin::process: not processing"))
				return ProcessStatus::Error;
			return m_plugin->process(process);
		}

		void* extension(std::string_view id) override
		{
			if (!onMain("Plugin::extension: not on the main thread") || !initialized("Plugin::extension: before init"))
				return nullptr;
			return m_plugin->extension(id);
		}

		void onMainThread() override
		{
			if (!onMain("Plugin::onMainThread: not on the main thread") ||
				!initialized("Plugin::onMainThread: before init"))
				return;
			m_plugin->onMainThread();
		}

	private:
		void violation(const char* message) noexcept
		{
			m_violations.fetch_add(1);
			m_lastViolation.store(message);
			if (m_handler != nullptr)
				m_handler(message, m_context);
		}

		bool onMain(const char* message) noexcept
		{
			if (m_threads.isMainThread())
				return true;
			violation(message);
			return false;
		}

		bool onAudio(const char* message) noexcept
		{
			if (m_threads.isAudioThread())
				return true;
			violation(message);
			return false;
		}

		bool in(Lifecycle lifecycle, const char* message) noexcept
		{
			if (m_lifecycle == lifecycle)
				return true;
			violation(message);
			return false;
		}

		bool initialized(const char* message) noexcept
		{
			if (m_lifecycle != Lifecycle::Created)
				return true;
			violation(message);
			return false;
		}

		std::unique_ptr<Plugin> m_plugin;
		const ThreadCheck& m_threads;
		ViolationHandler m_handler;
		void* m_context;
		// Written on the main and the audio thread, one after the other.
		std::atomic<Lifecycle> m_lifecycle{Lifecycle::Created};
		std::atomic<uint32_t> m_violations{0};
		std::atomic<const char*> m_lastViolation{nullptr};
	};
}
