// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/tempotrack.h"
#include "ui/timeformat.h"

#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Rml
{
	class Element;
	class ElementFormControl;
}

// The signature track in the UI (#156, reference manual pp. 163, 1468-1470): the time signature events on the
// signature track's lane in the event display and in the tempo track editor. The signatures are the engine's
// (signature.*); docs/features/signature-track.md describes both.
namespace gnubase::ui
{
	// "3/4" as numerator and denominator; nothing for text that isn't a signature.
	std::optional<std::pair<int, int>> parseSignature(const std::string& text);

	// The time signature events in an element of its own (<div class="signature-row">) that fills its parent: a
	// .signature-event per event in a .signature-events, with its signature as text, attribute bar (zero-based) and
	// .selected when selected.
	// With the select tool a click on an event selects it (Ctrl adds or removes it) and a drag moves the selected ones
	// to the nearest bar (signature.set); the draw tool adds an event at the nearest bar with the signature there
	// (signature.add) and opens it for editing; the erase tool removes events, but never the first. A double click on
	// an event opens it for editing: the row's <input class="signature-field">, .shown over it, where Enter sets the
	// signature typed (signature.set). The parent places it; the row follows the view it is given. Use it on the UI
	// thread; the binding must outlive it.
	class SignatureRow final : private Rml::EventListener
	{
	public:
		SignatureRow(EngineBinding& binding, Rml::Element& parent);
		~SignatureRow() override;
		SignatureRow(const SignatureRow&) = delete;
		SignatureRow& operator=(const SignatureRow&) = delete;
		SignatureRow(SignatureRow&&) = delete;
		SignatureRow& operator=(SignatureRow&&) = delete;

		Rml::Element* element() const { return m_element.get(); }
		const TimeContext& time() const { return m_time; }
		void setTime(TimeContext time);
		// The time axis: seconds from the project start at the left edge, and seconds per dp.
		void setView(double start, double scale);
		void setTool(std::function<TempoTool()> tool) { m_tool = std::move(tool); }
		// The selected events, by bar. onSelect is told of the user's changes.
		const std::vector<int64_t>& selection() const { return m_selection; }
		void setSelection(std::vector<int64_t> bars);
		void onSelect(std::function<void()> handler) { m_onSelect = std::move(handler); }
		// Removes the selected events but the first.
		void removeSelected();
		// Opens the event at a bar for editing.
		void edit(int64_t bar);

	private:
		void ProcessEvent(Rml::Event& event) override;
		bool alive() const { return static_cast<bool>(m_element); }
		float ratio() const;
		void show();
		void place();
		void press(Rml::Event& event);
		void release(Rml::Event& event);
		void select(std::vector<int64_t> bars);
		void open(int64_t bar, const std::string& text);
		// The bar start nearest to a time in seconds.
		int64_t nearestBar(double seconds) const;

		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_element;
		Rml::Element* m_events = nullptr;           // a .signature-event per event
		Rml::ElementFormControl* m_field = nullptr; // .shown while an event is edited
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
		TimeContext m_time;
		double m_start = 0.0;
		double m_scale = 0.01;
		std::function<TempoTool()> m_tool;
		std::vector<int64_t> m_selection;
		std::function<void()> m_onSelect;
		std::optional<float> m_dragFrom; // where a press on a selected event was, in pixels
		std::optional<int64_t> m_editing;
		std::optional<std::pair<int64_t, double>> m_lastPress; // the bar of an event pressed, and when
	};

	// The time signature events on the signature track's lane in an event display, which follow the project and the
	// display's view. The row takes the display's tools: draw draws, erase erases, the others select. Use it on the UI
	// thread; the display and the binding must outlive it.
	class SignatureLane
	{
	public:
		SignatureLane(EventDisplay& display, EngineBinding& binding);
		SignatureLane(const SignatureLane&) = delete;
		SignatureLane& operator=(const SignatureLane&) = delete;
		SignatureLane(SignatureLane&&) = delete;
		SignatureLane& operator=(SignatureLane&&) = delete;
		~SignatureLane() = default;

		SignatureRow& row() { return m_row; }

	private:
		void requestProject();
		void place();

		EventDisplay& m_display;
		EngineBinding& m_binding;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for the binding's answers after it went
		SignatureRow m_row;
		uint64_t m_track = 0; // the signature track, 0 without one
		bool m_loading = false;
		bool m_stale = false;
		std::vector<Subscription> m_subscriptions;
	};
}
