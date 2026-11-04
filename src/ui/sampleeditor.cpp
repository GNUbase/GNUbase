// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/sampleeditor.h"

#include "ui/canvas.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace gnubase::ui
{
	using engine::Json;

	namespace
	{
		constexpr std::array<std::pair<SampleTool, const char*>, 4> g_tools = {{
			{SampleTool::Range, "range"},
			{SampleTool::Zoom, "zoom"},
			{SampleTool::Play, "play"},
			{SampleTool::Scrub, "scrub"},
		}};
		constexpr std::array<const char*, 4> g_toolTitles = {"Range Selection", "Zoom", "Play", "Scrub"};

		// Set up Window Layout's parts (p. 614): the class the root gets while one is hidden.
		constexpr std::array<std::pair<const char*, const char*>, 3> g_layout = {{
			{"info", "Info Line"},
			{"overview", "Overview"},
			{"regions", "Regions"},
		}};

		// The Range section's Select menu (p. 618), as sampleEditor.set's select names them.
		constexpr std::array<std::pair<const char*, const char*>, 4> g_selects = {{
			{"all", "Select All"},
			{"none", "Select None"},
			{"loop", "Select in Loop"},
			{"event", "Select Event"},
		}};

		// What the editor listens to in its root element, and in its document: drags that leave the root, and the
		// window's size.
		constexpr std::array<Rml::EventId, 3> g_rootEvents = {
			Rml::EventId::Mousedown, Rml::EventId::Click, Rml::EventId::Dblclick};
		constexpr std::array<Rml::EventId, 3> g_documentEvents = {
			Rml::EventId::Mousemove, Rml::EventId::Mouseup, Rml::EventId::Resize};

		constexpr int64_t g_maxPixels = 65536; // media.peaks' limits
		constexpr int64_t g_maxColumnValues = 262144;
		constexpr double g_zoomStep = 2.0;
		constexpr double g_minScale = 1.0 / 16.0; // frames per dp: zoomed in to 16 dp per frame

		std::string attribute(const Rml::Element& element, const char* name)
		{
			return element.GetAttribute<Rml::String>(name, "");
		}

		Rml::Element* withAttribute(Rml::Element* element, const char* name, const Rml::Element* stop)
		{
			for (; element != nullptr && element != stop; element = element->GetParentNode())
				if (element->HasAttribute(name))
					return element;
			return nullptr;
		}

		Rml::Element* withClass(Rml::Element* element, const char* name, const Rml::Element* stop)
		{
			for (; element != nullptr && element != stop; element = element->GetParentNode())
				if (element->IsClassSet(name))
					return element;
			return nullptr;
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

		std::string channelsName(int channels)
		{
			if (channels == 1)
				return "Mono";
			if (channels == 2)
				return "Stereo";
			return std::to_string(channels) + " Channels";
		}
	}

	SampleEditor::SampleEditor(EngineBinding& binding, Rml::Element& root)
		: m_binding(binding), m_root(root.GetObserverPtr())
	{
		std::string tools;
		for (size_t i = 0; i < g_tools.size(); ++i)
			tools += std::string(R"(<button class="se-tool" se-tool=")") + g_tools[i].second + R"(">)" +
				g_toolTitles[i] + "</button>";
		std::string layout;
		for (const auto& [part, title] : g_layout)
			layout +=
				std::string(R"(<button class="se-layout checked" layout=")") + part + R"(">)" + title + "</button>";
		std::string info;
		for (const auto& [field, label] : std::vector<std::pair<const char*, const char*>>{{"format", "Format"},
				 {"length", "Length"}, {"selectionStart", "Selection Start"}, {"selectionEnd", "Selection End"},
				 {"selectionLength", "Selection Length"}, {"zoom", "Zoom"}})
			info += std::string(R"(<div class="info-field"><span class="info-label">)") + label +
				R"(</span><span class="display" field=")" + field + R"("/></div>)";
		std::string selects;
		for (const auto& [select, title] : g_selects)
			selects += std::string(R"(<button class="se-select" select=")") + select + R"(">)" + title + "</button>";
		root.SetInnerRML(R"(<div class="sample-editor empty"><div class="toolbar sample-editor-toolbar">)" + tools +
			R"(<button id="se-zero-crossing">Snap to Zero Crossing</button><div class="spacer"/>)" + layout +
			R"(</div><div class="info-line se-info">)" + info +
			R"(</div><div class="se-overview"><waveform class="se-overview-wave"/><div class="se-overview-event"/>)"
			R"(<div class="se-overview-selection"/><div class="se-overview-snap"/><div class="se-overview-view"/>)"
			R"(</div><div class="se-body"><div class="se-inspector"><div class="section-header">Range</div>)" +
			selects +
			R"(<button id="se-set-locators">Set Locators to Selection Range</button>)"
			R"(<button id="se-locate">Locate Selection</button>)"
			R"(<div class="section-header">Process</div>)"
			R"(<button id="se-range-region">Event or Range as Region</button></div>)"
			R"(<div class="se-main"><div class="se-display"><waveform class="se-wave"/>)"
			R"(<div class="se-outside se-before"/><div class="se-outside se-after"/><div class="se-region-shown"/>)"
			R"(<div class="se-selection"/><div class="se-snap-point"/></div>)"
			R"(<div class="se-regions"><div class="toolbar se-regions-toolbar">)"
			R"(<button id="se-add-region">Add Region</button><button id="se-remove-region">Remove Region</button>)"
			R"(<button id="se-select-region">Select Region</button></div><div class="se-region-list"/></div>)"
			R"(</div></div></div>)");
		for (const Rml::EventId event : g_rootEvents)
			root.AddEventListener(event, this);
		for (const Rml::EventId event : g_documentEvents)
			root.GetOwnerDocument()->AddEventListener(event, this);
		setTool(SampleTool::Range);

		for (const char* event : {"history.changed", "project.reset", "snap.changed"})
			m_subscriptions.push_back(m_binding.on(event,
				[this, reset = std::string(event) == "project.reset"](const engine::Event&)
				{
					requestProject();
					if (reset)
						requestView(); // a new project closes the editor
				}));
		m_subscriptions.push_back(m_binding.on("sampleEditor.changed",
			[this](const engine::Event& event)
			{
				m_view = event.data;
				show();
			}));
		for (const char* event : {"media.peaksProgress", "media.peaksReady"})
			m_subscriptions.push_back(m_binding.on(event,
				[this](const engine::Event& event)
				{
					if (event.data.at("media") == m_media)
					{
						ask(false);
						ask(true);
					}
				}));
		requestView();
		requestProject();
	}

	SampleEditor::~SampleEditor()
	{
		if (Rml::Element* root = m_root.get())
		{
			for (const Rml::EventId event : g_rootEvents)
				root->RemoveEventListener(event, this);
			for (const Rml::EventId event : g_documentEvents)
				root->GetOwnerDocument()->RemoveEventListener(event, this);
		}
	}

	void SampleEditor::setTool(SampleTool tool)
	{
		m_tool = tool;
		Rml::Element* root = m_root.get();
		if (root == nullptr)
			return;
		Rml::ElementList buttons;
		root->QuerySelectorAll(buttons, ".se-tool");
		const char* name = "range";
		for (const auto& [each, eachName] : g_tools)
			if (each == tool)
				name = eachName;
		for (Rml::Element* button : buttons)
			setClass(button, "checked", attribute(*button, "se-tool") == name);
		if (Rml::Element* editor = root->QuerySelector(".sample-editor"))
			editor->SetAttribute("tool", name);
	}

	// ponytail: reads the whole project after every change, as the MIDI editors do.
	void SampleEditor::requestProject()
	{
		if (m_loading)
		{
			m_stale = true;
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
					requestView(); // the view has the event's new section, or that it is gone
				}
				if (std::exchange(m_stale, false))
					requestProject();
			});
	}

	void SampleEditor::requestView()
	{
		m_binding.send("sampleEditor.get", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok())
					return;
				m_view = result.value;
				show();
			});
	}

	void SampleEditor::show()
	{
		if (m_view.empty() || !m_project.contains("audioEvents"))
			return;
		m_event = m_view.at("event").get<uint64_t>();
		m_media = m_view.at("media").get<uint64_t>();
		m_length = m_view.at("length").get<int64_t>();
		m_offset = m_view.at("offset").get<int64_t>();
		m_eventLength = m_view.at("eventLength").get<int64_t>();
		m_clipStart = m_view.at("clipStart").get<int64_t>();
		m_ratio = m_view.at("ratio").get<double>();
		m_selectionStart = m_view.at("selectionStart").get<int64_t>();
		m_selectionEnd = m_view.at("selectionEnd").get<int64_t>();
		// Without a snap point of its own inside it, the event snaps at its start (p. 626).
		m_snapPoint = static_cast<double>(m_offset);
		for (const Json& event : m_project.at("audioEvents"))
			if (event.at("id") == m_event && event.contains("snapPoint") && !event.at("snapPoint").is_null())
			{
				const auto point = event.at("snapPoint").get<int64_t>();
				if (point >= m_offset && point < m_offset + m_eventLength)
					m_snapPoint = static_cast<double>(point);
			}
		if (hasEvent() && m_fitted != m_event && !(displayWidth() > 0.0) && m_root)
		{
			render();                                     // shows the display
			m_root->GetOwnerDocument()->UpdateDocument(); // lays it out, so that it knows its width
		}
		if (hasEvent() && m_fitted != m_event && displayWidth() > 0.0)
		{
			m_fitted = m_event;
			fit(0.0, static_cast<double>(m_length)); // Zoom Full, as clip-based zoom has it (p. 623)
		}
		render();
		ask(false);
		ask(true);
	}

	float SampleEditor::dpRatio() const
	{
		const Rml::Context* context = m_root ? m_root->GetContext() : nullptr;
		return context == nullptr ? 1.0F : context->GetDensityIndependentPixelRatio();
	}

	double SampleEditor::displayWidth() const
	{
		Rml::Element* display = m_root ? m_root->QuerySelector(".se-display") : nullptr;
		return display != nullptr ? display->GetClientWidth() / dpRatio() : 0.0;
	}

	double SampleEditor::frameAt(const Rml::Event& event) const
	{
		Rml::Element* display = m_root->QuerySelector(".se-display");
		const double x = (event.GetParameter<float>("mouse_x", 0.0F) - display->GetAbsoluteLeft()) / dpRatio();
		return std::clamp(m_start + (x * m_scale), 0.0, static_cast<double>(m_length));
	}

	int64_t SampleEditor::projectSample(double frame) const
	{
		return m_clipStart + std::llround(frame * m_ratio);
	}

	std::pair<int64_t, int64_t> SampleEditor::rangeOrEvent() const
	{
		if (m_selectionStart < m_selectionEnd)
			return {m_selectionStart, m_selectionEnd};
		return {m_offset, m_offset + m_eventLength};
	}

	const Json* SampleEditor::region(uint64_t id) const
	{
		if (!m_project.contains("clips"))
			return nullptr;
		for (const Json& clip : m_project.at("clips")) // in the project, not a copy
			if (clip.at("id") == m_view.value("clip", Json(0)))
				for (const Json& each : clip.at("regions"))
					if (each.at("id") == id)
						return &each;
		return nullptr;
	}

	void SampleEditor::fit(double from, double to)
	{
		const double width = displayWidth();
		if (!(width > 0.0) || !(to > from))
			return;
		m_scale = std::max((to - from) / width, g_minScale);
		m_start = from;
		render();
		ask(false);
	}

	void SampleEditor::zoom(double factor, double around)
	{
		const double width = displayWidth();
		if (!(width > 0.0) || !hasEvent())
			return;
		const double full = static_cast<double>(m_length) / width;
		const double scale = std::clamp(m_scale / factor, g_minScale, std::max(full, g_minScale));
		m_start = std::clamp(around - ((around - m_start) * scale / m_scale), 0.0,
			std::max(0.0, static_cast<double>(m_length) - (width * scale)));
		m_scale = scale;
		render();
		ask(false);
	}

	void SampleEditor::render()
	{
		Rml::Element* root = m_root.get();
		Rml::Element* editor = root != nullptr ? root->QuerySelector(".sample-editor") : nullptr;
		if (editor == nullptr)
			return;
		setClass(editor, "empty", !hasEvent());
		bool zeroCrossing = false;
		if (m_project.contains("snap"))
			zeroCrossing = m_project.at("snap").value("zeroCrossing", false);
		setClass(root->QuerySelector("#se-zero-crossing"), "checked", zeroCrossing);

		// The info line (p. 614): the clip's format and length, the selection range and the zoom, in sample frames.
		const auto field = [root](const char* name, const std::string& text)
		{ setInner(root->QuerySelector(std::string(".se-info [field=") + name + "]"), text); };
		std::string format;
		for (const Json& media : m_project.value("media", Json::array()))
			if (media.at("id") == m_media && hasEvent())
				format = std::to_string(std::lround(media.at("sampleRate").get<double>())) + " Hz, " +
					channelsName(media.at("channels").get<int>());
		field("format", format);
		field("length", hasEvent() ? std::to_string(m_length) : "");
		const bool selection = m_selectionStart < m_selectionEnd;
		field("selectionStart", selection ? std::to_string(m_selectionStart) : "");
		field("selectionEnd", selection ? std::to_string(m_selectionEnd) : "");
		field("selectionLength", selection ? std::to_string(m_selectionEnd - m_selectionStart) : "");
		const double samplesPerPixel = m_scale / dpRatio();
		field("zoom", hasEvent() && m_scale > 0.0 ? Rml::CreateString("%.2f samples/pixel", samplesPerPixel) : "");
		if (!hasEvent() || !(m_scale > 0.0))
		{
			renderRegions();
			return;
		}

		// The overview (p. 615): the whole clip, with the event, the selection, the snap point and what the
		// display shows, in percent of the clip.
		const auto length = static_cast<double>(m_length);
		const auto span = [length](Rml::Element* element, double from, double to)
		{
			if (element == nullptr)
				return;
			element->SetProperty(Rml::PropertyId::Left, percent(from / length * 100.0));
			element->SetProperty(Rml::PropertyId::Width, percent(std::max(0.0, to - from) / length * 100.0));
		};
		span(root->QuerySelector(".se-overview-event"), static_cast<double>(m_offset),
			static_cast<double>(m_offset + m_eventLength));
		span(root->QuerySelector(".se-overview-selection"), static_cast<double>(m_selectionStart),
			static_cast<double>(m_selectionEnd));
		span(root->QuerySelector(".se-overview-view"), m_start, m_start + (displayWidth() * m_scale));
		span(root->QuerySelector(".se-overview-snap"), m_snapPoint, m_snapPoint);
		if (Rml::Element* wave = root->QuerySelector(".se-overview-wave"))
		{
			Rml::Element* overview = root->QuerySelector(".se-overview");
			const double width = overview != nullptr ? overview->GetClientWidth() / dpRatio() : 0.0;
			wave->SetAttribute("start", 0.0);
			wave->SetAttribute("scale", width > 0.0 ? length / width : 1.0);
			wave->SetAttribute("media", m_media);
		}

		// The waveform display (pp. 621-622): the event's edges (Show Clips and Events), the selection range, the
		// region selected in the list and the snap point, in dp from the display's left.
		const auto at = [this](double frame) { return (frame - m_start) / m_scale; };
		const auto place = [&](Rml::Element* element, double from, double to)
		{
			if (element == nullptr)
				return;
			element->SetProperty(Rml::PropertyId::Left, dp(at(from)));
			element->SetProperty(Rml::PropertyId::Width, dp(std::max(0.0, at(to) - at(from))));
		};
		if (Rml::Element* wave = root->QuerySelector(".se-wave"))
		{
			wave->SetAttribute("start", m_start);
			wave->SetAttribute("scale", m_scale);
			wave->SetAttribute("media", m_media);
		}
		place(root->QuerySelector(".se-before"), m_start, static_cast<double>(m_offset));
		place(root->QuerySelector(".se-after"), static_cast<double>(m_offset + m_eventLength),
			std::max(length, m_start + (displayWidth() * m_scale)));
		place(root->QuerySelector(".se-selection"), static_cast<double>(m_selectionStart),
			static_cast<double>(m_selectionEnd));
		setClass(root->QuerySelector(".se-selection"), "off", !selection && m_drag != Drag::Range);
		if (m_drag == Drag::Range)
			place(root->QuerySelector(".se-selection"), std::min(m_pressed, m_dragged), std::max(m_pressed, m_dragged));
		const double snap = m_drag == Drag::SnapPoint ? m_dragged : m_snapPoint;
		place(root->QuerySelector(".se-snap-point"), snap, snap);
		const Json* shown = region(m_region);
		setClass(root->QuerySelector(".se-region-shown"), "off", shown == nullptr);
		if (shown != nullptr)
			place(root->QuerySelector(".se-region-shown"), shown->at("start").get<double>(),
				shown->at("end").get<double>());
		renderRegions();
	}

	// The regions list (p. 624): a row per region of the clip with its name, start and end, the selected one
	// .selected.
	void SampleEditor::renderRegions()
	{
		Rml::Element* list = m_root ? m_root->QuerySelector(".se-region-list") : nullptr;
		if (list == nullptr)
			return;
		std::string rml;
		for (const Json& clip : m_project.value("clips", Json::array()))
			if (hasEvent() && clip.at("id") == m_view.at("clip"))
				for (const Json& each : clip.value("regions", Json::array()))
				{
					const auto id = each.at("id").get<uint64_t>();
					rml += R"(<div class="se-region)" + std::string(id == m_region ? " selected" : "") +
						R"(" region=")" + std::to_string(id) + R"("><span class="se-region-name">)" +
						Rml::StringUtilities::EncodeRml(each.at("name").get<std::string>()) +
						R"(</span><span class="se-region-start">)" + std::to_string(each.at("start").get<int64_t>()) +
						R"(</span><span class="se-region-end">)" + std::to_string(each.at("end").get<int64_t>()) +
						"</span></div>";
				}
		setInner(list, rml);
	}

	// Asks media.peaks for the display's columns, or the overview's of the whole clip; one question of each in
	// flight, and what changed meanwhile is asked once it is answered.
	void SampleEditor::ask(bool overview)
	{
		const auto index = static_cast<size_t>(overview);
		Rml::Element* wave = m_root ? m_root->QuerySelector(overview ? ".se-overview-wave" : ".se-wave") : nullptr;
		if (wave == nullptr || !hasEvent() || !(m_scale > 0.0))
			return;
		if (m_asking[index])
		{
			m_again[index] = true;
			return;
		}
		const double width = wave->GetClientWidth() / dpRatio();
		double from = 0.0;
		auto to = static_cast<double>(m_length);
		if (!overview)
		{
			from = std::max(0.0, std::floor(m_start));
			to = std::min(to, std::ceil(m_start + (width * m_scale)));
		}
		int channels = 1;
		for (const Json& media : m_project.value("media", Json::array()))
			if (media.at("id") == m_media)
				channels = std::max(media.at("channels").get<int>(), 1);
		const auto frames = static_cast<int64_t>(to - from);
		const int64_t pixels = std::clamp<int64_t>(
			std::llround(width * dpRatio() * (to - from) / std::max(to - from, overview ? to : width * m_scale)), 1,
			std::min(g_maxPixels, g_maxColumnValues / channels));
		if (frames < 1 || !(width > 0.0))
			return;
		m_asking[index] = true;
		m_binding.send("media.peaks",
			{{"media", m_media}, {"start", static_cast<int64_t>(from)}, {"frames", frames}, {"pixels", pixels}},
			[this, alive = std::weak_ptr<bool>(m_alive), overview, index, from, to](const engine::Result& result)
			{
				if (alive.expired())
					return;
				m_asking[index] = false;
				Rml::Element* shown =
					m_root ? m_root->QuerySelector(overview ? ".se-overview-wave" : ".se-wave") : nullptr;
				if (auto* waveform = dynamic_cast<Waveform*>(shown); waveform != nullptr && result.ok())
				{
					std::vector<PeakColumns> columns;
					for (const Json& channel : result.value.at("channels"))
						columns.push_back({channel.at("min").get<std::vector<int16_t>>(),
							channel.at("max").get<std::vector<int16_t>>()});
					waveform->setPeaks({from, to - from}, std::move(columns));
				}
				if (std::exchange(m_again[index], false))
					ask(overview);
			});
	}

	void SampleEditor::ProcessEvent(Rml::Event& event)
	{
		Rml::Element* target = event.GetTargetElement();
		if (target == nullptr || !m_root)
			return;
		switch (event.GetId())
		{
		case Rml::EventId::Resize:
			if (hasEvent() && m_fitted != m_event)
			{
				show();
			}
			else
			{
				render();
				ask(false);
				ask(true);
			}
			break;
		case Rml::EventId::Click:
			click(*target);
			break;
		case Rml::EventId::Dblclick:
			// A double click on a region selects it, as Select Region does.
			if (const Rml::Element* row = withAttribute(target, "region", m_root.get()))
			{
				m_region = std::stoull(attribute(*row, "region"));
				click(*m_root->QuerySelector("#se-select-region"));
			}
			break;
		case Rml::EventId::Mousedown:
			press(event, *target);
			break;
		case Rml::EventId::Mousemove:
			move(event);
			break;
		case Rml::EventId::Mouseup:
			release(event);
			break;
		default:
			break;
		}
	}

	void SampleEditor::click(Rml::Element& target)
	{
		Rml::Element* root = m_root.get();
		if (const Rml::Element* button = withAttribute(&target, "se-tool", root))
		{
			for (const auto& [tool, name] : g_tools)
				if (attribute(*button, "se-tool") == name)
					setTool(tool);
			return;
		}
		if (Rml::Element* button = withAttribute(&target, "layout", root))
		{
			const bool shown = !button->IsClassSet("checked");
			setClass(button, "checked", shown);
			setClass(root->QuerySelector(".sample-editor"), ("no-" + attribute(*button, "layout")).c_str(), !shown);
			return;
		}
		if (const Rml::Element* row = withAttribute(&target, "region", root))
		{
			m_region = std::stoull(attribute(*row, "region"));
			render();
			return;
		}
		const Rml::Element* named = withAttribute(&target, "id", root);
		const std::string id = named != nullptr ? attribute(*named, "id") : std::string();
		if (id == "se-zero-crossing")
		{
			const bool on = m_project.contains("snap") && m_project.at("snap").value("zeroCrossing", false);
			m_binding.send("snap.set", {{"zeroCrossing", !on}});
			return;
		}
		if (!hasEvent())
			return;
		const auto [start, end] = rangeOrEvent();
		if (const Rml::Element* button = withAttribute(&target, "select", root))
		{
			m_binding.send("sampleEditor.set", {{"select", attribute(*button, "select")}});
			return;
		}
		if (id == "se-set-locators")
		{
			m_binding.send("transport.set",
				{{"left", projectSample(static_cast<double>(start))},
					{"right", projectSample(static_cast<double>(end))}});
			return;
		}
		if (id == "se-locate")
		{
			m_binding.send(
				"transport.locate", {{"position", std::max<int64_t>(0, projectSample(static_cast<double>(start)))}});
			return;
		}
		if (id == "se-add-region" || id == "se-range-region")
		{
			m_binding.send("clip.addRegion", {{"clip", m_view.at("clip")}, {"start", start}, {"end", end}},
				[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
				{
					if (!alive.expired() && result.ok())
					{
						m_region = result.value.at("region").at("id").get<uint64_t>();
						render();
					}
				});
			return;
		}
		const Json* selected = region(m_region);
		if (selected == nullptr)
			return;
		if (id == "se-remove-region")
		{
			m_binding.send("clip.removeRegion", {{"clip", m_view.at("clip")}, {"region", m_region}});
		}
		else if (id == "se-select-region")
		{
			// Selects the region and zooms to it (p. 624).
			m_binding.send(
				"sampleEditor.set", {{"selectionStart", selected->at("start")}, {"selectionEnd", selected->at("end")}});
			fit(selected->at("start").get<double>(), selected->at("end").get<double>());
		}
	}

	void SampleEditor::press(Rml::Event& event, Rml::Element& target)
	{
		if (!hasEvent() || event.GetParameter<int>("button", 0) != 0)
			return;
		const Rml::Element* root = m_root.get();
		if (withClass(&target, "se-overview", root) != nullptr)
		{
			// A click in the overview shows that part of the clip (p. 615), and a drag moves it.
			m_drag = Drag::Overview;
			move(event);
			return;
		}
		if (withClass(&target, "se-display", root) == nullptr)
			return;
		const double frame = frameAt(event);
		m_pressed = m_dragged = frame;
		const int64_t track = m_view.at("track").get<int64_t>();
		// The snap point can be dragged with any tool (p. 628).
		if (withClass(&target, "se-snap-point", root) != nullptr)
		{
			m_drag = Drag::SnapPoint;
			return;
		}
		switch (m_tool)
		{
		case SampleTool::Range:
			// Shift and a click moves the nearer end of the selection range (p. 635).
			if (event.GetParameter<int>("shift_key", 0) != 0 && m_selectionStart < m_selectionEnd)
				m_pressed = std::abs(frame - static_cast<double>(m_selectionStart)) <
						std::abs(frame - static_cast<double>(m_selectionEnd))
					? static_cast<double>(m_selectionEnd)
					: static_cast<double>(m_selectionStart);
			m_drag = Drag::Range;
			render();
			break;
		case SampleTool::Zoom:
			// Zooms in, with Alt out (p. 630).
			zoom(event.GetParameter<int>("alt_key", 0) != 0 ? 1.0 / g_zoomStep : g_zoomStep, frame);
			break;
		case SampleTool::Play:
			m_drag = Drag::Play;
			m_binding.send(
				"audition.start", {{"track", track}, {"position", std::max<int64_t>(0, projectSample(frame))}});
			break;
		case SampleTool::Scrub:
			m_drag = Drag::Scrub;
			m_binding.send(
				"audition.scrub", {{"track", track}, {"position", std::max<int64_t>(0, projectSample(frame))}});
			break;
		}
	}

	void SampleEditor::move(Rml::Event& event)
	{
		if (m_drag == Drag::None || !hasEvent() || !m_root)
			return;
		if (m_drag == Drag::Overview)
		{
			Rml::Element* overview = m_root->QuerySelector(".se-overview");
			const double width = overview != nullptr ? overview->GetClientWidth() : 0.0;
			if (!(width > 0.0))
				return;
			const double x = event.GetParameter<float>("mouse_x", 0.0F) - overview->GetAbsoluteLeft();
			const double visible = displayWidth() * m_scale;
			const double centre = std::clamp(x / width, 0.0, 1.0) * static_cast<double>(m_length);
			m_start = std::clamp(centre - (visible / 2.0), 0.0, std::max(0.0, static_cast<double>(m_length) - visible));
			render();
			ask(false);
			return;
		}
		m_dragged = frameAt(event);
		if (m_drag == Drag::Scrub)
		{
			m_binding.send("audition.scrub",
				{{"track", m_view.at("track")}, {"position", std::max<int64_t>(0, projectSample(m_dragged))}});
		}
		else
		{
			render(); // the range or the snap point dragged
		}
	}

	void SampleEditor::release(Rml::Event& event)
	{
		const Drag drag = std::exchange(m_drag, Drag::None);
		if (drag == Drag::None || !hasEvent() || !m_root)
			return;
		if (drag == Drag::Play || drag == Drag::Scrub)
		{
			m_binding.send("audition.stop");
			return;
		}
		if (drag == Drag::Overview)
			return;
		m_dragged = frameAt(event);
		if (drag == Drag::SnapPoint)
		{
			// Within the event: a snap point outside it counts as none.
			const double frame =
				std::clamp(m_dragged, static_cast<double>(m_offset), static_cast<double>(m_offset + m_eventLength - 1));
			m_binding.send("event.setSnapPoint", {{"events", {m_event}}, {"position", projectSample(frame)}});
			render();
			return;
		}
		const auto from = std::llround(std::min(m_pressed, m_dragged));
		const auto to = std::llround(std::max(m_pressed, m_dragged));
		const bool zeroCrossing = m_project.contains("snap") && m_project.at("snap").value("zeroCrossing", false);
		// A click without a drag selects nothing.
		m_binding.send("sampleEditor.set",
			{{"selectionStart", from == to ? 0 : from}, {"selectionEnd", from == to ? 0 : to},
				{"zeroCrossing", zeroCrossing}});
		render();
	}
}
