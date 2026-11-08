// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/inplaceeditor.h"

#include "ui/eventdisplay.h"
#include "ui/timeformat.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>

#include <array>
#include <string>

namespace gnubase::ui
{
	using engine::Json;

	namespace
	{
		constexpr const char* g_prefix = "inPlace:";
		// Presses go on to the event display, which leaves those in an in-place editor alone: RmlUi sends double
		// clicks only for presses that propagate.
		constexpr std::array<Rml::EventId, 2> g_events = {Rml::EventId::Click, Rml::EventId::Dblclick};

		std::string viewName(uint64_t track)
		{
			return g_prefix + std::to_string(track);
		}

		Rml::Property dp(double value)
		{
			return {static_cast<float>(value), Rml::Unit::DP};
		}

		void setClass(Rml::Element& element, const char* name, bool set)
		{
			if (element.IsClassSet(name) != set)
				element.SetClass(name, set);
		}
	}

	InPlaceEditors::InPlaceEditors(EventDisplay& display, EngineBinding& binding)
		: m_display(display), m_binding(binding)
	{
		Rml::Element& area = display.eventArea();
		Rml::ElementPtr element = area.GetOwnerDocument()->CreateElement("div");
		element->SetClass("in-place-editors", true);
		// Over the events, under the cross-hair cursor.
		Rml::Element* before = area.QuerySelector(".cross-hair-vertical");
		Rml::Element* added =
			before != nullptr ? area.InsertBefore(std::move(element), before) : area.AppendChild(std::move(element));
		m_area = added->GetObserverPtr();
		for (const Rml::EventId id : g_events)
			added->AddEventListener(id, this);
		m_subscriptions.push_back(
			m_binding.on("editor.changed", [this](const engine::Event& event) { follow(event.data); }));
		// The editors that are open already, such as ones an MCP client opened.
		m_binding.send("editor.inPlace", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok())
					return;
				for (const Json& track : result.value.at("tracks"))
					if (m_editors.count(track.get<uint64_t>()) == 0)
						requestView(track.get<uint64_t>());
			});
	}

	InPlaceEditors::~InPlaceEditors()
	{
		for (auto& [track, editor] : m_editors)
			editor.keys.reset();
		m_editors.clear();
		if (Rml::Element* area = m_area.get())
		{
			for (const Rml::EventId id : g_events)
				area->RemoveEventListener(id, this);
			if (Rml::Element* parent = area->GetParentNode())
				parent->RemoveChild(area);
		}
		m_display.setInPlaceTracks({});
	}

	std::set<uint64_t> InPlaceEditors::tracks() const
	{
		std::set<uint64_t> tracks;
		for (const auto& [track, editor] : m_editors)
			tracks.insert(track);
		return tracks;
	}

	KeyEditor* InPlaceEditors::keyEditor(uint64_t track)
	{
		const auto found = m_editors.find(track);
		return found != m_editors.end() ? found->second.keys.get() : nullptr;
	}

	void InPlaceEditors::requestView(uint64_t track)
	{
		m_binding.send("editor.get", {{"editor", viewName(track)}},
			[this, track, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired())
					return;
				if (result.ok())
					follow(result.value);
				else
					close(track); // its track is gone, such as with another project
			});
	}

	void InPlaceEditors::follow(const Json& view)
	{
		const std::string name = view.at("editor").get<std::string>();
		const std::string prefix = g_prefix;
		Rml::Element* area = m_area.get();
		if (name.rfind(prefix, 0) != 0 || area == nullptr)
			return;
		const uint64_t track = std::stoull(name.substr(prefix.size()));
		if (!view.at("open").get<bool>())
		{
			close(track);
			return;
		}
		Editor& editor = m_editors[track];
		editor.view = view;
		if (!editor.keys)
		{
			Rml::ElementPtr element = area->GetOwnerDocument()->CreateElement("div");
			element->SetClass("in-place-editor", true);
			element->SetAttribute("track", std::to_string(track));
			element->SetInnerRML(
				R"(<div class="key-editor"/><button class="in-place-toolbar-toggle")"
				R"( title="Show/Hide Toolbar"><svg class="icon" src="../icons/general-arrow-down.svg"/>)"
				R"(</button>)");
			Rml::Element* added = area->AppendChild(std::move(element));
			editor.element = added->GetObserverPtr();
			editor.keys = std::make_unique<KeyEditor>(m_binding, *added->GetFirstChild(), name);
			m_display.setInPlaceTracks(tracks());
		}
		editor.keys->show(m_display.project(), view);
		place(track, editor);
	}

	void InPlaceEditors::close(uint64_t track)
	{
		const auto found = m_editors.find(track);
		if (found == m_editors.end())
			return;
		found->second.keys.reset(); // before its element goes
		if (Rml::Element* element = found->second.element.get())
			element->GetParentNode()->RemoveChild(element);
		m_editors.erase(found);
		m_display.setInPlaceTracks(tracks());
	}

	void InPlaceEditors::update()
	{
		if (!m_area)
			return;
		// Another project, or a change to this one: the views' parts and selections may have changed with it.
		if (m_display.loads() != m_loads)
		{
			m_loads = m_display.loads();
			for (const auto& [track, editor] : m_editors)
				requestView(track);
		}
		for (auto& [track, editor] : m_editors)
			place(track, editor);
	}

	void InPlaceEditors::place(uint64_t track, Editor& editor)
	{
		Rml::Element* element = editor.element.get();
		const Json& project = m_display.project();
		if (element == nullptr || !project.is_object())
			return;
		// The track's own lane: not its lanes (#99) or automation tracks; none while a folded folder hides it.
		const std::vector<uint64_t>& lanes = m_display.laneTracks();
		const std::vector<double>& tops = m_display.laneTops();
		size_t lane = lanes.size();
		for (size_t i = 0; i < lanes.size(); ++i)
			if (lanes[i] == track && m_display.laneAutomation()[i] == 0 && m_display.laneTakes()[i] < 0)
				lane = i;
		const bool shown = lane + 1 < tops.size();
		setClass(*element, "hidden", !shown);
		if (!shown)
			return;
		const TimelineView& view = m_display.view();
		const std::pair<double, double> place{tops[lane] - view.top, tops[lane + 1] - tops[lane]};
		if (editor.place != place)
		{
			element->SetProperty(Rml::PropertyId::Top, dp(place.first));
			element->SetProperty(Rml::PropertyId::Height, dp(place.second));
			editor.place = place;
		}
		// The notes on the timeline: the grid's left and right edges at the times the ruler has there. Until the
		// grid has its size, the whole width of the lanes.
		const Rml::Context* context = element->GetContext();
		const float ratio = context != nullptr ? context->GetDensityIndependentPixelRatio() : 1.0F;
		Rml::Element& area = m_display.eventArea();
		const float areaLeft = area.GetAbsoluteOffset(Rml::BoxArea::Border).x;
		double offset = 0.0;
		double width = area.GetClientWidth() / ratio;
		if (Rml::Element* grid = element->QuerySelector(".key-editor-grid"))
			if (const float across = grid->GetBox().GetSize(Rml::BoxArea::Border).x; across > 0.0F)
			{
				offset = (grid->GetAbsoluteOffset(Rml::BoxArea::Border).x - areaLeft) / ratio;
				width = across / ratio;
			}
		// ponytail: the key editor's axis is linear in ticks, so it matches the ruler at the edges and wherever the
		// tempo doesn't change in the view; a tempo-mapped axis in KeyEditor when tempo changes need it.
		const TimeContext time = projectTime(project);
		const double start = view.start + (offset * view.scale);
		editor.keys->setTimeRange(
			std::make_pair(time.tempo.toTicks(start), time.tempo.toTicks(start + (width * view.scale))));
	}

	void InPlaceEditors::ProcessEvent(Rml::Event& event)
	{
		// The key editor below has seen it; the event display mustn't select or edit what is under the editor.
		event.StopPropagation();
		if (event.GetId() != Rml::EventId::Click)
			return;
		Rml::Element* editor = nullptr;
		Rml::Element* part = nullptr;
		Rml::Element* toggle = nullptr;
		for (Rml::Element* up = event.GetTargetElement(); up != nullptr && up != m_area.get(); up = up->GetParentNode())
		{
			if (up->IsClassSet("in-place-toolbar-toggle"))
				toggle = up;
			else if (up->IsClassSet("editor-part") && up->HasAttribute("part"))
				part = up;
			else if (up->IsClassSet("in-place-editor"))
				editor = up;
		}
		if (editor == nullptr)
			return;
		// The toolbar and the info line (reference manual p. 1259).
		if (toggle != nullptr)
			setClass(*editor, "toolbar-shown", !editor->IsClassSet("toolbar-shown"));
		// A click on a part activates it (p. 1152).
		if (part != nullptr)
			m_binding.send("editor.set",
				{{"editor", viewName(std::stoull(editor->GetAttribute<Rml::String>("track", "0")))},
					{"activePart", std::stoull(part->GetAttribute<Rml::String>("part", "0"))}});
	}
}
