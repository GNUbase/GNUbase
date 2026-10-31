// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/eventdisplay.h"

#include "platform/log.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Property.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <stdexcept>
#include <utility>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		// Zoom limits, in seconds per dp: about half a sample at 48 kHz, and an hour in a minute of dp.
		constexpr double g_minScale = 1e-5;
		constexpr double g_maxScale = 60.0;
		constexpr double g_minTrackZoom = 0.25;
		constexpr double g_maxTrackZoom = 8.0;
		constexpr double g_zoomStep = 2.0;          // zoom in and out
		constexpr double g_verticalZoomStep = 1.25; // track heights
		constexpr double g_wheelZoomStep = 1.25;    // per line of the mouse wheel
		constexpr double g_wheelScroll = 0.1;       // of the view's width per line, scrolling across
		constexpr double g_dragZoomDistance = 40.0; // dp dragged in the ruler to zoom by a factor of 2
		constexpr double g_overviewEdge = 4.0;      // dp either side of the overview rectangle's edges

		// What the display listens to in its root element and in its document.
		constexpr std::array<Rml::EventId, 4> g_rootEvents = {
			Rml::EventId::Mousedown, Rml::EventId::Mousemove, Rml::EventId::Mousescroll, Rml::EventId::Dblclick};
		constexpr std::array<Rml::EventId, 5> g_documentEvents = {Rml::EventId::Mousemove, Rml::EventId::Mouseup,
			Rml::EventId::Resize, Rml::EventId::Click, Rml::EventId::Change};

		Rml::Element& add(Rml::Element& parent, const char* tag, const char* classes)
		{
			Rml::ElementPtr element = parent.GetOwnerDocument()->CreateElement(tag);
			element->SetClassNames(classes);
			return *parent.AppendChild(std::move(element));
		}

		template <typename T> T& addCanvas(Rml::Element& parent, const char* tag, const char* classes)
		{
			auto* canvas = dynamic_cast<T*>(&add(parent, tag, classes));
			if (canvas == nullptr)
				throw std::logic_error(std::string("<") + tag + "> isn't registered (ui::registerCanvas)");
			return *canvas;
		}

		bool inside(const Rml::Element* element, const Rml::Element* ancestor)
		{
			for (; element != nullptr; element = element->GetParentNode())
				if (element == ancestor)
					return true;
			return false;
		}

		Rml::Property dp(double value)
		{
			return {static_cast<float>(value), Rml::Unit::DP};
		}
	}

	EventDisplay::EventDisplay(engine::Engine& engine, EngineBinding& binding, Rml::Element& root)
		: m_engine(engine), m_binding(binding), m_root(root.GetObserverPtr()), m_document(root.GetOwnerDocument())
	{
		m_overview = &add(root, "div", "overview");
		m_overviewEvents = &addCanvas<EventLanes>(*m_overview, "event-lanes", "overview-events");
		m_overviewView = &add(*m_overview, "div", "overview-view");
		Rml::Element& timeline = add(root, "div", "timeline");
		m_ruler = &addCanvas<Ruler>(timeline, "ruler", "timeline-ruler");
		m_area = &add(timeline, "div", "event-area");
		m_lanes = &addCanvas<EventLanes>(*m_area, "event-lanes", "events");
		m_mutedLanes = &addCanvas<EventLanes>(*m_area, "event-lanes", "muted-events");
		m_selectedLanes = &addCanvas<EventLanes>(*m_area, "event-lanes", "selected-events");
		m_waveforms = std::make_unique<EventWaveforms>(binding, add(*m_area, "div", "waveforms"));
		m_crossVertical = &add(*m_area, "div", "cross-hair-vertical");
		m_crossHorizontal = &add(*m_area, "div", "cross-hair-horizontal");
		m_cursorLine = &add(timeline, "div", "project-cursor");
		root.SetClass("cross-hair", m_crossHair);
		for (const Rml::EventId id : g_rootEvents)
			root.AddEventListener(id, this);
		for (const Rml::EventId id : g_documentEvents)
			m_document->AddEventListener(id, this);
		findControls();
		setRulerFormat(m_format);

		m_actions = {{"zoom.in", [this] { zoomIn(); }}, {"zoom.out", [this] { zoomOut(); }},
			{"zoom.inVertically", [this] { zoomInVertically(); }},
			{"zoom.outVertically", [this] { zoomOutVertically(); }}, {"zoom.full", [this] { zoomFull(); }},
			{"zoom.undo", [this] { undoZoom(); }}, {"zoom.redo", [this] { redoZoom(); }},
			{"view.autoScroll", [this] { setAutoScroll(!m_autoScroll); }}};
		addEditActions();

		// Every change to the project is an undo step or a new project; the display shows the project as it is then.
		for (const char* event : {"history.changed", "project.reset"})
			m_subscriptions.push_back(m_binding.on(event,
				[this](const engine::Event&)
				{
					requestProject();
					requestMarkers();
				}));
		m_subscriptions.push_back(
			m_binding.on("transport.changed", [this](const engine::Event& event) { followTransport(event.data); }));
		m_subscriptions.push_back(m_binding.on("snap.changed",
			[this](const engine::Event& event)
			{
				m_snap = engine::snapSettingsFrom(event.data, m_snap);
				showSnap();
			}));
		m_subscriptions.push_back(m_binding.on("quantize.changed", [this](const engine::Event& event)
			{ m_quantize = engine::midi::quantizeSettingsFromJson(event.data.at("settings")); }));
		m_subscriptions.push_back(m_binding.on("settings.changed",
			[this](const engine::Event& event)
			{
				const Json& values = event.data.at("values");
				if (const auto presets = values.find("ui.zoomPresets"); presets != values.end())
				{
					m_presets = *presets;
					m_presetsKnown = true;
					showPresets();
				}
			}));
		const std::weak_ptr<bool> alive = m_alive;
		requestProject();
		m_binding.send("transport.get", Json::object(),
			[this, alive](const engine::Result& result)
			{
				if (!alive.expired() && result.ok())
					followTransport(result.value);
			});
		m_binding.send("settings.get", {{"key", "ui.zoomPresets"}},
			[this, alive](const engine::Result& result)
			{
				if (!alive.expired() && result.ok() && !m_presetsKnown) // newer changes win
				{
					m_presets = result.value.at("value");
					showPresets();
				}
			});
		requestMarkers();
		showAutoScroll();

		root.GetOwnerDocument()->UpdateDocument(); // lays it out, so that it knows its size
		apply();
	}

	EventDisplay::~EventDisplay()
	{
		if (m_dialog)
		{
			m_dialog->RemoveEventListener(Rml::EventId::Click, this);
			m_dialog->GetOwnerDocument()->Close();
		}
		if (!alive())
			return;
		for (const Rml::EventId id : g_rootEvents)
			m_root->RemoveEventListener(id, this);
		for (const Rml::EventId id : g_documentEvents)
			m_document->RemoveEventListener(id, this);
	}

	float EventDisplay::dpRatio() const
	{
		const Rml::Context* context = alive() ? m_root->GetContext() : nullptr;
		return context == nullptr ? 1.0F : context->GetDensityIndependentPixelRatio();
	}

	double EventDisplay::width() const
	{
		return alive() ? m_lanes->GetClientWidth() / dpRatio() : 0.0;
	}

	double EventDisplay::height() const
	{
		return alive() ? m_lanes->GetClientHeight() / dpRatio() : 0.0;
	}

	double EventDisplay::visibleLength() const
	{
		return width() * m_view.scale;
	}

	double EventDisplay::projectLength() const
	{
		return std::max(m_end, g_minimumProjectLength);
	}

	double EventDisplay::overviewLength() const
	{
		return std::max(projectLength(), m_view.start + visibleLength());
	}

	double EventDisplay::dpFrom(Rml::Element& element, float x) const
	{
		return (x - element.GetAbsoluteLeft()) / dpRatio();
	}

	// ponytail: reads the whole project after every change; read only what the events name if large projects lag.
	void EventDisplay::requestProject()
	{
		if (m_loading)
		{
			m_stale = true; // asks again when this answer is in
			return;
		}
		m_loading = true;
		m_binding.send("project.get", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired())
					return;
				m_loading = false;
				if (result.ok())
				{
					m_project = result.value;
					++m_loads;
					loadProject(m_project);
				}
				if (std::exchange(m_stale, false))
					requestProject();
			});
	}

	void EventDisplay::loadProject(const Json& project)
	{
		m_time.sampleRate = project.at("sampleRate").get<double>();
		std::vector<time::TempoPoint> tempo;
		for (const Json& point : project.at("tempo"))
			tempo.push_back(
				{point.at("position").get<time::Ticks>(), point.at("bpm").get<double>(), point.at("ramp").get<bool>()});
		std::vector<time::SignatureChange> signatures;
		for (const Json& change : project.at("signatures"))
			signatures.push_back({change.at("bar").get<int64_t>(),
				{change.at("numerator").get<int>(), change.at("denominator").get<int>()}});
		try
		{
			m_time.tempo = time::TempoMap(tempo);
			m_time.signatures = time::SignatureMap(signatures);
		}
		catch (const std::invalid_argument& error)
		{
			logging::warning("The event display can't read the project's tempo or signatures: {}", error.what());
		}
		m_snap = engine::snapSettingsFrom(project.at("snap"), m_snap);
		m_quantize = engine::midi::quantizeSettingsFromJson(project.at("quantize"));
		showSnap();

		// A lane per track that isn't hidden, in the project's order.
		std::map<uint64_t, size_t> lanes;
		std::map<uint64_t, bool> linear;
		m_tracks.clear();
		m_laneTracks.clear();
		m_laneTypes.clear();
		m_laneLinear.clear();
		m_events.clear();
		for (const Json& track : project.at("tracks"))
		{
			const auto id = track.at("id").get<uint64_t>();
			linear[id] = track.at("timeBase") == "linear";
			if (m_hidden.count(id) != 0)
				continue;
			lanes[id] = lanes.size();
			m_tracks.push_back({static_cast<float>(trackHeight(id)), {}});
			m_laneTracks.push_back(id);
			m_laneTypes.push_back(track.at("type").get<std::string>());
			m_laneLinear.push_back(linear[id]);
		}
		const auto seconds = [this](int64_t position, bool isLinear)
		{ return isLinear ? static_cast<double>(position) / m_time.sampleRate : m_time.tempo.toSeconds(position); };
		std::map<uint64_t, const Json*> media;
		for (const Json& file : project.at("media"))
			media[file.at("id").get<uint64_t>()] = &file;
		std::map<uint64_t, uint64_t> clipMedia;
		for (const Json& clip : project.at("clips"))
			clipMedia[clip.at("id").get<uint64_t>()] = clip.at("media").get<uint64_t>();
		m_end = 0.0;
		const auto addEvent = [&](const Json& event, double start, double end)
		{
			const auto track = event.at("track").get<uint64_t>();
			if (lanes.count(track) == 0)
				return;
			m_tracks[lanes[track]].events.push_back({start, end - start});
			m_events.push_back(
				{event.at("id").get<uint64_t>(), lanes[track], start, end, event.value("mute", false), std::nullopt});
			m_end = std::max(m_end, end);
		};
		std::vector<EventWaveforms::Event> waves;
		for (const Json& event : project.at("audioEvents"))
		{
			const auto track = event.at("track").get<uint64_t>();
			const auto file = media.find(clipMedia[event.at("clip").get<uint64_t>()]);
			const double rate = file == media.end() ? 0.0 : file->second->at("sampleRate").get<double>();
			const double start = seconds(event.at("position").get<int64_t>(), linear[track]);
			const double length = rate > 0.0 ? event.at("length").get<double>() / rate : 0.0;
			addEvent(event, start, start + length);
			// The snap point counts while it is inside the event.
			const Json& snapPoint = event.at("snapPoint");
			const double into = snapPoint.is_null() || rate <= 0.0
				? -1.0
				: (snapPoint.get<double>() - event.at("offset").get<double>()) / rate;
			if (!m_events.empty() && m_events.back().id == event.at("id") && into >= 0.0 && into < length)
				m_events.back().snapPoint = start + into;
			if (file != media.end() && rate > 0.0 && lanes.count(track) != 0)
				waves.push_back({event.at("id").get<uint64_t>(), lanes[track], {start, length}, file->first, rate,
					file->second->at("channels").get<int>(), event.at("offset").get<int64_t>()});
		}
		m_waveforms->setEvents(waves);
		for (const Json& part : project.at("midiParts"))
		{
			const auto track = part.at("track").get<uint64_t>();
			const auto position = part.at("position").get<int64_t>();
			addEvent(part, seconds(position, linear[track]),
				seconds(position + part.at("length").get<int64_t>(), linear[track]));
		}
		for (EventLanes::Lane& lane : m_tracks)
			std::sort(lane.events.begin(), lane.events.end(),
				[](const TimeSpan& a, const TimeSpan& b) { return a.start < b.start; });
		// Events that are gone leave the selection.
		std::vector<uint64_t> selection;
		for (const uint64_t id : m_selection)
			if (std::any_of(m_events.begin(), m_events.end(), [id](const Shown& shown) { return shown.id == id; }))
				selection.push_back(id);
		m_selection = std::move(selection);
		setLanes();
		showSelection();
		setRulerFormat(m_format); // the tempo or signatures may have changed
		setCursor(m_cursorSample);
		apply();
	}

	void EventDisplay::setLanes()
	{
		if (!alive())
			return;
		std::vector<EventLanes::Lane> lanes = m_tracks;
		std::vector<TimeSpan> all;
		m_laneTops.assign(1, 0.0);
		// Muted and selected events again over them, for the style sheets to show; selected ones where a drag
		// moves them.
		std::vector<EventLanes::Lane> muted;
		for (size_t i = 0; i < lanes.size(); ++i)
		{
			lanes[i].height = static_cast<float>(trackHeight(m_laneTracks[i]) * m_view.trackZoom);
			m_laneTops.push_back(m_laneTops.back() + lanes[i].height);
			muted.push_back({lanes[i].height, {}});
			all.insert(all.end(), lanes[i].events.begin(), lanes[i].events.end());
		}
		std::vector<EventLanes::Lane> selected = muted;
		const double shift = m_drag == Drag::Events && m_dragMoved ? m_dragDx * m_view.scale : 0.0;
		for (const Shown& shown : m_events)
		{
			if (shown.mute)
				muted[shown.lane].events.push_back({shown.start, shown.end - shown.start});
			const auto lane = static_cast<int64_t>(shown.lane) + (m_drag == Drag::Events ? m_dragLanes : 0);
			if (std::binary_search(m_selection.begin(), m_selection.end(), shown.id) && lane >= 0 &&
				lane < static_cast<int64_t>(selected.size()))
				selected[static_cast<size_t>(lane)].events.push_back({shown.start + shift, shown.end - shown.start});
		}
		for (std::vector<EventLanes::Lane>* overlay : {&muted, &selected})
			for (EventLanes::Lane& lane : *overlay)
				std::sort(lane.events.begin(), lane.events.end(),
					[](const TimeSpan& a, const TimeSpan& b) { return a.start < b.start; });
		m_mutedLanes->setLanes(std::move(muted));
		m_selectedLanes->setLanes(std::move(selected));
		m_lanes->setLanes(std::move(lanes));
		// The overview line has every event in one row, as high as the line is.
		std::sort(all.begin(), all.end(), [](const TimeSpan& a, const TimeSpan& b) { return a.start < b.start; });
		std::vector<EventLanes::Lane> overview{{10000.0F, std::move(all)}};
		m_overviewEvents->setLanes(std::move(overview));
	}

	void EventDisplay::followTransport(const Json& status)
	{
		setCursor(status.at("position").get<int64_t>());
		m_left = status.value("left", m_left);
		m_right = status.value("right", m_right);
		const bool playing = status.at("playing").get<bool>();
		if (playing && !m_position)
		{
			m_position = m_binding.watch<int64_t>([this] { return m_engine.playbackPosition(); },
				[this](const int64_t& sample)
				{
					setCursor(sample);
					follow();
				});
		}
		else if (!playing)
		{
			m_position.reset();
			m_suspended = false;
		}
		m_playing = playing;
		follow();
		apply();
		showAutoScroll();
	}

	void EventDisplay::setCursor(int64_t sample)
	{
		m_cursorSample = sample;
		m_cursor = static_cast<double>(sample) / m_time.sampleRate;
		if (alive())
			m_cursorLine->SetProperty(Rml::PropertyId::Left, dp((m_cursor - m_view.start) / m_view.scale));
	}

	void EventDisplay::follow()
	{
		const double visible = visibleLength();
		if (!m_autoScroll || m_suspended || !m_playing || !(visible > 0.0))
			return;
		if (m_autoScrollMode == AutoScrollMode::Stationary)
			scrollTo(m_cursor - (visible / 2.0));
		else if (m_cursor < m_view.start || m_cursor >= m_view.start + visible)
			scrollTo(m_cursor);
	}

	void EventDisplay::remember()
	{
		m_undo.push_back(m_view);
		m_redo.clear();
	}

	void EventDisplay::zoomAround(double factor, double time, double x)
	{
		TimelineView view = m_view;
		view.scale = std::clamp(view.scale * factor, g_minScale, g_maxScale);
		view.start = time - (x * view.scale);
		setView(view);
	}

	void EventDisplay::zoomVertically(double factor)
	{
		TimelineView view = m_view;
		view.trackZoom = std::clamp(view.trackZoom * factor, g_minTrackZoom, g_maxTrackZoom);
		setView(view);
	}

	void EventDisplay::zoomIn()
	{
		remember();
		const double x = (m_cursor - m_view.start) / m_view.scale;
		zoomAround(1.0 / g_zoomStep, m_cursor, x >= 0.0 && x <= width() ? x : width() / 2.0);
	}

	void EventDisplay::zoomOut()
	{
		remember();
		const double x = (m_cursor - m_view.start) / m_view.scale;
		zoomAround(g_zoomStep, m_cursor, x >= 0.0 && x <= width() ? x : width() / 2.0);
	}

	void EventDisplay::zoomInVertically()
	{
		remember();
		zoomVertically(g_verticalZoomStep);
	}

	void EventDisplay::zoomOutVertically()
	{
		remember();
		zoomVertically(1.0 / g_verticalZoomStep);
	}

	void EventDisplay::zoomFull()
	{
		zoomTo(0.0, projectLength());
	}

	void EventDisplay::zoomTo(double from, double to)
	{
		if (!(to > from) || !(width() > 0.0))
			return;
		remember();
		TimelineView view = m_view;
		view.start = from;
		view.scale = std::clamp((to - from) / width(), g_minScale, g_maxScale);
		setView(view);
	}

	bool EventDisplay::undoZoom()
	{
		if (m_undo.empty())
			return false;
		m_redo.push_back(m_view);
		setView(m_undo.back());
		m_undo.pop_back();
		return true;
	}

	bool EventDisplay::redoZoom()
	{
		if (m_redo.empty())
			return false;
		m_undo.push_back(m_view);
		setView(m_redo.back());
		m_redo.pop_back();
		return true;
	}

	void EventDisplay::scrollTo(double start)
	{
		TimelineView view = m_view;
		view.start = start;
		setView(view);
	}

	void EventDisplay::scrollVertically(double top)
	{
		TimelineView view = m_view;
		view.top = top;
		setView(view);
	}

	void EventDisplay::setView(TimelineView view)
	{
		view.start = std::max(view.start, 0.0); // nothing before the project start
		const bool heights = view.trackZoom != m_view.trackZoom;
		m_view.trackZoom = view.trackZoom;
		if (heights)
			setLanes();
		view.top = std::clamp(view.top, 0.0, std::max(0.0, m_laneTops.back() - height()));
		m_view = view;
		apply();
	}

	void EventDisplay::apply()
	{
		if (!alive())
			return;
		for (Rml::Element* canvas : {static_cast<Rml::Element*>(m_ruler), static_cast<Rml::Element*>(m_lanes),
				 static_cast<Rml::Element*>(m_mutedLanes), static_cast<Rml::Element*>(m_selectedLanes)})
		{
			canvas->SetAttribute("start", m_view.start);
			canvas->SetAttribute("scale", m_view.scale);
		}
		for (Rml::Element* canvas : {m_lanes, m_mutedLanes, m_selectedLanes})
			canvas->SetAttribute("scroll-top", m_view.top);
		m_waveforms->show({m_view.start, m_view.scale, m_view.top, m_laneTops, width(), height(), dpRatio()});
		setCursor(m_cursorSample);

		// The overview line shows the whole project, and the view as a rectangle.
		const double across = m_overview->GetClientWidth() / dpRatio();
		const double length = overviewLength();
		if (across > 0.0)
		{
			m_overviewEvents->SetAttribute("scale", length / across);
			m_overviewView->SetProperty(Rml::PropertyId::Left, dp(m_view.start / length * across));
			m_overviewView->SetProperty(Rml::PropertyId::Width, dp(visibleLength() / length * across));
		}
		if (m_layoutListener)
			m_layoutListener();
	}

	double EventDisplay::trackHeight(uint64_t track) const
	{
		const auto height = m_heights.find(track);
		return height == m_heights.end() ? g_trackHeight : height->second;
	}

	void EventDisplay::setTrackHeight(uint64_t track, double height)
	{
		m_heights[track] = std::clamp(height, g_minTrackHeight, g_maxTrackHeight);
		setLanes();
		setView(m_view);
	}

	void EventDisplay::setTrackHeights(double height)
	{
		for (const uint64_t track : m_laneTracks)
			m_heights[track] = std::clamp(height, g_minTrackHeight, g_maxTrackHeight);
		setLanes();
		setView(m_view);
	}

	void EventDisplay::setHiddenTracks(std::set<uint64_t> tracks)
	{
		if (tracks == m_hidden)
			return;
		m_hidden = std::move(tracks);
		if (m_project.is_object())
			loadProject(m_project);
	}

	void EventDisplay::setAutoScroll(bool on)
	{
		m_autoScroll = on;
		m_suspended = false;
		follow();
		showAutoScroll();
	}

	void EventDisplay::setAutoScrollMode(AutoScrollMode mode)
	{
		m_autoScrollMode = mode;
		follow();
		showAutoScroll();
	}

	void EventDisplay::setSuspendAutoScrollWhenEditing(bool suspend)
	{
		m_suspendWhenEditing = suspend;
		showAutoScroll();
	}

	void EventDisplay::setRulerFormat(TimeFormat format)
	{
		m_format = format;
		if (!alive())
			return;
		m_ruler->setScale(rulerScale(format, m_time));
		m_ruler->SetAttribute("format", timeFormatName(format));
		showFormat();
		showSnap(); // the grid types are the format's
	}

	void EventDisplay::setCrossHair(bool shown)
	{
		m_crossHair = shown;
		if (alive())
			m_root->SetClass("cross-hair", shown);
	}

	void EventDisplay::addZoomPreset(const std::string& name)
	{
		Json presets = Json::array();
		for (const Json& preset : m_presets)
			if (preset.at("name") != name)
				presets.push_back(preset);
		presets.push_back({{"name", name}, {"seconds", visibleLength()}});
		m_presets = std::move(presets);
		savePresets();
	}

	bool EventDisplay::applyZoomPreset(const std::string& name)
	{
		const auto preset = std::find_if(
			m_presets.begin(), m_presets.end(), [&name](const Json& each) { return each.at("name") == name; });
		if (preset == m_presets.end())
			return false;
		zoomTo(m_view.start, m_view.start + preset->at("seconds").get<double>());
		return true;
	}

	void EventDisplay::removeZoomPreset(const std::string& name)
	{
		Json presets = Json::array();
		for (const Json& preset : m_presets)
			if (preset.at("name") != name)
				presets.push_back(preset);
		m_presets = std::move(presets);
		savePresets();
	}

	void EventDisplay::renameZoomPreset(const std::string& from, const std::string& to)
	{
		for (Json& preset : m_presets)
			if (preset.at("name") == from)
				preset["name"] = to;
		savePresets();
	}

	void EventDisplay::savePresets()
	{
		m_presetsKnown = true;
		m_binding.send("settings.set", {{"key", "ui.zoomPresets"}, {"value", m_presets}});
		showPresets();
	}

	void EventDisplay::ProcessEvent(Rml::Event& event)
	{
		if (!alive())
			return;
		if (event.GetCurrentElement()->GetOwnerDocument() != m_document)
		{
			dialogEvent(event);
			return;
		}
		Rml::Element* target = event.GetTargetElement();
		switch (event.GetId())
		{
		case Rml::EventId::Mousedown:
			if (target != nullptr && event.GetParameter<int>("button", 0) == 0)
				mouseDown(event, *target);
			break;
		case Rml::EventId::Mousemove:
			if (event.GetCurrentElement() == m_document)
			{
				mouseMove(event);
			}
			else if (inside(target, m_area))
			{
				// The cross-hair cursor; the style sheets show it while the mouse is over the event lanes.
				const double x = dpFrom(*m_area, event.GetParameter<float>("mouse_x", 0.0F));
				const double y = (event.GetParameter<float>("mouse_y", 0.0F) - m_area->GetAbsoluteTop()) / dpRatio();
				m_crossVertical->SetProperty(Rml::PropertyId::Left, dp(x));
				m_crossHorizontal->SetProperty(Rml::PropertyId::Top, dp(y));
			}
			break;
		case Rml::EventId::Mouseup:
			areaUp(event);
			m_drag = Drag::None;
			break;
		case Rml::EventId::Dblclick:
			doubleClick(event);
			break;
		case Rml::EventId::Mousescroll:
			wheel(event);
			break;
		case Rml::EventId::Resize:
			setView(m_view);
			break;
		case Rml::EventId::Click:
			click(target);
			break;
		case Rml::EventId::Change:
			// A click on the auto-scroll button switches it off, or on again while it's suspended.
			if (target != nullptr && target == m_autoScrollButton)
				setAutoScroll(m_suspended || event.GetParameter<bool>("value", false));
			else if (target != nullptr && target == m_snapButton)
				setSnap({{"on", event.GetParameter<bool>("value", false)}});
			else if (target != nullptr && target == m_zeroCrossingButton)
				setSnap({{"zeroCrossing", event.GetParameter<bool>("value", false)}});
			break;
		default:
			break;
		}
	}

	void EventDisplay::mouseDown(Rml::Event& event, Rml::Element& target)
	{
		const auto x = event.GetParameter<float>("mouse_x", 0.0F);
		const auto y = event.GetParameter<float>("mouse_y", 0.0F);
		m_dragX = x;
		m_dragY = y;
		m_dragView = m_view;
		m_dragZoomed = false;
		if (&target == m_ruler)
		{
			// The lower half moves the project cursor there, and zooms when dragged down or up.
			if (y < m_ruler->GetAbsoluteTop() + (m_ruler->GetClientHeight() / 2.0F))
				return;
			m_dragAt = dpFrom(*m_ruler, x);
			m_dragTime = m_view.start + (m_dragAt * m_view.scale);
			m_drag = Drag::Ruler;
			m_binding.send("transport.locate", {{"position", std::llround(m_dragTime * m_time.sampleRate)}});
		}
		else if (inside(&target, m_overview))
		{
			m_dragLength = overviewLength();
			const double across = m_overview->GetClientWidth() / dpRatio();
			const double at = dpFrom(*m_overview, x);
			const double left = m_view.start / m_dragLength * across;
			const double right = (m_view.start + visibleLength()) / m_dragLength * across;
			if (std::abs(at - right) <= g_overviewEdge)
			{
				m_drag = Drag::OverviewEnd;
			}
			else if (std::abs(at - left) <= g_overviewEdge)
			{
				m_drag = Drag::OverviewStart;
			}
			else
			{
				if (at < left || at > right)
				{
					scrollTo((at / across * m_dragLength) - (visibleLength() / 2.0)); // centres the view there
					m_dragView = m_view;
				}
				m_drag = Drag::OverviewMove;
			}
		}
		else if (inside(&target, m_area))
		{
			if (m_playing && m_autoScroll && m_suspendWhenEditing)
			{
				m_suspended = true;
				showAutoScroll();
			}
			areaDown(event, dpFrom(*m_lanes, x), (y - m_lanes->GetAbsoluteTop()) / dpRatio());
			return; // goes on, so that RmlUi sends double clicks
		}
		event.StopPropagation();
	}

	void EventDisplay::mouseMove(Rml::Event& event)
	{
		if (m_drag == Drag::None)
			return;
		const double dx = (event.GetParameter<float>("mouse_x", 0.0F) - m_dragX) / dpRatio();
		const double dy = (event.GetParameter<float>("mouse_y", 0.0F) - m_dragY) / dpRatio();
		const double across = m_overview->GetClientWidth() / dpRatio();
		const double seconds = across > 0.0 ? dx / across * m_dragLength : 0.0;
		const auto zoomed = [this]
		{
			// One step in the zoom history for the whole drag.
			if (!std::exchange(m_dragZoomed, true))
			{
				m_undo.push_back(m_dragView);
				m_redo.clear();
			}
		};
		TimelineView view = m_dragView;
		switch (m_drag)
		{
		case Drag::Ruler:
			if (dy == 0.0 && !m_dragZoomed)
				return;
			zoomed();
			view.scale = std::clamp(m_dragView.scale * std::pow(2.0, -dy / g_dragZoomDistance), g_minScale, g_maxScale);
			view.start = m_dragTime - (m_dragAt * view.scale);
			break;
		case Drag::OverviewMove:
			view.start += seconds;
			break;
		case Drag::OverviewStart:
		case Drag::OverviewEnd:
		{
			if (!(width() > 0.0))
				return;
			zoomed();
			double start = m_dragView.start;
			double end = m_dragView.start + (width() * m_dragView.scale);
			(m_drag == Drag::OverviewStart ? start : end) += seconds;
			view.start = std::min(start, end - (width() * g_minScale));
			view.scale = std::clamp((end - view.start) / width(), g_minScale, g_maxScale);
			break;
		}
		case Drag::Events:
		case Drag::ResizeStart:
		case Drag::ResizeEnd:
			areaMove(dx, dy);
			return;
		case Drag::None:
			return;
		}
		setView(view);
	}

	void EventDisplay::wheel(Rml::Event& event)
	{
		const auto lines = event.GetParameter<float>("wheel_delta_y", 0.0F);
		const auto across = event.GetParameter<float>("wheel_delta_x", 0.0F);
		const bool control = event.GetParameter<int>("ctrl_key", 0) != 0 || event.GetParameter<int>("meta_key", 0) != 0;
		const bool shift = event.GetParameter<int>("shift_key", 0) != 0;
		if (control && shift)
		{
			remember();
			zoomVertically(std::pow(g_wheelZoomStep, -lines));
		}
		else if (control)
		{
			// Around the time under the mouse.
			remember();
			const double x = dpFrom(*m_lanes, event.GetParameter<float>("mouse_x", 0.0F));
			zoomAround(std::pow(g_wheelZoomStep, lines), m_view.start + (x * m_view.scale), x);
		}
		else if (shift)
		{
			scrollTo(m_view.start + (lines * g_wheelScroll * visibleLength()));
		}
		else
		{
			TimelineView view = m_view;
			view.start += across * g_wheelScroll * visibleLength();
			view.top += lines * g_trackHeight;
			setView(view);
		}
		event.StopPropagation();
	}
}
