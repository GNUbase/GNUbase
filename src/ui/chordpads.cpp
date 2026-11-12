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
#include <utility>

namespace gnubase::ui
{
	using engine::Json;

	namespace
	{
		constexpr std::array<Rml::EventId, 6> g_events = {Rml::EventId::Mousedown, Rml::EventId::Click,
			Rml::EventId::Dblclick, Rml::EventId::Change, Rml::EventId::Mousescroll, Rml::EventId::Keydown};

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

	ChordPadsPanel::ChordPadsPanel(EngineBinding& binding, Rml::Element& element)
		: m_binding(binding), m_root(element.GetObserverPtr())
	{
		element.SetInnerRML(R"(<div class="chord-pads"><div class="chord-pads-toolbar">)" +
			button("stepInput", "Step Input", "Step", true) + button("moveInsert", "Move Insert Mode", "Insert", true) +
			R"(<select id="chord-pads-root" title="Root Key"/>)" +
			button("transposePads", "Transpose Pads", "Transpose Pads", true) +
			R"(<select id="chord-pads-preset" title="Chord Pads Presets"/>)"
			R"(<select id="chord-pads-load" title="Load Preset Settings"/>)" +
			button("loadPreset", "Load Preset", "Load") +
			button("fromChordTrack", "Assign from Chord Track", "From Chord Track") +
			number("velocity", "Velocity on Mouse Click", 1, 127) +
			button("remote", "Pad Remote Control", "Remote", true) +
			number("remoteStart", "Pads Remote Range Start", 0, 127) +
			number("remoteEnd", "Pads Remote Range End", 0, 127) + number("octaveOffset", "Octave Offset", -3, 3) +
			button("assistant", "Show/Hide Editor/Assistant", "Assistant", true) +
			R"(</div><div class="chord-pads-toolbar chord-pads-extended">)" + button("selectAll", "Select All", "All") +
			button("edit", "Open Chord Editor", "Edit") + button("remove", "Remove Assignment", "Remove") +
			button("transposeDown", "Transpose Down", "-1") + button("transposeUp", "Transpose Up", "+1") +
			button("fewerTensions", "Fewer Tensions", "Fewer") + button("moreTensions", "More Tensions", "More") +
			button("previousVoicing", "Previous Voicing", "&lt;") + button("nextVoicing", "Next Voicing", "&gt;") +
			button("lock", "Lock", "Lock", true) + button("adaptive", "Adaptive Voicing", "Adaptive", true) +
			button("reference", "Adaptive Voicing Reference", "Reference", true) +
			R"(<input type="text" id="chord-pads-field" class="chord-pads-field" title="Chord"/>)"
			R"(</div><div class="chord-pads-body"><div id="chord-pads-pads" class="chord-pads-pads"/>)"
			R"(<div id="chord-pads-assistant-pane" class="chord-pads-assistant hidden"><div class="chord-pads-toolbar">)"
			R"(<button id="chord-pads-page-list" page="list" type="toggle" title="List">List</button>)"
			R"(<button id="chord-pads-page-circle" page="circle" type="toggle" title="Circle of Fifths">)"
			R"(Circle of Fifths</button><select id="chord-pads-algorithm" title="Algorithm"/>)"
			R"(<select id="chord-pads-mode" title="Major/Minor"/></div>)"
			R"(<div id="chord-pads-suggestions" class="chord-pads-suggestions"/></div></div></div>)");
		widgets::fill(*find("chord-pads-root"), rootKeys(), "0");
		widgets::fill(*find("chord-pads-preset"), presets(), "major");
		widgets::fill(*find("chord-pads-load"), loads(), "chordsAndPlayers");
		widgets::fill(*find("chord-pads-algorithm"), algorithms(), "cadence");
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
				if (const Rml::Element* pads = find("chord-pads-pads");
					pads != nullptr && pad >= 0 && pad < pads->GetNumChildren())
					setClass(pads->GetChild(pad), "triggered", event.data.value("on", false));
			}));
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
					R"(<svg class="icon chord-pad-lock" src="../icons/general-lock.svg"/></div></div>)";
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
			setText(element->GetChild(0), chord);
			setText(element->GetChild(1), pad.at("degree").get<std::string>());
			setText(element->GetChild(2), pad.at("key").is_number() ? noteName(pad.at("key").get<int>()) : "");
			setClass(element, "unassigned", chord.empty());
			setClass(element, "locked", pad.at("locked").get<bool>());
			setClass(element, "adaptive", pad.at("adaptive").get<bool>());
			setClass(element, "reference", i == reference);
		}
		for (const char* setting : {"stepInput", "moveInsert", "remote"})
			setChecked(find(std::string("chord-pads-") + setting), pads.at(setting).get<bool>());
		for (const char* setting : {"velocity", "remoteStart", "remoteEnd", "octaveOffset"})
			setValue(find(std::string("chord-pads-") + setting), pads.at(setting).get<int>());
		widgets::fill(*find("chord-pads-root"), rootKeys(), std::to_string(pads.at("rootKey").get<int>()));
		widgets::fill(*find("chord-pads-preset"), presets(), pads.at("preset").get<std::string>());
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
		showAssistant();
	}

	void ChordPadsPanel::showAssistant()
	{
		const Rml::Element* assistant = find("chord-pads-assistant-pane");
		if (assistant == nullptr || assistant->IsClassSet("hidden"))
			return;
		setChecked(find("chord-pads-page-list"), m_page == "list");
		setChecked(find("chord-pads-page-circle"), m_page == "circle");
		setClass(find("chord-pads-algorithm"), "hidden", m_page != "list");
		setClass(find("chord-pads-mode"), "hidden", m_page != "circle");
		Json parameters = {{"kind", m_page}};
		if (m_page == "list")
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
			parameters["algorithm"] = find("chord-pads-algorithm")->GetAttribute<Rml::String>("value", "cadence");
		}
		else
		{
			parameters["minor"] = find("chord-pads-mode")->GetAttribute<Rml::String>("value", "major") == "minor";
		}
		m_binding.send("chordPads.assistant", parameters,
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				Rml::Element* suggestions = alive.expired() || !result.ok() ? nullptr : find("chord-pads-suggestions");
				if (suggestions == nullptr)
					return;
				std::string rml;
				if (result.value.contains("suggestions"))
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
				if (Rml::Context* context = suggestions->GetContext())
					context->RequestNextUpdate(0);
			});
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
			}
			return;
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
			else if (const Rml::Element* actor = withAttribute(target, "action"))
			{
				action(actor->GetAttribute<Rml::String>("action", ""));
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
		if (!m_auditioned.empty())
		{
			send("chordPads.trigger", {{"chord", m_auditioned}, {"on", false}});
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
		if (name == "stepInput" || name == "moveInsert" || name == "remote")
		{
			send("chordPads.set", {{name, !m_pads.at(name).get<bool>()}});
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
		if (target.GetTagName() == "valuefield" && target.HasAttribute("setting"))
		{
			const auto value = static_cast<int>(event.GetParameter<float>("value", 0.0F));
			const auto setting = target.GetAttribute<Rml::String>("setting", "");
			if (m_pads.contains(setting) && m_pads.at(setting) != value)
				send("chordPads.set", {{setting, value}});
			return;
		}
		const auto value = event.GetParameter<Rml::String>("value", "");
		if (id == "chord-pads-root" && !value.empty() && std::to_string(m_pads.at("rootKey").get<int>()) != value)
		{
			const bool transpose = find("chord-pads-transposePads")->HasAttribute("checked");
			send("chordPads.set", {{"rootKey", std::stoi(value)}, {"transposePads", transpose}});
		}
		else if (id == "chord-pads-algorithm" || id == "chord-pads-mode")
		{
			showAssistant();
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
