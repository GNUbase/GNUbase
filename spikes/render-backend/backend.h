// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "image.h"

#include <RmlUi/Core/RenderInterface.h>
#include <SDL3/SDL.h>

#include <functional>
#include <memory>
#include <string>

// A render backend renders RmlUi into one SDL window: OpenGL 3 or SDL_GPU (Metal, Direct3D 12 or Vulkan).
namespace gnubase::spike
{
	struct WindowOptions
	{
		std::string title = "GNUbase render spike";
		int width = 800; // in window coordinates; the pixel size follows the monitor's scale
		int height = 600;
		bool visible = true;
		bool vsync = true;
	};

	class Backend
	{
	public:
		virtual ~Backend() = default;

		// "gl3" or "gpu".
		virtual const char* name() const = 0;

		// What renders, e.g. "OpenGL 4.5 (Core Profile) Mesa, llvmpipe" or "SDL_GPU vulkan".
		virtual std::string driver() const = 0;

		virtual SDL_Window* window() const = 0;
		virtual Rml::RenderInterface& renderInterface() = 0;

		// The size of the drawable area in pixels.
		virtual Rml::Vector2i pixelSize() const;

		// Renders one frame: clears it, calls draw, which renders the RmlUi context, and shows it. With capture, the
		// frame is rendered offscreen and read back into capture instead. False if the frame failed.
		virtual bool render(const std::function<void()>& draw, Image* capture) = 0;

		// Waits until the GPU has finished every frame, so timings include the GPU's work.
		virtual void finish() = 0;
	};

	// The backends; null with a reason in error if this machine can't run them.
	std::unique_ptr<Backend> makeGl3Backend(const WindowOptions& options, std::string& error);
	// msaa: samples per pixel, 0 for the renderer's default.
	std::unique_ptr<Backend> makeGpuBackend(
		const WindowOptions& options, const std::string& driver, int msaa, std::string& error);

	// A window with the options and the given extra flags, e.g. SDL_WINDOW_OPENGL.
	SDL_Window* createWindow(const WindowOptions& options, SDL_WindowFlags flags, std::string& error);
}
