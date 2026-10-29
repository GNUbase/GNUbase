// SPDX-License-Identifier: GPL-3.0-or-later
// OpenGL 3.3 core through RmlUi's GL3 renderer, which implements RmlUi's whole render interface.
#include "backend.h"

#include <RmlUi_Include_GL3.h>
#include <RmlUi_Renderer_GL3.h>

#include <cstring>
#include <memory>
#include <vector>

namespace gnubase::spike
{
	namespace
	{
		class Gl3Backend final : public Backend
		{
		public:
			Gl3Backend(SDL_Window* window, SDL_GLContext context) : m_window(window), m_context(context) {}

			~Gl3Backend() override
			{
				m_renderer.reset();
				RmlGL3::Shutdown();
				SDL_GL_DestroyContext(m_context);
				SDL_DestroyWindow(m_window);
			}

			Gl3Backend(const Gl3Backend&) = delete;
			Gl3Backend& operator=(const Gl3Backend&) = delete;

			bool start(std::string& error)
			{
				Rml::String message;
				if (!RmlGL3::Initialize(&message))
				{
					error = "cannot load OpenGL 3.3: " + message;
					return false;
				}
				m_driver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
				m_driver += ", ";
				m_driver += reinterpret_cast<const char*>(glGetString(GL_RENDERER));
				m_renderer = std::make_unique<RenderInterface_GL3>();
				if (!*m_renderer)
				{
					error = "the OpenGL 3 renderer cannot compile its shaders";
					return false;
				}
				return true;
			}

			const char* name() const override { return "gl3"; }
			std::string driver() const override { return "OpenGL " + m_driver; }
			SDL_Window* window() const override { return m_window; }
			Rml::RenderInterface& renderInterface() override { return *m_renderer; }

			bool render(const std::function<void()>& draw, Image* capture) override
			{
				const Rml::Vector2i size = pixelSize();
				m_renderer->SetViewport(size.x, size.y);
				m_renderer->Clear();
				m_renderer->BeginFrame();
				draw();
				m_renderer->EndFrame();
				if (capture == nullptr)
					return SDL_GL_SwapWindow(m_window);

				// EndFrame composited the frame into the window's back buffer.
				capture->width = size.x;
				capture->height = size.y;
				capture->pixels.resize(static_cast<size_t>(size.x) * static_cast<size_t>(size.y) * 4);
				std::vector<unsigned char> rows(capture->pixels.size());
				glReadBuffer(GL_BACK);
				glPixelStorei(GL_PACK_ALIGNMENT, 1);
				glReadPixels(0, 0, size.x, size.y, GL_RGBA, GL_UNSIGNED_BYTE, rows.data());
				const size_t stride = static_cast<size_t>(size.x) * 4;
				for (int row = 0; row < size.y; ++row) // OpenGL's first row is the bottom one
					std::memcpy(capture->pixels.data() + (static_cast<size_t>(row) * stride),
						rows.data() + (static_cast<size_t>(size.y - 1 - row) * stride), stride);
				return glGetError() == GL_NO_ERROR;
			}

			void finish() override { glFinish(); }

		private:
			SDL_Window* m_window;
			SDL_GLContext m_context;
			std::string m_driver;
			std::unique_ptr<RenderInterface_GL3> m_renderer;
		};
	}

	std::unique_ptr<Backend> makeGl3Backend(const WindowOptions& options, std::string& error)
	{
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#ifdef __APPLE__
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
		SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
		SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8); // clip masks
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);

		SDL_Window* window = createWindow(options, SDL_WINDOW_OPENGL, error);
		if (window == nullptr)
			return nullptr;
		SDL_GLContext context = SDL_GL_CreateContext(window);
		if (context == nullptr)
		{
			error = std::string("cannot create an OpenGL 3.3 context: ") + SDL_GetError();
			SDL_DestroyWindow(window);
			return nullptr;
		}
		SDL_GL_MakeCurrent(window, context);
		SDL_GL_SetSwapInterval(options.vsync ? 1 : 0);
		auto backend = std::make_unique<Gl3Backend>(window, context);
		if (!backend->start(error))
			return nullptr;
		return backend;
	}
}
