# SPDX-License-Identifier: GPL-3.0-or-later
# The GNUbase plugin API (docs/plugin-api.md): header-only and without dependencies, so the engine, the plugin host
# process and the format adapters can all use it.
add_library(gnubase-plugin INTERFACE events.h extensions.h plugin.h)
target_include_directories(gnubase-plugin INTERFACE ${PROJECT_SOURCE_DIR}/src)

# The remote plugin proxy (engine side) and stub (host process side), the messages between them and the engine's
# connection to a host process (docs/plugin-api.md, "Split across the engine and the host process").
add_library(gnubase-plugin-remote STATIC
	remote/hostconnection.cpp remote/hostconnection.h
	remote/protocol.cpp remote/protocol.h
	remote/proxy.cpp remote/proxy.h
	remote/stub.cpp remote/stub.h)
target_link_libraries(gnubase-plugin-remote PUBLIC gnubase-plugin gnubase-ipc)
gnubase_target_defaults(gnubase-plugin-remote)
