// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/chordpads.h"

#include "platform/log.h"
#include "ui/widgetutils.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <array>
#include <functional>
#include <utility>

namespace gnubase::ui
{
	using engine::Json;

	namespace
	{
		constexpr std::array<Rml::EventId, 9> g_events = {Rml::EventId::Mousedown, Rml::EventId::Click,
			Rml::EventId::Dblclick, Rml::EventId::Change, Rml::EventId::Mousescroll, Rml::EventId::Keydown,
			Rml::EventId::Mouseover, Rml::EventId::Mouseout, Rml::EventId::Mousemove};

		constexpr const char* g_displayKey = "ui.chordPads";
		// The keys of the zone's keyboard (reference manual p. 1355): C0 to C6.
		constexpr int g_lowestKey = 24;
		constexpr int g_highestKey = 96;
		// The pad modifier buttons, which play the first selected pad while they are pressed (p. 1351).
		constexpr std::array<const char*, 6> g_modifierActions = {
			"transposeDown", "transposeUp", "fewerTensions", "moreTensions", "previousVoicing", "nextVoicing"};
		constexpr std::array<const char*, 6> g_modifierKeyNames = {
			"previousVoicing", "nextVoicing", "fewerTensions", "moreTensions", "transposeDown", "transposeUp"};

		bool isBlack(int pitch)
		{
			const int degree = pitch % 12;
			return degree == 1 || degree == 3 || degree == 6 || degree == 8 || degree == 10;
		}

		constexpr std::array<const char*, 12> g_noteNames = {
			"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

		// A MIDI note's name, middle C (60) is C3.
		std::string noteName(int note)
		{
			return std::string(g_noteNames.at(static_cast<size_t>(note % 12))) + std::to_string((note / 12) - 2);
		}

		std::string encode(const std::string& text)
		{
			return Rml::StringUtilities::EncodeRml(text);
		}

		void setClass(Rml::Element* element, const char* name, bool set)
		{
			if (element != nullptr && element->IsClassSet(name) != set)
				element->SetClass(name, set);
		}

		void setText(Rml::Element* element, const std::string& text)
		{
			const std::string rml = encode(text);
			if (element != nullptr && element->GetInnerRML() != rml)
				element->SetInnerRML(rml);
		}

		// The checked attribute gives toggles the state class .checked.
		void setChecked(Rml::Element* element, bool set)
		{
			if (element == nullptr || element->HasAttribute("checked") == set)
				return;
			if (set)
				element->SetAttribute("checked", "");
			else
				element->RemoveAttribute("checked");
		}

		void setValue(Rml::Element* element, int value)
		{
			if (element != nullptr && element->GetAttribute<int>("value", value + 1) != value)
				element->SetAttribute("value", std::to_string(value));
		}

		// The element or its nearest ancestor with the attribute.
		const Rml::Element* withAttribute(const Rml::Element* element, const char* name)
		{
			for (; element != nullptr; element = element->GetParentNode())
				if (element->HasAttribute(name))
					return element;
			return nullptr;
		}

		// Whether the element is the one with the id or inside it.
		bool within(const Rml::Element* element, const std::string& id)
		{
			for (; element != nullptr; element = element->GetParentNode())
				if (element->GetId() == id)
					return true;
			return false;
		}

		// Whether the element or an ancestor takes the arrow keys itself: a text or value field, or a menu.
		bool takesArrows(const Rml::Element* element)
		{
			for (; element != nullptr; element = element->GetParentNode())
			{
				const Rml::String& tag = element->GetTagName();
				if (tag == "input" || tag == "textarea" || tag == "select" || tag == "valuefield")
					return true;
			}
			return false;
		}

		std::string button(const char* action, const char* title, const char* text, bool toggle = false)
		{
			return std::string(R"(<button action=")") + action + R"(" id="chord-pads-)" + action + '"' +
				(toggle ? R"( type="toggle")" : "") + R"( title=")" + title + R"(">)" + text + "</button>";
		}

		std::string number(const char* setting, const char* title, int min, int max)
		{
			return std::string(R"(<valuefield id="chord-pads-)") + setting + R"(" setting=")" + setting + R"(" min=")" +
				std::to_string(min) + R"(" max=")" + std::to_string(max) + R"(" step="1" decimals="0" title=")" +
				title + R"("/>)";
		}

		// The chord modifiers' remote keys (reference manual p. 1387), as value fields.
		std::string modifierKeys()
		{
			static const std::array<const char*, 6> titles = {"Previous Voicing", "Next Voicing", "Fewer Tensions",
				"More Tensions", "Transpose Down", "Transpose Up"};
			std::string rml;
			for (size_t i = 0; i < g_modifierKeyNames.size(); ++i)
				rml += std::string(R"(<valuefield id="chord-pads-modifier-)") + g_modifierKeyNames.at(i) +
					R"(" modifier=")" + g_modifierKeyNames.at(i) +
					R"(" min="0" max="127" step="1" decimals="0" title=")" + titles.at(i) + R"("/>)";
			return rml;
		}

		// The Player Remote Control tab's keys (reference manual p. 1389) and the sections player mode's settings
		// (pp. 1381-1385): five value fields of a setting of five, or three for the player keys.
		std::string fiveKeys(const char* setting, const char* title, int count, int min, int max)
		{
			std::string rml;
			for (int i = 0; i < count; ++i)
				rml += std::string(R"(<valuefield class="chord-pads-five" five=")") + setting + R"(" index=")" +
					std::to_string(i) + R"(" id="chord-pads-)" + setting + "-" + std::to_string(i) + R"(" min=")" +
					std::to_string(min) + R"(" max=")" + std::to_string(max) + R"(" step="1" decimals="0" title=")" +
					title + " " + std::to_string(i + 1) + R"("/>)";
			return rml;
		}

		// Which section each subsection plays, moved by its offset (p. 1384).
		std::string subsectionSections()
		{
			std::string rml;
			for (int i = 0; i < 5; ++i)
				rml += R"(<select class="chord-pads-five" five="subsectionSections" index=")" + std::to_string(i) +
					R"(" id="chord-pads-subsectionSections-)" + std::to_string(i) + R"(" title="Subsection )" +
					std::to_string(i + 1) + R"( Assigned To"/>)";
			return rml;
		}

		// The zone's keyboard (reference manual p. 1355), its keys from C0 to C6.
		std::string keyboard()
		{
			std::string rml = R"(<div id="chord-pads-keyboard" class="chord-pads-keyboard">)";
			for (int pitch = g_lowestKey; pitch <= g_highestKey; ++pitch)
				rml += std::string(R"(<div class="chord-pads-key )") + (isBlack(pitch) ? "black" : "white") +
					R"(" pitch=")" + std::to_string(pitch) + R"("/>)";
			return rml + "</div>";
		}

		widgets::Options rootKeys()
		{
			widgets::Options options;
			for (int key = 0; key < 12; ++key)
				options.emplace_back(std::to_string(key), g_noteNames.at(static_cast<size_t>(key)));
			return options;
		}

		const widgets::Options& presets()
		{
			static const widgets::Options options = {{"major", "Major (Ionian)"}, {"minor", "Natural Minor (Aeolian)"},
				{"dorian", "Dorian"}, {"phrygian", "Phrygian"}, {"lydian", "Lydian"}, {"mixolydian", "Mixolydian"},
				{"locrian", "Locrian"}};
			return options;
		}

		const widgets::Options& loads()
		{
			static const widgets::Options options = {{"chordsAndPlayers", "Load Chords & Players"},
				{"chords", "Load Chords Only"}, {"players", "Load Players Only"}};
			return options;
		}

		const widgets::Options& overlaps()
		{
			static const widgets::Options options = {
				{"holdFirst", "Hold First"}, {"legato", "Legato"}, {"stopFirst", "Stop First"}};
			return options;
		}

		const widgets::Options& libraries()
		{
			static const widgets::Options options = {
				{"basic", "Basic Player"}, {"piano", "Piano Player"}, {"guitar", "Guitar Player"}};
			return options;
		}

		const widgets::Options& styles(const std::string& library)
		{
			static const widgets::Options piano = {{"triads", "Triads"}, {"triadsMaj9", "Triads Maj9"},
				{"triadsMaj9Min9", "Triads Maj9 Min9"}, {"fourNote", "Four Note"}, {"fourNoteOpen", "Four Note Open"},
				{"fiveNote", "Five Note"}};
			static const widgets::Options guitar = {{"triads", "Triads"}, {"fourNote", "Four Note"},
				{"threeString", "Three String"}, {"modernJazz", "Modern Jazz"}};
			static const widgets::Options none;
			if (library == "piano")
				return piano;
			return library == "guitar" ? guitar : none;
		}

		const widgets::Options& symbols(bool withNone)
		{
			static const widgets::Options primary = {
				{"noteName", "Note Name"}, {"roman", "Roman Numerals"}, {"nashville", "Nashville Number System"}};
			static const widgets::Options secondary = {{"none", "None"}, {"noteName", "Note Name"},
				{"roman", "Roman Numerals"}, {"nashville", "Nashville Number System"}};
			return withNone ? secondary : primary;
		}

		const widgets::Options& layouts()
		{
			static const widgets::Options options = {{"keyboard", "Keyboard"}, {"grid", "Grid"}};
			return options;
		}

		// A pad's chord symbol of a display type (reference manual p. 1390).
		std::string symbol(const Json& pad, const std::string& type)
		{
			if (type == "roman")
				return pad.value("degree", std::string());
			if (type == "nashville")
				return pad.value("nashville", std::string());
			return type == "none" ? std::string() : pad.value("spelled", pad.value("chord", std::string()));
		}

		std::string signedNumber(int value)
		{
			return value > 0 ? "+" + std::to_string(value) : std::to_string(value);
		}

		// The chord editor's buttons (reference manual pp. 1321, 1357): root, type, tensions and bass.
		constexpr std::array<std::pair<const char*, const char*>, 6> g_chordTypes = {
			{{"maj", "Maj"}, {"min", "Min"}, {"dim", "Dim"}, {"aug", "Aug"}, {"sus2", "Sus2"}, {"sus4", "Sus4"}}};
		constexpr std::array<const char*, 10> g_tensions = {
			"6", "7", "maj7", "9", "11", "13", "b9", "#9", "#11", "b13"};

		std::string editorButton(const char* part, const std::string& value, const std::string& label, bool on)
		{
			return std::string(R"(<button class="chord-editor-button chord-editor-)") + part +
				R"(" type="toggle" part=")" + part + R"(" value=")" + encode(value) + '"' +
				(on ? R"( checked="")" : "") + ">" + encode(label) + "</button>";
		}

		const widgets::Options& cadenceTypes()
		{
			static const widgets::Options options = {
				{"all", "All Functions"}, {"tonic", "Tonic"}, {"subdominant", "Subdominant"}, {"dominant", "Dominant"}};
			return options;
		}

		const widgets::Options& algorithms()
		{
			static const widgets::Options options = {{"cadence", "Cadence"}, {"commonNotes", "Common Notes"}};
			return options;
		}

		const widgets::Options& modes()
		{
			static const widgets::Options options = {{"major", "Major"}, {"minor", "Minor"}};
			return options;
		}

		std::string chordButton(const Json& entry, const std::string& extraClass)
		{
			const std::string chord = entry.at("chord").get<std::string>();
			return R"(<button class="chord-suggestion )" + extraClass + R"(" chord=")" + encode(chord) +
				R"("><span class="chord-suggestion-chord">)" + encode(chord) +
				R"(</span><span class="chord-suggestion-degree">)" + encode(entry.value("degree", std::string())) +
				"</span></button>";
		}
	}

	ChordPadsPanel::ChordPadsPanel(
		EngineBinding& binding, Rml::Element& element, DropTarget dropOn, Selection selectedEvents)
		: m_binding(binding), m_dropOn(std::move(dropOn)), m_selectedEvents(std::move(selectedEvents)),
		  m_root(element.GetObserverPtr())
	{
		element.SetInnerRML(R"(<div class="chord-pads"><div class="chord-pads-toolbar">)" +
			button("outputMode", "Chord Pad Output Mode", "Output", true) +
			button("stepInput", "Step Input", "Step", true) + button("moveInsert", "Move Insert Mode", "Insert", true) +
			R"(<select id="chord-pads-root" title="Root Key"/>)" +
			button("transposePads", "Transpose Pads", "Transpose Pads", true) +
			button("flats", "Enh. Shift: Flats", "b", true) +
			R"(<select id="chord-pads-preset" title="Chord Pads Presets"/>)"
			R"(<select id="chord-pads-load" title="Load Preset Settings"/>)" +
			button("loadPreset", "Load Preset", "Load") + button("previousPreset", "Previous Preset", "&lt;") +
			button("nextPreset", "Next Preset", "&gt;") + button("savePreset", "Save Preset", "Save") +
			R"(<input type="text" id="chord-pads-preset-name" class="chord-pads-field" title="Preset Name"/>)" +
			button("fromChordTrack", "Assign from Chord Track", "From Chord Track") +
			button("snapPlayback", "Snap Playback to Musical Grid", "Snap", true) +
			number("velocity", "Velocity on Mouse Click", 1, 127) +
			button("remote", "Pad Remote Control", "Remote", true) +
			number("remoteStart", "Pads Remote Range Start", 0, 127) +
			number("remoteEnd", "Pads Remote Range End", 0, 127) +
			button("assistant", "Show/Hide Editor/Assistant", "Assistant", true) +
			R"(<button pane="player" id="chord-pads-pane-player-button" type="toggle" title="Player Settings">)"
			R"(Player</button><button pane="mode" id="chord-pads-pane-mode-button" type="toggle" )"
			R"(title="Player Mode Settings">Mode</button><button pane="remote" id="chord-pads-pane-remote-button" )"
			R"(type="toggle" title="Chord Pads Remote Settings">Remote Settings</button><button pane="playerRemote" )"
			R"(id="chord-pads-pane-playerRemote-button" type="toggle" title="Player Remote Control">Player Remote)"
			R"(</button><button pane="display" )"
			R"(id="chord-pads-pane-display-button" type="toggle" title="Pad Display Settings">Display</button>)" +
			R"(</div><div class="chord-pads-toolbar chord-pads-extended">)" + button("selectAll", "Select All", "All") +
			button("edit", "Open Chord Editor", "Edit") + button("remove", "Remove Assignment", "Remove") +
			button("transposeDown", "Transpose Down", "-1") + button("transposeUp", "Transpose Up", "+1") +
			button("fewerTensions", "Fewer Tensions", "Fewer") + button("moreTensions", "More Tensions", "More") +
			button("previousVoicing", "Previous Voicing", "&lt;") + button("nextVoicing", "Next Voicing", "&gt;") +
			button("lock", "Lock", "Lock", true) + button("adaptive", "Adaptive Voicing", "Adaptive", true) +
			button("reference", "Adaptive Voicing Reference", "Reference", true) +
			button("assignMidi", "Assign from MIDI Input", "MIDI", true) +
			R"(<input type="text" id="chord-pads-field" class="chord-pads-field" title="Chord"/>)"
			R"(</div><div id="chord-pads-pane-player" class="chord-pads-toolbar chord-pads-pane hidden">)"
			R"(<select id="chord-pads-player" title="Select Player"/><select id="chord-pads-library" )"
			R"(title="Player Type"/>)" +
			button("addPlayer", "Add Player", "Add") +
			R"(<input type="text" id="chord-pads-player-name" title="Rename Selected Player"/>)" +
			button("removePlayer", "Delete Selected Player", "Delete") +
			R"(<select id="chord-pads-style" title="Chord Voicing Style"/>)" +
			number("octaveOffset", "Octave Offset", -3, 3) + R"(<select id="chord-pads-player-track" title="Track"/>)" +
			R"(</div><div id="chord-pads-pane-mode" class="chord-pads-toolbar chord-pads-pane hidden">)"
			R"(<select id="chord-pads-playerMode" title="Player Mode"/><select id="chord-pads-patternPreset" )"
			R"(title="Pattern Preset"/>)" +
			button("patternFromPart", "Use the Selected MIDI Part as the Pattern", "From Part") +
			button("velocityFromKeyboard", "Velocity from MIDI Keyboard", "Keyboard Velocity", true) +
			R"(<span id="chord-pads-pattern-info" class="chord-pads-hint"/>)"
			R"(<select id="chord-pads-overlaps" title="Overlaps"/>)" +
			button("filterNotes", "Filter Notes From MIDI Thru", "Filter Notes", true) +
			R"(</div><div id="chord-pads-pane-remote" class="chord-pads-toolbar chord-pads-pane hidden">)"
			R"(<select id="chord-pads-remotePort" title="MIDI Input"/>)" +
			button("latch", "Latch Chords", "Latch", true) + button("learnRange", "MIDI Learn", "Learn", true) +
			button("modifiers", "Activate Chord Modifiers", "Modifiers", true) + modifierKeys() +
			R"(</div><div id="chord-pads-pane-playerRemote" class="chord-pads-toolbar chord-pads-pane hidden">)"
			R"(<select id="chord-pads-sectionPlay" title="Play Mode"/>)" +
			button("latchPads", "Latch Chord Pads", "Latch Pads", true) +
			button("spread", "Chord Note Distribution: Spread", "Spread", true) +
			fiveKeys("sectionKeys", "Section", 5, -1, 127) + fiveKeys("subsectionKeys", "Subsection", 5, -1, 127) +
			fiveKeys("subsectionOffsets", "Subsection Offset", 5, -48, 48) + subsectionSections() +
			R"(<button action="muteSection" index="0" id="chord-pads-mute-0" type="toggle" title="Mute Section 1">M1)"
			R"(</button><button action="muteSection" index="1" id="chord-pads-mute-1" type="toggle" )"
			R"(title="Mute Section 2">M2</button><button action="muteSection" index="2" id="chord-pads-mute-2" )"
			R"(type="toggle" title="Mute Section 3">M3</button><button action="muteSection" index="3" )"
			R"(id="chord-pads-mute-3" type="toggle" title="Mute Section 4">M4</button><button action="muteSection" )"
			R"(index="4" id="chord-pads-mute-4" type="toggle" title="Mute Section 5">M5</button>)" +
			fiveKeys("playerKeys", "Select/Mute Player Key", 3, -1, 127) +
			R"(</div><div id="chord-pads-pane-display" class="chord-pads-toolbar chord-pads-pane hidden">)"
			R"(<select id="chord-pads-primary" title="Primary Type"/><select id="chord-pads-secondary" )"
			R"(title="Secondary Type"/><select id="chord-pads-layout" title="Pad Layout"/>)"
			R"(<valuefield id="chord-pads-columns" display="columns" min="1" max="16" step="1" decimals="0" )"
			R"(title="Number of Columns"/>)" +
			button("listColors", "Show List Assistant Colors", "Colors", true) +
			button("voicingIndicators", "Show Voicing Indicators", "Voicings", true) +
			R"(</div><div class="chord-pads-body"><div id="chord-pads-pads" class="chord-pads-pads"/>)"
			R"(<div id="chord-pads-assistant-pane" class="chord-pads-assistant hidden"><div class="chord-pads-toolbar">)"
			R"(<button id="chord-pads-page-list" page="list" type="toggle" title="List">List</button>)"
			R"(<button id="chord-pads-page-circle" page="circle" type="toggle" title="Circle of Fifths">)"
			R"(Circle of Fifths</button><button id="chord-pads-page-proximity" page="proximity" type="toggle" )"
			R"(title="Proximity">Proximity</button><button id="chord-pads-page-editor" page="editor" type="toggle" )"
			R"(title="Chord Editor">Editor</button><select id="chord-pads-algorithm" title="Algorithm"/>)"
			R"(<select id="chord-pads-cadenceType" title="Cadence Type"/>)"
			R"(<select id="chord-pads-mode" title="Major/Minor"/></div>)"
			R"(<div id="chord-pads-suggestions" class="chord-pads-suggestions"/></div></div>)" +
			keyboard() + "</div>");
		widgets::fill(*find("chord-pads-overlaps"), overlaps(), "holdFirst");
		widgets::fill(*find("chord-pads-playerMode"),
			{{"plainChords", "Plain Chords"}, {"pattern", "Pattern"}, {"sections", "Sections"}}, "plainChords");
		widgets::fill(*find("chord-pads-sectionPlay"),
			{{"sections", "Sections"}, {"chordPads", "Chord Pads"}, {"combination", "Combination"}}, "sections");
		widgets::fill(*find("chord-pads-library"), libraries(), "basic");
		widgets::fill(*find("chord-pads-primary"), symbols(false), "noteName");
		widgets::fill(*find("chord-pads-secondary"), symbols(true), "roman");
		widgets::fill(*find("chord-pads-layout"), layouts(), "keyboard");
		widgets::fill(*find("chord-pads-root"), rootKeys(), "0");
		widgets::fill(*find("chord-pads-preset"), presets(), "major");
		widgets::fill(*find("chord-pads-load"), loads(), "chordsAndPlayers");
		widgets::fill(*find("chord-pads-algorithm"), algorithms(), "cadence");
		widgets::fill(*find("chord-pads-cadenceType"), cadenceTypes(), "all");
		widgets::fill(*find("chord-pads-mode"), modes(), "major");
		for (const Rml::EventId id : g_events)
			element.AddEventListener(id, this);
		if (Rml::ElementDocument* document = element.GetOwnerDocument(); document != nullptr)
		{
			document->AddEventListener(Rml::EventId::Mouseup, this);
			m_document = document->GetObserverPtr();
		}
		m_subscriptions.push_back(
			m_binding.on("chordPads.changed", [this](const engine::Event& event) { show(event.data); }));
		m_subscriptions.push_back(m_binding.on("chordPads.triggered",
			[this](const engine::Event& event)
			{
				const int pad = event.data.value("pad", -1);
				const bool on = event.data.value("on", false);
				if (const Rml::Element* pads = find("chord-pads-pads");
					pads != nullptr && pad >= 0 && pad < pads->GetNumChildren())
					setClass(pads->GetChild(pad), "triggered", on);
				// The zone's keyboard shows the notes that sound (p. 1355).
				if (on)
					m_sounding[pad] = event.data.value("pitches", std::vector<int>());
				else
					m_sounding.erase(pad);
				showKeyboard();
			}));
		m_subscriptions.push_back(m_binding.on("chordPads.listening",
			[this](const engine::Event& event)
			{
				m_listening = event.data.value("listening", Json());
				showSelection();
			}));
		m_subscriptions.push_back(m_binding.on("settings.changed",
			[this](const engine::Event& event)
			{
				const Json& values = event.data.at("values");
				if (const auto display = values.find(g_displayKey); display != values.end())
				{
					m_display = *display;
					showDisplay();
				}
				if (values.contains("chordPads.presets"))
					requestPresets();
			}));
		m_binding.send("settings.get", {{"key", g_displayKey}},
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (!alive.expired() && result.ok())
				{
					m_display = result.value.at("value");
					showDisplay();
				}
			});
		requestPresets();
		m_subscriptions.push_back(m_binding.on("project.reset", [this](const engine::Event&) { request(); }));
		request();
	}

	ChordPadsPanel::~ChordPadsPanel()
	{
		if (Rml::Element* root = m_root.get())
			for (const Rml::EventId id : g_events)
				root->RemoveEventListener(id, this);
		if (Rml::Element* document = m_document.get())
			document->RemoveEventListener(Rml::EventId::Mouseup, this);
	}

	Rml::Element* ChordPadsPanel::find(const std::string& id) const
	{
		Rml::Element* root = m_root.get();
		return root == nullptr ? nullptr : root->QuerySelector("#" + id);
	}

	void ChordPadsPanel::send(const std::string& command, Json parameters)
	{
		// A change the engine refuses changes nothing: the toolbar shows the pads as they are again, not the value
		// that was refused.
		m_binding.send(command, std::move(parameters),
			[this, command, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (result.ok())
					return;
				logging::warning("{} from the UI failed: {}", command, result.error);
				if (!alive.expired())
					request();
			});
	}

	void ChordPadsPanel::request()
	{
		m_binding.send("chordPads.get", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (!alive.expired() && result.ok())
					show(result.value);
			});
	}

	void ChordPadsPanel::show(const Json& pads)
	{
		Rml::Element* list = find("chord-pads-pads");
		if (list == nullptr)
			return;
		m_pads = pads;
		if (m_rootKeysSending == 0)
			m_rootKeySent.reset(); // the pads have the key the wheel sent last
		const Json& items = pads.at("pads");
		const auto count = static_cast<int>(items.size());
		if (list->GetNumChildren() != count)
		{
			std::string rml;
			for (int i = 0; i < count; ++i)
				rml += R"(<div class="chord-pad" pad=")" + std::to_string(i) +
					R"("><span class="chord-pad-chord"/><span class="chord-pad-degree"/>)"
					R"(<span class="chord-pad-key"/><div class="chord-pad-indicators">)"
					R"(<div class="chord-pad-reference" title="Adaptive Voicing Reference"/>)"
					R"(<div class="chord-pad-adaptive" title="Adaptive Voicing"/>)"
					R"(<svg class="icon chord-pad-lock" src="../icons/general-lock.svg"/></div>)"
					R"(<span class="chord-pad-voicing" title="Voicing"/></div>)";
			list->SetInnerRML(rml);
		}
		for (auto it = m_selection.begin(); it != m_selection.end();)
			it = *it >= count ? m_selection.erase(it) : std::next(it);
		const int reference = pads.at("reference").get<int>();
		for (int i = 0; i < count; ++i)
		{
			const Json& pad = items[static_cast<size_t>(i)];
			Rml::Element* element = list->GetChild(i);
			const std::string chord = pad.at("chord").get<std::string>();
			// The chord symbols of the display settings (reference manual p. 1390).
			setText(element->GetChild(0), symbol(pad, m_display.value("primary", std::string("noteName"))));
			setText(element->GetChild(1), symbol(pad, m_display.value("secondary", std::string("roman"))));
			setText(element->GetChild(4), chord.empty() ? std::string() : signedNumber(pad.at("voicing").get<int>()));
			setText(element->GetChild(2), pad.at("key").is_number() ? noteName(pad.at("key").get<int>()) : "");
			setClass(element, "unassigned", chord.empty());
			setClass(element, "locked", pad.at("locked").get<bool>());
			setClass(element, "adaptive", pad.at("adaptive").get<bool>());
			setClass(element, "reference", i == reference);
		}
		for (const char* setting : {"stepInput", "moveInsert", "remote", "outputMode", "filterNotes", "latch",
				 "modifiers", "snapPlayback", "flats"})
			setChecked(find(std::string("chord-pads-") + setting), pads.at(setting).get<bool>());
		for (const char* setting : {"velocity", "remoteStart", "remoteEnd", "octaveOffset"})
			setValue(find(std::string("chord-pads-") + setting), pads.at(setting).get<int>());
		for (const char* name : g_modifierKeyNames)
			setValue(find(std::string("chord-pads-modifier-") + name), pads.at("modifierKeys").at(name).get<int>());
		widgets::fill(*find("chord-pads-overlaps"), overlaps(), pads.at("overlaps").get<std::string>());
		// The Player Mode settings (pp. 1378-1380): the pattern player's pattern and velocities.
		const std::string mode = pads.at("mode").get<std::string>();
		widgets::fill(*find("chord-pads-playerMode"),
			{{"plainChords", "Plain Chords"}, {"pattern", "Pattern"}, {"sections", "Sections"}}, mode);
		// The sections player mode and the Player Remote Control tab (pp. 1381-1385, 1389).
		widgets::fill(*find("chord-pads-sectionPlay"),
			{{"sections", "Sections"}, {"chordPads", "Chord Pads"}, {"combination", "Combination"}},
			pads.at("sectionPlay").get<std::string>());
		for (const char* setting : {"latchPads", "spread"})
			setChecked(find(std::string("chord-pads-") + setting), pads.at(setting).get<bool>());
		for (const char* setting : {"sectionKeys", "subsectionKeys", "subsectionOffsets", "playerKeys"})
			for (size_t i = 0; i < pads.at(setting).size(); ++i)
				setValue(find(std::string("chord-pads-") + setting + "-" + std::to_string(i)),
					pads.at(setting).at(i).get<int>());
		for (size_t i = 0; i < 5; ++i)
		{
			setChecked(find("chord-pads-mute-" + std::to_string(i)), pads.at("mutedSections").at(i).get<bool>());
			if (Rml::Element* select = find("chord-pads-subsectionSections-" + std::to_string(i)))
				widgets::fill(*select,
					{{"0", "Section 1"}, {"1", "Section 2"}, {"2", "Section 3"}, {"3", "Section 4"},
						{"4", "Section 5"}},
					std::to_string(pads.at("subsectionSections").at(i).get<int>()));
		}
		const Json& pattern = pads.at("pattern");
		const std::string patternName = pattern.at("name").get<std::string>();
		widgets::fill(*find("chord-pads-patternPreset"),
			{{"", "Pattern Preset"}, {"arpeggioUp", "Arpeggio Up"}, {"arpeggioDown", "Arpeggio Down"},
				{"brokenChord", "Broken Chord"}, {"eighths", "Eighths"}},
			"");
		setChecked(find("chord-pads-velocityFromKeyboard"), pads.at("velocityFromKeyboard").get<bool>());
		setText(find("chord-pads-pattern-info"),
			pattern.at("length").get<int64_t>() > 0
				? patternName + " (" + std::to_string(pattern.at("voices").get<int>()) + " voices)"
				: std::string("No pattern"));
		for (const char* id : {"chord-pads-patternPreset", "chord-pads-patternFromPart",
				 "chord-pads-velocityFromKeyboard", "chord-pads-pattern-info"})
			setClass(find(id), "hidden", mode != "pattern");
		setClass(find("chord-pads-overlaps"), "hidden", mode == "pattern");
		setClass(find("chord-pads-filterNotes"), "hidden", mode == "pattern");
		// The Player Settings pane (p. 1377): the players, the selected one's name and its style or octave offset.
		const Json& players = pads.at("players");
		widgets::Options playerOptions;
		for (size_t i = 0; i < players.size(); ++i)
			playerOptions.emplace_back(std::to_string(i), players[i].at("name").get<std::string>());
		const int player = pads.at("player").get<int>();
		widgets::fill(*find("chord-pads-player"), playerOptions, std::to_string(player));
		const Json& selectedPlayer = players.at(static_cast<size_t>(player));
		const std::string library = selectedPlayer.at("library").get<std::string>();
		if (Rml::Element* name = find("chord-pads-player-name");
			name != nullptr && (name->GetContext() == nullptr || name->GetContext()->GetFocusElement() != name))
			name->SetAttribute("value", selectedPlayer.at("name").get<std::string>());
		const std::string style = selectedPlayer.at("style").get<std::string>();
		const widgets::Options& libraryStyles = styles(library);
		widgets::fill(*find("chord-pads-style"), libraryStyles,
			style.empty() && !libraryStyles.empty() ? libraryStyles.front().first : style);
		setClass(find("chord-pads-style"), "hidden", library == "basic");
		setClass(find("chord-pads-octaveOffset"), "hidden", library != "basic");
		setClass(find("chord-pads-removePlayer"), "hidden", players.size() < 2);
		showPlayerTrack();
		showDisplay();
		showKeyboard();
		widgets::fill(*find("chord-pads-root"), rootKeys(), std::to_string(pads.at("rootKey").get<int>()));
		widgets::Options presetOptions = presets();
		for (const Json& name : m_presets)
			if (std::none_of(presetOptions.begin(), presetOptions.end(),
					[&name](const auto& option) { return option.first == name.get<std::string>(); }))
				presetOptions.emplace_back(name.get<std::string>(), name.get<std::string>());
		widgets::fill(*find("chord-pads-preset"), presetOptions, pads.at("preset").get<std::string>());
		showSelection();
		if (Rml::Context* context = list->GetContext())
			context->RequestNextUpdate(0);
	}

	void ChordPadsPanel::showSelection()
	{
		const Rml::Element* list = find("chord-pads-pads");
		if (list == nullptr || !m_pads.contains("pads"))
			return;
		for (int i = 0; i < list->GetNumChildren(); ++i)
			setClass(list->GetChild(i), "selected", m_selection.count(i) != 0);
		// The pad state modifiers show the first selected pad's.
		const Json& pads = m_pads.at("pads");
		const int first = m_selection.empty() ? -1 : *m_selection.begin();
		const Json pad =
			first >= 0 && first < static_cast<int>(pads.size()) ? pads[static_cast<size_t>(first)] : Json();
		setChecked(find("chord-pads-lock"), pad.is_object() && pad.at("locked").get<bool>());
		setChecked(find("chord-pads-adaptive"), pad.is_object() && pad.at("adaptive").get<bool>());
		setChecked(find("chord-pads-reference"), first >= 0 && m_pads.at("reference") == first);
		// Assign from MIDI Input: the pad that gets the chord has a red frame (p. 1354).
		const int listening = m_listening.is_object() ? m_listening.value("assign", -1) : -1;
		for (int i = 0; i < list->GetNumChildren(); ++i)
			setClass(list->GetChild(i), "listening", i == listening);
		setChecked(find("chord-pads-assignMidi"), listening >= 0);
		setChecked(find("chord-pads-learnRange"),
			m_listening.is_object() && m_listening.value("learn", std::string()) == "range");
		showListColors();
		showAssistant();
	}

	void ChordPadsPanel::showAssistant()
	{
		const Rml::Element* assistant = find("chord-pads-assistant-pane");
		if (assistant == nullptr || assistant->IsClassSet("hidden"))
			return;
		setChecked(find("chord-pads-page-list"), m_page == "list");
		setChecked(find("chord-pads-page-circle"), m_page == "circle");
		setChecked(find("chord-pads-page-proximity"), m_page == "proximity");
		setChecked(find("chord-pads-page-editor"), m_page == "editor");
		if (m_page == "editor")
		{
			setClass(find("chord-pads-algorithm"), "hidden", true);
			setClass(find("chord-pads-cadenceType"), "hidden", true);
			setClass(find("chord-pads-mode"), "hidden", true);
			showEditor();
			return;
		}
		setClass(find("chord-pads-algorithm"), "hidden", m_page != "list");
		const auto algorithm = find("chord-pads-algorithm")->GetAttribute<Rml::String>("value", "cadence");
		setClass(find("chord-pads-cadenceType"), "hidden", m_page != "list" || algorithm != "cadence");
		setClass(find("chord-pads-mode"), "hidden", m_page != "circle");
		Json parameters = {{"kind", m_page}};
		if (m_page == "list" || m_page == "proximity")
		{
			// The reference is the first selected pad's chord.
			const int first = m_selection.empty() ? -1 : *m_selection.begin();
			const Json& pads = m_pads.value("pads", Json::array());
			const std::string chord = first >= 0 && first < static_cast<int>(pads.size())
				? pads[static_cast<size_t>(first)].at("chord").get<std::string>()
				: std::string();
			if (chord.empty())
			{
				if (Rml::Element* suggestions = find("chord-pads-suggestions"))
					suggestions->SetInnerRML(R"(<span class="chord-pads-hint">Select a pad with a chord</span>)");
				return;
			}
			parameters["chord"] = chord;
			if (m_page == "list")
			{
				parameters["algorithm"] = algorithm;
				if (algorithm == "cadence")
					parameters["cadenceType"] =
						find("chord-pads-cadenceType")->GetAttribute<Rml::String>("value", "all");
			}
		}
		else
		{
			parameters["minor"] = find("chord-pads-mode")->GetAttribute<Rml::String>("value", "major") == "minor";
		}
		m_binding.send("chordPads.assistant", parameters,
			[this, reference = parameters.value("chord", std::string()), alive = std::weak_ptr<bool>(m_alive)](
				const engine::Result& result)
			{
				Rml::Element* suggestions = alive.expired() || !result.ok() ? nullptr : find("chord-pads-suggestions");
				if (suggestions == nullptr)
					return;
				std::string rml;
				if (m_page == "proximity" && result.value.contains("suggestions"))
				{
					// The Proximity assistant (p. 1374): rows by distance, the farthest at the top, and the reference
					// at the bottom in the middle.
					std::map<int, std::string, std::greater<>> rows;
					for (const Json& entry : result.value.at("suggestions"))
						rows[entry.at("distance").get<int>()] +=
							chordButton(entry, "level-" + std::to_string(entry.at("level").get<int>()));
					for (const auto& [distance, row] : rows)
						rml += R"(<div class="chord-proximity-row" distance=")" + std::to_string(distance) + R"(">)" +
							row + "</div>";
					rml += R"(<div class="chord-proximity-reference">)" + encode(reference) + "</div>";
				}
				else if (result.value.contains("suggestions"))
				{
					for (const Json& entry : result.value.at("suggestions"))
						rml += chordButton(entry, "level-" + std::to_string(entry.at("level").get<int>()));
				}
				else
				{
					for (const char* ring : {"outer", "inner"})
					{
						rml += std::string(R"(<div class="chord-circle-)") + ring + R"(">)";
						for (const Json& entry : result.value.at(ring))
							rml += chordButton(entry, entry.at("degree").get<std::string>().empty() ? "" : "in-key");
						rml += "</div>";
					}
				}
				suggestions->SetInnerRML(rml);
				showRecent();
				if (Rml::Context* context = suggestions->GetContext())
					context->RequestNextUpdate(0);
			});
	}

	void ChordPadsPanel::showRecent()
	{
		Rml::Element* suggestions = find("chord-pads-suggestions");
		if (suggestions == nullptr)
			return;
		Rml::ElementList buttons;
		suggestions->GetElementsByClassName(buttons, "chord-suggestion");
		for (Rml::Element* button : buttons)
			setClass(button, "recent",
				std::find(m_recent.begin(), m_recent.end(), button->GetAttribute<Rml::String>("chord", "")) !=
					m_recent.end());
	}

	std::vector<int> ChordPadsPanel::selected() const
	{
		return {m_selection.begin(), m_selection.end()};
	}

	void ChordPadsPanel::ProcessEvent(Rml::Event& event)
	{
		Rml::Element* target = event.GetTargetElement();
		if (target == nullptr || !m_pads.contains("pads"))
			return;
		const Rml::Element* pad = withAttribute(target, "pad");
		const Rml::Element* suggestion = withAttribute(target, "chord");
		switch (event.GetId())
		{
		case Rml::EventId::Mousedown:
			// Assign to First Unassigned Pad from a suggestion's context click (p. 1375).
			if (event.GetParameter("button", 0) == 1 && suggestion != nullptr)
			{
				assignFirstFree(suggestion->GetAttribute<Rml::String>("chord", ""));
				return;
			}
			if (event.GetParameter("button", 0) != 0)
				return;
			if (pad != nullptr)
			{
				press(pad->GetAttribute<int>("pad", -1), event);
			}
			else if (suggestion != nullptr)
			{
				m_auditioned = suggestion->GetAttribute<Rml::String>("chord", "");
				send("chordPads.trigger", {{"chord", m_auditioned}});
				// The last three chords auditioned show in bold (pp. 1374-1375).
				m_recent.erase(std::remove(m_recent.begin(), m_recent.end(), m_auditioned), m_recent.end());
				m_recent.insert(m_recent.begin(), m_auditioned);
				if (m_recent.size() > 3)
					m_recent.resize(3);
				showRecent();
			}
			else if (const Rml::Element* part = withAttribute(target, "part"))
			{
				pressEditor(*part);
			}
			else if (const Rml::Element* actor = withAttribute(target, "action");
				actor != nullptr && modifies(actor->GetAttribute<Rml::String>("action", "")))
			{
				modifyPressed(actor->GetAttribute<Rml::String>("action", ""));
			}
			return;
		case Rml::EventId::Mouseover:
		case Rml::EventId::Mousemove:
		case Rml::EventId::Mouseout:
		{
			// The hovered pad's trigger key lights on the keyboard (p. 1355).
			const int hovered =
				event.GetId() != Rml::EventId::Mouseout && pad != nullptr ? pad->GetAttribute<int>("pad", -1) : -1;
			if (hovered != m_hovered)
			{
				m_hovered = hovered;
				showKeyboard();
			}
			return;
		}
		case Rml::EventId::Mouseup:
			release(target, event);
			return;
		case Rml::EventId::Dblclick:
			if (pad != nullptr)
				openField(pad->GetAttribute<int>("pad", -1));
			else if (suggestion != nullptr)
				assignFirstFree(suggestion->GetAttribute<Rml::String>("chord", ""));
			return;
		case Rml::EventId::Mousescroll:
			if (pad != nullptr)
			{
				wheel(pad->GetAttribute<int>("pad", -1), event);
				event.StopPropagation();
			}
			else if (within(target, "chord-pads-root"))
			{
				wheelRootKey(event);
				event.StopPropagation();
			}
			return;
		case Rml::EventId::Keydown:
		{
			// With step input, Left and Right Arrow move the step input cursor by a bar wherever the focus is in the
			// zone, such as on the Step Input button just turned on (reference manual p. 1369), except in the fields
			// and menus, which take them. With a modifier they are other key commands, such as Cmd+Left and Cmd+Right
			// on macOS (edit.nudgeLeft and edit.nudgeRight, #1174).
			keyCommand(event, target);
			if (!event.IsPropagating())
				return;
			const auto key = event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN);
			const bool modified = event.GetParameter("ctrl_key", 0) != 0 || event.GetParameter("shift_key", 0) != 0 ||
				event.GetParameter("alt_key", 0) != 0 || event.GetParameter("meta_key", 0) != 0;
			if (!takesArrows(target) && m_pads.at("stepInput").get<bool>() && !modified &&
				(key == Rml::Input::KI_LEFT || key == Rml::Input::KI_RIGHT))
			{
				send("chordPads.set", {{"stepBars", key == Rml::Input::KI_RIGHT ? 1 : -1}});
				event.StopPropagation();
			}
			return;
		}
		case Rml::EventId::Change:
			change(*target, event);
			return;
		case Rml::EventId::Click:
			if (const Rml::Element* page = withAttribute(target, "page"))
			{
				m_page = page->GetAttribute<Rml::String>("page", "list");
				showAssistant();
			}
			else if (const Rml::Element* pane = withAttribute(target, "pane"))
			{
				// The settings panes, one at a time: Player Settings, Player Mode, Remote Settings and Display.
				const auto name = pane->GetAttribute<Rml::String>("pane", "");
				for (const char* each : {"player", "mode", "remote", "playerRemote", "display"})
				{
					Rml::Element* shown = find(std::string("chord-pads-pane-") + each);
					const bool show = name == each && shown != nullptr && shown->IsClassSet("hidden");
					setClass(shown, "hidden", !show);
					setChecked(find(std::string("chord-pads-pane-") + each + "-button"), show);
				}
				if (name == "remote")
					requestPorts();
				if (name == "player")
					requestTracks();
			}
			else if (const Rml::Element* actor = withAttribute(target, "action"))
			{
				const auto name = actor->GetAttribute<Rml::String>("action", "");
				if (name == "muteSection")
				{
					// Mute Sections (p. 1384): the button's section.
					Json muted = m_pads.at("mutedSections");
					const auto index = static_cast<size_t>(actor->GetAttribute<int>("index", 0));
					if (index < muted.size())
					{
						muted.at(index) = !muted.at(index).get<bool>();
						send("chordPads.set", {{"mutedSections", muted}});
					}
				}
				else if (modifies(name) && m_pressedModifier)
				{
					m_pressedModifier = false; // done on the press
				}
				else
				{
					action(name);
				}
			}
			return;
		default:
			return;
		}
	}

	void ChordPadsPanel::press(int pad, const Rml::Event& event)
	{
		if (pad < 0)
			return;
		const bool ctrl = event.GetParameter("ctrl_key", 0) != 0 || event.GetParameter("meta_key", 0) != 0;
		if (ctrl)
		{
			if (m_selection.erase(pad) == 0)
				m_selection.insert(pad);
		}
		else if (m_selection.count(pad) == 0)
		{
			m_selection = {pad};
		}
		showSelection();
		m_pressed = pad;
		if (m_pads.at("stepInput").get<bool>())
			send("chordPads.step", {{"pad", pad}});
		send("chordPads.trigger", {{"pad", pad}});
	}

	void ChordPadsPanel::release(Rml::Element* target, const Rml::Event& event)
	{
		if (m_auditionPad >= 0)
		{
			send("chordPads.trigger", {{"pad", m_auditionPad}, {"on", false}});
			m_auditionPad = -1;
		}
		if (!m_auditioned.empty())
		{
			send("chordPads.trigger", {{"chord", m_auditioned}, {"on", false}});
			// An assistant's chord dropped on a pad goes onto it (pp. 1357-1359).
			const Rml::Element* over = withAttribute(target, "pad");
			bool inPanel = false;
			for (const Rml::Element* parent = over; parent != nullptr && !inPanel; parent = parent->GetParentNode())
				inPanel = parent == m_root.get();
			const int to = over == nullptr || !inPanel ? -1 : over->GetAttribute<int>("pad", -1);
			const Json& items = m_pads.at("pads");
			if (to >= 0 && to < static_cast<int>(items.size()) &&
				!items[static_cast<size_t>(to)].at("locked").get<bool>())
			{
				send("chordPads.assign", {{"assignments", {{{"pad", to}, {"chord", m_auditioned}}}}});
				m_selection = {to};
				showSelection();
			}
			m_auditioned.clear();
		}
		if (m_pressed < 0)
			return;
		const int from = m_pressed;
		m_pressed = -1;
		send("chordPads.trigger", {{"pad", from}, {"on", false}});
		// Released over another pad of the panel: swapped, or with Alt copied.
		const Rml::Element* over = withAttribute(target, "pad");
		bool inPanel = false;
		for (const Rml::Element* parent = over; parent != nullptr && !inPanel; parent = parent->GetParentNode())
			inPanel = parent == m_root.get();
		const int to = over == nullptr || !inPanel ? -1 : over->GetAttribute<int>("pad", -1);
		if (to >= 0 && to != from)
		{
			send("chordPads.move", {{"from", from}, {"to", to}, {"copy", event.GetParameter("alt_key", 0) != 0}});
			m_selection = {to};
			showSelection();
		}
		// Released outside the zone over a track's lane: onto the chord track as a chord event, onto a MIDI or
		// instrument track as a part a bar long (reference manual p. 1370).
		else if (!inPanel && m_dropOn &&
			!m_pads.at("pads")[static_cast<size_t>(from)].at("chord").get<std::string>().empty())
		{
			if (const auto drop =
					m_dropOn(event.GetParameter<float>("mouse_x", 0.0F), event.GetParameter<float>("mouse_y", 0.0F)))
				send("chordPads.toTrack", {{"pad", from}, {"track", drop->first}, {"position", drop->second}});
		}
	}

	void ChordPadsPanel::wheel(int pad, const Rml::Event& event)
	{
		if (pad < 0)
			return;
		const auto delta = event.GetParameter<float>("wheel_delta_y", 0.0F);
		if (delta == 0.0F)
			return;
		const int up = delta < 0.0F ? 1 : -1; // the wheel turned away from the user
		Json parameters = {{"pads", {pad}}};
		if (event.GetParameter("shift_key", 0) != 0)
			parameters["transpose"] = up;
		else if (event.GetParameter("alt_key", 0) != 0)
			parameters["tensions"] = up;
		else
			parameters["voicing"] = -up; // up: the previous voicing
		send("chordPads.modify", parameters);
	}

	void ChordPadsPanel::wheelRootKey(const Rml::Event& event)
	{
		const auto delta = event.GetParameter<float>("wheel_delta_y", 0.0F);
		if (delta == 0.0F)
			return;
		// Until the engine's pads come back with the key, m_pads holds an older one: the next notch goes on from the
		// key sent last, so that notches before the engine's answer all count.
		const int key = (m_rootKeySent.value_or(m_pads.at("rootKey").get<int>()) + (delta > 0.0F ? 1 : 11)) % 12;
		m_rootKeySent = key;
		++m_rootKeysSending;
		const Rml::Element* transpose = find("chord-pads-transposePads");
		m_binding.send("chordPads.set",
			{{"rootKey", key}, {"transposePads", transpose != nullptr && transpose->HasAttribute("checked")}},
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired())
					return;
				--m_rootKeysSending;
				if (result.ok())
					return;
				logging::warning("chordPads.set from the UI failed: {}", result.error);
				m_rootKeySent.reset();
				request();
			});
	}

	void ChordPadsPanel::action(const std::string& name)
	{
		const std::vector<int> pads = selected();
		const int first = pads.empty() ? -1 : pads.front();
		const Json& items = m_pads.at("pads");
		if (name == "stepInput" || name == "moveInsert" || name == "remote" || name == "outputMode" ||
			name == "filterNotes" || name == "latch" || name == "modifiers" || name == "snapPlayback" ||
			name == "flats" || name == "latchPads" || name == "spread" || name == "velocityFromKeyboard")
		{
			send("chordPads.set", {{name, !m_pads.at(name).get<bool>()}});
		}
		else if (name == "previousPreset" || name == "nextPreset")
		{
			// Previous and Next Preset (p. 1348): the neighbor of the loaded one in chordPads.presets' order.
			const auto current = std::find(m_presets.begin(), m_presets.end(), m_pads.at("preset"));
			if (m_presets.empty())
				return;
			const auto count = static_cast<std::ptrdiff_t>(m_presets.size());
			const std::ptrdiff_t at = current == m_presets.end() ? 0 : current - m_presets.begin();
			const std::ptrdiff_t next = (at + (name == "nextPreset" ? 1 : count - 1)) % count;
			send("chordPads.loadPreset",
				{{"preset", m_presets[static_cast<size_t>(next)]},
					{"load", find("chord-pads-load")->GetAttribute<Rml::String>("value", "chordsAndPlayers")}});
		}
		else if (name == "savePreset")
		{
			Rml::Element* field = find("chord-pads-preset-name");
			setClass(field, "shown", true);
			field->SetAttribute("value", "");
			field->Focus();
		}
		else if (name == "patternFromPart" && m_selectedEvents)
		{
			// A MIDI part dropped on the Player Mode section becomes the pattern (p. 1379): here the selected one.
			const std::vector<uint64_t> events = m_selectedEvents();
			if (!events.empty())
				send("chordPads.setPattern", {{"part", events.front()}});
		}
		else if (name == "addPlayer")
		{
			send("chordPads.addPlayer",
				{{"library", find("chord-pads-library")->GetAttribute<Rml::String>("value", "basic")}});
		}
		else if (name == "removePlayer")
		{
			send("chordPads.removePlayer", Json::object());
		}
		else if (name == "assignMidi")
		{
			// Assign from MIDI Input (p. 1360) for the first selected pad, or off again.
			if (m_listening.is_object() || first < 0)
				send("chordPads.listen", Json::object());
			else
				send("chordPads.listen", {{"assign", first}});
		}
		else if (name == "learnRange")
		{
			if (m_listening.is_object())
				send("chordPads.listen", Json::object());
			else
				send("chordPads.listen", {{"learn", "range"}});
		}
		else if (name == "listColors" || name == "voicingIndicators")
		{
			Json display = m_display.is_object() ? m_display : Json::object();
			const bool on = display.value(name, name == "voicingIndicators");
			display[name] = !on;
			m_binding.send("settings.set", {{"key", g_displayKey}, {"value", display}});
		}
		else if (name == "loadPreset")
		{
			send("chordPads.loadPreset",
				{{"preset", find("chord-pads-preset")->GetAttribute<Rml::String>("value", "major")},
					{"load", find("chord-pads-load")->GetAttribute<Rml::String>("value", "chordsAndPlayers")}});
		}
		else if (name == "fromChordTrack")
		{
			send("chordPads.fromChordTrack", Json::object());
		}
		else if (name == "assistant")
		{
			Rml::Element* pane = find("chord-pads-assistant-pane");
			setClass(pane, "hidden", !pane->IsClassSet("hidden")); // the toggle turns itself on or off
			showAssistant();
		}
		else if (name == "selectAll")
		{
			for (int i = 0; i < static_cast<int>(items.size()); ++i)
				m_selection.insert(i);
			showSelection();
		}
		else if (name == "edit" && first >= 0)
		{
			openField(first);
		}
		else if (name == "remove" && !pads.empty())
		{
			Json assignments = Json::array();
			for (const int pad : pads)
				if (!items[static_cast<size_t>(pad)].at("locked").get<bool>())
					assignments.push_back({{"pad", pad}, {"chord", ""}});
			send("chordPads.assign", {{"assignments", assignments}});
		}
		else if (!pads.empty() && (name == "transposeDown" || name == "transposeUp"))
		{
			send("chordPads.modify", {{"pads", pads}, {"transpose", name == "transposeUp" ? 1 : -1}});
		}
		else if (!pads.empty() && (name == "fewerTensions" || name == "moreTensions"))
		{
			send("chordPads.modify", {{"pads", pads}, {"tensions", name == "moreTensions" ? 1 : -1}});
		}
		else if (!pads.empty() && (name == "previousVoicing" || name == "nextVoicing"))
		{
			send("chordPads.modify", {{"pads", pads}, {"voicing", name == "nextVoicing" ? 1 : -1}});
		}
		else if (first >= 0 && (name == "lock" || name == "adaptive"))
		{
			// One undo step for all the selected pads. Locked pads refuse adaptive voicing, which would fail the
			// whole transaction, so they are left out of turning it on.
			const char* state = name == "lock" ? "locked" : "adaptive";
			const bool on = !items[static_cast<size_t>(first)].at(state).get<bool>();
			const bool skipLocked = on && name == "adaptive";
			Json commands = Json::array();
			for (const int pad : pads)
				if (!skipLocked || !items[static_cast<size_t>(pad)].at("locked").get<bool>())
					commands.push_back({{"command", "chordPads.setPad"}, {"parameters", {{"pad", pad}, {state, on}}}});
			if (!commands.empty())
				send("history.transaction", {{"description", "Change chord pads"}, {"commands", commands}});
		}
		else if (first >= 0 && name == "reference")
		{
			send("chordPads.setPad", {{"pad", first}, {"reference", m_pads.at("reference") != first}});
		}
	}

	void ChordPadsPanel::change(Rml::Element& target, const Rml::Event& event)
	{
		const std::string& id = target.GetId();
		if (id == "chord-pads-field")
		{
			if (event.GetParameter<bool>("linebreak", false))
				assignTyped();
			return;
		}
		if (id == "chord-pads-preset-name" || id == "chord-pads-player-name")
		{
			if (!event.GetParameter<bool>("linebreak", false))
				return;
			const auto name = target.GetAttribute<Rml::String>("value", "");
			if (id == "chord-pads-preset-name")
			{
				setClass(&target, "shown", false);
				if (!name.empty())
					send("chordPads.savePreset", {{"name", name}});
			}
			else if (!name.empty())
			{
				send("chordPads.setPlayer", {{"name", name}});
			}
			return;
		}
		if (target.GetTagName() == "valuefield" && target.HasAttribute("five"))
		{
			const auto setting = target.GetAttribute<Rml::String>("five", "");
			const auto index = static_cast<size_t>(target.GetAttribute<int>("index", 0));
			Json values = m_pads.value(setting, Json::array());
			const auto value = static_cast<int>(event.GetParameter<float>("value", 0.0F));
			if (index < values.size() && values.at(index) != value)
			{
				values.at(index) = value;
				send("chordPads.set", {{setting, values}});
			}
			return;
		}
		if (target.GetTagName() == "valuefield" && (target.HasAttribute("modifier") || target.HasAttribute("display")))
		{
			const auto value = static_cast<int>(event.GetParameter<float>("value", 0.0F));
			if (target.HasAttribute("modifier"))
			{
				const auto name = target.GetAttribute<Rml::String>("modifier", "");
				if (m_pads.at("modifierKeys").value(name, -1) != value)
					send("chordPads.set", {{"modifierKeys", {{name, value}}}});
			}
			else if (m_display.value("columns", 6) != value)
			{
				Json display = m_display.is_object() ? m_display : Json::object();
				display["columns"] = value;
				m_binding.send("settings.set", {{"key", g_displayKey}, {"value", display}});
			}
			return;
		}
		if (target.GetTagName() == "valuefield" && target.HasAttribute("setting"))
		{
			const auto value = static_cast<int>(event.GetParameter<float>("value", 0.0F));
			const auto setting = target.GetAttribute<Rml::String>("setting", "");
			if (m_pads.contains(setting) && m_pads.at(setting) != value)
				send("chordPads.set", {{setting, value}});
			return;
		}
		const auto value = event.GetParameter<Rml::String>("value", "");
		if (target.GetTagName() == "select" && target.HasAttribute("five") && !value.empty())
		{
			const auto setting = target.GetAttribute<Rml::String>("five", "");
			const auto index = static_cast<size_t>(target.GetAttribute<int>("index", 0));
			Json values = m_pads.value(setting, Json::array());
			if (index < values.size() && std::to_string(values.at(index).get<int>()) != value)
			{
				values.at(index) = std::stoi(value);
				send("chordPads.set", {{setting, values}});
			}
			return;
		}
		if (id == "chord-pads-root" && !value.empty() && std::to_string(m_pads.at("rootKey").get<int>()) != value)
		{
			const bool transpose = find("chord-pads-transposePads")->HasAttribute("checked");
			send("chordPads.set", {{"rootKey", std::stoi(value)}, {"transposePads", transpose}});
		}
		else if (id == "chord-pads-algorithm" || id == "chord-pads-mode" || id == "chord-pads-cadenceType")
		{
			showAssistant();
		}
		else if (id == "chord-pads-playerMode" && !value.empty() && m_pads.at("mode") != value)
		{
			send("chordPads.set", {{"mode", value}});
		}
		else if (id == "chord-pads-sectionPlay" && !value.empty() && m_pads.at("sectionPlay") != value)
		{
			send("chordPads.set", {{"sectionPlay", value}});
		}
		else if (id == "chord-pads-patternPreset" && !value.empty())
		{
			send("chordPads.setPattern", {{"preset", value}});
		}
		else if (id == "chord-pads-overlaps" && !value.empty() && m_pads.at("overlaps") != value)
		{
			send("chordPads.set", {{"overlaps", value}});
		}
		else if (id == "chord-pads-player" && !value.empty() && std::to_string(m_pads.at("player").get<int>()) != value)
		{
			send("chordPads.set", {{"player", std::stoi(value)}});
		}
		else if (id == "chord-pads-style" && !value.empty())
		{
			const Json& player = m_pads.at("players").at(static_cast<size_t>(m_pads.at("player").get<int>()));
			if (player.at("library") != "basic" && player.at("style") != value)
				send("chordPads.setPlayer", {{"style", value}});
		}
		else if (id == "chord-pads-player-track" && !value.empty())
		{
			const Json& player = m_pads.at("players").at(static_cast<size_t>(m_pads.at("player").get<int>()));
			if (std::to_string(player.at("track").get<uint64_t>()) != value)
				send("chordPads.setPlayer", {{"track", std::stoull(value)}});
		}
		else if (id == "chord-pads-remotePort" && m_pads.at("remotePort") != value)
		{
			send("chordPads.set", {{"remotePort", value}});
		}
		else if ((id == "chord-pads-primary" || id == "chord-pads-secondary" || id == "chord-pads-layout") &&
			!value.empty())
		{
			const std::string setting = id.substr(std::string("chord-pads-").size());
			Json display = m_display.is_object() ? m_display : Json::object();
			if (display.value(setting, std::string()) != value)
			{
				display[setting] = value;
				m_binding.send("settings.set", {{"key", g_displayKey}, {"value", display}});
			}
		}
	}

	void ChordPadsPanel::openField(int pad)
	{
		Rml::Element* field = find("chord-pads-field");
		if (field == nullptr || pad < 0 || pad >= static_cast<int>(m_pads.at("pads").size()))
			return;
		m_editing = pad;
		field->SetAttribute("value", m_pads.at("pads")[static_cast<size_t>(pad)].at("chord").get<std::string>());
		setClass(field, "shown", true);
		field->Focus();
	}

	void ChordPadsPanel::assignTyped()
	{
		Rml::Element* field = find("chord-pads-field");
		if (field == nullptr || m_editing < 0)
			return;
		const int pad = m_editing;
		m_editing = -1;
		setClass(field, "shown", false);
		send("chordPads.assign",
			{{"assignments", {{{"pad", pad}, {"chord", field->GetAttribute<Rml::String>("value", "")}}}}});
	}

	void ChordPadsPanel::keyCommand(Rml::Event& event, const Rml::Element* target)
	{
		// The pads' key commands while the zone has the focus (pp. 1351, 1357, 1362): Ctrl/Cmd+A selects all, Delete
		// and Backspace remove the selected pads' assignments, Return opens the chord field, Ctrl/Cmd+Shift+L locks.
		if (takesArrows(target))
			return;
		const auto key = event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN);
		const bool command = event.GetParameter("ctrl_key", 0) != 0 || event.GetParameter("meta_key", 0) != 0;
		const bool shift = event.GetParameter("shift_key", 0) != 0;
		const bool alt = event.GetParameter("alt_key", 0) != 0;
		std::string name;
		if (key == Rml::Input::KI_A && command && !shift && !alt)
			name = "selectAll";
		else if ((key == Rml::Input::KI_DELETE || key == Rml::Input::KI_BACK) && !command && !shift && !alt)
			name = "remove";
		else if (key == Rml::Input::KI_RETURN && !command && !shift && !alt)
			name = "edit";
		else if (key == Rml::Input::KI_L && command && shift && !alt)
			name = "lock";
		if (name.empty() || (name != "selectAll" && m_selection.empty()))
			return;
		action(name);
		event.StopPropagation();
	}

	bool ChordPadsPanel::modifies(const std::string& name)
	{
		return std::find(g_modifierActions.begin(), g_modifierActions.end(), name) != g_modifierActions.end();
	}

	void ChordPadsPanel::modifyPressed(const std::string& name)
	{
		// The pad modifiers play the first selected pad's new chord while their button is down (p. 1351).
		const std::vector<int> pads = selected();
		if (pads.empty())
			return;
		action(name);
		m_pressedModifier = true;
		m_auditionPad = pads.front();
		send("chordPads.trigger", {{"pad", m_auditionPad}});
	}

	void ChordPadsPanel::showDisplay()
	{
		// The Chord Pads Display Settings pane (pp. 1390-1391), the user setting ui.chordPads.
		Rml::Element* list = find("chord-pads-pads");
		if (list == nullptr)
			return;
		const std::string layout = m_display.value("layout", std::string("keyboard"));
		const int columns = std::clamp(m_display.value("columns", 6), 1, 16);
		widgets::fill(*find("chord-pads-primary"), symbols(false), m_display.value("primary", std::string("noteName")));
		widgets::fill(*find("chord-pads-secondary"), symbols(true), m_display.value("secondary", std::string("roman")));
		widgets::fill(*find("chord-pads-layout"), layouts(), layout);
		setValue(find("chord-pads-columns"), columns);
		setClass(find("chord-pads-columns"), "hidden", layout != "grid");
		const bool voicings = m_display.value("voicingIndicators", true);
		setChecked(find("chord-pads-listColors"), m_display.value("listColors", false));
		setChecked(find("chord-pads-voicingIndicators"), voicings);
		setClass(list, "grid", layout == "grid");
		setClass(list, "no-voicings", !voicings);
		// The grid's columns come from the setting: the pads' width is geometry, the look stays in the style sheets.
		for (int i = 0; i < list->GetNumChildren(); ++i)
		{
			Rml::Element* pad = list->GetChild(i);
			if (layout == "grid")
				pad->SetProperty("width", std::to_string(100.0F / static_cast<float>(columns)) + "%");
			else
				pad->RemoveProperty("width");
			// In the keyboard layout the pads on black keys of the remote range look like them.
			const Json& key = m_pads.contains("pads") && i < static_cast<int>(m_pads.at("pads").size())
				? m_pads.at("pads")[static_cast<size_t>(i)].at("key")
				: Json();
			setClass(pad, "black-key", layout == "keyboard" && key.is_number() && isBlack(key.get<int>()));
		}
		if (m_pads.contains("pads"))
		{
			const Json& items = m_pads.at("pads");
			for (int i = 0; i < list->GetNumChildren() && i < static_cast<int>(items.size()); ++i)
			{
				const Rml::Element* element = list->GetChild(i);
				const Json& pad = items[static_cast<size_t>(i)];
				setText(element->GetChild(0), symbol(pad, m_display.value("primary", std::string("noteName"))));
				setText(element->GetChild(1), symbol(pad, m_display.value("secondary", std::string("roman"))));
			}
		}
		showListColors();
	}

	void ChordPadsPanel::showListColors()
	{
		// List Assistant Color Indicator (p. 1353): the pads' chords as the List assistant colors them after the first
		// selected pad's chord.
		const Rml::Element* list = find("chord-pads-pads");
		if (list == nullptr || !m_pads.contains("pads"))
			return;
		const Json& items = m_pads.at("pads");
		const int first = m_selection.empty() ? -1 : *m_selection.begin();
		const std::string reference = first >= 0 && first < static_cast<int>(items.size())
			? items[static_cast<size_t>(first)].at("chord").get<std::string>()
			: std::string();
		const auto clear = [list]
		{
			for (int i = 0; i < list->GetNumChildren(); ++i)
				for (const char* level : {"level-0", "level-1", "level-2"})
					setClass(list->GetChild(i), level, false);
		};
		if (!m_display.value("listColors", false) || reference.empty())
		{
			clear();
			return;
		}
		m_binding.send("chordPads.assistant", {{"kind", "list"}, {"chord", reference}},
			[this, clear, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				const Rml::Element* pads = alive.expired() || !result.ok() ? nullptr : find("chord-pads-pads");
				if (pads == nullptr || !m_pads.contains("pads"))
					return;
				clear();
				const Json& items = m_pads.at("pads");
				for (const Json& suggestion : result.value.at("suggestions"))
					for (int i = 0; i < pads->GetNumChildren() && i < static_cast<int>(items.size()); ++i)
						if (items[static_cast<size_t>(i)].at("chord") == suggestion.at("chord"))
							setClass(pads->GetChild(i),
								("level-" + std::to_string(suggestion.at("level").get<int>())).c_str(), true);
			});
	}

	void ChordPadsPanel::showKeyboard()
	{
		// The zone's keyboard (p. 1355): the notes that sound, the pads' remote range, the chord modifiers' keys
		// while active, and the hovered pad's trigger key.
		const Rml::Element* keys = find("chord-pads-keyboard");
		if (keys == nullptr || !m_pads.contains("pads"))
			return;
		std::set<int> played;
		for (const auto& [pad, pitches] : m_sounding)
			played.insert(pitches.begin(), pitches.end());
		const int start = m_pads.at("remoteStart").get<int>();
		const int end =
			std::min(m_pads.at("remoteEnd").get<int>(), start + static_cast<int>(m_pads.at("pads").size()) - 1);
		const bool modifiers = m_pads.at("modifiers").get<bool>();
		std::set<int> modifierKeys;
		for (const auto& [name, value] : m_pads.at("modifierKeys").items())
			modifierKeys.insert(value.get<int>());
		const Json hoveredKey = m_hovered >= 0 && m_hovered < static_cast<int>(m_pads.at("pads").size())
			? m_pads.at("pads")[static_cast<size_t>(m_hovered)].at("key")
			: Json();
		for (int i = 0; i < keys->GetNumChildren(); ++i)
		{
			Rml::Element* key = keys->GetChild(i);
			const int pitch = g_lowestKey + i;
			setClass(key, "played", played.count(pitch) != 0);
			setClass(key, "remote", m_pads.at("remote").get<bool>() && pitch >= start && pitch <= end);
			setClass(key, "modifier", m_pads.at("remote").get<bool>() && modifiers && modifierKeys.count(pitch) != 0);
			setClass(key, "hover", hoveredKey.is_number() && hoveredKey.get<int>() == pitch);
		}
	}

	void ChordPadsPanel::requestPresets()
	{
		m_binding.send("chordPads.presets", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok())
					return;
				m_presets = result.value.at("presets");
				if (m_pads.contains("pads"))
					show(m_pads);
			});
	}

	void ChordPadsPanel::showEditor()
	{
		// The chord editor (pp. 1321, 1357) for the first selected pad: its root, type, tensions and bass lit.
		Rml::Element* suggestions = find("chord-pads-suggestions");
		if (suggestions == nullptr || !m_pads.contains("pads"))
			return;
		const Json& pads = m_pads.at("pads");
		const int first = m_selection.empty() ? -1 : *m_selection.begin();
		if (first < 0 || first >= static_cast<int>(pads.size()))
		{
			suggestions->SetInnerRML(R"(<span class="chord-pads-hint">Select a pad</span>)");
			return;
		}
		const Json& symbol = pads[static_cast<size_t>(first)].at("symbol");
		const int chordRoot = symbol.at("root").get<int>();
		const auto type = symbol.at("type").get<std::string>();
		const auto tensions = symbol.at("tensions").get<std::vector<std::string>>();
		const int bass = symbol.at("bass").get<int>();
		std::string rml = R"(<div class="chord-editor-row">)";
		for (int key = 0; key < 12; ++key)
			rml +=
				editorButton("root", std::to_string(key), g_noteNames.at(static_cast<size_t>(key)), key == chordRoot);
		rml += R"(</div><div class="chord-editor-row">)";
		for (const auto& [name, label] : g_chordTypes)
			rml += editorButton("type", name, label, chordRoot >= 0 && type == name);
		rml += R"(</div><div class="chord-editor-row">)";
		for (const char* tension : g_tensions)
			rml += editorButton(
				"tension", tension, tension, std::find(tensions.begin(), tensions.end(), tension) != tensions.end());
		rml +=
			R"(</div><div class="chord-editor-row">)" + editorButton("bass", "-1", "Root", chordRoot >= 0 && bass < 0);
		for (int key = 0; key < 12; ++key)
			rml += editorButton(
				"bass", std::to_string(key), "/" + std::string(g_noteNames.at(static_cast<size_t>(key))), key == bass);
		suggestions->SetInnerRML(rml + "</div>");
		if (Rml::Context* context = suggestions->GetContext())
			context->RequestNextUpdate(0);
	}

	void ChordPadsPanel::pressEditor(const Rml::Element& button)
	{
		// A chord definition button assigns the chord it makes to the first selected pad and plays it while it is
		// pressed (p. 1357).
		const Json& pads = m_pads.at("pads");
		const int first = m_selection.empty() ? -1 : *m_selection.begin();
		if (first < 0 || first >= static_cast<int>(pads.size()) ||
			pads[static_cast<size_t>(first)].at("locked").get<bool>())
			return;
		Json symbol = pads[static_cast<size_t>(first)].at("symbol");
		const auto part = button.GetAttribute<Rml::String>("part", "");
		const auto value = button.GetAttribute<Rml::String>("value", "");
		if (part == "root")
		{
			symbol["root"] = std::stoi(value);
		}
		else if (part == "type")
		{
			symbol["type"] = value;
		}
		else if (part == "bass")
		{
			symbol["bass"] = std::stoi(value);
		}
		else if (part == "tension")
		{
			std::vector<std::string> tensions = symbol.at("tensions").get<std::vector<std::string>>();
			const auto found = std::find(tensions.begin(), tensions.end(), value);
			if (found == tensions.end())
				tensions.push_back(value);
			else
				tensions.erase(found);
			symbol["tensions"] = tensions;
		}
		if (symbol.at("root").get<int>() < 0)
			return; // a root first
		send("chordPads.assign", {{"assignments", {{{"pad", first}, {"symbol", symbol}}}}});
		m_auditionPad = first;
		send("chordPads.trigger", {{"pad", first}});
	}

	void ChordPadsPanel::requestTracks()
	{
		// Using Different Players on Multiple Tracks (p. 1377): the MIDI and instrument tracks a player can play.
		m_binding.send("project.get", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (alive.expired() || !result.ok())
					return;
				m_tracks = {{"0", "No Track"}};
				for (const Json& track : result.value.at("tracks"))
					if (track.at("type") == "midi" || track.at("type") == "instrument")
						m_tracks.emplace_back(
							std::to_string(track.at("id").get<uint64_t>()), track.at("name").get<std::string>());
				showPlayerTrack();
			});
	}

	void ChordPadsPanel::showPlayerTrack()
	{
		Rml::Element* select = find("chord-pads-player-track");
		if (select == nullptr || !m_pads.contains("players"))
			return;
		const Json& player = m_pads.at("players").at(static_cast<size_t>(m_pads.at("player").get<int>()));
		const std::string track = std::to_string(player.at("track").get<uint64_t>());
		widgets::Options options = m_tracks.empty() ? widgets::Options{{"0", "No Track"}} : m_tracks;
		if (std::none_of(
				options.begin(), options.end(), [&track](const auto& option) { return option.first == track; }))
			options.emplace_back(track, "Track " + track);
		widgets::fill(*select, options, track);
	}

	void ChordPadsPanel::requestPorts()
	{
		m_binding.send("midi.ports", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				Rml::Element* select = alive.expired() || !result.ok() ? nullptr : find("chord-pads-remotePort");
				if (select == nullptr || !m_pads.contains("pads"))
					return;
				widgets::Options options{{"", "All MIDI Inputs"}};
				for (const Json& port : result.value.at("ports"))
					if (port.at("input").get<bool>() && port.at("visible").get<bool>())
						options.emplace_back(
							port.at("backend").get<std::string>() + "/in/" + port.at("id").get<std::string>(),
							port.at("showAs").get<std::string>());
				widgets::fill(*select, options, m_pads.at("remotePort").get<std::string>());
			});
	}

	void ChordPadsPanel::assignFirstFree(const std::string& chord)
	{
		const Json& pads = m_pads.at("pads");
		for (size_t i = 0; i < pads.size(); ++i)
			if (pads[i].at("chord").get<std::string>().empty() && !pads[i].at("locked").get<bool>())
			{
				send("chordPads.assign", {{"assignments", {{{"pad", i}, {"chord", chord}}}}});
				return;
			}
	}
}
