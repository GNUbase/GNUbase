// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/engine.h"
#include "ui/eventdisplay.h"
#include "ui/timeformat.h"

#include <RmlUi/Core/ObserverPtr.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace Rml
{
	class Element;
}

// Ruler tracks in the UI (#447, reference manual pp. 157-159): a ruler on each ruler track's lane in the event
// display, in the track's own time format. docs/features/ruler-tracks.md describes them.
namespace gnubase::ui
{
	// The time a ruler track shows, the track as project.get returns it: the project's, but with its time offset on,
	// counting from the offset instead of the project setup's start time and bar offset: 0:00:00.000, 00:00:00:00 and
	// sample 0 at the offset, bar 1 at the bar it is in.
	TimeContext rulerTrackTime(const TimeContext& project, const engine::Json& track);

	// The ruler tracks' rulers on their lanes of an event display: a <ruler class="ruler-lane"> per ruler track in
	// the event area, with the track's id and time format in its attributes track and format, .hidden while its lane
	// isn't shown. They follow the project and the display's view. Use it on the UI thread; the display must outlive
	// it.
	class RulerLanes
	{
	public:
		explicit RulerLanes(EventDisplay& display);
		RulerLanes(const RulerLanes&) = delete;
		RulerLanes& operator=(const RulerLanes&) = delete;
		RulerLanes(RulerLanes&&) = delete;
		RulerLanes& operator=(RulerLanes&&) = delete;
		~RulerLanes();

	private:
		void build();
		void place();

		EventDisplay& m_display;
		std::shared_ptr<bool> m_alive = std::make_shared<bool>(true); // for the display's listener after it went
		uint64_t m_loads = 0;                                         // the display's project answers built for
		std::vector<Rml::ObserverPtr<Rml::Element>> m_rulers;
	};
}
