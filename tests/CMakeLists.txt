# SPDX-License-Identifier: GPL-3.0-or-later
add_executable(gnubase-tests main.cpp)
target_link_libraries(gnubase-tests PRIVATE gnubase-engine)
target_compile_definitions(gnubase-tests PRIVATE GNUBASE_VERSION="${PROJECT_VERSION}")
gnubase_set_warnings(gnubase-tests)
add_test(NAME gnubase-tests COMMAND gnubase-tests)
