// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/sampleeditor.h"

#include "engine/time/tempomap.h"
#include "ui/canvas.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <charconv>
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

		// The Hitpoints section's Create section (p. 638), as event.fromHitpoints names them.
		constexpr std::array<std::pair<const char*, const char*>, 6> g_creates = {{
			{"slices", "Slices"},
			{"groove", "Groove"},
			{"markers", "Markers"},
			{"regions", "Regions"},
			{"events", "Events"},
			{"midiNotes", "MIDI Notes"},
		}};

		// What the editor listens to in its root element, and in its document: drags that leave the root, and the
		// window's size.
		constexpr std::array<Rml::EventId, 4> g_rootEvents = {
			Rml::EventId::Mousedown, Rml::EventId::Click, Rml::EventId::Dblclick, Rml::EventId::Change};
		constexpr std::array<Rml::EventId, 3> g_documentEvents = {
			Rml::EventId::Mousemove, Rml::EventId::Mouseup, Rml::EventId::Resize};
		// And in the capture phase: the keys and the focus leaving a region's field, before the field takes them.
		constexpr std::array<Rml::EventId, 2> g_captureEvents = {Rml::EventId::Keydown, Rml::EventId::Blur};

		constexpr int64_t g_maxPixels = 65536; // media.peaks' limits
		constexpr int64_t g_maxColumnValues = 262144;
		constexpr double g_zoomStep = 2.0;
		constexpr double g_minScale = 1.0 / 16.0; // frames per dp: zoomed in to 16 dp per frame
		constexpr double g_dragThreshold = 2.0;   // dp a hitpoint moves before a press drags it

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

		// A widget's value from the engine, unless the user is dragging it.
		void setValue(Rml::Element* element, double value)
		{
			if (element != nullptr && !element->IsPseudoClassSet("active") &&
				std::abs(element->GetAttribute<double>("value", 0.0) - value) > 1e-6)
				element->SetAttribute("value", value);
		}

		bool modifier(const Rml::Event& event, const char* key)
		{
			return event.GetParameter<int>(key, 0) != 0;
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

		// A whole number, with nothing else but spaces around it.
		std::optional<int64_t> integer(const std::string& text)
		{
			const size_t first = text.find_first_not_of(' ');
			const size_t last = text.find_last_not_of(' ');
			if (first == std::string::npos)
				return std::nullopt;
			int64_t value = 0;
			const char* end = text.data() + last + 1;
			const auto [at, error] = std::from_chars(text.data() + first, end, value);
			if (error != std::errc() || at != end)
				return std::nullopt;
			return value;
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
		std::string creates;
		for (const auto& [create, title] : g_creates)
			creates += std::string(R"(<button class="se-create" create=")") + create + R"(">)" + title + "</button>";
		const auto filter = [](const char* id, const char* label, const char* range)
		{
			return std::string(R"(<div class="se-filter"><span class="se-filter-label">)") + label +
				R"(</span><slider id=")" + id + R"(" )" + range + "/></div>";
		};
		// The Hitpoints section (pp. 637-647), with the options of Convert Hitpoints to MIDI Notes (pp. 646-647).
		const std::string hitpoints = R"(<div class="section-header">Hitpoints</div>)"
									  R"(<button id="se-edit-hitpoints">Edit Hitpoints</button>)" +
			filter("se-threshold", "Threshold", R"(min="-60" max="0" default="-60" step="0.5")") +
			filter("se-intensity", "Intensity", R"(min="0" max="1" default="0" step="0.01")") +
			filter("se-minimum-length", "Minimum Length", R"(min="0" max="1000" default="0" step="1")") +
			R"(<button id="se-remove-hitpoints">Remove All</button><div class="section-header">Create</div>)" +
			creates +
			R"(<div class="se-midi-options off"><div class="section-header">Convert Hitpoints to MIDI Notes</div>)"
			R"(<button class="se-velocity-mode checked" velocity-mode="dynamic">Dynamic Velocity</button>)"
			R"(<button class="se-velocity-mode" velocity-mode="fixed">Fixed Velocity</button>)"
			R"(<div class="se-filter"><span class="se-filter-label">Velocity</span><valuefield id="se-midi-velocity" )"
			R"(min="1" max="127" default="100" value="100" step="1" decimals="0"/></div>)"
			R"(<div class="se-filter"><span class="se-filter-label">Pitch</span><valuefield id="se-midi-pitch" )"
			R"(min="0" max="127" default="36" value="36" step="1" decimals="0"/></div>)"
			R"(<div class="se-filter"><span class="se-filter-label">Length</span><valuefield id="se-midi-length" )"
			R"(min="1" max="64" default="1" value="1" step="1" decimals="0" unit="/16"/></div>)"
			R"(<button id="se-midi-ok">OK</button><button id="se-midi-cancel">Cancel</button></div>)";
		// The Warp section and the Definition's Auto Adjust and audio tempo (pp. 616, 648-656).
		const std::string warpSection =
			R"(<div class="section-header">Warp</div><button id="se-musical">Musical Mode</button>)" +
			std::string(R"(<div class="se-filter"><span class="se-filter-label">Audio Tempo</span>)") +
			R"(<valuefield id="se-audio-tempo" min="1" max="999" default="120" value="120" step="0.01" decimals="2"/>)"
			R"(</div><button id="se-auto-adjust">Auto Adjust</button><button id="se-free-warp">Free Warp</button>)"
			R"(<button id="se-warp-reset">Reset</button>)";
		root.SetInnerRML(R"(<div class="sample-editor empty"><div class="toolbar sample-editor-toolbar">)" + tools +
			R"(<button id="se-zero-crossing">Snap to Zero Crossing</button><div class="spacer"/>)" + layout +
			R"(</div><div class="info-line se-info">)" + info +
			R"(</div><div class="se-overview"><waveform class="se-overview-wave"/><div class="se-overview-event"/>)"
			R"(<div class="se-overview-selection"/><div class="se-overview-snap"/><div class="se-overview-view"/>)"
			R"(</div><div class="se-body"><div class="se-inspector"><div class="section-header">Range</div>)" +
			selects +
			R"(<button id="se-set-locators">Set Locators to Selection Range</button>)"
			R"(<button id="se-locate">Locate Selection</button><button id="se-loop">Loop Selection</button>)"
			R"(<button id="se-create-sampler">Create Sampler Track</button>)"
			R"(<div class="section-header">Process</div>)"
			R"(<button id="se-range-region">Event or Range as Region</button><button id="se-flatten">Flatten</button>)" +
			hitpoints + warpSection +
			R"(</div><div class="se-main"><div class="se-display"><waveform class="se-wave"/>)"
			R"(<div class="se-outside se-before"/><div class="se-outside se-after"/><div class="se-region-shown"/>)"
			R"(<div class="se-selection"/><div class="se-hitpoints"/><div class="se-warp-markers"/>)"
			R"(<div class="se-snap-point"/></div>)"
			R"(<div class="se-regions"><div class="toolbar se-regions-toolbar">)"
			R"(<button id="se-add-region">Add Region</button><button id="se-remove-region">Remove Region</button>)"
			R"(<button id="se-select-region">Select Region</button><button id="se-play-region">Play Region</button>)"
			R"(</div><div class="se-region-list"/></div>)"
			R"(</div></div></div>)");
		for (const Rml::EventId event : g_rootEvents)
			root.AddEventListener(event, this);
		for (const Rml::EventId event : g_documentEvents)
			root.GetOwnerDocument()->AddEventListener(event, this);
		for (const Rml::EventId event : g_captureEvents)
			root.AddEventListener(event, this, true);
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
			for (const Rml::EventId event : g_captureEvents)
				root->RemoveEventListener(event, this, true);
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

	const Json* SampleEditor::clip() const
	{
		if (!m_project.contains("clips"))
			return nullptr;
		for (const Json& each : m_project.at("clips")) // in the project, not a copy
			if (each.at("id") == m_view.value("clip", Json(0)))
				return &each;
		return nullptr;
	}

	double SampleEditor::sampleRate() const
	{
		for (const Json& media : m_project.value("media", Json::array()))
			if (media.at("id") == m_media)
				return media.at("sampleRate").get<double>();
		return 48000.0;
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

	void SampleEditor::reveal(double from, double to)
	{
		const double visible = displayWidth() * m_scale;
		if (!(visible > 0.0) || (from >= m_start && to <= m_start + visible))
			return;
		// In the middle, or from its start if it is longer than the display.
		const double start = to - from < visible ? from - ((visible - (to - from)) / 2.0) : from;
		m_start = std::clamp(start, 0.0, std::max(0.0, static_cast<double>(m_length) - visible));
		render();
		ask(false);
	}

	void SampleEditor::auditionClip(int64_t from, std::optional<int64_t> to)
	{
		Json parameters = {{"media", m_media}, {"position", std::clamp<int64_t>(from, 0, m_length)}};
		if (to)
			parameters["end"] = *to;
		m_binding.send("audition.start", parameters);
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
		// The Hitpoints section: Edit Hitpoints and the clip's filters, the minimum length in milliseconds.
		setClass(root->QuerySelector("#se-edit-hitpoints"), "checked", m_editHitpoints);
		setClass(editor, "edit-hitpoints", m_editHitpoints);
		if (const Json* item = hasEvent() ? clip() : nullptr; item != nullptr && item->contains("hitpointFilter"))
		{
			const Json& filter = item->at("hitpointFilter");
			setValue(root->QuerySelector("#se-threshold"), filter.at("threshold").get<double>());
			setValue(root->QuerySelector("#se-intensity"), filter.at("intensity").get<double>());
			setValue(root->QuerySelector("#se-minimum-length"),
				std::round(filter.at("minimumLength").get<double>() * 1000.0 / sampleRate()));
		}

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
			renderHitpoints();
			renderWarp();
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
		renderHitpoints();
		renderWarp();
	}

	// The Warp section's switches and the audio tempo, and the clip's warp markers in the display: a line each
	// where its audio plays, with a handle at the bottom. The markers are rebuilt only when they change.
	void SampleEditor::renderWarp()
	{
		Rml::Element* root = m_root.get();
		Rml::Element* editor = root != nullptr ? root->QuerySelector(".sample-editor") : nullptr;
		if (editor == nullptr)
			return;
		setClass(editor, "free-warp", m_freeWarp);
		setClass(root->QuerySelector("#se-free-warp"), "checked", m_freeWarp);
		const Json* item = hasEvent() ? clip() : nullptr;
		const Json warp = item != nullptr ? item->value("warp", Json::object()) : Json::object();
		setClass(root->QuerySelector("#se-musical"), "checked", warp.value("musical", false));
		if (warp.value("tempo", 0.0) > 0.0)
			setValue(root->QuerySelector("#se-audio-tempo"), warp.at("tempo").get<double>());
		Rml::Element* shown = root->QuerySelector(".se-warp-markers");
		if (shown == nullptr)
			return;
		std::string rml;
		if (m_scale > 0.0)
			for (const Json& marker : warp.value("markers", Json::array()))
				rml += R"(<div class="se-warp-marker" marker=")" +
					std::to_string(marker.at("position").get<int64_t>()) + R"(" target=")" +
					std::to_string(marker.at("target").get<int64_t>()) +
					R"("><div class="se-warp-marker-line"/><div class="se-warp-marker-handle"/></div>)";
		if (rml != m_markerRows)
		{
			shown->SetInnerRML(rml);
			m_markerRows = rml;
		}
		for (int i = 0; i < shown->GetNumChildren(); ++i)
		{
			Rml::Element* marker = shown->GetChild(i);
			const auto position = std::stoll(attribute(*marker, "marker"));
			const double frame = m_drag == Drag::WarpMarker && position == m_marker
				? m_dragged
				: static_cast<double>(std::stoll(attribute(*marker, "target")));
			marker->SetProperty(Rml::PropertyId::Left, dp((frame - m_start) / m_scale));
		}
	}

	// The Warp section's buttons, Auto Adjust and Flatten (pp. 649-657). Auto Adjust takes the selection range,
	// or else the event.
	bool SampleEditor::clickWarp(const std::string& id)
	{
		if (!hasEvent())
			return false;
		const Json* item = clip();
		if (id == "se-musical")
		{
			const bool musical = item != nullptr && item->value("warp", Json::object()).value("musical", false);
			m_binding.send("clip.setWarp", {{"clip", m_view.at("clip")}, {"musical", !musical}});
		}
		else if (id == "se-auto-adjust")
		{
			const auto [from, to] = rangeOrEvent();
			m_binding.send(
				"clip.autoAdjust", {{"clip", m_view.at("clip")}, {"event", m_event}, {"start", from}, {"end", to}});
		}
		else if (id == "se-free-warp")
		{
			m_freeWarp = !m_freeWarp;
			m_editHitpoints = m_editHitpoints && !m_freeWarp;
			render();
		}
		else if (id == "se-warp-reset")
		{
			m_binding.send("clip.removeWarpMarkers", {{"clip", m_view.at("clip")}});
		}
		else if (id == "se-flatten")
		{
			m_binding.send("event.flatten", {{"events", Json::array({m_event})}});
		}
		else
		{
			return false;
		}
		return true;
	}

	// Free Warp's gestures in the display (pp. 654-656): a drag from anywhere inserts a warp marker there and moves it
	// to where it ends, a drag of a marker moves it, and a click on one with a modifier key removes it.
	void SampleEditor::pressWarp(Rml::Event& event, Rml::Element& target)
	{
		m_marker = -1;
		if (const Rml::Element* marker = withClass(&target, "se-warp-marker", m_root.get()))
		{
			const auto position = std::stoll(attribute(*marker, "marker"));
			if (event.GetParameter<int>("alt_key", 0) != 0 || event.GetParameter<int>("ctrl_key", 0) != 0 ||
				event.GetParameter<int>("shift_key", 0) != 0 || event.GetParameter<int>("meta_key", 0) != 0)
			{
				m_binding.send(
					"clip.removeWarpMarkers", {{"clip", m_view.at("clip")}, {"positions", Json::array({position})}});
				return;
			}
			m_marker = position;
			m_pressed = m_dragged = static_cast<double>(std::stoll(attribute(*marker, "target")));
		}
		m_drag = Drag::WarpMarker;
		render();
	}

	// The hitpoints that count, and the disabled ones (pp. 636-640): a line each, with its handle at the top,
	// .locked and .disabled. The elements are rebuilt only when they change.
	void SampleEditor::renderHitpoints()
	{
		Rml::Element* shown = m_root ? m_root->QuerySelector(".se-hitpoints") : nullptr;
		if (shown == nullptr)
			return;
		const Json* item = hasEvent() && m_scale > 0.0 ? clip() : nullptr;
		std::string rml;
		if (item != nullptr)
			for (const Json& hitpoint : item->value("hitpoints", Json::array()))
			{
				const bool locked = hitpoint.at("locked").get<bool>();
				const bool disabled = hitpoint.at("disabled").get<bool>();
				if (hitpoint.at("active").get<bool>() || disabled)
					rml += std::string(R"(<div class="se-hitpoint)") + (locked ? " locked" : "") +
						(disabled ? " disabled" : "") + R"(" hitpoint=")" +
						std::to_string(hitpoint.at("position").get<int64_t>()) +
						R"("><div class="se-hitpoint-line"/><div class="se-hitpoint-handle"/></div>)";
			}
		if (rml != m_hitpointRows)
		{
			shown->SetInnerRML(rml);
			m_hitpointRows = rml;
		}
		for (int i = 0; i < shown->GetNumChildren(); ++i)
		{
			Rml::Element* hitpoint = shown->GetChild(i);
			const auto position = std::stoll(attribute(*hitpoint, "hitpoint"));
			const double frame =
				m_drag == Drag::Hitpoint && position == m_hitpoint ? m_dragged : static_cast<double>(position);
			hitpoint->SetProperty(Rml::PropertyId::Left, dp((frame - m_start) / m_scale));
		}
	}

	// The regions list (p. 624): a row per region of the clip with its name, start and end, the selected one
	// .selected. The rows are rebuilt only when the regions change, so that a double click's second press lands on
	// the row its first one selected: RmlUi sends dblclick only then.
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
					rml += R"(<div class="se-region" region=")" + std::to_string(id) +
						R"("><span class="se-region-name" field="name">)" +
						Rml::StringUtilities::EncodeRml(each.at("name").get<std::string>()) +
						R"(</span><span class="se-region-start" field="start">)" +
						std::to_string(each.at("start").get<int64_t>()) +
						R"(</span><span class="se-region-end" field="end">)" +
						std::to_string(each.at("end").get<int64_t>()) + "</span></div>";
				}
		if (rml != m_regionRows)
		{
			// The regions changed meanwhile, such as by an undo: what is being typed goes, before its field does,
			// whose blur would otherwise take it.
			finishEditing(false);
			list->SetInnerRML(rml);
			m_regionRows = rml;
		}
		if (!m_editing)
		{
			// The field of the last edit, hidden since; not from its own events, which this never runs in.
			m_editedRegion = 0;
			Rml::ElementList fields;
			list->QuerySelectorAll(fields, ".se-region-field");
			for (Rml::Element* field : fields)
				field->GetParentNode()->RemoveChild(field);
		}
		for (int i = 0; i < list->GetNumChildren(); ++i)
		{
			Rml::Element* row = list->GetChild(i);
			setClass(row, "selected", attribute(*row, "region") == std::to_string(m_region));
		}
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
			// A double click on a region's name, start or end types a new one (p. 625).
			if (Rml::Element* cell = withAttribute(target, "field", m_root.get());
				cell != nullptr && cell->GetParentNode() != nullptr && cell->GetParentNode()->HasAttribute("region"))
				editRegion(*cell);
			break;
		case Rml::EventId::Keydown:
			if (m_editing && target == m_editing.get())
			{
				const auto key = event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN);
				if (key == Rml::Input::KI_RETURN || key == Rml::Input::KI_NUMPADENTER || key == Rml::Input::KI_ESCAPE)
				{
					finishEditing(key != Rml::Input::KI_ESCAPE);
					event.StopPropagation(); // not a key command
				}
			}
			break;
		case Rml::EventId::Blur:
			if (m_editing && target == m_editing.get())
				finishEditing(true);
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
		case Rml::EventId::Change:
			changed(event, *target);
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
			// A click on a region shows it in the display (p. 625).
			m_region = std::stoull(attribute(*row, "region"));
			render();
			if (const Json* shown = region(m_region))
				reveal(shown->at("start").get<double>(), shown->at("end").get<double>());
			return;
		}
		const Rml::Element* named = withAttribute(&target, "id", root);
		const std::string id = named != nullptr ? attribute(*named, "id") : std::string();
		if (clickWarp(id))
			return;
		if (id == "se-zero-crossing")
		{
			const bool on = m_project.contains("snap") && m_project.at("snap").value("zeroCrossing", false);
			m_binding.send("snap.set", {{"zeroCrossing", !on}});
			return;
		}
		if (!hasEvent())
			return;
		if (clickHitpoints(target, id))
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
			// To the start, and from there to the end (p. 619).
			const int64_t first = std::max<int64_t>(0, projectSample(static_cast<double>(start)));
			const int64_t last = std::max<int64_t>(0, projectSample(static_cast<double>(end)));
			m_binding.send("transport.get", Json::object(),
				[this, alive = std::weak_ptr<bool>(m_alive), first, last](const engine::Result& result)
				{
					if (!alive.expired() && result.ok())
						m_binding.send("transport.locate",
							{{"position", result.value.at("position").get<int64_t>() == first ? last : first}});
				});
			return;
		}
		if (id == "se-loop")
		{
			// Loop Selection: the locators around it, and cycle playback from its start (p. 619).
			const int64_t left = std::max<int64_t>(0, projectSample(static_cast<double>(start)));
			const int64_t right = std::max<int64_t>(left, projectSample(static_cast<double>(end)));
			m_binding.send("transport.set", {{"left", left}, {"right", right}, {"cycle", true}});
			m_binding.send("transport.locate", {{"position", left}});
			m_binding.send("transport.play");
			return;
		}
		if (id == "se-create-sampler")
		{
			// From the selection range, or the whole event without one (reference manual p. 711).
			m_binding.send("sampler.create", {{"event", m_view.at("event")}, {"start", start}, {"end", end}});
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
		else if (id == "se-play-region")
		{
			auditionClip(selected->at("start").get<int64_t>(), selected->at("end").get<int64_t>());
		}
		else if (id == "se-select-region")
		{
			// Selects the region and zooms to it (p. 624).
			m_binding.send(
				"sampleEditor.set", {{"selectionStart", selected->at("start")}, {"selectionEnd", selected->at("end")}});
			fit(selected->at("start").get<double>(), selected->at("end").get<double>());
		}
	}

	// A field in the regions list in place of a region's name, start or end, with what it shows selected, so that
	// typing replaces it. The cell hides while the field shows; finishEditing hides the field, and the next
	// renderRegions removes it.
	void SampleEditor::editRegion(Rml::Element& cell)
	{
		finishEditing(true);
		Rml::Element* row = cell.GetParentNode();
		const std::string field = attribute(cell, "field");
		Rml::XMLAttributes attributes;
		attributes["type"] = "text";
		attributes["class"] = "se-region-field se-region-" + field;
		Rml::Element* input =
			row->InsertBefore(Rml::Factory::InstanceElement(row, "input", "input", attributes), cell.GetNextSibling());
		if (input == nullptr)
			return;
		m_editing = input->GetObserverPtr();
		m_editedRegion = std::stoull(attribute(*row, "region"));
		m_editedField = field;
		m_edited = Rml::StringUtilities::DecodeRml(cell.GetInnerRML());
		input->SetAttribute("value", m_edited);
		setClass(&cell, "off", true);
		input->Focus();
		if (auto* text = dynamic_cast<Rml::ElementFormControlInput*>(input))
			text->Select();
	}

	void SampleEditor::finishEditing(bool keep)
	{
		Rml::Element* field = m_editing.get();
		m_editing.reset();
		if (field == nullptr)
			return;
		const auto typed = field->GetAttribute<Rml::String>("value", m_edited);
		setClass(field, "off", true);
		if (const Rml::Element* row = field->GetParentNode())
			for (int i = 0; i < row->GetNumChildren(); ++i)
				if (row->GetChild(i)->HasAttribute("field"))
					setClass(row->GetChild(i), "off", false);
		if (keep && typed != m_edited && hasEvent())
		{
			Json parameters = {{"clip", m_view.at("clip")}, {"region", m_editedRegion}};
			if (m_editedField == "name" && !typed.empty())
				parameters["name"] = typed;
			else if (const std::optional<int64_t> frame = integer(typed);
				frame && m_editedField != "name" && frame != integer(m_edited))
				parameters[m_editedField] = *frame; // not 0100 for 100: no undo step that changes nothing
			if (parameters.size() > 2)
				m_binding.send("clip.setRegion", parameters); // the engine refuses what isn't a section of the clip
		}
		if (m_root)
			m_root->GetOwnerDocument()->Focus(); // keys are key commands again
		requestView();                           // whose answer renders the list without the field
	}

	// The Hitpoints section's buttons (pp. 637-647). Edit Hitpoints detects the hitpoints of a clip that has none.
	bool SampleEditor::clickHitpoints(Rml::Element& target, const std::string& id)
	{
		Rml::Element* root = m_root.get();
		Rml::Element* options = root->QuerySelector(".se-midi-options");
		if (id == "se-edit-hitpoints")
		{
			m_editHitpoints = !m_editHitpoints;
			const Json* item = clip();
			if (m_editHitpoints && item != nullptr && item->value("hitpoints", Json::array()).empty())
				m_binding.send("clip.detectHitpoints", {{"clip", m_view.at("clip")}});
			render();
			return true;
		}
		if (id == "se-remove-hitpoints")
		{
			m_binding.send("clip.removeHitpoints", {{"clip", m_view.at("clip")}});
			return true;
		}
		if (const Rml::Element* button = withAttribute(&target, "create", root))
		{
			// MIDI Notes asks for its options first (Convert Hitpoints to MIDI Notes, pp. 646-647).
			const std::string create = attribute(*button, "create");
			if (create == "midiNotes")
				setClass(options, "off", false);
			else
				m_binding.send("event.fromHitpoints", {{"event", m_event}, {"create", create}});
			return true;
		}
		if (const Rml::Element* button = withAttribute(&target, "velocity-mode", root))
		{
			Rml::ElementList modes;
			root->QuerySelectorAll(modes, ".se-velocity-mode");
			for (Rml::Element* mode : modes)
				setClass(mode, "checked", mode == button);
			return true;
		}
		if (id != "se-midi-ok" && id != "se-midi-cancel")
			return false;
		setClass(options, "off", true);
		if (id == "se-midi-ok")
		{
			const auto number = [root](const char* selector)
			{
				const Rml::Element* field = root->QuerySelector(selector);
				return field != nullptr ? field->GetAttribute<double>("value", 0.0) : 0.0;
			};
			const Rml::Element* mode = root->QuerySelector(".se-velocity-mode.checked");
			m_binding.send("event.fromHitpoints",
				{{"event", m_event}, {"create", "midiNotes"},
					{"velocityMode", mode != nullptr ? attribute(*mode, "velocity-mode") : "dynamic"},
					{"velocity", number("#se-midi-velocity") / 127.0}, {"pitch", std::lround(number("#se-midi-pitch"))},
					{"length", std::llround(number("#se-midi-length")) * (time::g_ticksPerQuarter / 4)}});
		}
		return true;
	}

	// A filter's slider moved: one undo step for each drag (clip.setHitpointFilter), the minimum length from
	// milliseconds.
	void SampleEditor::changed(Rml::Event& event, Rml::Element& target)
	{
		const std::string id = attribute(target, "id");
		if (hasEvent() && id == "se-audio-tempo")
		{
			m_binding.send(
				"clip.setWarp", {{"clip", m_view.at("clip")}, {"tempo", event.GetParameter<float>("value", 120.0F)}});
			return;
		}
		if (!hasEvent() || (id != "se-threshold" && id != "se-intensity" && id != "se-minimum-length"))
			return;
		const double value = event.GetParameter<float>("value", 0.0F);
		Json parameters = {{"clip", m_view.at("clip")}};
		if (id == "se-threshold")
			parameters["threshold"] = value;
		else if (id == "se-intensity")
			parameters["intensity"] = value;
		else
			parameters["minimumLength"] = std::llround(value * sampleRate() / 1000.0);
		m_binding.sendStep(event.GetParameter<int>("gesture", 0), "hitpoint filter " + id, "Filter hitpoints",
			Json::array({{{"command", "clip.setHitpointFilter"}, {"parameters", parameters}}}));
	}

	// Edit Hitpoints' gestures in the display (pp. 636, 639-640): a click on a hitpoint's handle locks or unlocks
	// it; on its line, with Shift it disables or enables it, with Ctrl and Alt it resets it, and a drag moves it; Alt
	// and a click elsewhere inserts one, and a click between two plays that slice.
	void SampleEditor::pressHitpoint(Rml::Event& event, Rml::Element& target, double frame)
	{
		const Rml::Element* root = m_root.get();
		const Json clipId = m_view.at("clip");
		if (const Rml::Element* hitpoint = withClass(&target, "se-hitpoint", root))
		{
			const auto position = std::stoll(attribute(*hitpoint, "hitpoint"));
			const Json at = {{"clip", clipId}, {"position", position}};
			Json parameters = at;
			if (withClass(&target, "se-hitpoint-handle", root) != nullptr)
				parameters["locked"] = !hitpoint->IsClassSet("locked");
			else if (modifier(event, "shift_key"))
				parameters["disabled"] = !hitpoint->IsClassSet("disabled");
			else if ((modifier(event, "ctrl_key") || modifier(event, "meta_key")) && modifier(event, "alt_key"))
				parameters.update({{"locked", false}, {"disabled", false}});
			if (parameters != at)
			{
				m_binding.send("clip.setHitpoint", parameters);
				return;
			}
			m_drag = Drag::Hitpoint;
			m_hitpoint = position;
			m_dragged = static_cast<double>(position);
			return;
		}
		if (modifier(event, "alt_key"))
		{
			m_binding.send("clip.addHitpoint", {{"clip", clipId}, {"position", std::llround(frame)}});
			return;
		}
		// Play back Slice: from the hitpoint that counts before to the one after, or the event's start and end. A
		// click plays all of it (p. 639).
		auto from = static_cast<double>(m_offset);
		auto to = static_cast<double>(m_offset + m_eventLength);
		if (const Json* item = clip())
			for (const Json& hitpoint : item->value("hitpoints", Json::array()))
			{
				const auto position = hitpoint.at("position").get<double>();
				if (!hitpoint.at("active").get<bool>())
					continue;
				if (position <= frame && position > from)
					from = position;
				else if (position > frame && position < to)
					to = position;
			}
		const int64_t start = std::max<int64_t>(0, projectSample(from));
		const int64_t end = projectSample(to);
		if (end > start)
			m_binding.send("audition.start", {{"track", m_view.at("track")}, {"position", start}, {"end", end}});
	}

	// Scrubs the clip, and moves the project cursor to where the frame plays, as scrubbing locates (p. 634).
	void SampleEditor::scrub(double frame)
	{
		m_binding.send("audition.scrub", {{"media", m_media}, {"position", std::llround(frame)}});
		m_binding.send("transport.locate", {{"position", std::max<int64_t>(0, projectSample(frame))}});
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
		// The snap point can be dragged with any tool (p. 628).
		if (withClass(&target, "se-snap-point", root) != nullptr)
		{
			m_drag = Drag::SnapPoint;
			return;
		}
		if (m_editHitpoints)
		{
			pressHitpoint(event, target, frame);
			return;
		}
		if (m_freeWarp)
		{
			pressWarp(event, target);
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
			// The clip, also outside the event (p. 630).
			m_drag = Drag::Play;
			auditionClip(std::llround(frame), std::nullopt);
			break;
		case SampleTool::Scrub:
			m_drag = Drag::Scrub;
			scrub(frame);
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
			scrub(m_dragged);
		}
		else
		{
			render(); // the range, the snap point or a hitpoint dragged
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
		if (drag == Drag::Hitpoint)
		{
			// A press without a drag leaves it where it is; moved, it is locked (p. 640).
			const double moved = std::abs(m_dragged - static_cast<double>(m_hitpoint)) / m_scale;
			const auto to = std::clamp<int64_t>(std::llround(m_dragged), 0, m_length - 1);
			if (moved >= g_dragThreshold && to != m_hitpoint)
				m_binding.send(
					"clip.setHitpoint", {{"clip", m_view.at("clip")}, {"position", m_hitpoint}, {"moveTo", to}});
			render();
			return;
		}
		if (drag == Drag::WarpMarker)
		{
			// A press without a drag changes nothing.
			const auto to = std::clamp<int64_t>(std::llround(m_dragged), 1, m_length - 1);
			if (std::abs(m_dragged - m_pressed) / m_scale >= g_dragThreshold)
			{
				if (m_marker < 0)
					m_binding.send("clip.addWarpMarker",
						{{"clip", m_view.at("clip")}, {"position", std::llround(m_pressed)}, {"target", to}});
				else
					m_binding.send(
						"clip.setWarpMarker", {{"clip", m_view.at("clip")}, {"position", m_marker}, {"target", to}});
			}
			render();
			return;
		}
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
