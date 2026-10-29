# SPDX-License-Identifier: GPL-3.0-or-later
add_library(gnubase-testutils STATIC testutils/fakeui.cpp testutils/fakeui.h testutils/goldenimage.cpp
	testutils/goldenimage.h testutils/testutils.cpp testutils/testutils.h)
target_include_directories(gnubase-testutils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gnubase-testutils PUBLIC gnubase-ui)
target_compile_definitions(gnubase-testutils PRIVATE
	GNUBASE_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
	GNUBASE_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/output"
	$<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
gnubase_target_defaults(gnubase-testutils)

# Tests, and the app they start, keep their user settings in the build, never in the user's own settings folder.
set(settingsEnvironment "GNUBASE_SETTINGS_DIR=${CMAKE_CURRENT_BINARY_DIR}/output/settings")

add_executable(gnubase-tests analysis_tests.cpp app_tests.cpp audio_tests.cpp audiofile_tests.cpp automation_tests.cpp backup_tests.cpp coverage_tests.cpp dawproject_tests.cpp document_tests.cpp engine_tests.cpp fade_tests.cpp graph_tests.cpp history_tests.cpp log_tests.cpp marker_tests.cpp mcp_tests.cpp mcpevents_tests.cpp mcptools_tests.cpp midiedit_tests.cpp midifile_tests.cpp processor_tests.cpp rt_tests.cpp scripting_tests.cpp settings_tests.cpp streaming_tests.cpp testing_tests.cpp testutils_tests.cpp theme_tests.cpp time_tests.cpp
	debugreports.cpp midi_tests.cpp midifunctions_tests.cpp multicore_tests.cpp peaks_tests.cpp performance_tests.cpp playback_tests.cpp quantize_tests.cpp sequencing_tests.cpp timestretch_tests.cpp transport_tests.cpp)
target_sources(gnubase-tests PRIVATE device_tests.cpp effects_tests.cpp ipc_tests.cpp ipc_process_tests.cpp plugin_api_tests.cpp)
target_link_libraries(gnubase-tests PRIVATE gnubase-app gnubase-engine gnubase-ipc gnubase-mcp gnubase-plugin gnubase-pluginhost-core gnubase-scripting gnubase-testutils Catch2::Catch2WithMain miniz pugixml::pugixml
	cpp-httplib)

# The UI's tests (#48, #49), apart from the lists above so that both can grow without conflicts.
target_sources(gnubase-tests PRIVATE rmlui_tests.cpp shell_tests.cpp window_tests.cpp)
target_link_libraries(gnubase-tests PRIVATE gnubase-ui SDL3::SDL3)

# IPC tests run a helper process that plays a plugin host (tests/ipcpeer.cpp).
add_executable(gnubase-ipcpeer ipcpeer.cpp)
target_link_libraries(gnubase-ipcpeer PRIVATE gnubase-ipc)
gnubase_target_defaults(gnubase-ipcpeer)
target_compile_definitions(gnubase-tests PRIVATE GNUBASE_IPCPEER="$<TARGET_FILE:gnubase-ipcpeer>")
add_dependencies(gnubase-tests gnubase-ipcpeer)
# The test plugins and the loader tests that load them into the test process (plugins/README.md).
if(GNUBASE_TEST_PLUGINS)
	add_subdirectory(plugins)
	target_sources(gnubase-tests PRIVATE clap_plugin_tests.cpp vst3_plugin_tests.cpp)
	target_link_libraries(gnubase-tests PRIVATE clap-sdk vst3-sdk ${CMAKE_DL_LIBS})
	target_compile_definitions(gnubase-tests PRIVATE GNUBASE_TEST_PLUGINS_DIR="${GNUBASE_TEST_PLUGINS_DIR}")
	add_dependencies(gnubase-tests gnubase-test-plugins)
	# The VST3 adapter's tests and their plugin (vst3adapter/README.md).
	add_subdirectory(vst3adapter)
	target_sources(gnubase-tests PRIVATE vst3_adapter_tests.cpp)
	target_link_libraries(gnubase-tests PRIVATE gnubase-pluginhost-vst3)
	target_compile_definitions(gnubase-tests PRIVATE GNUBASE_VST3_ADAPTERTEST="${GNUBASE_VST3_ADAPTERTEST}")
	add_dependencies(gnubase-tests gnubase-vst3-adaptertest)

	# Plugin hosting tests start the tests' plugin host: the shipped host's code plus the test format, which runs
	# the same test plugins (pluginhost/testformat.h). The shipped host has no test plugins.
	add_library(gnubase-testformat STATIC pluginhost/testformat.cpp pluginhost/testformat.h)
	target_include_directories(gnubase-testformat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
	target_link_libraries(gnubase-testformat PUBLIC gnubase-pluginhost-core PRIVATE gnubase-test-plugins-static)
	gnubase_target_defaults(gnubase-testformat)
	add_executable(gnubase-testpluginhost pluginhost/main.cpp)
	target_link_libraries(gnubase-testpluginhost PRIVATE gnubase-testformat)
	gnubase_target_defaults(gnubase-testpluginhost)
	target_sources(gnubase-tests PRIVATE pluginhost_tests.cpp)
	target_link_libraries(gnubase-tests PRIVATE gnubase-testformat)
	target_compile_definitions(gnubase-tests PRIVATE GNUBASE_PLUGINHOST="$<TARGET_FILE:gnubase-testpluginhost>")
	add_dependencies(gnubase-tests gnubase-testpluginhost gnubase-pluginhost)
else()
	target_compile_definitions(gnubase-tests PRIVATE GNUBASE_NO_TEST_PLUGINS)
endif()
# Engine benchmarks (#212, #24): not a test, run by hand or by CI's benchmark job.
add_executable(gnubase-bench bench/bench.cpp)
target_link_libraries(gnubase-bench PRIVATE gnubase-engine)
gnubase_target_defaults(gnubase-bench)

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
endif()

# Every tests/lua/*.lua is a ctest test that the app runs headless; its test cases use gnubase.test (docs/lua.md).
# They run in the source folder, so a failure names tests/lua/<file>.lua and the line. Sanitizer builds run them too,
# with the app built under the same sanitizers, so building gnubase-tests builds the app.
add_dependencies(gnubase-tests gnubase)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/output")
file(GLOB luaTests CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/lua/*.lua")
foreach(luaTest IN LISTS luaTests)
	file(RELATIVE_PATH script "${PROJECT_SOURCE_DIR}" "${luaTest}")
	add_test(NAME "${script}" COMMAND gnubase --headless --script "${script}" WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}")
	set_tests_properties("${script}" PROPERTIES LABELS "integration;lua" TIMEOUT 300 ENVIRONMENT
		"GNUBASE_GOLDEN_DIR=${CMAKE_CURRENT_SOURCE_DIR}/golden;GNUBASE_TEST_OUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/output;${settingsEnvironment}")
endforeach()

target_compile_definitions(gnubase-tests PRIVATE GNUBASE_VERSION="${PROJECT_VERSION}"
	GNUBASE_DOCS_DIR="${PROJECT_SOURCE_DIR}/docs"
	GNUBASE_RESOURCES_DIR="${PROJECT_SOURCE_DIR}/resources"
	# Lato (SIL OFL) from RmlUi's samples, for text in tests until GNUbase bundles its own fonts (#207).
	GNUBASE_TEST_FONTS_DIR="${PROJECT_SOURCE_DIR}/ext/RmlUi/Samples/assets"
	$<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>
	GNUBASE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
gnubase_target_defaults(gnubase-tests)
# Sanitizer builds are too slow for benchmarks.
if(GNUBASE_SANITIZERS)
	target_compile_definitions(gnubase-tests PRIVATE GNUBASE_SANITIZED)
endif()

# Every TEST_CASE becomes a ctest test; its tags ([unit], [integration], [ui], [slow]) become ctest labels.
# PRE_TEST discovery lets cross-compiled builds (e.g. Windows ARM64 on x64) configure and build.
include(Catch)
# A test that hangs fails after five minutes instead of blocking the run.
catch_discover_tests(gnubase-tests DISCOVERY_MODE PRE_TEST ADD_TAGS_AS_LABELS PROPERTIES TIMEOUT 300 ENVIRONMENT "${settingsEnvironment}")

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

# Benchmarks print measurements for design notes; they are not part of the test run.
option(GNUBASE_BENCHMARKS "Build the benchmarks in tests/bench" OFF)
if(GNUBASE_BENCHMARKS)
	add_subdirectory(bench)
endif()
