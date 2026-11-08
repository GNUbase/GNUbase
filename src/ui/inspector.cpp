// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/inspector.h"

#include "engine/midi/modifiers.h"
#include "engine/time/tempomap.h"
#include "ui/widgetutils.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
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
			R"( title="Track Name"/><button class="inspector-edit" title="Edit Channel Settings">e</button></div>)"
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
			R"(<svg class="icon" src="../icons/track-monitor.svg"/></button>)"
			R"(<button type="toggle" class="toggle timeBase" control="timeBase" title="Switch Time Base">L</button>)"
			R"(<button type="toggle" class="toggle editInPlace" control="editInPlace" title="Edit In-Place">)"
			R"(<svg class="icon" src="../icons/tool-draw.svg"/></button>)"
			R"(</div>)"
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
			R"(<div class="inspector-row" row="device"><span class="inspector-label">Device</span>)"
			R"(<select control="device" title="MIDI Device"/></div>)"
			R"(<div class="inspector-row" row="bank"><span class="inspector-label">Bank</span>)"
			R"(<textdisplay class="display" control="bank" title="Bank Selector"/></div>)"
			R"(<div class="inspector-row" row="program"><span class="inspector-label">Program</span>)"
			R"(<select control="program" title="Program Selector"/></div>)"
			R"(</div></div>)"
			R"(<div class="inspector-section" section="modifiers" label="MIDI Modifiers"/>)"
			R"(<div class="inspector-section" section="inserts" label="Inserts">)"
			R"(<button class="section-header">Inserts</button><div class="section-body inserts"/></div>)"
			R"(<div class="inspector-section" section="notepad" label="Notepad">)"
			R"(<button class="section-header">Notepad</button><div class="section-body">)"
			R"(<textarea class="notepad" rows="5"/></div></div>)"
			R"(</div>)";

		using widgets::Options;

		// The MIDI modifiers (pp. 1099-1102): their controls are named after the channel's modifiers in channel.set,
		// "modifiers-" then the key, separated by dashes, with the index of a pair and the field of a generator or
		// range.
		Options randomTargets()
		{
			return {{"off", "Off"}, {"position", "Position"}, {"pitch", "Pitch"}, {"velocity", "Velocity"},
				{"length", "Length"}};
		}

		Options rangeTargets()
		{
			return {{"off", "Off"}, {"velocityLimit", "Vel. Limit"}, {"velocityFilter", "Vel. Filter"},
				{"noteLimit", "Note Limit"}, {"noteFilter", "Note Filter"}};
		}

		// Random positions and lengths show in ticks of 480 to a quarter note, as the reference's do; the engine's
		// are finer.
		constexpr int64_t g_ticksPerShownTick = time::g_ticksPerQuarter / 480;

		std::string integerField(const std::string& control, int min, int max, int value, const char* title = "")
		{
			return R"(<valuefield control="modifiers-)" + control + R"(" min=")" + std::to_string(min) + R"(" max=")" +
				std::to_string(max) + R"(" default=")" + std::to_string(value) + R"(" step="1" decimals="0" title=")" +
				title + R"("/>)";
		}

		std::string row(const std::string& name, const std::string& label, const std::string& controls)
		{
			return R"(<div class="inspector-row" row=")" + name + R"("><span class="inspector-label">)" + label +
				"</span>" + controls + "</div>";
		}

		std::string modifiersRml()
		{
			std::string rml = R"(<button class="section-header">MIDI Modifiers</button><div class="section-body">)"
							  R"(<div class="inspector-buttons"><button type="toggle" class="toggle bypass")"
							  R"( control="modifiers-bypass" title="Bypass MIDI Modifiers">Bypass</button></div>)";
			rml += row("transpose", "Transpose", integerField("transpose", -127, 127, 0));
			rml += row("velocityShift", "Vel. Shift", integerField("velocityShift", -127, 127, 0));
			rml += row("velocityCompression", "Vel. Comp.",
				integerField("velocityCompression-0", 1, 16, 1, "Numerator") +
					integerField("velocityCompression-1", 1, 16, 1, "Denominator"));
			rml += row("lengthCompression", "Len. Comp.",
				integerField("lengthCompression-0", 1, 16, 1, "Numerator") +
					integerField("lengthCompression-1", 1, 16, 1, "Denominator"));
			for (const char* kind : {"random", "range"})
				for (int i = 0; i < 2; ++i)
				{
					const bool random = kind == std::string("random");
					const std::string name = std::string(kind) + "-" + std::to_string(i);
					const std::string label = std::string(random ? "Random " : "Range ") + std::to_string(i + 1);
					const int low = random ? -127 : 0; // ranges: velocities or pitches
					std::string select = R"(<select control="modifiers-)";
					select += name;
					select += R"(-target" title=")";
					select += label;
					select += R"( Target"/>)";
					rml += row(name, label, select);
					rml += row(name + "-values", "",
						integerField(name + "-minimum", low, 127, 0, "Minimum") +
							integerField(name + "-maximum", low, 127, random ? 0 : 127, "Maximum"));
				}
			return rml + "</div>";
		}

		bool showsTicks(const Json& random)
		{
			return random.at("target") == "position" || random.at("target") == "length";
		}

		// A modifier control's name after "modifiers-": the key, then the index of a pair and a generator's or range's
		// field.
		std::vector<std::string> modifierPath(const std::string& control)
		{
			std::vector<std::string> path;
			size_t from = std::string("modifiers-").size();
			while (from <= control.size())
			{
				const size_t dash = std::min(control.find('-', from), control.size());
				path.push_back(control.substr(from, dash - from));
				from = dash + 1;
			}
			return path;
		}

		void setAttribute(Rml::Element& element, const char* name, const std::string& value)
		{
			if (element.GetAttribute<Rml::String>(name, "") != value)
				element.SetAttribute(name, value);
		}

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

		// Where the channel of a track of the type can play: output busses, group and effect channels, or for MIDI
		// tracks the instrument channels, as in the add track dialog; and where an audio track records from.
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
				if ((track.at("type") == "group" || track.at("type") == "effect" || track.at("type") == "folder") &&
					track.at("channel") != 0 && track.at("channel") != channel)
					options.emplace_back(std::to_string(track.at("channel").get<uint64_t>()), track.at("name"));
			return options;
		}

		// A MIDI track's patch as the program selector's value: "off", or the program and bank, "5:300" ("5:" without a
		// bank).
		std::string patchValue(const Json& program, const Json& bank)
		{
			if (!program.is_number_integer())
				return "off";
			return std::to_string(program.get<int>()) + ":" + (bank.is_number_integer() ? bank.dump() : "");
		}

		// The program selector's choices (reference manual pp. 1108, 1113): a device's patches by name, or without one
		// programs 1 to 128 (keeping the bank), always with Off, and the channel's own patch if no choice has it.
		Options programs(const Json* device, const Json& channel)
		{
			Options options{{"off", "Off"}};
			const Json bank = channel.value("bank", Json());
			if (device == nullptr)
			{
				for (int program = 0; program < 128; ++program)
					options.emplace_back(patchValue(program, bank), std::to_string(program + 1));
				return options;
			}
			const Json& banks = device->at("banks");
			for (const Json& deviceBank : banks)
				for (const Json& patch : deviceBank.at("patches"))
				{
					std::string name;
					for (const std::string& part : {banks.size() > 1 ? deviceBank.at("name").get<std::string>() : "",
							 patch.value("folder", std::string()), patch.at("name").get<std::string>()})
						if (!part.empty())
							name += (name.empty() ? "" : " / ") + part;
					options.emplace_back(patchValue(patch.at("program"), patch.value("bank", Json())), name);
				}
			const std::string current = patchValue(channel.value("program", Json()), bank);
			if (std::none_of(
					options.begin(), options.end(), [&](const auto& option) { return option.first == current; }))
				options.emplace_back(current, "Program " + std::to_string(channel.at("program").get<int>() + 1));
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

		// What a field's gesture changes, as EngineBinding::sendStep names it: the inspector's step owner, then e.g.
		// "channel 5 volume". The mixers have owners of their own, so they follow its steps (#716).
		std::string stepKey(const std::string& owner, const Json& channel, const std::string& name)
		{
			return owner + "channel " + channel.dump() + " " + name;
		}
	}

	Inspector::Inspector(EngineBinding& binding, Rml::Element& root, std::function<void(uint64_t track)> editChannel)
		: m_binding(binding), m_stepOwner(binding.stepOwner("inspector")), m_root(root.GetObserverPtr()),
		  m_editChannel(std::move(editChannel)),
		  // Another project replaces the edits in progress: the ids they were for may be the new project's too.
		  m_reset(binding.on("project.reset",
			  [this](const engine::Event&)
			  {
				  dropEdits();
				  // The in-place editors closed without an editor.changed, and the new project's ids start again.
				  m_inPlace.clear();
				  readInPlace();
			  })),
		  m_devicesChanged(binding.on("settings.changed",
			  [this](const engine::Event& event)
			  {
				  const Json& values = event.data.at("values");
				  if (const auto devices = values.find("midi.devices"); devices != values.end())
				  {
					  m_devices = *devices;
					  m_devicesKnown = true;
					  m_stale = true;
				  }
			  })),
		  m_inPlaceChanged(binding.on("editor.changed",
			  [this](const engine::Event& event)
			  {
				  const std::string editor = event.data.at("editor").get<std::string>();
				  const std::string prefix = "inPlace:";
				  if (editor.rfind(prefix, 0) != 0)
					  return;
				  const uint64_t track = std::stoull(editor.substr(prefix.size()));
				  if (event.data.at("open").get<bool>())
					  m_inPlace.insert(track);
				  else
					  m_inPlace.erase(track);
				  m_stale = true;
			  })),
		  // An undo or redo brings back or removes tracks with their open in-place editors, without an editor.changed.
		  m_historyChanged(binding.on("history.changed", [this](const engine::Event&) { readInPlace(); }))
	{
		root.SetClass("inspector", true);
		// Set up Sections from an empty area (p. 167), in the project window's document; the controls have none.
		root.SetAttribute("contextmenu", "inspector-setup");
		root.SetInnerRML(g_rml);
		if (Rml::Element* modifiers = root.QuerySelector("[section=modifiers]"))
			modifiers->SetInnerRML(modifiersRml());
		Rml::ElementList controls;
		root.QuerySelectorAll(controls, "[control], textarea, .inserts");
		for (Rml::Element* control : controls)
			control->SetAttribute("contextmenu", "");
		root.AddEventListener(Rml::EventId::Click, this);
		root.AddEventListener(Rml::EventId::Change, this);
		root.AddEventListener(Rml::EventId::Blur, this, true);    // blur doesn't bubble
		root.AddEventListener(Rml::EventId::Keydown, this, true); // Escape leaves the notepad

		readInPlace(); // the in-place editors open already (reference manual p. 1259)
		// The MIDI devices are a user setting (reference manual p. 1109); the next update shows them.
		m_binding.send("settings.get", {{"key", "midi.devices"}},
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok() || m_devicesKnown) // gone, or newer changes win
					return;
				m_devices = result.value.at("value");
				m_stale = true;
			});
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

	void Inspector::update(uint64_t track, const Json& project, uint64_t loads, uint64_t read)
	{
		m_project = &project;
		m_read = read;
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

	void Inspector::readInPlace()
	{
		m_binding.send("editor.inPlace", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok())
					return;
				m_inPlace.clear();
				for (const Json& track : result.value.at("tracks"))
					m_inPlace.insert(track.get<uint64_t>());
				m_stale = true;
			});
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
		const bool noteTrack = channel != nullptr && (type == "midi" || type == "instrument");
		available("[section=modifiers]", noteTrack);
		// Edit In-Place (p. 1259), checked while the track's in-place editor is open.
		available("button[control=editInPlace]", noteTrack);
		if (Rml::Element* button = element("button[control=editInPlace]"))
		{
			if (m_inPlace.count(m_track) != 0)
				button->SetAttribute("checked", "");
			else
				button->RemoveAttribute("checked");
		}
		available(".inspector-edit", channel != nullptr && m_editChannel);
		const bool records = type == "audio" || type == "instrument" || type == "midi" || vca;
		available("button[control=record]", records);
		available("button[control=monitor]", records);
		// Switch Time Base (reference manual pp. 179-181, 229), checked when linear.
		available("button[control=timeBase]", records || type == "group" || type == "effect");
		if (Rml::Element* button = element("button[control=timeBase]"))
		{
			if (track->at("timeBase") == "linear")
				button->SetAttribute("checked", "");
			else
				button->RemoveAttribute("checked");
		}
		available("[row=volume]", audio || vca);
		available("[row=pan]", audio);
		available("[row=input]", type == "audio");
		// A MIDI track's patch (pp. 1108, 1113): by its device's patches, or by bank and program without one.
		const Json* device = nullptr;
		if (channel != nullptr && type == "midi")
			if (const auto found = m_devices.find(channel->value("device", std::string())); found != m_devices.end())
				device = &*found;
		available("[row=device]", type == "midi");
		available("[row=bank]", type == "midi" && device == nullptr);
		available("[row=program]", type == "midi");
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
			// Not while the project is older than the fields' own steps (#716).
			const auto sending = [this, channel](const char* name)
			{ return m_binding.ahead(stepKey(m_stepOwner, channel->at("id"), name), m_read); };
			if (Rml::Element* volume = element("valuefield[control=volume]"); volume != nullptr && (audio || vca))
			{
				volume->SetAttribute("max", widgets::format(m_project->value("maxVolume", 6.0F)));
				if (!sending("volume"))
					volume->SetAttribute("value", widgets::format(channel->at("volume").get<float>()));
			}
			if (Rml::Element* pan = element("valuefield[control=pan]"); pan != nullptr && audio && !sending("pan"))
				pan->SetAttribute("value", widgets::format(channel->at("pan").get<float>() * 100.0F));
			const auto id = channel->at("id").get<uint64_t>();
			if (Rml::Element* output = element("select[control=output]"))
				widgets::fill(
					*output, outputs(*m_project, type, id), std::to_string(channel->at("output").get<uint64_t>()));
			if (Rml::Element* input = element("select[control=input]"); input != nullptr && type == "audio")
				widgets::fill(*input, inputs(*m_project), std::to_string(channel->at("input").get<uint64_t>()));
			if (type == "midi")
			{
				const std::string named = channel->value("device", std::string());
				Options devices{{"", "No Device"}};
				for (const auto& [name, value] : m_devices.items())
					devices.emplace_back(name, name);
				if (!named.empty() && device == nullptr)
					devices.emplace_back(named, named + " (not installed)");
				if (Rml::Element* select = element("select[control=device]"))
					widgets::fill(*select, devices, named);
				const Json bank = channel->value("bank", Json());
				if (Rml::Element* program = element("select[control=program]"))
					widgets::fill(
						*program, programs(device, *channel), patchValue(channel->value("program", Json()), bank));
				if (Rml::Element* field = element("textdisplay[control=bank]"))
					field->SetAttribute(
						"value", bank.is_number_integer() ? std::to_string(bank.get<int>() + 1) : "Off");
			}
			if (noteTrack && channel->contains("modifiers"))
				showModifiers(channel->at("modifiers"));

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

	void Inspector::showModifiers(const Json& modifiers)
	{
		Rml::ElementList controls;
		m_root->QuerySelectorAll(controls, "[section=modifiers] [control]");
		for (Rml::Element* control : controls)
		{
			const auto name = control->GetAttribute<Rml::String>("control", "");
			const std::vector<std::string> path = modifierPath(name);
			const Json& value = modifiers.at(path[0]);
			if (path[0] == "bypass")
			{
				if (value == true)
					control->SetAttribute("checked", "");
				else
					control->RemoveAttribute("checked");
				continue;
			}
			const size_t index = path.size() > 1 && path[1] == "1" ? 1 : 0;
			if (path.size() == 3 && path[2] == "target")
			{
				widgets::fill(*control, path[0] == "random" ? randomTargets() : rangeTargets(),
					value.at(index).at("target").get<std::string>());
				continue;
			}
			double shown = 0.0;
			if (path.size() == 1)
			{
				shown = value.get<double>();
			}
			else if (path.size() == 2)
			{
				shown = value.at(index).get<double>();
			}
			else
			{
				const Json& each = value.at(index);
				shown = each.at(path[2]).get<double>();
				// A generator's limits follow its target: ticks for positions and lengths.
				if (path[0] == "random")
				{
					const bool ticks = showsTicks(each);
					const int64_t limit = ticks ? engine::midi::g_maxRandomTicks / g_ticksPerShownTick : 127;
					setAttribute(*control, "min", std::to_string(-limit));
					setAttribute(*control, "max", std::to_string(limit));
					if (ticks)
						shown /= static_cast<double>(g_ticksPerShownTick);
				}
			}
			// Not while the project is older than the field's own steps (#716).
			if (!m_binding.ahead(stepKey(m_stepOwner, m_channel.at("id"), name), m_read))
				setAttribute(*control, "value", widgets::format(static_cast<float>(std::round(shown) + 0.0))); // not -0
		}
	}

	void Inspector::ProcessEvent(Rml::Event& event)
	{
		Rml::Element* target = event.GetTargetElement();
		if (target == nullptr)
			return;
		switch (event.GetId())
		{
		case Rml::EventId::Click:
			if (target->IsClassSet("inspector-edit") && m_editChannel && m_track != 0)
				m_editChannel(m_track); // Edit Channel Settings (reference manual p. 500)
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
		if (name == "editInPlace")
		{
			m_binding.send("editor.inPlace",
				{{"tracks", {m_track}}, {"open", event.GetParameter<bool>("value", false)}}, whenRefused());
			return;
		}
		if (name == "timeBase")
		{
			const bool linear = event.GetParameter<bool>("value", false);
			const Json* track = findIn(m_project, "tracks", m_track);
			if (track != nullptr && (track->at("timeBase") == "linear") != linear)
				m_binding.send(
					"track.set", {{"track", m_track}, {"timeBase", linear ? "linear" : "musical"}}, whenRefused());
			return;
		}
		if (!m_channel.is_object())
			return;
		if (name.rfind("modifiers-", 0) == 0)
		{
			changeModifier(name, event);
			return;
		}
		if (name == "volume" || name == "pan")
		{
			// A drag, wheel turns or a held arrow key: one undo step.
			const double number = event.GetParameter<float>("value", 0.0F);
			const Json channel = m_channel.at("id");
			const Json step = {{"command", "channel.set"},
				{"parameters", {{"channel", channel}, {name, name == "volume" ? number : number / 100.0}}}};
			m_binding.sendStep(event.GetParameter<int>("gesture", 0), stepKey(m_stepOwner, channel, name),
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
		else if (name == "device")
		{
			value = event.GetParameter<Rml::String>("value", "");
		}
		else if (name == "bank")
		{
			// Banks 1 to 16384 as typed; anything else, such as "Off", is none.
			const auto typed = event.GetParameter<Rml::String>("value", "");
			char* end = nullptr;
			const long number = std::strtol(typed.c_str(), &end, 10);
			const bool valid = end != typed.c_str() && *end == '\0' && number >= 1 && number <= 16384;
			const Json bank = valid ? Json(number - 1) : Json(nullptr);
			m_stale = true; // shows the bank as it is, such as "Off" for text that isn't one
			if (m_channel.value("bank", Json()) != bank)
				sendChannel({{"bank", bank}});
			return;
		}
		else if (name == "program")
		{
			// The patch's program and bank: "off", or "5:300" ("5:" without a bank).
			const auto chosen = event.GetParameter<Rml::String>("value", "off");
			const size_t colon = chosen.find(':');
			if (colon == std::string::npos)
			{
				if (!m_channel.value("program", Json()).is_null())
					sendChannel({{"program", nullptr}});
				return;
			}
			const Json program = std::stoi(chosen.substr(0, colon));
			const Json bank = colon + 1 < chosen.size() ? Json(std::stoi(chosen.substr(colon + 1))) : Json(nullptr);
			if (m_channel.value("program", Json()) != program || m_channel.value("bank", Json()) != bank)
				sendChannel({{"program", program}, {"bank", bank}});
			return;
		}
		// Not what it is already, such as a drop-down list that show filled.
		if (value.is_null() || m_channel.value(key, Json()) == value)
			return;
		sendChannel({{key, std::move(value)}});
	}

	void Inspector::changeModifier(const std::string& control, const Rml::Event& event)
	{
		const Json current = m_channel.value("modifiers", Json());
		if (!current.is_object())
			return;
		const std::vector<std::string> path = modifierPath(control);
		const std::string& key = path[0];
		if (key == "bypass")
		{
			const bool bypass = event.GetParameter<bool>("value", false);
			if (current.at("bypass") != bypass)
				sendChannel({{"modifiers", {{"bypass", bypass}}}});
			return;
		}
		const size_t index = path.size() > 1 && path[1] == "1" ? 1 : 0;
		// A generator or range changes only the field given: channel.set keeps the rest.
		Json pair = Json::array({Json::object(), Json::object()});
		if (path.size() == 3 && path[2] == "target")
		{
			const auto target = event.GetParameter<Rml::String>("value", "");
			if (current.at(key).at(index).at("target") == target)
				return;
			pair[index]["target"] = target;
			sendChannel({{"modifiers", {{key, std::move(pair)}}}});
			return;
		}
		int64_t value = std::llround(event.GetParameter<float>("value", 0.0F));
		Json modifiers = Json::object();
		if (path.size() == 1)
		{
			modifiers[key] = value;
		}
		else if (path.size() == 2)
		{
			Json fraction = current.at(key); // numerator and denominator both
			fraction[index] = value;
			modifiers[key] = std::move(fraction);
		}
		else
		{
			if (key == "random" && showsTicks(current.at(key).at(index)))
				value *= g_ticksPerShownTick;
			pair[index][path[2]] = value;
			modifiers[key] = std::move(pair);
		}
		// What it is already, such as a value typed again: no undo step. Not during a gesture, whose earlier steps the
		// channel shown may not have yet: a drag back to where it started must still send that value.
		const int gesture = event.GetParameter<int>("gesture", 0);
		Json changed = current;
		if (path.size() == 1)
			changed[key] = value;
		else if (path.size() == 2)
			changed[key][index] = value;
		else
			changed[key][index][path[2]] = value;
		if (gesture == 0 && changed == current)
			return;
		// A drag, wheel turns or a held arrow key: one undo step.
		const Json channel = m_channel.at("id");
		const Json step = {
			{"command", "channel.set"}, {"parameters", {{"channel", channel}, {"modifiers", std::move(modifiers)}}}};
		m_binding.sendStep(gesture, stepKey(m_stepOwner, channel, control), "Change MIDI Modifiers",
			Json::array({step}), whenRefused());
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
