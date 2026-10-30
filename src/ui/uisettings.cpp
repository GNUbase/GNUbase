// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/uisettings.h"

#include "engine/engine.h"
#include "platform/log.h"
#include "ui/shell.h"
#include "ui/themes.h"

#include <string>

namespace gnubase::ui
{
	namespace
	{
		// The theme the setting names, or the default theme if there is no such theme.
		std::string themeFor(const engine::Json& value, const Themes& themes)
		{
			std::string name = value.is_string() ? value.get<std::string>() : std::string();
			if (themes.has(name))
				return name;
			logging::warning("There is no theme {}; using {}", value.dump(), g_defaultTheme);
			return g_defaultTheme;
		}
	}

	UiSettings::UiSettings(engine::Engine& engine, Shell& shell, const Themes& themes)
	{
		engine::Client client(engine);
		const engine::Result theme = client.call("settings.get", {{"key", "ui.theme"}});
		shell.setTheme(themeFor(theme.ok() ? theme.value.at("value") : engine::Json(), themes));
		const engine::Result scale = client.call("settings.get", {{"key", "ui.scale"}});
		if (scale.ok())
			shell.setUiScale(scale.value.at("value").get<float>());

		// On the engine's thread: decide here, apply on the UI thread.
		m_listener = std::make_unique<engine::Listener>(engine,
			[&shell, &themes](const engine::Event& event)
			{
				if (event.name != "settings.changed")
					return;
				const engine::Json& values = event.data.at("values");
				if (const auto it = values.find("ui.theme"); it != values.end())
					shell.post([&shell, name = themeFor(*it, themes)] { shell.setTheme(name); });
				if (const auto it = values.find("ui.scale"); it != values.end() && it->is_number())
					shell.post([&shell, scale = it->get<float>()] { shell.setUiScale(scale); });
			});
	}

	UiSettings::~UiSettings() = default;
}
