// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/eventwaveforms.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Property.h>

#include <algorithm>
#include <cmath>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		// media.peaks' limits: columns, and columns times channels.
		constexpr int64_t g_maxPixels = 65536;
		constexpr int64_t g_maxColumnValues = 262144;

		Rml::Property dp(double value)
		{
			return {static_cast<float>(value), Rml::Unit::DP};
		}

		bool sameEvent(const EventWaveforms::Event& a, const EventWaveforms::Event& b)
		{
			return std::tie(a.lane, a.time.start, a.time.length, a.media, a.sampleRate, a.channels, a.offset) ==
				std::tie(b.lane, b.time.start, b.time.length, b.media, b.sampleRate, b.channels, b.offset);
		}
	}

	EventWaveforms::EventWaveforms(EngineBinding& binding, Rml::Element& container)
		: m_binding(binding), m_container(container.GetObserverPtr())
	{
		// Peaks that grow while they are generated or recorded, or become ready, are asked for again.
		for (const char* name : {"media.peaksProgress", "media.peaksReady"})
			m_subscriptions.push_back(m_binding.on(name,
				[this](const engine::Event& event)
				{
					const auto media = event.data.at("media").get<uint64_t>();
					for (Wave& wave : m_waves)
						if (wave.event.media == media)
							wave.stale = true;
					show(m_view);
				}));
	}

	void EventWaveforms::setEvents(const std::vector<Event>& events)
	{
		std::unordered_map<uint64_t, Wave*> old; // by event id
		for (Wave& wave : m_waves)
			old[wave.event.id] = &wave;
		std::vector<Wave> waves;
		waves.reserve(events.size());
		for (const Event& event : events)
		{
			Wave wave{event, nullptr, {}, false, false};
			if (const auto found = old.find(event.id); found != old.end())
			{
				// The element shows what it has until the new columns are in, and a question in flight stays the
				// only one.
				Wave& before = *found->second;
				wave.element = std::exchange(before.element, nullptr);
				wave.pending = before.pending;
				if (sameEvent(before.event, event))
				{
					wave.asked = before.asked;
					wave.stale = before.stale;
				}
			}
			waves.push_back(wave);
		}
		for (const Wave& wave : m_waves)
			if (wave.element != nullptr && m_container)
				m_container->RemoveChild(wave.element);
		m_waves = std::move(waves);
		show(m_view);
	}

	void EventWaveforms::show(const View& view)
	{
		m_view = view;
		for (Wave& wave : m_waves)
			update(wave);
	}

	void EventWaveforms::update(Wave& wave)
	{
		if (!m_container)
			return;
		// Like the canvas elements, the view and one view's width and height on each side.
		const View& view = m_view;
		const TimeSpan& time = wave.event.time;
		const double visible = view.width * view.scale;
		const double from = std::max(time.start, view.start - visible);
		const double to = std::min(time.end(), view.start + (2.0 * visible));
		const double laneTop = static_cast<double>(wave.event.lane) * view.laneHeight;
		if (!(visible > 0.0) || !(to > from) || laneTop + view.laneHeight <= view.top - view.height ||
			laneTop >= view.top + (2.0 * view.height))
		{
			if (wave.element != nullptr)
				m_container->RemoveChild(std::exchange(wave.element, nullptr));
			wave.asked = {};
			return;
		}
		if (wave.element == nullptr)
		{
			Rml::ElementPtr element = m_container->GetOwnerDocument()->CreateElement("waveform");
			wave.element = dynamic_cast<Waveform*>(element.get());
			if (wave.element == nullptr)
				return; // <waveform> isn't registered: nothing to draw with
			m_container->AppendChild(std::move(element));
		}
		wave.element->SetAttribute("media", static_cast<int64_t>(wave.event.media)); // for the UI tools
		wave.element->SetAttribute("start", view.start);
		wave.element->SetAttribute("scale", view.scale);
		wave.element->SetProperty(Rml::PropertyId::Top, dp(laneTop - view.top));
		wave.element->SetProperty(Rml::PropertyId::Height, dp(view.laneHeight));
		if (!wave.pending && (wave.stale || !covers(wave, wave.asked)))
			ask(wave, from, to);
	}

	bool EventWaveforms::covers(const Wave& wave, const Question& question) const
	{
		// What the lanes show of the event must be in what was asked for, at this zoom and dp ratio. The asked ends
		// are compared as they were computed, so that rounding doesn't make every view update ask again.
		const TimeSpan& time = wave.event.time;
		const double shownFrom = std::max(time.start, m_view.start);
		const double shownTo = std::min(time.end(), m_view.start + (m_view.width * m_view.scale));
		return question.scale == m_view.scale && question.dpRatio == m_view.dpRatio && shownFrom >= question.from &&
			shownTo <= question.to;
	}

	void EventWaveforms::ask(Wave& wave, double from, double to)
	{
		const Event& event = wave.event;
		const int64_t start = event.offset + std::llround((from - event.time.start) * event.sampleRate);
		const int64_t frames = std::llround((to - from) * event.sampleRate);
		const int64_t pixels = std::clamp<int64_t>(std::llround((to - from) / m_view.scale * m_view.dpRatio), 1,
			std::min(g_maxPixels, g_maxColumnValues / std::max(event.channels, 1)));
		if (frames < 1 || start < 0)
			return;
		const Question question{from, to, m_view.scale, m_view.dpRatio};
		wave.asked = question;
		wave.pending = true;
		wave.stale = false;
		m_binding.send("media.peaks",
			{{"media", event.media}, {"start", start}, {"frames", frames}, {"pixels", pixels}},
			[this, alive = std::weak_ptr<bool>(m_alive), event, question](const engine::Result& result)
			{
				if (!alive.expired())
					answered(event, question, result);
			});
	}

	void EventWaveforms::answered(const Event& event, const Question& question, const engine::Result& result)
	{
		const auto wave =
			std::find_if(m_waves.begin(), m_waves.end(), [&](const Wave& other) { return other.event.id == event.id; });
		if (wave == m_waves.end())
			return;
		wave->pending = false;
		if (!m_container || wave->element == nullptr || !result.ok())
			return;
		// The answer is shown unless the event or the view has moved on. Then, or if the peaks changed meanwhile,
		// what the view needs now is asked for.
		if (sameEvent(wave->event, event) && covers(*wave, question))
		{
			wave->asked = question;
			std::vector<PeakColumns> channels;
			for (const Json& channel : result.value.at("channels"))
				channels.push_back(
					{channel.at("min").get<std::vector<int16_t>>(), channel.at("max").get<std::vector<int16_t>>()});
			wave->element->setPeaks({question.from, question.to - question.from}, std::move(channels));
		}
		update(*wave);
	}
}
