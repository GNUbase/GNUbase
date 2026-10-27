# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(gnubase-pluginhost main.cpp)
target_link_libraries(gnubase-pluginhost PRIVATE gnubase-platform)
gnubase_target_defaults(gnubase-pluginhost)
