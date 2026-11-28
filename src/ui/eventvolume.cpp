// SPDX-License-Identifier: GPL-3.0-or-later
// Event volume and volume curves in the event display (#139, reference manual pp. 398-402): the volume lines of audio
// events, the event volume control, the draw tool's curve points, the erase tool on them and Remove Volume Curve.
// docs/features/volume-curves.md describes them.
#include "engine/audio/fades.h"
#include "engine/document/document.h"
#include "engine/document/serialize.h"
#include "ui/eventdisplay.h"
#include "ui/eventwaveforms.h"

#include <RmlUi/Core/Element.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		constexpr double g_handleSize =
			8.0; // dp at the bottom of a lane, and around the event volume control, that grab it
		constexpr double g_faderInset = 16.0; // dp from an event's start to its volume control's middle, past its edge
		constexpr double g_pointReach = 6.0;  // dp around a curve point that grab it
		constexpr double g_fineVolume = 0.1;  // of the mouse's movement that moves the control with Shift
		constexpr double g_wheelVolume = 1.0; // dB a line of the mouse wheel
		constexpr double g_wheelFade = 10.0;  // dp of the display a line of the mouse wheel moves a fade's end

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

	std::optional<double> EventDisplay::faderOf(const Shown& shown) const
	{
		// At the bottom left (reference manual p. 401), on events wide enough for it beside their edges.
		const double inset = g_faderInset * m_view.scale;
		return shown.end - shown.start >= 2.0 * inset ? std::optional<double>(shown.start + inset) : std::nullopt;
	}

	bool EventDisplay::volumeDown(const Shown& shown, double x, double y)
	{
		// Only selected events show their volume control (showVolume), so only theirs can be grabbed, as the fade
		// handles.
		const auto fades = m_fades.find(shown.id);
		const std::optional<double> fader = faderOf(shown);
		if (fades == m_fades.end() || !(fades->second.rate > 0.0) || !fader ||
			!std::binary_search(m_selection.begin(), m_selection.end(), shown.id) ||
			m_laneTops[shown.lane + 1] - (y + m_view.top) > g_handleSize ||
			std::abs(x - ((*fader - m_view.start) / m_view.scale)) > g_handleSize)
			return false;
		m_dragEvent = shown.id;
		m_drag = Drag::Volume;
		m_volumeDy = 0.0;
		return true;
	}

	void EventDisplay::volumeMove(double dy, bool fine)
	{
		// From where the control is, so that pressing or letting go of Shift doesn't make it jump.
		m_dragDy += (dy - m_volumeDy) * (fine ? g_fineVolume : 1.0);
		m_volumeDy = dy;
		showFades();
	}

	void EventDisplay::showVolumeReadout()
	{
		const auto shown = std::find_if(
			m_events.begin(), m_events.end(), [this](const Shown& each) { return each.id == m_dragEvent; });
		const std::optional<double> fader = shown != m_events.end() ? faderOf(*shown) : std::nullopt;
		const bool dragging = m_drag == Drag::Volume && m_dragMoved && fader && m_fades.count(m_dragEvent) != 0;
		m_volumeReadout->SetClass("shown", dragging);
		if (!dragging)
			return;
		const double volume = draggedVolume(m_dragEvent);
		std::array<char, 48> text{};
		std::snprintf(text.data(), text.size(), "%.1f dB (%+.1f)", volume, volume - m_fades.at(m_dragEvent).volume);
		m_volumeReadout->SetInnerRML(text.data());
		m_volumeReadout->SetProperty(Rml::PropertyId::Left,
			Rml::Property(static_cast<float>(((*fader - m_view.start) / m_view.scale) + g_handleSize), Rml::Unit::DP));
		m_volumeReadout->SetProperty(Rml::PropertyId::Top,
			Rml::Property(static_cast<float>(m_laneTops[shown->lane] - m_view.top), Rml::Unit::DP));
	}

	bool EventDisplay::wheelVolume(double x, double y, double lines, bool fades)
	{
		const Shown* shown = eventAt(x, y);
		if (shown == nullptr || lines == 0.0)
			return false;
		const auto found = m_fades.find(shown->id);
		if (found == m_fades.end() || !(found->second.rate > 0.0))
			return false;
		const AudioFades& audio = found->second;
		// Lines faster than the project reads after them, several in a frame on a quick turn, go on from what the
		// wheel sent last, which the record keeps until the project has it.
		const std::string key = std::to_string(shown->id);
		const Json* record = m_binding.shown(m_wheelSteps, m_read);
		const Json sent = record != nullptr && record->contains(key) ? record->at(key) : Json::object();
		const auto send = [this, &key](const char* description, const char* command, Json parameters, Json value)
		{
			m_binding.sendStep(0, m_wheelSteps, description,
				Json::array({{{"command", command}, {"parameters", std::move(parameters)}}}),
				[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
				{
					if (!alive.expired() && !result.ok())
						showRefusal(result.error);
				},
				{{key, std::move(value)}});
		};
		if (!fades)
		{
			// Up, which turns the wheel by negative lines, is louder.
			const double from = sent.value("volume", audio.volume);
			const double volume =
				std::clamp(from - (lines * g_wheelVolume), engine::g_minEventVolume, engine::g_maxEventVolume);
			if (volume != from)
				send("Change event volume", "audioEvent.set", {{"event", shown->id}, {"volume", volume}},
					{{"volume", volume}});
			return true;
		}
		// The fade in's end in the event's left half, the fade out's start in its right half (p. 382): up lengthens.
		const bool in = m_view.start + (x * m_view.scale) < (shown->start + shown->end) / 2.0;
		const int64_t own = sent.value(in ? "fadeIn" : "fadeOut", (in ? audio.in : audio.out).length);
		const int64_t other = sent.value(in ? "fadeOut" : "fadeIn", (in ? audio.out : audio.in).length);
		const int64_t length = std::clamp<int64_t>(own - std::llround(lines * g_wheelFade * m_view.scale * audio.rate),
			0, std::max<int64_t>(0, audio.fadeRoom - other));
		if (length != own)
			send("Change fades", "audioEvent.setFades",
				{{"event", shown->id}, {in ? "fadeIn" : "fadeOut", {{"length", length}}}},
				{{in ? "fadeIn" : "fadeOut", length}});
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
		m_volumeDy = 0.0;
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
			const double seconds = shown.start + (static_cast<double>(point.position - audio.offset) / audio.timeRate);
			if (seconds < shown.start || seconds > shown.end)
				continue; // the volume line shows no square for it: a split or resize left it outside the event
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
		m_drawnCurve.clear();
		if (m_pressed == "ctrl") // draws a curve (reference manual p. 400)
		{
			m_dragPoint.reset();
			m_drawnCurve.emplace_back(x, y);
		}
		m_dragDx = 0.0;
		m_dragDy = 0.0;
		m_drag = Drag::VolumePoint;
		return true;
	}

	void EventDisplay::curveMove(double x, double y)
	{
		m_dragDx = x - m_pressX;
		m_dragDy = y - m_pressY;
		drawCurveTo(x, y, false);
		showFades();
	}

	void EventDisplay::drawCurveTo(double x, double y, bool release)
	{
		const auto fades = m_fades.find(m_dragEvent);
		const auto shown = std::find_if(
			m_events.begin(), m_events.end(), [this](const Shown& each) { return each.id == m_dragEvent; });
		if (m_drawnCurve.empty() || fades == m_fades.end() || shown == m_events.end() ||
			!(fades->second.timeRate > 0.0))
			return;
		// Inside the event: past its edges the drawing stays at the edge, where it has one point.
		const double left = (shown->start - m_view.start) / m_view.scale;
		const double right = left + (static_cast<double>(fades->second.length) / fades->second.timeRate / m_view.scale);
		x = std::clamp(x, left, std::max(left, right));
		// ponytail: a point every few dp the mouse goes sideways, and one where it is let go; thin them out if curves
		// get too dense.
		constexpr double step = 4.0;
		const double from = m_drawnCurve.back().first;
		if (x == from || (!release && std::abs(x - from) < step))
			return;
		// Where the drawing goes back over itself, it replaces what it drew there before.
		const auto passed = [from, x](const std::pair<double, double>& point)
		{ return x > from ? point.first > from && point.first <= x : point.first >= x && point.first < from; };
		const auto last = std::prev(m_drawnCurve.end());
		m_drawnCurve.erase(std::remove_if(m_drawnCurve.begin(), last, passed), last);
		m_drawnCurve.emplace_back(x, y);
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
		const auto pointAt = [this, &fades, &shown](double x, double fraction)
		{
			const double seconds = m_view.start + (x * m_view.scale);
			return engine::VolumePoint{fades.offset +
					std::clamp<int64_t>(std::llround((seconds - shown->start) * fades.timeRate), 0, fades.length),
				engine::volumeDb(fraction * g_volumeLineTop / gain(fades.volume)), 0.0};
		};
		const auto byPosition = [](const engine::VolumePoint& a, const engine::VolumePoint& b)
		{ return a.position < b.position; };
		if (!m_drawnCurve.empty())
		{
			// The drawn points replace those between the first and the last of them.
			std::vector<engine::VolumePoint> drawn;
			drawn.reserve(m_drawnCurve.size());
			for (const auto& [x, y] : m_drawnCurve)
				drawn.push_back(pointAt(x, volumeFraction(*shown, y)));
			std::stable_sort(drawn.begin(), drawn.end(), byPosition);
			curve.erase(
				std::remove_if(curve.begin(), curve.end(), [&drawn](const engine::VolumePoint& point)
					{ return point.position >= drawn.front().position && point.position <= drawn.back().position; }),
				curve.end());
			curve.insert(curve.end(), drawn.begin(), drawn.end());
			std::stable_sort(curve.begin(), curve.end(), byPosition);
			return curve;
		}
		// A new point goes where the press was and follows the mouse. The pressed point moves from where it is by the
		// drag's movement (reference manual p. 400: a click only selects it): inside the event.
		double fromX = m_pressX;
		double fraction = volumeFraction(*shown, m_pressY + m_dragDy);
		bool keepsVolume = m_dragDy == 0.0; // a sideways drag keeps the volume exactly
		if (m_dragPoint)
		{
			const engine::VolumePoint& point = curve[*m_dragPoint];
			const double height = m_laneTops[shown->lane + 1] - m_laneTops[shown->lane];
			const double at = shown->start + (static_cast<double>(point.position - fades.offset) / fades.timeRate);
			fromX = (at - m_view.start) / m_view.scale;
			// From the point's own volume, which can be above the line's top where the point is drawn at the top
			// (#683): there a drag down lowers it by the drag's distance, and a drag up leaves it as it is.
			const double from = gain(fades.volume) * gain(point.volume) / g_volumeLineTop;
			fraction = std::clamp(from - (height > 0.0 ? m_dragDy / height : 0.0), 0.0, std::max(from, 1.0));
			keepsVolume = keepsVolume || fraction == from;
		}
		const engine::VolumePoint moved = pointAt(fromX + m_dragDx, fraction);
		if (m_dragPoint)
		{
			curve[*m_dragPoint].position = moved.position;
			if (!keepsVolume)
				curve[*m_dragPoint].volume = moved.volume;
		}
		else
		{
			curve.push_back(moved);
		}
		std::stable_sort(curve.begin(), curve.end(), byPosition);
		return curve;
	}

	std::vector<size_t> EventDisplay::selectedCurvePoints(
		uint64_t event, const std::vector<engine::VolumePoint>& curve) const
	{
		std::vector<size_t> points;
		if (event != m_curveEvent || !std::binary_search(m_selection.begin(), m_selection.end(), event))
			return points;
		for (size_t i = 0; i < curve.size(); ++i)
			if (std::find(m_curvePoints.begin(), m_curvePoints.end(), curve[i]) != m_curvePoints.end())
				points.push_back(i);
		return points;
	}

	void EventDisplay::curveUp(double x, double y)
	{
		drawCurveTo(x, y, true);
		// A click on a point, or a drag that leaves it where it is, changes nothing and makes no undo step.
		if (const auto fades = m_fades.find(m_dragEvent); fades != m_fades.end())
		{
			const std::vector<engine::VolumePoint> curve = draggedCurve(m_dragEvent);
			const auto same = [](const engine::VolumePoint& a, const engine::VolumePoint& b)
			{ return a.position == b.position && a.volume == b.volume && a.curve == b.curve; };
			const bool changed =
				!std::equal(curve.begin(), curve.end(), fades->second.curve.begin(), fades->second.curve.end(), same);
			// A click on a point selects it, with Shift adds it to the selected ones or takes it out again (p. 400).
			if (m_dragPoint && *m_dragPoint < fades->second.curve.size() && !changed)
			{
				const engine::VolumePoint& clicked = fades->second.curve[*m_dragPoint];
				if (m_curveEvent != m_dragEvent || m_pressed != "shift")
					m_curvePoints.clear();
				m_curveEvent = m_dragEvent;
				if (const auto found = std::find(m_curvePoints.begin(), m_curvePoints.end(), clicked);
					found != m_curvePoints.end())
					m_curvePoints.erase(found);
				else
					m_curvePoints.push_back(clicked);
			}
			if (changed)
				edit("audioEvent.setVolumeCurve", {{"event", m_dragEvent}, {"points", engine::toJson(curve)}});
		}
		m_dragPoint.reset();
		m_drawnCurve.clear();
		m_dragDx = 0.0;
		m_dragDy = 0.0;
		m_drag = Drag::None;
		showFades(); // the points selected
	}

	bool EventDisplay::eraseCurvePoint(const Shown& shown, double x, double y)
	{
		// Only selected events show their points (showVolume), so only theirs can be erased (#670).
		if (!std::binary_search(m_selection.begin(), m_selection.end(), shown.id))
			return false;
		const std::optional<size_t> point = curvePointAt(shown, x, y);
		if (!point)
			return false;
		std::vector<engine::VolumePoint> curve = m_fades.at(shown.id).curve;
		curve.erase(curve.begin() + static_cast<std::ptrdiff_t>(*point));
		edit("audioEvent.setVolumeCurve", {{"event", shown.id}, {"points", engine::toJson(curve)}});
		return true;
	}

	std::optional<VolumeLine> EventDisplay::volumeLine(const Shown& shown) const
	{
		const AudioFades& fades = m_fades.at(shown.id);
		const bool selected = std::binary_search(m_selection.begin(), m_selection.end(), shown.id);
		const double volume = draggedVolume(shown.id);
		const std::vector<engine::VolumePoint> curve = draggedCurve(shown.id);
		// Events at 0 dB without a curve show no line, unless the volume is being dragged.
		const bool dragging = selected && m_drag == Drag::Volume && m_dragMoved;
		if (curve.empty() && volume == 0.0 && !dragging)
			return std::nullopt;
		return VolumeLine{shown.start, shown.end, fades.timeRate, fades.offset, gain(volume), engine::gainPoints(curve),
			selected, selectedCurvePoints(shown.id, curve)};
	}

	void EventDisplay::showVolume(FadeLines::Lane& lane, const Shown& shown) const
	{
		const bool selected = std::binary_search(m_selection.begin(), m_selection.end(), shown.id);
		if (const std::optional<double> fader = faderOf(shown); selected && fader)
			lane.faders.push_back(*fader);
		if (std::optional<VolumeLine> line = volumeLine(shown))
			lane.volumes.push_back(std::move(*line));
	}

	void EventDisplay::showWaveVolumes()
	{
		// The waveforms stay where the events are while a drag moves them, and so do the volumes they're drawn with.
		std::map<uint64_t, VolumeLine> volumes;
		for (const Shown& shown : m_events)
			if (const auto fades = m_fades.find(shown.id); fades != m_fades.end() && fades->second.rate > 0.0)
				if (std::optional<VolumeLine> line = volumeLine(shown))
					volumes[shown.id] = std::move(*line);
		m_waveforms->setVolumes(std::move(volumes));
	}

	void EventDisplay::addVolumeActions()
	{
		// Delete removes the selected curve points rather than events (reference manual p. 400).
		const std::function<void()> deleteEvents = m_actions.at("edit.delete");
		m_actions["edit.delete"] = [this, deleteEvents]
		{
			const auto fades = m_fades.find(m_curveEvent);
			const std::vector<size_t> points =
				fades != m_fades.end() ? selectedCurvePoints(m_curveEvent, fades->second.curve) : std::vector<size_t>();
			if (points.empty())
			{
				deleteEvents();
				return;
			}
			std::vector<engine::VolumePoint> curve;
			for (size_t i = 0; i < fades->second.curve.size(); ++i)
				if (!std::binary_search(points.begin(), points.end(), i))
					curve.push_back(fades->second.curve[i]);
			m_curvePoints.clear();
			edit("audioEvent.setVolumeCurve", {{"event", m_curveEvent}, {"points", engine::toJson(curve)}});
		};
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
