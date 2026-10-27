# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(gnubase main.cpp)
target_link_libraries(gnubase PRIVATE gnubase-ui)
gnubase_target_defaults(gnubase)
