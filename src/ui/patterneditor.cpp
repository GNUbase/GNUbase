// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/patterneditor.h"

#include "engine/midi/chords.h"
#include "engine/midi/quantizesettings.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>

namespace gnubase::ui
{
	using engine::Json;

	namespace
	{
		constexpr std::array<Rml::EventId, 5> g_events = {Rml::EventId::Mousedown, Rml::EventId::Mousemove,
			Rml::EventId::Click, Rml::EventId::Dblclick, Rml::EventId::Keydown};

		constexpr std::array<std::pair<const char*, const char*>, 3> g_modes = {{
			{"drum", "Drum"},
			{"mono", "Mono"},
			{"poly", "Poly"},
		}};

		constexpr std::array<std::pair<const char*, const char*>, 2> g_tools = {{
			{"select", "Step Selection"},
			{"draw", "Draw"},
		}};

		constexpr std::array<std::pair<const char*, const char*>, 3> g_directions = {{
			{"forward", "Forward"},
			{"backward", "Backward"},
			{"alternating", "Alternating"},
		}};

		constexpr std::array<int, 9> g_lengths = {4, 8, 12, 16, 24, 32, 48, 64, 128};
		constexpr std::array<const char*, 10> g_resolutions = {
			"1/4", "1/8", "1/16", "1/32", "1/64", "1/8T", "1/16T", "1/32T", "1/8.", "1/16."};

		// The parameter lane's parameters (p. 929), with their ranges.
		struct Parameter
		{
			const char* name;
			const char* label;
			double low;
			double high;
			double fallback;
			bool whole;
		};

		constexpr std::array<Parameter, 6> g_parameters = {{
			{"velocity", "Velocity", 1.0, 127.0, 100.0, true},
			{"repeats", "Repeats", 1.0, 10.0, 1.0, true},
			{"offset", "Offset", -0.5, 0.5, 0.0, false},
			{"probability", "Probability", 0.0, 1.0, 1.0, false},
			{"velocityVariance", "Velocity Variance", 0.0, 1.0, 0.0, false},
			{"gate", "Gate", 0.01, 1.0, 1.0, false},
		}};

		// Melodic mode shows at least these pitches, C2 to C4.
		constexpr int g_lowestRow = 48;
		constexpr int g_highestRow = 72;

		const Parameter& parameterOf(const std::string& name)
		{
			for (const Parameter& parameter : g_parameters)
				if (name == parameter.name)
					return parameter;
			return g_parameters.front();
		}

		std::string encode(const std::string& text)
		{
			return Rml::StringUtilities::EncodeRml(text);
		}

		std::string attribute(const Rml::Element& element, const char* name)
		{
			return element.GetAttribute<Rml::String>(name, "");
		}

		// The number in the attribute of the element or its closest parent that has it, up to stop.
		std::optional<int> numberOf(Rml::Element* element, const char* name, const Rml::Element* stop)
		{
			for (; element != nullptr && element != stop; element = element->GetParentNode())
				if (element->HasAttribute(name))
					return std::stoi(attribute(*element, name));
			return std::nullopt;
		}

		bool within(Rml::Element* element, const char* name, const Rml::Element* stop)
		{
			for (; element != nullptr && element != stop; element = element->GetParentNode())
				if (element->IsClassSet(name))
					return true;
			return false;
		}

		void setChecked(Rml::Element& item, bool checked)
		{
			if (checked && !item.HasAttribute("checked"))
				item.SetAttribute("checked", "");
			else if (!checked && item.HasAttribute("checked"))
				item.RemoveAttribute("checked");
		}

		std::string button(const std::string& classes, const std::string& attributes, const std::string& title,
			const std::string& label, bool checked = false)
		{
			return R"(<button class=")" + classes + (checked ? " checked" : "") + "\" " + attributes + R"( title=")" +
				title + R"(">)" + encode(label) + "</button>";
		}

		Json newStep(int index)
		{
			return {{"index", index}, {"length", 1}, {"velocity", 100}, {"repeats", 1}, {"offset", 0.0},
				{"probability", 1.0}, {"velocityVariance", 0.0}, {"gate", 1.0}};
		}
	}

	PatternEditor::PatternEditor(EngineBinding& binding, Rml::Element& root, std::string view)
		: m_binding(binding), m_root(root.GetObserverPtr()), m_name(std::move(view))
	{
		// The menus go into the body, so that no zone clips them.
		Rml::ElementDocument* document = root.GetOwnerDocument();
		std::string lengths;
		for (const int length : g_lengths)
			lengths += R"(<menuitem type="toggle" length=")" + std::to_string(length) + R"(">)" +
				std::to_string(length) + " Steps</menuitem>";
		std::string resolutions;
		for (const char* resolution : g_resolutions)
			resolutions += std::string(R"(<menuitem type="toggle" resolution=")") + resolution + R"(">)" + resolution +
				"</menuitem>";
		std::string directions;
		for (const auto& [direction, label] : g_directions)
			directions +=
				std::string(R"(<menuitem type="toggle" direction=")") + direction + R"(">)" + label + "</menuitem>";
		for (const std::string& rml :
			{std::string(R"(<menu id="pattern-menu"/>)"), R"(<menu id="pattern-length-menu">)" + lengths + "</menu>",
				R"(<menu id="pattern-resolution-menu">)" + resolutions + "</menu>",
				R"(<menu id="pattern-direction-menu">)" + directions + "</menu>"})
		{
			const Rml::ElementPtr holder = document->CreateElement("div");
			holder->SetInnerRML(rml);
			Rml::Element* menu = document->AppendChild(holder->RemoveChild(holder->GetFirstChild()));
			menu->AddEventListener(Rml::EventId::Click, this);
			m_menus.push_back(menu->GetObserverPtr());
		}
		for (const Rml::EventId event : g_events)
			root.AddEventListener(event, this);
	}

	PatternEditor::~PatternEditor()
	{
		listen(false);
		if (Rml::Element* root = m_root.get())
			for (const Rml::EventId event : g_events)
				root->RemoveEventListener(event, this);
		for (const Rml::ObserverPtr<Rml::Element>& menu : m_menus)
			if (Rml::Element* element = menu.get())
			{
				element->RemoveEventListener(Rml::EventId::Click, this);
				if (Rml::Element* parent = element->GetParentNode())
					parent->RemoveChild(element);
			}
	}

	void PatternEditor::show(const Json& project, const Json& view)
	{
		m_project = project;
		m_part = view.value("activePart", uint64_t{0});
		// A gesture going on keeps its own copy of the pattern, which its release sends.
		m_stale = m_gesture != Gesture::None;
		if (!m_stale)
			load();
	}

	void PatternEditor::load()
	{
		m_pattern = nullptr;
		uint64_t pattern = 0;
		for (const Json& part : m_project.value("midiParts", Json::array()))
			if (part.at("id") == m_part)
				pattern = part.value("pattern", uint64_t{0});
		for (const Json& each : m_project.value("patterns", Json::array()))
			if (each.at("id") == pattern)
				m_pattern = each;
		if (m_pattern.is_object() && lane(m_lane) == nullptr)
			m_lane = m_pattern.at("lanes").empty() ? -1 : m_pattern.at("lanes").at(0).at("pitch").get<int>();
		render();
	}

	std::vector<int> PatternEditor::rows() const
	{
		std::vector<int> pitches;
		if (!m_pattern.is_object())
			return pitches;
		const Json& lanes = m_pattern.at("lanes");
		if (m_pattern.at("mode") == "drum")
		{
			for (const Json& each : lanes)
				pitches.push_back(each.at("pitch").get<int>());
			return pitches;
		}
		// Melodic mode: a row per pitch, from the top, the lanes' and C2 to C4.
		int low = g_lowestRow;
		int high = g_highestRow;
		for (const Json& each : lanes)
		{
			low = std::min(low, each.at("pitch").get<int>());
			high = std::max(high, each.at("pitch").get<int>());
		}
		for (int pitch = high; pitch >= low; --pitch)
			pitches.push_back(pitch);
		return pitches;
	}

	Json* PatternEditor::lane(int pitch)
	{
		if (!m_pattern.is_object())
			return nullptr;
		for (Json& each : m_pattern.at("lanes"))
			if (each.at("pitch") == pitch)
				return &each;
		return nullptr;
	}

	const Json* PatternEditor::lane(int pitch) const
	{
		if (!m_pattern.is_object())
			return nullptr;
		for (const Json& each : m_pattern.at("lanes"))
			if (each.at("pitch") == pitch)
				return &each;
		return nullptr;
	}

	int PatternEditor::laneLength(int pitch) const
	{
		if (!m_pattern.is_object())
			return 0;
		for (const Json& each : m_pattern.at("lanes"))
			if (each.at("pitch") == pitch && each.value("length", 0) > 0)
				return each.at("length").get<int>();
		return m_pattern.at("length").get<int>();
	}

	const Json* PatternEditor::step(int pitch, int index, bool covering) const
	{
		const Json* found = lane(pitch);
		if (found == nullptr)
			return nullptr;
		const int length = laneLength(pitch);
		for (const Json& each : found->at("steps"))
		{
			const int start = each.at("index").get<int>();
			const int span = each.value("length", 1);
			if (start == index || (covering && ((index - start + length) % length) < span))
				return &each;
		}
		return nullptr;
	}

	void PatternEditor::addStep(int pitch, int index)
	{
		if (step(pitch, index, true) != nullptr)
			return;
		const auto insert = [this](int on, Json one)
		{
			if (lane(on) == nullptr)
				m_pattern.at("lanes").push_back({{"pitch", on}, {"steps", Json::array()}});
			Json& steps = lane(on)->at("steps");
			steps.push_back(std::move(one));
			std::sort(steps.begin(), steps.end(),
				[](const Json& a, const Json& b) { return a.at("index").get<int>() < b.at("index").get<int>(); });
		};
		Json added = newStep(index);
		// Mono mode plays one step at a time (p. 913): a step at the same position on another lane goes, and the
		// steps tied to it move to the new pitch from the position clicked, all of them at the tie's start.
		std::vector<int> pitches;
		if (m_pattern.at("mode") == "mono")
			for (const Json& each : m_pattern.at("lanes"))
				pitches.push_back(each.at("pitch").get<int>());
		for (const int other : pitches)
			if (const Json* covering = step(other, index, true); covering != nullptr)
			{
				const Json tied = *covering;
				const int start = tied.at("index").get<int>();
				const int span = tied.value("length", 1);
				const int length = std::max(laneLength(other), 1);
				const int into = (((index - start) % length) + length) % length;
				removeStep(other, start);
				if (into > 0)
				{
					Json kept = tied;
					kept["length"] = into;
					insert(other, std::move(kept));
				}
				if (span > 1)
				{
					added = tied;
					added["index"] = index;
					added["length"] = span - into;
				}
				break;
			}
		insert(pitch, std::move(added));
		m_changed = true;
	}

	void PatternEditor::removeStep(int pitch, int index)
	{
		Json* found = lane(pitch);
		if (found == nullptr)
			return;
		Json& steps = found->at("steps");
		const auto erased =
			std::remove_if(steps.begin(), steps.end(), [index](const Json& each) { return each.at("index") == index; });
		m_changed = m_changed || erased != steps.end();
		steps.erase(erased, steps.end());
		m_selection.erase({pitch, index});
	}

	void PatternEditor::setValue(int index, double value)
	{
		Json* found = nullptr;
		if (Json* each = lane(m_lane))
			for (Json& one : each->at("steps"))
				if (one.at("index") == index)
					found = &one;
		if (found == nullptr)
			return;
		const Parameter& parameter = parameterOf(m_parameter);
		double set = parameter.low + (std::clamp(value, 0.0, 1.0) * (parameter.high - parameter.low));
		if (parameter.whole)
		{
			(*found)[parameter.name] = static_cast<int>(std::lround(set));
		}
		else
		{
			set = std::round(set * 100.0) / 100.0;
			(*found)[parameter.name] = std::clamp(set, parameter.low, parameter.high);
		}
		m_changed = true;
	}

	void PatternEditor::sendLanes(const std::string& description)
	{
		if (!m_pattern.is_object())
			return;
		m_binding.send("history.transaction",
			{{"description", description},
				{"commands",
					Json::array({{{"command", "pattern.set"},
						{"parameters", {{"pattern", m_pattern.at("id")}, {"lanes", m_pattern.at("lanes")}}}}})}},
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				// Refused, the copy that was sent goes: the engine's pattern shows again.
				if (!alive.expired() && !result.ok() && m_gesture == Gesture::None)
					load();
			});
	}

	void PatternEditor::set(Json changes)
	{
		if (!m_pattern.is_object())
			return;
		changes["pattern"] = m_pattern.at("id");
		m_binding.send("pattern.set", changes);
	}

	void PatternEditor::tie(bool split)
	{
		for (Json& each : m_pattern.at("lanes"))
		{
			const int pitch = each.at("pitch").get<int>();
			const auto chosen = [&](const Json& one)
			{ return m_selection.empty() || m_selection.count({pitch, one.at("index").get<int>()}) != 0; };
			Json steps = Json::array();
			for (const Json& one : each.at("steps"))
			{
				const int length = one.value("length", 1);
				if (split && length > 1 && chosen(one))
				{
					for (int i = 0; i < length && one.at("index").get<int>() + i < laneLength(pitch); ++i)
					{
						Json part = one;
						part["index"] = one.at("index").get<int>() + i;
						part["length"] = 1;
						steps.push_back(part);
					}
					continue;
				}
				if (!split && !steps.empty() && chosen(one) && chosen(steps.back()) &&
					steps.back().at("index").get<int>() + steps.back().value("length", 1) == one.at("index").get<int>())
				{
					steps.back()["length"] = steps.back().value("length", 1) + length;
					continue;
				}
				steps.push_back(one);
			}
			each["steps"] = std::move(steps);
		}
		m_selection.clear();
		sendLanes(split ? "Split tied steps" : "Tie adjacent steps");
	}

	// Double Length of Pattern (p. 903) repeats the steps of the lanes that follow the pattern's length. Double Step
	// Resolution of Pattern doubles the indexes and lengths of the steps of the lanes that follow its resolution, and
	// the length of those of their own length, so that they play as before; one that can't double keeps the
	// resolution it had as its own. The other lanes loop on their own length or resolution and stay as they are.
	void PatternEditor::doubleLength(bool resolution)
	{
		const int length = m_pattern.at("length").get<int>();
		std::optional<engine::midi::NoteValue> value =
			engine::midi::noteValueFromString(m_pattern.at("resolution").get<std::string>());
		if (length * 2 > 128 || !value || (resolution && value->denominator >= 64))
			return;
		Json changes = {{"length", length * 2}};
		if (resolution)
		{
			value->denominator *= 2;
			changes["resolution"] = engine::midi::toString(*value);
		}
		Json lanes = m_pattern.at("lanes");
		for (Json& each : lanes)
		{
			const int own = each.value("length", 0);
			const bool ownResolution = each.contains("resolution") && !each.at("resolution").is_null();
			if (resolution ? ownResolution : own > 0)
				continue;
			if (resolution && own * 2 > 128)
			{
				each["resolution"] = m_pattern.at("resolution");
				continue;
			}
			Json steps = Json::array();
			for (const Json& one : each.at("steps"))
			{
				Json moved = one;
				if (resolution)
				{
					moved["index"] = one.at("index").get<int>() * 2;
					moved["length"] = one.value("length", 1) * 2;
					steps.push_back(moved);
					continue;
				}
				steps.push_back(one);
				moved["index"] = one.at("index").get<int>() + length;
				steps.push_back(moved);
			}
			if (own > 0)
				each["length"] = own * 2;
			each["steps"] = std::move(steps);
		}
		changes["lanes"] = std::move(lanes);
		set(std::move(changes));
	}

	void PatternEditor::ProcessEvent(Rml::Event& event)
	{
		Rml::Element* target = event.GetTargetElement();
		if (target == nullptr)
			return;
		switch (event.GetId())
		{
		case Rml::EventId::Mousedown:
			if (event.GetParameter("button", 0) == 0)
				press(event);
			break;
		case Rml::EventId::Mousemove:
			drag(event);
			break;
		case Rml::EventId::Mouseup:
			release();
			break;
		case Rml::EventId::Click:
			if (target->GetTagName() == "menuitem")
				choose(*target);
			else
				click(*target);
			break;
		case Rml::EventId::Dblclick:
			// The step selection tool's double click adds a step or removes it (p. 924).
			if (const std::optional<int> index = numberOf(target, "index", m_root.get());
				m_tool == "select" && index && within(target, "pattern-step", m_root.get()))
			{
				const int pitch = numberOf(target, "pitch", m_root.get()).value_or(-1);
				if (step(pitch, *index, true) != nullptr)
					removeStep(pitch, step(pitch, *index, true)->at("index").get<int>());
				else
					addStep(pitch, *index);
				m_changed = false;
				sendLanes("Edit steps");
				refresh();
			}
			break;
		case Rml::EventId::Keydown:
		{
			const auto key = static_cast<Rml::Input::KeyIdentifier>(event.GetParameter<int>("key_identifier", 0));
			if ((key == Rml::Input::KI_DELETE || key == Rml::Input::KI_BACK) && !m_selection.empty() &&
				m_pattern.is_object())
			{
				for (const auto& [pitch, index] : std::set<std::pair<int, int>>(m_selection))
					removeStep(pitch, index);
				m_changed = false;
				sendLanes("Delete steps");
				refresh();
				event.StopPropagation();
			}
			break;
		}
		default:
			break;
		}
	}

	void PatternEditor::press(Rml::Event& event)
	{
		Rml::Element* target = event.GetTargetElement();
		const Rml::Element* root = m_root.get();
		const std::optional<int> index = numberOf(target, "index", root);
		const std::optional<int> pitch = numberOf(target, "pitch", root);
		if (!m_pattern.is_object() || !index)
			return;
		m_changed = false;
		if (within(target, "pattern-bars", root))
		{
			m_gesture = Gesture::Values;
			drag(event);
		}
		else if (pitch && within(target, "pattern-step", root))
		{
			m_lane = *pitch;
			const Json* pressed = step(*pitch, *index, true);
			const bool alt = event.GetParameter("alt_key", 0) != 0;
			if (m_tool == "draw" || alt)
			{
				// The draw tool adds a step, or removes the one clicked, and its drag to the right more (p. 923).
				m_gesture = pressed != nullptr ? Gesture::Remove : Gesture::Add;
				if (pressed != nullptr)
					removeStep(*pitch, pressed->at("index").get<int>());
				else
					addStep(*pitch, *index);
			}
			else
			{
				// The step selection tool selects (p. 925): with Shift more, with Ctrl (Cmd) one on or off.
				const bool shift = event.GetParameter("shift_key", 0) != 0;
				const bool ctrl = event.GetParameter("ctrl_key", 0) != 0 || event.GetParameter("meta_key", 0) != 0;
				const std::pair<int, int> key{*pitch, pressed != nullptr ? pressed->at("index").get<int>() : -1};
				if (pressed == nullptr)
				{
					if (!shift && !ctrl)
						m_selection.clear();
				}
				else if (ctrl && m_selection.count(key) != 0)
				{
					m_selection.erase(key);
				}
				else
				{
					if (!shift && !ctrl)
						m_selection.clear();
					m_selection.insert(key);
				}
			}
		}
		if (m_gesture != Gesture::None)
			listen(true);
		refresh();
	}

	void PatternEditor::drag(Rml::Event& event)
	{
		if (m_gesture == Gesture::None)
			return;
		Rml::Element* target = event.GetTargetElement();
		const Rml::Element* root = m_root.get();
		const std::optional<int> index = numberOf(target, "index", root);
		if (!index)
			return;
		if (m_gesture == Gesture::Values)
		{
			Rml::Element* cell = target;
			while (cell != nullptr && !cell->IsClassSet("pattern-bar-cell"))
				cell = cell->GetParentNode();
			if (cell == nullptr)
				return;
			const float top = cell->GetAbsoluteOffset(Rml::BoxArea::Border).y;
			const float height = std::max(cell->GetBox().GetSize(Rml::BoxArea::Border).y, 1.0F);
			const auto y = event.GetParameter<float>("mouse_y", top);
			setValue(*index, 1.0 - ((y - top) / height));
			refresh();
			return;
		}
		if (numberOf(target, "pitch", root).value_or(-1) != m_lane || !within(target, "pattern-step", root))
			return;
		const bool changed = m_changed;
		if (m_gesture == Gesture::Add)
			addStep(m_lane, *index);
		else if (const Json* pressed = step(m_lane, *index); pressed != nullptr)
			removeStep(m_lane, *index);
		if (m_changed != changed || m_changed)
			refresh();
	}

	void PatternEditor::release()
	{
		listen(false);
		const Gesture gesture = m_gesture;
		m_gesture = Gesture::None;
		// What the gesture changed goes to the engine, whose answer shows the project; else the project that came
		// while it went on shows now.
		if (m_changed)
			sendLanes(gesture == Gesture::Values ? "Change step parameters" : "Edit steps");
		else if (m_stale)
			load();
		m_changed = false;
		m_stale = false;
	}

	void PatternEditor::listen(bool document)
	{
		if (Rml::Element* listened = m_document.get())
			listened->RemoveEventListener(Rml::EventId::Mouseup, this);
		m_document.reset();
		if (const Rml::Element* root = m_root.get(); document && root != nullptr)
			if (Rml::ElementDocument* owner = root->GetOwnerDocument())
			{
				owner->AddEventListener(Rml::EventId::Mouseup, this);
				m_document = owner->GetObserverPtr();
			}
	}

	void PatternEditor::click(Rml::Element& target)
	{
		const std::string id = target.GetId();
		const std::optional<int> pitch = numberOf(&target, "pitch", m_root.get());
		if (id == "pattern-new-drum" || id == "pattern-new-melodic")
		{
			m_binding.send("pattern.add", {{"part", m_part}, {"mode", id == "pattern-new-drum" ? "drum" : "poly"}});
			return;
		}
		if (!m_pattern.is_object())
			return;
		if (target.HasAttribute("pattern-mode"))
		{
			set({{"mode", attribute(target, "pattern-mode")}});
		}
		else if (target.HasAttribute("pattern-tool"))
		{
			m_tool = attribute(target, "pattern-tool");
			refresh();
		}
		else if (target.HasAttribute("parameter"))
		{
			m_parameter = attribute(target, "parameter");
			refresh();
		}
		else if (pitch && (target.IsClassSet("pattern-lane-mute") || target.IsClassSet("pattern-lane-solo")))
		{
			if (Json* found = lane(*pitch))
			{
				const char* key = target.IsClassSet("pattern-lane-mute") ? "mute" : "solo";
				(*found)[key] = !found->value(key, false);
				sendLanes(std::string(key == std::string("mute") ? "Mute" : "Solo") + " step lane");
				refresh();
			}
		}
		else if (pitch && target.IsClassSet("pattern-lane-name"))
		{
			m_lane = *pitch;
			refresh();
		}
		else if (id == "pattern-add-lane")
		{
			// The next pitch without a lane, from Bass Drum 1 up.
			int free = 36;
			while (free < 127 && lane(free) != nullptr)
				++free;
			if (lane(free) == nullptr)
			{
				m_pattern.at("lanes").push_back({{"pitch", free}, {"steps", Json::array()}});
				m_lane = free;
				sendLanes("Add step lane");
			}
		}
		else if (id == "pattern-remove-lane" && lane(m_lane) != nullptr)
		{
			Json& lanes = m_pattern.at("lanes");
			lanes.erase(std::remove_if(lanes.begin(), lanes.end(),
							[this](const Json& each) { return each.at("pitch") == m_lane; }),
				lanes.end());
			sendLanes("Remove step lane");
		}
		else if (id == "pattern-tie" || id == "pattern-split")
		{
			tie(id == "pattern-split");
		}
		else if (id == "pattern-double-length" || id == "pattern-double-resolution")
		{
			doubleLength(id == "pattern-double-resolution");
		}
	}

	void PatternEditor::choose(Rml::Element& item)
	{
		if (item.HasAttribute("pattern-id"))
		{
			m_binding.send("midiPart.set", {{"part", m_part}, {"pattern", std::stoull(attribute(item, "pattern-id"))}});
		}
		else if (item.HasAttribute("pattern-action"))
		{
			const std::string action = attribute(item, "pattern-action");
			const Json id = m_pattern.is_object() ? m_pattern.at("id") : Json(nullptr);
			if (action == "new")
				m_binding.send("pattern.add",
					{{"part", m_part}, {"mode", m_pattern.is_object() ? m_pattern.at("mode") : Json("drum")}});
			else if (action == "duplicate" && !id.is_null())
				m_binding.send("pattern.add", {{"part", m_part}, {"copy", id}});
			else if (action == "remove" && !id.is_null())
				m_binding.send("pattern.remove", {{"pattern", id}});
			else if (action == "convert" && !id.is_null())
				m_binding.send("pattern.convert", {{"part", m_part}});
		}
		else if (item.HasAttribute("length"))
		{
			set({{"length", std::stoi(attribute(item, "length"))}});
		}
		else if (item.HasAttribute("resolution"))
		{
			set({{"resolution", attribute(item, "resolution")}});
		}
		else if (item.HasAttribute("direction"))
		{
			set({{"direction", attribute(item, "direction")}});
		}
	}

	std::string PatternEditor::barsRml() const
	{
		const Parameter& parameter = parameterOf(m_parameter);
		std::string rml;
		for (int index = 0; m_lane >= 0 && index < laneLength(m_lane); ++index)
		{
			rml += R"(<div class="pattern-bar-cell" index=")" + std::to_string(index) + R"(">)";
			if (const Json* found = step(m_lane, index); found != nullptr)
			{
				const double value = found->value(parameter.name, parameter.fallback);
				const double share = (value - parameter.low) / (parameter.high - parameter.low);
				rml += R"(<div class="pattern-bar" style="height: )" +
					std::to_string(std::clamp(share, 0.0, 1.0) * 100.0) + R"(%;"/>)";
			}
			rml += "</div>";
		}
		return rml;
	}

	void PatternEditor::refresh()
	{
		Rml::Element* root = m_root.get();
		if (root == nullptr || !m_pattern.is_object())
			return;
		// The elements stay: those under the mouse, so that a gesture's mouseup reaches them, and the target of the
		// event being handled, which the views around the editor hear after it.
		Rml::ElementList cells;
		root->GetElementsByClassName(cells, "pattern-step");
		for (Rml::Element* cell : cells)
		{
			const int pitch = numberOf(cell, "pitch", root).value_or(-1);
			const int index = std::stoi(attribute(*cell, "index"));
			const Json* covering = step(pitch, index, true);
			const bool starts = covering != nullptr && covering->at("index") == index;
			cell->SetClass("on", starts);
			cell->SetClass("tied", covering != nullptr && !starts);
			cell->SetClass(
				"selected", covering != nullptr && m_selection.count({pitch, covering->at("index").get<int>()}) != 0);
		}
		Rml::ElementList lanes;
		root->GetElementsByClassName(lanes, "pattern-lane");
		for (Rml::Element* each : lanes)
		{
			const int pitch = numberOf(each, "pitch", root).value_or(-1);
			each->SetClass("selected", pitch == m_lane);
			const Json* found = lane(pitch);
			for (const char* key : {"mute", "solo"})
				if (Rml::Element* toggle = each->QuerySelector(std::string(".pattern-lane-") + key))
					toggle->SetClass("checked", found != nullptr && found->value(key, false));
		}
		Rml::ElementList buttons;
		root->GetElementsByTagName(buttons, "button");
		for (Rml::Element* each : buttons)
			if (each->HasAttribute("pattern-tool"))
				each->SetClass("checked", attribute(*each, "pattern-tool") == m_tool);
			else if (each->HasAttribute("parameter"))
				each->SetClass("checked", attribute(*each, "parameter") == m_parameter);
		if (Rml::Element* bars = root->QuerySelector(".pattern-bars .pattern-lane-steps"))
		{
			if (m_gesture != Gesture::Values)
			{
				bars->SetInnerRML(barsRml());
			}
			else
			{
				Rml::ElementList found;
				bars->GetElementsByClassName(found, "pattern-bar");
				const Parameter& parameter = parameterOf(m_parameter);
				for (Rml::Element* bar : found)
					if (const Json* each = step(m_lane, numberOf(bar, "index", root).value_or(-1)); each != nullptr)
					{
						const double value = each->value(parameter.name, parameter.fallback);
						bar->SetProperty(Rml::PropertyId::Height,
							Rml::Property(
								static_cast<float>(
									std::clamp((value - parameter.low) / (parameter.high - parameter.low), 0.0, 1.0) *
									100.0),
								Rml::Unit::PERCENT));
					}
			}
		}
		if (Rml::Context* context = root->GetContext())
			context->RequestNextUpdate(0);
	}

	void PatternEditor::render()
	{
		Rml::Element* root = m_root.get();
		if (root == nullptr)
			return;
		// Pattern Management (p. 899): the track's patterns, then New, Duplicate, Remove and Convert.
		uint64_t track = 0;
		for (const Json& part : m_project.value("midiParts", Json::array()))
			if (part.at("id") == m_part)
				track = part.at("track").get<uint64_t>();
		std::string items;
		for (const Json& each : m_project.value("patterns", Json::array()))
			if (each.at("track") == track)
				items += R"(<menuitem type="toggle" pattern-id=")" + std::to_string(each.at("id").get<uint64_t>()) +
					R"(">)" + encode(each.at("name").get<std::string>()) + "</menuitem>";
		items += R"(<menuseparator/><menuitem pattern-action="new">New Pattern</menuitem>)";
		if (m_pattern.is_object())
			items += R"(<menuitem pattern-action="duplicate">Duplicate Pattern</menuitem>)"
					 R"(<menuitem pattern-action="remove">Remove Pattern</menuitem>)"
					 R"(<menuitem pattern-action="convert">Convert Pattern Event to MIDI Part</menuitem>)";
		std::string toolbar = R"(<div class="toolbar pattern-editor-toolbar">)" +
			button("", R"(id="pattern-select" menu="pattern-menu")", "Pattern Management",
				m_pattern.is_object() ? m_pattern.at("name").get<std::string>() : "No Pattern");
		if (!m_pattern.is_object())
		{
			root->SetInnerRML(toolbar +
				R"(</div><div class="pattern-editor-empty">The part plays no pattern.)"
				R"(<button id="pattern-new-drum">New Drum Pattern</button>)"
				R"(<button id="pattern-new-melodic">New Melodic Pattern</button></div>)");
		}
		else
		{
			const std::string mode = m_pattern.at("mode").get<std::string>();
			toolbar += R"(<span class="separator"/>)";
			for (const auto& [name, label] : g_modes)
				toolbar += button("pattern-mode", std::string(R"(pattern-mode=")") + name + "\"",
					std::string(label) + " Mode", label, mode == name);
			toolbar += R"(<span class="separator"/>)";
			for (const auto& [name, label] : g_tools)
				toolbar += button(
					"pattern-tool", std::string(R"(pattern-tool=")") + name + "\"", label, label, m_tool == name);
			std::string direction;
			for (const auto& [name, label] : g_directions)
				if (m_pattern.at("direction") == name)
					direction = label;
			toolbar += R"(<span class="separator"/>)" +
				button("", R"(id="pattern-length" menu="pattern-length-menu")", "Number of Steps",
					std::to_string(m_pattern.at("length").get<int>()) + " Steps") +
				button("", R"(id="pattern-resolution" menu="pattern-resolution-menu")", "Step Resolution",
					m_pattern.at("resolution").get<std::string>()) +
				button("", R"(id="pattern-direction" menu="pattern-direction-menu")", "Pattern Play Direction",
					direction) +
				button("", R"(id="pattern-double-length")", "Double Length of Pattern", "Double Length") +
				button(
					"", R"(id="pattern-double-resolution")", "Double Step Resolution of Pattern", "Double Resolution") +
				button("", R"(id="pattern-tie")", "Tie Adjacent Steps", "Tie") +
				button("", R"(id="pattern-split")", "Split Tied Steps", "Split");
			if (mode == "drum")
				toolbar += button("", R"(id="pattern-add-lane")", "Add Step Lane", "Add Lane") +
					button("", R"(id="pattern-remove-lane")", "Remove Step Lane", "Remove Lane");
			toolbar += "</div>";

			std::string body = R"(<div class="pattern-editor-body">)";
			for (const int pitch : rows())
			{
				const Json* found = lane(pitch);
				const std::string name = found != nullptr && !found->value("name", std::string()).empty()
					? found->at("name").get<std::string>()
					: engine::midi::pitchName(pitch);
				body += R"(<div class="pattern-lane)" + std::string(pitch == m_lane ? " selected" : "") +
					(engine::midi::pitchName(pitch).find('#') != std::string::npos ? " black" : "") + R"(" pitch=")" +
					std::to_string(pitch) + R"("><div class="pattern-lane-controls">)";
				if (found != nullptr)
					body += button("pattern-lane-mute", "", "Mute", "M", found->value("mute", false)) +
						button("pattern-lane-solo", "", "Solo", "S", found->value("solo", false));
				body += R"(<span class="pattern-lane-name">)" + encode(name) +
					R"(</span></div><div class="pattern-lane-steps">)";
				const int length = laneLength(pitch);
				for (int index = 0; index < length; ++index)
				{
					const Json* covering = step(pitch, index, true);
					std::string classes = "pattern-step";
					if (covering != nullptr)
						classes += covering->at("index") == index ? " on" : " tied";
					if (covering != nullptr && m_selection.count({pitch, covering->at("index").get<int>()}) != 0)
						classes += " selected";
					if (index % 4 == 0)
						classes += " beat";
					body += R"(<div class=")" + classes + R"(" index=")" + std::to_string(index) + R"("/>)";
				}
				body += "</div></div>";
			}
			body += "</div>";

			// The parameter lane (p. 928): the selected lane's steps' values of the parameter chosen.
			std::string lane = R"(<div class="pattern-parameter-lane"><div class="toolbar pattern-parameters">)";
			for (const Parameter& each : g_parameters)
				lane += button("pattern-parameter", std::string(R"(parameter=")") + each.name + "\"", each.label,
					each.label, m_parameter == each.name);
			lane += R"(</div><div class="pattern-bars"><div class="pattern-lane-controls"/>)"
					R"(<div class="pattern-lane-steps">)";
			lane += barsRml();
			lane += "</div></div></div>";
			root->SetInnerRML(toolbar + body + lane);
		}
		for (const Rml::ObserverPtr<Rml::Element>& observed : m_menus)
		{
			Rml::Element* menu = observed.get();
			if (menu == nullptr)
				continue;
			if (menu->GetId() == "pattern-menu")
			{
				menu->SetInnerRML(items);
				continue;
			}
			for (int i = 0; i < menu->GetNumChildren(); ++i)
			{
				Rml::Element* item = menu->GetChild(i);
				if (!m_pattern.is_object())
					break;
				if (item->HasAttribute("length"))
					setChecked(*item, std::to_string(m_pattern.at("length").get<int>()) == attribute(*item, "length"));
				else if (item->HasAttribute("resolution"))
					setChecked(*item, m_pattern.at("resolution") == attribute(*item, "resolution"));
				else if (item->HasAttribute("direction"))
					setChecked(*item, m_pattern.at("direction") == attribute(*item, "direction"));
			}
		}
		if (Rml::ElementDocument* menuOwner = root->GetOwnerDocument())
			if (const Rml::Element* menu = menuOwner->GetElementById("pattern-menu");
				menu != nullptr && m_pattern.is_object())
				for (int i = 0; i < menu->GetNumChildren(); ++i)
					if (Rml::Element* item = menu->GetChild(i); item->HasAttribute("pattern-id"))
						setChecked(*item,
							attribute(*item, "pattern-id") == std::to_string(m_pattern.at("id").get<uint64_t>()));
		if (Rml::Context* context = root->GetContext())
			context->RequestNextUpdate(0);
	}
}
