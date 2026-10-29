// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/gpurenderer.h"

#include "platform/log.h"
#include "ui/sdlwindowsystem.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi_Renderer_SDL_GPU.h>
#include <SDL3/SDL.h>

#include <utility>

namespace gnubase::ui
{
	namespace
	{
		bool isBgra(SDL_GPUTextureFormat format)
		{
			return format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
				format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
		}

		// Clears the target, then draws the context into it through the render interface.
		void draw(SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, Uint32 width, Uint32 height,
			RenderInterface_SDL_GPU& renderer, Rml::Context& context)
		{
			SDL_GPUColorTargetInfo clear{};
			clear.texture = target;
			clear.clear_color = {0.0F, 0.0F, 0.0F, 1.0F};
			clear.load_op = SDL_GPU_LOADOP_CLEAR;
			clear.store_op = SDL_GPU_STOREOP_STORE;
			SDL_EndGPURenderPass(SDL_BeginGPURenderPass(commands, &clear, 1, nullptr));
			renderer.BeginFrame(commands, target, width, height);
			context.Render();
			renderer.EndFrame();
		}
	}

	GpuRenderer::GpuRenderer(SdlWindowSystem& windows) : m_windows(windows)
	{
		// RmlUi's renderer brings its shaders in all three formats; SDL_GPU picks the API this machine has.
		const SDL_PropertiesID properties = SDL_CreateProperties();
		SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
		SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN, true);
		SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN, true);
		m_device = SDL_CreateGPUDeviceWithProperties(properties);
		SDL_DestroyProperties(properties);
		if (m_device == nullptr)
			throw GpuUnavailable(std::string("no SDL_GPU device: ") + SDL_GetError());
		logging::info("Drawing through SDL_GPU, {}", driver());
	}

	GpuRenderer::~GpuRenderer()
	{
		SDL_WaitForGPUIdle(m_device);
		if (m_interface)
		{
			m_interface->Shutdown();
			m_interface.reset();
		}
		while (!m_attached.empty())
			detach(*m_attached.begin());
		SDL_DestroyGPUDevice(m_device);
	}

	std::string GpuRenderer::driver() const
	{
		const char* name = SDL_GetGPUDeviceDriver(m_device);
		return name != nullptr ? name : "";
	}

	void GpuRenderer::attach(WindowId window)
	{
		SDL_Window* sdl = SdlWindowSystem::sdlWindow(window);
		if (sdl == nullptr || !SDL_ClaimWindowForGPUDevice(m_device, sdl))
			throw std::runtime_error(std::string("SDL_GPU cannot draw into the window: ") + SDL_GetError());
		m_attached.insert(window);
		SDL_SetGPUSwapchainParameters(
			m_device, sdl, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC); // vsync is always supported
		const auto format = static_cast<int>(SDL_GetGPUSwapchainTextureFormat(m_device, sdl));
		if (!m_interface)
		{
			m_interface = std::make_unique<RenderInterface_SDL_GPU>(m_device, sdl); // its pipelines use this format
			m_format = format;
		}
		else if (format != m_format)
		{
			logging::error("Window {} has another swapchain format ({}) than the first window ({}); it can't be drawn",
				window, format, m_format);
		}
	}

	void GpuRenderer::detach(WindowId window)
	{
		if (m_attached.erase(window) == 0)
			return;
		SDL_WaitForGPUIdle(m_device);
		if (SDL_Window* sdl = SdlWindowSystem::sdlWindow(window))
			SDL_ReleaseWindowFromGPUDevice(m_device, sdl);
	}

	Rml::RenderInterface& GpuRenderer::renderInterface()
	{
		if (!m_interface)
			throw std::logic_error("GpuRenderer::renderInterface before a window is attached");
		return *m_interface;
	}

	bool GpuRenderer::present(WindowId window, Rml::Context& context)
	{
		SDL_Window* sdl = SdlWindowSystem::sdlWindow(window);
		if (sdl == nullptr || m_attached.count(window) == 0 || !m_interface)
			return false;
		SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(m_device);
		if (commands == nullptr)
			return false;
		SDL_GPUTexture* target = nullptr;
		Uint32 width = 0;
		Uint32 height = 0;
		if (!SDL_WaitAndAcquireGPUSwapchainTexture(commands, sdl, &target, &width, &height))
		{
			SDL_CancelGPUCommandBuffer(commands);
			return false;
		}
		if (target == nullptr || width == 0 || height == 0)
		{
			SDL_CancelGPUCommandBuffer(commands); // minimized or hidden: nothing to show it in
			return true;
		}
		draw(commands, target, width, height, *m_interface, context);
		return SDL_SubmitGPUCommandBuffer(commands);
	}

	std::optional<Image> GpuRenderer::capture(WindowId window, Rml::Context& context)
	{
		SDL_Window* sdl = SdlWindowSystem::sdlWindow(window);
		if (sdl == nullptr || m_attached.count(window) == 0 || !m_interface)
			return std::nullopt;
		const Size size = m_windows.pixelSize(window);
		if (size.width <= 0 || size.height <= 0)
			return std::nullopt;
		const auto width = static_cast<Uint32>(size.width);
		const auto height = static_cast<Uint32>(size.height);

		// A texture like the window's swapchain, and a buffer to read it back through.
		SDL_GPUTextureCreateInfo textureInfo{};
		textureInfo.type = SDL_GPU_TEXTURETYPE_2D;
		textureInfo.format = SDL_GetGPUSwapchainTextureFormat(m_device, sdl);
		textureInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
		textureInfo.width = width;
		textureInfo.height = height;
		textureInfo.layer_count_or_depth = 1;
		textureInfo.num_levels = 1;
		SDL_GPUTexture* texture = SDL_CreateGPUTexture(m_device, &textureInfo);
		SDL_GPUTransferBufferCreateInfo bufferInfo{};
		bufferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
		bufferInfo.size = width * height * 4;
		SDL_GPUTransferBuffer* buffer = SDL_CreateGPUTransferBuffer(m_device, &bufferInfo);
		SDL_GPUCommandBuffer* commands =
			texture != nullptr && buffer != nullptr ? SDL_AcquireGPUCommandBuffer(m_device) : nullptr;

		std::optional<Image> image;
		if (commands != nullptr)
		{
			draw(commands, texture, width, height, *m_interface, context);
			SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
			SDL_GPUTextureRegion source{};
			source.texture = texture;
			source.w = width;
			source.h = height;
			source.d = 1;
			SDL_GPUTextureTransferInfo destination{};
			destination.transfer_buffer = buffer;
			SDL_DownloadFromGPUTexture(copy, &source, &destination);
			SDL_EndGPUCopyPass(copy);
			SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
			if (fence != nullptr && SDL_WaitForGPUFences(m_device, true, &fence, 1))
			{
				if (const auto* pixels = static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(m_device, buffer, false)))
				{
					image = Image{size.width, size.height, std::vector<uint8_t>(pixels, pixels + bufferInfo.size)};
					SDL_UnmapGPUTransferBuffer(m_device, buffer);
					if (isBgra(textureInfo.format))
						for (size_t i = 0; i < image->pixels.size(); i += 4)
							std::swap(image->pixels[i], image->pixels[i + 2]);
				}
			}
			if (fence != nullptr)
				SDL_ReleaseGPUFence(m_device, fence);
		}
		SDL_ReleaseGPUTransferBuffer(m_device, buffer);
		SDL_ReleaseGPUTexture(m_device, texture);
		return image;
	}
}
