// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/eventdisplay.h"

#include "platform/log.h"
#include "ui/rmlui.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Property.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
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
		constexpr std::array<Rml::EventId, 6> g_rootEvents = {Rml::EventId::Mousedown, Rml::EventId::Mousemove,
			Rml::EventId::Mouseover, Rml::EventId::Mouseout, Rml::EventId::Mousescroll, Rml::EventId::Dblclick};
		constexpr std::array<Rml::EventId, 7> g_documentEvents = {Rml::EventId::Mousemove, Rml::EventId::Mouseup,
			Rml::EventId::Resize, Rml::EventId::Click, Rml::EventId::Change, Rml::EventId::Keydown,
			Rml::EventId::Keyup};

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

		// A press in an in-place editor (#131, ui/inplaceeditor.h) is the editor's, not the lanes' under it.
		bool inInPlaceEditor(const Rml::Element* element)
		{
			for (; element != nullptr; element = element->GetParentNode())
				if (element->IsClassSet("in-place-editor"))
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
		m_folderLanes = &addCanvas<EventLanes>(*m_area, "event-lanes", "folder-data");
		m_lanes = &addCanvas<EventLanes>(*m_area, "event-lanes", "events");
		m_mutedLanes = &addCanvas<EventLanes>(*m_area, "event-lanes", "muted-events");
		m_selectedLanes = &addCanvas<EventLanes>(*m_area, "event-lanes", "selected-events");
		m_waveforms = std::make_unique<EventWaveforms>(binding, add(*m_area, "div", "waveforms"));
		m_fadeLines = &addCanvas<FadeLines>(*m_area, "fade-lines", "fades");
		m_partMarks = &add(*m_area, "div", "part-marks");
		m_automationArea = &add(*m_area, "div", "automation-lanes");
		m_editor = &add(*m_area, "div", "automation-editor");
		m_rangeElement = &add(*m_area, "div", "range-selection");
		m_rectangle = &add(*m_area, "div", "tool-rectangle");
		m_crossVertical = &add(*m_area, "div", "cross-hair-vertical");
		m_crossHorizontal = &add(*m_area, "div", "cross-hair-horizontal");
		m_cursorLine = &add(timeline, "div", "project-cursor");
		for (const Rml::EventId id : g_rootEvents)
			root.AddEventListener(id, this);
		root.AddEventListener(g_fileDropEvent, this);
		for (const Rml::EventId id : g_documentEvents)
			m_document->AddEventListener(id, this);
		findControls();
		showRuler();

		m_actions = {{"zoom.in", [this] { zoomIn(); }}, {"zoom.out", [this] { zoomOut(); }},
			{"zoom.inVertically", [this] { zoomInVertically(); }},
			{"zoom.outVertically", [this] { zoomOutVertically(); }}, {"zoom.full", [this] { zoomFull(); }},
			{"zoom.undo", [this] { undoZoom(); }}, {"zoom.redo", [this] { redoZoom(); }},
			{"view.autoScroll", [this] { setAutoScroll(!m_autoScroll); }}};
		addEditActions();
		addFadeActions();
		addVolumeActions();
		addToolActions();
		addTimeWarpActions();
		addAutomationActions();
		setToolModifiers(Json::object());

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
				takeSnap(event.data);
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
				if (const auto mode = values.find("ui.folderData"); mode != values.end())
					setFolderMode(mode->get<std::string>());
				if (const auto modifiers = values.find("ui.toolModifiers"); modifiers != values.end())
				{
					setToolModifiers(*modifiers);
					m_modifiersKnown = true;
				}
				if (const auto toolbox = values.find("ui.toolboxOnRightClick"); toolbox != values.end())
				{
					m_toolboxOnRightClick = toolbox->get<bool>();
					m_toolboxKnown = true;
					showContextMenus();
				}
				if (const auto notifies = values.find("ui.toolSwitchNotification"); notifies != values.end())
				{
					m_notifies = notifies->get<bool>();
					m_notifiesKnown = true;
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
		m_binding.send("settings.get", {{"key", "ui.folderData"}},
			[this, alive](const engine::Result& result)
			{
				if (!alive.expired() && result.ok())
					setFolderMode(result.value.at("value").get<std::string>());
			});
		m_binding.send("settings.get", {{"key", "ui.toolModifiers"}},
			[this, alive](const engine::Result& result)
			{
				if (!alive.expired() && result.ok() && !m_modifiersKnown) // newer changes win
					setToolModifiers(result.value.at("value"));
			});
		m_binding.send("settings.get", {{"key", "ui.toolboxOnRightClick"}},
			[this, alive](const engine::Result& result)
			{
				if (alive.expired() || !result.ok() || m_toolboxKnown)
					return;
				m_toolboxOnRightClick = result.value.at("value").get<bool>();
				showContextMenus();
			});
		m_binding.send("settings.get", {{"key", "ui.toolSwitchNotification"}},
			[this, alive](const engine::Result& result)
			{
				if (!alive.expired() && result.ok() && !m_notifiesKnown)
					m_notifies = result.value.at("value").get<bool>();
			});
		requestMarkers();
		showAutoScroll();
		showTools();

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
		m_root->RemoveEventListener(g_fileDropEvent, this);
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
		return std::max(m_end, m_length);
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
					m_read = m_binding.handed();
					// Settings, not undo steps: only a fresh answer has them, not m_project shown again later.
					takeSnap(m_project.at("snap"));
					m_quantize = engine::midi::quantizeSettingsFromJson(m_project.at("quantize"));
					resizeProjectChanged();
					showProject();
				}
				if (std::exchange(m_stale, false))
					requestProject();
			});
	}

	void EventDisplay::showProject()
	{
		if (!m_project.is_object())
			return;
		if (m_resizePreview.is_null() && m_releasedPreview.is_null())
		{
			loadProject(m_project);
			return;
		}
		// While an edge is dragged, and after the release until the project shows the resize, the events as
		// event.previewResize says the release leaves them (#732).
		Json project = m_project;
		for (const Json* preview : {&m_releasedPreview, &m_resizePreview})
		{
			if (preview->is_null())
				continue;
			for (const char* kind : {"audioEvents", "audioParts", "midiParts"})
			{
				std::map<uint64_t, const Json*> previewed;
				for (const Json& item : preview->at(kind))
					previewed[item.at("id").get<uint64_t>()] = &item;
				for (Json& item : project.at(kind))
					if (const auto found = previewed.find(item.at("id").get<uint64_t>()); found != previewed.end())
						item = *found->second;
			}
		}
		loadProject(project);
	}

	void EventDisplay::loadProject(const Json& project)
	{
		if (!readTimeContext(project, m_time))
			logging::warning("The event display can't read the project's tempo or signatures");
		readMarkerMagnets(project);
		showSnap();
		showRootKey(project);

		// A lane per track that isn't hidden, in the project's order.
		std::map<uint64_t, size_t> lanes;
		std::map<uint64_t, bool> linear;
		std::map<uint64_t, uint64_t> parents;
		std::set<uint64_t> folded;           // folders whose tracks are hidden
		std::map<uint64_t, uint32_t> colors; // each track's color, 0 for the default
		m_tracks.clear();
		m_laneTracks.clear();
		m_laneTypes.clear();
		m_laneLinear.clear();
		m_laneAutomation.clear();
		m_laneTakes.clear();
		m_events.clear();
		m_independent.clear();
		m_mutedPieces.clear();
		m_fades.clear();
		readAutomation(project);
		// Each track's highest lane (#99), and the display lane of each lane of a track whose lanes show. The lane of
		// an audio event in an audio part is one of the part's, in the audio part editor, not one of the track.
		std::map<uint64_t, int> topLanes;
		for (const char* kind : {"audioEvents", "audioParts", "midiParts"})
			for (const Json& item : project.at(kind))
			{
				if (item.value("part", uint64_t{0}) != 0)
					continue;
				int& top = topLanes[item.at("track").get<uint64_t>()];
				top = std::max(top, item.value("lane", 0));
			}
		std::map<std::pair<uint64_t, int>, size_t> takeLanes;
		for (const Json& track : project.at("tracks"))
		{
			const auto id = track.at("id").get<uint64_t>();
			linear[id] = track.at("timeBase") == "linear";
			parents[id] = track.at("parent").get<uint64_t>();
			colors[id] = track.at("color").is_string() ? spanColor(track.at("color").get<std::string>()) : 0;
			if (m_hidden.count(id) != 0)
			{
				folded.insert(parents[id]);
				continue;
			}
			lanes[id] = m_tracks.size();
			m_tracks.push_back({static_cast<float>(trackHeight(id)), {}});
			m_laneTracks.push_back(id);
			m_laneTypes.push_back(track.at("type").get<std::string>());
			m_laneLinear.push_back(linear[id]);
			m_laneAutomation.push_back(0);
			m_laneTakes.push_back(-1);
			// Its lanes (#99), then its automation tracks below it (#121).
			const std::string type = track.at("type").get<std::string>();
			if (lanesShown(id) && (type == "audio" || type == "instrument" || type == "midi"))
				for (int take = 0; take <= topLanes[id]; ++take)
				{
					takeLanes[{id, take}] = m_tracks.size();
					m_tracks.push_back({static_cast<float>(trackHeight(id)), {}});
					m_laneTracks.push_back(id);
					m_laneTypes.push_back(type);
					m_laneLinear.push_back(linear[id]);
					m_laneAutomation.push_back(0);
					m_laneTakes.push_back(take);
				}
			for (const uint64_t lane : openLanes(id))
			{
				m_tracks.push_back({static_cast<float>(g_trackHeight), {}});
				m_laneTracks.push_back(id);
				m_laneTypes.emplace_back("automation");
				m_laneLinear.push_back(linear[id]);
				m_laneAutomation.push_back(lane);
				m_laneTakes.push_back(-1);
			}
		}
		const auto seconds = [this](int64_t position, bool isLinear)
		{ return isLinear ? static_cast<double>(position) / m_time.sampleRate : m_time.tempo.toSeconds(position); };
		std::map<uint64_t, const Json*> media;
		for (const Json& file : project.at("media"))
			media[file.at("id").get<uint64_t>()] = &file;
		std::map<uint64_t, uint64_t> clipMedia;
		for (const Json& clip : project.at("clips"))
			clipMedia[clip.at("id").get<uint64_t>()] = clip.at("media").get<uint64_t>();
		m_folderData.assign(m_tracks.size(), {});
		m_end = 0.0;
		// The display lane of an event's own lane, if its track's lanes show.
		const auto takeLane = [&takeLanes](const Json& event)
		{
			const auto found = takeLanes.find({event.at("track").get<uint64_t>(), event.value("lane", 0)});
			return found == takeLanes.end() ? std::optional<size_t>() : found->second;
		};
		// With onTrackLane false, the track's lane gets no span for it: audio events show there where they are in
		// front.
		const auto addEvent = [&](const Json& event, double start, double end, bool onTrackLane = true)
		{
			const auto track = event.at("track").get<uint64_t>();
			// Its own color, or else its track's (reference manual p. 114).
			const Json& own = event.at("color");
			TimeSpan span{start, end - start};
			span.color = own.is_string() ? spanColor(own.get<std::string>()) : colors[track];
			// The folders around the track show it as a data block.
			for (uint64_t folder = parents[track]; folder != 0; folder = parents[folder])
				if (const auto lane = lanes.find(folder); lane != lanes.end() &&
					(m_folderMode == "always" || (m_folderMode == "hideWhenExpanded" && folded.count(folder) != 0)))
					m_folderData[lane->second].events.push_back(span);
			if (lanes.count(track) == 0)
				return;
			if (onTrackLane)
				m_tracks[lanes[track]].events.push_back(span);
			m_events.push_back({event.at("id").get<uint64_t>(), lanes[track], start, end, event.value("mute", false),
				std::nullopt, !onTrackLane});
			if (const std::optional<size_t> taken = takeLane(event))
			{
				m_tracks[*taken].events.push_back(span);
				m_events.push_back({event.at("id").get<uint64_t>(), *taken, start, end, event.value("mute", false),
					std::nullopt, false});
			}
			m_end = std::max(m_end, end);
		};
		// Audio parts (#471) show as parts; their events only as the waveforms within them.
		std::map<uint64_t, std::pair<double, double>> audioParts;
		for (const Json& part : project.at("audioParts"))
		{
			const auto track = part.at("track").get<uint64_t>();
			const auto position = part.at("position").get<int64_t>();
			const double start = seconds(position, linear[track]);
			const double end = seconds(position + part.at("length").get<int64_t>(), linear[track]);
			audioParts[part.at("id").get<uint64_t>()] = {start, end};
			addEvent(part, start, end);
		}
		std::vector<EventWaveforms::Event> waves;
		// Audio events in the order they are stacked, the one in front last, so that a click finds it (#99). Muted
		// ones play nothing and hide nothing (makePlaylist): they come behind every other.
		std::vector<const Json*> stacked;
		for (const Json& event : project.at("audioEvents"))
			stacked.push_back(&event);
		const auto front = [](const Json* event)
		{
			const auto own = event->value("front", uint64_t{0});
			return std::make_pair(!event->value("mute", false), own != 0 ? own : event->at("id").get<uint64_t>());
		};
		std::stable_sort(
			stacked.begin(), stacked.end(), [&front](const Json* a, const Json* b) { return front(a) < front(b); });
		// What a track's lane shows of its audio events: where none in front of them covers them.
		struct Piece
		{
			TimeSpan span;
			EventWaveforms::Event wave; // with media 0 for none
			bool mute = false;
		};
		std::map<size_t, std::vector<Piece>> pieces; // by lane
		for (const Json* stackedEvent : stacked)
		{
			const Json& event = *stackedEvent;
			const auto track = event.at("track").get<uint64_t>();
			const auto file = media.find(clipMedia[event.at("clip").get<uint64_t>()]);
			const double rate = file == media.end() ? 0.0 : file->second->at("sampleRate").get<double>();
			const double start = seconds(event.at("position").get<int64_t>(), linear[track]);
			const double length = rate > 0.0 ? event.at("length").get<double>() / rate : 0.0;
			if (const auto part = audioParts.find(event.value("part", uint64_t{0})); part != audioParts.end())
			{
				const double from = std::max(start, part->second.first);
				const double to = std::min(start + length, part->second.second);
				if (file != media.end() && rate > 0.0 && lanes.count(track) != 0 && to > from)
					waves.push_back({event.at("id").get<uint64_t>(), lanes[track], {from, to - from}, file->first, rate,
						file->second->at("channels").get<int>(),
						event.at("offset").get<int64_t>() + static_cast<int64_t>(std::llround((from - start) * rate))});
				continue;
			}
			addEvent(event, start, start + length, false);
			if (lanes.count(track) != 0)
			{
				const Json& own = event.at("color");
				Piece piece{{start, length, own.is_string() ? spanColor(own.get<std::string>()) : colors[track]}, {},
					event.value("mute", false)};
				if (file != media.end() && rate > 0.0)
					piece.wave = {event.at("id").get<uint64_t>(), lanes[track], {start, length}, file->first, rate,
						file->second->at("channels").get<int>(), event.at("offset").get<int64_t>()};
				std::vector<Piece> covered;
				for (const Piece& other : pieces[lanes[track]])
				{
					// What lies before it and after it stays.
					if (other.span.start < start)
					{
						Piece before = other;
						before.span.length = std::min(other.span.end(), start) - other.span.start;
						covered.push_back(before);
					}
					if (other.span.end() > start + length)
					{
						Piece after = other;
						after.span.start = std::max(other.span.start, start + length);
						after.span.length = other.span.end() - after.span.start;
						covered.push_back(after);
					}
				}
				covered.push_back(piece);
				pieces[lanes[track]] = std::move(covered);
			}

			// The snap point counts while it is inside the event.
			const Json& snapPoint = event.at("snapPoint");
			const double into = snapPoint.is_null() || rate <= 0.0
				? -1.0
				: (snapPoint.get<double>() - event.at("offset").get<double>()) / rate;
			for (auto shown = m_events.rbegin(); shown != m_events.rend() && shown->id == event.at("id"); ++shown)
				if (into >= 0.0 && into < length)
					shown->snapPoint = start + into;
			if (rate > 0.0)
				readFades(event, rate);
			if (const std::optional<size_t> taken = takeLane(event); taken && file != media.end() && rate > 0.0)
				waves.push_back({event.at("id").get<uint64_t>(), *taken, {start, length}, file->first, rate,
					file->second->at("channels").get<int>(), event.at("offset").get<int64_t>()});
		}
		for (const auto& [lane, shown] : pieces)
			for (const Piece& piece : shown)
			{
				m_tracks[lane].events.push_back(piece.span);
				if (piece.mute)
					m_mutedPieces.emplace_back(lane, piece.span);
				if (piece.wave.media == 0)
					continue;
				EventWaveforms::Event wave = piece.wave;
				wave.offset += std::llround((piece.span.start - wave.time.start) * wave.sampleRate);
				wave.time = {piece.span.start, piece.span.length};
				waves.push_back(wave);
			}
		m_waveforms->setEvents(waves);
		for (const Json& part : project.at("midiParts"))
		{
			const auto track = part.at("track").get<uint64_t>();
			const auto position = part.at("position").get<int64_t>();
			addEvent(part, seconds(position, linear[track]),
				seconds(position + part.at("length").get<int64_t>(), linear[track]));
			if (part.value("globalTranspose", std::string()) == "independent")
				m_independent.insert(part.at("id").get<uint64_t>());
		}
		for (std::vector<EventLanes::Lane>* list : {&m_tracks, &m_folderData})
			for (EventLanes::Lane& lane : *list)
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
		m_length = project.value("length", g_defaultProjectLength);
		// The ruler shows the snap settings' format (#413), which the project keeps and the engine switches when the
		// project setup's time format changes (reference manual p. 69).
		showRuler(); // the tempo, signatures, frame rate or start time may have changed
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
			const double height = m_laneAutomation[i] != 0 ? g_trackHeight : trackHeight(m_laneTracks[i]);
			lanes[i].height = static_cast<float>(height * m_view.trackZoom);
			m_laneTops.push_back(m_laneTops.back() + lanes[i].height);
			muted.push_back({lanes[i].height, {}});
			all.insert(all.end(), lanes[i].events.begin(), lanes[i].events.end());
		}
		std::vector<EventLanes::Lane> selected = muted;
		std::vector<EventLanes::Lane> folders = m_folderData;
		for (size_t i = 0; i < folders.size() && i < lanes.size(); ++i)
			folders[i].height = lanes[i].height;
		for (const auto& [lane, piece] : m_mutedPieces)
			if (lane < muted.size())
				muted[lane].events.push_back({piece.start, piece.length});
		const double shift = m_drag == Drag::Events && m_dragMoved ? m_dragDx * m_view.scale : 0.0;
		for (const Shown& shown : m_events)
		{
			if (shown.mute && !shown.onTrackLane)
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
		m_folderLanes->setLanes(std::move(folders));
		m_lanes->setLanes(std::move(lanes));
		// The overview line has every event in one row, as high as the line is.
		std::sort(all.begin(), all.end(), [](const TimeSpan& a, const TimeSpan& b) { return a.start < b.start; });
		std::vector<EventLanes::Lane> overview{{10000.0F, std::move(all)}};
		m_overviewEvents->setLanes(std::move(overview));
		showFades();
		showAutomationLanes();
		showPointer();
	}

	void EventDisplay::followTransport(const Json& status)
	{
		setCursor(status.at("position").get<int64_t>());
		m_left = status.value("left", m_left);
		m_right = status.value("right", m_right);
		const bool playing = status.at("playing").get<bool>();
		if (playing && !m_position)
		{
			m_position = m_binding.watch<int64_t>(
				m_root.get(), [this] { return m_engine.playbackPosition(); },
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
		showPointer(); // what's under a mouse that stays where it was
	}

	void EventDisplay::apply()
	{
		if (!alive())
			return;
		for (Rml::Element* canvas : {static_cast<Rml::Element*>(m_ruler), static_cast<Rml::Element*>(m_lanes),
				 static_cast<Rml::Element*>(m_mutedLanes), static_cast<Rml::Element*>(m_selectedLanes),
				 static_cast<Rml::Element*>(m_fadeLines)})
		{
			canvas->SetAttribute("start", m_view.start);
			canvas->SetAttribute("scale", m_view.scale);
		}
		for (Rml::Element* canvas : {static_cast<Rml::Element*>(m_lanes), static_cast<Rml::Element*>(m_mutedLanes),
				 static_cast<Rml::Element*>(m_selectedLanes), static_cast<Rml::Element*>(m_fadeLines)})
			canvas->SetAttribute("scroll-top", m_view.top);
		m_waveforms->show({m_view.start, m_view.scale, m_view.top, m_laneTops, width(), height(), dpRatio()});
		setCursor(m_cursorSample);
		showRange();
		placeAutomation();
		placePartMarks();

		// The overview line shows the whole project, and the view as a rectangle.
		const double across = m_overview->GetClientWidth() / dpRatio();
		const double length = overviewLength();
		if (across > 0.0)
		{
			m_overviewEvents->SetAttribute("scale", length / across);
			m_overviewView->SetProperty(Rml::PropertyId::Left, dp(m_view.start / length * across));
			m_overviewView->SetProperty(Rml::PropertyId::Width, dp(visibleLength() / length * across));
		}
		for (const std::function<void()>& listener : m_viewListeners)
			listener();
		if (m_layoutListener)
			m_layoutListener();
	}

	void EventDisplay::placePartMarks()
	{
		std::vector<const Shown*> marked;
		std::string rml;
		for (const Shown& shown : m_events)
			if (m_independent.count(shown.id) != 0 && shown.lane + 1 < m_laneTops.size())
			{
				marked.push_back(&shown);
				rml += R"(<div class="part-mark independent" part=")" + std::to_string(shown.id) +
					R"("><svg class="icon" src="../icons/part-independent.svg"/></div>)";
			}
		if (rml != m_partMarksRml)
		{
			m_partMarksRml = rml;
			m_partMarks->SetInnerRML(rml);
		}
		const double scale = m_view.scale > 0.0 ? m_view.scale : g_minScale;
		for (size_t i = 0; i < marked.size(); ++i)
			if (Rml::Element* mark = m_partMarks->GetChild(static_cast<int>(i)))
			{
				mark->SetProperty(Rml::PropertyId::Left, dp((marked[i]->end - m_view.start) / scale));
				mark->SetProperty(Rml::PropertyId::Top, dp(m_laneTops[marked[i]->lane] - m_view.top));
			}
	}

	double EventDisplay::trackHeight(uint64_t track) const
	{
		const auto height = m_heights.find(track);
		const double own = height == m_heights.end() ? g_trackHeight : height->second;
		return m_inPlace.count(track) != 0 ? std::max(own, g_inPlaceHeight) : own;
	}

	void EventDisplay::setInPlaceTracks(std::set<uint64_t> tracks)
	{
		if (tracks == m_inPlace)
			return;
		m_inPlace = std::move(tracks);
		setLanes();
		setView(m_view);
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
		showProject();
	}

	void EventDisplay::showLanes(uint64_t track, bool shown)
	{
		if (shown == lanesShown(track))
			return;
		if (shown)
			m_lanesShown.insert(track);
		else
			m_lanesShown.erase(track);
		showProject();
	}

	void EventDisplay::setFolderMode(const std::string& mode)
	{
		if (mode == m_folderMode)
			return;
		m_folderMode = mode;
		showProject();
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

	// The snap grid follows the format at once; the engine's snap.changed confirms it.
	void EventDisplay::setRulerFormat(TimeFormat format)
	{
		m_format = format;
		const std::string name = timeFormatName(format);
		if (const std::optional<engine::GridFormat> grid = engine::gridFormatFromName(name);
			grid && *grid != m_snap.format)
		{
			m_snap.format = *grid;
			setSnap({{"format", name}});
		}
		showRuler();
	}

	// Settings the engine sent before it had the format the ruler chose keep that format.
	void EventDisplay::takeSnap(const Json& settings)
	{
		const engine::GridFormat chosen = m_snap.format;
		m_snap = engine::snapSettingsFrom(settings, m_snap);
		if (m_formatsSent > 0)
			m_snap.format = chosen;
		followSnapFormat();
	}

	void EventDisplay::followSnapFormat()
	{
		if (const std::optional<TimeFormat> format = timeFormatFromName(engine::gridFormatName(m_snap.format));
			format && *format != m_format)
		{
			m_format = *format;
			showRuler();
		}
	}

	void EventDisplay::showRuler()
	{
		if (!alive())
			return;
		m_ruler->setScale(rulerScale(m_format, m_time));
		m_ruler->SetAttribute("format", timeFormatName(m_format));
		showFormat();
		showSnap(); // the grid types are the format's
	}

	void EventDisplay::setCrossHair(bool shown)
	{
		m_crossHair = shown;
		showTools(); // and only with the tools that use it
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
		if (event.GetType() == g_fileDropEvent)
		{
			dropFiles(event);
			return;
		}
		Rml::Element* target = event.GetTargetElement();
		switch (event.GetId())
		{
		case Rml::EventId::Mousedown:
			if (inInPlaceEditor(target))
				break; // the editor's (ui/inplaceeditor.h)
			if (target != nullptr && event.GetParameter<int>("button", 0) == 0)
				mouseDown(event, *target);
			else if (event.GetParameter<int>("button", 0) == 1 && inside(target, m_area))
				areaRightDown(event, dpFrom(*m_lanes, event.GetParameter<float>("mouse_x", 0.0F)),
					(event.GetParameter<float>("mouse_y", 0.0F) - m_lanes->GetAbsoluteTop()) / dpRatio());
			break;
		case Rml::EventId::Mousemove:
		case Rml::EventId::Mouseover: // back over the lanes, e.g. when a menu over them closes, without a move
			if (event.GetId() == Rml::EventId::Mousemove && event.GetCurrentElement() == m_document)
			{
				mouseMove(event);
			}
			else if (event.GetId() == Rml::EventId::Mousemove ? inside(target, m_area) : target == m_area)
			{
				// The cross-hair cursor; the style sheets show it while the mouse is over the event lanes.
				const double x = dpFrom(*m_area, event.GetParameter<float>("mouse_x", 0.0F));
				const double y = (event.GetParameter<float>("mouse_y", 0.0F) - m_area->GetAbsoluteTop()) / dpRatio();
				m_crossVertical->SetProperty(Rml::PropertyId::Left, dp(x));
				m_crossHorizontal->SetProperty(Rml::PropertyId::Top, dp(y));
				// The mouse pointer for what a press there does, with the modifier keys held.
				m_mouse = std::make_pair(dpFrom(*m_lanes, event.GetParameter<float>("mouse_x", 0.0F)),
					(event.GetParameter<float>("mouse_y", 0.0F) - m_lanes->GetAbsoluteTop()) / dpRatio());
				heldKeys(event);
				showPointer();
			}
			break;
		case Rml::EventId::Mouseout:
			// Left the event lanes: their changes don't change the pointer until the mouse is back.
			if (target == m_area)
				m_mouse.reset();
			break;
		case Rml::EventId::Keydown:
			// Escape cancels a drag of edges (#732), and does nothing else then.
			if (event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN) == Rml::Input::KI_ESCAPE &&
				cancelResize())
			{
				event.StopPropagation();
				break;
			}
			heldKeys(event); // a modifier key may change what a press does
			resizeKeysChanged();
			break;
		case Rml::EventId::Keyup:
			heldKeys(event);
			resizeKeysChanged();
			break;
		case Rml::EventId::Mouseup:
			areaUp(event);
			m_drag = Drag::None;
			showPointer();
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
			else if (target != nullptr && target == m_combineButton)
				setCombineSelectionTools(event.GetParameter<bool>("value", false));
			break;
		default:
			break;
		}
	}

	void EventDisplay::mouseDown(Rml::Event& event, Rml::Element& target)
	{
		// A drag of edges whose release the display didn't see (#732).
		if (!m_resizeWanted.is_null())
		{
			endResize();
			showProject();
		}
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
		case Drag::Rectangle:
		case Drag::Range:
		case Drag::DrawPart:
		case Drag::Comp:
		case Drag::Play:
		case Drag::Click:
		case Drag::FadeIn:
		case Drag::FadeOut:
		case Drag::Automation:
		case Drag::Volume:
		case Drag::VolumePoint:
		case Drag::TimeWarp:
			areaMove(event, dx, dy);
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
