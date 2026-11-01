# SPDX-License-Identifier: GPL-3.0-or-later
find_package(Threads REQUIRED)

add_library(gnubase-platform STATIC
	audio/backends.cpp
	audio/device.h
	audio/dummydevice.cpp audio/dummydevice.h
	audio/inputfifo.h
	audio/sampleformat.cpp audio/sampleformat.h
	crashhandler.cpp crashhandler.h
	files.cpp files.h
	hosttime.h
	log.cpp log.h
	midi/backends.cpp
	midi/dummyport.cpp midi/dummyport.h
	midi/inputqueue.h
	midi/port.h
	process.cpp process.h
	rt/denormals.cpp rt/denormals.h
	rt/mpmcqueue.h
	rt/realtime.cpp rt/realtime.h
	rt/rtutil.h
	rt/seqlockpair.h
	rt/snapshotpublisher.h
	rt/spscqueue.h
	rt/triplebuffer.h
	rt/workerpool.cpp rt/workerpool.h
	sharedmemory.cpp sharedmemory.h
	thread.cpp thread.h)
if(WIN32)
	target_sources(gnubase-platform PRIVATE
		audio/asiohost.cpp audio/asiohost.h
		audio/wasapi.cpp audio/wasapi.h
		midi/winmm.cpp midi/winmm.h
		winutf8.h)
	target_include_directories(gnubase-platform SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/ext/asio/common)
elseif(APPLE)
	target_sources(gnubase-platform PRIVATE
		audio/coreaudio.cpp audio/coreaudio.h)
	target_link_libraries(gnubase-platform PRIVATE "-framework CoreAudio" "-framework CoreFoundation")
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
	target_sources(gnubase-platform PRIVATE
		audio/pipewire.cpp audio/pipewire.h)
	target_link_libraries(gnubase-platform PRIVATE PkgConfig::PIPEWIRE)
	# PipeWire's interface macros (pw_core_add_listener and the like) expand to GNU statement expressions, which
	# clang's -Wpedantic reports at the call site.
	set_source_files_properties(audio/pipewire.cpp PROPERTIES COMPILE_OPTIONS
		$<$<CXX_COMPILER_ID:Clang>:-Wno-gnu-statement-expression-from-macro-expansion>)
	# libjack's headers only: the JACK backend loads libjack when it is first used, so GNUbase runs without it.
	pkg_check_modules(JACK REQUIRED jack)
	target_sources(gnubase-platform PRIVATE
		audio/jack.cpp audio/jack.h
		jacklibrary.cpp jacklibrary.h
		midi/jackmidi.cpp midi/jackmidi.h)
	target_include_directories(gnubase-platform SYSTEM PRIVATE ${JACK_INCLUDE_DIRS})
	target_link_libraries(gnubase-platform PRIVATE ${CMAKE_DL_LIBS})
	pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
	target_sources(gnubase-platform PRIVATE
		midi/alsaseq.cpp midi/alsaseq.h)
	target_link_libraries(gnubase-platform PRIVATE PkgConfig::ALSA)
	# Worker threads ask rtkit for real-time priority over the system bus; GLOBAL for the tests' fake rtkit.
	pkg_check_modules(DBUS REQUIRED IMPORTED_TARGET GLOBAL dbus-1)
	target_link_libraries(gnubase-platform PRIVATE PkgConfig::DBUS)
endif()
if(APPLE)
	target_sources(gnubase-platform PRIVATE
		midi/coremidi.cpp midi/coremidi.h)
	target_link_libraries(gnubase-platform PUBLIC "-framework CoreFoundation" "-framework CoreMIDI")
endif()
target_include_directories(gnubase-platform PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(gnubase-platform PRIVATE $<$<CXX_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS>)
# shm_open is in librt on glibc before 2.34.
target_link_libraries(gnubase-platform PUBLIC Threads::Threads PRIVATE
	"$<$<PLATFORM_ID:Windows>:avrt;cfgmgr32;dbghelp;ole32;shell32;winmm>" $<$<PLATFORM_ID:Linux>:rt>)
gnubase_target_defaults(gnubase-platform)

# RT checker: replaces the global operator new/delete to report allocations in real-time code.
# An object library, so the replacement is always linked; only test and debug executables use it.
add_library(gnubase-rtcheck OBJECT rt/allocationcheck.cpp)
target_link_libraries(gnubase-rtcheck PUBLIC gnubase-platform)
gnubase_target_defaults(gnubase-rtcheck)
# On Linux it checks the C heap too (malloc, free, posix_memalign and so on, which third-party code calls): the
# executable defines those functions in allocationcheck.cpp and hands them on to glibc's allocator. That file is kept
# out of link-time optimization and builtin handling, so the definitions stay plain functions the dynamic linker binds
# every library's calls to. Not in sanitizer builds, which replace the C heap themselves. On Windows, debug builds hook
# the C runtime's heap instead; macOS has no such hook.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT GNUBASE_SANITIZERS)
	target_compile_definitions(gnubase-rtcheck PUBLIC GNUBASE_RTCHECK_MALLOC)
	target_compile_options(gnubase-rtcheck PRIVATE -fno-builtin)
	set_target_properties(gnubase-rtcheck PROPERTIES INTERPROCEDURAL_OPTIMIZATION OFF
		INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)
endif()
