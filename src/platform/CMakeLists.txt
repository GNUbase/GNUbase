# SPDX-License-Identifier: GPL-3.0-or-later
find_package(Threads REQUIRED)

add_library(gnubase-platform STATIC
	audio/backends.cpp
	audio/device.h
	audio/dummydevice.cpp audio/dummydevice.h
	audio/inputfifo.h
	audio/sampleformat.cpp audio/sampleformat.h
	crashhandler.cpp crashhandler.h
	hosttime.h
	log.cpp log.h
	midi/backends.cpp
	midi/dummyport.cpp midi/dummyport.h
	midi/inputqueue.h
	midi/port.h
	rt/denormals.cpp rt/denormals.h
	rt/mpmcqueue.h
	rt/realtime.cpp rt/realtime.h
	rt/rtutil.h
	rt/snapshotpublisher.h
	rt/spscqueue.h
	rt/triplebuffer.h)
if(WIN32)
	target_sources(gnubase-platform PRIVATE audio/wasapi.cpp audio/wasapi.h midi/winmm.cpp midi/winmm.h winutf8.h)
endif()
target_include_directories(gnubase-platform PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(gnubase-platform PRIVATE $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
target_link_libraries(gnubase-platform PUBLIC Threads::Threads PRIVATE $<$<PLATFORM_ID:Windows>:dbghelp;avrt;cfgmgr32;winmm>)
gnubase_target_defaults(gnubase-platform)

# RT checker: replaces the global operator new/delete to report allocations in real-time code.
# An object library, so the replacement is always linked; only test and debug executables use it.
add_library(gnubase-rtcheck OBJECT rt/allocationcheck.cpp)
target_link_libraries(gnubase-rtcheck PUBLIC gnubase-platform)
gnubase_target_defaults(gnubase-rtcheck)
