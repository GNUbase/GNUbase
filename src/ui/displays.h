// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Monitors and where windows go on them (CLAUDE.md, rule 10). Positions and sizes are in desktop coordinates, the
// window system's screen coordinates, which can be negative left of and above the primary monitor.
namespace gnubase::ui
{
	struct Point
	{
		int x = 0;
		int y = 0;
		bool operator==(const Point& other) const { return x == other.x && y == other.y; }
	};

	struct Size
	{
		int width = 0;
		int height = 0;
		bool operator==(const Size& other) const { return width == other.width && height == other.height; }
	};

	struct Rect
	{
		int x = 0;
		int y = 0;
		int width = 0;
		int height = 0;

		Point position() const { return {x, y}; }
		Size size() const { return {width, height}; }
		bool operator==(const Rect& other) const
		{
			return x == other.x && y == other.y && width == other.width && height == other.height;
		}
	};

	// The area two rectangles share; zero if they don't overlap.
	int64_t overlap(const Rect& a, const Rect& b);

	using DisplayId = uint32_t;

	struct Display
	{
		DisplayId id = 0;
		std::string name;
		Rect bounds;        // the whole monitor
		Rect usable;        // without task bars, docks and menu bars
		float scale = 1.0F; // the monitor's content scale: 1 at 96 dpi on Windows, 2 on a Retina display
	};

	// The display a window is on: the one it overlaps most. Null if it overlaps none.
	const Display* displayOf(const Rect& window, const std::vector<Display>& displays);

	// Whether the user can reach the window to move it: a part of its top edge, where the title bar is, at least
	// g_reachableWidth wide and g_reachableHeight high, lies in a display's usable area.
	inline constexpr int g_reachableWidth = 64;
	inline constexpr int g_reachableHeight = 24;
	bool reachable(const Rect& window, const std::vector<Display>& displays);

	// Where a window goes: where it is if it's reachable, otherwise centred in the usable area of the first display
	// (the primary one), shrunk to fit. For windows on a monitor that was disconnected, and for windows that open
	// where a monitor used to be. Without any display the window stays where it is.
	Rect place(const Rect& window, const std::vector<Display>& displays);

	// A window of the given size, centred in the display's usable area and shrunk to fit.
	Rect centred(Size size, const Display& display);
}
