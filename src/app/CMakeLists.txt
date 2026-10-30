# SPDX-License-Identifier: GPL-3.0-or-later
# Everything of the app that works without the UI, so tests can run it in-process.
add_library(gnubase-app STATIC
	headless.cpp
	headless.h
	options.cpp
	options.h
	resources.cpp
	resources.h
	scripttool.cpp
	scripttool.h)
target_link_libraries(gnubase-app PUBLIC gnubase-engine gnubase-mcp gnubase-scripting PRIVATE $<$<PLATFORM_ID:Windows>:shell32>)
# Builds that run where they were built take the resources (fonts, icons, themes) from the source tree.
target_compile_definitions(gnubase-app PRIVATE GNUBASE_SOURCE_RESOURCES_DIR="${PROJECT_SOURCE_DIR}/resources")
gnubase_target_defaults(gnubase-app)

# The executable adds the UI, which the library leaves out.
add_executable(gnubase main.cpp windowed.cpp windowed.h)
target_link_libraries(gnubase PRIVATE gnubase-app gnubase-ui)
# Debug builds load documents again when their RML or RCSS files change (ui/livereload.h).
target_compile_definitions(gnubase PRIVATE $<$<CONFIG:Debug>:GNUBASE_LIVE_RELOAD>)
gnubase_target_defaults(gnubase)
# The engine starts plugin host processes from the program next to its own (src/pluginhost).
add_dependencies(gnubase gnubase-pluginhost)
