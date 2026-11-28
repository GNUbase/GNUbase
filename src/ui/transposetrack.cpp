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
#include <array>
#include <cmath>
#include <string>
#include <tuple>

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

		// What the document tells the lane while a drag goes on.
		constexpr std::array g_dragEvents = {Rml::EventId::Mousemove, Rml::EventId::Mouseup};
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
					show(); // the lanes may show the track now
			});
		// One read of the project for the display and every lane; the track's own events carry its part of it.
		display.onSelectionShown(
			[this, alive = std::weak_ptr<bool>(m_alive)]
			{
				if (!alive.expired())
					follow();
			});
		m_subscriptions.push_back(
			m_binding.on(command(".changed"), [this](const engine::Event& event) { read(event.data); }));
		follow();
	}

	GlobalEventLane::~GlobalEventLane()
	{
		if (!alive())
			return;
		for (const Rml::EventId id : {Rml::EventId::Mousedown, Rml::EventId::Change, Rml::EventId::Blur})
			m_element->RemoveEventListener(id, this);
		if (Rml::ElementDocument* document = m_element->GetOwnerDocument())
			for (const Rml::EventId id : g_dragEvents)
				document->RemoveEventListener(id, this);
	}

	std::string GlobalEventLane::named(const char* suffix) const
	{
		if (m_kind == LaneKind::Scale)
			return "scale" + std::string(suffix);
		if (m_kind == LaneKind::Arranger)
			return "arranger-lane" + std::string(suffix);
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
		Event result{event.at("id").get<uint64_t>(), start, event.value("endSamples", start), {}, {}, false, 0};
		if (m_kind == LaneKind::Arranger)
		{
			result.text = result.value = event.at("name").get<std::string>();
			result.position = event.at("position").get<int64_t>();
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

	void GlobalEventLane::follow()
	{
		const Json& project = m_display.project();
		if (!project.is_object() || m_display.answers() == m_loaded)
			return;
		m_loaded = m_display.answers();
		const char* section = "chords";
		if (m_kind == LaneKind::Transpose || m_kind == LaneKind::Arranger)
			section = m_kind == LaneKind::Transpose ? "transpose" : "arranger";
		read(project.at(section));
	}

	void GlobalEventLane::read(const Json& section)
	{
		m_track = section.at("track").get<uint64_t>();
		m_showScales = section.value("showScales", false);
		m_audition = section.value("acousticFeedback", false) && section.value("audition", uint64_t{0}) != 0 &&
			!section.value("mute", false);
		m_linear = false;
		if (const Json& project = m_display.project(); project.is_object())
			for (const Json& track : project.at("tracks"))
				if (track.at("id") == m_track)
					m_linear = track.at("timeBase") == "linear";
		m_list.clear();
		for (const Json& event : section.at(m_kind == LaneKind::Scale ? "scales" : "events"))
			m_list.push_back(eventFrom(event));
		if (m_editing &&
			std::none_of(m_list.begin(), m_list.end(), [this](const Event& event) { return event.id == *m_editing; }))
			m_editing.reset();
		m_selection.erase(
			std::remove_if(m_selection.begin(), m_selection.end(), [this](uint64_t id) { return find(id) == nullptr; }),
			m_selection.end());
		settle();
		show();
	}

	void GlobalEventLane::send(
		const std::string& name, Json parameters, std::function<void(const engine::Result&)> done)
	{
		m_binding.send(name, std::move(parameters),
			[this, done = std::move(done), alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired())
					return;
				if (!result.ok())
					m_display.showRefusal(result.error);
				if (done)
					done(result);
			});
	}

	void GlobalEventLane::edit(uint64_t event)
	{
		const auto found =
			std::find_if(m_list.begin(), m_list.end(), [event](const Event& each) { return each.id == event; });
		if (!alive() || m_field == nullptr || found == m_list.end())
			return;
		m_editing = event;
		// "X" is the chord lane's undefined chord, typed afresh; an arranger event may be named "X".
		m_field->SetValue(m_kind == LaneKind::Chord && found->value == "X" ? std::string() : found->value);
		show();
		m_field->Focus();
	}

	void GlobalEventLane::deselect()
	{
		if (m_selection.empty())
			return;
		m_selection.clear();
		show();
	}

	bool GlobalEventLane::removeSelected()
	{
		if (m_kind != LaneKind::Arranger || m_selection.empty())
			return false;
		send("arranger.remove", {{"events", m_selection}});
		return true;
	}

	int64_t GlobalEventLane::nearestBar(double seconds) const
	{
		const TimeContext& time = m_display.time();
		const time::Ticks ticks = time.tempo.toTicks(std::max(0.0, seconds));
		int64_t bar = time.signatures.toBarsBeats(ticks).bar;
		const time::Ticks start = time.signatures.barStart(bar);
		const time::Ticks end = time.signatures.barStart(bar + 1);
		if ((ticks - start) * 2 > end - start)
			++bar;
		const time::Ticks at = time.signatures.barStart(bar);
		return m_linear ? std::llround(time.tempo.toSeconds(at) * time.sampleRate) : at;
	}

	int64_t GlobalEventLane::position(double seconds) const
	{
		const TimeContext& time = m_display.time();
		seconds = clampPositionSeconds(seconds, time.sampleRate);
		return m_linear ? positionSamples(seconds, time.sampleRate) : time.tempo.toTicks(seconds);
	}

	void GlobalEventLane::show()
	{
		if (!alive())
			return;
		// The same events keep their elements, so that a click still ends on the one it pressed.
		// Not before the display shows the track's lane: a track and its first event can come before the project.
		std::string rml;
		if (shownOnLane())
			for (const Event& event : m_list)
			{
				rml += R"(<div class=")" + named("-event") + (event.detected ? " detected" : "") + R"(" event=")" +
					std::to_string(event.id) + R"(">)" + Rml::StringUtilities::EncodeRml(event.text);
				// An arranger event's edges, which a drag resizes it by.
				if (m_kind == LaneKind::Arranger)
					rml +=
						R"(<div class="arranger-lane-edge" edge="start"/><div class="arranger-lane-edge" edge="end"/>)";
				rml += "</div>";
			}
		if (m_drawn)
			rml += R"(<div class=")" + named("-event") + R"( drawing" event="0"/>)";
		if (m_events->GetAttribute<Rml::String>("events", "") != rml)
		{
			m_events->SetAttribute("events", rml);
			m_events->SetInnerRML(rml);
		}
		// The selection changes the elements' classes only, so that a press that selects keeps its element.
		for (int i = 0; i < m_events->GetNumChildren(); ++i)
		{
			Rml::Element* element = m_events->GetChild(i);
			const auto id = element->GetAttribute<uint64_t>("event", 0);
			const bool selected = std::find(m_selection.begin(), m_selection.end(), id) != m_selection.end();
			if (element->IsClassSet("selected") != selected)
				element->SetClass("selected", selected);
		}
		m_field->SetClass("shown", m_editing.has_value());
		place();
	}

	bool GlobalEventLane::shownOnLane() const
	{
		const std::vector<uint64_t>& tracks = m_display.laneTracks();
		return m_track != 0 && std::find(tracks.begin(), tracks.end(), m_track) != tracks.end() &&
			(m_kind != LaneKind::Scale || m_showScales);
	}

	void GlobalEventLane::place()
	{
		if (!alive())
			return;
		const TimelineView& view = m_display.view();
		const std::vector<uint64_t>& tracks = m_display.laneTracks();
		const std::vector<double>& tops = m_display.laneTops();
		const auto lane = std::find(tracks.begin(), tracks.end(), m_track);
		const bool shown = shownOnLane();
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
			// An element from start to end, in seconds: arranger events as wide as their sections.
			const auto put = [this, &view, scale](Rml::Element& element, double start, double end)
			{
				element.SetProperty(Rml::PropertyId::Left, dp((start - view.start) / scale));
				if (m_kind == LaneKind::Arranger)
					element.SetProperty(Rml::PropertyId::Width, dp((end - start) / scale));
			};
			// An event where it shows: moved while a drag moves it.
			const auto span = [this](const Event& event)
			{
				const bool moved = m_moved &&
					(m_moved->event == event.id ||
						(m_moved->group &&
							std::find(m_selection.begin(), m_selection.end(), event.id) != m_selection.end()));
				return std::make_pair(seconds(event.start) + (moved ? m_moved->shift : 0.0),
					seconds(event.end) + (moved ? m_moved->endShift : 0.0));
			};
			for (int i = 0; i < m_events->GetNumChildren(); ++i)
			{
				Rml::Element* element = m_events->GetChild(i);
				const auto id = element->GetAttribute<uint64_t>("event", 0);
				if (id == 0 && m_drawn)
					put(*element, m_drawn->first, m_drawn->second);
				for (const Event& event : m_list)
					if (event.id == id)
						put(*element, span(event).first, span(event).second);
			}
			for (const Event& event : m_list)
				if (m_editing && event.id == *m_editing)
					put(*m_field, span(event).first, span(event).second);
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
		else if (event == Rml::EventId::Mousemove)
		{
			move(event);
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
				send("chord.set",
					{{"events",
						{{{"event", *id}, {m_kind == LaneKind::Scale ? "scale" : "chord", m_field->GetValue()}}}}});
			else if (m_kind == LaneKind::Arranger && id && !m_field->GetValue().empty()) // empty: keeps its name
				send("arranger.set", {{"event", *id}, {"name", m_field->GetValue()}});
			else if (m_kind == LaneKind::Transpose && value && id)
				send("transpose.set",
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
		m_edge = Edge::None;
		const Tool tool = m_display.tool();
		const bool drawing = tool == Tool::Draw || tool == Tool::Line;
		std::optional<uint64_t> on = eventId(event.GetTargetElement(), named("-event"));
		if (tool == Tool::Erase)
		{
			if (on)
				send(command(".remove"), {{"events", {*on}}});
			return;
		}
		// Arranger events may overlap (reference manual p. 404): the draw tool draws over them.
		if (m_kind == LaneKind::Arranger && drawing)
			on.reset();
		const auto listen = [this]
		{
			if (Rml::ElementDocument* document = m_element->GetOwnerDocument())
				for (const Rml::EventId id : g_dragEvents)
					document->AddEventListener(id, this);
		};
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
			if (m_kind == LaneKind::Arranger)
			{
				// A click selects the event, with Ctrl (Cmd) or Shift adds or removes it; an edge resizes it.
				const auto selected = std::find(m_selection.begin(), m_selection.end(), *on);
				if (event.GetParameter<int>("ctrl_key", 0) != 0 || event.GetParameter<int>("meta_key", 0) != 0 ||
					event.GetParameter<int>("shift_key", 0) != 0)
				{
					if (selected == m_selection.end())
						m_selection.push_back(*on);
					else
						m_selection.erase(selected);
				}
				else if (selected == m_selection.end())
				{
					m_selection = {*on};
				}
				if (!m_selection.empty() && m_onSelect)
					m_onSelect();
				const Rml::Element* target = event.GetTargetElement();
				const auto edge = target != nullptr ? target->GetAttribute<Rml::String>("edge", "") : Rml::String();
				if (edge == "start")
					m_edge = Edge::Start;
				else if (edge == "end")
					m_edge = Edge::End;
				show();
			}
			// Auditioning (reference manual pp. 1329-1330): the event plays while the button is down.
			if (m_audition && (m_kind == LaneKind::Chord || m_kind == LaneKind::Scale))
			{
				m_binding.send("chord.audition", {{"event", *on}});
				m_auditioning = true;
			}
			m_drag = std::make_pair(*on, event.GetParameter<float>("mouse_x", 0.0F));
			listen();
			return;
		}
		m_lastPress.reset(); // a press beside the events ends a double click
		if (!drawing)
		{
			// A press beside the arranger events clears their selection.
			if (!m_selection.empty())
			{
				m_selection.clear();
				show();
			}
			return;
		}
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
			listen();
			return;
		}
		Json added = {{"position", nearestBar(view.start + (x * view.scale))}};
		if (m_kind == LaneKind::Scale)
			added["scale"] = "C major";
		send(command(".add"), added,
			[this](const engine::Result& result)
			{
				if (!result.ok())
					return;
				const Json& added = result.value.at("event");
				const auto id = added.at("id").get<uint64_t>();
				if (std::none_of(m_list.begin(), m_list.end(), [id](const Event& each) { return each.id == id; }))
					m_list.push_back(eventFrom(added));
				show();
				edit(id); // before the project with the new event comes
			});
	}

	void GlobalEventLane::move(Rml::Event& event)
	{
		const Rml::Context* context = m_element->GetContext();
		const float ratio = context == nullptr ? 1.0F : context->GetDensityIndependentPixelRatio();
		const auto mouseX = event.GetParameter<float>("mouse_x", 0.0F);
		if (m_draw)
		{
			const double dx = (mouseX - m_draw->second) / ratio;
			m_drawn = std::abs(dx) < g_dragStart ? std::nullopt : std::make_optional(drawn(m_draw->first, dx));
			show();
		}
		if (m_drag)
			moveTo(m_drag->first, (mouseX - m_drag->second) / ratio);
	}

	const GlobalEventLane::Event* GlobalEventLane::find(uint64_t id) const
	{
		const auto found =
			std::find_if(m_list.begin(), m_list.end(), [id](const Event& each) { return each.id == id; });
		return found == m_list.end() ? nullptr : &*found;
	}

	bool GlobalEventLane::moveTo(uint64_t id, double dx)
	{
		const Event* found = find(id);
		if (std::abs(dx) >= g_dragStart && found != nullptr)
		{
			// A drag of one of the arranger events selected moves them all; an edge resizes the one.
			const bool group = m_kind == LaneKind::Arranger && m_edge == Edge::None && m_selection.size() > 1 &&
				std::find(m_selection.begin(), m_selection.end(), id) != m_selection.end();
			Moved moved{id, 0.0, 0.0, found->start, found->end, true, false, group};
			const double start = seconds(found->start);
			const double end = seconds(found->end);
			if (m_edge == Edge::None)
				moved.shift = moved.endShift = movedTo(*found, dx) - start;
			else if (m_edge == Edge::Start) // not past its end
				moved.shift = std::min(edgeTo(start, id, dx), end) - start;
			else
				moved.endShift = std::max(edgeTo(end, id, dx), start) - end;
			if (group) // none of them before the project start
			{
				for (const uint64_t each : m_selection)
					if (const Event* selected = find(each))
						moved.shift = std::max(moved.shift, -seconds(selected->start));
				moved.endShift = moved.shift;
			}
			m_moved = moved;
			place();
			return true;
		}
		// Back within reach of the press, the event shows where it is; an earlier drag's move waits for the lane to
		// read it.
		if (m_moved && m_moved->dragging)
		{
			m_moved.reset();
			place();
		}
		return false;
	}

	void GlobalEventLane::settle()
	{
		if (m_added && find(*m_added) != nullptr)
		{
			m_drawn.reset();
			m_added.reset();
		}
		if (m_moved && m_moved->answered)
			if (const Event* found = find(m_moved->event);
				found == nullptr || found->start != m_moved->from || found->end != m_moved->fromEnd)
				m_moved.reset();
	}

	void GlobalEventLane::release(Rml::Event& event)
	{
		if (Rml::ElementDocument* document = m_element->GetOwnerDocument())
			for (const Rml::EventId id : g_dragEvents)
				document->RemoveEventListener(id, this);
		if (std::exchange(m_auditioning, false))
			m_binding.send("chord.audition", {{"on", false}});
		const Rml::Context* context = m_element->GetContext();
		const float ratio = context == nullptr ? 1.0F : context->GetDensityIndependentPixelRatio();
		const auto mouseX = event.GetParameter<float>("mouse_x", 0.0F);
		// What a drag shows stays until the lane reads the event drawn or moved as the engine has it, which can come
		// after the answer; a refused edit leaves the events as they are.
		if (const std::optional<std::pair<double, float>> pressed = std::exchange(m_draw, std::nullopt))
		{
			const auto [start, end] = drawn(pressed->first, (mouseX - pressed->second) / ratio);
			m_drawn = std::make_pair(start, end);
			show();
			// The engine refuses an event shorter than a sample, or than a tick on a musical track.
			send("arranger.add", {{"position", position(start)}, {"end", position(end)}},
				[this](const engine::Result& result)
				{
					if (result.ok())
						m_added = result.value.at("event").at("id").get<uint64_t>();
					else
						m_drawn.reset();
					settle();
					show();
				});
		}
		const std::optional<std::pair<uint64_t, float>> drag = std::exchange(m_drag, std::nullopt);
		if (!drag)
			return;
		if (m_kind == LaneKind::Arranger && m_edge == Edge::None && dropOutside(event, drag->first))
		{
			m_moved.reset();
			place();
			return;
		}
		const double dx = (mouseX - drag->second) / ratio;
		const Event* moved = find(drag->first);
		if (!moveTo(drag->first, dx) || !m_moved || moved == nullptr)
			return;
		m_moved->dragging = false;
		const Event& found = *moved;
		const auto answered = [this, id = found.id](const engine::Result& result)
		{
			if (m_moved && m_moved->event == id)
			{
				m_moved->answered = true;
				if (!result.ok())
					m_moved.reset();
			}
			settle();
			place();
		};
		if (m_moved->group)
		{
			// All selected by as much in the track's time base, in one undo step, none before the project start. The
			// one dragged comes last, so that what the drag shows ends once all have moved.
			int64_t by = position(seconds(found.start) + m_moved->shift) - found.position;
			for (const uint64_t id : m_selection)
				if (const Event* each = find(id))
					by = std::max(by, -each->position);
			if (by == 0) // held at the project start: nothing moves
			{
				m_moved.reset();
				place();
				return;
			}
			Json commands = Json::array();
			for (const uint64_t id : m_selection)
				if (const Event* each = find(id); each != nullptr && id != found.id)
					commands.push_back({{"command", "arranger.set"},
						{"parameters", {{"event", id}, {"position", each->position + by}}}});
			commands.push_back({{"command", "arranger.set"},
				{"parameters", {{"event", found.id}, {"position", found.position + by}}}});
			send("history.transaction", {{"description", "Move Arranger Events"}, {"commands", std::move(commands)}},
				answered);
			return;
		}
		if (m_kind == LaneKind::Arranger && std::exchange(m_edge, Edge::None) != Edge::None)
			send("arranger.set",
				{{"event", found.id}, {"position", position(seconds(found.start) + m_moved->shift)},
					{"end", position(seconds(found.end) + m_moved->endShift)}},
				answered);
		else if (m_kind == LaneKind::Arranger)
			send("arranger.set", {{"event", found.id}, {"position", position(seconds(found.start) + m_moved->shift)}},
				answered);
		else
			send(command(".set"),
				{{"events",
					{{{"event", found.id},
						{"position", nearestBar(seconds(found.start) + (dx * m_display.view().scale))}}}}},
				answered);
	}

	bool GlobalEventLane::dropOutside(Rml::Event& event, uint64_t dragged)
	{
		Rml::Element* target = event.GetTargetElement();
		for (const Rml::Element* up = target; up != nullptr; up = up->GetParentNode())
			if (up == m_element.get())
				return false;
		if (target == nullptr)
			return false;
		std::vector<const Event*> dropped;
		if (std::find(m_selection.begin(), m_selection.end(), dragged) != m_selection.end())
		{
			for (const uint64_t id : m_selection)
				if (const Event* each = find(id))
					dropped.push_back(each);
		}
		else if (const Event* each = find(dragged))
		{
			dropped.push_back(each);
		}
		std::sort(dropped.begin(), dropped.end(),
			[](const Event* a, const Event* b) { return std::tie(a->start, a->id) < std::tie(b->start, b->id); });
		std::string ids;
		for (const Event* each : dropped)
			ids += (ids.empty() ? "" : " ") + std::to_string(each->id);
		const Rml::Dictionary parameters = {
			{"events", Rml::Variant(ids)}, {"mouse_y", Rml::Variant(event.GetParameter<float>("mouse_y", 0.0F))}};
		return !ids.empty() && !target->DispatchEvent(g_arrangerDrop, parameters);
	}

	std::pair<double, double> GlobalEventLane::drawn(double from, double dx) const
	{
		const std::vector<int64_t> others = magnets(0);
		double start = m_display.snapped(from, std::nullopt, others);
		if (std::abs(dx) < g_dragStart)
		{
			// A click: from where it snaps, or the grid line at or before it if that is after it, up to the bar line
			// after the point clicked, so that the event holds it also when its start snapped back over a bar line.
			if (start > from)
				start = m_display.gridLineBefore(from);
			const TimeContext& time = m_display.time();
			const time::Ticks ticks = time.tempo.toTicks(from);
			return {start, time.tempo.toSeconds(time.signatures.barStart(time.signatures.toBarsBeats(ticks).bar + 1))};
		}
		const double end = m_display.snapped(from + (dx * m_display.view().scale), std::nullopt, others);
		return {std::min(start, end), std::max(start, end)};
	}

	double GlobalEventLane::movedTo(const Event& event, double dx) const
	{
		const double to = seconds(event.start) + (dx * m_display.view().scale);
		if (m_kind == LaneKind::Arranger)
			return std::max(0.0, m_display.snapped(to, seconds(event.start), magnets(event.id)));
		const int64_t bar = nearestBar(to);
		return m_linear ? seconds(bar) : m_display.time().tempo.toSeconds(bar);
	}

	double GlobalEventLane::edgeTo(double edge, uint64_t event, double dx) const
	{
		return std::max(0.0, m_display.snapped(edge + (dx * m_display.view().scale), edge, magnets(event)));
	}

	std::vector<int64_t> GlobalEventLane::magnets(uint64_t except) const
	{
		std::vector<int64_t> edges;
		if (m_kind != LaneKind::Arranger)
			return edges;
		for (const Event& event : m_list)
			if (event.id != except)
			{
				edges.push_back(event.start);
				edges.push_back(event.end);
			}
		return edges;
	}
}
