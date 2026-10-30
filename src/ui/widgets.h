// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace gnubase::ui
{
	class RmlUi;

	// The event a <dialog> sends when it closes, with the parameter "result": "cancel" for Escape, or the result
	// attribute of what closed it.
	inline constexpr const char* g_dialogCloseEvent = "close";

	// GNUbase's widget library (#54): the elements documents get for its widget tags, and the behaviour of RmlUi's own
	// ones that GNUbase adds. docs/features/widgets.md describes each widget. Widgets keep their state in attributes
	// (value, checked, disabled) and in the state classes that the themes style, and report what the user changed
	// with a "change" event whose "value" parameter is the new value, so data-value and data-checked bind them to data
	// models. Call it before loading documents that use them.
	void registerWidgets(RmlUi& rmlui);

	// The groups of widgets that registerWidgets registers, each in a source file of its own.
	void registerValueWidgets(RmlUi& rmlui); // knobs, faders, sliders, value fields, meters
	void registerPopupWidgets(RmlUi& rmlui); // menus, context menus, tooltips, dialogs
}
