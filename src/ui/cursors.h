// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui/windowsystem.h"

#include <array>
#include <filesystem>
#include <map>
#include <optional>
#include <string>

// GNUbase's own mouse pointers (#469): SVG files that the style sheets name with RmlUi's cursor property.
// docs/features/svg.md describes them.
namespace gnubase::ui
{
	// The display scales a pointer is rasterized at; the window system scales the nearest one for others.
	inline constexpr std::array<float, 4> g_cursorScales = {1.0F, 1.5F, 2.0F, 3.0F};
	// The largest width and height a pointer's SVG may have; OSes show none much bigger.
	inline constexpr float g_maxCursorSize = 128.0F;

	// The pointers in a folder: resources/cursors/cursors.json names each one, {"<name>": {"hotspot": [x, y]}}, and
	// <name>.svg next to it is its picture, as many pixels big at scale 1 as the SVG's width and height say, pointing
	// at the hotspot in those units. A name the folder doesn't have is one of the OS's pointers.
	class Cursors
	{
	public:
		// Without a folder, every name is the OS's. Logs a manifest it can't read and goes on without it.
		explicit Cursors(std::filesystem::path folder = {});

		// The pointer for the name, its pictures rasterized at g_cursorScales the first time it is asked for. A
		// picture that can't be read is logged, and the OS's arrow shows instead.
		const Cursor& get(const std::string& name);

		// Whether the folder has a pointer of that name.
		bool has(const std::string& name) const { return m_hotspots.count(name) != 0; }

	private:
		std::optional<Cursor> load(const std::string& name) const;

		std::filesystem::path m_folder;
		std::map<std::string, std::array<float, 2>> m_hotspots; // by name, in the SVG's width and height units
		std::map<std::string, Cursor> m_loaded;
	};
}
