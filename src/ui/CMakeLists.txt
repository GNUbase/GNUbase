# SPDX-License-Identifier: GPL-3.0-or-later
# RmlUi's SDL platform code (input) and SDL_GPU renderer from its Backends folder, and stb_image and stb_image_write,
# compiled as they are, without GNUbase's warnings and clang-tidy.
set(rmluiBackends "${PROJECT_SOURCE_DIR}/ext/RmlUi/Backends")
add_library(gnubase-ui-thirdparty STATIC
	"${rmluiBackends}/RmlUi_Platform_SDL.cpp"
	"${rmluiBackends}/RmlUi_Renderer_SDL_GPU.cpp"
	thirdparty.c)
target_include_directories(gnubase-ui-thirdparty SYSTEM PUBLIC "${rmluiBackends}" "${PROJECT_SOURCE_DIR}/ext/stb"
	# The SDL_GPU renderer loads image files with SDL_image, which GNUbase doesn't build: thirdparty.c has the function.
	"${CMAKE_CURRENT_SOURCE_DIR}/sdlimage")
target_compile_definitions(gnubase-ui-thirdparty PUBLIC RMLUI_SDL_VERSION_MAJOR=3)
target_link_libraries(gnubase-ui-thirdparty PUBLIC RmlUi::RmlUi SDL3::SDL3)

add_library(gnubase-ui STATIC
	automation.cpp
	automation.h
	datamodel.cpp
	datamodel.h
	displays.cpp
	displays.h
	enginebinding.cpp
	enginebinding.h
	fonts.cpp
	fonts.h
	gpurenderer.cpp
	gpurenderer.h
	image.cpp
	livereload.cpp
	livereload.h
	pseudolocale.cpp
	pseudolocale.h
	renderer.h
	rmlui.cpp
	rmlui.h
	sdlwindowsystem.cpp
	sdlwindowsystem.h
	shell.cpp
	shell.h
	themes.cpp
	themes.h
	ui.cpp
	ui.h
	uisettings.cpp
	uisettings.h
	widgets.cpp
	widgets.h
	windowsystem.h)
target_link_libraries(gnubase-ui
	PUBLIC gnubase-engine RmlUi::RmlUi
	PRIVATE Freetype::Freetype gnubase-ui-thirdparty SDL3::SDL3)
gnubase_target_defaults(gnubase-ui)
