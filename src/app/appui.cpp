// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/appui.h"

#include "ui/fonts.h"

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

	AppUi::AppUi(
		engine::Engine& engine, const std::filesystem::path& resources, const ui::WindowSpec& main, int msaaSamples)
		: m_renderer(m_windows, msaaSamples), m_rmlui(m_windows), m_themes(fontsAndThemes(resources)),
		  m_shell(m_windows, m_renderer), m_settings(engine, m_shell, m_themes),
		  m_binding(engine, [this] { m_shell.wake(); }), m_main(m_shell.open(main))
	{
		m_shell.eachStep([this] { return m_binding.update(); });
	}
}
