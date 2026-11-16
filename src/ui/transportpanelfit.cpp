// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/transportpanelfit.h"

#include "ui/docking.h"
#include "ui/shell.h"

#include <RmlUi/Core/Context.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

namespace gnubase::ui
{
	namespace
	{
		constexpr const char* g_name = "transportPanel"; // its window's in ui.layout
	}

	TransportPanelFit::TransportPanelFit(Shell& shell, Docking& docking) : m_shell(shell), m_docking(docking)
	{
		m_shell.afterUpdate([this](Window& window)
			{ return window.id() == m_window && m_follows && fit(window, std::exchange(m_centre, false)); });
		// The shell moves only windows that are out of reach: a panel as wide as its monitor's usable area gets
		// narrower with it, e.g. at another resolution or with the taskbar moved to the side.
		m_shell.onDisplaysChanged(
			[this]
			{
				m_measured = 0;
				if (const Window* window = shown())
					window->context().RequestNextUpdate(0);
			});
	}

	void TransportPanelFit::open(Window& window, Rml::Element& sections, bool centre)
	{
		m_window = window.id();
		m_sections = sections.GetObserverPtr();
		m_unfitted = 0;
		m_follows = false;
		m_opensCentred = centre;
		m_centre = false;
		m_measured = 0;
		if (centre)
			fit(window, true);
	}

	void TransportPanelFit::follow()
	{
		m_follows = true;
		m_centre = m_opensCentred;
		if (const Window* window = shown())
			window->context().RequestNextUpdate(0);
	}

	void TransportPanelFit::reset()
	{
		if (const Window* window = shown())
		{
			m_measured = 0;
			m_docking.setFittedWidth(g_name, m_shell.bounds(window->id()).width);
			window->context().RequestNextUpdate(0);
		}
	}

	const Window* TransportPanelFit::shown() const
	{
		return m_window == 0 ? nullptr : m_shell.find(m_window);
	}

	bool TransportPanelFit::fit(Window& window, bool centre)
	{
		Rml::Element* controls = m_sections.get();
		const Size pixels = window.pixelSize();
		if (controls == nullptr || pixels.width <= 0)
			return false;
		const Rect bounds = m_shell.bounds(window.id());
		const std::optional<int> fitted = m_docking.fittedWidth(g_name);
		// Once it follows, a new panel is centred again only where it opened: where the first fit put it, or, with a
		// window system that moves windows later, where it was then or where that fit asked for.
		const bool opening = centre && !m_follows;
		centre = centre &&
			(opening || bounds.position() == m_measuredBounds.position() || bounds.position() == m_asked.position());
		if (!opening)
		{
			if (!fitted || (bounds.width != *fitted && bounds.width != m_unfitted))
				return false; // sized by the user, or by a layout from before #1527
			if (bounds.width == *fitted)
				m_unfitted = 0;
		}
		else
		{
			window.context().Update(); // a new document
		}
		float right = 0;
		Rml::ElementList sections;
		controls->QuerySelectorAll(sections, ".transport-section");
		for (Rml::Element* section : sections)
			if (section->IsVisible())
				right = std::max(right,
					section->GetAbsoluteLeft() + section->GetOffsetWidth() +
						section->GetBox().GetEdge(Rml::BoxArea::Margin, Rml::BoxEdge::Right));
		// Unchanged since the last look, e.g. in every frame while the transport runs: no monitors asked for.
		if (right <= 0 || (!centre && right == m_measured && bounds == m_measuredBounds))
			return false;
		m_measured = right;
		m_measuredBounds = bounds;
		const std::vector<Display> displays = m_shell.displays();
		if (displays.empty())
			return false;
		const Rect fit = fitWidth(bounds,
			static_cast<int>(std::ceil(right * static_cast<float>(bounds.width) / static_cast<float>(pixels.width))),
			displays, centre);
		if (!centre && fit.width == fitted)
			return false;
		const int width = fit.width;
		m_shell.setBounds(window.id(), fit);
		m_asked = fit;
		// A window system that sizes windows later has the old width meanwhile; one that gave the window a width of
		// its own, such as a minimum, keeps that as the fit's.
		const int now = m_shell.bounds(window.id()).width;
		const bool applied = now != bounds.width || now == width;
		m_unfitted = applied ? 0 : bounds.width;
		m_docking.setFittedWidth(g_name, applied ? now : width);
		m_docking.saveWindows(); // so that a panel closed at once comes back as wide, in one save with the width
		m_measuredBounds = m_shell.bounds(window.id());
		return true;
	}
}
