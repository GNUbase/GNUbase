# SPDX-License-Identifier: GPL-3.0-or-later
add_library(gnubase-testutils STATIC testutils/testutils.cpp testutils/testutils.h)
target_include_directories(gnubase-testutils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(gnubase-testutils PRIVATE
	GNUBASE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
	GNUBASE_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/output"
	$<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
gnubase_set_warnings(gnubase-testutils)

add_executable(gnubase-tests engine_tests.cpp testutils_tests.cpp)
target_link_libraries(gnubase-tests PRIVATE gnubase-engine gnubase-testutils Catch2::Catch2WithMain)
target_compile_definitions(gnubase-tests PRIVATE GNUBASE_VERSION="${PROJECT_VERSION}")
gnubase_set_warnings(gnubase-tests)

# Every TEST_CASE becomes a ctest test; its tags ([unit], [integration], [ui], [slow]) become ctest labels.
# PRE_TEST discovery lets cross-compiled builds (e.g. Windows ARM64 on x64) configure and build.
include(Catch)
catch_discover_tests(gnubase-tests DISCOVERY_MODE PRE_TEST ADD_TAGS_AS_LABELS)
