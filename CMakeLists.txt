# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)
project(GNUbase VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The app starts the plugin host process, so all executables go into one folder.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(GNUBASE_WARNINGS_AS_ERRORS "Treat compiler warnings in GNUbase code as errors" ON)
set(GNUBASE_SANITIZERS "" CACHE STRING "Sanitizers for GNUbase code (GCC/Clang), e.g. address,undefined or thread")
option(GNUBASE_CLANG_TIDY "Run clang-tidy on GNUbase code while compiling" OFF)

if(GNUBASE_SANITIZERS AND MSVC)
	message(FATAL_ERROR "GNUBASE_SANITIZERS is only supported with GCC and Clang")
endif()
if(GNUBASE_CLANG_TIDY)
	find_program(GNUBASE_CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT GNUBASE_IPO_SUPPORTED)
if(GNUBASE_IPO_SUPPORTED)
	set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
endif()

# Third-party code: static libraries, no tests or samples. SYSTEM keeps their warnings out of our builds.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries")
set(SDL_SHARED OFF CACHE BOOL "Build a shared version of SDL")
set(SDL_STATIC ON CACHE BOOL "Build a static version of SDL")
set(SDL_TEST_LIBRARY OFF CACHE BOOL "Build the SDL3_test library")
# ponytail: no font engine yet; switch to "freetype" when FreeType lands (#207)
set(RMLUI_FONT_ENGINE "none" CACHE STRING "RmlUi font engine")
set(JSON_ImplicitConversions OFF CACHE BOOL "Enable implicit conversions") # use get<T>() explicitly
add_subdirectory(ext/SDL EXCLUDE_FROM_ALL SYSTEM)
add_subdirectory(ext/RmlUi EXCLUDE_FROM_ALL SYSTEM)
add_subdirectory(ext/Catch2 EXCLUDE_FROM_ALL SYSTEM)
add_subdirectory(ext/json EXCLUDE_FROM_ALL SYSTEM)
# libFLAC only, for writing FLAC files (dr_flac reads them): no C++ library, programs, tests, docs or Ogg.
foreach(flacOption BUILD_CXXLIBS BUILD_PROGRAMS BUILD_EXAMPLES BUILD_TESTING BUILD_DOCS INSTALL_MANPAGES
		INSTALL_PKGCONFIG_MODULES INSTALL_CMAKE_CONFIG_MODULE WITH_OGG ENABLE_MULTITHREADING)
	set(${flacOption} OFF CACHE BOOL "Set by GNUbase for libFLAC")
endforeach()
add_subdirectory(ext/flac EXCLUDE_FROM_ALL SYSTEM)
# pugixml and miniz for DAWproject files: XML inside a ZIP file.
set(PUGIXML_INSTALL OFF CACHE BOOL "Set by GNUbase for pugixml")
add_subdirectory(ext/pugixml EXCLUDE_FROM_ALL SYSTEM)
add_subdirectory(ext/miniz EXCLUDE_FROM_ALL SYSTEM)
# cpp-httplib for the MCP server's HTTP transport: header-only, without TLS or compression.
add_library(cpp-httplib INTERFACE)
target_include_directories(cpp-httplib SYSTEM INTERFACE ${PROJECT_SOURCE_DIR}/ext/cpp-httplib)
find_package(Threads REQUIRED)
target_link_libraries(cpp-httplib INTERFACE Threads::Threads $<$<PLATFORM_ID:Windows>:ws2_32>)

# Settings for every GNUbase target; third-party code is not affected.
function(gnubase_target_defaults target)
	if(MSVC)
		target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 $<$<BOOL:${GNUBASE_WARNINGS_AS_ERRORS}>:/WX>)
	else()
		target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic $<$<BOOL:${GNUBASE_WARNINGS_AS_ERRORS}>:-Werror>)
		# No fused multiply-adds where the source has none: they round differently, and rendering must give the same
		# samples on every platform (CLAUDE.md, rule 6). GCC fuses by default on ARM64.
		target_compile_options(${target} PRIVATE -ffp-contract=off)
	endif()
	if(GNUBASE_SANITIZERS)
		target_compile_options(${target} PRIVATE -fsanitize=${GNUBASE_SANITIZERS} -fno-sanitize-recover=all -fno-omit-frame-pointer)
		target_link_options(${target} PRIVATE -fsanitize=${GNUBASE_SANITIZERS})
	endif()
	if(GNUBASE_CLANG_TIDY)
		set_target_properties(${target} PROPERTIES CXX_CLANG_TIDY "${GNUBASE_CLANG_TIDY_EXE};--warnings-as-errors=*")
	endif()
endfunction()

add_subdirectory(src/platform)
add_subdirectory(src/engine)
add_subdirectory(src/mcp)
add_subdirectory(src/ui)
add_subdirectory(src/app)
add_subdirectory(src/pluginhost)

enable_testing()
add_subdirectory(tests)

# The engine must never depend on UI code (CLAUDE.md, rule 1). Walks all transitive link dependencies.
function(gnubase_require_no_ui target)
	set(pending ${target})
	set(seen "")
	while(pending)
		list(POP_FRONT pending current)
		if(current IN_LIST seen OR NOT TARGET ${current})
			continue()
		endif()
		list(APPEND seen ${current})
		get_target_property(libs ${current} LINK_LIBRARIES)
		get_target_property(interfaceLibs ${current} INTERFACE_LINK_LIBRARIES)
		foreach(lib IN LISTS libs interfaceLibs)
			string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" lib "${lib}")
			if(lib MATCHES "SDL|RmlUi|rmlui|gnubase-ui")
				message(FATAL_ERROR "${target} must not depend on UI code, but ${current} links ${lib}")
			endif()
			list(APPEND pending ${lib})
		endforeach()
	endwhile()
endfunction()
gnubase_require_no_ui(gnubase-engine)
gnubase_require_no_ui(gnubase-mcp)
gnubase_require_no_ui(gnubase-app)
