// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>

namespace gnubase::engine
{
	class Engine;
	class Listener;
}

namespace gnubase::ui
{
	class Shell;
	class Themes;

	// The user settings that belong to the UI, ui.theme and ui.scale (docs/features/themes.md). Applies them to the
	// shell when it is made, and again whenever they change, e.g. through settings.set from MCP or Lua: the changes
	// arrive as settings.changed events on the engine's thread and apply on the UI thread in the shell's next step. A
	// theme that doesn't exist, such as one a newer GNUbase saved, falls back to the default theme. The engine, the
	// shell and the themes must outlive the object.
	class UiSettings
	{
	public:
		UiSettings(engine::Engine& engine, Shell& shell, const Themes& themes);
		~UiSettings();
		UiSettings(const UiSettings&) = delete;
		UiSettings& operator=(const UiSettings&) = delete;
		UiSettings(UiSettings&&) = delete;
		UiSettings& operator=(UiSettings&&) = delete;

	private:
		std::unique_ptr<engine::Listener> m_listener;
	};
}
