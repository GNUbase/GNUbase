// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/rulertracks.h"

#include "ui/canvas.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include <algorithm>
#include <optional>
#include <string>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		Rml::Property dp(double value)
		{
			return {static_cast<float>(value), Rml::Unit::DP};
		}
	}

	TimeContext rulerTrackTime(const TimeContext& project, const Json& track)
	{
		TimeContext context = project;
		if (!track.value("timeOffsetOn", false))
			return context;
		const auto offset = track.value("timeOffset", int64_t{0});
		const bool linear = track.value("timeBase", std::string()) == "linear";
		const time::Ticks ticks =
			linear ? context.tempo.toTicks(static_cast<double>(offset) / context.sampleRate) : offset;
		const double seconds =
			linear ? static_cast<double>(offset) / context.sampleRate : context.tempo.toSeconds(ticks);
		context.startTime = -seconds;
		context.sampleStart = -seconds;
		context.barOffset = -static_cast<int>(context.signatures.toBarsBeats(ticks).bar);
		return context;
	}

	RulerLanes::RulerLanes(EventDisplay& display) : m_display(display)
	{
		display.onViewChanged(
			[this, alive = std::weak_ptr<bool>(m_alive)]
			{
				if (alive.expired())
					return;
				if (m_display.loads() != m_loads)
					build();
				place();
			});
		build();
		place();
	}

	RulerLanes::~RulerLanes()
	{
		for (const Rml::ObserverPtr<Rml::Element>& ruler : m_rulers)
			if (ruler && ruler->GetParentNode() != nullptr)
				ruler->GetParentNode()->RemoveChild(ruler.get());
	}

	// ponytail: builds every ruler again for each answer of project.get, as the display reads the whole project.
	void RulerLanes::build()
	{
		m_loads = m_display.loads();
		const Json& project = m_display.project();
		if (!project.is_object())
			return;
		Rml::Element& area = m_display.eventArea();
		for (const Rml::ObserverPtr<Rml::Element>& ruler : m_rulers)
			if (ruler && ruler->GetParentNode() != nullptr)
				ruler->GetParentNode()->RemoveChild(ruler.get());
		m_rulers.clear();
		TimeContext time;
		readTimeContext(project, time);
		for (const Json& track : project.at("tracks"))
		{
			if (track.at("type") != "ruler")
				continue;
			const std::optional<TimeFormat> format = timeFormatFromName(track.value("timeFormat", std::string()));
			Rml::ElementPtr element = area.GetOwnerDocument()->CreateElement("ruler");
			auto* ruler = dynamic_cast<Ruler*>(element.get());
			if (ruler == nullptr || !format)
				continue;
			ruler->setScale(rulerScale(*format, rulerTrackTime(time, track)));
			element->SetClassNames("ruler-lane");
			element->SetAttribute("track", std::to_string(track.at("id").get<uint64_t>()));
			element->SetAttribute("format", timeFormatName(*format));
			m_rulers.push_back(area.AppendChild(std::move(element))->GetObserverPtr());
		}
	}

	void RulerLanes::place()
	{
		const TimelineView& view = m_display.view();
		const std::vector<uint64_t>& tracks = m_display.laneTracks();
		const std::vector<double>& tops = m_display.laneTops();
		for (const Rml::ObserverPtr<Rml::Element>& ruler : m_rulers)
		{
			if (!ruler)
				continue;
			const uint64_t track = std::stoull(ruler->GetAttribute<Rml::String>("track", "0"));
			const auto lane = std::find(tracks.begin(), tracks.end(), track);
			const bool shown = lane != tracks.end() && tops.size() > tracks.size();
			ruler->SetClass("hidden", !shown);
			if (!shown)
				continue;
			const auto index = static_cast<size_t>(lane - tracks.begin());
			ruler->SetProperty(Rml::PropertyId::Top, dp(tops[index] - view.top));
			ruler->SetProperty(Rml::PropertyId::Height, dp(tops[index + 1] - tops[index]));
			ruler->SetAttribute("start", view.start);
			ruler->SetAttribute("scale", view.scale);
		}
		if (Rml::Context* context = m_display.eventArea().GetContext())
			context->RequestNextUpdate(0);
	}
}
