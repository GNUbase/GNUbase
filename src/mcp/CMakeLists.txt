# SPDX-License-Identifier: GPL-3.0-or-later
add_library(gnubase-mcp STATIC
	catalogue.cpp catalogue.h
	server.cpp server.h
	sessions.cpp sessions.h
	transport.cpp transport.h)
target_link_libraries(gnubase-mcp PUBLIC gnubase-engine PRIVATE cpp-httplib)
gnubase_target_defaults(gnubase-mcp)
