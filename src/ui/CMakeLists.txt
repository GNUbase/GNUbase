# SPDX-License-Identifier: GPL-3.0-or-later
add_library(gnubase-ui STATIC ui.cpp ui.h)
target_link_libraries(gnubase-ui PUBLIC gnubase-engine PRIVATE SDL3::SDL3 RmlUi::RmlUi)
gnubase_target_defaults(gnubase-ui)
