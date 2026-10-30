// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace gnubase::ui
{
	// The theme of a fresh installation, also used when the user setting ui.theme names one that doesn't exist.
	inline constexpr const char* g_defaultTheme = "gnubase";

	// GNUbase's design system and themes (#53, docs/features/themes.md). The shared style sheets in
	// <resources>/styles lay out every widget in dp. Each theme is a folder in <resources>/themes whose theme.rcss
	// gives the widgets their colors, fonts and skins, all inside "@media (theme: <folder name>)".
	//
	// While the object lives, every document RmlUi loads gets the shared style sheets and every theme ahead of its own
	// style, so documents never name a theme or a shared style sheet, and their own rules come last. The shell
	// activates one theme (Shell::setTheme). Style sheets that change on disk apply to documents loaded after RmlUi's
	// style sheet cache is cleared, as live reload does (ui/livereload.h).
	//
	// RmlUi must be initialised (ui/rmlui.h). Throws std::runtime_error if there are no themes, or a theme's style
	// sheet can't be read or puts rules outside its media query.
	class Themes
	{
	public:
		explicit Themes(const std::filesystem::path& resources);
		~Themes();
		Themes(const Themes&) = delete;
		Themes& operator=(const Themes&) = delete;
		Themes(Themes&&) = delete;
		Themes& operator=(Themes&&) = delete;

		// The themes' names, sorted.
		const std::vector<std::string>& names() const { return m_names; }
		bool has(std::string_view name) const;

	private:
		class Loader; // the RmlUi plugin that gives loaded documents the style sheets

		std::vector<std::string> m_names;
		std::unique_ptr<Loader> m_loader;
	};
}
