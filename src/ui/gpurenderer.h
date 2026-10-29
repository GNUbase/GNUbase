// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui/renderer.h"

#include <memory>
#include <set>
#include <stdexcept>
#include <string>

struct SDL_GPUDevice;
class RenderInterface_SDL_GPU; // NOLINT(readability-identifier-naming): RmlUi's name for its SDL_GPU renderer

namespace gnubase::ui
{
	class SdlWindowSystem;

	// No SDL_GPU device on this machine: no Metal, Direct3D 12 or Vulkan driver that SDL_GPU can use.
	class GpuUnavailable : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};

	// Draws through SDL_GPU (docs/render-backend.md): one device for all windows, a swapchain per window, and
	// RmlUi's SDL_GPU renderer as the render interface they share.
	class GpuRenderer final : public Renderer
	{
	public:
		// Throws GpuUnavailable if this machine has no SDL_GPU device.
		explicit GpuRenderer(SdlWindowSystem& windows);
		~GpuRenderer() override;
		GpuRenderer(const GpuRenderer&) = delete;
		GpuRenderer& operator=(const GpuRenderer&) = delete;
		GpuRenderer(GpuRenderer&&) = delete;
		GpuRenderer& operator=(GpuRenderer&&) = delete;

		// The graphics API: "metal", "direct3d12" or "vulkan".
		std::string driver() const;

		// Throws std::runtime_error if the device can't draw into the window.
		void attach(WindowId window) override;
		// Does nothing for a window that isn't attached.
		void detach(WindowId window) override;
		Rml::RenderInterface& renderInterface() override;
		bool present(WindowId window, Rml::Context& context) override;
		std::optional<Image> capture(WindowId window, Rml::Context& context) override;

	private:
		SdlWindowSystem& m_windows;
		SDL_GPUDevice* m_device = nullptr;
		std::unique_ptr<RenderInterface_SDL_GPU> m_interface;
		std::set<WindowId> m_attached;
		int m_format = 0; // the swapchain texture format of the first window, which RmlUi's pipelines are made for
	};
}
