// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "mcp/server.h"
#include "scripting/runtime.h"
#include "ui/shell.h"

#include <cstdint>
#include <vector>

namespace gnubase::app
{
	// Where the UI tools are called from: another thread, such as the MCP server's, whose tools post their work to the
	// UI thread and wait for it; or the UI thread, such as the UI's Lua, where they run at once and ui_wait steps the
	// shell between its checks.
	enum class Caller : uint8_t
	{
		OtherThread,
		UiThread,
	};

	// The UI tools (#58) for that caller, as MCP tools: ui_windows, ui_dom, ui_screenshot, ui_wait and the input tools
	// ui_move, ui_click, ui_drag, ui_wheel, ui_key and ui_text, through ui::Automation. The shell must outlive them.
	std::vector<mcp::Server::Tool> uiTools(ui::Shell& shell, Caller caller);

	// Adds the MCP tools for the UI (#58): ui_windows, ui_dom, ui_screenshot, ui_wait and the input tools ui_move,
	// ui_click, ui_drag, ui_wheel, ui_key and ui_text, through ui::Automation. Their work runs on the UI thread, so it
	// must step the shell while the server handles requests. The shell must outlive the server.
	void addUiTools(mcp::Server& server, ui::Shell& shell);

	// Adds the same tools as Lua functions, for a runtime on the UI thread such as the UI's (app/appui.h): ui_click
	// becomes gnubase.ui.click, which takes the tool's arguments as a table and returns its result, a screenshot as
	// the PNG file's bytes. They run at once, and gnubase.ui.wait steps the shell while it waits, so it fails inside
	// the UI's own event handlers. Call them only while the shell exists.
	void addUiFunctions(scripting::Runtime& lua, ui::Shell& shell);
}
