# SPDX-License-Identifier: GPL-3.0-or-later
# Everything of the plugin host process except main: the control loop and the format adapters. The tests build a
# host of their own from it with a test format added (tests/pluginhost).
add_library(gnubase-pluginhost-core STATIC
	formats.cpp formats.h
	host.cpp host.h)
target_link_libraries(gnubase-pluginhost-core PUBLIC gnubase-plugin-remote gnubase-platform)
gnubase_target_defaults(gnubase-pluginhost-core)

add_executable(gnubase-pluginhost main.cpp)
target_link_libraries(gnubase-pluginhost PRIVATE gnubase-pluginhost-core)
gnubase_target_defaults(gnubase-pluginhost)
