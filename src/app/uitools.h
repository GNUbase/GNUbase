// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mcp/server.h"
#include "ui/shell.h"

namespace gnubase::app
{
	// Adds the MCP tools for the UI (#58): ui_windows, ui_dom, ui_screenshot, ui_wait and the input tools ui_move,
	// ui_click, ui_drag, ui_wheel, ui_key and ui_text, through ui::Automation. Their work runs on the UI thread, so it
	// must step the shell while the server handles requests. The shell must outlive the server.
	void addUiTools(mcp::Server& server, ui::Shell& shell);
}
