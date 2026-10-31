// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui/canvas.h"
#include "ui/enginebinding.h"

#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace Rml
{
	class Element;
}

// The waveforms of the event display's audio events (#83), drawn from the engine's waveform overviews (media.peaks).
// docs/features/peaks.md describes them.
namespace gnubase::ui
{
	// Fills a container over the event lanes with a <waveform> per audio event around the view, over the event's
	// lane, and asks media.peaks for the columns of the part around the view whenever the zoom or the dp ratio
	// changes, the view leaves that part, or the media file's peaks grow or become ready. Each waveform has at most
	// one question in flight: what the view needs meanwhile is asked once the answer is in. Use it on the UI thread;
	// the binding must outlive it. If the container goes, it shows nothing more.
	class EventWaveforms
	{
	public:
		// An audio event: where it is on the timeline and which frames of which media file it plays.
		struct Event
		{
			uint64_t id = 0; // the audio event's
			size_t lane = 0;
			TimeSpan time; // seconds
			uint64_t media = 0;
			double sampleRate = 0.0; // the media file's
			int channels = 1;
			int64_t offset = 0; // the file's frame at the event's start
		};

		// What the event lanes show.
		struct View
		{
			double start = 0.0;      // seconds at the left edge
			double scale = 0.01;     // seconds per dp
			double top = 0.0;        // dp scrolled down
			double laneHeight = 0.0; // dp
			double width = 0.0;      // of the lanes, in dp
			double height = 0.0;
			float dpRatio = 1.0F;
		};

		EventWaveforms(EngineBinding& binding, Rml::Element& container);

		// The project's audio events. Waveforms of events that didn't change keep their columns.
		void setEvents(const std::vector<Event>& events);
		void show(const View& view);

	private:
		// A media.peaks question: the seconds from and to, at a zoom and dp ratio.
		struct Question
		{
			double from = 0.0;
			double to = 0.0;
			double scale = 0.0; // 0 when nothing was asked
			float dpRatio = 0.0F;
		};

		struct Wave
		{
			Event event;
			Waveform* element = nullptr; // while the event is around the view
			Question asked;              // the latest question, in flight or answered
			bool pending = false;        // asked is in flight
			bool stale = false;          // the file's peaks changed since asked
		};

		void update(Wave& wave);
		bool covers(const Wave& wave, const Question& question) const; // what the view shows of the event
		void ask(Wave& wave, double from, double to);
		void answered(const Event& event, const Question& question, const engine::Result& result);

		EngineBinding& m_binding;
		Rml::ObserverPtr<Rml::Element> m_container;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for answers after it went
		std::vector<Wave> m_waves;
		View m_view;
		std::vector<Subscription> m_subscriptions;
	};
}
