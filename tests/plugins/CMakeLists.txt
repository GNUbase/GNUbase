# SPDX-License-Identifier: GPL-3.0-or-later
# GNUbase's test plugins (README.md): one CLAP plugin per file in <build>/plugins/<config>/clap, with each platform's
# layout: a .clap bundle folder on macOS, a single .clap library file on Windows and Linux. All of them are also linked
# into one static library for the test format of the tests' plugin host (tests/pluginhost/testformat.h).

# The CLAP SDK is header-only.
add_library(clap-sdk INTERFACE)
target_include_directories(clap-sdk SYSTEM INTERFACE ${PROJECT_SOURCE_DIR}/ext/clap/include)

set(GNUBASE_TEST_PLUGINS gain synth latency paramstress crash hang stateblob)
set(GNUBASE_TEST_PLUGINS_DIR "${CMAKE_BINARY_DIR}/plugins/$<CONFIG>" PARENT_SCOPE)

# The format-independent part and the CLAP wrapper, compiled once and linked into every plugin. Object libraries,
# because nothing in a plugin's own code refers to the wrapper's entry point, so a static library would drop it.
add_library(gnubase-testplugin-clap OBJECT common/testplugin.cpp common/testplugin.h clap/wrapper.cpp)
target_link_libraries(gnubase-testplugin-clap PUBLIC clap-sdk)

set(clapTargets "")
foreach(name IN LISTS GNUBASE_TEST_PLUGINS)
	set(target gnubase-test-${name}-clap)
	add_library(${target} MODULE common/${name}.cpp)
	target_link_libraries(${target} PRIVATE gnubase-testplugin-clap)
	set_target_properties(${target} PROPERTIES OUTPUT_NAME gnubase-test-${name} PREFIX ""
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/$<CONFIG>/clap")
	if(APPLE)
		set_target_properties(${target} PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION clap
			MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/clap/Info.plist.in
			MACOSX_BUNDLE_GUI_IDENTIFIER org.gnubase.test.${name}
			MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
			MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
	else()
		set_target_properties(${target} PROPERTIES SUFFIX .clap)
	endif()
	list(APPEND clapTargets ${target})
endforeach()

# Only the entry point is exported.
foreach(target IN LISTS clapTargets ITEMS gnubase-testplugin-clap)
	set_target_properties(${target} PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
		POSITION_INDEPENDENT_CODE ON)
	gnubase_target_defaults(${target})
endforeach()

add_custom_target(gnubase-test-plugins DEPENDS ${clapTargets})

# The static library: each plugin's pluginDescriptor() and createPlugin() get the plugin's name as a prefix, e.g.
# gainPluginDescriptor(), so they don't collide in one program.
set(staticObjects "")
foreach(name IN LISTS GNUBASE_TEST_PLUGINS)
	set(target gnubase-test-${name}-static)
	add_library(${target} OBJECT common/${name}.cpp)
	target_compile_definitions(${target} PRIVATE pluginDescriptor=${name}PluginDescriptor
		createPlugin=${name}CreatePlugin)
	gnubase_target_defaults(${target})
	list(APPEND staticObjects $<TARGET_OBJECTS:${target}>)
endforeach()
add_library(gnubase-test-plugins-static STATIC common/testplugin.cpp common/testplugin.h ${staticObjects})
target_include_directories(gnubase-test-plugins-static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/common)
gnubase_target_defaults(gnubase-test-plugins-static)
