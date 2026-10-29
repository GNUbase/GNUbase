# SPDX-License-Identifier: GPL-3.0-or-later
# Checks that a test plugin's binary exports exactly its format's entry points (CMakeLists.txt in this folder).
# Usage: cmake -DLISTER=<nm or dumpbin> -DFILE=<binary> -DEXPECTED=<symbol,symbol,...> -P check-exports.cmake
cmake_minimum_required(VERSION 3.25) # script mode starts with old policies; IN_LIST needs CMP0057
string(REPLACE "," ";" expected "${EXPECTED}")
list(SORT expected)

get_filename_component(listerName "${LISTER}" NAME_WE)
string(TOLOWER "${listerName}" listerName)
if(listerName STREQUAL "dumpbin")
	execute_process(COMMAND "${LISTER}" /nologo /exports "${FILE}" OUTPUT_VARIABLE output RESULT_VARIABLE result)
	# "    ordinal hint RVA      name", then one line per export: "1    0 0001B0B4 ExitDll = ..."
	set(pattern "^ +[0-9]+ +[0-9A-Fa-f]+ +[0-9A-Fa-f]+ +([^ ]+)")
elseif(CMAKE_HOST_APPLE)
	# External, defined symbols of every architecture in the universal binary.
	execute_process(COMMAND "${LISTER}" -g -U "${FILE}" OUTPUT_VARIABLE output RESULT_VARIABLE result)
	set(pattern "^[0-9a-fA-F]+ [A-Za-z] _([^ ]+)$")
else()
	execute_process(COMMAND "${LISTER}" -D --defined-only "${FILE}" OUTPUT_VARIABLE output RESULT_VARIABLE result)
	set(pattern "^[0-9a-fA-F]+ [A-Za-z] ([^ @]+)")
endif()
if(NOT result EQUAL 0)
	message(FATAL_ERROR "${LISTER} failed on ${FILE}: ${result}")
endif()

# Symbols the linker defines in every shared library or bundle, not the plugin's code.
set(linkerSymbols _init _fini _edata _end __bss_start _mh_bundle_header)
set(exports "")
string(REPLACE "\r" "" output "${output}")
string(REPLACE "\n" ";" lines "${output}")
foreach(line IN LISTS lines)
	if(line MATCHES "${pattern}")
		set(symbol "${CMAKE_MATCH_1}")
		if(NOT symbol IN_LIST linkerSymbols)
			list(APPEND exports "${symbol}")
		endif()
	endif()
endforeach()
list(REMOVE_DUPLICATES exports) # once per architecture in a universal binary
list(SORT exports)

if(NOT exports STREQUAL expected)
	message(FATAL_ERROR "${FILE} exports\n  ${exports}\nbut should export only\n  ${expected}")
endif()
