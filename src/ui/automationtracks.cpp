// SPDX-License-Identifier: GPL-3.0-or-later
// Automation tracks (#121, reference manual pp. 870-880, docs/features/automation-tracks.md): the event display's
// lanes for a track's automation, their curves and static value lines, and the tools on them. Every edit goes to the
// engine as automationLane.* and automationPoint.* commands, one undo step each.
#include "ui/eventdisplay.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Property.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <tuple>
#include <utility>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		constexpr double g_pointReach = 5.0;    // dp around a point that a press takes
		constexpr double g_editorEdge = 6.0;    // dp either side of the automation event editor's borders
		constexpr double g_editorCorner = 12.0; // dp: its top corners tilt, the middle of its top scales
		constexpr double g_editorMargin = 10.0; // dp between the editor's box and the points in it
		constexpr double g_volumeFloor = -60.0; // dB at the bottom of a volume lane, as the mixer's fader shows it
		constexpr double g_reduction = 2.0;     // dp that a drawn curve may stray from the points it keeps
		constexpr int g_sinePoints = 16;        // points per period of the line tool's sine
		constexpr double g_pi = 3.14159265358979323846;

		Rml::Property dp(double value)
		{
			return {static_cast<float>(value), Rml::Unit::DP};
		}

		bool has(const std::string& keys, const char* key)
		{
			return keys.find(key) != std::string::npos;
		}

		// Ramer-Douglas-Peucker: keeps the points of path from first to last that stray more than g_reduction from
		// the line between those kept (reference manual p. 873: the number of drawn events is reduced).
		void reduce(
			const std::vector<std::pair<double, double>>& path, size_t first, size_t last, std::vector<bool>& keep)
		{
			if (last <= first + 1)
				return;
			const auto [x0, y0] = path[first];
			const auto [x1, y1] = path[last];
			const double length = std::hypot(x1 - x0, y1 - y0);
			double farthest = 0.0;
			size_t at = first;
			for (size_t i = first + 1; i < last; ++i)
			{
				const auto [x, y] = path[i];
				const double distance = length > 0.0
					? std::abs(((x1 - x0) * (y0 - y)) - ((x0 - x) * (y1 - y0))) / length
					: std::hypot(x - x0, y - y0);
				if (distance > farthest)
				{
					farthest = distance;
					at = i;
				}
			}
			if (farthest <= g_reduction)
				return;
			keep[at] = true;
			reduce(path, first, at, keep);
			reduce(path, at, last, keep);
		}

		bool isVolume(const Json& info)
		{
			return info.at("target").at("type") == "volume";
		}

		const char* shapeName(engine::AutomationShape shape)
		{
			switch (shape)
			{
			case engine::AutomationShape::Step:
				return "step";
			case engine::AutomationShape::Curve:
				return "curve";
			case engine::AutomationShape::Linear:
				break;
			}
			return "linear";
		}

		engine::AutomationShape shapeOf(const std::string& name)
		{
			if (name == "step")
				return engine::AutomationShape::Step;
			return name == "curve" ? engine::AutomationShape::Curve : engine::AutomationShape::Linear;
		}
	}

	// ponytail: a volume lane shows -60 dB to the maximum in a straight line, as the mixer's fader does; a fader law
	// can come with the mixer's.
	double EventDisplay::displayValue(const Json& info, double value)
	{
		if (!info.is_object())
			return 0.0;
		const double minimum = info.at("minimum").get<double>();
		const double maximum = info.at("maximum").get<double>();
		const double bottom = isVolume(info) ? g_volumeFloor : minimum;
		return maximum > bottom ? std::clamp((value - bottom) / (maximum - bottom), 0.0, 1.0) : 0.0;
	}

	double EventDisplay::targetValue(const Json& info, double value)
	{
		const double minimum = info.at("minimum").get<double>();
		const double maximum = info.at("maximum").get<double>();
		value = std::clamp(value, 0.0, 1.0);
		if (isVolume(info))
			return value <= 0.0 ? minimum : g_volumeFloor + (value * (maximum - g_volumeFloor));
		const double result = minimum + (value * (maximum - minimum));
		return info.value("stepped", false) ? std::round(result) : result;
	}

	void EventDisplay::addAutomationActions()
	{
		// Delete removes the selected automation points rather than events (reference manual p. 878).
		const std::function<void()> deleteEvents = m_actions.at("edit.delete");
		m_actions["edit.delete"] = [this, deleteEvents]
		{
			if (m_selectedPoints.empty())
			{
				deleteEvents();
				return;
			}
			Json commands = Json::array();
			for (const auto& [lane, points] : m_selectedPoints)
				if (!points.empty())
					commands.push_back({{"command", "automationPoint.remove"},
						{"parameters",
							{{"lane", lane}, {"points", std::vector<uint64_t>(points.begin(), points.end())}}}});
			selectPoints({});
			m_binding.send("history.transaction",
				{{"description", "Delete Automation Points"}, {"commands", std::move(commands)}});
		};
		// Virgin territory isn't an undo step: the curves show their gaps anew when it changes.
		m_subscriptions.push_back(m_binding.on("automation.changed",
			[this](const engine::Event& event)
			{
				if (!m_project.is_object() ||
					event.data.at("virginTerritory") != m_project.value("virginTerritory", false))
					requestProject();
			}));
	}

	void EventDisplay::readAutomation(const Json& project)
	{
		std::map<uint64_t, bool> linear;
		for (const Json& track : project.at("tracks"))
			linear[track.at("id").get<uint64_t>()] = track.at("timeBase") == "linear";
		std::map<uint64_t, AutomationView> lanes;
		for (const Json& json : project.at("automation"))
		{
			AutomationView lane;
			lane.id = json.at("id").get<uint64_t>();
			lane.track = json.at("track").get<uint64_t>();
			lane.read = json.at("read").get<bool>();
			lane.linear = linear[lane.track];
			lane.target = json.at("target");
			if (const auto targets = m_targets.find(lane.track); targets != m_targets.end())
				for (const Json& info : targets->second)
					if (info.at("target") == lane.target)
						lane.info = info;
			for (const Json& point : json.at("points"))
			{
				const auto position = point.at("position").get<int64_t>();
				const double seconds =
					lane.linear ? static_cast<double>(position) / m_time.sampleRate : m_time.tempo.toSeconds(position);
				lane.ids.push_back(point.at("id").get<uint64_t>());
				lane.points.push_back({seconds, displayValue(lane.info, point.at("value").get<double>()),
					shapeOf(point.at("shape").get<std::string>()), point.value("curve", 0.0), false,
					point.value("terminator", false)});
			}
			lanes[lane.id] = std::move(lane);
		}
		m_automation = std::move(lanes);
		// Points that went leave the selection.
		for (auto& [lane, points] : m_selectedPoints)
		{
			const auto found = m_automation.find(lane);
			for (auto point = points.begin(); point != points.end();)
				point = found == m_automation.end() ||
						std::find(found->second.ids.begin(), found->second.ids.end(), *point) == found->second.ids.end()
					? points.erase(point)
					: std::next(point);
		}
		for (auto lane = m_selectedPoints.begin(); lane != m_selectedPoints.end();)
			lane = lane->second.empty() ? m_selectedPoints.erase(lane) : std::next(lane);
		// The names, ranges and static values may have changed with the project.
		for (const auto& [track, lanesShown] : m_openAutomation)
			requestTargets(track);
	}

	void EventDisplay::requestTargets(uint64_t track, std::function<void()> then)
	{
		m_binding.send("automationLane.targets", {{"track", track}},
			[this, track, then = std::move(then), alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok())
					return;
				const Json& targets = result.value.at("targets");
				const bool changed = m_targets[track] != targets;
				m_targets[track] = targets;
				if (changed && m_project.is_object())
					reload();
				if (then)
					then();
			});
	}

	const Json* EventDisplay::automationTargets(uint64_t track) const
	{
		const auto found = m_targets.find(track);
		return found == m_targets.end() ? nullptr : &found->second;
	}

	std::vector<uint64_t> EventDisplay::openLanes(uint64_t track) const
	{
		std::vector<uint64_t> lanes;
		const auto open = m_openAutomation.find(track);
		if (open == m_openAutomation.end())
			return lanes;
		for (const uint64_t lane : open->second)
			if (const auto found = m_automation.find(lane); found != m_automation.end() && found->second.track == track)
				lanes.push_back(lane);
		return lanes;
	}

	void EventDisplay::reload()
	{
		if (m_project.is_object())
			loadProject(m_project);
	}

	void EventDisplay::showAutomation(uint64_t track, bool shown)
	{
		if (!shown)
		{
			m_openAutomation.erase(track);
			reload();
			return;
		}
		std::vector<uint64_t> used;
		std::vector<uint64_t> all;
		for (const auto& [id, lane] : m_automation)
			if (lane.track == track)
			{
				all.push_back(id);
				if (!lane.points.empty())
					used.push_back(id);
			}
		if (used.empty() && !all.empty())
			used.push_back(all.front());
		m_openAutomation[track] = used;
		if (all.empty()) // a lane for its first parameter (reference manual p. 880)
			requestTargets(track,
				[this, track]
				{
					const Json* targets = automationTargets(track);
					if (automationShown(track) && openLanes(track).empty() && targets != nullptr && !targets->empty())
						addLane(track, targets->at(0).at("target"));
				});
		else
			requestTargets(track);
		reload();
	}

	void EventDisplay::showUsedAutomation()
	{
		m_openAutomation.clear();
		for (const auto& [id, lane] : m_automation)
			if (!lane.points.empty())
				m_openAutomation[lane.track].push_back(id);
		for (const auto& [track, lanes] : m_openAutomation)
			requestTargets(track);
		reload();
	}

	void EventDisplay::hideAllAutomation()
	{
		m_openAutomation.clear();
		reload();
	}

	void EventDisplay::addLane(uint64_t track, const Json& target, Json before, std::optional<size_t> at)
	{
		before.push_back({{"command", "automationLane.add"},
			{"parameters", {{"track", track}, {"target", target}, {"read", false}}}});
		const size_t index = before.size() - 1;
		m_binding.send("history.transaction",
			{{"description", "Add Automation Track"}, {"commands", std::move(before)}},
			[this, track, index, at, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok() || !automationShown(track))
					return;
				const auto lane = result.value.at("results").at(index).at("lane").at("id").get<uint64_t>();
				std::vector<uint64_t>& open = m_openAutomation[track];
				open.insert(
					at && *at <= open.size() ? open.begin() + static_cast<std::ptrdiff_t>(*at) : open.end(), lane);
				reload();
			});
	}

	void EventDisplay::appendAutomation(uint64_t track)
	{
		const Json* targets = automationTargets(track);
		if (targets == nullptr)
		{
			requestTargets(track, [this, track] { appendAutomation(track); });
			return;
		}
		const std::vector<uint64_t> shown = openLanes(track);
		for (const Json& target : *targets)
		{
			auto lane = target.at("lane").get<uint64_t>();
			// Targets read before a lane was added still say it has none: the project's lanes know better.
			for (const auto& [id, known] : m_automation)
				if (lane == 0 && known.track == track && known.target == target.at("target"))
					lane = id;
			if (lane != 0 && std::find(shown.begin(), shown.end(), lane) != shown.end())
				continue;
			if (lane != 0 && m_automation.count(lane) != 0)
			{
				m_openAutomation[track].push_back(lane);
				reload();
			}
			else
			{
				m_openAutomation.try_emplace(track); // shows while the lane is added
				addLane(track, target.at("target"));
			}
			return;
		}
	}

	void EventDisplay::showOnlyPlugin(uint64_t lane)
	{
		const auto found = m_automation.find(lane);
		if (found == m_automation.end() || found->second.target.at("type") != "plugin")
			return;
		std::vector<uint64_t> lanes;
		for (const auto& [id, other] : m_automation)
			if (other.track == found->second.track && other.target.at("type") == "plugin" &&
				other.target.at("slot") == found->second.target.at("slot"))
				lanes.push_back(id);
		m_openAutomation[found->second.track] = std::move(lanes);
		reload();
	}

	void EventDisplay::setAutomationTarget(uint64_t lane, const Json& target)
	{
		const auto found = m_automation.find(lane);
		if (found == m_automation.end() || found->second.target == target)
			return;
		const uint64_t track = found->second.track;
		std::vector<uint64_t>& open = m_openAutomation[track];
		const auto at = std::find(open.begin(), open.end(), lane);
		const auto index = static_cast<size_t>(at - open.begin());
		const bool unused = found->second.points.empty();
		Json removeOld = Json::array();
		if (unused)
			removeOld.push_back({{"command", "automationLane.remove"}, {"parameters", {{"lane", lane}}}});
		for (const auto& [id, other] : m_automation)
			if (other.track == track && other.target == target)
			{
				// The track has a lane for it: the automation track shows that one, wherever another showed it.
				open.erase(std::remove(open.begin(), open.end(), id), open.end());
				std::replace(open.begin(), open.end(), lane, id);
				if (std::find(open.begin(), open.end(), id) == open.end())
					open.push_back(id);
				if (unused)
					m_binding.send("automationLane.remove", {{"lane", lane}});
				reload();
				return;
			}
		if (at != open.end())
			open.erase(at);
		addLane(track, target, std::move(removeOld), index);
	}

	void EventDisplay::removeUnusedAutomation(uint64_t track)
	{
		Json commands = Json::array();
		for (const auto& [id, lane] : m_automation)
			if (lane.track == track && lane.points.empty())
				commands.push_back({{"command", "automationLane.remove"}, {"parameters", {{"lane", id}}}});
		if (!commands.empty())
			m_binding.send("history.transaction",
				{{"description", "Remove Unused Parameters"}, {"commands", std::move(commands)}});
	}

	void EventDisplay::selectPoints(std::map<uint64_t, std::set<uint64_t>> points)
	{
		for (auto lane = points.begin(); lane != points.end();)
			lane = lane->second.empty() ? points.erase(lane) : std::next(lane);
		m_selectedPoints = std::move(points);
		if (!m_selectedPoints.empty() && !m_selection.empty())
			select({});
		showAutomationLanes();
	}

	const EventDisplay::AutomationView* EventDisplay::automationAt(size_t lane) const
	{
		if (lane >= m_laneAutomation.size() || m_laneAutomation[lane] == 0)
			return nullptr;
		const auto found = m_automation.find(m_laneAutomation[lane]);
		return found == m_automation.end() ? nullptr : &found->second;
	}

	void EventDisplay::showAutomationLanes()
	{
		if (!alive())
			return;
		std::vector<size_t> shown;
		for (size_t i = 0; i < m_laneAutomation.size(); ++i)
			if (automationAt(i) != nullptr)
				shown.push_back(i);
		while (static_cast<size_t>(m_automationArea->GetNumChildren()) > shown.size())
			m_automationArea->RemoveChild(m_automationArea->GetLastChild());
		while (static_cast<size_t>(m_automationArea->GetNumChildren()) < shown.size())
		{
			Rml::ElementPtr lane = m_document->GetOwnerDocument()->CreateElement("div");
			lane->SetClass("automation-lane", true);
			lane->AppendChild(m_document->GetOwnerDocument()->CreateElement("curve"))->SetClass("static-value", true);
			lane->AppendChild(m_document->GetOwnerDocument()->CreateElement("curve"))
				->SetClass("automation-curve", true);
			m_automationArea->AppendChild(std::move(lane));
		}
		for (size_t k = 0; k < shown.size(); ++k)
		{
			const AutomationView& lane = *automationAt(shown[k]);
			Rml::Element* element = m_automationArea->GetChild(static_cast<int>(k));
			element->SetAttribute("lane", std::to_string(lane.id));
			element->SetClass("off", !lane.read); // the curve is grayed out (reference manual p. 870)
			// The static value line: while the lane has no points or doesn't read.
			if (auto* line = dynamic_cast<Curve*>(element->GetChild(0)))
			{
				std::vector<CurvePoint> level;
				if (lane.info.is_object() && (lane.points.empty() || !lane.read))
					level.push_back({0.0, displayValue(lane.info, lane.info.at("value").get<double>())});
				line->setPoints(std::move(level), false, true);
			}
			if (auto* curve = dynamic_cast<Curve*>(element->GetChild(1)))
			{
				std::vector<CurvePoint> points = lane.points;
				const auto selected = m_selectedPoints.find(lane.id);
				for (size_t i = 0; i < points.size(); ++i)
					points[i].selected = selected != m_selectedPoints.end() && selected->second.count(lane.ids[i]) != 0;
				curve->setPoints(std::move(points), true);
				curve->setVirginTerritory(m_project.is_object() && m_project.value("virginTerritory", false));
			}
		}
		showPointSelection();
		placeAutomation();
	}

	void EventDisplay::preview(size_t lane, std::vector<CurvePoint> points)
	{
		const AutomationView* view = automationAt(lane);
		if (view == nullptr || !alive())
			return;
		std::sort(points.begin(), points.end(),
			[](const CurvePoint& a, const CurvePoint& b) { return a.position < b.position; });
		for (int i = 0; i < m_automationArea->GetNumChildren(); ++i)
			if (const Rml::Element* element = m_automationArea->GetChild(i);
				element->GetAttribute<Rml::String>("lane", "") == std::to_string(view->id))
				if (auto* curve = dynamic_cast<Curve*>(element->GetChild(1)))
				{
					curve->setPoints(std::move(points), true);
					return;
				}
	}

	void EventDisplay::placeAutomation()
	{
		if (!alive())
			return;
		size_t k = 0;
		for (size_t i = 0; i < m_laneAutomation.size() && i + 1 < m_laneTops.size(); ++i)
		{
			if (automationAt(i) == nullptr || static_cast<int>(k) >= m_automationArea->GetNumChildren())
				continue;
			Rml::Element* element = m_automationArea->GetChild(static_cast<int>(k++));
			element->SetProperty(Rml::PropertyId::Top, dp(m_laneTops[i] - m_view.top));
			element->SetProperty(Rml::PropertyId::Height, dp(m_laneTops[i + 1] - m_laneTops[i]));
			for (int c = 0; c < element->GetNumChildren(); ++c)
			{
				element->GetChild(c)->SetAttribute("start", m_view.start);
				element->GetChild(c)->SetAttribute("scale", m_view.scale);
			}
		}
		const std::optional<EditorBox> box = editorBox();
		m_editor->SetClass("shown", box.has_value());
		if (box)
		{
			m_editor->SetProperty(Rml::PropertyId::Left, dp(box->left));
			m_editor->SetProperty(Rml::PropertyId::Top, dp(box->top));
			m_editor->SetProperty(Rml::PropertyId::Width, dp(box->right - box->left));
			m_editor->SetProperty(Rml::PropertyId::Height, dp(box->bottom - box->top));
		}
	}

	void EventDisplay::showPointSelection()
	{
		if (!alive())
			return;
		std::string text;
		for (const auto& [lane, points] : m_selectedPoints)
			for (const uint64_t point : points)
				text += (text.empty() ? "" : " ") + std::to_string(lane) + ":" + std::to_string(point);
		if (m_root->GetAttribute<Rml::String>("automation-selection", "") != text)
			m_root->SetAttribute("automation-selection", text);
	}

	double EventDisplay::automationValue(size_t lane, double y) const
	{
		const double top = m_laneTops[lane] - m_view.top;
		const double height = m_laneTops[lane + 1] - m_laneTops[lane];
		return height > 0.0 ? std::clamp(1.0 - ((y - top) / height), 0.0, 1.0) : 0.0;
	}

	double EventDisplay::automationY(size_t lane, double value) const
	{
		return m_laneTops[lane] - m_view.top + ((1.0 - value) * (m_laneTops[lane + 1] - m_laneTops[lane]));
	}

	std::optional<size_t> EventDisplay::pointAt(const AutomationView& lane, size_t index, double x, double y) const
	{
		std::optional<size_t> found;
		double nearest = g_pointReach * 2.0;
		for (size_t i = 0; i < lane.points.size(); ++i)
		{
			const double dx = std::abs(((lane.points[i].position - m_view.start) / m_view.scale) - x);
			const double dy = std::abs(automationY(index, lane.points[i].value) - y);
			if (dx <= g_pointReach && dy <= g_pointReach && dx + dy < nearest)
			{
				nearest = dx + dy;
				found = i;
			}
		}
		return found;
	}

	int64_t EventDisplay::pointPosition(const AutomationView& lane, double seconds) const
	{
		seconds = std::max(seconds, 0.0);
		return lane.linear ? std::llround(seconds * m_time.sampleRate) : m_time.tempo.toTicks(seconds);
	}

	std::optional<EventDisplay::EditorBox> EventDisplay::editorBox() const
	{
		if (m_selectedPoints.size() != 1 || m_selectedPoints.begin()->second.size() < 2)
			return std::nullopt;
		const auto lane = std::find(m_laneAutomation.begin(), m_laneAutomation.end(), m_selectedPoints.begin()->first);
		if (lane == m_laneAutomation.end())
			return std::nullopt;
		const auto index = static_cast<size_t>(lane - m_laneAutomation.begin());
		const AutomationView* view = automationAt(index);
		// Only for ramp curves (reference manual p. 876).
		if (view == nullptr || !view->info.is_object() || view->info.value("stepped", false))
			return std::nullopt;
		double first = 0.0;
		double last = 0.0;
		double low = 1.0;
		double high = 0.0;
		bool any = false;
		for (size_t i = 0; i < view->points.size(); ++i)
			if (m_selectedPoints.begin()->second.count(view->ids[i]) != 0)
			{
				const CurvePoint& point = view->points[i];
				first = any ? std::min(first, point.position) : point.position;
				last = any ? std::max(last, point.position) : point.position;
				low = std::min(low, point.value);
				high = std::max(high, point.value);
				any = true;
			}
		const double laneTop = m_laneTops[index] - m_view.top;
		const double laneBottom = m_laneTops[index + 1] - m_view.top;
		return EditorBox{index, (first - m_view.start) / m_view.scale,
			std::max(laneTop, automationY(index, high) - g_editorMargin), (last - m_view.start) / m_view.scale,
			std::min(laneBottom, automationY(index, low) + g_editorMargin)};
	}

	std::optional<EventDisplay::EditorControl> EventDisplay::editorControlAt(double x, double y) const
	{
		const std::optional<EditorBox> box = editorBox();
		if (!box || x < box->left - g_editorEdge || x > box->right + g_editorEdge)
			return std::nullopt;
		if (std::abs(y - box->top) <= g_editorEdge)
		{
			// The smart controls along the top (reference manual p. 876).
			if (std::abs(x - ((box->left + box->right) / 2.0)) <= g_editorCorner)
				return EditorControl::ScaleVertically;
			if (x - box->left <= g_editorCorner)
				return EditorControl::TiltLeft;
			if (box->right - x <= g_editorCorner)
				return EditorControl::TiltRight;
			return EditorControl::MoveVertically;
		}
		if (std::abs(y - box->bottom) <= g_editorEdge)
			return EditorControl::Stretch;
		return std::nullopt;
	}

	std::vector<std::pair<uint64_t, CurvePoint>> EventDisplay::transformed(double x, double y) const
	{
		std::vector<std::pair<uint64_t, CurvePoint>> points;
		const AutomationView* lane = automationAt(m_gestureLane);
		if (lane == nullptr)
			return points;
		const auto selected = m_selectedPoints.find(lane->id);
		if (selected == m_selectedPoints.end())
			return points;
		const double height = m_laneTops[m_gestureLane + 1] - m_laneTops[m_gestureLane];
		auto [dx, dy] = std::make_pair(x - m_pressX, y - m_pressY);
		if (m_gesture == AutomationGesture::Move)
			std::tie(dx, dy) = restricted(dx, dy, has(m_pressed, "ctrl"));
		const double dv = height > 0.0 ? -dy / height : 0.0;
		double first = 0.0;
		double last = 0.0;
		double low = 1.0;
		double high = 0.0;
		for (size_t i = 0; i < lane->points.size(); ++i)
			if (selected->second.count(lane->ids[i]) != 0)
			{
				const CurvePoint& point = lane->points[i];
				first = points.empty() ? point.position : std::min(first, point.position);
				last = points.empty() ? point.position : std::max(last, point.position);
				low = std::min(low, point.value);
				high = std::max(high, point.value);
				points.emplace_back(lane->ids[i], point);
			}
		// A move snaps the pressed point's time and keeps the others' distance to it (reference manual p. 877).
		double dt = dx * m_view.scale;
		if (m_gesture == AutomationGesture::Move)
		{
			const auto pressed = std::find(lane->ids.begin(), lane->ids.end(), m_dragEvent);
			if (pressed != lane->ids.end() && dx != 0.0)
			{
				const double from = lane->points[static_cast<size_t>(pressed - lane->ids.begin())].position;
				const time::Position to = snapAt(from + dt, from);
				dt = (static_cast<double>(time::toSamples(to, m_time.tempo, m_time.sampleRate)) / m_time.sampleRate) -
					from;
			}
		}
		const double span = last - first;
		for (auto& [id, point] : points)
		{
			if (m_gesture == AutomationGesture::Move)
			{
				point.position = std::max(0.0, point.position + dt);
				point.value += dv;
			}
			else if (m_control == EditorControl::MoveVertically ||
				(m_control == EditorControl::ScaleVertically && high <= low))
			{
				point.value += dv;
			}
			else if (m_control == EditorControl::ScaleVertically)
			{
				point.value = low + ((point.value - low) * (high + dv - low) / (high - low));
			}
			else if (m_control == EditorControl::TiltLeft)
			{
				point.value += span > 0.0 ? dv * (last - point.position) / span : dv;
			}
			else if (m_control == EditorControl::TiltRight)
			{
				point.value += span > 0.0 ? dv * (point.position - first) / span : dv;
			}
			else if (span > 0.0) // stretch
			{
				point.position = first + ((point.position - first) * std::max(0.0, span + dt) / span);
			}
			point.value = std::clamp(point.value, 0.0, 1.0);
		}
		return points;
	}

	std::vector<CurvePoint> EventDisplay::replaced(
		const AutomationView& lane, double start, double end, const std::vector<CurvePoint>& points)
	{
		std::vector<CurvePoint> result;
		for (const CurvePoint& point : lane.points)
			if (point.position < start || point.position > end)
				result.push_back(point);
		result.insert(result.end(), points.begin(), points.end());
		std::stable_sort(result.begin(), result.end(),
			[](const CurvePoint& a, const CurvePoint& b) { return a.position < b.position; });
		return result;
	}

	std::vector<CurvePoint> EventDisplay::linePoints(double x, double y) const
	{
		using engine::AutomationShape;
		const double t0 = m_view.start + (std::min(m_pressX, x) * m_view.scale);
		const double t1 = m_view.start + (std::max(m_pressX, x) * m_view.scale);
		// From left to right: the value where the drag started on its side.
		const bool rightwards = x >= m_pressX;
		const double v0 = automationValue(m_gestureLane, rightwards ? m_pressY : y);
		const double v1 = automationValue(m_gestureLane, rightwards ? y : m_pressY);
		std::vector<CurvePoint> points;
		if (!(t1 > t0))
			return points;
		switch (m_lineMode)
		{
		case LineMode::Line:
			return {{t0, v0, AutomationShape::Linear, 0.0}, {t1, v1, AutomationShape::Linear, 0.0}};
		case LineMode::Parabola:
			// Fast at first when drawn rightwards, slow at first when drawn leftwards (reference manual p. 873).
			return {{t0, v0, AutomationShape::Curve, rightwards ? 0.5 : -0.5}, {t1, v1, AutomationShape::Linear, 0.0}};
		case LineMode::Sine:
		case LineMode::Triangle:
		case LineMode::Square:
			break;
		}
		// Periods of the grid (reference manual p. 874), as zoom adapts it, between the start's value and the end's.
		const time::Ticks start = m_time.tempo.toTicks(t0);
		const double period = std::max(m_time.tempo.toSeconds(start + snapGrid().zoomStep) - t0, m_view.scale);
		const int steps = m_lineMode == LineMode::Sine ? g_sinePoints : 2;
		const double step = period / steps;
		for (int64_t i = 0; t0 + (static_cast<double>(i) * step) < t1; ++i)
		{
			const double at = t0 + (static_cast<double>(i) * step);
			const double phase = static_cast<double>(i % steps) / steps; // in its period
			double value = phase < 0.5 ? v0 : v1;
			AutomationShape shape = m_lineMode == LineMode::Square ? AutomationShape::Step : AutomationShape::Linear;
			if (m_lineMode == LineMode::Sine)
				value = v0 + ((v1 - v0) * (1.0 - std::cos(2.0 * g_pi * phase)) / 2.0);
			points.push_back({at, value, shape, 0.0});
		}
		points.push_back({t1, m_lineMode == LineMode::Square ? points.back().value : v1, AutomationShape::Linear, 0.0});
		return points;
	}

	void EventDisplay::automationEdit(const std::string& description, const AutomationView& lane, Json commands)
	{
		// A click with a tool turns reading on (reference manual p. 873).
		if (!lane.read)
			commands.insert(commands.begin(),
				Json{{"command", "automationLane.set"}, {"parameters", {{"lane", lane.id}, {"read", true}}}});
		if (!commands.empty())
			m_binding.send("history.transaction", {{"description", description}, {"commands", std::move(commands)}});
	}

	void EventDisplay::replacePoints(
		const AutomationView& lane, double start, double end, const std::vector<CurvePoint>& points)
	{
		Json commands = Json::array();
		std::vector<uint64_t> removed;
		for (size_t i = 0; i < lane.points.size(); ++i)
			if (lane.points[i].position >= start && lane.points[i].position <= end)
				removed.push_back(lane.ids[i]);
		if (!removed.empty())
			commands.push_back(
				{{"command", "automationPoint.remove"}, {"parameters", {{"lane", lane.id}, {"points", removed}}}});
		Json added = Json::array();
		for (const CurvePoint& point : points)
		{
			Json json = {{"position", pointPosition(lane, point.position)},
				{"value", targetValue(lane.info, point.value)}, {"shape", shapeName(point.shape)}};
			if (point.shape == engine::AutomationShape::Curve)
				json["curve"] = point.curve;
			added.push_back(std::move(json));
		}
		if (!added.empty())
			commands.push_back({{"command", "automationPoint.add"},
				{"parameters", {{"lane", lane.id}, {"points", std::move(added)}}}});
		automationEdit("Draw Automation", lane, std::move(commands));
	}

	bool EventDisplay::automationDown(double x, double y)
	{
		const size_t index = laneAt(y + m_view.top);
		const AutomationView* lane = y + m_view.top < 0.0 ? nullptr : automationAt(index);
		if (lane == nullptr)
		{
			if (!m_selectedPoints.empty())
				selectPoints({});
			return false;
		}
		m_drag = Drag::Automation;
		m_gestureLane = index;
		m_gesture = AutomationGesture::None;
		m_drawn.clear();
		if (!m_selection.empty())
			select({});
		if (!lane->info.is_object())
			return true; // its range isn't known yet
		const bool add = has(m_pressed, "ctrl") || has(m_pressed, "shift");
		switch (m_tool)
		{
		case Tool::ObjectSelection:
			if (const std::optional<size_t> point = pointAt(*lane, index, x, y))
			{
				// A press selects the point (with Ctrl or Shift adds it, or released without moving takes it out) and
				// a drag moves the selection, as with events.
				const uint64_t id = lane->ids[*point];
				std::map<uint64_t, std::set<uint64_t>> points = m_selectedPoints;
				const bool selected = points[lane->id].count(id) != 0;
				m_dragToggles = add && selected;
				if (add && !selected)
					points[lane->id].insert(id);
				else if (!selected)
					points = {{lane->id, {id}}};
				m_dragEvent = id;
				m_gesture = AutomationGesture::Move;
				selectPoints(std::move(points));
				return true;
			}
			if (const std::optional<EditorControl> control = editorControlAt(x, y))
			{
				m_control = *control;
				m_gesture = AutomationGesture::Editor;
				return true;
			}
			m_pointsBase = add ? m_selectedPoints : std::map<uint64_t, std::set<uint64_t>>();
			m_gesture = AutomationGesture::Add;
			return true;
		case Tool::Draw:
			m_gesture = AutomationGesture::Draw;
			m_drawn.push_back({m_view.start + (x * m_view.scale), automationValue(index, y)});
			return true;
		case Tool::Line:
			// Only on ramp curves (reference manual p. 874).
			if (!lane->info.value("stepped", false))
				m_gesture = AutomationGesture::Line;
			return true;
		case Tool::Erase:
			m_gesture = AutomationGesture::Erase;
			return true;
		default:
			return true;
		}
	}

	void EventDisplay::automationMove(double x, double y)
	{
		const AutomationView* lane = automationAt(m_gestureLane);
		if (lane == nullptr)
			return;
		switch (m_gesture)
		{
		case AutomationGesture::Add:
		{
			// A rectangle selects the points in it, on the pressed lane.
			const double left = std::min(m_pressX, x);
			const double right = std::max(m_pressX, x);
			const double top = std::min(m_pressY, y);
			const double bottom = std::max(m_pressY, y);
			showRectangle(true, left, top, right, bottom);
			std::map<uint64_t, std::set<uint64_t>> points = m_pointsBase;
			for (size_t i = 0; i < lane->points.size(); ++i)
			{
				const double px = (lane->points[i].position - m_view.start) / m_view.scale;
				const double py = automationY(m_gestureLane, lane->points[i].value);
				if (px >= left && px <= right && py >= top && py <= bottom)
					points[lane->id].insert(lane->ids[i]);
			}
			selectPoints(std::move(points));
			break;
		}
		case AutomationGesture::Move:
		case AutomationGesture::Editor:
		{
			std::vector<CurvePoint> points = lane->points;
			for (const auto& [id, point] : transformed(x, y))
				points[static_cast<size_t>(std::find(lane->ids.begin(), lane->ids.end(), id) - lane->ids.begin())] =
					point;
			preview(m_gestureLane, std::move(points));
			break;
		}
		case AutomationGesture::Draw:
		{
			m_drawn.push_back({m_view.start + (x * m_view.scale), automationValue(m_gestureLane, y)});
			const auto [first, last] = std::minmax_element(m_drawn.begin(), m_drawn.end(),
				[](const CurvePoint& a, const CurvePoint& b) { return a.position < b.position; });
			preview(m_gestureLane, replaced(*lane, first->position, last->position, m_drawn));
			break;
		}
		case AutomationGesture::Line:
		{
			const std::vector<CurvePoint> points = linePoints(x, y);
			if (!points.empty())
				preview(m_gestureLane, replaced(*lane, points.front().position, points.back().position, points));
			break;
		}
		case AutomationGesture::Erase:
		case AutomationGesture::None:
			break;
		}
	}

	void EventDisplay::automationUp(double x, double y)
	{
		showRectangle(false);
		const AutomationGesture gesture = m_gesture; // transformed() reads it
		const AutomationView* lane = automationAt(m_gestureLane);
		if (lane == nullptr || !lane->info.is_object())
		{
			m_gesture = AutomationGesture::None;
			return;
		}
		const auto clicked = [&]
		{
			// A point where the click was, snapped (reference manual pp. 872-873).
			const time::Position at = snapAt(m_view.start + (m_pressX * m_view.scale), std::nullopt);
			const double seconds =
				static_cast<double>(time::toSamples(at, m_time.tempo, m_time.sampleRate)) / m_time.sampleRate;
			const CurvePoint point{seconds, automationValue(m_gestureLane, m_pressY)};
			replacePoints(*lane, seconds, seconds, {point});
		};
		switch (gesture)
		{
		case AutomationGesture::Add:
			if (!m_dragMoved)
				clicked();
			break;
		case AutomationGesture::Draw:
		{
			if (!m_dragMoved || m_drawn.size() < 2)
			{
				clicked();
				break;
			}
			std::stable_sort(m_drawn.begin(), m_drawn.end(),
				[](const CurvePoint& a, const CurvePoint& b) { return a.position < b.position; });
			const double height = m_laneTops[m_gestureLane + 1] - m_laneTops[m_gestureLane];
			std::vector<std::pair<double, double>> path;
			path.reserve(m_drawn.size());
			for (const CurvePoint& point : m_drawn)
				path.emplace_back(point.position / m_view.scale, point.value * height);
			std::vector<bool> keep(path.size(), false);
			keep.front() = true;
			keep.back() = true;
			reduce(path, 0, path.size() - 1, keep);
			std::vector<CurvePoint> points;
			for (size_t i = 0; i < m_drawn.size(); ++i)
				if (keep[i])
					points.push_back(m_drawn[i]);
			replacePoints(*lane, points.front().position, points.back().position, points);
			break;
		}
		case AutomationGesture::Line:
		{
			const std::vector<CurvePoint> points = linePoints(x, y);
			if (m_dragMoved && !points.empty())
				replacePoints(*lane, points.front().position, points.back().position, points);
			break;
		}
		case AutomationGesture::Move:
		case AutomationGesture::Editor:
		{
			if (!m_dragMoved && std::exchange(m_dragToggles, false))
			{
				std::map<uint64_t, std::set<uint64_t>> points = m_selectedPoints;
				points[lane->id].erase(m_dragEvent);
				selectPoints(std::move(points));
			}
			if (!m_dragMoved)
				break;
			Json changes = Json::array();
			for (const auto& [id, point] : transformed(x, y))
				changes.push_back({{"id", id}, {"position", pointPosition(*lane, point.position)},
					{"value", targetValue(lane->info, point.value)}});
			automationEdit(gesture == AutomationGesture::Move ? "Move Automation Points" : "Edit Automation Points",
				*lane,
				{{{"command", "automationPoint.set"}, {"parameters", {{"lane", lane->id}, {"points", changes}}}}});
			break;
		}
		case AutomationGesture::Erase:
			if (const std::optional<size_t> point = pointAt(*lane, m_gestureLane, x, y))
				m_binding.send("automationPoint.remove", {{"lane", lane->id}, {"points", {lane->ids[*point]}}});
			break;
		case AutomationGesture::None:
			break;
		}
		m_gesture = AutomationGesture::None;
		m_drawn.clear();
	}
}
