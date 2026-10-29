// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui/displays.h"

#include <RmlUi/Core/Types.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// What the shell needs from the operating system's windowing: monitors, top-level windows and their events. SDL3
// provides it in the app (SdlWindowSystem); tests use a fake one with any monitors they like.
namespace gnubase::ui
{
	using WindowId = uint32_t;

	struct WindowSpec
	{
		std::string title = "GNUbase";
		Size size{1280, 800};       // in desktop coordinates; used if bounds is empty
		std::optional<Rect> bounds; // where to open it, e.g. from a workspace (#163); centred on the primary otherwise
		bool hidden = false;        // tests and offscreen screenshots
	};

	enum class WindowEventKind : uint8_t
	{
		CloseRequested,  // the user wants to close the window
		Moved,           // its position changed
		Resized,         // its size in pixels changed
		ScaleChanged,    // its display scale changed, e.g. it moved to a monitor with another scale
		Exposed,         // it needs to be drawn again
		Input,           // mouse, keyboard or touch input went to its RmlUi context
		DisplaysChanged, // a monitor was connected, disconnected, moved or changed its scale
		Quit,            // the application should quit, e.g. Cmd+Q, the last window closed by the OS, or SIGTERM
	};

	struct WindowEvent
	{
		WindowEventKind kind = WindowEventKind::Exposed;
		WindowId window = 0; // zero for DisplaysChanged and Quit
	};

	class WindowSystem
	{
	public:
		virtual ~WindowSystem() = default;

		// The connected monitors, the primary one first.
		virtual std::vector<Display> displays() const = 0;

		// Opens a window where spec says. Throws std::runtime_error if it can't.
		virtual WindowId createWindow(const WindowSpec& spec) = 0;
		virtual void destroyWindow(WindowId window) = 0;

		virtual Rect bounds(WindowId window) const = 0;
		virtual void setBounds(WindowId window, const Rect& bounds) = 0;

		// The size of what the window shows, in pixels.
		virtual Size pixelSize(WindowId window) const = 0;

		// How much bigger the window's content should be than at 96 dpi: its monitor's scale, in pixels, so 2 on a
		// Retina display whether the OS counts the window's size in points or pixels.
		virtual float scale(WindowId window) const = 0;

		// The RmlUi context that the window's mouse, keyboard and touch input goes to; null for none.
		virtual void setInputTarget(WindowId window, Rml::Context* context) = 0;

		// The next event, waiting up to timeout for one; without timeout, until one arrives. Input events are
		// delivered to the window's input target before they are reported.
		virtual std::optional<WindowEvent> nextEvent(std::optional<std::chrono::milliseconds> timeout) = 0;
	};
}
