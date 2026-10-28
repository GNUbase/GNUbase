# SPDX-License-Identifier: GPL-3.0-or-later
# The VST3 plugin of the VST3 adapter tests (README.md), as a .vst3 module in <build>/plugins/<config>/vst3adapter
# with each platform's layout: a bundle folder on macOS and Linux, a single .vst3 library file on Windows.
set(name gnubase-vst3-adaptertest)
set(GNUBASE_VST3_ADAPTERTEST "${CMAKE_BINARY_DIR}/plugins/$<CONFIG>/vst3adapter/${name}.vst3" PARENT_SCOPE)
add_library(${name} MODULE adaptertest.cpp adaptertest.h)
target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(${name} PRIVATE vst3-interfaces)
set_target_properties(${name} PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
	POSITION_INDEPENDENT_CODE ON)
if(APPLE)
	set_target_properties(${name} PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION vst3
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/$<CONFIG>/vst3adapter"
		MACOSX_BUNDLE_INFO_PLIST ${PROJECT_SOURCE_DIR}/tests/plugins/clap/Info.plist.in
		MACOSX_BUNDLE_GUI_IDENTIFIER org.gnubase.test.vst3adapter
		MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
		MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
elseif(WIN32)
	set_target_properties(${name} PROPERTIES SUFFIX .vst3
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/$<CONFIG>/vst3adapter")
else()
	# <name>.vst3/Contents/<machine>-linux/<name>.so, the machine as uname -m names it.
	set_target_properties(${name} PROPERTIES SUFFIX .so
		LIBRARY_OUTPUT_DIRECTORY
		"${CMAKE_BINARY_DIR}/plugins/$<CONFIG>/vst3adapter/${name}.vst3/Contents/${CMAKE_SYSTEM_PROCESSOR}-linux")
endif()
gnubase_target_defaults(${name})
