// SPDX-License-Identifier: GPL-3.0-or-later
// Event volume and volume curves in the event display (#139, reference manual pp. 398-402): the volume lines of audio
// events, the event volume handle, the draw tool's curve points, the erase tool on them and Remove Volume Curve.
// docs/features/volume-curves.md describes them.
#include "engine/audio/fades.h"
#include "engine/document/serialize.h"
#include "ui/eventdisplay.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		constexpr double g_handleSize =
			8.0; // dp at the top of a lane, and around the event volume handle, that grab it
		constexpr double g_pointReach = 6.0; // dp around a curve point that grab it

		double gain(double db)
		{
			return std::pow(10.0, db / 20.0);
		}
	}

	double EventDisplay::volumeFraction(const Shown& shown, double y) const
	{
		const double top = m_laneTops[shown.lane];
		const double bottom = m_laneTops[shown.lane + 1];
		return bottom > top ? std::clamp((bottom - (y + m_view.top)) / (bottom - top), 0.0, 1.0) : 0.0;
	}

	double EventDisplay::draggedVolume(uint64_t event) const
	{
		const AudioFades& fades = m_fades.at(event);
		const auto pressed = m_fades.find(m_dragEvent);
		const auto shown = std::find_if(
			m_events.begin(), m_events.end(), [this](const Shown& each) { return each.id == m_dragEvent; });
		// The pressed event can go during the drag, with an undo or a remote client: then no volume changes.
		if (m_drag != Drag::Volume || !m_dragMoved || pressed == m_fades.end() || shown == m_events.end() ||
			!std::binary_search(m_selection.begin(), m_selection.end(), event))
			return fades.volume;
		// Every selected event gets the volume that the pressed one gets, as with fades.
		const double height = m_laneTops[shown->lane + 1] - m_laneTops[shown->lane];
		const double fraction = std::clamp(
			(gain(pressed->second.volume) / g_volumeLineTop) - (height > 0.0 ? m_dragDy / height : 0.0), 0.0, 1.0);
		return engine::volumeDb(fraction * g_volumeLineTop);
	}

	bool EventDisplay::volumeDown(const Shown& shown, double x, double y)
	{
		const auto fades = m_fades.find(shown.id);
		if (fades == m_fades.end() || !(fades->second.rate > 0.0) ||
			y + m_view.top - m_laneTops[shown.lane] > g_handleSize ||
			std::abs(x - ((((shown.start + shown.end) / 2.0) - m_view.start) / m_view.scale)) > g_handleSize)
			return false;
		if (!std::binary_search(m_selection.begin(), m_selection.end(), shown.id))
			select({shown.id});
		m_dragEvent = shown.id;
		m_drag = Drag::Volume;
		return true;
	}

	void EventDisplay::volumeUp()
	{
		Json commands = Json::array();
		for (const uint64_t id : m_fades.count(m_dragEvent) != 0 ? selectedAudio() : std::vector<uint64_t>())
			commands.push_back(
				{{"command", "audioEvent.set"}, {"parameters", {{"event", id}, {"volume", draggedVolume(id)}}}});
		m_drag = Drag::None;
		m_dragMoved = false;
		m_dragDy = 0.0;
		if (!commands.empty())
			edit("history.transaction", {{"description", "Change event volume"}, {"commands", std::move(commands)}});
		showFades();
	}

	std::optional<size_t> EventDisplay::curvePointAt(const Shown& shown, double x, double y) const
	{
		const auto fades = m_fades.find(shown.id);
		if (fades == m_fades.end() || !(fades->second.rate > 0.0))
			return std::nullopt;
		const AudioFades& audio = fades->second;
		const double bottom = m_laneTops[shown.lane + 1] - m_view.top;
		const double height = m_laneTops[shown.lane + 1] - m_laneTops[shown.lane];
		std::optional<size_t> found;
		double nearest = g_pointReach;
		for (size_t i = 0; i < audio.curve.size(); ++i)
		{
			const engine::VolumePoint& point = audio.curve[i];
			const double seconds = shown.start + (static_cast<double>(point.position - audio.offset) / audio.rate);
			const double px = (seconds - m_view.start) / m_view.scale;
			const double fraction = std::clamp(gain(audio.volume) * gain(point.volume) / g_volumeLineTop, 0.0, 1.0);
			const double distance = std::max(std::abs(x - px), std::abs(y - (bottom - (fraction * height))));
			if (distance <= nearest)
			{
				nearest = distance;
				found = i;
			}
		}
		return found;
	}

	bool EventDisplay::curveDown(const Shown& shown, double x, double y)
	{
		const auto fades = m_fades.find(shown.id);
		if (fades == m_fades.end() || !(fades->second.rate > 0.0))
			return false;
		if (!std::binary_search(m_selection.begin(), m_selection.end(), shown.id))
			select({shown.id});
		m_dragEvent = shown.id;
		m_dragPoint = curvePointAt(shown, x, y);
		m_dragDx = 0.0;
		m_dragDy = 0.0;
		m_drag = Drag::VolumePoint;
		return true;
	}

	std::vector<engine::VolumePoint> EventDisplay::draggedCurve(uint64_t event) const
	{
		const AudioFades& fades = m_fades.at(event);
		std::vector<engine::VolumePoint> curve = fades.curve;
		const auto shown =
			std::find_if(m_events.begin(), m_events.end(), [event](const Shown& each) { return each.id == event; });
		if (m_drag != Drag::VolumePoint || event != m_dragEvent || shown == m_events.end() ||
			(m_dragPoint && *m_dragPoint >= curve.size()))
			return curve;
		// A new point goes where the press was and follows the mouse. The pressed point moves from where it is by the
		// drag's movement (reference manual p. 400: a click only selects it): inside the event.
		double fromX = m_pressX;
		double fraction = volumeFraction(*shown, m_pressY + m_dragDy);
		if (m_dragPoint)
		{
			const engine::VolumePoint& point = curve[*m_dragPoint];
			const double height = m_laneTops[shown->lane + 1] - m_laneTops[shown->lane];
			const double at = shown->start + (static_cast<double>(point.position - fades.offset) / fades.rate);
			fromX = (at - m_view.start) / m_view.scale;
			// From the point's own volume, which can be above the line's top where the point is drawn at the top
			// (#683): there a drag down lowers it by the drag's distance, and a drag up takes it no higher.
			const double from = gain(fades.volume) * gain(point.volume) / g_volumeLineTop;
			fraction = std::clamp(from - (height > 0.0 ? m_dragDy / height : 0.0), 0.0, std::max(from, 1.0));
		}
		const double seconds = m_view.start + ((fromX + m_dragDx) * m_view.scale);
		const int64_t frame =
			fades.offset + std::clamp<int64_t>(std::llround((seconds - shown->start) * fades.rate), 0, fades.length);
		const double volume = engine::volumeDb(fraction * g_volumeLineTop / gain(fades.volume));
		if (m_dragPoint)
		{
			curve[*m_dragPoint].position = frame;
			if (m_dragDy != 0.0) // a sideways drag keeps the volume exactly
				curve[*m_dragPoint].volume = volume;
		}
		else
		{
			curve.push_back({frame, volume, 0.0});
		}
		std::stable_sort(curve.begin(), curve.end(),
			[](const engine::VolumePoint& a, const engine::VolumePoint& b) { return a.position < b.position; });
		return curve;
	}

	void EventDisplay::curveUp()
	{
		// A click on a point, without moving it, changes nothing.
		const bool clicked = m_dragPoint && m_dragDx == 0.0 && m_dragDy == 0.0;
		if (m_fades.count(m_dragEvent) != 0 && !clicked)
			edit("audioEvent.setVolumeCurve",
				{{"event", m_dragEvent}, {"points", engine::toJson(draggedCurve(m_dragEvent))}});
		m_dragPoint.reset();
		m_dragDx = 0.0;
		m_dragDy = 0.0;
	}

	bool EventDisplay::eraseCurvePoint(const Shown& shown, double x, double y)
	{
		const std::optional<size_t> point = curvePointAt(shown, x, y);
		if (!point)
			return false;
		std::vector<engine::VolumePoint> curve = m_fades.at(shown.id).curve;
		curve.erase(curve.begin() + static_cast<std::ptrdiff_t>(*point));
		edit("audioEvent.setVolumeCurve", {{"event", shown.id}, {"points", engine::toJson(curve)}});
		return true;
	}

	void EventDisplay::showVolume(FadeLines::Lane& lane, const Shown& shown) const
	{
		const AudioFades& fades = m_fades.at(shown.id);
		const bool selected = std::binary_search(m_selection.begin(), m_selection.end(), shown.id);
		if (selected)
			lane.handles.push_back((shown.start + shown.end) / 2.0); // the event volume handle
		const double volume = draggedVolume(shown.id);
		const std::vector<engine::VolumePoint> curve = draggedCurve(shown.id);
		// Events at 0 dB without a curve show no line, unless the volume is being dragged.
		const bool dragging = selected && m_drag == Drag::Volume && m_dragMoved;
		if (curve.empty() && volume == 0.0 && !dragging)
			return;
		lane.volumes.push_back(
			{shown.start, shown.end, fades.rate, fades.offset, gain(volume), engine::gainPoints(curve), selected});
	}

	void EventDisplay::addVolumeActions()
	{
		// Remove Volume Curve (reference manual p. 401): in the selection range, else of the selected events.
		m_actions["audio.removeVolumeCurve"] = [this]
		{
			if (m_range && m_range->end > m_range->start)
				edit("range.volumeCurve",
					{{"start", m_range->start}, {"end", m_range->end}, {"tracks", m_range->tracks}});
			else if (const std::vector<uint64_t> events = selectedAudio(); !events.empty())
				edit("event.removeVolumeCurve", {{"events", events}});
		};
	}
}
