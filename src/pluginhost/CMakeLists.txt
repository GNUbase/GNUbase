# SPDX-License-Identifier: GPL-3.0-or-later
# The VST3 adapter (vst3/vst3format.h): the only code that talks to VST3 plugins, with the VST3 SDK's interfaces and
# module loader.
add_library(gnubase-pluginhost-vst3 STATIC
	vst3/hostobjects.cpp vst3/hostobjects.h
	vst3/presetfile.cpp vst3/presetfile.h
	vst3/vst3format.cpp vst3/vst3format.h
	vst3/vst3ns.h
	vst3/vst3plugin.cpp vst3/vst3plugin.h)
target_include_directories(gnubase-pluginhost-vst3 PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(gnubase-pluginhost-vst3 PUBLIC gnubase-plugin gnubase-platform PRIVATE vst3-hosting)
gnubase_target_defaults(gnubase-pluginhost-vst3)

# Everything of the plugin host process except main: the control loop and the format adapters. The tests build a
# host of their own from it with a test format added (tests/pluginhost).
add_library(gnubase-pluginhost-core STATIC
	formats.cpp formats.h
	host.cpp host.h)
target_link_libraries(gnubase-pluginhost-core PUBLIC gnubase-plugin-remote gnubase-platform gnubase-pluginhost-vst3)
gnubase_target_defaults(gnubase-pluginhost-core)

add_executable(gnubase-pluginhost main.cpp)
target_link_libraries(gnubase-pluginhost PRIVATE gnubase-pluginhost-core)
gnubase_target_defaults(gnubase-pluginhost)
