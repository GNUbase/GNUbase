// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui/windowsystem.h"

#include <RmlUi/Core/RenderInterface.h>
#include <RmlUi/Core/Types.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>

namespace gnubase::ui
{
	// An 8-bit RGBA picture, top row first.
	struct Image
	{
		int width = 0;
		int height = 0;
		std::vector<uint8_t> pixels; // width * height * 4 bytes

		std::array<uint8_t, 4> pixel(int x, int y) const
		{
			const size_t at = ((static_cast<size_t>(y) * static_cast<size_t>(width)) + static_cast<size_t>(x)) * 4;
			return {pixels[at], pixels[at + 1], pixels[at + 2], pixels[at + 3]};
		}
	};

	// Writes the image as a PNG file; false if it can't.
	bool writePng(const Image& image, const std::filesystem::path& file);

	// Draws windows' RmlUi contexts: on the screen, or offscreen into an image for screenshots (#58, #59).
	class Renderer
	{
	public:
		virtual ~Renderer() = default;

		// Makes the renderer draw into the window. The first window it attaches sets up the render interface.
		virtual void attach(WindowId window) = 0;
		virtual void detach(WindowId window) = 0;

		// What RmlUi draws through; valid once a window is attached, until the renderer is destroyed. Every window's
		// context shares it, so textures and geometry are made once for all windows.
		virtual Rml::RenderInterface& renderInterface() = 0;

		// Draws the context into its window and shows it, waiting for vsync if the window is behind. True if the
		// frame was shown or there was nothing to show it in (a minimized or hidden window).
		virtual bool present(WindowId window, Rml::Context& context) = 0;

		// Draws the context offscreen at the window's pixel size and reads the result back. Null if it can't.
		virtual std::optional<Image> capture(WindowId window, Rml::Context& context) = 0;
	};
}
