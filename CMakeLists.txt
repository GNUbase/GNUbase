# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.25)
project(GNUbase VERSION 0.1.0 LANGUAGES C CXX)
# The VST3 SDK's module loader on macOS is Objective-C++. Enabled before any subdirectory, so every generated
# project has the language's compile and archive rules.
if(APPLE)
	enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The app starts the plugin host process, so all executables go into one folder.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(GNUBASE_WARNINGS_AS_ERRORS "Treat compiler warnings in GNUbase code as errors" ON)
set(GNUBASE_SANITIZERS "" CACHE STRING "Sanitizers for GNUbase code (GCC/Clang), e.g. address,undefined or thread")
option(GNUBASE_CLANG_TIDY "Run clang-tidy on GNUbase code while compiling" OFF)
# Release builds are optimized across files, as they ship. tools/ci-local.sh turns it off: it makes every link of the
# tests take minutes.
option(GNUBASE_LTO "Link-time optimization in Release builds, where the compiler supports it" ON)
option(GNUBASE_TEST_PLUGINS "Build the test plugins (tests/plugins) and the tests that load them" ON)
option(GNUBASE_RENDER_SPIKE "Build the render backend spike (spikes/render-backend, #47)" OFF)

if(GNUBASE_SANITIZERS AND MSVC)
	message(FATAL_ERROR "GNUBASE_SANITIZERS is only supported with GCC and Clang")
endif()
if(GNUBASE_CLANG_TIDY)
	find_program(GNUBASE_CLANG_TIDY_EXE NAMES clang-tidy REQUIRED)
endif()

if(GNUBASE_LTO)
	include(CheckIPOSupported)
	check_ipo_supported(RESULT GNUBASE_IPO_SUPPORTED)
	if(GNUBASE_IPO_SUPPORTED)
		set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
	endif()
endif()

# Third-party code: static libraries, no tests or samples. SYSTEM keeps their warnings out of our builds.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build shared libraries")
set(SDL_SHARED OFF CACHE BOOL "Build a shared version of SDL")
set(SDL_STATIC ON CACHE BOOL "Build a static version of SDL")
set(SDL_TEST_LIBRARY OFF CACHE BOOL "Build the SDL3_test library")
set(JSON_ImplicitConversions OFF CACHE BOOL "Enable implicit conversions") # use get<T>() explicitly
add_subdirectory(ext/SDL EXCLUDE_FROM_ALL SYSTEM)
# FreeType for RmlUi's font engine, the same version on every platform so text looks the same everywhere: TrueType and
# OpenType fonts only, no system libraries (its own zlib for compressed fonts).
foreach(freetypeOption FT_DISABLE_ZLIB FT_DISABLE_BZIP2 FT_DISABLE_PNG FT_DISABLE_HARFBUZZ FT_DISABLE_BROTLI
		FT_DISABLE_HVF SKIP_INSTALL_ALL)
	set(${freetypeOption} ON CACHE BOOL "Set by GNUbase for FreeType")
endforeach()
add_subdirectory(ext/freetype EXCLUDE_FROM_ALL SYSTEM)
add_library(Freetype::Freetype ALIAS freetype)
set(CMAKE_DISABLE_FIND_PACKAGE_Freetype ON) # RmlUi takes the target above, never a FreeType installed on the system
set(RMLUI_FONT_ENGINE "freetype" CACHE STRING "RmlUi font engine" FORCE) # also in build folders made before FreeType
# LunaSVG for RmlUi's SVG plugin: icons and UI graphics are SVG files (#210). The graphics have no text, so it loads no
# fonts from the system and draws the same everywhere, and an SVG can't make it read other files: images inside one
# must be data: URIs.
foreach(lunasvgOption LUNASVG_DISABLE_LOAD_SYSTEM_FONTS LUNASVG_DISABLE_EXTERNAL_RESOURCES
	PLUTOVG_DISABLE_FONT_FACE_CACHE_LOAD)
	set(${lunasvgOption} ON CACHE BOOL "Set by GNUbase for LunaSVG")
endforeach()
set(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "Set by GNUbase for LunaSVG")
add_subdirectory(ext/lunasvg EXCLUDE_FROM_ALL SYSTEM)
set(CMAKE_DISABLE_FIND_PACKAGE_lunasvg ON) # RmlUi takes the target above, never a LunaSVG installed on the system
set(RMLUI_SVG_PLUGIN ON CACHE BOOL "Enable plugin for SVG images" FORCE)
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
# miniz adds /Zi: its files share one .pdb, which compiles through sccache can't write in parallel. Four small files,
# so they skip the compiler cache.
set_target_properties(miniz PROPERTIES C_COMPILER_LAUNCHER "")
# Lua 5.4 for scripting. Compiled as C++, so a Lua error unwinds C++ code with an exception instead of longjmp and
# destructors run. Everything else is the official build: the library without the lua program and the test library.
file(GLOB luaSources ${PROJECT_SOURCE_DIR}/ext/lua/l*.c)
list(FILTER luaSources EXCLUDE REGEX "/(lua|ltests)[.]c$")
set_source_files_properties(${luaSources} PROPERTIES LANGUAGE CXX)
add_library(lua STATIC ${luaSources})
target_include_directories(lua SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/ext/lua)
target_compile_definitions(lua PRIVATE $<$<NOT:$<PLATFORM_ID:Windows>>:LUA_USE_POSIX>)
# cpp-httplib for the MCP server's HTTP transport: header-only, without TLS or compression.
add_library(cpp-httplib INTERFACE)
target_include_directories(cpp-httplib SYSTEM INTERFACE ${PROJECT_SOURCE_DIR}/ext/cpp-httplib)
find_package(Threads REQUIRED)
target_link_libraries(cpp-httplib INTERFACE Threads::Threads $<$<PLATFORM_ID:Windows>:ws2_32>)
# The VST3 SDK's interfaces, header-only: GNUbase implements and calls the interfaces itself. The SDK's C++ namespace
# is read from its header and passed as GNUBASE_VST3_NS, so GNUbase code refers to it only through the alias in
# src/pluginhost/vst3/vst3ns.h (docs/dependencies.md). The plugin host's VST3 adapter needs it in every build.
set(vst3Header "${PROJECT_SOURCE_DIR}/ext/vst3sdk/pluginterfaces/base/funknown.h")
if(NOT EXISTS "${vst3Header}")
	message(FATAL_ERROR "The VST3 SDK is missing. Fetch it with\n"
		"  git submodule update --init --checkout ext/vst3sdk\n"
		"  git -C ext/vst3sdk submodule update --init base cmake pluginterfaces public.sdk")
endif()
file(STRINGS "${vst3Header}" vst3Namespace REGEX "^namespace [A-Za-z_][A-Za-z0-9_]* *{")
list(GET vst3Namespace 0 vst3Namespace)
string(REGEX REPLACE "^namespace ([A-Za-z0-9_]+).*" "\\1" vst3Namespace "${vst3Namespace}")
add_library(vst3-sdk INTERFACE)
target_include_directories(vst3-sdk SYSTEM INTERFACE ${PROJECT_SOURCE_DIR}/ext/vst3sdk)
target_compile_definitions(vst3-sdk INTERFACE GNUBASE_VST3_NS=${vst3Namespace})
# The parts of the SDK the VST3 adapter of the plugin host builds: the interface ids, and the module loader and host
# classes. Test plugins built against the interfaces alone link vst3-interfaces too.
set(vst3sdk ${PROJECT_SOURCE_DIR}/ext/vst3sdk)
add_library(vst3-interfaces STATIC
	${vst3sdk}/pluginterfaces/base/coreiids.cpp
	${vst3sdk}/pluginterfaces/base/funknown.cpp
	${vst3sdk}/public.sdk/source/common/commoniids.cpp
	${vst3sdk}/public.sdk/source/vst/vstinitiids.cpp)
target_link_libraries(vst3-interfaces PUBLIC vst3-sdk)
# Linked into plugin modules too: position-independent, and exporting nothing.
set_target_properties(vst3-interfaces PROPERTIES POSITION_INDEPENDENT_CODE ON CXX_VISIBILITY_PRESET hidden)
if(APPLE)
	set(vst3ModuleLoader ${vst3sdk}/public.sdk/source/vst/hosting/module_mac.mm)
	set_source_files_properties(${vst3ModuleLoader} PROPERTIES COMPILE_OPTIONS -fobjc-arc)
elseif(WIN32)
	set(vst3ModuleLoader ${vst3sdk}/public.sdk/source/vst/hosting/module_win32.cpp)
else()
	set(vst3ModuleLoader ${vst3sdk}/public.sdk/source/vst/hosting/module_linux.cpp)
endif()
add_library(vst3-hosting STATIC
	${vst3ModuleLoader}
	${vst3sdk}/public.sdk/source/vst/hosting/hostclasses.cpp
	${vst3sdk}/public.sdk/source/vst/hosting/module.cpp
	${vst3sdk}/public.sdk/source/vst/hosting/pluginterfacesupport.cpp
	${vst3sdk}/public.sdk/source/common/commonstringconvert.cpp
	${vst3sdk}/public.sdk/source/vst/utility/stringconvert.cpp)
target_link_libraries(vst3-hosting PUBLIC vst3-interfaces ${CMAKE_DL_LIBS})
if(APPLE)
	target_link_libraries(vst3-hosting PUBLIC "-framework Cocoa")
elseif(WIN32)
	target_link_libraries(vst3-hosting PUBLIC ole32 shell32)
endif()

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
add_subdirectory(src/ipc)
add_subdirectory(src/plugin)
add_subdirectory(src/engine)
add_subdirectory(src/mcp)
add_subdirectory(src/scripting)
add_subdirectory(src/ui)
add_subdirectory(src/app)
add_subdirectory(src/pluginhost)
if(GNUBASE_RENDER_SPIKE)
	add_subdirectory(spikes/render-backend)
endif()

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
gnubase_require_no_ui(gnubase-ipc)
gnubase_require_no_ui(gnubase-mcp)
gnubase_require_no_ui(gnubase-scripting)
gnubase_require_no_ui(gnubase-app)
