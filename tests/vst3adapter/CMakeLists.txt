# SPDX-License-Identifier: GPL-3.0-or-later
# The VST3 plugin of the VST3 adapter tests (README.md), as a <name>.vst3 bundle in <build>/plugins/<config>/vst3adapter
# with the layout of the shared test plugins (tests/plugins/README.md).
set(name gnubase-vst3-adaptertest)
set(bundle "${CMAKE_BINARY_DIR}/plugins/$<CONFIG>/vst3adapter/${name}.vst3")
set(GNUBASE_VST3_ADAPTERTEST "${bundle}" PARENT_SCOPE)
add_library(${name} MODULE adaptertest.cpp adaptertest.h)
target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(${name} PRIVATE vst3-interfaces)
set_target_properties(${name} PROPERTIES PREFIX "" CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
	POSITION_INDEPENDENT_CODE ON)
if(APPLE)
	set_target_properties(${name} PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION vst3
		LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/plugins/$<CONFIG>/vst3adapter"
		MACOSX_BUNDLE_INFO_PLIST ${PROJECT_SOURCE_DIR}/tests/plugins/common/Info.plist.in
		MACOSX_BUNDLE_GUI_IDENTIFIER org.gnubase.test.vst3adapter
		MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
		MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
else()
	# <name>.vst3/Contents/<architecture>/<name>.vst3 (Windows) or <name>.so (Linux), as VST3 names the folders.
	set(suffix .so)
	if(WIN32 AND CMAKE_CXX_COMPILER_ARCHITECTURE_ID MATCHES "ARM64")
		set(architecture arm64-win)
		set(suffix .vst3)
	elseif(WIN32)
		set(architecture x86_64-win)
		set(suffix .vst3)
	elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
		set(architecture aarch64-linux)
	else()
		set(architecture x86_64-linux)
	endif()
	set_target_properties(${name} PROPERTIES SUFFIX ${suffix}
		LIBRARY_OUTPUT_DIRECTORY "${bundle}/Contents/${architecture}")
endif()
gnubase_target_defaults(${name})
