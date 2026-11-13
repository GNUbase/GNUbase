// SPDX-License-Identifier: GPL-3.0-or-later
// The tempo curve, the tempo track's lane in the event display and the tempo track editor (ui/tempotrack.h).
#include "ui/tempotrack.h"

#include "ui/canvas.h"
#include "ui/signaturetrack.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		constexpr double g_reach = 5.0;      // dp around a tempo event's point that a click hits
		constexpr double g_dragStart = 2.0;  // dp a press moves before it moves events
		constexpr int g_rampSteps = 16;      // straight pieces that draw a ramp, which is straight in beats
		constexpr double g_editorEdge = 8.0; // dp of the editor's curve area left and right of the project

		Rml::Property dp(double value)
		{
			return {static_cast<float>(value), Rml::Unit::DP};
		}

		Rml::Element& add(Rml::Element& parent, const char* tag, const char* classes)
		{
			Rml::ElementPtr element = parent.GetOwnerDocument()->CreateElement(tag);
			element->SetClassNames(classes);
			return *parent.AppendChild(std::move(element));
		}

		double rounded(double bpm)
		{
			return std::clamp(std::round(bpm * 1000.0) / 1000.0, time::g_minBpm, time::g_maxBpm);
		}

		time::TempoMap tempoFrom(const Json& points, const time::TempoMap& fallback)
		{
			std::vector<time::TempoPoint> tempo;
			for (const Json& point : points)
				tempo.push_back({point.at("position").get<time::Ticks>(), point.at("bpm").get<double>(),
					point.at("ramp").get<bool>()});
			try
			{
				return time::TempoMap(tempo);
			}
			catch (const std::invalid_argument&)
			{
				return fallback;
			}
		}

		std::string attribute(const Rml::Element& element, const char* name)
		{
			return element.GetAttribute<Rml::String>(name, "");
		}

		// The editor's buttons [tempo-tool] name their tools.
		const char* toolName(TempoTool tool)
		{
			switch (tool)
			{
			case TempoTool::Draw:
				return "draw";
			case TempoTool::Erase:
				return "erase";
			case TempoTool::Select:
				break;
			}
			return "select";
		}
	}

	ProjectTempo readTempo(const Json& project)
	{
		ProjectTempo result;
		result.time = projectTime(project);
		result.tempo = result.time.tempo;
		result.active = project.value("tempoTrackActive", false);
		result.fixed = project.value("fixedTempo", 120.0);
		result.tempoTrack = tempoFrom(project.value("tempoTrack", project.at("tempo")), result.tempo);
		result.length = project.value("length", 600.0);
		for (const Json& track : project.at("tracks"))
			if (track.at("type") == "tempo")
				result.track = track.at("id").get<uint64_t>();
		return result;
	}

	std::pair<double, double> visibleTempo(const ProjectTempo& tempo)
	{
		double slowest = tempo.active ? time::g_maxBpm : tempo.fixed;
		double fastest = tempo.active ? time::g_minBpm : tempo.fixed;
		for (const time::TempoPoint& point : tempo.tempoTrack.points())
		{
			slowest = std::min(slowest, point.bpm);
			fastest = std::max(fastest, point.bpm);
		}
		const double lower = std::max(0.0, (std::floor(slowest / 10.0) * 10.0) - 20.0);
		return {lower, std::max(lower + 40.0, (std::ceil(fastest / 10.0) * 10.0) + 20.0)};
	}

	double currentTempo(const TimeContext& context, int64_t cursor)
	{
		const double seconds = static_cast<double>(std::max<int64_t>(cursor, 0)) / context.sampleRate;
		return context.tempo.bpmAt(context.tempo.toTicks(seconds));
	}

	std::pair<std::string, Json> currentTempoCommand(
		const TimeContext& context, bool tempoTrackActive, int64_t cursor, double bpm)
	{
		if (!tempoTrackActive)
			return {"project.set", {{"tempo", rounded(bpm)}}};
		const double seconds = static_cast<double>(std::max<int64_t>(cursor, 0)) / context.sampleRate;
		const time::Ticks at = context.tempo.toTicks(seconds);
		time::Ticks position = 0;
		for (const time::TempoPoint& point : context.tempo.points())
			if (point.position <= at)
				position = point.position;
		return {"tempo.set", {{"events", {{{"position", position}, {"bpm", rounded(bpm)}}}}}};
	}

	std::string tempoText(double bpm)
	{
		std::ostringstream text;
		text << std::fixed << std::setprecision(3) << bpm;
		return text.str();
	}

	// The curve.

	TempoCurve::TempoCurve(EngineBinding& binding, Rml::Element& parent) : m_binding(binding)
	{
		Rml::Element& element = add(parent, "div", "tempo-curve");
		m_element = element.GetObserverPtr();
		m_line = dynamic_cast<Curve*>(&add(element, "curve", "tempo-line"));
		m_fixed = dynamic_cast<Curve*>(&add(element, "curve", "tempo-line fixed-tempo"));
		if (m_line == nullptr || m_fixed == nullptr)
			throw std::logic_error("<curve> isn't registered (ui::registerCanvas)");
		m_points = &add(element, "div", "tempo-points");
		m_rectangle = &add(element, "div", "tool-rectangle");
		element.AddEventListener(Rml::EventId::Mousedown, this);
		show();
	}

	TempoCurve::~TempoCurve()
	{
		if (!alive())
			return;
		m_element->RemoveEventListener(Rml::EventId::Mousedown, this);
		if (Rml::ElementDocument* document = m_element->GetOwnerDocument())
		{
			document->RemoveEventListener(Rml::EventId::Mousemove, this);
			document->RemoveEventListener(Rml::EventId::Mouseup, this);
		}
	}

	void TempoCurve::setTempo(ProjectTempo tempo)
	{
		m_tempo = std::move(tempo);
		// Events that went are no longer selected.
		const std::vector<time::TempoPoint>& points = m_tempo.tempoTrack.points();
		m_selection.erase(std::remove_if(m_selection.begin(), m_selection.end(),
							  [&points](int64_t position)
							  {
								  return std::none_of(points.begin(), points.end(),
									  [position](const time::TempoPoint& point) { return point.position == position; });
							  }),
			m_selection.end());
		show();
	}

	void TempoCurve::setView(double start, double scale)
	{
		m_start = start;
		m_scale = scale > 0.0 ? scale : 0.01;
		place();
	}

	void TempoCurve::setSelection(std::vector<int64_t> positions)
	{
		m_selection = std::move(positions);
		show();
	}

	void TempoCurve::select(std::vector<int64_t> positions)
	{
		if (positions == m_selection)
			return;
		setSelection(std::move(positions));
		if (m_onSelect)
			m_onSelect();
	}

	void TempoCurve::selectAll()
	{
		std::vector<int64_t> positions;
		for (const time::TempoPoint& point : m_tempo.tempoTrack.points())
			positions.push_back(point.position);
		select(std::move(positions));
	}

	void TempoCurve::removeSelected()
	{
		Json positions = Json::array();
		for (const int64_t position : m_selection)
			if (position != 0)
				positions.push_back(position);
		if (!positions.empty())
			m_binding.send("tempo.remove", {{"positions", positions}});
	}

	float TempoCurve::ratio() const
	{
		const Rml::Context* context = alive() ? m_element->GetContext() : nullptr;
		return context == nullptr ? 1.0F : context->GetDensityIndependentPixelRatio();
	}

	double TempoCurve::height() const
	{
		return alive() ? m_element->GetClientHeight() / ratio() : 0.0;
	}

	void TempoCurve::show()
	{
		if (!alive())
			return;
		m_element->SetClass("inactive", !m_tempo.active);
		const auto [lower, upper] = visibleTempo(m_tempo);
		const auto value = [lower = lower, upper = upper](double bpm) { return (bpm - lower) / (upper - lower); };
		// The tempo track's events where the project plays them: a ramp in straight pieces, a jump as a step.
		const std::vector<time::TempoPoint>& points = m_tempo.tempoTrack.points();
		std::vector<CurvePoint> curve;
		for (size_t i = 0; i < points.size(); ++i)
		{
			const time::TempoPoint& point = points[i];
			const bool ramp = i + 1 < points.size() && points[i + 1].ramp;
			curve.push_back({m_tempo.tempo.toSeconds(point.position), value(point.bpm),
				ramp ? engine::AutomationShape::Linear : engine::AutomationShape::Step, 0.0});
			if (!ramp)
				continue;
			const time::TempoPoint& next = points[i + 1];
			for (int step = 1; step < g_rampSteps; ++step)
			{
				const double part = static_cast<double>(step) / g_rampSteps;
				const auto at =
					point.position + std::llround(static_cast<double>(next.position - point.position) * part);
				curve.push_back({m_tempo.tempo.toSeconds(at), value(point.bpm + ((next.bpm - point.bpm) * part)),
					engine::AutomationShape::Linear, 0.0});
			}
		}
		m_line->setPoints(std::move(curve));
		// The fixed tempo, a horizontal line, in fixed tempo mode.
		std::vector<CurvePoint> fixed;
		if (!m_tempo.active)
			fixed.push_back({0.0, value(m_tempo.fixed), engine::AutomationShape::Step, 0.0});
		m_fixed->setPoints(std::move(fixed));

		std::string rml;
		for (const time::TempoPoint& point : points)
			rml += R"(<div class="tempo-point)" + std::string(point.ramp ? " ramp" : "") + R"(" position=")" +
				std::to_string(point.position) + R"("/>)";
		// The same points keep their elements, so that a click that selects one still ends on it.
		if (attribute(*m_points, "points") != rml)
		{
			m_points->SetAttribute("points", rml);
			m_points->SetInnerRML(rml);
		}
		for (int i = 0; i < m_points->GetNumChildren(); ++i)
		{
			Rml::Element* handle = m_points->GetChild(i);
			const auto position = handle->GetAttribute<int64_t>("position", -1);
			handle->SetClass(
				"selected", std::find(m_selection.begin(), m_selection.end(), position) != m_selection.end());
		}
		place();
	}

	void TempoCurve::place()
	{
		if (!alive())
			return;
		for (Curve* line : {m_line, m_fixed})
		{
			line->SetAttribute("start", m_start);
			line->SetAttribute("scale", m_scale);
		}
		const auto [lower, upper] = visibleTempo(m_tempo);
		const std::vector<time::TempoPoint>& points = m_tempo.tempoTrack.points();
		for (int i = 0; i < m_points->GetNumChildren() && static_cast<size_t>(i) < points.size(); ++i)
		{
			Rml::Element* handle = m_points->GetChild(i);
			const time::TempoPoint& point = points[static_cast<size_t>(i)];
			const Rml::Property left = dp((m_tempo.tempo.toSeconds(point.position) - m_start) / m_scale);
			handle->SetProperty(Rml::PropertyId::Left, left);
			// From the top, as a share of the height, which may not be laid out yet.
			const Rml::Property top(
				static_cast<float>(100.0 * (upper - point.bpm) / (upper - lower)), Rml::Unit::PERCENT);
			handle->SetProperty(Rml::PropertyId::Top, top);
		}
	}

	std::optional<int64_t> TempoCurve::hit(Rml::Vector2f mouse) const
	{
		const float reach = static_cast<float>(g_reach) * ratio();
		for (int i = 0; i < m_points->GetNumChildren(); ++i)
		{
			Rml::Element* handle = m_points->GetChild(i);
			const Rml::Vector2f centre = handle->GetAbsoluteOffset(Rml::BoxArea::Border) +
				(handle->GetBox().GetSize(Rml::BoxArea::Border) / 2.0F);
			if (std::abs(centre.x - mouse.x) <= reach && std::abs(centre.y - mouse.y) <= reach)
				return handle->GetAttribute<int64_t>("position", 0);
		}
		return std::nullopt;
	}

	void TempoCurve::ProcessEvent(Rml::Event& event)
	{
		if (!alive())
			return;
		if (event == Rml::EventId::Mousemove)
		{
			move(event);
			return;
		}
		if (event == Rml::EventId::Mouseup)
		{
			release(event);
			return;
		}
		if (event == Rml::EventId::Mousedown && event.GetParameter<int>("button", 0) == 0)
		{
			press(event);
			event.StopPropagation(); // the event display around it doesn't select or draw
		}
	}

	void TempoCurve::press(Rml::Event& event)
	{
		endDrag(); // a press that never saw its mouse button go up
		const Rml::Vector2f mouse = {
			event.GetParameter<float>("mouse_x", 0.0F), event.GetParameter<float>("mouse_y", 0.0F)};
		const TempoTool tool = m_tool ? m_tool() : TempoTool::Select;
		const std::optional<int64_t> on = hit(mouse);
		if (tool == TempoTool::Erase)
		{
			if (on && *on != 0)
				m_binding.send("tempo.remove", {{"positions", {*on}}});
			return;
		}
		if (tool == TempoTool::Draw)
		{
			addAt(mouse);
			return;
		}
		// The select tool, as on automation tracks: Ctrl or Shift adds to the selection.
		const bool adds = event.GetParameter<int>("ctrl_key", 0) != 0 || event.GetParameter<int>("meta_key", 0) != 0 ||
			event.GetParameter<int>("shift_key", 0) != 0;
		Drag drag;
		drag.from = mouse;
		drag.range = visibleTempo(m_tempo);
		if (on)
		{
			// A press selects the event, and a drag moves the selected ones.
			std::vector<int64_t> selection = m_selection;
			const bool selected = std::find(selection.begin(), selection.end(), *on) != selection.end();
			if (adds && selected)
				drag.toggled = on;
			else if (adds)
				selection.push_back(*on);
			else if (!selected)
				selection = {*on};
			select(std::move(selection));
			drag.onEvent = true;
		}
		else if (adds)
		{
			drag.base = m_selection;
		}
		m_drag = std::move(drag);
		if (Rml::ElementDocument* document = m_element->GetOwnerDocument())
		{
			document->AddEventListener(Rml::EventId::Mousemove, this);
			document->AddEventListener(Rml::EventId::Mouseup, this);
		}
	}

	void TempoCurve::move(Rml::Event& event)
	{
		if (!m_drag)
			return;
		const Rml::Vector2f mouse = {
			event.GetParameter<float>("mouse_x", 0.0F), event.GetParameter<float>("mouse_y", 0.0F)};
		const Rml::Vector2f from = m_drag->from;
		if (std::abs(mouse.x - from.x) / ratio() >= g_dragStart || std::abs(mouse.y - from.y) / ratio() >= g_dragStart)
			m_drag->moved = true;
		if (m_drag->onEvent || !m_drag->moved)
			return;
		// A rectangle selects the events in it.
		const Rml::Vector2f low = {std::min(from.x, mouse.x), std::min(from.y, mouse.y)};
		const Rml::Vector2f high = {std::max(from.x, mouse.x), std::max(from.y, mouse.y)};
		const Rml::Vector2f offset = m_element->GetAbsoluteOffset(Rml::BoxArea::Content);
		m_rectangle->SetClass("shown", true);
		m_rectangle->SetProperty(Rml::PropertyId::Left, dp((low.x - offset.x) / ratio()));
		m_rectangle->SetProperty(Rml::PropertyId::Top, dp((low.y - offset.y) / ratio()));
		m_rectangle->SetProperty(Rml::PropertyId::Width, dp((high.x - low.x) / ratio()));
		m_rectangle->SetProperty(Rml::PropertyId::Height, dp((high.y - low.y) / ratio()));
		std::vector<int64_t> selection = m_drag->base;
		for (int i = 0; i < m_points->GetNumChildren(); ++i)
		{
			Rml::Element* handle = m_points->GetChild(i);
			const Rml::Vector2f centre = handle->GetAbsoluteOffset(Rml::BoxArea::Border) +
				(handle->GetBox().GetSize(Rml::BoxArea::Border) / 2.0F);
			const auto position = handle->GetAttribute<int64_t>("position", 0);
			if (centre.x >= low.x && centre.x <= high.x && centre.y >= low.y && centre.y <= high.y &&
				std::find(selection.begin(), selection.end(), position) == selection.end())
				selection.push_back(position);
		}
		std::sort(selection.begin(), selection.end());
		select(std::move(selection));
	}

	void TempoCurve::endDrag()
	{
		if (Rml::ElementDocument* document = m_element->GetOwnerDocument())
		{
			document->RemoveEventListener(Rml::EventId::Mousemove, this);
			document->RemoveEventListener(Rml::EventId::Mouseup, this);
		}
		m_rectangle->SetClass("shown", false);
		m_drag.reset();
	}

	void TempoCurve::addAt(Rml::Vector2f mouse)
	{
		// The tempo where the mouse is.
		const Rml::Vector2f offset = m_element->GetAbsoluteOffset(Rml::BoxArea::Content);
		const double x = (mouse.x - offset.x) / ratio();
		const double y = (mouse.y - offset.y) / ratio();
		const auto [lower, upper] = visibleTempo(m_tempo);
		const double seconds = std::max(0.0, m_start + (x * m_scale));
		const double bpm = rounded(upper - ((upper - lower) * y / std::max(height(), 1.0)));
		select({});
		m_binding.send("tempo.add", {{"position", m_tempo.tempo.toTicks(seconds)}, {"bpm", bpm}});
	}

	void TempoCurve::release(Rml::Event& event)
	{
		const std::optional<Drag> drag = m_drag;
		endDrag();
		if (!drag)
			return;
		const Rml::Vector2f mouse = {
			event.GetParameter<float>("mouse_x", 0.0F), event.GetParameter<float>("mouse_y", 0.0F)};
		const double dx = (mouse.x - drag->from.x) / ratio();
		const double dy = (mouse.y - drag->from.y) / ratio();
		const bool dragged = drag->moved || std::abs(dx) >= g_dragStart || std::abs(dy) >= g_dragStart;
		if (!drag->onEvent)
		{
			// A click without a rectangle adds a tempo event there.
			if (!dragged)
				addAt(drag->from);
			return;
		}
		if (!dragged)
		{
			// A selected event clicked with Ctrl or Shift goes out of the selection.
			if (drag->toggled)
			{
				std::vector<int64_t> selection = m_selection;
				selection.erase(std::remove(selection.begin(), selection.end(), *drag->toggled), selection.end());
				select(std::move(selection));
			}
			return;
		}
		// Horizontally by time, vertically by tempo; the first event stays at the project start.
		const double tempoDelta = -dy * (drag->range.second - drag->range.first) / std::max(height(), 1.0);
		Json events = Json::array();
		std::vector<int64_t> moved;
		for (const time::TempoPoint& point : m_tempo.tempoTrack.points())
		{
			if (std::find(m_selection.begin(), m_selection.end(), point.position) == m_selection.end())
				continue;
			Json change = {{"position", point.position}, {"bpm", rounded(point.bpm + tempoDelta)}};
			int64_t to = point.position;
			if (point.position != 0)
			{
				to = std::max<int64_t>(
					1, m_tempo.tempo.toTicks(m_tempo.tempo.toSeconds(point.position) + (dx * m_scale)));
				change["to"] = to;
			}
			events.push_back(std::move(change));
			moved.push_back(to);
		}
		m_binding.send("tempo.set", {{"events", events}},
			[this, moved](const engine::Result& result)
			{
				if (result.ok())
					select(moved);
			});
	}

	// The lane.

	TempoLane::TempoLane(EventDisplay& display, EngineBinding& binding)
		: m_display(display), m_binding(binding), m_curve(binding, display.eventArea())
	{
		m_curve.setTool(
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

	// ponytail: reads the whole project after every change, as the markers do.
	void TempoLane::requestProject()
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
					m_curve.setTempo(readTempo(result.value));
					place();
				}
				if (std::exchange(m_stale, false))
					requestProject();
			});
	}

	void TempoLane::place()
	{
		Rml::Element* element = m_curve.element();
		if (element == nullptr)
			return;
		const TimelineView& view = m_display.view();
		const std::vector<uint64_t>& tracks = m_display.laneTracks();
		const std::vector<double>& tops = m_display.laneTops();
		const auto lane = std::find(tracks.begin(), tracks.end(), m_curve.tempo().track);
		const bool shown = m_curve.tempo().track != 0 && lane != tracks.end();
		element->SetClass("hidden", !shown);
		if (!shown)
			return;
		const auto index = static_cast<size_t>(lane - tracks.begin());
		element->SetProperty(Rml::PropertyId::Top, dp(tops[index] - view.top));
		element->SetProperty(Rml::PropertyId::Height, dp(tops[index + 1] - tops[index]));
		m_curve.setView(view.start, view.scale);
	}

	// The editor.

	TempoEditor::TempoEditor(engine::Engine& engine, EngineBinding& binding, Rml::ElementDocument& document)
		: m_engine(engine), m_binding(binding), m_document(document.GetObserverPtr())
	{
		if (Rml::Element* area = document.GetElementById("tempo-curve-area"))
		{
			m_curve = std::make_unique<TempoCurve>(binding, *area);
			m_curve->setTool([this] { return m_tool; });
			m_curve->onSelect(
				[this]
				{
					// The info line shows a tempo or a time signature event.
					if (m_signatures && !m_curve->selection().empty())
						m_signatures->setSelection({});
					show();
				});
		}
		if (Rml::Element* area = document.GetElementById("signature-area"))
		{
			m_signatures = std::make_unique<SignatureRow>(binding, *area);
			m_signatures->setTool([this] { return m_tool; });
			m_signatures->onSelect(
				[this]
				{
					if (m_curve && !m_signatures->selection().empty())
						m_curve->setSelection({});
					show();
				});
		}
		for (const Rml::EventId id :
			{Rml::EventId::Click, Rml::EventId::Change, Rml::EventId::Keydown, Rml::EventId::Resize})
			document.AddEventListener(id, this);
		for (const char* event : {"history.changed", "project.reset"})
			m_subscriptions.push_back(m_binding.on(event, [this](const engine::Event&) { requestProject(); }));
		m_subscriptions.push_back(
			m_binding.on("transport.changed", [this](const engine::Event& event) { followTransport(event.data); }));
		m_subscriptions.push_back(m_binding.on("settings.changed",
			[this](const engine::Event& event)
			{
				const Json& values = event.data.at("values");
				if (const auto type = values.find("editing.tempoType"); type != values.end() && type->is_string())
				{
					m_type = type->get<std::string>();
					show();
				}
			}));
		m_binding.send("settings.get", {{"key", "editing.tempoType"}},
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok() || !result.value.at("value").is_string())
					return;
				m_type = result.value.at("value").get<std::string>();
				show();
			});
		m_binding.send("transport.get", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok())
					return;
				followTransport(result.value);
			});
		requestProject();
	}

	TempoEditor::~TempoEditor()
	{
		m_curve.reset();
		m_signatures.reset();
		if (Rml::Element* document = m_document.get())
			for (const Rml::EventId id :
				{Rml::EventId::Click, Rml::EventId::Change, Rml::EventId::Keydown, Rml::EventId::Resize})
				document->RemoveEventListener(id, this);
	}

	void TempoEditor::requestProject()
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
				if (result.ok() && m_curve)
					m_curve->setTempo(readTempo(result.value));
				if (result.ok() && m_signatures)
					m_signatures->setTime(projectTime(result.value));
				show();
				if (std::exchange(m_stale, false))
					requestProject();
			});
	}

	void TempoEditor::followTransport(const Json& status)
	{
		m_cursor = status.at("position").get<int64_t>();
		// While playing the cursor moves without events: Current Tempo reads the playback position once a frame
		// (#831), as the transport bar's tempo field does.
		if (status.at("playing").get<bool>())
		{
			if (!m_playback)
				m_playback = m_binding.watch<int64_t>(
					m_document.get(), [this] { return m_engine.playbackPosition(); },
					[this](const int64_t& sample)
					{
						m_cursor = sample;
						return showCurrentTempo();
					});
		}
		else
			m_playback.reset();
		show();
	}

	bool TempoEditor::showCurrentTempo()
	{
		Rml::Element* document = m_document.get();
		if (document == nullptr || !m_curve)
			return false;
		auto* control = dynamic_cast<Rml::ElementFormControl*>(document->GetElementById("current-tempo"));
		const std::string value = tempoText(currentTempo(m_curve->tempo().time, m_cursor));
		if (control == nullptr || control->IsPseudoClassSet("focus") || control->GetValue() == value)
			return false; // what is being typed stays
		m_showing = true;
		control->SetValue(value);
		m_showing = false;
		return true;
	}

	void TempoEditor::show()
	{
		Rml::Element* document = m_document.get();
		if (document == nullptr || !m_curve)
			return;
		const ProjectTempo& tempo = m_curve->tempo();
		// The fields' change events that showing sends aren't the user's.
		const auto set = [this, document](const char* id, const std::string& value)
		{
			if (auto* control = dynamic_cast<Rml::ElementFormControl*>(document->GetElementById(id));
				control != nullptr && control->GetValue() != value)
			{
				m_showing = true;
				control->SetValue(value);
				m_showing = false;
			}
		};
		if (Rml::Element* active = document->GetElementById("tempo-track-active"))
			active->SetClass("checked", tempo.active);
		Rml::ElementList tools;
		document->QuerySelectorAll(tools, "[tempo-tool]");
		for (Rml::Element* button : tools)
			button->SetClass("checked", attribute(*button, "tempo-tool") == toolName(m_tool));
		set("tempo-type", m_type);
		showCurrentTempo(); // the fixed tempo, or the tempo at the cursor
		// The info line: the first selected event.
		const std::vector<int64_t>& selection = m_curve->selection();
		const std::vector<time::TempoPoint>& points = tempo.tempoTrack.points();
		const auto first = std::find_if(points.begin(), points.end(), [&selection](const time::TempoPoint& point)
			{ return std::find(selection.begin(), selection.end(), point.position) != selection.end(); });
		if (Rml::Element* info = document->GetElementById("tempo-info"))
			info->SetClass("hidden", first == points.end());
		if (first != points.end())
		{
			if (Rml::Element* position = document->GetElementById("tempo-position"))
			{
				const std::string text = Rml::StringUtilities::EncodeRml(
					formatTime(TimeFormat::BarsBeats, tempo.tempo.toSeconds(first->position), tempo.time));
				if (position->GetInnerRML() != text)
					position->SetInnerRML(text);
			}
			set("tempo-value", tempoText(first->bpm));
			set("tempo-ramp", first->ramp ? "ramp" : "jump");
		}
		// Or the first selected time signature event: its bar as the ruler counts them, and its signature.
		std::optional<time::SignatureChange> signature;
		if (m_signatures)
			for (const time::SignatureChange& change : tempo.time.signatures.changes())
				if (!signature &&
					std::find(m_signatures->selection().begin(), m_signatures->selection().end(), change.bar) !=
						m_signatures->selection().end())
					signature = change;
		if (Rml::Element* info = document->GetElementById("signature-info"))
			info->SetClass("hidden", !signature);
		if (signature)
		{
			if (Rml::Element* position = document->GetElementById("signature-position"))
			{
				const std::string text = std::to_string(signature->bar + 1 + tempo.time.barOffset);
				if (position->GetInnerRML() != text)
					position->SetInnerRML(text);
			}
			set("signature-value",
				std::to_string(signature->signature.numerator) + "/" +
					std::to_string(signature->signature.denominator));
		}
		place();
	}

	void TempoEditor::place()
	{
		Rml::Element* document = m_document.get();
		Rml::Element* area = document != nullptr ? document->GetElementById("tempo-curve-area") : nullptr;
		if (area == nullptr || !m_curve)
			return;
		const Rml::Context* context = area->GetContext();
		const float ratio = context == nullptr ? 1.0F : context->GetDensityIndependentPixelRatio();
		// The whole project, with a little room either side.
		const ProjectTempo& tempo = m_curve->tempo();
		const double width = std::max(1.0, (area->GetClientWidth() / ratio) - (2.0 * g_editorEdge));
		const double scale = tempo.length / width;
		const double start = -g_editorEdge * scale;
		m_curve->setView(start, scale);
		if (m_signatures)
			m_signatures->setView(start, scale);
		if (auto* ruler = dynamic_cast<Ruler*>(document->GetElementById("tempo-ruler")))
		{
			ruler->setScale(rulerScale(TimeFormat::BarsBeats, tempo.time));
			ruler->SetAttribute("start", start);
			ruler->SetAttribute("scale", scale);
		}
		if (Rml::Element* scaleLabels = document->GetElementById("tempo-scale"))
		{
			const auto [lower, upper] = visibleTempo(tempo);
			std::string rml;
			constexpr int labels = 4;
			for (int i = 0; i <= labels; ++i)
				rml += R"(<div class="tempo-scale-label">)" +
					std::to_string(std::lround(upper - ((upper - lower) * static_cast<double>(i) / labels))) + "</div>";
			if (attribute(*scaleLabels, "labels") != rml)
			{
				scaleLabels->SetAttribute("labels", rml);
				scaleLabels->SetInnerRML(rml);
				for (int i = 0; i < scaleLabels->GetNumChildren(); ++i)
				{
					const Rml::Property top(
						100.0F * static_cast<float>(i) / static_cast<float>(labels), Rml::Unit::PERCENT);
					scaleLabels->GetChild(i)->SetProperty(Rml::PropertyId::Top, top);
				}
			}
		}
	}

	void TempoEditor::ProcessEvent(Rml::Event& event)
	{
		const Rml::Element* target = event.GetTargetElement();
		if (target == nullptr || !m_curve)
			return;
		if (event == Rml::EventId::Resize)
		{
			place();
			return;
		}
		if (event == Rml::EventId::Keydown)
		{
			const auto key = static_cast<Rml::Input::KeyIdentifier>(event.GetParameter<int>("key_identifier", 0));
			if ((key == Rml::Input::KI_BACK || key == Rml::Input::KI_DELETE) && target->GetTagName() != "input")
			{
				m_curve->removeSelected();
				if (m_signatures)
					m_signatures->removeSelected();
			}
			return;
		}
		const ProjectTempo& tempo = m_curve->tempo();
		const std::string id = target->GetId();
		if (event == Rml::EventId::Click)
		{
			for (const Rml::Element* element = target; element != nullptr; element = element->GetParentNode())
				if (element->HasAttribute("tempo-tool"))
				{
					const std::string name = attribute(*element, "tempo-tool");
					if (name == "draw")
						m_tool = TempoTool::Draw;
					else if (name == "erase")
						m_tool = TempoTool::Erase;
					else
						m_tool = TempoTool::Select;
					show();
					return;
				}
			if (id == "tempo-track-active")
				m_binding.send("project.set", {{"tempoTrackActive", !tempo.active}});
			return;
		}
		// Change: the type of new events, the current tempo and the info line's fields.
		if (m_showing)
			return;
		const auto value = event.GetParameter<Rml::String>("value", "");
		if (id == "tempo-type" && value != m_type && !value.empty())
		{
			m_type = value;
			m_binding.send("settings.set", {{"key", "editing.tempoType"}, {"value", value}});
			return;
		}
		char* end = nullptr;
		const double bpm = std::strtod(value.c_str(), &end);
		const bool number = end != value.c_str() && bpm >= time::g_minBpm && bpm <= time::g_maxBpm;
		const bool entered = event.GetParameter<bool>("linebreak", false); // text fields change on Enter
		if (id == "current-tempo" && number && entered)
		{
			if (rounded(bpm) != rounded(currentTempo(tempo.time, m_cursor)))
			{
				const auto [command, parameters] = currentTempoCommand(tempo.time, tempo.active, m_cursor, bpm);
				m_binding.send(command, parameters);
			}
			return;
		}
		if (id == "signature-value" && entered && m_signatures)
		{
			if (const auto signature = parseSignature(value))
			{
				Json events = Json::array();
				for (const int64_t bar : m_signatures->selection())
					events.push_back(
						{{"bar", bar}, {"numerator", signature->first}, {"denominator", signature->second}});
				if (!events.empty())
					m_binding.send("signature.set", {{"events", events}});
			}
			return;
		}
		const std::vector<int64_t>& selection = m_curve->selection();
		if (selection.empty())
			return;
		if (id == "tempo-value" && number && entered)
		{
			Json events = Json::array();
			for (const int64_t position : selection)
				events.push_back({{"position", position}, {"bpm", rounded(bpm)}});
			m_binding.send("tempo.set", {{"events", events}});
		}
		else if (id == "tempo-ramp" && (value == "ramp" || value == "jump"))
		{
			Json events = Json::array();
			for (const int64_t position : selection)
				if (position != 0) // nothing ramps to the first
					events.push_back({{"position", position}, {"ramp", value == "ramp"}});
			if (!events.empty())
				m_binding.send("tempo.set", {{"events", events}});
		}
	}
}
