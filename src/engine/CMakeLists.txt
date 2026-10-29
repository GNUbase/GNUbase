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

add_library(gnubase-engine STATIC
	analysis/analysis.cpp analysis/analysis.h
	audio/analysiscommands.cpp
	audio/audioengine.cpp audio/audioengine.h
	audio/playback.cpp audio/playback.h
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
	engine.cpp engine.h
	graph/events.h
	graph/graph.cpp graph/graph.h
	graph/processor.h
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
target_link_libraries(gnubase-engine PUBLIC gnubase-platform nlohmann_json::nlohmann_json PRIVATE gnubase-decoders gnubase-r8brain FLAC::FLAC pugixml::pugixml miniz)
gnubase_target_defaults(gnubase-engine)
