# SPDX-License-Identifier: GPL-3.0-or-later
find_package(Threads REQUIRED)

add_library(gnubase-engine STATIC
	engine.cpp engine.h
	rt/denormals.cpp rt/denormals.h
	rt/mpmcqueue.h
	rt/realtime.cpp rt/realtime.h
	rt/rtutil.h
	rt/spscqueue.h
	rt/triplebuffer.h
	time/signaturemap.cpp time/signaturemap.h
	time/tempomap.cpp time/tempomap.h
	time/timecode.cpp time/timecode.h)
target_include_directories(gnubase-engine PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(gnubase-engine PRIVATE GNUBASE_VERSION="${PROJECT_VERSION}")
target_link_libraries(gnubase-engine PUBLIC Threads::Threads)
gnubase_target_defaults(gnubase-engine)

# RT checker: replaces the global operator new/delete to report allocations in real-time code.
# An object library, so the replacement is always linked; only test and debug executables use it.
add_library(gnubase-rtcheck OBJECT rt/allocationcheck.cpp)
target_link_libraries(gnubase-rtcheck PUBLIC gnubase-engine)
gnubase_target_defaults(gnubase-rtcheck)
