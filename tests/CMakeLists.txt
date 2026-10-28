# SPDX-License-Identifier: GPL-3.0-or-later
add_library(gnubase-testutils STATIC testutils/testutils.cpp testutils/testutils.h)
target_include_directories(gnubase-testutils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(gnubase-testutils PRIVATE
	GNUBASE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
	GNUBASE_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/output"
	$<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
gnubase_target_defaults(gnubase-testutils)

add_executable(gnubase-tests analysis_tests.cpp app_tests.cpp audio_tests.cpp automation_tests.cpp audiofile_tests.cpp coverage_tests.cpp dawproject_tests.cpp document_tests.cpp engine_tests.cpp graph_tests.cpp history_tests.cpp log_tests.cpp mcp_tests.cpp mcptools_tests.cpp midiedit_tests.cpp processor_tests.cpp rt_tests.cpp scripting_tests.cpp streaming_tests.cpp testing_tests.cpp testutils_tests.cpp theme_tests.cpp time_tests.cpp
	debugreports.cpp midi_tests.cpp playback_tests.cpp sequencing_tests.cpp transport_tests.cpp)
target_sources(gnubase-tests PRIVATE device_tests.cpp)
target_link_libraries(gnubase-tests PRIVATE gnubase-app gnubase-engine gnubase-mcp gnubase-scripting gnubase-testutils Catch2::Catch2WithMain miniz pugixml::pugixml
	cpp-httplib)

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
	# The app itself, for end-to-end tests of headless mode.
	target_compile_definitions(gnubase-tests PRIVATE GNUBASE_APP="$<TARGET_FILE:gnubase>")
	add_dependencies(gnubase-tests gnubase)

	# Every tests/lua/*.lua is a ctest test that the app runs headless; its test cases use gnubase.test (docs/lua.md).
	# They run in the source folder, so a failure names tests/lua/<file>.lua and the line.
	file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/output")
	file(GLOB luaTests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/lua/*.lua")
	foreach(luaTest IN LISTS luaTests)
		file(RELATIVE_PATH script "${PROJECT_SOURCE_DIR}" "${luaTest}")
		add_test(NAME "${script}" COMMAND gnubase --headless --script "${script}" WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
		set_tests_properties("${script}" PROPERTIES LABELS "integration;lua" ENVIRONMENT
			"GNUBASE_GOLDEN_DIR=${CMAKE_CURRENT_SOURCE_DIR}/golden;GNUBASE_TEST_OUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/output")
	endforeach()
endif()
target_compile_definitions(gnubase-tests PRIVATE GNUBASE_VERSION="${PROJECT_VERSION}"
	GNUBASE_DOCS_DIR="${PROJECT_SOURCE_DIR}/docs"
	GNUBASE_RESOURCES_DIR="${PROJECT_SOURCE_DIR}/resources"
	$<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
	GNUBASE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
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
function(gnubase_add_shell_test name script)
	add_test(NAME "${name}" COMMAND "${GNUBASE_SH}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/${script}" "${PROJECT_SOURCE_DIR}")
	set_tests_properties("${name}" PROPERTIES LABELS integration)
endfunction()
gnubase_add_shell_test("the keyword guard blocks private keywords in commits and pull requests" keyword_guard_tests.sh)
gnubase_add_shell_test("the license header check lists files without the SPDX header" spdx_check_tests.sh)
