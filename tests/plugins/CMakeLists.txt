# SPDX-License-Identifier: GPL-3.0-or-later
# GNUbase's test plugins (README.md): every plugin as a CLAP plugin in <build>/plugins/<config>/clap and as a VST3
# plugin in <build>/plugins/<config>/vst3, with each platform's layout. All of them are also linked into one static
# library for the test format of the tests' plugin host (tests/pluginhost/testformat.h).

# The CLAP SDK is header-only.
add_library(clap-sdk INTERFACE)
target_include_directories(clap-sdk SYSTEM INTERFACE ${PROJECT_SOURCE_DIR}/ext/clap/include)

set(GNUBASE_TEST_PLUGINS gain synth latency paramstress crash hang stateblob)
set(GNUBASE_TEST_PLUGINS_DIR "${CMAKE_BINARY_DIR}/plugins/$<CONFIG>" PARENT_SCOPE)

# The format-independent part and each format's wrapper, compiled once and linked into every plugin. Object libraries,
# because nothing in a plugin's own code refers to the wrapper's entry points, so a static library would drop them.
add_library(gnubase-testplugin-clap OBJECT common/testplugin.cpp common/testplugin.h clap/wrapper.cpp)
target_link_libraries(gnubase-testplugin-clap PUBLIC clap-sdk)
add_library(gnubase-testplugin-vst3 OBJECT common/testplugin.cpp common/testplugin.h vst3/wrapper.cpp)
target_link_libraries(gnubase-testplugin-vst3 PUBLIC vst3-sdk)
target_include_directories(gnubase-testplugin-vst3 PRIVATE ${PROJECT_SOURCE_DIR}/src)

# The VST3 bundle's architecture folder on Windows and Linux (Contents/<architecture>/), as the VST3 format names it.
if(WIN32)
	if(CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "ARM64")
		set(vst3Architecture arm64-win)
	else()
		set(vst3Architecture x86_64-win)
	endif()
elseif(NOT APPLE)
	if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
		set(vst3Architecture aarch64-linux)
	else()
		set(vst3Architecture x86_64-linux)
	endif()
endif()

# Each format's entry points, the only symbols a plugin exports. Everything else has hidden visibility, and on Linux a
# version script and on macOS an exported symbols list hide what the compiler still exports, such as the standard
# library's template code. Windows exports only what the code marks dllexport.
set(clapExports clap_entry)
if(WIN32)
	set(vst3Exports GetPluginFactory InitDll ExitDll)
elseif(APPLE)
	set(vst3Exports GetPluginFactory bundleEntry bundleExit)
else()
	set(vst3Exports GetPluginFactory ModuleEntry ModuleExit)
endif()
foreach(format clap vst3)
	set(${format}LinkOptions "")
	if(APPLE)
		set(exportsFile "${CMAKE_CURRENT_BINARY_DIR}/${format}-exports.txt")
		list(TRANSFORM ${format}Exports PREPEND _ OUTPUT_VARIABLE symbols)
		list(JOIN symbols "\n" symbols)
		file(CONFIGURE OUTPUT "${exportsFile}" CONTENT "@symbols@\n" @ONLY)
		set(${format}LinkOptions "LINKER:-exported_symbols_list,${exportsFile}")
	elseif(NOT WIN32)
		set(exportsFile "${CMAKE_CURRENT_BINARY_DIR}/${format}-exports.map")
		list(JOIN ${format}Exports "; " symbols)
		file(CONFIGURE OUTPUT "${exportsFile}" CONTENT "{\n\tglobal: @symbols@;\n\tlocal: *;\n};\n" @ONLY)
		set(${format}LinkOptions "LINKER:--version-script=${exportsFile}")
	endif()
	set(${format}ExportsFile "${exportsFile}")
endforeach()

# A test per plugin checks its exports with the toolchain's symbol lister: nm, or dumpbin next to the MSVC compiler.
if(MSVC)
	get_filename_component(msvcBin "${CMAKE_CXX_COMPILER}" DIRECTORY)
	find_program(GNUBASE_DUMPBIN dumpbin HINTS "${msvcBin}" NO_DEFAULT_PATH)
	set(symbolLister "${GNUBASE_DUMPBIN}")
else()
	set(symbolLister "${CMAKE_NM}")
endif()

set(pluginTargets "")
foreach(name IN LISTS GNUBASE_TEST_PLUGINS)
	set(clapTarget gnubase-test-${name}-clap)
	add_library(${clapTarget} MODULE common/${name}.cpp)
	target_link_libraries(${clapTarget} PRIVATE gnubase-testplugin-clap)
	set_target_properties(${clapTarget} PROPERTIES
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/$<CONFIG>/clap")

	set(vst3Target gnubase-test-${name}-vst3)
	add_library(${vst3Target} MODULE common/${name}.cpp)
	target_link_libraries(${vst3Target} PRIVATE gnubase-testplugin-vst3)

	# Both formats have the same name, so MSVC's import libraries go into a folder per target: in a shared one, parallel
	# links (Ninja) write the same .lib and .exp.
	foreach(target ${clapTarget} ${vst3Target})
		set_target_properties(${target} PROPERTIES OUTPUT_NAME gnubase-test-${name} PREFIX ""
			ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${target}")
	endforeach()
	if(APPLE)
		# Bundle folders: <name>.clap or <name>.vst3/Contents/MacOS/<name> with Contents/Info.plist.
		foreach(format clap vst3)
			set_target_properties(gnubase-test-${name}-${format} PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION ${format}
				MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/common/Info.plist.in
				MACOSX_BUNDLE_GUI_IDENTIFIER org.gnubase.test.${name}.${format}
				MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
				MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
		endforeach()
		set_target_properties(${vst3Target} PROPERTIES
			LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/$<CONFIG>/vst3")
	else()
		# A single .clap file; the VST3 binary inside a <name>.vst3 folder: .vst3 on Windows, .so on Linux.
		set_target_properties(${clapTarget} PROPERTIES SUFFIX .clap)
		if(WIN32)
			set_target_properties(${vst3Target} PROPERTIES SUFFIX .vst3)
		else()
			set_target_properties(${vst3Target} PROPERTIES SUFFIX .so)
		endif()
		set_target_properties(${vst3Target} PROPERTIES LIBRARY_OUTPUT_DIRECTORY
			"${CMAKE_BINARY_DIR}/plugins/$<CONFIG>/vst3/gnubase-test-${name}.vst3/Contents/${vst3Architecture}")
	endif()
	list(APPEND pluginTargets ${clapTarget} ${vst3Target})

	foreach(format clap vst3)
		set(target gnubase-test-${name}-${format})
		if(${format}LinkOptions)
			target_link_options(${target} PRIVATE "${${format}LinkOptions}")
			set_target_properties(${target} PROPERTIES LINK_DEPENDS "${${format}ExportsFile}")
		endif()
		if(symbolLister)
			string(TOUPPER ${format} formatName)
			list(JOIN ${format}Exports "," expected)
			set(test "the ${formatName} test plugin ${name} exports only its entry points")
			add_test(NAME "${test}" COMMAND ${CMAKE_COMMAND} "-DLISTER=${symbolLister}" "-DFILE=$<TARGET_FILE:${target}>"
				"-DEXPECTED=${expected}" -P "${CMAKE_CURRENT_SOURCE_DIR}/check-exports.cmake")
			set_tests_properties("${test}" PROPERTIES LABELS integration)
		endif()
	endforeach()
endforeach()

foreach(target IN LISTS pluginTargets ITEMS gnubase-testplugin-clap gnubase-testplugin-vst3)
	set_target_properties(${target} PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
		POSITION_INDEPENDENT_CODE ON)
	gnubase_target_defaults(${target})
endforeach()

add_custom_target(gnubase-test-plugins DEPENDS ${pluginTargets})

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
