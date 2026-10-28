# SPDX-License-Identifier: GPL-3.0-or-later
# The GNUbase plugin API (docs/plugin-api.md): header-only and without dependencies, so the engine, the plugin host
# process and the format adapters can all use it.
add_library(gnubase-plugin INTERFACE events.h extensions.h plugin.h)
target_include_directories(gnubase-plugin INTERFACE ${PROJECT_SOURCE_DIR}/src)
