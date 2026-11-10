// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/audioparteditor.h"

#include "engine/midi/quantizesettings.h"
#include "platform/log.h"
#include "ui/timeformat.h"
#include "ui/widgetutils.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <tuple>
#include <utility>

namespace gnubase::ui
{
	using engine::Json;

	namespace
	{
		constexpr double g_laneHeight = 48.0;   // dp, each lane of the part display
		constexpr double g_dragThreshold = 3.0; // dp the mouse moves before a press becomes a drag

		constexpr std::array<std::pair<AudioPartTool, const char*>, 7> g_tools = {{
			{AudioPartTool::ObjectSelection, "objectSelection"},
			{AudioPartTool::Erase, "erase"},
			{AudioPartTool::Split, "split"},
			{AudioPartTool::Glue, "glue"},
			{AudioPartTool::Mute, "mute"},
			{AudioPartTool::Play, "play"},
			{AudioPartTool::Scrub, "scrub"},
		}};
		constexpr std::array<const char*, 7> g_toolTitles = {
			"Object Selection", "Erase", "Split", "Glue", "Mute", "Play", "Scrub"};
		constexpr std::array<const char*, 7> g_toolLabels = {
			"Select", "Erase", "Split", "Glue", "Mute", "Play", "Scrub"};

		// The root's. What a press that starts a gesture hits is an RmlUi drag element (drag: drag in the style sheet,
		// by tool): a gesture's mouse moves come as its drag events, and its dragend comes wherever the button goes up,
		// also outside the window, where no element gets a mouseup. A gesture's mouseup in the window comes from the
		// document (listen()), also for a press that didn't move, which makes no drag.
		constexpr std::array<Rml::EventId, 5> g_events = {Rml::EventId::Mousedown, Rml::EventId::Click,
			Rml::EventId::Dblclick, Rml::EventId::Drag, Rml::EventId::Dragend};

		std::string attribute(const Rml::Element& element, const char* name)
		{
			return element.GetAttribute<Rml::String>(name, "");
		}

		uint64_t idAttribute(const Rml::Element& element, const char* name)
		{
			const std::string value = attribute(element, name);
			return value.empty() ? 0 : std::stoull(value);
		}

		Rml::Element* withAttribute(Rml::Element* element, const char* name, const Rml::Element* stop)
		{
			for (; element != nullptr && element != stop; element = element->GetParentNode())
				if (element->HasAttribute(name))
					return element;
			return nullptr;
		}

		bool inside(const Rml::Element* element, const Rml::Element* area)
		{
			for (; element != nullptr; element = element->GetParentNode())
				if (element == area)
					return true;
			return false;
		}

		void setClass(Rml::Element* element, const char* name, bool set)
		{
			if (element != nullptr && element->IsClassSet(name) != set)
				element->SetClass(name, set);
		}

		void setInner(Rml::Element* element, const std::string& rml)
		{
			if (element != nullptr && element->GetInnerRML() != rml)
				element->SetInnerRML(rml);
		}

		Rml::Property percent(double value)
		{
			return {static_cast<float>(value), Rml::Unit::PERCENT};
		}

		Rml::Property dp(double value)
		{
			return {static_cast<float>(value), Rml::Unit::DP};
		}

	}

	const char* audioPartToolName(AudioPartTool tool)
	{
		for (const auto& [each, name] : g_tools)
			if (each == tool)
				return name;
		return "objectSelection";
	}

	std::optional<AudioPartTool> audioPartToolFromName(const std::string& name)
	{
		for (const auto& [tool, each] : g_tools)
			if (name == each)
				return tool;
		return std::nullopt;
	}

	AudioPartEditor::AudioPartEditor(EngineBinding& binding, Rml::Element& root, std::string view)
		: m_binding(binding), m_root(root.GetObserverPtr()), m_name(std::move(view))
	{
		std::string tools;
		for (size_t i = 0; i < g_tools.size(); ++i)
			tools += std::string(R"(<button class="ape-tool" ape-tool=")") + g_tools[i].second + R"(" title=")" +
				g_toolTitles[i] + R"(">)" + g_toolLabels[i] + "</button>";
		std::string info;
		for (const auto& [field, label] : std::vector<std::pair<const char*, const char*>>{
				 {"start", "Start"}, {"end", "End"}, {"length", "Length"}, {"volume", "Volume"}})
			info += std::string(R"(<div class="info-field" info-field=")") + field + R"("><span class="info-label">)" +
				label + R"(</span><textdisplay class="display" field=")" + field + R"("/></div>)";
		root.SetInnerRML(R"(<div class="toolbar audio-part-editor-toolbar">)" + tools +
			R"(</div><div class="info-line ape-info empty"><span class="info-empty">No Object Selected</span>)" + info +
			R"(</div><div class="editor-parts"/><div class="ape-body"><div class="ape-display"><div class="ape-lanes"/>)"
			R"(<div class="ape-events"/><div class="ape-waveforms"/></div></div>)");
		if (Rml::Element* waves = root.QuerySelector(".ape-waveforms"))
			m_waveforms = std::make_unique<EventWaveforms>(m_binding, *waves);
		for (const Rml::EventId event : g_events)
			root.AddEventListener(event, this);
		root.AddEventListener(Rml::EventId::Change, this);
		// A window that changes its size changes the display's width, which the waveforms' scale follows.
		if (Rml::ElementDocument* document = root.GetOwnerDocument())
		{
			document->AddEventListener(Rml::EventId::Resize, this);
			m_document = document->GetObserverPtr();
		}
		setTool(AudioPartTool::ObjectSelection);
	}

	AudioPartEditor::~AudioPartEditor()
	{
		listen(false);
		if (Rml::Element* root = m_root.get())
		{
			for (const Rml::EventId event : g_events)
				root->RemoveEventListener(event, this);
			root->RemoveEventListener(Rml::EventId::Change, this);
		}
		if (Rml::Element* document = m_document.get())
			document->RemoveEventListener(Rml::EventId::Resize, this);
		if (m_gesture == Gesture::Audition)
		{
			try
			{
				m_binding.send("audition.stop"); // what the play or scrub tool auditions ends with the editor
			}
			catch (const std::exception&) // NOLINT(bugprone-empty-catch): nothing to do about it while it goes
			{
			}
		}
	}

	void AudioPartEditor::setTool(AudioPartTool tool)
	{
		m_tool = tool;
		Rml::Element* root = m_root.get();
		if (root == nullptr)
			return;
		Rml::ElementList buttons;
		root->QuerySelectorAll(buttons, ".ape-tool");
		for (Rml::Element* button : buttons)
			setClass(button, "checked", attribute(*button, "ape-tool") == audioPartToolName(tool));
		root->SetAttribute("tool", audioPartToolName(tool));
	}

	void AudioPartEditor::show(const Json& project, const Json& view)
	{
		m_project = project;
		m_view = view;
		m_events.clear();
		m_parts.clear();
		m_waves.clear();
		// A view that comes while select()'s editor.set is unanswered may be older than the selection it sent (results
		// and events come in order, and that command's editor.changed after its result), so the selection stays.
		const bool selectionIsCurrent = m_selecting == 0;
		if (selectionIsCurrent)
			m_selection.clear();
		if (!m_project.contains("tracks") || !m_view.contains("parts"))
			return;
		const TimeContext time = projectTime(m_project);
		std::map<uint64_t, bool> linear;
		for (const Json& track : m_project.at("tracks"))
			linear[track.at("id").get<uint64_t>()] = track.value("timeBase", std::string("musical")) == "linear";
		const auto seconds = [&](int64_t position, uint64_t track)
		{ return linear[track] ? static_cast<double>(position) / time.sampleRate : time.tempo.toSeconds(position); };
		std::map<uint64_t, const Json*> media;
		for (const Json& file : m_project.at("media"))
			media[file.at("id").get<uint64_t>()] = &file;
		std::map<uint64_t, uint64_t> clipMedia;
		for (const Json& clip : m_project.at("clips"))
			clipMedia[clip.at("id").get<uint64_t>()] = clip.at("media").get<uint64_t>();
		// The shown parts: their time and whether the editing mode reaches them.
		const Json& editable = m_view.at("editable");
		m_from = 0.0;
		m_to = 1.0;
		bool first = true;
		for (const Json& part : m_project.value("audioParts", Json::array()))
		{
			const Json& shown = m_view.at("parts");
			if (std::find(shown.begin(), shown.end(), part.at("id")) == shown.end())
				continue;
			const auto track = part.at("track").get<uint64_t>();
			const auto position = part.at("position").get<int64_t>();
			const double start = seconds(position, track);
			const double end = seconds(position + part.at("length").get<int64_t>(), track);
			m_parts[part.at("id").get<uint64_t>()] = {
				start, end, track, std::find(editable.begin(), editable.end(), part.at("id")) != editable.end()};
			m_from = first ? start : std::min(m_from, start);
			m_to = first ? end : std::max(m_to, end);
			first = false;
		}
		for (const Json& audio : m_project.value("audioEvents", Json::array()))
		{
			const auto part = m_parts.find(audio.value("part", uint64_t{0}));
			if (part == m_parts.end())
				continue;
			const auto track = audio.at("track").get<uint64_t>();
			const auto file = media.find(clipMedia[audio.at("clip").get<uint64_t>()]);
			const double rate = file == media.end() ? 0.0 : file->second->at("sampleRate").get<double>();
			const double start = seconds(audio.at("position").get<int64_t>(), track);
			const double length = rate > 0.0 ? audio.at("length").get<double>() / rate : 0.0;
			if (!(length > 0.0))
				continue;
			// What the part lets play of it; all of it where the part hides it entirely, so that it can be selected
			// and moved back.
			double from = std::max(start, part->second.start);
			double to = std::min(start + length, part->second.end);
			const bool outside = !(to > from);
			if (outside)
			{
				from = start;
				to = start + length;
				m_from = std::min(m_from, from);
				m_to = std::max(m_to, to);
			}
			const Event event{audio.at("id").get<uint64_t>(), part->first, track, start, start + length, from, to,
				std::max(audio.value("lane", 0), 0), audio.value("volume", 0.0), audio.value("mute", false),
				part->second.editable, outside, audio.at("clip").get<uint64_t>(), audio.at("offset").get<int64_t>(),
				audio.at("length").get<int64_t>()};
			m_events.push_back(event);
			m_waves.push_back({event.id, static_cast<size_t>(event.lane), {from, to - from}, file->first, rate,
				file->second->at("channels").get<int>(),
				audio.at("offset").get<int64_t>() + static_cast<int64_t>(std::llround((from - start) * rate))});
		}
		m_to = std::max(m_to, m_from + 0.001);
		updateMove();
		if (selectionIsCurrent)
			for (const Json& id : m_view.value("eventSelection", Json::array()))
				m_selection.push_back(id.get<uint64_t>());
		if (m_waveforms)
			m_waveforms->setEvents(m_waves);
		// The display needs its width for the waveforms' scale: lay it out once if it hasn't been yet.
		Rml::Element* display = m_root ? m_root->QuerySelector(".ape-display") : nullptr;
		if (display != nullptr && !(display->GetClientWidth() > 0.0F))
		{
			render();
			m_root->GetOwnerDocument()->UpdateDocument();
		}
		render();
	}

	bool AudioPartEditor::moving() const
	{
		return m_gesture == Gesture::Move || m_gesture == Gesture::MoveVertical;
	}

	// A move goes on with its event as it is now, and by the way the mouse has gone on the display as it is now. An
	// event that went or that the part editing mode no longer reaches ends it without its effect: the event's selection
	// went with it, and so may its drag element (renderEvents()).
	void AudioPartEditor::updateMove()
	{
		if (!moving() || !m_pressed)
			return;
		const auto found =
			std::find_if(m_events.begin(), m_events.end(), [&](const Event& each) { return each.id == m_pressed->id; });
		if (found == m_events.end() || !found->editable)
		{
			cancel();
			return;
		}
		// A move is measured from its event and from the time under the press. While those stay, it stays as the mouse
		// last left it, even if the project shown is new.
		const double pressedAt = at(m_pressedMouse).first;
		const bool changed = found->start != m_pressed->start || pressedAt != m_pressedAt.first;
		m_pressed = *found;
		m_pressedAt.first = pressedAt;
		if (m_dragged && changed)
			updateMoved();
	}

	// What a dragged move has moved so far, by the mouse's last place in the drag.
	void AudioPartEditor::updateMoved()
	{
		if (!m_pressed)
			return;
		const auto [seconds, lane] = at(m_draggedTo);
		m_moved = {
			m_gesture == Gesture::Move ? snap(m_pressed->start + seconds - m_pressedAt.first) - m_pressed->start : 0.0,
			lane - m_pressedAt.second};
	}

	float AudioPartEditor::dpRatio() const
	{
		const Rml::Context* context = m_root ? m_root->GetContext() : nullptr;
		return context == nullptr ? 1.0F : context->GetDensityIndependentPixelRatio();
	}

	// The lanes the events are on, and one more to move events onto.
	int AudioPartEditor::lanes() const
	{
		int count = 1;
		for (const Event& event : m_events)
			count = std::max(count, event.lane + 1);
		return count + 1;
	}

	void AudioPartEditor::place(Rml::Element& element, double start, double end, int lane) const
	{
		const double range = m_to - m_from;
		element.SetProperty(Rml::PropertyId::Left, percent((start - m_from) / range * 100.0));
		element.SetProperty(Rml::PropertyId::Width, percent((end - start) / range * 100.0));
		element.SetProperty(Rml::PropertyId::Top, dp(lane * g_laneHeight));
		element.SetProperty(Rml::PropertyId::Height, dp(g_laneHeight));
	}

	// The parts by track, a row per track in the editor's order: each part at its time, .active and .editable as
	// MidiEditor's view says.
	std::string AudioPartEditor::partsRml() const
	{
		std::string rml;
		std::vector<uint64_t> tracks;
		for (const Json& id : m_view.at("parts"))
			for (const Json& part : m_project.value("audioParts", Json::array()))
				if (part.at("id") == id &&
					std::find(tracks.begin(), tracks.end(), part.at("track").get<uint64_t>()) == tracks.end())
					tracks.push_back(part.at("track").get<uint64_t>());
		const Json& editable = m_view.at("editable");
		for (const uint64_t track : tracks)
		{
			rml += R"(<div class="editor-part-row" track=")" + std::to_string(track) + R"(">)";
			for (const Json& id : m_view.at("parts"))
				for (const Json& part : m_project.value("audioParts", Json::array()))
					if (part.at("id") == id && part.at("track") == track)
						rml += R"(<div class="editor-part)" +
							std::string(id == m_view.at("activePart") ? " active" : "") +
							(std::find(editable.begin(), editable.end(), id) != editable.end() ? " editable" : "") +
							R"(" part=")" + id.dump() + R"(">)" +
							Rml::StringUtilities::EncodeRml(part.at("name").get<std::string>()) + "</div>";
			rml += "</div>";
		}
		return rml;
	}

	void AudioPartEditor::render()
	{
		Rml::Element* root = m_root.get();
		if (root == nullptr || !m_view.contains("parts"))
			return;
		setInner(root->QuerySelector(".editor-parts"), partsRml());
		Rml::ElementList shown;
		root->QuerySelectorAll(shown, ".editor-part");
		const TimeContext time = projectTime(m_project);
		for (Rml::Element* element : shown)
			for (const Json& part : m_project.value("audioParts", Json::array()))
				if (part.at("id") == idAttribute(*element, "part"))
				{
					std::string base = "musical";
					for (const Json& track : m_project.at("tracks"))
						if (track.at("id") == part.at("track"))
							base = track.value("timeBase", base);
					const auto seconds = [&](int64_t position)
					{
						return base == "linear" ? static_cast<double>(position) / time.sampleRate
												: time.tempo.toSeconds(position);
					};
					const auto position = part.at("position").get<int64_t>();
					const double range = m_to - m_from;
					element->SetProperty(Rml::PropertyId::Left, percent((seconds(position) - m_from) / range * 100.0));
					element->SetProperty(Rml::PropertyId::Width,
						percent((seconds(position + part.at("length").get<int64_t>()) - seconds(position)) / range *
							100.0));
				}

		const int count = lanes();
		std::string lanesRml;
		for (int lane = 0; lane < count; ++lane)
			lanesRml += R"(<div class="ape-lane" lane=")" + std::to_string(lane) + R"("/>)";
		Rml::Element* laneList = root->QuerySelector(".ape-lanes");
		setInner(laneList, lanesRml);
		if (laneList != nullptr)
			for (int lane = 0; lane < laneList->GetNumChildren(); ++lane)
				place(*laneList->GetChild(lane), m_from, m_to, lane);
		Rml::Element* display = root->QuerySelector(".ape-display");
		if (display != nullptr)
			display->SetProperty(Rml::PropertyId::Height, dp(count * g_laneHeight));

		Rml::Element* events = root->QuerySelector(".ape-events");
		if (events != nullptr)
		{
			renderEvents(*events);
			for (int i = 0; i < events->GetNumChildren(); ++i)
				if (const Event* found = eventOf(events->GetChild(i)))
				{
					place(*events->GetChild(i), found->shownStart, found->shownEnd, found->lane);
					setClass(events->GetChild(i), "selected", selected(*found));
					setClass(events->GetChild(i), "muted", found->mute);
					setClass(events->GetChild(i), "outside", found->outside);
				}
		}
		preview(); // a render while a move drags, as when the selection comes back, keeps showing it
		if (m_waveforms && display != nullptr)
		{
			const double width = display->GetClientWidth() / dpRatio();
			std::vector<double> tops;
			for (int lane = 0; lane <= count; ++lane)
				tops.push_back(lane * g_laneHeight);
			if (width > 0.0)
				m_waveforms->show({m_from, (m_to - m_from) / width, 0.0, tops, width, count * g_laneHeight, dpRatio()});
		}
		renderInfo();
		if (Rml::Context* context = root->GetContext())
			context->RequestNextUpdate(0);
	}

	// The events' elements in m_events' order, made again only when the events or what the part editing mode reaches
	// change: the selection only changes classes, so that a press that selects keeps the element under the mouse. A
	// move's pressed element always stays: it is RmlUi's drag element, and when that goes RmlUi drops the drag without
	// a dragend (Context::OnElementDetach), so that the move would get no more of its events.
	void AudioPartEditor::renderEvents(Rml::Element& events)
	{
		std::string shown;
		for (const Event& event : m_events)
			shown +=
				std::to_string(event.part) + ' ' + std::to_string(event.id) + (event.editable ? "\n" : " inactive\n");
		Rml::ElementDocument* document = events.GetOwnerDocument();
		if (shown == m_shownEvents || document == nullptr)
			return;
		m_shownEvents = std::move(shown);

		Rml::Element* kept = nullptr;
		uint64_t keptId = 0;
		if (moving() && m_pressed)
		{
			keptId = m_pressed->id;
			for (int i = 0; i < events.GetNumChildren() && kept == nullptr; ++i)
				if (idAttribute(*events.GetChild(i), "event") == keptId)
					kept = events.GetChild(i);
		}
		for (int i = events.GetNumChildren() - 1; i >= 0; --i)
			if (events.GetChild(i) != kept)
				events.RemoveChild(events.GetChild(i));
		bool afterKept = kept == nullptr;
		for (const Event& event : m_events)
		{
			if (kept != nullptr && event.id == keptId)
			{
				kept->SetAttribute("part", std::to_string(event.part));
				afterKept = true;
				continue;
			}
			Rml::ElementPtr element = document->CreateElement("div");
			element->SetClass("ape-event", true);
			element->SetClass("inactive", !event.editable);
			element->SetAttribute("part", std::to_string(event.part));
			element->SetAttribute("event", std::to_string(event.id));
			if (afterKept)
				events.AppendChild(std::move(element));
			else
				events.InsertBefore(std::move(element), kept);
		}
	}

	// The info line (p. 698): the first selected event's start, end and length in bars and beats and its volume in dB;
	// .multiple with several events selected.
	void AudioPartEditor::renderInfo()
	{
		Rml::Element* line = m_root.get()->QuerySelector(".ape-info");
		if (line == nullptr)
			return;
		const std::vector<const Event*> events = selectedEvents();
		setClass(line, "empty", events.empty());
		setClass(line, "multiple", events.size() > 1);
		if (events.empty())
			return;
		const Event& first = *events.front();
		const TimeContext time = projectTime(m_project);
		const std::map<std::string, std::string> texts = {
			{"start", formatTime(TimeFormat::BarsBeats, first.start, time)},
			{"end", formatTime(TimeFormat::BarsBeats, first.end, time)},
			{"length", formatLength(TimeFormat::BarsBeats, first.start, first.end, time)},
			{"volume", widgets::format(static_cast<float>(first.volume), 2)}};
		for (const auto& [field, text] : texts)
			if (Rml::Element* display = line->QuerySelector("textdisplay[field=" + field + "]");
				display != nullptr && attribute(*display, "value") != text)
				display->SetAttribute("value", text);
	}

	const AudioPartEditor::Event* AudioPartEditor::eventOf(Rml::Element* element) const
	{
		const Rml::Element* found = withAttribute(element, "event", m_root.get());
		if (found == nullptr)
			return nullptr;
		const uint64_t id = idAttribute(*found, "event");
		for (const Event& each : m_events)
			if (each.id == id)
				return &each;
		return nullptr;
	}

	bool AudioPartEditor::selected(const Event& event) const
	{
		return std::find(m_selection.begin(), m_selection.end(), event.id) != m_selection.end();
	}

	bool AudioPartEditor::continues(const Event& event, const Event& next) const
	{
		return next.id != event.id && next.part == event.part && next.clip == event.clip &&
			next.offset == event.offset + event.length && std::abs(samples(next.start) - samples(event.end)) <= 1;
	}

	std::vector<const AudioPartEditor::Event*> AudioPartEditor::selectedEvents() const
	{
		std::vector<const Event*> events;
		for (const Event& each : m_events)
			if (selected(each))
				events.push_back(&each);
		std::stable_sort(events.begin(), events.end(),
			[](const Event* a, const Event* b) { return std::tie(a->start, a->lane) < std::tie(b->start, b->lane); });
		return events;
	}

	std::pair<double, int> AudioPartEditor::at(const Rml::Event& event) const
	{
		return at(Rml::Vector2f(event.GetParameter("mouse_x", 0.0F), event.GetParameter("mouse_y", 0.0F)));
	}

	std::pair<double, int> AudioPartEditor::at(Rml::Vector2f mouse) const
	{
		Rml::Element* display = m_root.get()->QuerySelector(".ape-display");
		if (display == nullptr)
			return {0.0, 0};
		const Rml::Vector2f origin = display->GetAbsoluteOffset(Rml::BoxArea::Border);
		const float width = std::max(display->GetBox().GetSize(Rml::BoxArea::Border).x, 1.0F);
		const double x = (mouse.x - origin.x) / width;
		const double y = (mouse.y - origin.y) / dpRatio();
		return {m_from + (x * (m_to - m_from)), std::max(0, static_cast<int>(std::floor(y / g_laneHeight)))};
	}

	// To the project's quantize grid (quantize.set), from the project start, while the project's snap is on.
	double AudioPartEditor::snap(double seconds) const
	{
		if (!m_project.value("snap", Json::object()).value("on", false))
			return seconds;
		const TimeContext time = projectTime(m_project);
		const std::string grid = m_project.value("quantize", Json::object()).value("grid", std::string("1/16"));
		const auto step =
			static_cast<double>(engine::midi::noteValueFromString(grid).value_or(engine::midi::NoteValue{}).ticks());
		return time.tempo.toSeconds(
			std::llround(std::round(static_cast<double>(time.tempo.toTicks(seconds)) / step) * step));
	}

	bool AudioPartEditor::dragged(const Rml::Event& event)
	{
		const double dx = (event.GetParameter("mouse_x", 0.0F) - m_pressedMouse.x) / dpRatio();
		const double dy = (event.GetParameter("mouse_y", 0.0F) - m_pressedMouse.y) / dpRatio();
		m_dragged = m_dragged || std::abs(dx) >= g_dragThreshold || std::abs(dy) >= g_dragThreshold;
		return m_dragged;
	}

	int64_t AudioPartEditor::samples(double seconds) const
	{
		return std::llround(seconds * projectTime(m_project).sampleRate);
	}

	void AudioPartEditor::select(std::vector<uint64_t> events)
	{
		m_selection = std::move(events);
		render(); // at once; editor.changed confirms it
		++m_selecting;
		m_binding.send("editor.set", {{"editor", m_name}, {"eventSelection", m_selection}},
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (!result.ok())
					logging::warning("editor.set from the audio part editor failed: {}", result.error);
				if (!alive.expired())
					--m_selecting;
			});
	}

	void AudioPartEditor::edit(const std::string& description, Json commands)
	{
		if (commands.empty())
			return;
		if (commands.size() == 1)
			m_binding.send(commands[0].at("command").get<std::string>(), commands[0].at("parameters"));
		else
			m_binding.send("history.transaction", {{"description", description}, {"commands", std::move(commands)}});
	}

	// Edits on the info line go to every selected event, by as much as the first one's value changes.
	void AudioPartEditor::editInfo(const std::string& field, const std::string& text)
	{
		const std::vector<const Event*> events = selectedEvents();
		if (events.empty())
			return;
		const Event& first = *events.front();
		const TimeContext time = projectTime(m_project);
		Json ids = Json::array();
		for (const Event* each : events)
			ids.push_back(each->id);
		if (field == "volume")
		{
			const std::optional<float> parsed = widgets::parse(text);
			if (!parsed)
				return;
			const double typed = *parsed;
			Json commands = Json::array();
			for (const Event* each : events)
				commands.push_back({{"command", "audioEvent.set"},
					{"parameters",
						{{"event", each->id},
							{"volume", std::clamp(each->volume + typed - first.volume, -144.0, 6.0)}}}});
			edit("Change volume", std::move(commands));
			return;
		}
		std::optional<double> seconds;
		if (field == "length")
		{
			if (const std::optional<double> length = parseLength(TimeFormat::BarsBeats, text, first.start, time))
				seconds = first.start + *length;
		}
		else
		{
			seconds = parseTime(TimeFormat::BarsBeats, text, time);
		}
		if (!seconds)
			return;
		if (field == "start")
			edit("Move",
				Json::array({{{"command", "event.move"},
					{"parameters", {{"events", ids}, {"delta", samples(*seconds) - samples(first.start)}}}}}));
		else
			edit("Resize",
				Json::array({{{"command", "event.resize"},
					{"parameters",
						{{"events", ids}, {"edge", "end"}, {"delta", samples(*seconds) - samples(first.end)}}}}}));
	}

	void AudioPartEditor::ProcessEvent(Rml::Event& event)
	{
		const Rml::Element* target = event.GetTargetElement();
		if (target == nullptr || m_view.empty())
			return;
		switch (event.GetId())
		{
		case Rml::EventId::Mousedown:
			if (event.GetParameter("button", 0) != 0)
				break;
			if (event.GetCurrentElement() == m_listened.get())
			{
				// A left press anywhere in the document while a gesture goes on: the gesture's mouseup never came, as
				// when the mouse went up outside the window. It ends without its effect before anything hears the
				// press; the events go back where they are.
				if (cancel())
					render();
				break;
			}
			press(event);
			break;
		case Rml::EventId::Drag:
			drag(event);
			break;
		case Rml::EventId::Dragend:
			// After the button went up outside the window; in it, the mouseup has ended the gesture already.
			if (m_gesture != Gesture::None)
				release(event);
			break;
		case Rml::EventId::Mouseup:
			if (event.GetParameter("button", 0) == 0)
				release(event);
			break;
		case Rml::EventId::Dblclick:
			doubleClick(event);
			break;
		case Rml::EventId::Click:
			if (target->HasAttribute("ape-tool"))
				if (const std::optional<AudioPartTool> tool = audioPartToolFromName(attribute(*target, "ape-tool")))
					setTool(*tool);
			break;
		case Rml::EventId::Change:
			if (target->HasAttribute("field"))
				editInfo(attribute(*target, "field"), event.GetParameter<Rml::String>("value", ""));
			break;
		case Rml::EventId::Resize:
			render();
			break;
		default:
			break;
		}
	}

	void AudioPartEditor::press(Rml::Event& event)
	{
		// The document's mousedown (ProcessEvent) has ended a gesture whose mouseup never came already.
		m_gesture = Gesture::None;
		begin(event);
		if (m_gesture != Gesture::None)
			listen(true);
	}

	bool AudioPartEditor::cancel()
	{
		listen(false);
		const Gesture gesture = std::exchange(m_gesture, Gesture::None);
		if (gesture == Gesture::Audition)
			m_binding.send("audition.stop");
		return gesture == Gesture::Move || gesture == Gesture::MoveVertical;
	}

	void AudioPartEditor::listen(bool document)
	{
		if (Rml::Element* listened = m_listened.get())
		{
			listened->RemoveEventListener(Rml::EventId::Mouseup, this);
			listened->RemoveEventListener(Rml::EventId::Mousedown, this, true);
		}
		m_listened.reset();
		if (const Rml::Element* root = m_root.get(); document && root != nullptr)
			if (Rml::ElementDocument* owner = root->GetOwnerDocument())
			{
				owner->AddEventListener(Rml::EventId::Mouseup, this);
				// In the capture phase: before the press reaches the root or anything else in the document.
				owner->AddEventListener(Rml::EventId::Mousedown, this, true);
				m_listened = owner->GetObserverPtr();
			}
	}

	void AudioPartEditor::begin(Rml::Event& event)
	{
		Rml::Element* root = m_root.get();
		Rml::Element* target = event.GetTargetElement();
		if (!inside(target, root->QuerySelector(".ape-display")))
			return;
		const bool shift = event.GetParameter("shift_key", 0) != 0;
		const bool ctrl = event.GetParameter("ctrl_key", 0) != 0 || event.GetParameter("meta_key", 0) != 0;
		m_pressedAt = at(event);
		m_pressedMouse = {event.GetParameter("mouse_x", 0.0F), event.GetParameter("mouse_y", 0.0F)};
		m_dragged = false;
		const Event* pressed = eventOf(target);
		if (pressed != nullptr && !pressed->editable)
			pressed = nullptr;
		m_pressed.reset();
		if (pressed != nullptr)
			m_pressed = *pressed;
		const double seconds = m_pressedAt.first;
		switch (m_tool)
		{
		case AudioPartTool::ObjectSelection:
			if (pressed == nullptr)
			{
				if (!shift && !ctrl)
					select({}); // a click in the empty display deselects
				return;
			}
			if (shift)
			{
				std::vector<uint64_t> events = m_selection;
				const auto found = std::find(events.begin(), events.end(), pressed->id);
				if (found != events.end())
					events.erase(found);
				else
					events.push_back(pressed->id);
				select(std::move(events));
				return;
			}
			// With Ctrl, a drag moves across lanes only (p. 699) and a click toggles the selection.
			m_gesture = ctrl ? Gesture::MoveVertical : Gesture::Move;
			if (!ctrl && !selected(*pressed))
				select({pressed->id});
			return;
		case AudioPartTool::Erase:
			if (pressed != nullptr)
				edit(
					"Erase", Json::array({{{"command", "event.remove"}, {"parameters", {{"events", {pressed->id}}}}}}));
			return;
		case AudioPartTool::Split:
		{
			// At the snapped position; on a selected event, every selected event there.
			if (pressed == nullptr)
				return;
			Json ids = Json::array({pressed->id});
			if (selected(*pressed))
			{
				ids = Json::array();
				for (const Event* each : selectedEvents())
					ids.push_back(each->id);
			}
			const double cut = snap(seconds);
			if (cut > pressed->start && cut < pressed->end)
				edit("Split",
					Json::array({{{"command", "event.split"},
						{"parameters", {{"events", ids}, {"positions", {samples(cut)}}}}}}));
			return;
		}
		case AudioPartTool::Glue:
		{
			// Glues the event to the one on its lane that continues it in the clip, as after a split; the engine glues
			// no other audio events in a part, so a click on one that nothing continues does nothing.
			if (pressed == nullptr)
				return;
			const auto next = std::find_if(m_events.begin(), m_events.end(),
				[&](const Event& each) { return each.lane == pressed->lane && continues(*pressed, each); });
			if (next != m_events.end())
				edit("Glue",
					Json::array({{{"command", "event.glue"}, {"parameters", {{"events", {pressed->id, next->id}}}}}}));
			return;
		}
		case AudioPartTool::Mute:
			if (pressed != nullptr)
				edit("Mute",
					Json::array({{{"command", "event.set"},
						{"parameters", {{"events", {pressed->id}}, {"mute", !pressed->mute}}}}}));
			return;
		case AudioPartTool::Play:
		case AudioPartTool::Scrub:
		{
			// Auditions the track of the event pressed, or of the active part, from the mouse until the release
			// (pp. 700-701); the play tool up to the end of that part.
			const auto part =
				m_parts.find(pressed != nullptr ? pressed->part : m_view.value("activePart", uint64_t{0}));
			if (part == m_parts.end())
				return;
			const int64_t position = std::max<int64_t>(0, samples(seconds));
			Json parameters = {{"track", part->second.track}, {"position", position}};
			if (m_tool == AudioPartTool::Play)
			{
				const int64_t end = samples(part->second.end);
				if (end <= position)
					return; // after the part's end
				parameters["end"] = end;
			}
			m_auditioned = part->second.track;
			m_gesture = Gesture::Audition;
			m_binding.send(m_tool == AudioPartTool::Play ? "audition.start" : "audition.scrub", std::move(parameters));
			return;
		}
		}
	}

	void AudioPartEditor::drag(Rml::Event& event)
	{
		if (m_gesture == Gesture::None)
			return;
		if (m_gesture == Gesture::Audition)
		{
			if (m_tool == AudioPartTool::Scrub)
				m_binding.send("audition.scrub",
					{{"track", m_auditioned}, {"position", std::max<int64_t>(0, samples(at(event).first))}});
			return;
		}
		if (!dragged(event) || !m_pressed)
			return; // a click only selects
		m_draggedTo = {event.GetParameter("mouse_x", 0.0F), event.GetParameter("mouse_y", 0.0F)};
		updateMoved();
		preview();
	}

	void AudioPartEditor::preview()
	{
		if (!moving() || !m_dragged || !m_pressed)
			return;
		const auto [dt, dl] = m_moved;
		const Rml::Element* events = m_root.get()->QuerySelector(".ape-events");
		if (events == nullptr)
			return;
		// What release() moves: the selected events, or with Ctrl the pressed one if it isn't selected.
		const bool alone = m_gesture == Gesture::MoveVertical && !selected(*m_pressed);
		for (int i = 0; i < events->GetNumChildren(); ++i)
		{
			Rml::Element* element = events->GetChild(i);
			const Event* shown = eventOf(element);
			if (shown != nullptr && (alone ? shown->id == m_pressed->id : selected(*shown)))
				place(*element, shown->shownStart + dt, shown->shownEnd + dt, std::max(shown->lane + dl, 0));
		}
	}

	void AudioPartEditor::release(Rml::Event& event)
	{
		listen(false);
		const Gesture gesture = std::exchange(m_gesture, Gesture::None);
		if (gesture == Gesture::None)
			return;
		if (gesture == Gesture::Audition)
		{
			m_binding.send("audition.stop");
			return;
		}
		const bool isDrag = dragged(event); // a click only selects, even on an event off the grid
		if (!m_pressed)
			return;
		const auto [seconds, lane] = at(event);
		const double dt = gesture == Gesture::Move && isDrag
			? snap(m_pressed->start + seconds - m_pressedAt.first) - m_pressed->start
			: 0.0;
		const int dl = isDrag ? lane - m_pressedAt.second : 0;
		if (gesture == Gesture::MoveVertical && dl == 0)
		{
			// A click with Ctrl adds the event to the selection or takes it out.
			std::vector<uint64_t> events = m_selection;
			const auto found = std::find(events.begin(), events.end(), m_pressed->id);
			if (found != events.end())
				events.erase(found);
			else
				events.push_back(m_pressed->id);
			select(std::move(events));
			return;
		}
		// The selected events, or with Ctrl the pressed one if it isn't selected: each with its lane.
		std::vector<std::pair<uint64_t, int>> moved;
		for (const Event* each : selectedEvents())
			moved.emplace_back(each->id, each->lane);
		if (gesture == Gesture::MoveVertical && !selected(*m_pressed))
			moved = {{m_pressed->id, m_pressed->lane}};
		Json ids = Json::array();
		std::map<int, Json> lanes; // the events by the lane they go to
		for (const auto& [id, from] : moved)
		{
			ids.push_back(id);
			if (dl != 0)
				lanes[std::max(from + dl, 0)].push_back(id);
		}
		Json commands = Json::array();
		if (samples(dt) != 0)
			commands.push_back({{"command", "event.move"}, {"parameters", {{"events", ids}, {"delta", samples(dt)}}}});
		for (auto& [to, events] : lanes)
			commands.push_back({{"command", "event.set"}, {"parameters", {{"events", events}, {"lane", to}}}});
		edit("Move", std::move(commands));
	}

	// With the object selection tool, a double click on an event opens it in the sample editor.
	void AudioPartEditor::doubleClick(Rml::Event& event)
	{
		if (m_tool != AudioPartTool::ObjectSelection)
			return;
		cancel(); // the second press's move
		if (const Event* pressed = eventOf(event.GetTargetElement()); pressed != nullptr)
			m_binding.send("sampleEditor.open", {{"event", pressed->id}});
	}
}
