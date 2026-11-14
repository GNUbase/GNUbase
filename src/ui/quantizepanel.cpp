// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/quantizepanel.h"

#include "engine/midi/quantize.h"
#include "engine/midi/quantizesettings.h"
#include "engine/time/signaturemap.h"
#include "engine/time/tempomap.h"
#include "platform/log.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <stdexcept>
#include <utility>

namespace gnubase::ui
{
	using engine::Json;

	namespace
	{
		// A choice of a menu: the value it sets and its text.
		using Choice = std::pair<std::string, std::string>;

		// The grid types (reference manual p. 375): straight note values from 1/1 to 1/128, triplets and dotted ones
		// from 1/2 to 1/64, named as the factory presets are.
		std::vector<Choice> noteValues(bool straightOnly)
		{
			std::vector<Choice> values;
			for (int denominator = 1; denominator <= 128; denominator *= 2)
			{
				const std::string value = "1/" + std::to_string(denominator);
				values.emplace_back(value, value);
				if (!straightOnly && denominator >= 2 && denominator <= 64)
				{
					values.emplace_back(value + "T", value + " Triplet");
					values.emplace_back(value + ".", value + " Dotted");
				}
			}
			return values;
		}

		std::string labelOf(const std::vector<Choice>& choices, const std::string& value)
		{
			for (const auto& [each, label] : choices)
				if (each == value)
					return label;
			return value;
		}

		// Settings in percent, which the engine has from 0 to 1.
		bool percent(const std::string& setting)
		{
			return setting == "strength" || setting == "swing" || setting == "velocityStrength" ||
				setting == "lengthStrength";
		}

		// Ticks as the panel shows them (p. 375: 120 ticks are a 1/16), in the engine's ticks.
		constexpr time::Ticks g_displayTick = time::g_ticksPerQuarter / 4 / time::g_defaultDisplayTicksPerSixteenth;

		std::string encode(const std::string& text)
		{
			return Rml::StringUtilities::EncodeRml(text);
		}

		std::string attribute(const Rml::Element& element, const char* name)
		{
			return element.GetAttribute<Rml::String>(name, "");
		}

		const Rml::Element* withAttribute(const Rml::Element* element, const char* name)
		{
			for (; element != nullptr; element = element->GetParentNode())
				if (element->HasAttribute(name))
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

		// The checked attribute gives toggles and menu items the state class .checked; disabled gives :disabled.
		void setFlag(Rml::Element* element, const char* name, bool set)
		{
			if (element == nullptr || element->HasAttribute(name) == set)
				return;
			if (set)
				element->SetAttribute(name, "");
			else
				element->RemoveAttribute(name);
		}

		void setValue(Rml::Element* element, double value)
		{
			if (element == nullptr)
				return;
			const std::string text = std::to_string(value);
			if (std::abs(element->GetAttribute<double>("value", -1.0) - value) > 1e-9)
				element->SetAttribute("value", text);
		}

		std::string menuItems(const char* attributeName, const std::vector<Choice>& choices, const std::string& current)
		{
			std::string rml;
			for (const auto& [value, label] : choices)
				rml += std::string(R"(<menuitem type="toggle" )") + attributeName + R"(=")" + encode(value) + '"' +
					(value == current ? " checked" : "") + ">" + encode(label) + "</menuitem>";
			return rml;
		}

		std::vector<Choice> tuplets()
		{
			std::vector<Choice> choices{{"0", "Off"}};
			for (int tuplet = 3; tuplet <= 15; ++tuplet)
				choices.emplace_back(std::to_string(tuplet), std::to_string(tuplet));
			return choices;
		}

		std::vector<Choice> withOff(std::vector<Choice> choices)
		{
			choices.insert(choices.begin(), {"off", "Off"});
			return choices;
		}

		// The max. move as the note value it is, if it is one: "off", "1/32" or ticks.
		std::string maxMoveValue(int64_t ticks)
		{
			if (ticks == 0)
				return "off";
			for (const auto& [value, label] : noteValues(false))
				if (const auto note = engine::midi::noteValueFromString(value); note && note->ticks() == ticks)
					return value;
			return std::to_string(ticks) + " ticks";
		}

		std::string field(const char* label, const std::string& control)
		{
			return std::string(R"(<div class="quantize-field"><span class="quantize-label">)") + label + "</span>" +
				control + "</div>";
		}

		std::string number(const char* id, const char* setting, int max, const char* unit, const char* title)
		{
			return std::string(R"(<valuefield id=")") + id + R"(" setting=")" + setting + R"(" min="0" max=")" +
				std::to_string(max) + R"(" step="1" decimals="0" unit=")" + unit + R"(" title=")" + title + R"("/>)";
		}

		// A value field of the panel's own, not a quantize setting: milliseconds for the audio tracks' commands.
		std::string milliseconds(const char* id, int min, int max, int value, const char* title)
		{
			return std::string(R"(<valuefield id=")") + id + R"(" min=")" + std::to_string(min) + R"(" max=")" +
				std::to_string(max) + R"(" value=")" + std::to_string(value) +
				R"(" step="1" decimals="0" unit="ms" title=")" + title + R"("/>)";
		}

		std::string menuButton(const char* id, const char* title)
		{
			return std::string(R"(<button id=")") + id + R"(" menu=")" + id + R"(-menu" title=")" + title + R"("/>)";
		}
	}

	QuantizePanel::QuantizePanel(EngineBinding& binding, Rml::Element& element, Layout layout,
		std::function<void(const std::string&)> run, std::function<void(const std::string&, engine::Json)> audio)
		: m_binding(binding), m_root(element.GetObserverPtr()), m_layout(layout),
		  m_prefix(layout == Layout::Panel ? "quantize" : "toolbar-quantize"),
		  m_presetAttribute(layout == Layout::Panel ? "preset" : "toolbar-preset"), m_run(std::move(run)),
		  m_audio(std::move(audio))
	{
		std::string menus = "<menu id=\"" + m_prefix + "-presets-menu\"/>";
		if (layout == Layout::Toolbar)
		{
			element.SetInnerRML(
				R"(<button id="toolbar-quantize-apply" class="icon-only" action="apply" title="Apply Quantize">)"
				R"(Q</button><button id="toolbar-quantize-presets" menu="toolbar-quantize-presets-menu" )"
				R"(title="Quantize Presets"/><button id="toolbar-quantize-soft" type="toggle" action="soft" )"
				R"(title="Soft Quantize On/Off">Soft</button><button id="toolbar-quantize-warp" type="toggle" )"
				R"(action="warp" fit="spare" title="Warp Quantize On/Off">Warp</button>)"
				R"(<button id="toolbar-quantize-panel" class="icon-only" )"
				R"(action="panel" title="Open Quantize Panel">...</button>)");
		}
		else
		{
			// The grid's settings or the groove's (pp. 374-377), then what both have.
			element.SetInnerRML(R"(<div id="quantize-panel" class="quantize-panel"><div class="quantize-row">)" +
				menuButton("quantize-presets", "Quantize Presets") +
				R"(</div><div class="quantize-row"><input type="text" id="quantize-preset-name" )"
				R"(class="quantize-preset-name" title="Preset Name"/>)"
				R"(<button id="quantize-save-preset" class="icon-only" action="save" title="Save Preset">)"
				R"(<svg class="icon" src="../icons/general-save.svg"/></button>)"
				R"(<button id="quantize-rename-preset" action="rename" title="Rename Preset">Rename</button>)"
				R"(<button id="quantize-remove-preset" class="icon-only" action="remove" title="Remove Preset">)"
				R"(<svg class="icon" src="../icons/general-remove.svg"/></button></div>)"
				R"(<div class="quantize-row"><button id="quantize-soft" type="toggle" action="soft" )"
				R"(title="Soft Quantize">Soft Quantize</button>)" +
				number("quantize-strength", "strength", 100, "%", "Quantize Strength") +
				R"(<button id="quantize-warp" type="toggle" action="warp" title="Warp Quantize On/Off">Warp</button>)"
				R"(</div><div id="quantize-grid-settings" class="quantize-settings">)" +
				field("Grid Type", menuButton("quantize-grid", "Grid Type")) +
				field("Quantize Tuplet", menuButton("quantize-tuplet", "Quantize Tuplet")) +
				field("Quantize Swing", number("quantize-swing", "swing", 100, "%", "Quantize Swing")) +
				field("Catch Range", number("quantize-catch-range", "catchRange", 960, "ticks", "Catch Range")) +
				R"(</div><div id="quantize-groove-settings" class="quantize-settings">)" +
				field("Velocity", number("quantize-velocity", "velocityStrength", 100, "%", "Velocity")) +
				field("Length", number("quantize-length", "lengthStrength", 100, "%", "Length")) +
				field("Pre-Q", menuButton("quantize-pre-quantize", "Pre-Q")) +
				field("Max. Move", menuButton("quantize-max-move", "Max. Move")) +
				field("Orig. Position", number("quantize-origin", "origin", 16777216, "ticks", "Orig. Position")) +
				R"(</div><div class="quantize-settings">)" +
				field("Safe Range", number("quantize-safe-range", "safeRange", 960, "ticks", "Safe Range")) +
				field("Rough Quantize", number("quantize-randomize", "randomize", 960, "ticks", "Rough Quantize")) +
				R"(</div><div id="quantize-grid-display" class="quantize-display"/><div class="quantize-row">)"
				R"(<button id="quantize-auto-apply" type="toggle" title="Auto Apply Quantize">)"
				R"(Auto Apply</button><div class="quantize-spacer"/>)"
				R"(<button id="quantize-reset" action="reset" title="Reset Quantize">Reset</button>)"
				R"(<button id="quantize-apply" class="primary" action="apply" title="Apply Quantize">Apply</button>)"
				// Several audio tracks (pp. 370-372, 378-380): Slice Rules, or with warp quantizing Warp Marker
				// Creation Rules, and Crossfades.
				R"(</div><div id="quantize-tracks" class="quantize-settings">)"
				R"(<div class="quantize-row">Hitpoint Tracks</div><div id="quantize-hitpoint-tracks"/>)" +
				field("Range", milliseconds("quantize-range", 0, 1000, 20, "Range")) +
				field("Offset", milliseconds("quantize-offset", 0, 1000, 0, "Offset")) +
				R"(<div class="quantize-row"><div class="quantize-spacer"/>)"
				R"(<button id="quantize-slice" action="slice" title="Slice">Slice</button>)"
				R"(<button id="quantize-create-markers" action="createWarpMarkers" title="Create">Create</button>)"
				R"(</div>)" +
				field("Crossfade Length", milliseconds("quantize-crossfade-length", 1, 500, 10, "Length")) +
				R"(<div class="quantize-row">)"
				R"(<button id="quantize-nudge-left" class="icon-only" action="nudgeLeft" )"
				R"(title="Nudge Crossfade Left">&lt;</button>)"
				R"(<button id="quantize-nudge-right" class="icon-only" action="nudgeRight" )"
				R"(title="Nudge Crossfade Right">&gt;</button>)"
				R"(<button id="quantize-crossfade-editor" action="crossfadeEditor" title="Open Crossfade Editor">)"
				R"(Editor</button><div class="quantize-spacer"/>)"
				R"(<button id="quantize-crossfade" action="crossfade" title="Crossfade">Crossfade</button>)"
				R"(</div></div></div>)");
			menus += R"(<menu id="quantize-grid-menu">)" + menuItems("grid", noteValues(false), "") +
				R"(</menu><menu id="quantize-tuplet-menu">)" + menuItems("tuplet", tuplets(), "") +
				R"(</menu><menu id="quantize-pre-quantize-menu">)" +
				menuItems("pre-quantize", withOff(noteValues(true)), "") +
				R"(</menu><menu id="quantize-max-move-menu">)" + menuItems("max-move", withOff(noteValues(false)), "") +
				"</menu>";
		}
		Rml::ElementDocument* document = element.GetOwnerDocument();
		const Rml::ElementPtr holder = document->CreateElement("div");
		holder->SetInnerRML(menus);
		while (holder->HasChildNodes())
		{
			Rml::Element* menu = document->AppendChild(holder->RemoveChild(holder->GetFirstChild()));
			menu->AddEventListener(Rml::EventId::Click, this);
			m_menus.push_back(menu->GetObserverPtr());
		}
		element.AddEventListener(Rml::EventId::Click, this);
		element.AddEventListener(Rml::EventId::Change, this);

		m_subscriptions.push_back(
			m_binding.on("quantize.changed", [this](const engine::Event& event) { showSettings(event.data); }));
		m_subscriptions.push_back(m_binding.on("quantize.presetsChanged", [this](const engine::Event&) { request(); }));
		m_subscriptions.push_back(m_binding.on("project.reset", [this](const engine::Event&) { request(); }));
		request();
	}

	QuantizePanel::~QuantizePanel()
	{
		if (Rml::Element* root = m_root.get())
		{
			root->RemoveEventListener(Rml::EventId::Click, this);
			root->RemoveEventListener(Rml::EventId::Change, this);
		}
		for (const Rml::ObserverPtr<Rml::Element>& menu : m_menus)
			if (Rml::Element* element = menu.get())
			{
				element->RemoveEventListener(Rml::EventId::Click, this);
				if (Rml::Element* parent = element->GetParentNode())
					parent->RemoveChild(element);
			}
	}

	void QuantizePanel::request()
	{
		const std::weak_ptr<bool> alive = m_alive;
		m_binding.send("quantize.get", Json::object(),
			[this, alive](const engine::Result& result)
			{
				if (alive.expired() || !result.ok())
					return;
				showSettings(result.value);
			});
		m_binding.send("quantize.presets", Json::object(),
			[this, alive](const engine::Result& result)
			{
				if (alive.expired() || !result.ok())
					return;
				m_presets = result.value.at("presets");
				show();
			});
	}

	void QuantizePanel::ProcessEvent(Rml::Event& event)
	{
		const Rml::Element* target = event.GetTargetElement();
		if (target == nullptr || !m_settings.is_object() || m_settings.empty())
			return;
		if (event == Rml::EventId::Change)
		{
			// A value field's own change has the number; its text field's has the text typed.
			if (target->GetTagName() != "valuefield")
				return;
			if (target->HasAttribute("track"))
			{
				m_priorities[std::stoull(attribute(*target, "track"))] =
					static_cast<int>(std::lround(event.GetParameter<float>("value", 1.0F)));
				return;
			}
			if (!target->HasAttribute("setting"))
				return;
			const std::string setting = attribute(*target, "setting");
			const auto value = static_cast<double>(event.GetParameter<float>("value", 0.0F));
			if (percent(setting))
				set({{"settings", {{setting, value / 100.0}}}});
			else
				set({{"settings", {{setting, static_cast<int64_t>(std::llround(value)) * g_displayTick}}}});
			return;
		}
		if (target->HasAttribute("disabled"))
			return;
		if (const Rml::Element* item = withAttribute(target, m_presetAttribute.c_str()))
		{
			set({{"preset", attribute(*item, m_presetAttribute.c_str())}});
		}
		else if (const Rml::Element* grid = withAttribute(target, "grid"))
		{
			set({{"settings", {{"grid", attribute(*grid, "grid")}}}});
		}
		else if (const Rml::Element* tuplet = withAttribute(target, "tuplet"))
		{
			set({{"settings", {{"tuplet", std::stoi(attribute(*tuplet, "tuplet"))}}}});
		}
		else if (const Rml::Element* pre = withAttribute(target, "pre-quantize"))
		{
			const std::string value = attribute(*pre, "pre-quantize");
			set({{"settings", {{"preQuantize", value == "off" ? Json(nullptr) : Json(value)}}}});
		}
		else if (const Rml::Element* move = withAttribute(target, "max-move"))
		{
			const std::string value = attribute(*move, "max-move");
			const auto note = engine::midi::noteValueFromString(value);
			set({{"settings", {{"maxMove", note ? note->ticks() : 0}}}});
		}
		else if (const Rml::Element* button = withAttribute(target, "action"))
		{
			const std::string action = attribute(*button, "action");
			if (action == "apply")
				m_run("edit.quantize");
			else if (action == "reset")
				m_run("edit.resetQuantize");
			else if (action == "panel")
				m_run("window.quantizePanel");
			else if (action == "crossfadeEditor")
				m_run("audio.crossfadeEditor");
			else if (action == "soft") // the toggle's state before this click: m_settings may not have caught up yet
				set({{"settings", {{"soft", !button->HasAttribute("checked")}}}});
			else if (action == "warp")
				set({{"settings", {{"warpQuantize", !button->HasAttribute("checked")}}}});
			else if (m_audio && action == "slice")
				m_audio("quantize.slice",
					{{"range", fieldValue("quantize-range")}, {"offset", fieldValue("quantize-offset")},
						{"priorities", priorities()}});
			else if (m_audio && action == "createWarpMarkers")
				m_audio("quantize.createWarpMarkers",
					{{"range", fieldValue("quantize-range")}, {"priorities", priorities()}});
			else if (m_audio && action == "crossfade")
				m_audio("quantize.crossfade", {{"length", fieldValue("quantize-crossfade-length")}});
			else if (m_audio && (action == "nudgeLeft" || action == "nudgeRight"))
				m_audio("quantize.nudgeCrossfade", {{"amount", action == "nudgeLeft" ? -1.0 : 1.0}});
			else if (action == "save" || action == "rename")
				savePreset(action == "rename");
			else if (action == "remove" && isUserPreset(currentPreset()))
				m_binding.send("quantize.removePreset", {{"name", currentPreset()}});
		}
	}

	void QuantizePanel::set(Json parameters)
	{
		m_binding.send("quantize.set", std::move(parameters));
		// Auto Apply is the toggle's own state: it changes nothing else.
		const Rml::Element* autoApply = m_layout == Layout::Panel ? find("quantize-auto-apply") : nullptr;
		if (autoApply != nullptr && autoApply->HasAttribute("checked"))
			m_run("edit.quantize");
	}

	void QuantizePanel::savePreset(bool rename)
	{
		const std::string name = presetName();
		if (name.empty())
			return;
		if (!rename)
			m_binding.send("quantize.savePreset", {{"name", name}});
		else if (isUserPreset(currentPreset()))
			m_binding.send("quantize.renamePreset", {{"name", currentPreset()}, {"newName", name}});
	}

	void QuantizePanel::showHitpointTracks(const Json& tracks)
	{
		m_hitpointTracks = tracks;
		std::string rml;
		for (const Json& track : tracks)
		{
			const auto id = track.at("track").get<uint64_t>();
			const auto found = m_priorities.find(id);
			rml += R"(<div class="quantize-field"><span class="quantize-label">)" +
				encode(track.at("name").get<std::string>()) +
				R"(</span><valuefield class="quantize-priority" track=")" + std::to_string(id) +
				R"(" min="0" max="5" step="1" decimals="0" value=")" +
				std::to_string(found != m_priorities.end() ? found->second : 1) + R"(" title="Priority"/></div>)";
		}
		setInner(find("quantize-hitpoint-tracks"), rml);
	}

	Json QuantizePanel::priorities() const
	{
		Json list = Json::array();
		for (const Json& track : m_hitpointTracks)
		{
			const auto id = track.at("track").get<uint64_t>();
			const auto found = m_priorities.find(id);
			list.push_back({{"track", id}, {"priority", found != m_priorities.end() ? found->second : 1}});
		}
		return list;
	}

	double QuantizePanel::fieldValue(const std::string& id) const
	{
		const Rml::Element* field = find(id);
		return field != nullptr ? field->GetAttribute<double>("value", 0.0) : 0.0;
	}

	std::string QuantizePanel::presetName() const
	{
		auto* field = dynamic_cast<Rml::ElementFormControl*>(find("quantize-preset-name"));
		return field != nullptr ? field->GetValue() : std::string();
	}

	void QuantizePanel::showSettings(const Json& data)
	{
		m_settings = data.at("settings");
		const auto preset = data.find("preset");
		m_preset = preset != data.end() && preset->is_string() ? preset->get<std::string>() : std::string();
		show();
	}

	std::string QuantizePanel::currentPreset() const
	{
		// Among presets with equal settings, such as a user preset saved from a factory preset's, the engine picks
		// the one every client shows (#770).
		return m_preset;
	}

	bool QuantizePanel::isUserPreset(const std::string& name) const
	{
		return std::any_of(m_presets.begin(), m_presets.end(),
			[&name](const Json& preset) { return preset.at("name") == name && !preset.at("factory").get<bool>(); });
	}

	Rml::Element* QuantizePanel::find(const std::string& id) const
	{
		const Rml::Element* root = m_root.get();
		return root != nullptr ? root->GetOwnerDocument()->GetElementById(id) : nullptr;
	}

	std::string QuantizePanel::presetsMenu() const
	{
		const std::string current = currentPreset();
		std::string factory;
		std::string user;
		for (const Json& preset : m_presets)
		{
			const auto name = preset.at("name").get<std::string>();
			(preset.at("factory").get<bool>() ? factory : user) +=
				menuItems(m_presetAttribute.c_str(), {{name, name}}, current);
		}
		return factory + (user.empty() ? "" : "<menuseparator/>" + user);
	}

	void QuantizePanel::show()
	{
		if (m_root.get() == nullptr || !m_settings.is_object() || m_settings.empty())
			return;
		const std::string current = currentPreset();
		setInner(find(m_prefix + "-presets"), encode(current.empty() ? "No Preset" : current));
		if (!m_menus.empty())
			setInner(m_menus.front().get(), presetsMenu());
		const bool soft = m_settings.value("soft", false);
		setFlag(find(m_prefix + "-soft"), "checked", soft);
		const bool warp = m_settings.value("warpQuantize", false);
		setFlag(find(m_prefix + "-warp"), "checked", warp);
		if (m_layout == Layout::Toolbar)
			return;
		// Warp quantizing several tracks creates warp markers instead of slicing them (p. 378).
		setClass(find("quantize-slice"), "hidden", warp);
		setClass(find("quantize-create-markers"), "hidden", !warp);

		const bool groove = m_settings.at("groove").is_object();
		setClass(find("quantize-grid-settings"), "hidden", groove);
		setClass(find("quantize-groove-settings"), "hidden", !groove);
		const std::string grid = m_settings.at("grid").get<std::string>();
		const int tuplet = m_settings.at("tuplet").get<int>();
		setInner(find("quantize-grid"), encode(labelOf(noteValues(false), grid)));
		setInner(find("quantize-tuplet"), labelOf(tuplets(), std::to_string(tuplet)));
		const Json& pre = m_settings.at("preQuantize");
		setInner(find("quantize-pre-quantize"), pre.is_null() ? "Off" : encode(pre.get<std::string>()));
		const std::string maxMove = maxMoveValue(m_settings.at("maxMove").get<int64_t>());
		setInner(find("quantize-max-move"), encode(labelOf(withOff(noteValues(false)), maxMove)));
		// Swing on straight grids without tuplets only (p. 375).
		const bool straight = grid.back() != 'T' && grid.back() != '.';
		setFlag(find("quantize-swing"), "disabled", !straight || tuplet != 0);
		for (const char* id : {"quantize-strength", "quantize-swing", "quantize-catch-range", "quantize-velocity",
				 "quantize-length", "quantize-origin", "quantize-safe-range", "quantize-randomize"})
			if (Rml::Element* element = find(id))
			{
				const std::string setting = attribute(*element, "setting");
				const double value = m_settings.at(setting).get<double>();
				setValue(element,
					percent(setting) ? std::round(value * 100.0)
									 : std::round(value / static_cast<double>(g_displayTick)));
			}
		const bool user = isUserPreset(current);
		setFlag(find("quantize-rename-preset"), "disabled", !user);
		setFlag(find("quantize-remove-preset"), "disabled", !user);
		// The menus check what is chosen.
		for (const Rml::ObserverPtr<Rml::Element>& menu : m_menus)
			if (const Rml::Element* element = menu.get())
				for (int i = 0; i < element->GetNumChildren(); ++i)
				{
					Rml::Element* item = element->GetChild(i);
					if (item->HasAttribute("grid"))
						setFlag(item, "checked", attribute(*item, "grid") == grid);
					else if (item->HasAttribute("tuplet"))
						setFlag(item, "checked", attribute(*item, "tuplet") == std::to_string(tuplet));
					else if (item->HasAttribute("pre-quantize"))
						setFlag(item, "checked",
							attribute(*item, "pre-quantize") == (pre.is_null() ? "off" : pre.get<std::string>()));
					else if (item->HasAttribute("max-move"))
						setFlag(item, "checked", attribute(*item, "max-move") == maxMove);
				}
		showGrid();
	}

	// The grid display (p. 375): a line at each position of a bar of 4/4, and the catch range around it.
	void QuantizePanel::showGrid()
	{
		Rml::Element* display = find("quantize-grid-display");
		if (display == nullptr)
			return;
		std::vector<time::Ticks> positions;
		const time::SignatureMap signatures;
		const time::Ticks bar = signatures.barStart(1);
		time::Ticks catchRange = 0;
		try
		{
			const engine::midi::QuantizeSettings settings = engine::midi::quantizeSettingsFromJson(m_settings);
			catchRange = settings.groove ? 0 : settings.catchRange;
			const engine::midi::QuantizeGrid grid(settings, signatures);
			time::Ticks at = grid.nearest(0).position;
			if (at < 0)
				at = grid.next(at);
			for (int i = 0; at < bar && i < 1024; ++i)
			{
				positions.push_back(at);
				at = grid.next(at);
			}
		}
		catch (const std::invalid_argument& error)
		{
			logging::warning("The quantize panel cannot show the settings: {}", error.what());
		}
		// The catch ranges, then the lines over them, placed in percent of the bar.
		std::string rml;
		for (size_t i = 0; catchRange > 0 && i < positions.size(); ++i)
			rml += R"(<div class="quantize-catch"/>)";
		for (size_t i = 0; i < positions.size(); ++i)
			rml += R"(<div class="quantize-line"/>)";
		setInner(display, rml);
		const auto percentOf = [bar](time::Ticks ticks)
		{ return Rml::Property(100.0F * static_cast<float>(ticks) / static_cast<float>(bar), Rml::Unit::PERCENT); };
		const size_t ranges = catchRange > 0 ? positions.size() : 0;
		for (size_t i = 0; i < positions.size(); ++i)
		{
			if (ranges > 0)
			{
				Rml::Element* range = display->GetChild(static_cast<int>(i));
				range->SetProperty(Rml::PropertyId::Left, percentOf(positions[i] - catchRange));
				range->SetProperty(Rml::PropertyId::Width, percentOf(2 * catchRange));
			}
			display->GetChild(static_cast<int>(ranges + i))
				->SetProperty(Rml::PropertyId::Left, percentOf(positions[i]));
		}
	}
}
