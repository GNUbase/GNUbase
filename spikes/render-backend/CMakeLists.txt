# SPDX-License-Identifier: GPL-3.0-or-later
# The render backend spike (#47): renders RmlUi documents through OpenGL 3 and SDL_GPU, writes screenshots and times
# frames. Not part of GNUbase; README.md says how to run it.

# RmlUi's own renderers and SDL platform code and stb, compiled as they are, without GNUbase's warnings.
set(rmluiBackends "${PROJECT_SOURCE_DIR}/ext/RmlUi/Backends")
add_library(gnubase-render-spike-rmlui STATIC
	"${rmluiBackends}/RmlUi_Platform_SDL.cpp"
	"${rmluiBackends}/RmlUi_Renderer_GL3.cpp"
	"${rmluiBackends}/RmlUi_Renderer_SDL_GPU.cpp"
	stb.c)
target_include_directories(gnubase-render-spike-rmlui SYSTEM PUBLIC "${rmluiBackends}" "${PROJECT_SOURCE_DIR}/ext/stb"
	# The SDL_GPU renderer loads images with SDL_image, which GNUbase doesn't have; the spike loads them with stb.
	"${CMAKE_CURRENT_SOURCE_DIR}/shim")
target_compile_definitions(gnubase-render-spike-rmlui PUBLIC RMLUI_SDL_VERSION_MAJOR=3)
target_link_libraries(gnubase-render-spike-rmlui PUBLIC RmlUi::RmlUi SDL3::SDL3 ${CMAKE_DL_LIBS})

add_executable(gnubase-render-spike main.cpp backend.cpp backend.h backend_gl3.cpp backend_gpu.cpp elements.cpp
	elements.h image.cpp image.h)
target_link_libraries(gnubase-render-spike PRIVATE gnubase-render-spike-rmlui)
target_compile_definitions(gnubase-render-spike PRIVATE
	GNUBASE_SPIKE_DOCUMENTS="${CMAKE_CURRENT_SOURCE_DIR}/documents")
gnubase_target_defaults(gnubase-render-spike)
