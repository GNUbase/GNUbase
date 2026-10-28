# SPDX-License-Identifier: GPL-3.0-or-later
add_library(gnubase-mcp STATIC
	server.cpp server.h
	transport.cpp transport.h)
target_link_libraries(gnubase-mcp PUBLIC gnubase-engine PRIVATE cpp-httplib)
gnubase_target_defaults(gnubase-mcp)
