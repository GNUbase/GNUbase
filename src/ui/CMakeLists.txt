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
	automationtracks.cpp
	canvas.cpp
	canvas.h
	colormenu.cpp
	colormenu.h
	colorwidgets.cpp
	cursors.cpp
	cursors.h
	datamodel.cpp
	datamodel.h
	displays.cpp
	displays.h
	docking.cpp
	docking.h
	enginebinding.cpp
	enginebinding.h
	eventdisplay.cpp
	eventdisplay.h
	eventdisplaymenus.cpp
	eventediting.cpp
	eventfades.cpp
	eventtools.cpp
	eventvolume.cpp
	eventwaveforms.cpp
	eventwaveforms.h
	fileimport.cpp
	fonts.cpp
	fonts.h
	gpurenderer.cpp
	gpurenderer.h
	historypanel.cpp
	historypanel.h
	image.cpp
	infoline.cpp
	infoline.h
	inspector.cpp
	inspector.h
	keycommands.cpp
	keycommands.h
	keyeditor.cpp
	keyeditor.h
	listeditor.cpp
	listeditor.h
	livereload.cpp
	livereload.h
	mainmenu.cpp
	mainmenu.h
	markers.cpp
	markers.h
	markerswindow.cpp
	midieditor.cpp
	midieditor.h
	mixer.cpp
	mixer.h
	onscreenkeyboard.cpp
	onscreenkeyboard.h
	poolpanel.cpp
	poolpanel.h
	popupwidgets.cpp
	projectfiles.cpp
	projectfiles.h
	pseudolocale.cpp
	pseudolocale.h
	renderer.h
	rmlui.cpp
	rmlui.h
	sampleeditor.cpp
	sampleeditor.h
	sdlwindowsystem.cpp
	sdlwindowsystem.h
	shell.cpp
	shell.h
	signaturetrack.cpp
	signaturetrack.h
	statusline.cpp
	statusline.h
	tempotrack.cpp
	tempotrack.h
	themes.cpp
	themes.h
	timeformat.cpp
	timeformat.h
	tracklist.cpp
	tracklist.h
	transport.cpp
	transport.h
	ui.cpp
	ui.h
	uisettings.cpp
	uisettings.h
	valuewidgets.cpp
	viewwidgets.cpp
	widgets.cpp
	widgets.h
	widgetutils.h
	windowsystem.h)
target_link_libraries(gnubase-ui
	PUBLIC gnubase-engine RmlUi::RmlUi
	PRIVATE Freetype::Freetype gnubase-ui-thirdparty lunasvg::lunasvg SDL3::SDL3)
gnubase_target_defaults(gnubase-ui)
