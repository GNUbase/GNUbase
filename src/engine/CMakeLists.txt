# SPDX-License-Identifier: GPL-3.0-or-later
# The single-file decoders in ext/, compiled once as third-party code: without GNUbase's warnings or clang-tidy.
add_library(gnubase-decoders STATIC audiofile/decoders.c)
target_include_directories(gnubase-decoders SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/ext/dr_libs ${PROJECT_SOURCE_DIR}/ext/stb)

find_package(Threads REQUIRED)

# r8brain-free-src for sample-rate conversion, with its built-in FFT, and the one file that includes it: third-party
# code as well, so r8brain's headers stay out of GNUbase's warnings and clang-tidy checks.
add_library(gnubase-r8brain STATIC ${PROJECT_SOURCE_DIR}/ext/r8brain/r8bbase.cpp audiofile/r8brain.cpp audiofile/r8brain.h)
target_include_directories(gnubase-r8brain SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/ext/r8brain)
target_include_directories(gnubase-r8brain PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(gnubase-r8brain PUBLIC Threads::Threads)

# Rubber Band Library for time-stretching and pitch-shifting (docs/time-stretch.md): its single-file build, configured
# the same on every platform (built-in FFT and resampler, no threads, no timing), so every platform runs the same code.
# Third-party code, without GNUbase's warnings and clang-tidy checks; only dsp/timestretch.cpp includes it.
# GNUbase's changes, which make real-time mode allocate nothing when the ratio or pitch changes, are one patch:
# ext/patches/rubberband.patch. Git applies it to a copy of the sources in the build folder whenever the patch or a
# file it changes is new. ponytail: carry the changes in a GNUbase fork like SDL and RmlUi (or upstream), then build
# ext/rubberband directly and drop this.
find_package(Git REQUIRED)
set(rubberband ${PROJECT_SOURCE_DIR}/ext/rubberband)
set(rubberbandPatch ${PROJECT_SOURCE_DIR}/ext/patches/rubberband.patch)
set(rubberbandPatched ${CMAKE_CURRENT_BINARY_DIR}/rubberband)
file(READ ${rubberbandPatch} patch)
string(REPLACE "\r\n" "\n" patch "${patch}") # Git may check text files out with CRLF line endings
string(REGEX MATCHALL "\n\\+\\+\\+ b/[^\n]+" patchedFiles "${patch}")
list(TRANSFORM patchedFiles REPLACE "^\n\\+\\+\\+ b/" "")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${rubberbandPatch})
set(patchStamp "${patch}")
foreach(file IN LISTS patchedFiles)
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${rubberband}/${file})
	file(SHA256 ${rubberband}/${file} hash)
	string(APPEND patchStamp "${file} ${hash}\n")
endforeach()
string(SHA256 patchStamp "${patchStamp}")
set(appliedStamp "")
if(EXISTS ${rubberbandPatched}/patch.stamp)
	file(READ ${rubberbandPatched}/patch.stamp appliedStamp)
endif()
if(NOT appliedStamp STREQUAL patchStamp)
	file(REMOVE_RECURSE ${rubberbandPatched})
	file(COPY ${rubberband}/rubberband ${rubberband}/single ${rubberband}/src DESTINATION ${rubberbandPatched})
	foreach(file IN LISTS patchedFiles) # the patch has LF line endings
		file(READ ${rubberbandPatched}/${file} source)
		string(REPLACE "\r\n" "\n" source "${source}")
		file(WRITE ${rubberbandPatched}/${file} "${source}")
	endforeach()
	file(WRITE ${rubberbandPatched}/rubberband.patch "${patch}")
	# Outside a repository, as the build folder may be inside GNUbase's: Git would skip paths outside the current one.
	execute_process(COMMAND ${CMAKE_COMMAND} -E env GIT_CEILING_DIRECTORIES=${CMAKE_CURRENT_BINARY_DIR}
		${GIT_EXECUTABLE} apply --verbose --whitespace=nowarn rubberband.patch
		WORKING_DIRECTORY ${rubberbandPatched} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
	if(NOT result EQUAL 0 OR error MATCHES "Skipped" OR NOT error MATCHES "Applied patch")
		message(FATAL_ERROR "ext/patches/rubberband.patch no longer applies to ${rubberband}: ${output}${error}")
	endif()
	file(WRITE ${rubberbandPatched}/patch.stamp "${patchStamp}")
endif()
add_library(gnubase-rubberband STATIC ${rubberbandPatched}/single/RubberBandSingle.cpp)
target_include_directories(gnubase-rubberband SYSTEM PUBLIC ${rubberbandPatched})
target_include_directories(gnubase-rubberband SYSTEM PRIVATE ${rubberbandPatched}/src/common)
target_compile_definitions(gnubase-rubberband PRIVATE ALREADY_CONFIGURED USE_BUILTIN_FFT USE_BQRESAMPLER NO_TIMING
	NO_THREADING NO_THREAD_CHECKS $<$<CXX_COMPILER_ID:MSVC>:__MSVC__ NOMINMAX _USE_MATH_DEFINES>
	$<$<PLATFORM_ID:Darwin>:LACK_SINCOS>) # macOS's C library has no sincos; Rubber Band's own build sets this too
if(NOT MSVC)
	target_compile_options(gnubase-rubberband PRIVATE -ffp-contract=off) # as GNUbase's own code, see gnubase_target_defaults
endif()
# Rubber Band asks std::atomic<double> whether it is lock-free, which needs libatomic with clang on Linux; the same
# check as in Rubber Band's own build.
include(CheckCXXSourceCompiles)
set(atomicCheck "#include <atomic>
int main() { std::atomic<int> i; std::atomic<double> d; return int(i.is_lock_free() && d.is_lock_free()); }")
check_cxx_source_compiles("${atomicCheck}" GNUBASE_ATOMIC_WITHOUT_LIBATOMIC)
if(NOT GNUBASE_ATOMIC_WITHOUT_LIBATOMIC)
	set(CMAKE_REQUIRED_LIBRARIES atomic)
	check_cxx_source_compiles("${atomicCheck}" GNUBASE_ATOMIC_WITH_LIBATOMIC)
	unset(CMAKE_REQUIRED_LIBRARIES)
	if(NOT GNUBASE_ATOMIC_WITH_LIBATOMIC)
		message(FATAL_ERROR "std::atomic<double> needs libatomic, which was not found")
	endif()
	target_link_libraries(gnubase-rubberband PUBLIC atomic)
endif()

add_library(gnubase-engine STATIC
	analysis/analysis.cpp analysis/analysis.h
	audio/analysiscommands.cpp
	audio/audioengine.cpp audio/audioengine.h
	audio/automation.cpp audio/automation.h
	audio/playback.cpp audio/playback.h
	audio/sequencer.cpp audio/sequencer.h
	audio/transport.cpp audio/transport.h
	audiofile/audiofile.h
	audiofile/diskstream.cpp audiofile/diskstream.h
	audiofile/file.cpp audiofile/file.h
	audiofile/reader.cpp
	audiofile/resampler.cpp audiofile/resampler.h
	audiofile/writer.cpp
	dawproject/dawproject.cpp dawproject/dawproject.h
	document/document.cpp document/document.h
	document/history.cpp document/history.h
	document/midiedits.cpp
	document/projectmodel.cpp document/projectmodel.h
	document/rendersnapshot.cpp document/rendersnapshot.h
	document/serialize.cpp document/serialize.h
	dsp/timestretch.cpp dsp/timestretch.h
	engine.cpp engine.h
	graph/events.h
	graph/graph.cpp graph/graph.h
	graph/processor.h
	jobs/jobs.cpp jobs/jobs.h
	media/backup.cpp media/backup.h
	midi/midivalues.cpp midi/midivalues.h
	peaks/peakcache.cpp peaks/peakcache.h
	peaks/peaks.cpp peaks/peaks.h
	processors/builtin.cpp processors/builtin.h
	processors/gainpan.cpp processors/gainpan.h
	processors/generators.cpp processors/generators.h
	processors/testsynth.cpp processors/testsynth.h
	schema.cpp schema.h
	schematext.cpp schematext.h
	settings/store.cpp settings/store.h
	settings/usersettings.cpp settings/usersettings.h
	time/signaturemap.cpp time/signaturemap.h
	time/tempomap.cpp time/tempomap.h
	time/timecode.cpp time/timecode.h)
target_include_directories(gnubase-engine PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(gnubase-engine PRIVATE GNUBASE_VERSION="${PROJECT_VERSION}")
target_link_libraries(gnubase-engine PUBLIC gnubase-platform nlohmann_json::nlohmann_json PRIVATE gnubase-decoders gnubase-r8brain gnubase-rubberband FLAC::FLAC pugixml::pugixml miniz)
gnubase_target_defaults(gnubase-engine)
