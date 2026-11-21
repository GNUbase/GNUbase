// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui/enginebinding.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
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
	// attribute color, the color it gives ("#rrggbb", or empty for the reset). An Alt-click on the left area of a track
	// in the track list (its color strip, or the row left of its name) or on the header of the inspector's Basics
	// section opens it at the mouse as the Choose Track Color pane (p. 113): its color goes to that one track, the
	// inspector's the first selected one. Use it on the UI thread; the display, the track list and the binding must
	// outlive it.
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
		// selected tracks (empty: the default color); the pane's track alone while it's open for one.
		void apply(const std::string& color);

		// Fills the menu: the reset item, then the project's colors with their names.
		void fill();

	private:
		void ProcessEvent(Rml::Event& event) override;

		// The track whose Choose Track Color pane an Alt-click on that element opens, or 0.
		uint64_t trackAt(Rml::Event& event) const;

		EventDisplay& m_display;
		TrackList& m_tracks;
		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_document;
		Rml::ObserverPtr<Rml::Element> m_button;
		Rml::ObserverPtr<Rml::Element> m_menu;
		uint64_t m_track = 0; // the one track the menu is open for, or 0 for the selection
	};
}
