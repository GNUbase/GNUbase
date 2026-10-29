// SPDX-License-Identifier: GPL-3.0-or-later
// SDL_GPU (Metal, Direct3D 12 or Vulkan) through RmlUi's SDL_GPU renderer, which draws everything the OpenGL 3 renderer
// does: geometry and textures with scissoring and transforms, clip masks, layers with filters, and gradients (#285).
#include "backend.h"

#include <RmlUi_Renderer_SDL_GPU.h>

#include <memory>
#include <utility>

namespace gnubase::spike
{
	namespace
	{
		class GpuBackend final : public Backend
		{
		public:
			GpuBackend(SDL_Window* window, SDL_GPUDevice* device, int msaa)
				: m_window(window), m_device(device),
				  m_renderer(std::make_unique<RenderInterface_SDL_GPU>(device, window, msaa))
			{
			}

			~GpuBackend() override
			{
				SDL_WaitForGPUIdle(m_device);
				m_renderer->Shutdown();
				m_renderer.reset();
				SDL_ReleaseGPUTexture(m_device, m_offscreen);
				SDL_ReleaseGPUTransferBuffer(m_device, m_download);
				SDL_ReleaseWindowFromGPUDevice(m_device, m_window);
				SDL_DestroyGPUDevice(m_device);
				SDL_DestroyWindow(m_window);
			}

			GpuBackend(const GpuBackend&) = delete;
			GpuBackend& operator=(const GpuBackend&) = delete;

			const char* name() const override { return "gpu"; }
			std::string driver() const override
			{
				return std::string("SDL_GPU ") + SDL_GetGPUDeviceDriver(m_device) + ", " +
					std::to_string(m_renderer->GetMsaaSamples()) + "x MSAA";
			}
			SDL_Window* window() const override { return m_window; }
			Rml::RenderInterface& renderInterface() override { return *m_renderer; }

			bool render(const std::function<void()>& draw, Image* capture) override
			{
				SDL_GPUCommandBuffer* commands = SDL_AcquireGPUCommandBuffer(m_device);
				if (commands == nullptr)
					return false;
				SDL_GPUTexture* target = nullptr;
				Uint32 width = 0;
				Uint32 height = 0;
				if (capture != nullptr)
				{
					const Rml::Vector2i size = pixelSize();
					width = static_cast<Uint32>(size.x);
					height = static_cast<Uint32>(size.y);
					target = offscreen(width, height);
				}
				else if (!SDL_WaitAndAcquireGPUSwapchainTexture(commands, m_window, &target, &width, &height))
				{
					SDL_CancelGPUCommandBuffer(commands);
					return false;
				}
				if (target == nullptr || width == 0 || height == 0)
				{
					SDL_CancelGPUCommandBuffer(commands); // minimized
					return capture == nullptr;
				}

				SDL_GPUColorTargetInfo clear{};
				clear.texture = target;
				clear.clear_color = {0.0F, 0.0F, 0.0F, 1.0F};
				clear.load_op = SDL_GPU_LOADOP_CLEAR;
				clear.store_op = SDL_GPU_STOREOP_STORE;
				SDL_EndGPURenderPass(SDL_BeginGPURenderPass(commands, &clear, 1, nullptr));
				m_renderer->BeginFrame(commands, target, width, height);
				draw();
				m_renderer->EndFrame();

				if (capture == nullptr)
					return SDL_SubmitGPUCommandBuffer(commands);
				return download(commands, target, width, height, *capture);
			}

			void finish() override { SDL_WaitForGPUIdle(m_device); }

			bool setVsync(bool vsync)
			{
				SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
				if (!vsync && SDL_WindowSupportsGPUPresentMode(m_device, m_window, SDL_GPU_PRESENTMODE_IMMEDIATE))
					mode = SDL_GPU_PRESENTMODE_IMMEDIATE;
				else if (!vsync && SDL_WindowSupportsGPUPresentMode(m_device, m_window, SDL_GPU_PRESENTMODE_MAILBOX))
					mode = SDL_GPU_PRESENTMODE_MAILBOX;
				return SDL_SetGPUSwapchainParameters(m_device, m_window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);
			}

		private:
			// A texture like the swapchain's for rendering offscreen, and a buffer to read it back through.
			SDL_GPUTexture* offscreen(Uint32 width, Uint32 height)
			{
				if (m_offscreen != nullptr && width == m_offscreenWidth && height == m_offscreenHeight)
					return m_offscreen;
				SDL_ReleaseGPUTexture(m_device, m_offscreen);
				SDL_ReleaseGPUTransferBuffer(m_device, m_download);
				SDL_GPUTextureCreateInfo texture{};
				texture.type = SDL_GPU_TEXTURETYPE_2D;
				texture.format = SDL_GetGPUSwapchainTextureFormat(m_device, m_window);
				texture.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
				texture.width = width;
				texture.height = height;
				texture.layer_count_or_depth = 1;
				texture.num_levels = 1;
				m_offscreen = SDL_CreateGPUTexture(m_device, &texture);
				SDL_GPUTransferBufferCreateInfo buffer{};
				buffer.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
				buffer.size = width * height * 4;
				m_download = SDL_CreateGPUTransferBuffer(m_device, &buffer);
				m_offscreenWidth = width;
				m_offscreenHeight = height;
				m_format = texture.format;
				return m_download != nullptr ? m_offscreen : nullptr;
			}

			bool download(
				SDL_GPUCommandBuffer* commands, SDL_GPUTexture* target, Uint32 width, Uint32 height, Image& capture)
			{
				SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands);
				SDL_GPUTextureRegion source{};
				source.texture = target;
				source.w = width;
				source.h = height;
				source.d = 1;
				SDL_GPUTextureTransferInfo destination{};
				destination.transfer_buffer = m_download;
				SDL_DownloadFromGPUTexture(copy, &source, &destination);
				SDL_EndGPUCopyPass(copy);
				SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
				if (fence == nullptr)
					return false;
				const bool done = SDL_WaitForGPUFences(m_device, true, &fence, 1);
				SDL_ReleaseGPUFence(m_device, fence);
				if (!done)
					return false;

				const auto* pixels =
					static_cast<const unsigned char*>(SDL_MapGPUTransferBuffer(m_device, m_download, false));
				if (pixels == nullptr)
					return false;
				capture.width = static_cast<int>(width);
				capture.height = static_cast<int>(height);
				capture.pixels.assign(pixels, pixels + (static_cast<size_t>(width) * height * 4));
				SDL_UnmapGPUTransferBuffer(m_device, m_download);
				const bool bgra = m_format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
					m_format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
				if (bgra)
					for (size_t i = 0; i < capture.pixels.size(); i += 4)
						std::swap(capture.pixels[i], capture.pixels[i + 2]);
				return true;
			}

			SDL_Window* m_window;
			SDL_GPUDevice* m_device;
			std::unique_ptr<RenderInterface_SDL_GPU> m_renderer;
			SDL_GPUTexture* m_offscreen = nullptr;
			SDL_GPUTransferBuffer* m_download = nullptr;
			Uint32 m_offscreenWidth = 0;
			Uint32 m_offscreenHeight = 0;
			SDL_GPUTextureFormat m_format = SDL_GPU_TEXTUREFORMAT_INVALID;
		};
	}

	std::unique_ptr<Backend> makeGpuBackend(
		const WindowOptions& options, const std::string& driver, int msaa, std::string& error)
	{
		SDL_Window* window = createWindow(options, 0, error);
		if (window == nullptr)
			return nullptr;
		const SDL_PropertiesID properties = SDL_CreateProperties();
		SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true);
		SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN, true);
		SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN, true);
		if (!driver.empty())
			SDL_SetStringProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, driver.c_str());
		SDL_GPUDevice* device = SDL_CreateGPUDeviceWithProperties(properties);
		SDL_DestroyProperties(properties);
		if (device == nullptr)
		{
			error = std::string("cannot create an SDL_GPU device: ") + SDL_GetError();
			SDL_DestroyWindow(window);
			return nullptr;
		}
		if (!SDL_ClaimWindowForGPUDevice(device, window))
		{
			error = std::string("the SDL_GPU device cannot render into the window: ") + SDL_GetError();
			SDL_DestroyGPUDevice(device);
			SDL_DestroyWindow(window);
			return nullptr;
		}
		auto backend = std::make_unique<GpuBackend>(window, device, msaa > 0 ? msaa : RMLUI_SDL_GPU_MSAA_SAMPLES);
		backend->setVsync(options.vsync);
		return backend;
	}
}
