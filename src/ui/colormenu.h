// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui/enginebinding.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <string>

namespace Rml
{
	class Element;
}

// Colorizing tracks and events in the project window (#94, docs/features/colors.md).
namespace gnubase::ui
{
	class EventDisplay;
	class TrackList;

	// The toolbar's Select Color for Selected Tracks or Events (reference manual pp. 112-115): the button
	// #select-color opens #color-menu, which this fills with the project's color set when it opens. With events
	// selected in the display, a color goes to them and Set Event Color to Track makes them follow their track again;
	// otherwise a color goes to the selected tracks and Set Track Color to Default resets them. Each item has the
	// attribute color, the color it gives ("#rrggbb", or empty for the reset). Use it on the UI thread; the display,
	// the track list and the binding must outlive it.
	class ColorMenu final : private Rml::EventListener
	{
	public:
		ColorMenu(EventDisplay& display, TrackList& tracks, EngineBinding& binding, Rml::Element& document);
		~ColorMenu() override;
		ColorMenu(const ColorMenu&) = delete;
		ColorMenu& operator=(const ColorMenu&) = delete;
		ColorMenu(ColorMenu&&) = delete;
		ColorMenu& operator=(ColorMenu&&) = delete;

		// Gives the selected events the color ("#rrggbb"; empty: their track's), or without selected events, the
		// selected tracks (empty: the default color).
		void apply(const std::string& color);

		// Fills the menu: the reset item, then the project's colors with their names.
		void fill();

	private:
		void ProcessEvent(Rml::Event& event) override;

		EventDisplay& m_display;
		TrackList& m_tracks;
		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_button;
		Rml::ObserverPtr<Rml::Element> m_menu;
	};
}
