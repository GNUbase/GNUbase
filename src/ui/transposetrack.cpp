// SPDX-License-Identifier: GPL-3.0-or-later
// The transpose events on the transpose track's lane, the chord and scale events on the chord track's and the arranger
// events on the arranger track's (ui/transposetrack.h).
#include "ui/transposetrack.h"

#include "ui/widgetutils.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>
#include <RmlUi/Core/SystemInterface.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		constexpr double g_dragStart = 2.0;       // dp a press moves before it moves the event
		constexpr double g_doubleClickTime = 0.5; // seconds, as RmlUi's double clicks

		Rml::Property dp(double value)
		{
			return {static_cast<float>(value), Rml::Unit::DP};
		}

		std::string valueText(int value)
		{
			return value > 0 ? "+" + std::to_string(value) : std::to_string(value);
		}

		// The id of the event (an element with the class name) an element is in or is.
		std::optional<uint64_t> eventId(const Rml::Element* element, const std::string& name)
		{
			for (; element != nullptr; element = element->GetParentNode())
				if (element->IsClassSet(name))
					return element->GetAttribute<uint64_t>("event", 0);
			return std::nullopt;
		}
	}

	GlobalEventLane::GlobalEventLane(EventDisplay& display, EngineBinding& binding, LaneKind kind)
		: m_display(display), m_binding(binding), m_kind(kind)
	{
		Rml::Element& parent = display.eventArea();
		Rml::ElementPtr element = parent.GetOwnerDocument()->CreateElement("div");
		element->SetClassNames(named("-row"));
		m_element = parent.AppendChild(std::move(element))->GetObserverPtr();
		// The events are rebuilt when they change; the field stays, so that it never goes while it handles an event.
		m_element->SetInnerRML(
			R"(<div class=")" + named("-events") + R"("/><input type="text" class=")" + named("-field") + R"("/>)");
		m_events = m_element->GetChild(0);
		m_field = dynamic_cast<Rml::ElementFormControl*>(m_element->GetChild(1));
		for (const Rml::EventId id : {Rml::EventId::Mousedown, Rml::EventId::Change, Rml::EventId::Blur})
			m_element->AddEventListener(id, this);
		display.onViewChanged(
			[this, alive = std::weak_ptr<bool>(m_alive)]
			{
				if (!alive.expired())
					place();
			});
		for (const std::string& event :
			{std::string("history.changed"), std::string("project.reset"), command(".changed")})
			m_subscriptions.push_back(m_binding.on(event, [this](const engine::Event&) { requestProject(); }));
		requestProject();
	}

	GlobalEventLane::~GlobalEventLane()
	{
		if (!alive())
			return;
		for (const Rml::EventId id : {Rml::EventId::Mousedown, Rml::EventId::Change, Rml::EventId::Blur})
			m_element->RemoveEventListener(id, this);
		if (Rml::ElementDocument* document = m_element->GetOwnerDocument())
			document->RemoveEventListener(Rml::EventId::Mouseup, this);
	}

	std::string GlobalEventLane::named(const char* suffix) const
	{
		if (m_kind == LaneKind::Scale)
			return "scale" + std::string(suffix);
		if (m_kind == LaneKind::Arranger)
			return "arranger" + std::string(suffix);
		return (m_kind == LaneKind::Chord ? "chord" : "transpose") + std::string(suffix);
	}

	std::string GlobalEventLane::command(const char* suffix) const
	{
		if (m_kind == LaneKind::Arranger)
			return "arranger" + std::string(suffix);
		return (m_kind == LaneKind::Transpose ? "transpose" : "chord") + std::string(suffix);
	}

	GlobalEventLane::Event GlobalEventLane::eventFrom(const Json& event) const
	{
		const auto start = event.at("start").get<int64_t>();
		Event result{event.at("id").get<uint64_t>(), start, event.value("endSamples", start), {}, {}, false};
		if (m_kind == LaneKind::Arranger)
		{
			result.text = result.value = event.at("name").get<std::string>();
		}
		else if (m_kind == LaneKind::Chord)
		{
			result.text = result.value = event.at("chord").get<std::string>();
			result.detected = event.value("audioEvent", uint64_t{0}) != 0;
		}
		else if (m_kind == LaneKind::Scale)
		{
			result.text = result.value = event.at("scale").get<std::string>();
		}
		else
		{
			const int value = event.at("value").get<int>();
			result.text = valueText(value);
			result.value = std::to_string(value);
		}
		return result;
	}

	// ponytail: reads the whole project after every change, as the signature lane does.
	void GlobalEventLane::requestProject()
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
					m_time = projectTime(result.value);
					const char* section = "chords";
					if (m_kind == LaneKind::Transpose || m_kind == LaneKind::Arranger)
						section = m_kind == LaneKind::Transpose ? "transpose" : "arranger";
					const Json& transpose = result.value.at(section);
					m_track = transpose.at("track").get<uint64_t>();
					m_showScales = transpose.value("showScales", false);
					m_audition = transpose.value("acousticFeedback", false) &&
						transpose.value("audition", uint64_t{0}) != 0 && !transpose.value("mute", false);
					m_linear = false;
					for (const Json& track : result.value.at("tracks"))
						if (track.at("id") == m_track)
							m_linear = track.at("timeBase") == "linear";
					m_list.clear();
					for (const Json& event : transpose.at(m_kind == LaneKind::Scale ? "scales" : "events"))
						m_list.push_back(eventFrom(event));
					if (m_editing &&
						std::none_of(m_list.begin(), m_list.end(),
							[this](const Event& event) { return event.id == *m_editing; }))
						m_editing.reset();
					show();
				}
				if (std::exchange(m_stale, false))
					requestProject();
			});
	}

	void GlobalEventLane::edit(uint64_t event)
	{
		const auto found =
			std::find_if(m_list.begin(), m_list.end(), [event](const Event& each) { return each.id == event; });
		if (!alive() || m_field == nullptr || found == m_list.end())
			return;
		m_editing = event;
		m_field->SetValue(found->value == "X" ? std::string() : found->value);
		show();
		m_field->Focus();
	}

	int64_t GlobalEventLane::nearestBar(double seconds) const
	{
		const time::Ticks ticks = m_time.tempo.toTicks(std::max(0.0, seconds));
		int64_t bar = m_time.signatures.toBarsBeats(ticks).bar;
		const time::Ticks start = m_time.signatures.barStart(bar);
		const time::Ticks end = m_time.signatures.barStart(bar + 1);
		if ((ticks - start) * 2 > end - start)
			++bar;
		const time::Ticks at = m_time.signatures.barStart(bar);
		return m_linear ? std::llround(m_time.tempo.toSeconds(at) * m_time.sampleRate) : at;
	}

	int64_t GlobalEventLane::position(double seconds) const
	{
		seconds = clampPositionSeconds(seconds, m_time.sampleRate);
		return m_linear ? positionSamples(seconds, m_time.sampleRate) : m_time.tempo.toTicks(seconds);
	}

	void GlobalEventLane::show()
	{
		if (!alive())
			return;
		// The same events keep their elements, so that a click still ends on the one it pressed.
		std::string rml;
		for (const Event& event : m_list)
			rml += R"(<div class=")" + named("-event") + (event.detected ? " detected" : "") + R"(" event=")" +
				std::to_string(event.id) + R"(">)" + Rml::StringUtilities::EncodeRml(event.text) + "</div>";
		if (m_events->GetAttribute<Rml::String>("events", "") != rml)
		{
			m_events->SetAttribute("events", rml);
			m_events->SetInnerRML(rml);
		}
		m_field->SetClass("shown", m_editing.has_value());
		place();
	}

	void GlobalEventLane::place()
	{
		if (!alive())
			return;
		const TimelineView& view = m_display.view();
		const std::vector<uint64_t>& tracks = m_display.laneTracks();
		const std::vector<double>& tops = m_display.laneTops();
		const auto lane = std::find(tracks.begin(), tracks.end(), m_track);
		const bool shown = m_track != 0 && lane != tracks.end() && (m_kind != LaneKind::Scale || m_showScales);
		m_element->SetClass("hidden", !shown);
		if (shown)
		{
			const auto index = static_cast<size_t>(lane - tracks.begin());
			// With Show Scales, the chord events in the upper half of the lane, the scale events in the lower one.
			const bool halves = m_kind != LaneKind::Transpose && m_showScales;
			const double height = (tops[index + 1] - tops[index]) / (halves ? 2.0 : 1.0);
			const double top = tops[index] + (m_kind == LaneKind::Scale ? height : 0.0);
			m_element->SetProperty(Rml::PropertyId::Top, dp(top - view.top));
			m_element->SetProperty(Rml::PropertyId::Height, dp(height));
			const double scale = view.scale > 0.0 ? view.scale : 0.01;
			const auto left = [this, &view, scale](int64_t start) { return dp((seconds(start) - view.start) / scale); };
			for (int i = 0; i < m_events->GetNumChildren(); ++i)
			{
				Rml::Element* element = m_events->GetChild(i);
				const auto id = element->GetAttribute<uint64_t>("event", 0);
				for (const Event& event : m_list)
					if (event.id == id)
					{
						element->SetProperty(Rml::PropertyId::Left, left(event.start));
						if (m_kind == LaneKind::Arranger) // as wide as its section
						{
							const double width = (seconds(event.end) - seconds(event.start)) / scale;
							element->SetProperty(Rml::PropertyId::Width, dp(width));
						}
					}
			}
			for (const Event& event : m_list)
				if (m_editing && event.id == *m_editing)
					m_field->SetProperty(Rml::PropertyId::Left, left(event.start));
		}
		// Answers arrive outside input handling: ask the window to update and draw them.
		if (Rml::Context* context = m_element->GetContext())
			context->RequestNextUpdate(0);
	}

	void GlobalEventLane::ProcessEvent(Rml::Event& event)
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
			const std::optional<float> value = widgets::parse(m_field->GetValue());
			const std::optional<uint64_t> id = std::exchange(m_editing, std::nullopt);
			if ((m_kind == LaneKind::Chord || m_kind == LaneKind::Scale) && id)
				m_binding.send("chord.set",
					{{"events",
						{{{"event", *id}, {m_kind == LaneKind::Scale ? "scale" : "chord", m_field->GetValue()}}}}});
			else if (m_kind == LaneKind::Arranger && id)
				m_binding.send("arranger.set", {{"event", *id}, {"name", m_field->GetValue()}});
			else if (value && id)
				m_binding.send("transpose.set",
					{{"events",
						{{{"event", *id}, {"value", std::clamp(static_cast<int>(std::lround(*value)), -24, 24)}}}}});
			show();
		}
		else if (event == Rml::EventId::Blur && inField && m_editing)
		{
			m_editing.reset();
			show();
		}
	}

	void GlobalEventLane::press(Rml::Event& event)
	{
		// A press elsewhere in the row closes the field.
		if (m_editing)
		{
			m_editing.reset();
			m_field->Blur();
			show();
		}
		const Tool tool = m_display.tool();
		const std::optional<uint64_t> on = eventId(event.GetTargetElement(), named("-event"));
		if (tool == Tool::Erase)
		{
			if (on)
				m_binding.send(command(".remove"), {{"events", {*on}}});
			return;
		}
		if (on)
		{
			// A second press on an event soon after the first is a double click, which opens it. RmlUi has none
			// for presses whose propagation stops, as they must here.
			const double now = Rml::GetSystemInterface()->GetElapsedTime();
			const std::optional<std::pair<uint64_t, double>> last = std::exchange(m_lastPress, std::nullopt);
			if (last && last->first == *on && now - last->second < g_doubleClickTime)
			{
				edit(*on);
				return;
			}
			m_lastPress = std::make_pair(*on, now);
			// Auditioning (reference manual pp. 1329-1330): the event plays while the button is down.
			if (m_audition && (m_kind == LaneKind::Chord || m_kind == LaneKind::Scale))
			{
				m_binding.send("chord.audition", {{"event", *on}});
				m_auditioning = true;
			}
			m_drag = std::make_pair(*on, event.GetParameter<float>("mouse_x", 0.0F));
			if (Rml::ElementDocument* document = m_element->GetOwnerDocument())
				document->AddEventListener(Rml::EventId::Mouseup, this);
			return;
		}
		if (tool != Tool::Draw && tool != Tool::Line)
			return;
		// The draw tool adds an event at the nearest bar, to edit.
		const Rml::Context* context = m_element->GetContext();
		const float ratio = context == nullptr ? 1.0F : context->GetDensityIndependentPixelRatio();
		const auto mouseX = event.GetParameter<float>("mouse_x", 0.0F);
		const double x = (mouseX - m_element->GetAbsoluteLeft()) / ratio;
		const TimelineView& view = m_display.view();
		if (m_kind == LaneKind::Arranger)
		{
			// The draw tool draws an arranger event over the stretch dragged, on release.
			m_draw = std::make_pair(view.start + (x * view.scale), mouseX);
			if (Rml::ElementDocument* document = m_element->GetOwnerDocument())
				document->AddEventListener(Rml::EventId::Mouseup, this);
			return;
		}
		Json added = {{"position", nearestBar(view.start + (x * view.scale))}};
		if (m_kind == LaneKind::Scale)
			added["scale"] = "C major";
		m_binding.send(command(".add"), added,
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok())
					return;
				const Json& added = result.value.at("event");
				const auto id = added.at("id").get<uint64_t>();
				if (std::none_of(m_list.begin(), m_list.end(), [id](const Event& each) { return each.id == id; }))
					m_list.push_back(eventFrom(added));
				show();
				edit(id); // before the project with the new event comes
			});
	}

	void GlobalEventLane::release(Rml::Event& event)
	{
		if (Rml::ElementDocument* document = m_element->GetOwnerDocument())
			document->RemoveEventListener(Rml::EventId::Mouseup, this);
		if (std::exchange(m_auditioning, false))
			m_binding.send("chord.audition", {{"on", false}});
		const Rml::Context* context = m_element->GetContext();
		const float ratio = context == nullptr ? 1.0F : context->GetDensityIndependentPixelRatio();
		const auto mouseX = event.GetParameter<float>("mouse_x", 0.0F);
		if (const std::optional<std::pair<double, float>> drawn = std::exchange(m_draw, std::nullopt))
			draw(drawn->first, (mouseX - drawn->second) / ratio);
		const std::optional<std::pair<uint64_t, float>> drag = std::exchange(m_drag, std::nullopt);
		if (!drag)
			return;
		const double dx = (mouseX - drag->second) / ratio;
		const auto found =
			std::find_if(m_list.begin(), m_list.end(), [&drag](const Event& each) { return each.id == drag->first; });
		if (std::abs(dx) < g_dragStart || found == m_list.end())
			return;
		const double to = seconds(found->start) + (dx * m_display.view().scale);
		if (m_kind == LaneKind::Arranger)
			m_binding.send("arranger.set",
				{{"event", found->id}, {"position", position(m_display.snapped(to, seconds(found->start)))}});
		else
			m_binding.send(command(".set"), {{"events", {{{"event", found->id}, {"position", nearestBar(to)}}}}});
	}

	void GlobalEventLane::draw(double from, double dx)
	{
		const double start = m_display.snapped(from);
		double end = m_display.snapped(from + (dx * m_display.view().scale));
		if (std::abs(dx) < g_dragStart)
		{
			// A click: up to the next bar line.
			const time::Ticks ticks = m_time.tempo.toTicks(start);
			end = m_time.tempo.toSeconds(m_time.signatures.barStart(m_time.signatures.toBarsBeats(ticks).bar + 1));
		}
		if (std::abs(end - start) * m_time.sampleRate < 1.0)
			return;
		m_binding.send(
			"arranger.add", {{"position", position(std::min(start, end))}, {"end", position(std::max(start, end))}});
	}
}
