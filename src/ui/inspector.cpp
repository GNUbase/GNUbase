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
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <set>
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
			R"(<div class="inspector-row" row="inputTransformer"><span class="inspector-label">Transformer</span>)"
			R"(<select control="inputTransformer" title="Input Transformer"/></div>)"
			R"(</div></div>)"
			R"(<div class="inspector-section" section="modifiers" label="MIDI Modifiers"/>)"
			R"(<div class="inspector-section" section="midiInserts" label="MIDI Inserts"/>)"
			R"(<div class="inspector-section" section="midiSends" label="MIDI Sends"/>)"
			R"(<div class="inspector-section" section="chords" label="Chords">)"
			R"(<button class="section-header">Chords</button><div class="section-body">)"
			R"(<div class="inspector-row" row="followChords"><span class="inspector-label">Follow</span>)"
			R"(<select control="followChords" title="Follow Chord Track"/></div>)"
			R"(<div class="inspector-row" row="followVoice"><span class="inspector-label">Voice</span>)"
			R"(<select control="followVoice" title="Single Voice"/></div>)"
			R"(<div class="inspector-row" row="followVoicings"><span class="inspector-label">Voicings</span>)"
			R"(<select control="followVoicings" title="This Track's Voicings"/></div>)"
			R"(<div class="inspector-row" row="followMaster"><span class="inspector-label">Master</span>)"
			R"(<select control="followMaster" title="Voicing Master Track"/></div>)"
			R"(<div class="inspector-row" row="liveInput"><span class="inspector-label">Live Input</span>)"
			R"(<select control="liveInput" title="Live Input"/></div>)"
			R"(</div></div>)"
			// The chord track's own settings (reference manual pp. 1329, 1333-1335).
			R"(<div class="inspector-section" section="chordTrack" label="Chord Track">)"
			R"(<button class="section-header">Chord Track</button><div class="section-body">)"
			R"(<div class="inspector-buttons">)"
			R"(<button type="toggle" class="toggle" control="chordAdaptiveVoicings" title="Adaptive Voicings">)"
			R"(Adaptive</button>)"
			R"(<button type="toggle" class="toggle" control="chordAutomaticScales" title="Automatic Scales">)"
			R"(Scales</button>)"
			R"(<button type="toggle" class="toggle" control="chordAcousticFeedback" title="Acoustic Feedback">)"
			R"(Feedback</button></div>)"
			R"(<div class="inspector-row" row="chordVoicingLibrary"><span class="inspector-label">Voicings</span>)"
			R"(<select control="chordVoicingLibrary" title="Voicing Library"/></div>)"
			R"(<div class="inspector-row" row="chordVoicingStyle"><span class="inspector-label">Subset</span>)"
			R"(<select control="chordVoicingStyle" title="Voicing Library Subset"/></div>)"
			R"(<div class="inspector-row" row="chordOctaveOffset"><span class="inspector-label">Octave</span>)"
			R"(<valuefield control="chordOctaveOffset" min="-3" max="3" default="0" step="1" decimals="0")"
			R"( title="Octave Offset"/></div>)"
			// The Custom Voicing panel's piano options and range (p. 1335).
			R"(<div class="inspector-buttons" row="chordPianoOptions">)"
			R"(<button type="toggle" class="toggle" control="chordAddRoot" title="Add Root Note">Root</button>)"
			R"(<button type="toggle" class="toggle" control="chordDuplicateRoot" title="Duplicate Root">2x</button>)"
			R"(<button type="toggle" class="toggle" control="chordFattenUp" title="Fatten Up">Fat</button></div>)"
			R"(<div class="inspector-row" row="chordLowestRoot"><span class="inspector-label">Lowest Root</span>)"
			R"(<valuefield control="chordLowestRoot" min="0" max="127" default="36" step="1" decimals="0")"
			R"( title="Lowest Root Note"/></div>)"
			R"(<div class="inspector-row" row="chordLowest"><span class="inspector-label">Lowest</span>)"
			R"(<valuefield control="chordLowest" min="0" max="115" default="52" step="1" decimals="0")"
			R"( title="Lowest Note"/></div>)"
			R"(<div class="inspector-row" row="chordHighest"><span class="inspector-label">Highest</span>)"
			R"(<valuefield control="chordHighest" min="12" max="127" default="84" step="1" decimals="0")"
			R"( title="Highest Note"/></div>)"
			R"(<div class="inspector-row" row="chordShift"><span class="inspector-label">Shift</span>)"
			R"(<valuefield control="chordShift" min="-480" max="480" default="0" step="1" decimals="0")"
			R"( title="Shift Chords in Ticks"/></div>)"
			R"(</div></div>)"
			R"(<div class="inspector-section" section="articulations" label="Articulation Map">)"
			R"(<button class="section-header">Articulation Map</button><div class="section-body">)"
			R"(<div class="inspector-row" row="articulationMap"><span class="inspector-label">Map</span>)"
			R"(<select control="articulationMap" title="Active Articulation Map"/></div>)"
			R"(<div class="inspector-buttons"><button class="inspector-articulation-setup")"
			R"( title="Articulation Map Setup">Setup...</button></div>)"
			R"(</div></div>)"
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

		// The MIDI Inserts and MIDI Sends sections (pp. 1104-1106): their controls are named after the channel's
		// midiInserts and midiSends in channel.set, the section, then the slot's index and its field or "parameter-"
		// and a parameter's key ("midiInserts-0-effect", "midiSends-1-parameter-rate"), or the section's own field
		// ("midiInserts-bypass", "midiInserts-divider", "midiSends-pre"). Each slot has the control panel of its effect
		// below it, which its edit button opens and closes.
		std::string midiEffectsRml(bool sends)
		{
			const std::string section = sends ? "midiSends" : "midiInserts";
			const std::string title = sends ? "MIDI Sends" : "MIDI Inserts";
			std::string rml = R"(<button class="section-header">)" + title + R"(</button><div class="section-body">)";
			rml += R"(<div class="inspector-buttons"><button type="toggle" class="toggle bypass" control=")" + section +
				R"(-bypass" title="Bypass )" + title + R"(">Bypass</button>)";
			if (sends)
				rml += R"(<button type="toggle" class="toggle" control="midiSends-pre")"
					   R"( title="Pre/Post MIDI Modifiers and Inserts">Pre</button>)";
			rml += "</div>";
			if (!sends)
				rml += row("divider", "Divider",
					R"(<select control="midiInserts-divider" title="Divider: the inserts above it are recorded"/>)");
			for (size_t slot = 0; slot < 4; ++slot)
			{
				const std::string name = section + "-" + std::to_string(slot);
				rml += R"(<div class="midi-effect-slot" slot=")";
				rml += std::to_string(slot);
				rml += R"("><div class="inspector-row"><button type="toggle" class="toggle" control=")";
				rml += name;
				rml += R"(-on" title="Activate">)";
				rml += std::to_string(slot + 1);
				rml += R"(</button><select control=")";
				rml += name;
				rml += R"(-effect" title="Select Effect Type"/><button class="midi-effect-edit" title="Open/Close )";
				rml += sends ? "Send" : "Insert";
				rml += R"( Effect Editor">e</button></div>)";
				if (sends)
					rml += row("output", "Output", R"(<select control=")" + name + R"(-output" title="Send Output"/>)");
				rml += R"(<div class="midi-effect-panel"/></div>)";
			}
			return rml + "</div>";
		}

		// A MIDI effect's control panel: a drop-down list for a parameter with options, a value field for the others.
		std::string midiEffectPanelRml(const std::string& slot, const Json& effect)
		{
			std::string rml;
			for (const Json& parameter : effect.at("parameters"))
			{
				const std::string key = parameter.at("key").get<std::string>();
				const std::string label = Rml::StringUtilities::EncodeRml(parameter.at("name").get<std::string>());
				std::string control = slot;
				control += "-parameter-";
				control += key;
				std::string field;
				if (!parameter.at("options").empty())
				{
					field = R"(<select control=")";
					field += control;
					field += R"(" title=")";
					field += label;
					field += R"("/>)";
					rml += row(key, label, field);
					continue;
				}
				const bool stepped = parameter.at("stepped").get<bool>();
				field = R"(<valuefield control=")";
				field += control;
				for (const auto& [attribute, value] : {std::pair{"min", parameter.at("minimum")},
						 std::pair{"max", parameter.at("maximum")}, std::pair{"default", parameter.at("default")}})
				{
					field += R"(" )";
					field += attribute;
					field += R"(=")";
					field += widgets::format(value.get<float>());
				}
				field += stepped ? R"(" step="1" decimals="0)" : R"(" step="0.5" decimals="1)";
				field += R"(" unit=")";
				field += Rml::StringUtilities::EncodeRml(parameter.at("unit").get<std::string>());
				field += R"(" title=")";
				field += label;
				field += R"("/>)";
				rml += row(key, label, field);
			}
			return rml;
		}

		// A MIDI effect control's name split at its dashes: section, slot, field and, for a parameter, its key.
		std::vector<std::string> midiEffectPath(const std::string& control)
		{
			std::vector<std::string> path;
			size_t from = 0;
			while (from <= control.size() && path.size() < 3)
			{
				const size_t dash = std::min(control.find('-', from), control.size());
				path.push_back(control.substr(from, dash - from));
				from = dash + 1;
			}
			if (from <= control.size())
				path.push_back(control.substr(from));
			return path;
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

		// A toggle button's state.
		void checked(Rml::Element& button, bool on)
		{
			if (on)
				button.SetAttribute("checked", "");
			else
				button.RemoveAttribute("checked");
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

		// A channel's input transformer mode: off, project or track.
		std::string inputTransformerMode(const Json& channel)
		{
			if (!channel.is_object())
				return "off";
			const Json transformer = channel.value("inputTransformer", Json::object());
			return transformer.value("mode", std::string("off"));
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

	Inspector::Inspector(EngineBinding& binding, Rml::Element& root, std::function<void(uint64_t track)> editChannel,
		std::function<void(uint64_t track)> setupArticulations,
		std::function<void(uint64_t track, bool project)> openInputTransformer)
		: m_binding(binding), m_stepOwner(binding.stepOwner("inspector")), m_root(root.GetObserverPtr()),
		  m_editChannel(std::move(editChannel)), m_setupArticulations(std::move(setupArticulations)),
		  m_openInputTransformer(std::move(openInputTransformer)),
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
		for (const bool sends : {false, true})
			if (Rml::Element* section = root.QuerySelector(sends ? "[section=midiSends]" : "[section=midiInserts]"))
				section->SetInnerRML(midiEffectsRml(sends));
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
		// The MIDI effects the slots offer, with their parameters (midiEffect.list).
		m_binding.send("midiEffect.list", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok())
					return;
				m_midiEffects = result.value.at("effects");
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
				std::set<uint64_t> open;
				for (const Json& track : result.value.at("tracks"))
					open.insert(track.get<uint64_t>());
				// Only a change shows again: showing in between would put back fields with edits under way.
				if (open != m_inPlace)
				{
					m_inPlace = std::move(open);
					m_stale = true;
				}
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
		available("[section=midiInserts]", noteTrack);
		available("[section=midiSends]", noteTrack);
		// Edit In-Place (p. 1259), checked while the track's in-place editor is open.
		available("button[control=editInPlace]", noteTrack);
		if (Rml::Element* button = element("button[control=editInPlace]"))
		{
			if (m_inPlace.count(m_track) != 0)
				button->SetAttribute("checked", "");
			else
				button->RemoveAttribute("checked");
		}
		// Follow Chord Track (reference manual pp. 1338-1340), while the project has a chord track.
		const Json chords = m_project->value("chords", Json::object());
		available("[section=chords]", noteTrack && chords.value("track", 0) != 0);
		if (noteTrack)
		{
			Json follow = {
				{"mode", "off"}, {"voice", "soprano"}, {"voicings", "chordTrack"}, {"master", 0}, {"liveInput", "off"}};
			for (const Json& each : chords.value("follow", Json::array()))
				if (each.at("track") == m_track)
					follow = each;
			const std::string mode = follow.at("mode").get<std::string>();
			const auto fill = [&element](const char* control, const Options& options, const std::string& value)
			{
				if (Rml::Element* select = element("select[control=" + std::string(control) + "]"))
					widgets::fill(*select, options, value);
			};
			fill("followChords",
				{{"off", "Off"}, {"chordsAndScales", "Chords & Scales"}, {"chords", "Chords"}, {"scales", "Scales"},
					{"rootNotes", "Root Notes"}, {"voicings", "Voicings"}, {"singleVoice", "Single Voice"}},
				mode);
			fill("followVoice", {{"soprano", "Soprano"}, {"alto", "Alto"}, {"tenor", "Tenor"}, {"bass", "Bass"}},
				follow.at("voice").get<std::string>());
			fill("followVoicings",
				{{"chordTrack", "Chord Track"}, {"basic", "Basic"}, {"piano", "Piano"}, {"guitar", "Guitar"}},
				follow.at("voicings").get<std::string>());
			Options masters = {{"0", "None"}};
			for (const Json& each : m_project->at("tracks"))
				if ((each.at("type") == "midi" || each.at("type") == "instrument") && each.at("id") != m_track)
					masters.emplace_back(
						std::to_string(each.at("id").get<uint64_t>()), each.at("name").get<std::string>());
			fill("followMaster", masters, std::to_string(follow.at("master").get<uint64_t>()));
			fill("liveInput", {{"off", "Off"}, {"chords", "Chords"}, {"scales", "Scales"}},
				follow.at("liveInput").get<std::string>());
			available("[row=followVoice]", mode == "singleVoice");
			available("[row=followMaster]", mode == "singleVoice");
			available("[row=followVoicings]", mode == "voicings" || mode == "singleVoice");
		}
		// The chord track's settings.
		available("[section=chordTrack]", type == "chord");
		if (type == "chord")
		{
			const Json voicing = chords.value("voicing", Json::object());
			const std::string library = voicing.value("library", std::string("basic"));
			if (Rml::Element* select = element("select[control=chordVoicingLibrary]"))
				widgets::fill(*select, {{"basic", "Basic"}, {"piano", "Piano"}, {"guitar", "Guitar"}}, library);
			Options styles;
			for (const auto& [value, label] : std::vector<std::pair<const char*, const char*>>{{"triads", "Triads"},
					 {"triadsMaj9", "Triads with maj9"}, {"triadsMaj9Min9", "Triads with maj9 and min9"},
					 {"fourNote", "4-Note Chords"}, {"fourNoteOpen", "4-Note Chords (Open Jazz)"},
					 {"fiveNote", "5-Note Chords"}})
				if (library == "piano")
					styles.emplace_back(value, label);
			if (library == "guitar")
				styles = {{"triads", "Triads"}, {"fourNote", "4-Note Chords"}, {"threeString", "3-String Triads"},
					{"modernJazz", "Modern Jazz"}};
			std::string style = voicing.value("style", std::string());
			if (style.empty() && !styles.empty())
				style = styles.front().first;
			if (Rml::Element* select = element("select[control=chordVoicingStyle]"))
				widgets::fill(*select, styles, style);
			available("[row=chordVoicingStyle]", library != "basic");
			available("[row=chordOctaveOffset]", library == "basic");
			for (const char* row : {"chordPianoOptions", "chordLowestRoot", "chordLowest", "chordHighest"})
				available("[row=" + std::string(row) + "]", library == "piano");
			for (const auto& [control, key] : {std::pair{"chordAdaptiveVoicings", "adaptiveVoicings"},
					 std::pair{"chordAutomaticScales", "automaticScales"},
					 std::pair{"chordAcousticFeedback", "acousticFeedback"}})
				if (Rml::Element* button = element("button[control=" + std::string(control) + "]"))
					checked(*button, chords.value(key, false));
			for (const auto& [control, key] : {std::pair{"chordAddRoot", "addRoot"},
					 std::pair{"chordDuplicateRoot", "duplicateRoot"}, std::pair{"chordFattenUp", "fattenUp"}})
				if (Rml::Element* button = element("button[control=" + std::string(control) + "]"))
					checked(*button, voicing.value(key, false));
			for (const auto& [control, value] : {std::pair{"chordOctaveOffset", chords.value("octaveOffset", 0)},
					 std::pair{"chordLowestRoot", voicing.value("lowestRoot", 36)},
					 std::pair{"chordLowest", voicing.value("lowest", 52)},
					 std::pair{"chordHighest", voicing.value("highest", 84)},
					 std::pair{"chordShift",
						 static_cast<int>(chords.value("shiftChords", int64_t{0}) / g_ticksPerShownTick)}})
				if (Rml::Element* field = element("valuefield[control=" + std::string(control) + "]"))
					field->SetAttribute("value", std::to_string(value));
		}
		// The track's articulation map (#175, reference manual pp. 1263, 1286), and the setup for the maps.
		available("[section=articulations]", noteTrack);
		if (Rml::Element* maps = element("select[control=articulationMap]"); maps != nullptr && noteTrack)
		{
			Options options = {{"0", "No Map"}};
			for (const Json& map : m_project->value("articulationMaps", Json::array()))
				options.emplace_back(std::to_string(map.at("id").get<uint64_t>()), map.at("name").get<std::string>());
			widgets::fill(*maps, options, std::to_string(track->value("articulationMap", uint64_t{0})));
		}
		available(".inspector-articulation-setup", noteTrack && m_setupArticulations);
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
		available("[row=inputTransformer]", noteTrack);
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
			// The input transformer (reference manual pp. 1097, 1393): its mode, and Open Panel... while it is on.
			if (Rml::Element* select = element("select[control=inputTransformer]"); select != nullptr && noteTrack)
			{
				const std::string mode = inputTransformerMode(*channel);
				Options modes = {{"off", "Off"}, {"project", "Project"}, {"track", "Track"}};
				if (mode != "off")
					modes.emplace_back("open", "Open Panel...");
				widgets::fill(*select, modes, mode);
			}
			if (noteTrack && channel->contains("modifiers"))
				showModifiers(channel->at("modifiers"));
			if (noteTrack && channel->contains("midiInserts"))
				for (const char* section : {"midiInserts", "midiSends"})
					showMidiEffects(section, channel->at(section));

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

	void Inspector::showMidiEffects(const std::string& section, const Json& rack)
	{
		Rml::Element* root = m_root->QuerySelector("[section=" + section + "]");
		if (root == nullptr)
			return;
		const bool sends = section == "midiSends";
		const auto element = [root](const std::string& selector) { return root->QuerySelector(selector); };
		if (Rml::Element* bypass = element("button[control=" + section + "-bypass]"))
			checked(*bypass, rack.at("bypass").get<bool>());
		if (Rml::Element* pre = element("button[control=midiSends-pre]"))
			checked(*pre, rack.value("pre", false));
		const int divider = rack.value("divider", 0);
		if (Rml::Element* select = element("select[control=midiInserts-divider]"))
			widgets::fill(*select,
				{{"0", "Above Slot 1"}, {"1", "Below Slot 1"}, {"2", "Below Slot 2"}, {"3", "Below Slot 3"},
					{"4", "Below Slot 4"}},
				std::to_string(divider));
		Options effects = {{"", "No Effect"}};
		for (const Json& effect : m_midiEffects)
			effects.emplace_back(effect.at("id").get<std::string>(), effect.at("name").get<std::string>());
		// A send plays to the track's own output or to another instrument track (p. 1106).
		Options outputs = {{"0", "Track Output"}};
		for (const Json& track : m_project->at("tracks"))
			if (track.at("type") == "instrument" && track.at("channel") != m_channel.at("id"))
				outputs.emplace_back(
					std::to_string(track.at("channel").get<uint64_t>()), track.at("name").get<std::string>());
		const Json& slots = rack.at("slots");
		for (size_t index = 0; index < slots.size(); ++index)
		{
			const Json& slot = slots[index];
			const std::string name = section + "-" + std::to_string(index);
			Rml::Element* box = element(".midi-effect-slot[slot=" + std::to_string(index) + "]");
			if (box == nullptr)
				continue;
			const std::string id = slot.at("effect").get<std::string>();
			setClass(*box, "empty", id.empty());
			setClass(*box, "off", !slot.at("on").get<bool>());
			setClass(*box, "recorded", !sends && static_cast<int>(index) < divider);
			if (Rml::Element* select = box->QuerySelector("select[control=" + name + "-effect]"))
				widgets::fill(*select, effects, id);
			if (Rml::Element* on = box->QuerySelector("button[control=" + name + "-on]"))
				checked(*on, slot.at("on").get<bool>());
			if (Rml::Element* output = box->QuerySelector("select[control=" + name + "-output]"))
				widgets::fill(*output, outputs, std::to_string(slot.value("output", uint64_t{0})));
			if (Rml::Element* row = box->QuerySelector("[row=output]"))
				setClass(*row, "unavailable", id.empty());
			// The control panel of the slot's effect, made again when the effect changes.
			Rml::Element* panel = box->QuerySelector(".midi-effect-panel");
			const auto info = std::find_if(m_midiEffects.begin(), m_midiEffects.end(),
				[&id](const Json& effect) { return effect.at("id") == id; });
			if (panel == nullptr)
				continue;
			if (panel->GetAttribute<Rml::String>("effect", "") != id || panel->GetNumChildren() == 0)
			{
				panel->SetInnerRML(info != m_midiEffects.end() ? midiEffectPanelRml(name, *info) : std::string());
				panel->SetAttribute("effect", id);
				Rml::ElementList controls;
				panel->QuerySelectorAll(controls, "[control]");
				for (Rml::Element* control : controls)
					control->SetAttribute("contextmenu", "");
			}
			if (info == m_midiEffects.end())
				continue;
			for (const Json& parameter : info->at("parameters"))
			{
				const std::string key = parameter.at("key").get<std::string>();
				std::string control = name;
				control += "-parameter-";
				control += key;
				const Json value = slot.at("parameters").value(key, parameter.at("default"));
				if (Rml::Element* select = panel->QuerySelector("select[control=" + control + "]"))
				{
					Options options;
					const Json& names = parameter.at("options");
					for (size_t option = 0; option < names.size(); ++option)
						options.emplace_back(std::to_string(option), names[option].get<std::string>());
					widgets::fill(*select, options, std::to_string(std::lround(value.get<double>())));
				}
				else if (Rml::Element* field = panel->QuerySelector("valuefield[control=" + control + "]");
					field != nullptr && !m_binding.ahead(stepKey(m_stepOwner, m_channel.at("id"), control), m_read))
				{
					setAttribute(*field, "value", widgets::format(value.get<float>()));
				}
			}
		}
	}

	void Inspector::changeMidiEffect(const std::string& control, const Rml::Event& event)
	{
		const std::vector<std::string> path = midiEffectPath(control);
		const std::string& section = path[0];
		const Json current = m_channel.value(section, Json());
		if (!current.is_object() || path.size() < 2)
			return;
		Json rack = Json::object();
		if (path[1] == "bypass" || path[1] == "pre")
		{
			const bool on = event.GetParameter<bool>("value", false);
			if (current.at(path[1]) == on)
				return;
			rack[path[1]] = on;
		}
		else if (path[1] == "divider")
		{
			const auto divider =
				static_cast<int>(std::strtol(event.GetParameter<Rml::String>("value", "0").c_str(), nullptr, 10));
			if (current.at("divider") == divider)
				return;
			rack["divider"] = divider;
		}
		else if (path.size() >= 3)
		{
			// The slot changes only the field given, channel.set keeps the rest: the slots before it are empty objects.
			const auto index = static_cast<size_t>(std::strtoul(path[1].c_str(), nullptr, 10));
			if (index >= current.at("slots").size())
				return;
			const Json& slot = current.at("slots")[index];
			Json changed = Json::object();
			const std::string& field = path[2];
			if (field == "on")
			{
				const bool on = event.GetParameter<bool>("value", false);
				if (slot.at("on") == on)
					return;
				changed["on"] = on;
			}
			else if (field == "effect")
			{
				const auto effect = event.GetParameter<Rml::String>("value", "");
				if (slot.at("effect") == effect)
					return;
				changed["effect"] = effect;
				changed["on"] = true; // a chosen effect is active, its control panel open (p. 1105)
				if (Rml::Element* box =
						m_root->QuerySelector("[section=" + section + "] .midi-effect-slot[slot=" + path[1] + "]"))
					setClass(*box, "open", !effect.empty());
			}
			else if (field == "output")
			{
				const uint64_t output =
					std::strtoull(event.GetParameter<Rml::String>("value", "0").c_str(), nullptr, 10);
				if (slot.value("output", uint64_t{0}) == output)
					return;
				changed["output"] = output;
			}
			else if (field == "parameter" && path.size() == 4)
			{
				const bool select =
					event.GetTargetElement() != nullptr && event.GetTargetElement()->GetTagName() == "select";
				const double value = select
					? std::strtod(event.GetParameter<Rml::String>("value", "0").c_str(), nullptr)
					: static_cast<double>(event.GetParameter<float>("value", 0.0F));
				const int gesture = event.GetParameter<int>("gesture", 0);
				if (gesture == 0 && slot.at("parameters").contains(path[3]) &&
					std::abs(slot.at("parameters").at(path[3]).get<double>() - value) < 1e-6)
					return;
				Json slots = Json::array();
				for (size_t i = 0; i < index; ++i)
					slots.push_back(Json::object());
				slots.push_back({{"parameters", {{path[3], value}}}});
				// A drag, wheel turns or a held arrow key: one undo step.
				const Json channel = m_channel.at("id");
				const Json step = {{"command", "channel.set"},
					{"parameters", {{"channel", channel}, {section, {{"slots", std::move(slots)}}}}}};
				m_binding.sendStep(gesture, stepKey(m_stepOwner, channel, control), "Change MIDI Effect",
					Json::array({step}), whenRefused());
				return;
			}
			else
			{
				return;
			}
			Json slots = Json::array();
			for (size_t i = 0; i < index; ++i)
				slots.push_back(Json::object());
			slots.push_back(std::move(changed));
			rack["slots"] = std::move(slots);
		}
		else
		{
			return;
		}
		sendChannel({{section, std::move(rack)}});
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
			if (target->IsClassSet("inspector-articulation-setup") && m_setupArticulations && m_track != 0)
				m_setupArticulations(m_track);
			// A MIDI effect's edit button opens and closes its control panel.
			if (target->IsClassSet("midi-effect-edit") && target->GetParentNode() != nullptr &&
				target->GetParentNode()->GetParentNode() != nullptr)
			{
				Rml::Element& slot = *target->GetParentNode()->GetParentNode();
				slot.SetClass("open", !slot.IsClassSet("open"));
			}
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
		if (name == "inputTransformer")
		{
			const auto value = event.GetParameter<Rml::String>("value", "off");
			const std::string mode = inputTransformerMode(m_channel);
			if (value == "open")
			{
				m_stale = true; // shows the mode again
				if (m_openInputTransformer)
					m_openInputTransformer(m_track, mode == "project");
			}
			else if (value != mode)
			{
				sendChannel({{"inputTransformer", {{"mode", value}}}});
			}
			return;
		}
		if (name == "articulationMap")
		{
			const uint64_t map = std::strtoull(event.GetParameter<Rml::String>("value", "0").c_str(), nullptr, 10);
			m_binding.send("track.set", {{"track", m_track}, {"articulationMap", map}}, whenRefused());
			return;
		}
		if (name == "followChords" || name == "followVoice" || name == "followVoicings" || name == "followMaster" ||
			name == "liveInput")
		{
			const auto value = event.GetParameter<Rml::String>("value", "");
			Json parameters = {{"track", m_track}};
			if (name == "followMaster")
				parameters["master"] = std::strtoull(value.c_str(), nullptr, 10);
			else if (name == "liveInput")
				parameters["liveInput"] = value;
			else if (name == "followChords")
				parameters["mode"] = value;
			else
				parameters[name == "followVoice" ? "voice" : "voicings"] = value;
			m_binding.send("chord.follow", parameters, whenRefused());
			return;
		}
		// The chord track's settings (chord.setTrack).
		if (name.rfind("chord", 0) == 0)
		{
			Json parameters;
			if (name == "chordAdaptiveVoicings" || name == "chordAutomaticScales" || name == "chordAcousticFeedback")
			{
				std::string key = name.substr(5);
				key[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(key[0])));
				parameters[key] = event.GetParameter<bool>("value", false);
			}
			else if (name == "chordAddRoot" || name == "chordDuplicateRoot" || name == "chordFattenUp")
			{
				std::string key = name.substr(5);
				key[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(key[0])));
				parameters["voicing"] = {{key, event.GetParameter<bool>("value", false)}};
			}
			else if (name == "chordVoicingLibrary")
			{
				parameters["voicing"] = {{"library", event.GetParameter<Rml::String>("value", "basic")}};
			}
			else if (name == "chordVoicingStyle")
			{
				parameters["voicing"] = {{"style", event.GetParameter<Rml::String>("value", "")}};
			}
			else
			{
				const auto number = static_cast<int>(std::lround(event.GetParameter<float>("value", 0.0F)));
				if (name == "chordOctaveOffset")
					parameters["octaveOffset"] = number;
				else if (name == "chordShift")
					parameters["shiftChords"] = static_cast<int64_t>(number) * g_ticksPerShownTick;
				else if (name == "chordLowestRoot")
					parameters["voicing"] = {{"lowestRoot", number}};
				else if (name == "chordLowest")
					parameters["voicing"] = {{"lowest", number}};
				else if (name == "chordHighest")
					parameters["voicing"] = {{"highest", number}};
			}
			if (!parameters.is_null())
				m_binding.send("chord.setTrack", parameters, whenRefused());
			return;
		}
		if (!m_channel.is_object())
			return;
		if (name.rfind("modifiers-", 0) == 0)
		{
			changeModifier(name, event);
			return;
		}
		if (name.rfind("midiInserts-", 0) == 0 || name.rfind("midiSends-", 0) == 0)
		{
			changeMidiEffect(name, event);
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
