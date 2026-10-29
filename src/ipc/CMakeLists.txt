# SPDX-License-Identifier: GPL-3.0-or-later
# Shared-memory IPC between the engine and plugin host processes: layout, rings, wake-up, connection.
add_library(gnubase-ipc STATIC
	connection.cpp connection.h
	doorbell.cpp doorbell.h
	protocol.cpp protocol.h
	rings.cpp rings.h)
target_link_libraries(gnubase-ipc PUBLIC gnubase-platform)
gnubase_target_defaults(gnubase-ipc)
