// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/timeformat.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Rml
{
	class Element;
}

// The project window's info line (#89, reference manual p. 72): docs/features/inspector.md describes it.
namespace gnubase::ui
{
	// Fills an element with the info line of the event display's selected events and parts: the first one's name,
	// start, end and length in the ruler's time format (<textdisplay field="start">), an audio event's volume
	// (<valuefield field="volume">) and mute and lock (toggle buttons), from event.get. The element has the class empty
	// without a selection and multiple with more than one selected. Edits go to every selected event or part as one
	// undo step (history.transaction): relative to each one's value, or with Ctrl (Cmd) held when Enter takes the
	// value, the same value for all. Use it on the UI thread; the display and the binding must outlive it.
	class InfoLine final : private Rml::EventListener
	{
	public:
		InfoLine(EventDisplay& display, EngineBinding& binding, Rml::Element& root);
		~InfoLine() override;
		InfoLine(const InfoLine&) = delete;
		InfoLine& operator=(const InfoLine&) = delete;
		InfoLine(InfoLine&&) = delete;
		InfoLine& operator=(InfoLine&&) = delete;

		// Follows the display's selection and project; call it once per step.
		void update();

	private:
		void ProcessEvent(Rml::Event& event) override;
		void show();
		void edit(const std::string& field, const std::string& text, float number);
		// The selected event's volume in dB, from the display's project.
		double volumeOf(uint64_t event) const;

		EventDisplay& m_display;
		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_root;
		std::vector<uint64_t> m_selection;             // shown, or asked for
		uint64_t m_loads = 0;                          // the display's project shown
		TimeFormat m_format = TimeFormat::BarsBeats;   // the display's ruler's
		engine::Json m_events = engine::Json::array(); // event.get's, the first one first
		bool m_absolute = false;                       // Ctrl held with Enter
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
	};
}
