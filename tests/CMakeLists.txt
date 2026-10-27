# SPDX-License-Identifier: GPL-3.0-or-later
add_library(gnubase-testutils STATIC testutils/testutils.cpp testutils/testutils.h)
target_include_directories(gnubase-testutils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(gnubase-testutils PRIVATE
	GNUBASE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
	GNUBASE_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/output"
	$<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
gnubase_target_defaults(gnubase-testutils)

add_executable(gnubase-tests engine_tests.cpp log_tests.cpp rt_tests.cpp testutils_tests.cpp time_tests.cpp)
target_link_libraries(gnubase-tests PRIVATE gnubase-engine gnubase-testutils Catch2::Catch2WithMain)

# ThreadSanitizer defines its own operator new/delete, which the RT checker replaces; every other build runs it.
if(NOT GNUBASE_SANITIZERS MATCHES "thread")
	target_sources(gnubase-tests PRIVATE rtcheck_tests.cpp)
	target_link_libraries(gnubase-tests PRIVATE gnubase-rtcheck)
endif()
# Crash tests run a helper process that crashes. Sanitizers install their own crash handlers, so their builds skip it.
if(NOT GNUBASE_SANITIZERS)
	add_executable(gnubase-crashtest crashtest.cpp)
	target_link_libraries(gnubase-crashtest PRIVATE gnubase-platform)
	gnubase_target_defaults(gnubase-crashtest)
	target_sources(gnubase-tests PRIVATE crash_tests.cpp)
	target_compile_definitions(gnubase-tests PRIVATE GNUBASE_CRASHTEST="$<TARGET_FILE:gnubase-crashtest>")
	add_dependencies(gnubase-tests gnubase-crashtest)
endif()
target_compile_definitions(gnubase-tests PRIVATE GNUBASE_VERSION="${PROJECT_VERSION}")
gnubase_target_defaults(gnubase-tests)

# Every TEST_CASE becomes a ctest test; its tags ([unit], [integration], [ui], [slow]) become ctest labels.
# PRE_TEST discovery lets cross-compiled builds (e.g. Windows ARM64 on x64) configure and build.
include(Catch)
catch_discover_tests(gnubase-tests DISCOVERY_MODE PRE_TEST ADD_TAGS_AS_LABELS)

# Shell tests for the scripts in tools/. They use the sh next to Git, which on Windows is the one Git runs hooks with
# (Git/bin, found from Git/cmd or Git/mingw64/bin).
find_package(Git REQUIRED)
get_filename_component(gitBinDir "${GIT_EXECUTABLE}" DIRECTORY)
find_program(GNUBASE_SH sh HINTS "${gitBinDir}/../bin" "${gitBinDir}/../../bin" REQUIRED)
set(keywordGuardTest "the keyword guard blocks private keywords in commits and pull requests")
add_test(NAME "${keywordGuardTest}"
	COMMAND "${GNUBASE_SH}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/keyword_guard_tests.sh" "${PROJECT_SOURCE_DIR}")
set_tests_properties("${keywordGuardTest}" PROPERTIES LABELS integration)
