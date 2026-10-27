# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)
project(GNUbase VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The app starts the plugin host process, so all executables go into one folder.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(GNUBASE_WARNINGS_AS_ERRORS "Treat compiler warnings in GNUbase code as errors" ON)

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
add_subdirectory(ext/SDL EXCLUDE_FROM_ALL SYSTEM)
add_subdirectory(ext/RmlUi EXCLUDE_FROM_ALL SYSTEM)

function(gnubase_set_warnings target)
	if(MSVC)
		target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 $<$<BOOL:${GNUBASE_WARNINGS_AS_ERRORS}>:/WX>)
	else()
		target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic $<$<BOOL:${GNUBASE_WARNINGS_AS_ERRORS}>:-Werror>)
	endif()
endfunction()

add_subdirectory(src/engine)
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
