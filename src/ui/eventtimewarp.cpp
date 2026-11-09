// SPDX-License-Identifier: GPL-3.0-or-later
// The time warp tool, Process Tempo, Process Bars, Set Definition from Tempo and copying warp markers in the event
// display (#159, docs/features/time-warp.md, reference manual pp. 1463-1468). The dialogs' Lua sends their commands.
#include "ui/eventdisplay.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace gnubase::ui
{
	using engine::Json;

	void EventDisplay::addTimeWarpActions()
	{
		const auto selectedEvents = [this]
		{
			std::string text;
			for (const uint64_t id : selectedAudio())
				text += (text.empty() ? "" : " ") + std::to_string(id);
			return text;
		};
		m_actions.insert({{"tempo.openProcessTempo", [this] { openFadeDialog("process-tempo.rml", {}); }},
			{"tempo.openProcessBars", [this] { openFadeDialog("process-bars.rml", {}); }},
			{"audio.setDefinitionFromTempo",
				[this, selectedEvents]
				{
					if (const std::string events = selectedEvents(); !events.empty())
						openFadeDialog("definition-from-tempo.rml", {{"events", events}});
				}},
			{"audio.copyWarpMarkers",
				[this]
				{
					if (const std::vector<uint64_t> events = selectedAudio(); !events.empty())
						m_warpSource = events.front();
				}},
			{"audio.pasteWarpMarkers", [this]
				{
					if (const std::vector<uint64_t> events = selectedAudio(); m_warpSource != 0 && !events.empty())
						edit("event.pasteWarpMarkers", {{"from", m_warpSource}, {"events", events}});
				}}});
	}

	bool EventDisplay::timeWarpDown()
	{
		m_drag = Drag::TimeWarp;
		return true;
	}

	// Warp Grid: the musical position pressed, snapped, plays where the mouse is released (tempo.warp). Free Warp: a
	// warp marker where the pressed audio event is pressed, moved to where it is released (clip.addWarpMarker).
	void EventDisplay::timeWarpUp(double x)
	{
		const double pressTime = m_view.start + (m_pressX * m_view.scale);
		const double releaseTime = m_view.start + (x * m_view.scale);
		if (m_warpMode == WarpMode::Free)
		{
			const auto fades = m_fades.find(m_dragEvent);
			const auto shown = std::find_if(
				m_events.begin(), m_events.end(), [this](const Shown& each) { return each.id == m_dragEvent; });
			if (fades == m_fades.end() || shown == m_events.end())
				return;
			const AudioFades& event = fades->second;
			const auto frame = [&event, &shown](double seconds)
			{ return event.offset + std::llround((seconds - shown->start) * event.rate); };
			edit("clip.addWarpMarker",
				{{"clip", event.clip}, {"position", frame(pressTime)}, {"target", frame(releaseTime)}});
			return;
		}
		if (!m_dragMoved)
			return;
		const time::Ticks position = time::toTicks(snapAt(pressTime, std::nullopt), m_time.tempo, m_time.sampleRate);
		edit("tempo.warp",
			{{"position", position}, {"to", samples(releaseTime)}, {"follow", m_warpMode == WarpMode::GridFollow}});
	}
}
