# SPDX-License-Identifier: GPL-3.0-or-later
add_library(gnubase-engine STATIC engine.cpp engine.h)
target_include_directories(gnubase-engine PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(gnubase-engine PRIVATE GNUBASE_VERSION="${PROJECT_VERSION}")
gnubase_set_warnings(gnubase-engine)
