# SPDX-License-Identifier: GPL-3.0-or-later
add_library(gnubase-scripting STATIC
	runtime.cpp runtime.h)
target_link_libraries(gnubase-scripting PUBLIC gnubase-engine PRIVATE lua)
gnubase_target_defaults(gnubase-scripting)
