# SPDX-License-Identifier: GPL-3.0-or-later
# The single-file decoders in ext/, compiled once as third-party code: without GNUbase's warnings or clang-tidy.
add_library(gnubase-decoders STATIC audiofile/decoders.c)
target_include_directories(gnubase-decoders SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/ext/dr_libs ${PROJECT_SOURCE_DIR}/ext/stb)

add_library(gnubase-engine STATIC
	audio/audioengine.cpp audio/audioengine.h
	audiofile/audiofile.h
	audiofile/file.cpp audiofile/file.h
	audiofile/reader.cpp
	audiofile/writer.cpp
	document/document.cpp document/document.h
	document/history.cpp document/history.h
	document/projectmodel.cpp document/projectmodel.h
	document/rendersnapshot.cpp document/rendersnapshot.h
	engine.cpp engine.h
	graph/events.h
	graph/graph.cpp graph/graph.h
	graph/processor.h
	processors/builtin.cpp processors/builtin.h
	processors/gainpan.cpp processors/gainpan.h
	processors/generators.cpp processors/generators.h
	processors/testsynth.cpp processors/testsynth.h
	schema.cpp schema.h
	time/signaturemap.cpp time/signaturemap.h
	time/tempomap.cpp time/tempomap.h
	time/timecode.cpp time/timecode.h)
target_include_directories(gnubase-engine PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(gnubase-engine PRIVATE GNUBASE_VERSION="${PROJECT_VERSION}")
target_link_libraries(gnubase-engine PUBLIC gnubase-platform nlohmann_json::nlohmann_json PRIVATE gnubase-decoders FLAC::FLAC)
gnubase_target_defaults(gnubase-engine)
