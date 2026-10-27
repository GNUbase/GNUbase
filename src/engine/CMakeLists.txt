# SPDX-License-Identifier: GPL-3.0-or-later
add_library(gnubase-engine STATIC
	document/document.cpp document/document.h
	document/projectmodel.cpp document/projectmodel.h
	document/rendersnapshot.cpp document/rendersnapshot.h
	engine.cpp engine.h
	schema.cpp schema.h
	time/signaturemap.cpp time/signaturemap.h
	time/tempomap.cpp time/tempomap.h
	time/timecode.cpp time/timecode.h)
target_include_directories(gnubase-engine PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_definitions(gnubase-engine PRIVATE GNUBASE_VERSION="${PROJECT_VERSION}")
target_link_libraries(gnubase-engine PUBLIC gnubase-platform nlohmann_json::nlohmann_json)
gnubase_target_defaults(gnubase-engine)
