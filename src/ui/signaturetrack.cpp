// SPDX-License-Identifier: GPL-3.0-or-later
// The time signature events on the signature track's lane and in the tempo track editor (ui/signaturetrack.h).
#include "ui/signaturetrack.h"

#include "ui/widgetutils.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/SystemInterface.h>

#include <algorithm>
#include <cmath>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		constexpr double g_dragStart = 2.0;       // dp a press moves before it moves events
		constexpr double g_doubleClickTime = 0.5; // seconds, as RmlUi's double clicks

		Rml::Property dp(double value)
		{
			return {static_cast<float>(value), Rml::Unit::DP};
		}

		std::string signatureText(const time::TimeSignature& signature)
		{
			return std::to_string(signature.numerator) + "/" + std::to_string(signature.denominator);
		}

		// The bar of the .signature-event an element is in or is.
		std::optional<int64_t> eventBar(const Rml::Element* element)
		{
			for (; element != nullptr; element = element->GetParentNode())
				if (element->IsClassSet("signature-event"))
					return element->GetAttribute<int64_t>("bar", 0);
			return std::nullopt;
		}
	}

	std::optional<std::pair<int, int>> parseSignature(const std::string& text)
	{
		const size_t slash = text.find('/');
		if (slash == std::string::npos)
			return std::nullopt;
		const std::optional<float> numerator = widgets::parse(text.substr(0, slash));
		const std::optional<float> denominator = widgets::parse(text.substr(slash + 1));
		if (!numerator || !denominator || *numerator != std::floor(*numerator) ||
			*denominator != std::floor(*denominator))
			return std::nullopt;
		return std::make_pair(static_cast<int>(*numerator), static_cast<int>(*denominator));
	}

	// The row.

	SignatureRow::SignatureRow(EngineBinding& binding, Rml::Element& parent) : m_binding(binding)
	{
		Rml::ElementPtr element = parent.GetOwnerDocument()->CreateElement("div");
		element->SetClassNames("signature-row");
		m_element = parent.AppendChild(std::move(element))->GetObserverPtr();
		// The events are rebuilt when they change; the field stays, so that it never goes while it handles an event.
		m_element->SetInnerRML(R"(<div class="signature-events"/><input type="text" class="signature-field"/>)");
		m_events = m_element->GetChild(0);
		m_field = dynamic_cast<Rml::ElementFormControl*>(m_element->GetChild(1));
		for (const Rml::EventId id : {Rml::EventId::Mousedown, Rml::EventId::Change, Rml::EventId::Blur})
			m_element->AddEventListener(id, this);
		show();
	}

	SignatureRow::~SignatureRow()
	{
		if (!alive())
			return;
		for (const Rml::EventId id : {Rml::EventId::Mousedown, Rml::EventId::Change, Rml::EventId::Blur})
			m_element->RemoveEventListener(id, this);
		if (Rml::ElementDocument* document = m_element->GetOwnerDocument())
			document->RemoveEventListener(Rml::EventId::Mouseup, this);
	}

	void SignatureRow::setTime(TimeContext time)
	{
		m_time = std::move(time);
		// Events that went are no longer selected or edited.
		const std::vector<time::SignatureChange> changes = m_time.signatures.changes();
		const auto gone = [&changes](int64_t bar)
		{
			return std::none_of(changes.begin(), changes.end(),
				[bar](const time::SignatureChange& change) { return change.bar == bar; });
		};
		m_selection.erase(std::remove_if(m_selection.begin(), m_selection.end(), gone), m_selection.end());
		if (m_editing && gone(*m_editing))
			m_editing.reset();
		show();
	}

	void SignatureRow::setView(double start, double scale)
	{
		m_start = start;
		m_scale = scale > 0.0 ? scale : 0.01;
		place();
	}

	void SignatureRow::setSelection(std::vector<int64_t> bars)
	{
		m_selection = std::move(bars);
		show();
	}

	void SignatureRow::select(std::vector<int64_t> bars)
	{
		if (bars == m_selection)
			return;
		setSelection(std::move(bars));
		if (m_onSelect)
			m_onSelect();
	}

	void SignatureRow::removeSelected()
	{
		Json bars = Json::array();
		for (const int64_t bar : m_selection)
			if (bar != 0)
				bars.push_back(bar);
		if (!bars.empty())
			m_binding.send("signature.remove", {{"bars", bars}});
	}

	void SignatureRow::edit(int64_t bar)
	{
		const std::vector<time::SignatureChange> changes = m_time.signatures.changes();
		const auto found = std::find_if(
			changes.begin(), changes.end(), [bar](const time::SignatureChange& change) { return change.bar == bar; });
		if (found != changes.end())
			open(bar, signatureText(found->signature));
	}

	void SignatureRow::open(int64_t bar, const std::string& text)
	{
		if (!alive() || m_field == nullptr)
			return;
		m_editing = bar;
		m_field->SetValue(text);
		show();
		m_field->Focus();
	}

	float SignatureRow::ratio() const
	{
		const Rml::Context* context = alive() ? m_element->GetContext() : nullptr;
		return context == nullptr ? 1.0F : context->GetDensityIndependentPixelRatio();
	}

	int64_t SignatureRow::nearestBar(double seconds) const
	{
		const time::Ticks ticks = m_time.tempo.toTicks(std::max(0.0, seconds));
		const int64_t bar = m_time.signatures.toBarsBeats(ticks).bar;
		const time::Ticks start = m_time.signatures.barStart(bar);
		const time::Ticks end = m_time.signatures.barStart(bar + 1);
		return (ticks - start) * 2 > end - start ? bar + 1 : bar;
	}

	void SignatureRow::show()
	{
		if (!alive())
			return;
		// The same events keep their elements, so that a click that selects one still ends on it.
		std::string rml;
		for (const time::SignatureChange& change : m_time.signatures.changes())
			rml += R"(<div class="signature-event" bar=")" + std::to_string(change.bar) + R"(">)" +
				signatureText(change.signature) + "</div>";
		if (m_events->GetAttribute<Rml::String>("events", "") != rml)
		{
			m_events->SetAttribute("events", rml);
			m_events->SetInnerRML(rml);
		}
		m_field->SetClass("shown", m_editing.has_value());
		for (int i = 0; i < m_events->GetNumChildren(); ++i)
		{
			Rml::Element* event = m_events->GetChild(i);
			const auto bar = event->GetAttribute<int64_t>("bar", -1);
			event->SetClass("selected", std::find(m_selection.begin(), m_selection.end(), bar) != m_selection.end());
		}
		place();
	}

	void SignatureRow::place()
	{
		if (!alive())
			return;
		const auto left = [this](int64_t bar)
		{ return dp((m_time.tempo.toSeconds(m_time.signatures.barStart(bar)) - m_start) / m_scale); };
		for (int i = 0; i < m_events->GetNumChildren(); ++i)
		{
			Rml::Element* event = m_events->GetChild(i);
			event->SetProperty(Rml::PropertyId::Left, left(event->GetAttribute<int64_t>("bar", 0)));
		}
		if (m_editing)
			m_field->SetProperty(Rml::PropertyId::Left, left(*m_editing));
		// Answers arrive outside input handling: ask the window to update and draw them.
		if (Rml::Context* context = m_element->GetContext())
			context->RequestNextUpdate(0);
	}

	void SignatureRow::ProcessEvent(Rml::Event& event)
	{
		if (!alive())
			return;
		const Rml::Element* target = event.GetTargetElement();
		const bool inField = target != nullptr && target == m_field;
		if (event == Rml::EventId::Mouseup)
		{
			release(event);
		}
		else if (event == Rml::EventId::Mousedown && event.GetParameter<int>("button", 0) == 0)
		{
			if (!inField)
				press(event);
			event.StopPropagation(); // the event display around it doesn't select or draw
		}
		else if (event == Rml::EventId::Change && inField && event.GetParameter<bool>("linebreak", false))
		{
			const std::optional<std::pair<int, int>> signature = parseSignature(m_field->GetValue());
			const std::optional<int64_t> bar = std::exchange(m_editing, std::nullopt);
			if (signature && bar)
				m_binding.send("signature.set",
					{{"events",
						{{{"bar", *bar}, {"numerator", signature->first}, {"denominator", signature->second}}}}});
			show();
		}
		else if (event == Rml::EventId::Blur && inField && m_editing)
		{
			m_editing.reset();
			show();
		}
	}

	void SignatureRow::press(Rml::Event& event)
	{
		// A press elsewhere in the row closes the field.
		if (m_editing)
		{
			m_editing.reset();
			m_field->Blur();
			show();
		}
		const TempoTool tool = m_tool ? m_tool() : TempoTool::Select;
		const std::optional<int64_t> on = eventBar(event.GetTargetElement());
		if (tool == TempoTool::Erase)
		{
			if (on && *on != 0)
				m_binding.send("signature.remove", {{"bars", {*on}}});
			return;
		}
		// A second press on an event soon after the first is a double click, which opens it. RmlUi has none for
		// presses whose propagation stops, as they must here.
		const double now = Rml::GetSystemInterface()->GetElapsedTime();
		const std::optional<std::pair<int64_t, double>> last = std::exchange(m_lastPress, std::nullopt);
		if (on && last && last->first == *on && now - last->second < g_doubleClickTime)
		{
			edit(*on);
			return;
		}
		if (on)
			m_lastPress = std::make_pair(*on, now);
		if (on)
		{
			std::vector<int64_t> selection = m_selection;
			const bool toggle =
				event.GetParameter<int>("ctrl_key", 0) != 0 || event.GetParameter<int>("meta_key", 0) != 0;
			const auto at = std::find(selection.begin(), selection.end(), *on);
			if (toggle && at != selection.end())
				selection.erase(at);
			else if (toggle)
				selection.push_back(*on);
			else if (at == selection.end())
				selection = {*on};
			select(std::move(selection));
			if (!m_selection.empty())
			{
				m_dragFrom = event.GetParameter<float>("mouse_x", 0.0F);
				if (Rml::ElementDocument* document = m_element->GetOwnerDocument())
					document->AddEventListener(Rml::EventId::Mouseup, this);
			}
			return;
		}
		if (tool != TempoTool::Draw)
			return;
		// The draw tool adds an event at the nearest bar, with the signature there, to edit.
		const double x = (event.GetParameter<float>("mouse_x", 0.0F) - m_element->GetAbsoluteLeft()) / ratio();
		const int64_t bar = nearestBar(m_start + (x * m_scale));
		const time::TimeSignature signature = m_time.signatures.signatureAt(m_time.signatures.barStart(bar));
		select({});
		m_binding.send("signature.add",
			{{"bar", bar}, {"numerator", signature.numerator}, {"denominator", signature.denominator}},
			[this, bar, signature, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok())
					return;
				select({bar});
				open(bar, signatureText(signature)); // before the project with the new event comes
			});
	}

	void SignatureRow::release(Rml::Event& event)
	{
		if (Rml::ElementDocument* document = m_element->GetOwnerDocument())
			document->RemoveEventListener(Rml::EventId::Mouseup, this);
		const std::optional<float> from = std::exchange(m_dragFrom, std::nullopt);
		if (!from)
			return;
		const double dx = (event.GetParameter<float>("mouse_x", 0.0F) - *from) / ratio();
		if (std::abs(dx) < g_dragStart)
			return;
		// To the nearest bar; the first event stays at the first bar, and no other goes there.
		Json events = Json::array();
		std::vector<int64_t> moved;
		for (const int64_t bar : m_selection)
		{
			if (bar == 0)
				continue;
			const double seconds = m_time.tempo.toSeconds(m_time.signatures.barStart(bar)) + (dx * m_scale);
			const int64_t to = std::max<int64_t>(1, nearestBar(seconds));
			events.push_back({{"bar", bar}, {"to", to}});
			moved.push_back(to);
		}
		if (events.empty())
			return;
		m_binding.send("signature.set", {{"events", events}},
			[this, moved, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (!alive.expired() && result.ok())
					select(moved);
			});
	}

	// The lane.

	SignatureLane::SignatureLane(EventDisplay& display, EngineBinding& binding)
		: m_display(display), m_binding(binding), m_row(binding, display.eventArea())
	{
		m_row.setTool(
			[this]
			{
				switch (m_display.tool())
				{
				case Tool::Draw:
				case Tool::Line:
					return TempoTool::Draw;
				case Tool::Erase:
					return TempoTool::Erase;
				default:
					return TempoTool::Select;
				}
			});
		display.onViewChanged(
			[this, alive = std::weak_ptr<bool>(m_alive)]
			{
				if (!alive.expired())
					place();
			});
		for (const char* event : {"history.changed", "project.reset"})
			m_subscriptions.push_back(m_binding.on(event, [this](const engine::Event&) { requestProject(); }));
		requestProject();
	}

	// ponytail: reads the whole project after every change, as the tempo lane and the markers do.
	void SignatureLane::requestProject()
	{
		if (m_loading)
		{
			m_stale = true;
			return;
		}
		m_loading = true;
		m_binding.send("project.get", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired())
					return;
				m_loading = false;
				if (result.ok())
				{
					m_track = 0;
					for (const Json& track : result.value.at("tracks"))
						if (track.at("type") == "signature")
							m_track = track.at("id").get<uint64_t>();
					m_row.setTime(projectTime(result.value));
					place();
				}
				if (std::exchange(m_stale, false))
					requestProject();
			});
	}

	void SignatureLane::place()
	{
		Rml::Element* element = m_row.element();
		if (element == nullptr)
			return;
		const TimelineView& view = m_display.view();
		const std::vector<uint64_t>& tracks = m_display.laneTracks();
		const std::vector<double>& tops = m_display.laneTops();
		const auto lane = std::find(tracks.begin(), tracks.end(), m_track);
		const bool shown = m_track != 0 && lane != tracks.end();
		element->SetClass("hidden", !shown);
		if (!shown)
		{
			if (Rml::Context* context = element->GetContext())
				context->RequestNextUpdate(0);
			return;
		}
		const auto index = static_cast<size_t>(lane - tracks.begin());
		element->SetProperty(Rml::PropertyId::Top, dp(tops[index] - view.top));
		element->SetProperty(Rml::PropertyId::Height, dp(tops[index + 1] - tops[index]));
		m_row.setView(view.start, view.scale);
	}
}
