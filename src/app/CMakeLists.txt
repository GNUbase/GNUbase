# SPDX-License-Identifier: GPL-3.0-or-later
# Everything of the app that works without the UI, so tests can run it in-process.
add_library(gnubase-app STATIC
	headless.cpp headless.h
	options.cpp options.h)
target_link_libraries(gnubase-app PUBLIC gnubase-engine gnubase-mcp gnubase-scripting)
gnubase_target_defaults(gnubase-app)

add_executable(gnubase main.cpp)
target_link_libraries(gnubase PRIVATE gnubase-app gnubase-ui)
gnubase_target_defaults(gnubase)
