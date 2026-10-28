# SPDX-License-Identifier: GPL-3.0-or-later
# Everything of the plugin host process except main: the control loop, the format adapters and the built-in test
# plugins. Tests link it to run the test plugins in-process.
add_library(gnubase-pluginhost-core STATIC
	formats.cpp formats.h
	host.cpp host.h
	testplugins.cpp testplugins.h)
target_link_libraries(gnubase-pluginhost-core PUBLIC gnubase-plugin-remote gnubase-platform)
gnubase_target_defaults(gnubase-pluginhost-core)

add_executable(gnubase-pluginhost main.cpp)
target_link_libraries(gnubase-pluginhost PRIVATE gnubase-pluginhost-core)
gnubase_target_defaults(gnubase-pluginhost)
