// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/inspector.h"

#include "ui/widgetutils.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/StringUtilities.h>

#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		// The track's name above the sections, which the docking system shows, hides and orders (sections="inspector").
		const char* const g_rml =
			R"(<div class="inspector-header"><textdisplay class="display inspector-name" control="name")"
			R"( title="Track Name"/></div>)"
			R"(<div class="inspector-empty">No Track Selected</div>)"
			R"(<div class="inspector-sections" sections="inspector">)"
			R"(<div class="inspector-section" section="basics" label="Basics">)"
			R"(<button class="section-header">Basics</button><div class="section-body">)"
			R"(<div class="inspector-buttons">)"
			R"(<button type="toggle" class="toggle mute" control="mute" title="Mute">M</button>)"
			R"(<button type="toggle" class="toggle solo" control="solo" title="Solo">S</button>)"
			R"(<button type="toggle" class="toggle record" control="record" title="Record Enable">)"
			R"(<svg class="icon" src="../icons/track-record-arm.svg"/></button>)"
			R"(<button type="toggle" class="toggle monitor" control="monitor" title="Monitor">)"
			R"(<svg class="icon" src="../icons/track-monitor.svg"/></button></div>)"
			R"(<div class="inspector-row" row="volume"><span class="inspector-label">Volume</span>)"
			R"(<valuefield control="volume" min="-144" max="6" default="0" step="0.5" decimals="1" unit="dB"/></div>)"
			R"(<div class="inspector-row" row="pan"><span class="inspector-label">Pan</span>)"
			R"(<valuefield control="pan" min="-100" max="100" default="0" step="1" decimals="0"/></div>)"
			R"(</div></div>)"
			R"(<div class="inspector-section" section="routing" label="Routing">)"
			R"(<button class="section-header">Routing</button><div class="section-body">)"
			R"(<div class="inspector-row" row="input"><span class="inspector-label">Input</span>)"
			R"(<select control="input" title="Input Routing"/></div>)"
			R"(<div class="inspector-row" row="output"><span class="inspector-label">Output</span>)"
			R"(<select control="output" title="Output Routing"/></div>)"
			R"(</div></div>)"
			R"(<div class="inspector-section" section="inserts" label="Inserts">)"
			R"(<button class="section-header">Inserts</button><div class="section-body inserts"/></div>)"
			R"(<div class="inspector-section" section="notepad" label="Notepad">)"
			R"(<button class="section-header">Notepad</button><div class="section-body">)"
			R"(<textarea class="notepad" rows="5"/></div></div>)"
			R"(</div>)";

		void setClass(Rml::Element& element, const char* name, bool set)
		{
			if (element.IsClassSet(name) != set)
				element.SetClass(name, set);
		}

		// The object with that id in a list of the project, or null.
		const Json* findIn(const Json* project, const char* list, const Json& id)
		{
			if (project == nullptr || !project->is_object())
				return nullptr;
			for (const Json& object : project->at(list))
				if (object.at("id") == id)
					return &object;
			return nullptr;
		}

		using widgets::Options;

		// Where the channel of a track of the type can play: output busses and group channels, or for MIDI tracks
		// the instrument channels, as in the add track dialog; and where an audio track records from.
		Options outputs(const Json& project, const std::string& type, uint64_t channel)
		{
			Options options;
			if (type == "midi")
			{
				options.emplace_back("0", "Not Connected");
				for (const Json& track : project.at("tracks"))
					if (track.at("type") == "instrument")
						options.emplace_back(std::to_string(track.at("channel").get<uint64_t>()), track.at("name"));
				return options;
			}
			options.emplace_back("0", "No Bus");
			for (const Json& bus : project.at("busses"))
				if (bus.at("direction") == "output")
					options.emplace_back(std::to_string(bus.at("id").get<uint64_t>()), bus.at("name"));
			for (const Json& track : project.at("tracks"))
				if ((track.at("type") == "group" || track.at("type") == "folder") && track.at("channel") != 0 &&
					track.at("channel") != channel)
					options.emplace_back(std::to_string(track.at("channel").get<uint64_t>()), track.at("name"));
			return options;
		}

		Options inputs(const Json& project)
		{
			Options options{{"0", "No Bus"}};
			for (const Json& bus : project.at("busses"))
				if (bus.at("direction") == "input")
					options.emplace_back(std::to_string(bus.at("id").get<uint64_t>()), bus.at("name"));
			return options;
		}

	}

	Inspector::Inspector(EngineBinding& binding, Rml::Element& root)
		: m_binding(binding), m_root(root.GetObserverPtr()),
		  // Another project replaces the edits in progress: the ids they were for may be the new project's too.
		  m_reset(binding.on("project.reset", [this](const engine::Event&) { dropEdits(); }))
	{
		root.SetClass("inspector", true);
		// Set up Sections from an empty area (p. 167), in the project window's document; the controls have none.
		root.SetAttribute("contextmenu", "inspector-setup");
		root.SetInnerRML(g_rml);
		Rml::ElementList controls;
		root.QuerySelectorAll(controls, "[control], textarea, .inserts");
		for (Rml::Element* control : controls)
			control->SetAttribute("contextmenu", "");
		root.AddEventListener(Rml::EventId::Click, this);
		root.AddEventListener(Rml::EventId::Change, this);
		root.AddEventListener(Rml::EventId::Blur, this, true);    // blur doesn't bubble
		root.AddEventListener(Rml::EventId::Keydown, this, true); // Escape leaves the notepad
		show();
	}

	Inspector::~Inspector()
	{
		if (!m_root)
			return;
		m_root->RemoveEventListener(Rml::EventId::Click, this);
		m_root->RemoveEventListener(Rml::EventId::Change, this);
		m_root->RemoveEventListener(Rml::EventId::Blur, this, true);
		m_root->RemoveEventListener(Rml::EventId::Keydown, this, true);
	}

	void Inspector::update(uint64_t track, const Json& project, uint64_t loads)
	{
		m_project = &project;
		if (track == m_track && loads == m_loads && !m_stale)
			return;
		m_stale = false;
		// An edit belongs to the track it started on: leaving the field takes it there before another track shows.
		Rml::Context* context = m_root ? m_root->GetContext() : nullptr;
		if (Rml::Element* focus = context != nullptr ? context->GetFocusElement() : nullptr;
			track != m_track && focus != nullptr && focus != m_root.get() && m_root->Contains(focus))
			focus->Blur();
		m_track = track;
		m_loads = loads;
		show();
	}

	void Inspector::dropEdits()
	{
		Rml::Context* context = m_root ? m_root->GetContext() : nullptr;
		Rml::Element* focus = context != nullptr ? context->GetFocusElement() : nullptr;
		if (focus == nullptr || focus == m_root.get() || !m_root->Contains(focus))
			return;
		m_dropping = true;
		focus->Blur();
		m_dropping = false;
		m_stale = true; // the fields show the channel's values again
	}

	void Inspector::show()
	{
		if (!m_root)
			return;
		Rml::Element& root = *m_root.get();
		const Json* track = m_track == 0 ? nullptr : findIn(m_project, "tracks", m_track);
		setClass(root, "empty", track == nullptr);
		const Json* channel = track == nullptr ? nullptr : findIn(m_project, "channels", track->at("channel"));
		m_channel = channel != nullptr ? *channel : Json();
		if (track == nullptr)
			return;
		m_showing = true;
		const std::string type = track->at("type").get<std::string>();
		const bool vca = channel != nullptr && type == "vca";            // a VCA fader: a fader and buttons
		const bool audio = channel != nullptr && type != "midi" && !vca; // a MIDI channel has no audio
		const auto element = [&root](const std::string& selector) { return root.QuerySelector(selector); };
		const auto available = [&element](const std::string& selector, bool yes)
		{
			if (Rml::Element* found = element(selector))
				setClass(*found, "unavailable", !yes);
		};

		if (Rml::Element* name = element("textdisplay.inspector-name"))
			name->SetAttribute("value", track->at("name").get<std::string>());
		for (const char* section : {"basics", "routing", "notepad"})
			available("[section=" + std::string(section) + "]",
				channel != nullptr && (!vca || std::string(section) != "routing"));
		available("[section=inserts]", audio);
		const bool records = type == "audio" || type == "instrument" || type == "midi" || vca;
		available("button[control=record]", records);
		available("button[control=monitor]", records);
		available("[row=volume]", audio || vca);
		available("[row=pan]", audio);
		available("[row=input]", type == "audio");
		if (channel != nullptr)
		{
			for (const auto& [control, key] : {std::pair{"mute", "mute"}, std::pair{"solo", "solo"},
					 std::pair{"record", "recordEnable"}, std::pair{"monitor", "monitor"}})
				if (Rml::Element* button = element("button[control=" + std::string(control) + "]"))
				{
					if (channel->value(key, false))
						button->SetAttribute("checked", "");
					else
						button->RemoveAttribute("checked");
				}
			if (Rml::Element* volume = element("valuefield[control=volume]"); volume != nullptr && (audio || vca))
			{
				volume->SetAttribute("max", widgets::format(m_project->value("maxVolume", 6.0F)));
				volume->SetAttribute("value", widgets::format(channel->at("volume").get<float>()));
			}
			if (Rml::Element* pan = element("valuefield[control=pan]"); pan != nullptr && audio)
				pan->SetAttribute("value", widgets::format(channel->at("pan").get<float>() * 100.0F));
			const auto id = channel->at("id").get<uint64_t>();
			if (Rml::Element* output = element("select[control=output]"))
				widgets::fill(
					*output, outputs(*m_project, type, id), std::to_string(channel->at("output").get<uint64_t>()));
			if (Rml::Element* input = element("select[control=input]"); input != nullptr && type == "audio")
				widgets::fill(*input, inputs(*m_project), std::to_string(channel->at("input").get<uint64_t>()));

			// The inserts: an overview of the slots, bypassed and deactivated ones marked.
			std::string slots;
			for (const Json& slotId : channel->value("slots", Json::array()))
				if (const Json* slot = findIn(m_project, "slots", slotId))
					slots += std::string(R"(<div class="insert)") + (slot->at("bypass") == true ? " bypassed" : "") +
						(slot->at("active") == false ? " inactive" : "") + R"(">)" +
						Rml::StringUtilities::EncodeRml(slot->at("name").get<std::string>()) + "</div>";
			if (Rml::Element* inserts = element(".inserts"); inserts != nullptr && inserts->GetInnerRML() != slots)
				inserts->SetInnerRML(slots);

			// The notepad, unless the user is typing in it.
			auto* notes = rmlui_dynamic_cast<Rml::ElementFormControlTextArea*>(element("textarea.notepad"));
			Rml::Context* context = root.GetContext();
			if (notes != nullptr && (context == nullptr || context->GetFocusElement() != notes))
				notes->SetValue(channel->value("notes", std::string()));
		}
		m_showing = false;
	}

	void Inspector::ProcessEvent(Rml::Event& event)
	{
		Rml::Element* target = event.GetTargetElement();
		if (target == nullptr)
			return;
		switch (event.GetId())
		{
		case Rml::EventId::Click:
			// A section's header opens and closes it.
			for (const Rml::Element* up = target; up != nullptr && up != m_root.get(); up = up->GetParentNode())
				if (up->IsClassSet("section-header") && up->GetParentNode() != nullptr)
				{
					Rml::Element& section = *up->GetParentNode();
					section.SetClass("collapsed", !section.IsClassSet("collapsed"));
					break;
				}
			break;
		case Rml::EventId::Change:
			if (!m_showing && !m_dropping && target->HasAttribute("control"))
				change(*target, event);
			break;
		case Rml::EventId::Blur:
			if (target->IsClassSet("notepad") && !m_dropping)
				saveNotes();
			break;
		case Rml::EventId::Keydown:
			if (target->IsClassSet("notepad") &&
				event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN) == Rml::Input::KI_ESCAPE)
			{
				target->Blur();
				event.StopPropagation();
			}
			break;
		default:
			break;
		}
	}

	void Inspector::change(Rml::Element& control, const Rml::Event& event)
	{
		const auto name = control.GetAttribute<Rml::String>("control", "");
		if (name == "name")
		{
			m_binding.send("track.set", {{"track", m_track}, {"name", event.GetParameter<Rml::String>("value", "")}});
			return;
		}
		if (!m_channel.is_object())
			return;
		if (name == "volume" || name == "pan")
		{
			// A drag, wheel turns or a held arrow key: one undo step.
			const double number = event.GetParameter<float>("value", 0.0F);
			const Json channel = m_channel.at("id");
			const Json step = {{"command", "channel.set"},
				{"parameters", {{"channel", channel}, {name, name == "volume" ? number : number / 100.0}}}};
			m_binding.sendStep(event.GetParameter<int>("gesture", 0), "channel " + channel.dump() + " " + name,
				name == "volume" ? "Change Volume" : "Change Pan", Json::array({step}), whenRefused());
			return;
		}
		std::string key = name;
		Json value;
		if (name == "mute" || name == "solo" || name == "monitor" || name == "record")
		{
			key = name == "record" ? "recordEnable" : name;
			value = event.GetParameter<bool>("value", false);
		}
		else if (name == "input" || name == "output")
		{
			value = std::strtoull(event.GetParameter<Rml::String>("value", "0").c_str(), nullptr, 10);
		}
		// Not what it is already, such as a drop-down list that show filled.
		if (value.is_null() || m_channel.value(key, Json()) == value)
			return;
		sendChannel({{key, std::move(value)}});
	}

	void Inspector::sendChannel(Json parameters)
	{
		parameters["channel"] = m_channel.at("id");
		m_binding.send("channel.set", std::move(parameters), whenRefused());
	}

	std::function<void(const engine::Result&)> Inspector::whenRefused()
	{
		return [this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
		{
			if (!alive.expired() && !result.ok())
				m_stale = true;
		};
	}

	void Inspector::saveNotes()
	{
		auto* notes = m_root
			? rmlui_dynamic_cast<Rml::ElementFormControlTextArea*>(m_root->QuerySelector("textarea.notepad"))
			: nullptr;
		if (notes == nullptr || !m_channel.is_object() || notes->GetValue() == m_channel.value("notes", std::string()))
			return;
		sendChannel({{"notes", notes->GetValue()}});
	}
}
