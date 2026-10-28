# SPDX-License-Identifier: GPL-3.0-or-later
add_library(gnubase-scripting STATIC
	reference.cpp reference.h
	runtime.cpp runtime.h
	testing.lua)
target_link_libraries(gnubase-scripting PUBLIC gnubase-engine PRIVATE lua)
gnubase_target_defaults(gnubase-scripting)

# The test framework testing.lua is compiled in as a list of bytes, because MSVC limits the length of string literals.
file(READ testing.lua testFrameworkHex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," testFrameworkBytes "${testFrameworkHex}")
string(REPEAT "0x..," 16 sixteenBytes) # one line each, to stay far below compilers' line limits
string(REGEX REPLACE "(${sixteenBytes})" "\\1\n" testFrameworkBytes "${testFrameworkBytes}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/testing.lua.inc" CONTENT "${testFrameworkBytes}\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS testing.lua)
target_include_directories(gnubase-scripting PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
