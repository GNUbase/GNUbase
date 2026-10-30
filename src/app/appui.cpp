// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/appui.h"

#include "app/uitools.h"
#include "platform/log.h"
#include "ui/fonts.h"
#include "ui/widgets.h"

#include <RmlUi/Lua.h>
#include <lua.h>

#include <optional>
#include <string>

namespace gnubase::app
{
	namespace
	{
		// GNUbase's fonts, which documents need, then the themes.
		ui::Themes fontsAndThemes(const std::filesystem::path& resources)
		{
			ui::Fonts(resources / "fonts").load();
			return ui::Themes(resources);
		}
	}

	AppUi::AppUi(engine::Engine& engine, const std::filesystem::path& resources, const ui::WindowSpec& main,
		std::ostream& output, int msaaSamples)
		: m_renderer(m_windows, msaaSamples), m_lua(engine, scripting::Trust::Trusted, output), m_rmlui(m_windows),
		  m_themes(fontsAndThemes(resources)), m_shell(m_windows, m_renderer), m_settings(engine, m_shell, m_themes),
		  m_binding(engine, [this] { m_shell.wake(); }), m_keys(engine, m_binding), m_main(m_shell.open(main))
	{
		ui::registerWidgets(m_rmlui);
		lua_State* state = m_lua.state();
		lua_getglobal(state, "print");
		Rml::Lua::Initialise(state);   // until Rml::Shutdown
		lua_setglobal(state, "print"); // GNUbase's, instead of the plugin's, which writes into RmlUi's log
		addUiFunctions(m_lua, m_shell);
		m_shell.eachStep(
			[this]
			{
				if (const std::optional<std::string> error = m_lua.dispatchEvents())
					logging::warning("A Lua event handler of the UI failed: {}", *error);
				return m_binding.update();
			});
		m_shell.onUnusedKey([this](ui::Window& window, const ui::KeyPress& keys) { m_keys.press(window, keys); });
	}
}
