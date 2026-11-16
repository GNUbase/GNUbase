// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui/displays.h"
#include "ui/windowsystem.h"

#include <RmlUi/Core/Element.h>

namespace gnubase::ui
{
	class Docking;
	class Shell;
	class Window;

	// Gives the transport panel, the docking window transportPanel, the width of its shown sections (the elements of
	// class transport-section), as far as its monitor has room (#1424): a new one in the middle of the primary
	// monitor, an open one where it is, whenever its sections' width changes and its window still has the width of
	// their last fit, which ui.layout keeps as fittedWidth: the user didn't size it since (#1527). It fits the panel
	// after each update of its window's context, and again when the monitors change.
	class TransportPanelFit
	{
	public:
		TransportPanelFit(Shell& shell, Docking& docking);

		// The panel opened in window, with its sections inside sections. One that opened without bounds (centre) is
		// fitted to them at once, in the middle of the primary monitor, so that a panel closed before its controls
		// show the project doesn't keep the unfitted width in ui.layout.
		void open(Window& window, Rml::Element& sections, bool centre);
		// The controls show the project: from then on the panel follows its sections, so that one with fitted bounds
		// isn't fitted to the controls' defaults first, and one that opened without bounds is centred once more for
		// what they show (#1612).
		void follow();
		// Reset All in its setup pane: a panel the user sized, or one with bounds from before #1527, follows its
		// sections again from the width it has (#1612).
		void reset();

	private:
		const Window* shown() const;
		// Whether it changed the window's bounds.
		bool fit(Window& window, bool centre);

		Shell& m_shell;
		Docking& m_docking;
		WindowId m_window = 0;
		Rml::ObserverPtr<Rml::Element> m_sections;
		// The width the window had before the last fit while the window system hasn't given it the fitted one yet.
		int m_unfitted = 0;
		// Whether the panel follows its sections, whether it opened without bounds, and whether its next fit centres
		// it.
		bool m_follows = false;
		bool m_opensCentred = false;
		bool m_centre = false;
		// Where the shown sections ended and the window's bounds when the panel last looked at them: unchanged, it
		// asks for no monitors. The monitors changing forget them.
		float m_measured = 0;
		Rect m_measuredBounds;
		// The bounds the last fit asked for, which a window system that moves windows later, such as X11's, gives the
		// window only after the fit looked at its bounds again.
		Rect m_asked;
	};
}
