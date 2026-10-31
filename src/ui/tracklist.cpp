// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/tracklist.h"

#include "ui/widgets.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Property.h>
#include <RmlUi/Core/StringUtilities.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <sstream>
#include <tuple>
#include <utility>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		constexpr double g_edge = 4.0;          // dp above a row's lower edge that size its track
		constexpr double g_dragThreshold = 4.0; // dp a press moves before it drags

		constexpr std::array<Rml::EventId, 5> g_rootEvents = {Rml::EventId::Mousedown, Rml::EventId::Dblclick,
			Rml::EventId::Click, Rml::EventId::Mousescroll, Rml::EventId::Change};
		constexpr std::array<Rml::EventId, 2> g_documentEvents = {Rml::EventId::Mousemove, Rml::EventId::Mouseup};

		Rml::Property dp(double value)
		{
			return {static_cast<float>(value), Rml::Unit::DP};
		}

		// The controls a track type can have.
		bool applies(const std::string& control, const std::string& type, bool hasChannel)
		{
			if (!hasChannel)
				return control == "timeBase" && type != "folder" && type != "ruler";
			if (control == "record" || control == "monitor")
				return type == "audio";
			if (control == "timeBase")
				return type == "audio" || type == "instrument" || type == "midi";
			return true;
		}

		// How a control looks in its button.
		std::string buttonRml(const std::string& control)
		{
			static const std::map<std::string, std::pair<const char*, const char*>> looks = {{"mute", {"Mute", "M"}},
				{"solo", {"Solo", "S"}},
				{"record", {"Record Enable", R"(<svg class="icon" src="../icons/track-record-arm.svg"/>)"}},
				{"monitor", {"Monitor", R"(<svg class="icon" src="../icons/track-monitor.svg"/>)"}},
				{"read", {"Read Automation", R"(<svg class="icon" src="../icons/track-read-automation.svg"/>)"}},
				{"timeBase", {"Switch Time Base", "L"}}};
			const auto& [title, content] = looks.at(control);
			return R"(<button class="toggle )" + control + R"(" control=")" + control + R"(" title=")" + title +
				R"(">)" + content + "</button>";
		}

		bool held(const Rml::Event& event, const char* key)
		{
			return event.GetParameter<int>(key, 0) != 0;
		}

		void setClass(Rml::Element& element, const char* name, bool set)
		{
			if (element.IsClassSet(name) != set)
				element.SetClass(name, set);
		}
	}

	TrackList::TrackList(EventDisplay& display, EngineBinding& binding, Rml::Element& root)
		: m_display(display), m_binding(binding), m_root(root.GetObserverPtr()), m_document(root.GetOwnerDocument())
	{
		Rml::ElementPtr rows = root.GetOwnerDocument()->CreateElement("div");
		rows->SetClass("track-rows", true);
		m_rows = root.AppendChild(std::move(rows));
		m_rows->SetInnerRML(R"(<listview class="tracks" multiple=""/><div class="drop-line"/>)");
		m_view = m_rows->GetChild(0);
		m_dropLine = m_rows->GetChild(1);
		for (const Rml::EventId id : g_rootEvents)
			root.AddEventListener(id, this);
		root.AddEventListener(Rml::EventId::Blur, this, true);    // blur doesn't bubble
		root.AddEventListener(Rml::EventId::Keydown, this, true); // before the name field takes Escape
		m_view->AddEventListener(g_treeToggleEvent, this);
		for (const Rml::EventId id : g_documentEvents)
			m_document->AddEventListener(id, this);

		const auto selected = [this](const std::function<void(uint64_t)>& each)
		{
			for (const uint64_t track : topSelection())
				each(track);
		};
		m_actions = {
			{"track.selectPrevious", [this] { step(-1); }},
			{"track.selectNext", [this] { step(1); }},
			{"track.rename",
				[this]
				{
					const std::vector<uint64_t> tracks = selection();
					if (!tracks.empty())
						rename(tracks.front());
				}},
			{"track.removeSelected",
				[this, selected]
				{
					Json commands = Json::array();
					selected([&commands](uint64_t track)
						{ commands.push_back({{"command", "track.remove"}, {"parameters", {{"track", track}}}}); });
					transaction("Remove Selected Tracks", std::move(commands));
				}},
			{"track.removeEmpty",
				[this]
				{
					// Audio, instrument, MIDI and marker tracks without events, parts, markers or automation, and
					// folders without tracks, unless other tracks play into them; groups and effects are where other
					// tracks play through, ruler tracks never hold anything.
					Json commands = Json::array();
					for (const Row& row : m_tracks)
						if (row.empty && row.type != "group" && row.type != "effect" && row.type != "ruler")
							commands.push_back({{"command", "track.remove"}, {"parameters", {{"track", row.id}}}});
					transaction("Remove Empty Tracks", std::move(commands));
				}},
			{"track.duplicateSelected",
				[this, selected]
				{
					Json commands = Json::array();
					selected([&commands](uint64_t track)
						{ commands.push_back({{"command", "track.duplicate"}, {"parameters", {{"track", track}}}}); });
					transaction("Duplicate Tracks", std::move(commands));
				}},
			{"track.toggleFolding",
				[this, selected]
				{
					selected(
						[this](uint64_t track)
						{
							if (m_tracks[m_index.at(track)].type == "folder")
								setFolded(track, !folded(track));
						});
				}},
			{"track.foldAll",
				[this]
				{
					for (const Row& row : m_tracks)
						if (row.type == "folder")
							m_folded.insert(row.id);
					hide();
				}},
			{"track.unfoldAll",
				[this]
				{
					m_folded.clear();
					hide();
				}},
			{"track.flipFolding",
				[this]
				{
					std::set<uint64_t> flipped;
					for (const Row& row : m_tracks)
						if (row.type == "folder" && !folded(row.id))
							flipped.insert(row.id);
					m_folded = std::move(flipped);
					hide();
				}},
		};
		for (const bool enabled : {false, true})
			m_actions[enabled ? "track.enableSelected" : "track.disableSelected"] = [this, enabled]
			{
				Json commands = Json::array();
				for (const uint64_t track : selection())
				{
					const std::string& type = m_tracks[m_index.at(track)].type;
					if (type == "audio" || type == "instrument" || type == "midi")
						commands.push_back(
							{{"command", "track.set"}, {"parameters", {{"track", track}, {"enabled", enabled}}}});
				}
				transaction(enabled ? "Enable Tracks" : "Disable Tracks", std::move(commands));
			};
		// Folder tracks (docs/features/folder-tracks.md, reference manual pp. 222-225).
		for (const bool on : {false, true})
		{
			m_actions[on ? "track.enableSelectedGroupChannels" : "track.disableSelectedGroupChannels"] = [this, on]
			{
				Json commands = Json::array();
				for (const uint64_t track : selection())
					if (m_tracks[m_index.at(track)].type == "folder")
						commands.push_back(
							{{"command", "track.set"}, {"parameters", {{"track", track}, {"groupChannel", on}}}});
				transaction(on ? "Enable Group Channels" : "Disable Group Channels", std::move(commands));
			};
			m_actions[on ? "track.moveSelectedToNewFolderWithGroupChannel" : "track.moveSelectedToNewFolder"] =
				[this, on]
			{
				// A new folder where the first of them was, with them inside; marker tracks stay at the top level.
				Json tracks = Json::array();
				for (const uint64_t track : topSelection())
					if (m_tracks[m_index.at(track)].type != "marker")
						tracks.push_back(track);
				if (tracks.empty())
					return;
				const auto first = tracks[0].get<uint64_t>();
				const uint64_t parent = m_tracks[m_index.at(first)].parent;
				int64_t index = 0;
				for (const Row& row : m_tracks)
				{
					if (row.id == first)
						break;
					index += row.parent == parent ? 1 : 0;
				}
				m_binding.send("track.add",
					{{"type", "folder"}, {"parent", parent}, {"index", index}, {"groupChannel", on},
						{"tracks", std::move(tracks)}});
			};
		}

		m_subscriptions.push_back(m_binding.on("settings.changed",
			[this](const engine::Event& event)
			{
				const Json& values = event.data.at("values");
				if (const auto controls = values.find("ui.trackControls"); controls != values.end())
					loadControls(*controls);
			}));
		m_binding.send("settings.get", {{"key", "ui.trackControls"}},
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (!alive.expired() && result.ok())
					loadControls(result.value.at("value"));
			});
		m_display.onLayout([this] { layout(); });
		layout();
	}

	TrackList::~TrackList()
	{
		m_display.onLayout({});
		if (!alive())
			return;
		for (const Rml::EventId id : g_rootEvents)
			m_root->RemoveEventListener(id, this);
		m_root->RemoveEventListener(Rml::EventId::Blur, this, true);
		m_root->RemoveEventListener(Rml::EventId::Keydown, this, true);
		m_view->RemoveEventListener(g_treeToggleEvent, this);
		for (const Rml::EventId id : g_documentEvents)
			m_document->RemoveEventListener(id, this);
	}

	float TrackList::dpRatio() const
	{
		const Rml::Context* context = alive() ? m_root->GetContext() : nullptr;
		return context == nullptr ? 1.0F : context->GetDensityIndependentPixelRatio();
	}

	std::vector<uint64_t> TrackList::selection() const
	{
		std::vector<uint64_t> tracks;
		if (!alive())
			return tracks;
		std::istringstream split(m_view->GetAttribute<Rml::String>("value", ""));
		for (std::string one; std::getline(split, one, ',');)
			if (const uint64_t id = std::strtoull(one.c_str(), nullptr, 10); m_index.count(id) != 0)
				tracks.push_back(id);
		std::sort(
			tracks.begin(), tracks.end(), [this](uint64_t a, uint64_t b) { return m_index.at(a) < m_index.at(b); });
		return tracks;
	}

	void TrackList::select(const std::vector<uint64_t>& tracks)
	{
		if (!alive())
			return;
		std::string value;
		for (const uint64_t track : tracks)
			value += (value.empty() ? "" : ",") + std::to_string(track);
		m_view->SetAttribute("value", value);
		if (Rml::Context* context = m_view->GetContext())
			context->RequestNextUpdate(0);
	}

	void TrackList::setFolded(uint64_t folder, bool folded)
	{
		if (folded)
			m_folded.insert(folder);
		else
			m_folded.erase(folder);
		hide();
	}

	void TrackList::loadControls(const Json& value)
	{
		m_controls = value.is_object() ? value : Json::object();
		if (alive())
			showState();
	}

	void TrackList::layout()
	{
		if (!alive() || m_layingOut)
			return;
		m_layingOut = true;
		if (m_display.loads() != m_loads)
		{
			m_loads = m_display.loads();
			build();
			showState();
			hide(); // may load the display's lanes again, which lays them out again
		}
		place();
		m_layingOut = false;
	}

	void TrackList::build()
	{
		const Json& project = m_display.project();
		if (!project.is_object())
			return;
		std::vector<Row> tracks;
		std::map<uint64_t, size_t> index;
		for (const Json& track : project.at("tracks"))
		{
			Row row;
			row.id = track.at("id").get<uint64_t>();
			row.type = track.at("type").get<std::string>();
			row.parent = track.at("parent").get<uint64_t>();
			row.channel = track.at("channel").get<uint64_t>();
			if (const auto parent = index.find(row.parent); parent != index.end())
			{
				row.level = tracks[parent->second].level + 1;
				tracks[parent->second].empty = false;
			}
			index[row.id] = tracks.size();
			tracks.push_back(row);
		}
		for (const char* kind : {"audioEvents", "midiParts", "markers", "automation"})
			for (const Json& item : project.at(kind))
				if (const auto track = index.find(item.at("track").get<uint64_t>()); track != index.end())
					tracks[track->second].empty = false;
		// Tracks that others play into, e.g. an instrument track that MIDI tracks play, aren't empty either.
		std::set<uint64_t> outputs;
		for (const Json& channel : project.at("channels"))
			outputs.insert(channel.at("output").get<uint64_t>());
		for (Row& row : tracks)
			if (row.channel != 0 && outputs.count(row.channel) != 0)
				row.empty = false;
		// Folded folders that went stay folded if they come back, e.g. by undo; that's harmless.
		const bool same = tracks.size() == m_tracks.size() &&
			std::equal(tracks.begin(), tracks.end(), m_tracks.begin(), [](const Row& a, const Row& b)
				{ return a.id == b.id && a.type == b.type && a.level == b.level && a.channel == b.channel; });
		m_tracks = std::move(tracks);
		m_index = std::move(index);
		if (same)
			return;
		std::string rml;
		for (const Row& row : m_tracks)
			rml += R"(<listitem value=")" + std::to_string(row.id) + R"(" level=")" + std::to_string(row.level) +
				R"(" type=")" + row.type +
				R"("><span class="track-name"/><input type="text" class="track-name-field"/>)"
				R"(<div class="track-controls"/></listitem>)";
		finishRename(false);
		const auto selected = m_view->GetAttribute<Rml::String>("value", "");
		m_view->SetInnerRML(rml);
		m_view->SetAttribute("value", selected); // the list view keeps those that are still there
	}

	const Json* TrackList::channelOf(const Row& row) const
	{
		if (row.channel == 0)
			return nullptr;
		for (const Json& channel : m_display.project().at("channels"))
			if (channel.at("id") == row.channel)
				return &channel;
		return nullptr;
	}

	std::vector<std::string> TrackList::controlsOf(const Row& row) const
	{
		std::vector<std::string> shown;
		const bool hasChannel = row.channel != 0;
		const Json chosen = m_controls.value(row.type, Json::array());
		// Mute and solo are always there (reference manual p. 191).
		for (const char* always : {"mute", "solo"})
			if (hasChannel && std::find(chosen.begin(), chosen.end(), always) == chosen.end())
				shown.emplace_back(always);
		for (const Json& control : chosen)
			if (control.is_string() && applies(control.get<std::string>(), row.type, hasChannel))
				shown.push_back(control.get<std::string>());
		return shown;
	}

	void TrackList::showState()
	{
		const Json& project = m_display.project();
		if (!alive() || !project.is_object())
			return;
		std::map<uint64_t, std::vector<bool>> reading; // each track's automation lanes, whether they read
		for (const Json& lane : project.at("automation"))
			reading[lane.at("track").get<uint64_t>()].push_back(lane.at("read").get<bool>());
		const Json& tracks = project.at("tracks");
		for (int i = 0; i < m_view->GetNumChildren() && static_cast<size_t>(i) < m_tracks.size(); ++i)
		{
			Rml::Element* element = m_view->GetChild(i);
			const Row& row = m_tracks[static_cast<size_t>(i)];
			const Json& track = tracks[static_cast<size_t>(i)];
			setClass(*element, "disabled", !track.at("enabled").get<bool>());
			const bool expanded = !folded(row.id);
			if (expanded != element->HasAttribute("expanded"))
			{
				if (expanded)
					element->SetAttribute("expanded", "");
				else
					element->RemoveAttribute("expanded");
			}
			// The name, unless it's being typed.
			if (Rml::Element* name = element->QuerySelector(".track-name"); name != nullptr && row.id != m_renaming)
			{
				const std::string text = Rml::StringUtilities::EncodeRml(track.at("name").get<std::string>());
				if (name->GetInnerRML() != text)
					name->SetInnerRML(text);
			}
			Rml::Element* controls = element->QuerySelector(".track-controls");
			if (controls == nullptr)
				continue;
			const std::vector<std::string> shown = controlsOf(row);
			std::string rml;
			for (const std::string& control : shown)
				rml += buttonRml(control);
			if (controls->GetAttribute<Rml::String>("controls", "") != rml)
			{
				controls->SetAttribute("controls", rml);
				controls->SetInnerRML(rml);
			}
			const Json* channel = channelOf(row);
			const std::vector<bool>& lanes = reading[row.id];
			for (int c = 0; c < controls->GetNumChildren(); ++c)
			{
				Rml::Element* button = controls->GetChild(c);
				const auto name = button->GetAttribute<Rml::String>("control", "");
				bool on = false;
				if (name == "record")
					on = channel != nullptr && channel->at("recordEnable").get<bool>();
				else if (name == "timeBase")
					on = track.at("timeBase") == "linear";
				else if (name == "read")
					on = !lanes.empty() && std::all_of(lanes.begin(), lanes.end(), [](bool read) { return read; });
				else
					on = channel != nullptr && channel->value(name, false);
				setClass(*button, "checked", on);
				// Read needs automation lanes to read.
				const bool disabled = name == "read" && lanes.empty();
				if (disabled != button->HasAttribute("disabled"))
				{
					if (disabled)
						button->SetAttribute("disabled", "");
					else
						button->RemoveAttribute("disabled");
				}
			}
		}
		if (Rml::Context* context = m_view->GetContext())
			context->RequestNextUpdate(0);
	}

	void TrackList::hide()
	{
		std::set<uint64_t> hidden;
		for (const Row& row : m_tracks)
			if (row.parent != 0 && (folded(row.parent) || hidden.count(row.parent) != 0))
				hidden.insert(row.id);
		// The list view hides the same rows by their expanded attributes.
		if (alive())
			for (int i = 0; i < m_view->GetNumChildren() && static_cast<size_t>(i) < m_tracks.size(); ++i)
			{
				Rml::Element* element = m_view->GetChild(i);
				const bool expanded = !folded(m_tracks[static_cast<size_t>(i)].id);
				if (expanded && !element->HasAttribute("expanded"))
					element->SetAttribute("expanded", "");
				else if (!expanded && element->HasAttribute("expanded"))
					element->RemoveAttribute("expanded");
			}
		m_display.setHiddenTracks(std::move(hidden));
		place();
	}

	void TrackList::place()
	{
		if (!alive())
			return;
		const std::vector<uint64_t>& lanes = m_display.laneTracks();
		const std::vector<double>& tops = m_display.laneTops();
		for (size_t lane = 0; lane < lanes.size() && lane + 1 < tops.size(); ++lane)
			if (Rml::Element* element = rowElement(lanes[lane]))
				element->SetProperty(Rml::PropertyId::Height, dp(tops[lane + 1] - tops[lane]));
		m_view->SetProperty(Rml::PropertyId::Top, dp(-m_display.view().top));
		m_rows->SetScrollTop(0.0F); // the list view's keys scroll it into view; the display scrolls it
	}

	const TrackList::Row* TrackList::rowOf(Rml::Element* element) const
	{
		for (; element != nullptr && element != m_view; element = element->GetParentNode())
			if (element->GetTagName() == "listitem")
			{
				const auto id = static_cast<uint64_t>(element->GetAttribute<double>("value", 0.0));
				const auto found = m_index.find(id);
				return found == m_index.end() ? nullptr : &m_tracks[found->second];
			}
		return nullptr;
	}

	Rml::Element* TrackList::rowElement(uint64_t track) const
	{
		const auto found = m_index.find(track);
		if (!alive() || found == m_index.end() || static_cast<int>(found->second) >= m_view->GetNumChildren())
			return nullptr;
		return m_view->GetChild(static_cast<int>(found->second));
	}

	void TrackList::ProcessEvent(Rml::Event& event)
	{
		if (!alive())
			return;
		Rml::Element* target = event.GetTargetElement();
		switch (event.GetId())
		{
		case Rml::EventId::Mousedown:
			mouseDown(event);
			break;
		case Rml::EventId::Mousemove:
			mouseMove(event);
			break;
		case Rml::EventId::Mouseup:
			mouseUp(event);
			break;
		case Rml::EventId::Click:
			for (Rml::Element* element = target; element != nullptr && element != m_view;
				element = element->GetParentNode())
				if (element->HasAttribute("control"))
				{
					if (const Row* row = rowOf(element); row != nullptr && !element->HasAttribute("disabled"))
						control(*row, element->GetAttribute<Rml::String>("control", ""));
					break;
				}
			break;
		case Rml::EventId::Dblclick:
			for (Rml::Element* element = target; element != nullptr && element != m_view;
				element = element->GetParentNode())
				if (element->IsClassSet("track-name"))
				{
					if (const Row* row = rowOf(element))
						rename(row->id);
					break;
				}
			break;
		case Rml::EventId::Mousescroll:
			m_display.scrollVertically(
				m_display.view().top + (event.GetParameter<float>("wheel_delta_y", 0.0F) * g_trackHeight));
			event.StopPropagation();
			break;
		case Rml::EventId::Keydown:
			if (m_renaming != 0 && target != nullptr && target->GetTagName() == "input")
			{
				const auto key = event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN);
				if (key == Rml::Input::KI_RETURN || key == Rml::Input::KI_NUMPADENTER || key == Rml::Input::KI_ESCAPE)
				{
					finishRename(key != Rml::Input::KI_ESCAPE);
					event.StopPropagation();
				}
			}
			break;
		case Rml::EventId::Blur:
			if (m_renaming != 0 && target != nullptr && target->GetTagName() == "input")
				finishRename(true);
			break;
		case Rml::EventId::Change:
			// Enter in the name being typed; the selection, which the keys may have moved out of view.
			if (m_renaming != 0 && target != nullptr && target->GetTagName() == "input" &&
				event.GetParameter<bool>("linebreak", false))
				finishRename(true);
			else if (target == m_view)
				if (const std::vector<uint64_t> tracks = selection(); !tracks.empty())
					reveal(tracks.front());
			break;
		default:
			if (event.GetType() == g_treeToggleEvent && target == m_view)
			{
				const auto id = static_cast<uint64_t>(
					std::strtoull(event.GetParameter<Rml::String>("value", "").c_str(), nullptr, 10));
				setFolded(id, !event.GetParameter<bool>("expanded", true));
			}
			break;
		}
	}

	void TrackList::control(const Row& row, const std::string& name)
	{
		const Json& project = m_display.project();
		if (name == "read")
		{
			// Every automation lane of the track reads, or none does.
			bool all = true;
			std::vector<uint64_t> lanes;
			for (const Json& lane : project.at("automation"))
				if (lane.at("track") == row.id)
				{
					lanes.push_back(lane.at("id").get<uint64_t>());
					all = all && lane.at("read").get<bool>();
				}
			Json commands = Json::array();
			for (const uint64_t lane : lanes)
				commands.push_back(
					{{"command", "automationLane.set"}, {"parameters", {{"lane", lane}, {"read", !all}}}});
			transaction("Read Automation", std::move(commands));
			return;
		}
		if (name == "timeBase")
		{
			const Json& track = project.at("tracks").at(m_index.at(row.id));
			m_binding.send("track.set",
				{{"track", row.id}, {"timeBase", track.at("timeBase") == "linear" ? "musical" : "linear"}});
			return;
		}
		const Json* channel = channelOf(row);
		if (channel == nullptr)
			return;
		const std::string key = name == "record" ? "recordEnable" : name;
		m_binding.send("channel.set", {{"channel", row.channel}, {key, !channel->value(key, false)}});
	}

	void TrackList::transaction(const std::string& description, Json commands)
	{
		if (commands.empty())
			return;
		m_binding.send("history.transaction", {{"description", description}, {"commands", std::move(commands)}});
	}

	std::vector<uint64_t> TrackList::topSelection() const
	{
		const std::vector<uint64_t> tracks = selection();
		std::vector<uint64_t> top;
		for (const uint64_t track : tracks)
		{
			bool inside = false;
			for (uint64_t parent = m_tracks[m_index.at(track)].parent; parent != 0 && !inside;
				parent = m_tracks[m_index.at(parent)].parent)
				inside = std::find(tracks.begin(), tracks.end(), parent) != tracks.end();
			if (!inside)
				top.push_back(track);
		}
		return top;
	}

	void TrackList::step(int direction)
	{
		const std::vector<uint64_t>& lanes = m_display.laneTracks(); // the tracks shown
		if (lanes.empty())
			return;
		const std::vector<uint64_t> tracks = selection();
		const auto at = tracks.empty() ? lanes.end() : std::find(lanes.begin(), lanes.end(), tracks.front());
		const std::ptrdiff_t before = direction > 0 ? -1 : 0; // nothing selected: start from an end
		std::ptrdiff_t index = at == lanes.end() ? before : at - lanes.begin();
		index = std::clamp<std::ptrdiff_t>(index + direction, 0, static_cast<std::ptrdiff_t>(lanes.size()) - 1);
		const uint64_t track = lanes[static_cast<size_t>(index)];
		select({track});
		reveal(track);
	}

	void TrackList::reveal(uint64_t track)
	{
		const std::vector<uint64_t>& lanes = m_display.laneTracks();
		const std::vector<double>& tops = m_display.laneTops();
		const auto at = std::find(lanes.begin(), lanes.end(), track);
		if (!alive() || at == lanes.end())
			return;
		const auto lane = static_cast<size_t>(at - lanes.begin());
		const double shown = m_rows->GetClientHeight() / dpRatio();
		const double top = m_display.view().top;
		if (tops[lane] < top)
			m_display.scrollVertically(tops[lane]);
		else if (tops[lane + 1] > top + shown)
			m_display.scrollVertically(tops[lane + 1] - shown);
		place();
	}

	void TrackList::rename(uint64_t track)
	{
		finishRename(true);
		Rml::Element* element = rowElement(track);
		Rml::Element* field = element == nullptr ? nullptr : element->QuerySelector(".track-name-field");
		if (field == nullptr)
			return;
		m_renaming = track;
		m_renamed = m_display.project().at("tracks").at(m_index.at(track)).at("name").get<std::string>();
		// The field shows instead of the name while the row has .renaming.
		element->SetClass("renaming", true);
		field->SetAttribute("value", m_renamed);
		field->Focus();
		if (auto* input = dynamic_cast<Rml::ElementFormControlInput*>(field))
			input->Select(); // typing replaces the name
		if (Rml::Context* context = field->GetContext())
			context->RequestNextUpdate(0);
	}

	void TrackList::finishRename(bool keep)
	{
		// Called from the field's own events: it hides the field rather than removing it.
		const uint64_t track = std::exchange(m_renaming, 0);
		Rml::Element* element = track == 0 ? nullptr : rowElement(track);
		Rml::Element* name = element == nullptr ? nullptr : element->QuerySelector(".track-name");
		const Rml::Element* field = element == nullptr ? nullptr : element->QuerySelector(".track-name-field");
		if (name == nullptr || field == nullptr)
			return;
		const auto typed = field->GetAttribute<Rml::String>("value", m_renamed);
		element->SetClass("renaming", false);
		name->SetInnerRML(Rml::StringUtilities::EncodeRml(keep ? typed : m_renamed));
		if (keep && typed != m_renamed)
			m_binding.send("track.set", {{"track", track}, {"name", typed}});
		m_view->Focus(); // the keys go on selecting tracks
	}

	void TrackList::mouseDown(Rml::Event& event)
	{
		Rml::Element* target = event.GetTargetElement();
		const Row* row = rowOf(target);
		if (row == nullptr)
			return;
		// A right click on a track that isn't selected selects it, for the context menu.
		const std::vector<uint64_t> tracks = selection();
		const bool selected = std::find(tracks.begin(), tracks.end(), row->id) != tracks.end();
		if (event.GetParameter<int>("button", 0) == 1 && !selected)
			select({row->id});
		if (event.GetParameter<int>("button", 0) != 0)
			return;
		for (const Rml::Element* part = target; part != nullptr && part->GetTagName() != "listitem";
			part = part->GetParentNode())
			if (part->HasAttribute("control") || part->GetTagName() == "input" || part->IsClassSet("tree-toggle"))
				return; // the control's
		Rml::Element* element = rowElement(row->id);
		const auto y = event.GetParameter<float>("mouse_y", 0.0F);
		m_dragY = y;
		m_dragTrack = row->id;
		const double bottom =
			(element->GetAbsoluteTop() + element->GetBox().GetSize(Rml::BoxArea::Border).y - y) / dpRatio();
		if (bottom <= g_edge)
		{
			m_drag = Drag::Height;
			m_dragHeight = m_display.trackHeight(row->id);
			m_dragAll = held(event, "ctrl_key") || held(event, "meta_key");
		}
		else
		{
			m_drag = Drag::Pressed;
		}
	}

	void TrackList::mouseMove(Rml::Event& event)
	{
		if (m_drag == Drag::None)
			return;
		const auto y = event.GetParameter<float>("mouse_y", 0.0F);
		const double dy = (y - m_dragY) / dpRatio();
		if (m_drag == Drag::Height)
		{
			const double height = m_dragHeight + (dy / m_display.view().trackZoom);
			if (m_dragAll)
				m_display.setTrackHeights(height);
			else
				m_display.setTrackHeight(m_dragTrack, height);
			return;
		}
		if (m_drag == Drag::Pressed && std::abs(dy) < g_dragThreshold)
			return;
		if (m_drag == Drag::Pressed)
		{
			// Dragging a track that isn't selected moves it alone.
			const std::vector<uint64_t> tracks = selection();
			if (std::find(tracks.begin(), tracks.end(), m_dragTrack) == tracks.end())
				select({m_dragTrack});
			m_drag = Drag::Move;
		}
		std::tie(m_dropParent, m_dropBefore) = dropAt(y);
		// The line where they go: above the track they go before, or below the last track of the folder.
		double top = 0.0;
		const std::vector<uint64_t>& lanes = m_display.laneTracks();
		const std::vector<double>& tops = m_display.laneTops();
		if (const auto before = std::find(lanes.begin(), lanes.end(), m_dropBefore);
			m_dropBefore != 0 && before != lanes.end())
		{
			top = tops[static_cast<size_t>(before - lanes.begin())];
		}
		else
		{
			// After the last shown track inside the folder (or of the project).
			for (size_t lane = 0; lane < lanes.size(); ++lane)
			{
				bool inside = m_dropParent == 0;
				for (uint64_t parent = m_tracks[m_index.at(lanes[lane])].parent; parent != 0 && !inside;
					parent = m_tracks[m_index.at(parent)].parent)
					inside = parent == m_dropParent;
				if (inside || lanes[lane] == m_dropParent)
					top = tops[lane + 1];
			}
		}
		m_dropLine->SetProperty(Rml::PropertyId::Top, dp(top - m_display.view().top));
		setClass(*m_dropLine, "shown", true);
	}

	void TrackList::mouseUp(Rml::Event& /*event*/)
	{
		const Drag drag = std::exchange(m_drag, Drag::None);
		if (alive())
			setClass(*m_dropLine, "shown", false);
		if (drag == Drag::Move)
			move(m_dropParent, m_dropBefore);
	}

	std::pair<uint64_t, uint64_t> TrackList::dropAt(float y) const
	{
		const std::vector<uint64_t>& lanes = m_display.laneTracks();
		const std::vector<double>& tops = m_display.laneTops();
		const double at = ((y - m_rows->GetAbsoluteTop()) / dpRatio()) + m_display.view().top;
		for (size_t lane = 0; lane < lanes.size(); ++lane)
		{
			if (at >= tops[lane + 1])
				continue;
			const Row& row = m_tracks[m_index.at(lanes[lane])];
			const double into = (at - tops[lane]) / (tops[lane + 1] - tops[lane]);
			// The middle of a folder's row: into the folder, last (reference manual p. 214).
			if (row.type == "folder" && into > 0.25 && into < 0.75)
				return {row.id, 0};
			if (into < 0.5 || lane + 1 == lanes.size())
				return into < 0.5 ? std::pair{row.parent, row.id} : std::pair{row.parent, uint64_t{0}};
			// Below the middle: before the next shown track, in its folder, unless that is inside this one.
			const Row& next = m_tracks[m_index.at(lanes[lane + 1])];
			return {next.parent, next.id};
		}
		return {0, 0};
	}

	void TrackList::move(uint64_t parent, uint64_t before)
	{
		// Simulates the moves, so that each track.move's index counts the tracks as the moves before left them.
		std::map<uint64_t, std::vector<uint64_t>> children;
		for (const Row& row : m_tracks)
			children[row.parent].push_back(row.id);
		Json commands = Json::array();
		for (const uint64_t track : topSelection())
		{
			// Not into itself or a folder inside it.
			bool inside = track == parent;
			for (uint64_t up = parent; up != 0 && !inside; up = m_tracks[m_index.at(up)].parent)
				inside = up == track;
			if (inside || track == before)
				continue;
			const Row& row = m_tracks[m_index.at(track)];
			if (row.type == "marker" && parent != 0)
				continue;
			std::vector<uint64_t>& from = children[row.parent];
			from.erase(std::find(from.begin(), from.end(), track));
			std::vector<uint64_t>& to = children[parent];
			const auto at = before == 0 ? to.end() : std::find(to.begin(), to.end(), before);
			const auto index = at - to.begin();
			to.insert(at, track);
			commands.push_back(
				{{"command", "track.move"}, {"parameters", {{"track", track}, {"parent", parent}, {"index", index}}}});
		}
		transaction("Move Tracks", std::move(commands));
	}
}
