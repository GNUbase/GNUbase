# SPDX-License-Identifier: GPL-3.0-or-later
find_package(Threads REQUIRED)

add_library(gnubase-platform STATIC
	audio/backends.cpp
	audio/device.h
	audio/dummydevice.cpp audio/dummydevice.h
	crashhandler.cpp crashhandler.h
	log.cpp log.h
	rt/denormals.cpp rt/denormals.h
	rt/mpmcqueue.h
	rt/realtime.cpp rt/realtime.h
	rt/rtutil.h
	rt/snapshotpublisher.h
	rt/spscqueue.h
	rt/triplebuffer.h)
target_include_directories(gnubase-platform PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(gnubase-platform PRIVATE $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
target_link_libraries(gnubase-platform PUBLIC Threads::Threads PRIVATE $<$<PLATFORM_ID:Windows>:dbghelp>)
gnubase_target_defaults(gnubase-platform)

# RT checker: replaces the global operator new/delete to report allocations in real-time code.
# An object library, so the replacement is always linked; only test and debug executables use it.
add_library(gnubase-rtcheck OBJECT rt/allocationcheck.cpp)
target_link_libraries(gnubase-rtcheck PUBLIC gnubase-platform)
gnubase_target_defaults(gnubase-rtcheck)
# On Linux it checks the C heap too (malloc, free, posix_memalign and so on, which third-party code calls): the linker
# sends the executable's calls through wrappers in allocationcheck.cpp. Not in sanitizer builds, which replace the C
# heap themselves. On Windows, debug builds hook the C runtime's heap instead; macOS has no such hook.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT GNUBASE_SANITIZERS)
	target_compile_definitions(gnubase-rtcheck PUBLIC GNUBASE_RTCHECK_MALLOC)
	target_link_options(gnubase-rtcheck INTERFACE
		"LINKER:--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=free,--wrap=posix_memalign,--wrap=aligned_alloc")
endif()
