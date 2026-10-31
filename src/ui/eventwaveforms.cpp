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
							wave.askedScale = 0.0;
					show(m_view);
				}));
	}

	void EventWaveforms::setEvents(const std::vector<Event>& events)
	{
		std::unordered_map<uint64_t, Wave*> shown; // by event id
		for (Wave& wave : m_waves)
			if (wave.element != nullptr)
				shown[wave.event.id] = &wave;
		std::vector<Wave> waves;
		waves.reserve(events.size());
		for (const Event& event : events)
		{
			Wave wave{event, nullptr, {}, 0.0};
			if (const auto old = shown.find(event.id); old != shown.end() && old->second->element != nullptr)
			{
				// The element shows what it has until the new columns are in.
				wave.element = std::exchange(old->second->element, nullptr);
				if (sameEvent(old->second->event, event))
				{
					wave.asked = old->second->asked;
					wave.askedScale = old->second->askedScale;
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
		if (!m_container)
			return;
		// Like the canvas elements, the view and one view's width and height on each side.
		const double visible = view.width * view.scale;
		const double from = view.start - visible;
		const double to = view.start + (2.0 * visible);
		const double top = view.top - view.height;
		const double bottom = view.top + (2.0 * view.height);
		for (Wave& wave : m_waves)
		{
			const TimeSpan& time = wave.event.time;
			const double laneTop = static_cast<double>(wave.event.lane) * view.laneHeight;
			const TimeSpan range{std::max(time.start, from), std::min(time.end(), to) - std::max(time.start, from)};
			if (!(visible > 0.0) || !(range.length > 0.0) || laneTop + view.laneHeight <= top || laneTop >= bottom)
			{
				if (wave.element != nullptr)
					m_container->RemoveChild(std::exchange(wave.element, nullptr));
				wave.askedScale = 0.0;
				continue;
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

			// What the lanes show of the event must be in what was asked for, at this zoom.
			const double shownFrom = std::max(time.start, view.start);
			const double shownTo = std::min(time.end(), view.start + visible);
			if (wave.askedScale != view.scale || shownFrom < wave.asked.start || shownTo > wave.asked.end())
				ask(wave, range);
		}
	}

	void EventWaveforms::ask(Wave& wave, TimeSpan range)
	{
		const Event& event = wave.event;
		const int64_t start = event.offset + std::llround((range.start - event.time.start) * event.sampleRate);
		const int64_t frames = std::llround(range.length * event.sampleRate);
		const int64_t pixels = std::clamp<int64_t>(std::llround(range.length / m_view.scale * m_view.dpRatio), 1,
			std::min(g_maxPixels, g_maxColumnValues / std::max(event.channels, 1)));
		if (frames < 1 || start < 0)
			return;
		wave.asked = range;
		wave.askedScale = m_view.scale;
		m_binding.send("media.peaks",
			{{"media", event.media}, {"start", start}, {"frames", frames}, {"pixels", pixels}},
			[this, alive = std::weak_ptr<bool>(m_alive), id = event.id, range, scale = m_view.scale](
				const engine::Result& result)
			{
				if (alive.expired() || !m_container || !result.ok())
					return;
				// Only the latest question's answer is shown.
				const auto wave = std::find_if(m_waves.begin(), m_waves.end(),
					[&](const Wave& other)
					{
						return other.event.id == id && other.element != nullptr && other.askedScale == scale &&
							other.asked.start == range.start && other.asked.length == range.length;
					});
				if (wave == m_waves.end())
					return;
				std::vector<PeakColumns> channels;
				for (const Json& channel : result.value.at("channels"))
					channels.push_back(
						{channel.at("min").get<std::vector<int16_t>>(), channel.at("max").get<std::vector<int16_t>>()});
				wave->element->setPeaks(range, std::move(channels));
			});
	}
}
