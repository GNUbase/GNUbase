// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/tracklist.h"

#include "ui/mixer.h"
#include "ui/timeformat.h"
#include "ui/widgets.h"
#include "ui/widgetutils.h"

#include "platform/log.h"

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
#include <filesystem>
#include <iterator>
#include <optional>
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
		constexpr double g_width = 120.0;       // dp of a name or controls area that ui.trackWidths leaves out
		constexpr double g_toggleSize = 16.0;   // dp of Show/Hide Automation, in a row's lower left corner

		constexpr std::array<Rml::EventId, 5> g_rootEvents = {Rml::EventId::Mousedown, Rml::EventId::Dblclick,
			Rml::EventId::Click, Rml::EventId::Mousescroll, Rml::EventId::Change};
		constexpr std::array<Rml::EventId, 2> g_documentEvents = {Rml::EventId::Mousemove, Rml::EventId::Mouseup};

		Rml::Property dp(double value)
		{
			return {static_cast<float>(value), Rml::Unit::DP};
		}

		// Ruler tracks' controls (reference manual p. 159).
		bool rulerControl(const std::string& control)
		{
			return control == "timeFormat" || control == "timeOffsetOn" || control == "timeOffset" ||
				control == "setTimeOffset";
		}

		// The ruler time formats as the track list's drop-down list names them.
		const widgets::Options& timeFormatOptions()
		{
			static const widgets::Options options = {{"barsBeats", "Bars+Beats"}, {"seconds", "Seconds"},
				{"timecode", "Timecode"}, {"samples", "Samples"}, {"userFrames", "fps (User)"}};
			return options;
		}

		// A ruler track's time offset in seconds, and a time in seconds in the track's time base.
		double offsetSeconds(const Json& track, const TimeContext& time)
		{
			const auto offset = track.value("timeOffset", int64_t{0});
			return track.at("timeBase") == "linear" ? static_cast<double>(offset) / time.sampleRate
													: time.tempo.toSeconds(offset);
		}

		int64_t inTimeBase(const Json& track, double seconds, const TimeContext& time)
		{
			return track.at("timeBase") == "linear" ? std::llround(seconds * time.sampleRate)
													: time.tempo.toTicks(seconds);
		}

		// The controls a track type can have.
		bool applies(const std::string& control, const std::string& type, bool hasChannel)
		{
			if (type == "ruler" || rulerControl(control))
				return type == "ruler" && rulerControl(control);
			// The tempo track's only control is Activate Tempo Track (reference manual p. 162).
			if (type == "tempo" || control == "tempoTrack")
				return type == "tempo" && control == "tempoTrack";
			if (!hasChannel)
				return control == "timeBase" && type != "folder" && type != "ruler" && type != "signature";
			if (control == "record" || control == "monitor")
				return type == "audio" || type == "instrument" || type == "midi";
			if (control == "timeBase")
				return type == "audio" || type == "instrument" || type == "midi";
			return true;
		}

		// How a control looks in its button, or the ruler tracks' drop-down list and display.
		std::string buttonRml(const std::string& control)
		{
			if (control == "timeFormat")
				return R"(<select class="time-format" control="timeFormat" title="Time Format"/>)";
			if (control == "timeOffset")
				return R"(<textdisplay class="display time-offset" control="timeOffset" title="Time Offset"/>)";
			if (control == "setTimeOffset")
				return R"(<button class="set-time-offset" control="setTimeOffset" )"
					   R"(title="Set Time Offset to Project Cursor Position">C</button>)";
			static const std::map<std::string, std::pair<const char*, const char*>> looks = {{"mute", {"Mute", "M"}},
				{"solo", {"Solo", "S"}},
				{"record", {"Record Enable", R"(<svg class="icon" src="../icons/track-record-arm.svg"/>)"}},
				{"monitor", {"Monitor", R"(<svg class="icon" src="../icons/track-monitor.svg"/>)"}},
				{"read", {"Read Automation", R"(<svg class="icon" src="../icons/track-read-automation.svg"/>)"}},
				{"timeBase", {"Switch Time Base", "L"}}, {"tempoTrack", {"Activate Tempo Track", "T"}},
				{"timeOffsetOn", {"Time Offset On/Off", "O"}}};
			const auto& [title, content] = looks.at(control);
			return R"(<button class="toggle )" + control + R"(" control=")" + control + R"(" title=")" + title +
				R"(">)" + content + "</button>";
		}

		bool held(const Rml::Event& event, const char* key)
		{
			return event.GetParameter<int>(key, 0) != 0;
		}

		// Whether the element is the ancestor or inside it.
		bool inside(const Rml::Element* element, const Rml::Element* ancestor)
		{
			for (; element != nullptr; element = element->GetParentNode())
				if (element == ancestor)
					return true;
			return false;
		}

		// Whether the element is a drop-down list or inside one, which keeps Up and Down.
		bool inSelect(const Rml::Element* element)
		{
			for (; element != nullptr; element = element->GetParentNode())
				if (element->GetTagName() == "select")
					return true;
			return false;
		}

		void setClass(Rml::Element& element, const char* name, bool set)
		{
			if (element.IsClassSet(name) != set)
				element.SetClass(name, set);
		}

		void setWidth(Rml::Element* element, double width)
		{
			const Rml::Property value = dp(width);
			if (const Rml::Property* now =
					element == nullptr ? nullptr : element->GetLocalProperty(Rml::PropertyId::Width);
				element != nullptr && (now == nullptr || !(*now == value)))
				element->SetProperty(Rml::PropertyId::Width, value);
		}

		// A width of ui.trackWidths, under key in widths, or the default where it has none.
		double widthOf(const Json& widths, const std::string& key)
		{
			const auto found = widths.find(key);
			return found != widths.end() && found->is_number() ? found->get<double>() : g_width;
		}
	}

	TrackList::TrackList(EventDisplay& display, EngineBinding& binding, Rml::Element& root)
		: m_display(display), m_binding(binding), m_root(root.GetObserverPtr()), m_document(root.GetOwnerDocument())
	{
		Rml::ElementPtr rows = root.GetOwnerDocument()->CreateElement("div");
		rows->SetClass("track-rows", true);
		m_rows = root.AppendChild(std::move(rows));
		// Shift-click on a selected track deselects it (reference manual p. 219).
		m_rows->SetInnerRML(R"(<listview class="tracks" multiple="" shiftdeselects=""/><div class="drop-line"/>)");
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
			{"track.removeSelected", [this] { removeSelected(); }},
			{"track.duplicateSelected",
				[this, selected]
				{
					Json commands = Json::array();
					// The project has one tempo track and one signature track.
					selected(
						[this, &commands](uint64_t track)
						{
							if (const std::string& type = m_tracks[m_index.at(track)].type;
								type != "tempo" && type != "signature")
								commands.push_back(
									{{"command", "track.duplicate"}, {"parameters", {{"track", track}}}});
						});
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
		// Freeze and render in place (docs/features/render-in-place.md), with the settings the commands default to.
		const auto renderable = [this]
		{
			std::vector<uint64_t> tracks;
			for (const uint64_t track : selection())
				if (const std::string& type = m_tracks[m_index.at(track)].type; type == "audio" || type == "instrument")
					tracks.push_back(track);
			return tracks;
		};
		m_actions["track.freezeSelected"] = [this, renderable]
		{
			// Unfreezes them if every one is frozen, else freezes those that aren't.
			const Json& project = m_display.project();
			std::vector<uint64_t> unfrozen;
			std::vector<uint64_t> frozen;
			for (const uint64_t track : renderable())
				(project.at("tracks").at(m_index.at(track)).at("frozen").is_null() ? unfrozen : frozen)
					.push_back(track);
			if (!unfrozen.empty())
				m_binding.send("track.freeze", {{"tracks", unfrozen}});
			else if (!frozen.empty())
				m_binding.send("track.unfreeze", {{"tracks", frozen}});
		};
		m_actions["track.renderSelected"] = [this, renderable]
		{
			if (const std::vector<uint64_t> tracks = renderable(); !tracks.empty())
				m_binding.send("track.renderInPlace", {{"tracks", tracks}});
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
				// A new folder where the first of them was, with them inside; marker tracks, the tempo track and the
				// signature track stay at the top level.
				Json tracks = Json::array();
				for (const uint64_t track : topSelection())
					if (const std::string& type = m_tracks[m_index.at(track)].type;
						type != "marker" && type != "tempo" && type != "signature")
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

		// Automation tracks (#121, reference manual p. 880).
		m_actions["track.toggleAutomationSelected"] = [this]
		{
			const std::vector<uint64_t> tracks = selection();
			const bool show = std::any_of(
				tracks.begin(), tracks.end(), [this](uint64_t track) { return !m_display.automationShown(track); });
			for (const uint64_t track : tracks)
				if (m_tracks[m_index.at(track)].channel != 0)
					m_display.showAutomation(track, show);
		};
		m_actions["track.showUsedAutomation"] = [this] { m_display.showUsedAutomation(); };
		// Lanes (#99, reference manual p. 226): Show Lanes.
		m_actions["track.toggleLanesSelected"] = [this]
		{
			std::vector<uint64_t> tracks;
			for (const uint64_t track : selection())
				if (const std::string& type = m_tracks[m_index.at(track)].type;
					type == "audio" || type == "instrument" || type == "midi")
					tracks.push_back(track);
			const bool show = std::any_of(
				tracks.begin(), tracks.end(), [this](uint64_t track) { return !m_display.lanesShown(track); });
			for (const uint64_t track : tracks)
				m_display.showLanes(track, show);
		};
		m_actions["track.hideAllAutomation"] = [this] { m_display.hideAllAutomation(); };
		// Copying automation to other tracks (#125, reference manual pp. 878-879), between the locators, which the
		// engine takes.
		for (const char* which : {"copy", "paste"})
		{
			const std::string prefix = std::string("track.") + which;
			m_actions[prefix + "AutomationAll"] = [this, which]
			{ m_binding.send(std::string("automation.") + which, Json::object()); };
			m_actions[prefix + "AutomationSelected"] = [this, which]
			{
				if (const std::vector<uint64_t> tracks = selection(); !tracks.empty())
					m_binding.send(std::string("automation.") + which, {{"tracks", tracks}});
			};
			m_actions[prefix + "AutomationSelectedParameters"] = [this, which]
			{
				if (const std::vector<uint64_t> lanes = selectedLanes(); !lanes.empty())
					m_binding.send(std::string("automation.") + which, {{"lanes", lanes}});
			};
		}
		m_parameterMenu = m_document->GetElementById("automation-parameter-menu");
		if (m_parameterMenu != nullptr)
			for (const Rml::EventId id : {Rml::EventId::Click, Rml::EventId::Change})
				m_parameterMenu->AddEventListener(id, this);
		// Track versions (#165, docs/features/track-versions.md) of the selected audio, instrument and MIDI tracks.
		for (const auto& [action, command] : {std::pair{"track.newVersion", "trackVersion.add"},
				 std::pair{"track.duplicateVersion", "trackVersion.duplicate"},
				 std::pair{"track.deleteVersion", "trackVersion.remove"},
				 std::pair{"track.assignCommonVersionId", "trackVersion.assignCommonId"}})
			m_actions[action] = [this, command = std::string(command)]
			{
				if (const std::vector<uint64_t> tracks = versioned(); !tracks.empty())
					m_binding.send(command, {{"tracks", tracks}});
			};
		m_actions["track.selectSameVersionId"] = [this]
		{
			const std::vector<uint64_t> tracks = versioned();
			if (tracks.empty())
				return;
			const Json& all = m_display.project().at("tracks");
			const Json& id = all.at(m_index.at(tracks.front())).at("activeVersion");
			std::vector<uint64_t> same;
			for (const Json& track : all)
				for (const Json& version : track.value("versions", Json::array()))
					if (version.at("id") == id)
						same.push_back(track.at("id").get<uint64_t>());
			select(same);
			showVersions();
		};
		m_versionMenu = m_document->GetElementById("track-versions-list");
		if (m_versionMenu != nullptr)
			m_versionMenu->AddEventListener(Rml::EventId::Click, this);

		m_subscriptions.push_back(m_binding.on("settings.changed",
			[this](const engine::Event& event)
			{
				const Json& values = event.data.at("values");
				if (const auto controls = values.find("ui.trackControls"); controls != values.end())
					loadControls(*controls);
				if (const auto widths = values.find("ui.trackWidths"); widths != values.end())
					loadWidths(*widths);
				if (const auto warn = values.find("ui.removeTracksWarning"); warn != values.end() && warn->is_boolean())
					m_warnBeforeRemoving = warn->get<bool>();
				if (const auto only = values.find("ui.upDownSelectsTracksOnly");
					only != values.end() && only->is_boolean())
					m_upDownTracksOnly = only->get<bool>();
			}));
		for (const std::pair<const char*, bool*>& setting :
			{std::make_pair("ui.removeTracksWarning", &m_warnBeforeRemoving),
				std::make_pair("ui.upDownSelectsTracksOnly", &m_upDownTracksOnly)})
			m_binding.send("settings.get", {{"key", setting.first}},
				[value = setting.second, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
				{
					if (!alive.expired() && result.ok() && result.value.at("value").is_boolean())
						*value = result.value.at("value").get<bool>();
				});
		m_binding.send("settings.get", {{"key", "ui.trackControls"}},
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (!alive.expired() && result.ok())
					loadControls(result.value.at("value"));
			});
		m_binding.send("settings.get", {{"key", "ui.trackWidths"}},
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (!alive.expired() && result.ok())
					loadWidths(result.value.at("value"));
			});
		m_display.onLayout([this] { layout(); });
		layout();
	}

	TrackList::~TrackList()
	{
		m_display.onLayout({});
		if (auto* dialog = dynamic_cast<Rml::ElementDocument*>(m_removeDialog.get()))
		{
			// Nothing removes the tracks any more: the dialog goes too.
			dialog->RemoveEventListener(g_dialogCloseEvent, this);
			closeDialog(*dialog, "cancel");
		}
		if (!alive())
			return;
		if (m_parameterMenu != nullptr)
			for (const Rml::EventId id : {Rml::EventId::Click, Rml::EventId::Change})
				m_parameterMenu->RemoveEventListener(id, this);
		if (m_versionMenu != nullptr)
			m_versionMenu->RemoveEventListener(Rml::EventId::Click, this);
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

	bool TrackList::selectsFolder(bool withGroupChannel) const
	{
		const std::vector<uint64_t> tracks = selection();
		return std::any_of(tracks.begin(), tracks.end(),
			[this, withGroupChannel](uint64_t track)
			{
				const Row& row = m_tracks[m_index.at(track)];
				return row.type == "folder" && (row.channel != 0) == withGroupChannel;
			});
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

	void TrackList::loadWidths(const Json& value)
	{
		m_widths = value.is_object() ? value : Json::object();
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
			row.holds = track.value("versions", Json::array()).size() > 1; // as track.removeEmpty counts them
			if (const auto parent = index.find(row.parent); parent != index.end())
				row.level = tracks[parent->second].level + 1;
			index[row.id] = tracks.size();
			tracks.push_back(row);
		}
		// What Remove Selected Tracks warns about: the kinds the engine's track.removeEmpty counts as content too
		// (emptyTracks in src/engine/document/projectmodel.cpp); change both together.
		for (const char* kind : {"audioEvents", "audioParts", "midiParts", "markers", "automation"})
			for (const Json& item : project.at(kind))
				if (const auto track = index.find(item.at("track").get<uint64_t>()); track != index.end())
					tracks[track->second].holds = true;
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
				R"("><span class="track-color"/><span class="track-name"/><input type="text" class="track-name-field"/>)"
				R"(<div class="track-controls"/>)" +
				// Show/Hide Automation in the lower left corner, and the automation tracks below (#121).
				(row.channel != 0 ? R"(<button class="automation-toggle icon-only" title="Show/Hide Automation">)"
									R"(<svg class="icon" src="../icons/general-arrow-down.svg"/></button>)"
									R"(<div class="automation-rows"/>)"
								  : "") +
				"</listitem>";
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
		showVersions();
		std::map<uint64_t, std::vector<bool>> reading; // each track's automation lanes, whether they read
		for (const Json& lane : project.at("automation"))
			reading[lane.at("track").get<uint64_t>()].push_back(lane.at("read").get<bool>());
		const Json& tracks = project.at("tracks");
		TimeContext time;
		readTimeContext(project, time);
		const double nameWidth = widthOf(m_widths, "name");
		const Json controlsWidths = m_widths.value("controls", Json::object());
		for (int i = 0; i < m_view->GetNumChildren() && static_cast<size_t>(i) < m_tracks.size(); ++i)
		{
			Rml::Element* element = m_view->GetChild(i);
			const Row& row = m_tracks[static_cast<size_t>(i)];
			const Json& track = tracks[static_cast<size_t>(i)];
			setClass(*element, "disabled", !track.at("enabled").get<bool>());
			setClass(*element, "frozen", !track.value("frozen", Json()).is_null());
			const bool expanded = !folded(row.id);
			if (expanded != element->HasAttribute("expanded"))
			{
				if (expanded)
					element->SetAttribute("expanded", "");
				else
					element->RemoveAttribute("expanded");
			}
			// The color strip (reference manual p. 113): the track's color, which is user data, or the theme's.
			if (Rml::Element* strip = element->QuerySelector(".track-color"); strip != nullptr)
			{
				const Json& color = track.at("color");
				const std::string value = color.is_string() ? color.get<std::string>() : std::string();
				if (strip->GetAttribute<Rml::String>("color", "") != value)
				{
					strip->SetAttribute("color", value);
					if (value.empty())
						strip->RemoveProperty("background-color");
					else
						strip->SetProperty("background-color", value);
				}
			}
			// The name, unless it's being typed.
			Rml::Element* label = element->QuerySelector(".track-name");
			if (label != nullptr && row.id != m_renaming)
			{
				std::string text = Rml::StringUtilities::EncodeRml(track.at("name").get<std::string>());
				// With more than one version, the active one's name (reference manual p. 236).
				for (const Json& version : track.value("versions", Json::array()))
					if (track.at("versions").size() > 1 && version.at("id") == track.at("activeVersion"))
						text += R"( <span class="track-version">)" +
							Rml::StringUtilities::EncodeRml(version.at("name").get<std::string>()) + "</span>";
				if (label->GetInnerRML() != text)
					label->SetInnerRML(text);
			}
			// The widths of the name and the controls area (reference manual p. 191), which are user data.
			setWidth(label, nameWidth);
			setWidth(element->QuerySelector(".track-name-field"), nameWidth);
			Rml::Element* controls = element->QuerySelector(".track-controls");
			if (controls == nullptr)
				continue;
			setWidth(controls, widthOf(controlsWidths, row.type));
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
				if (name == "timeFormat")
				{
					widgets::fill(*button, timeFormatOptions(), track.value("timeFormat", std::string()));
					continue;
				}
				if (name == "timeOffset")
				{
					const std::optional<TimeFormat> format =
						timeFormatFromName(track.value("timeFormat", std::string()));
					const std::string text = format ? formatTime(*format, offsetSeconds(track, time), time) : "";
					if (button->GetAttribute<Rml::String>("value", "") != text)
						button->SetAttribute("value", text);
					continue;
				}
				if (name == "setTimeOffset")
					continue;
				bool on = false;
				if (name == "record")
					on = channel != nullptr && channel->at("recordEnable").get<bool>();
				else if (name == "timeBase")
					on = track.at("timeBase") == "linear";
				else if (name == "tempoTrack")
					on = project.value("tempoTrackActive", false);
				else if (name == "timeOffsetOn")
					on = track.value("timeOffsetOn", false);
				else if (name == "read")
					on = !lanes.empty() && std::all_of(lanes.begin(), lanes.end(), [](bool read) { return read; });
				else
					on = channel != nullptr && channel->value(name, false);
				setClass(*button, "checked", on);
				if (name == "solo")
					setClass(*button, "defeated", channel != nullptr && channel->value("soloDefeat", false));
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
		{
			Rml::Element* element = ownLane(lane) ? rowElement(lanes[lane]) : nullptr;
			if (element == nullptr)
				continue;
			// The row reaches down over its lanes (#99) and automation tracks' lanes (#121).
			const size_t end = rowEnd(lane);
			element->SetProperty(Rml::PropertyId::Height, dp(tops[end] - tops[lane]));
			showAutomationRows(*element, lane, end);
		}
		m_view->SetProperty(Rml::PropertyId::Top, dp(-m_display.view().top));
		m_rows->SetScrollTop(0.0F); // the list view's keys scroll it into view; the display scrolls it
	}

	bool TrackList::ownLane(size_t lane) const
	{
		return m_display.laneAutomation()[lane] == 0 && m_display.laneTakes()[lane] < 0;
	}

	size_t TrackList::rowEnd(size_t lane) const
	{
		size_t end = lane + 1;
		while (end < m_display.laneTracks().size() && !ownLane(end))
			++end;
		return end;
	}

	const Json* TrackList::laneOf(uint64_t lane) const
	{
		const Json& project = m_display.project();
		if (!project.is_object())
			return nullptr;
		for (const Json& json : project.at("automation"))
			if (json.at("id") == lane)
				return &json;
		return nullptr;
	}

	const Json* TrackList::targetOf(const Json& lane) const
	{
		const Json* targets = m_display.automationTargets(lane.at("track").get<uint64_t>());
		if (targets != nullptr)
			for (const Json& target : *targets)
				if (target.at("target") == lane.at("target"))
					return &target;
		return nullptr;
	}

	void TrackList::showAutomationRows(Rml::Element& row, size_t lane, size_t end)
	{
		Rml::Element* toggle = row.QuerySelector(".automation-toggle");
		Rml::Element* rows = row.QuerySelector(".automation-rows");
		if (toggle == nullptr || rows == nullptr)
			return;
		const std::vector<double>& tops = m_display.laneTops();
		const std::vector<uint64_t>& automation = m_display.laneAutomation();
		const uint64_t track = m_display.laneTracks()[lane];
		const double own = tops[lane + 1] - tops[lane];
		// The automation tracks come after the track's lanes (#99).
		size_t first = lane + 1;
		while (first < end && automation[first] == 0)
			++first;
		setClass(*toggle, "checked", m_display.automationShown(track));
		toggle->SetProperty(Rml::PropertyId::Top, dp(own - g_toggleSize));
		rows->SetProperty(Rml::PropertyId::Top, dp(tops[first] - tops[lane]));
		// A row per automation track: its parameter, which opens the parameter selector, Read and Append.
		std::string rml;
		for (size_t i = first; i < end; ++i)
		{
			const Json* json = laneOf(automation[i]);
			const Json* target = json == nullptr ? nullptr : targetOf(*json);
			const std::string name = target != nullptr ? target->at("name").get<std::string>() : "";
			rml += R"(<div class="automation-row" lane=")" + std::to_string(automation[i]) +
				R"("><button class="automation-parameter" menu="automation-parameter-menu" title="Parameter">)" +
				Rml::StringUtilities::EncodeRml(name) +
				R"(</button><button class="toggle automation-read" automation-control="read" title="Read Automation">)"
				R"(<svg class="icon" src="../icons/track-read-automation.svg"/></button>)"
				R"(<button class="automation-append icon-only" automation-control="append" )"
				R"(title="Append Automation Track"><svg class="icon" src="../icons/general-add.svg"/></button></div>)";
		}
		if (rows->GetAttribute<Rml::String>("rows", "") != rml)
		{
			rows->SetAttribute("rows", rml);
			rows->SetInnerRML(rml);
		}
		for (int i = 0; i < rows->GetNumChildren(); ++i)
		{
			Rml::Element* element = rows->GetChild(i);
			const size_t at = first + static_cast<size_t>(i);
			element->SetProperty(Rml::PropertyId::Height, dp(tops[at + 1] - tops[at]));
			setClass(*element, "selected", m_selectedLanes.count(automation[at]) != 0);
			const Json* json = laneOf(automation[at]);
			if (Rml::Element* read = element->QuerySelector(".automation-read"); read != nullptr && json != nullptr)
				setClass(*read, "checked", json->at("read").get<bool>());
		}
	}

	// The parameter selector (reference manual p. 880): the track's parameters, which a search narrows, and the
	// automation track's functions.
	std::vector<uint64_t> TrackList::versioned() const
	{
		std::vector<uint64_t> tracks;
		for (const uint64_t track : selection())
			if (const std::string& type = m_tracks[m_index.at(track)].type;
				type == "audio" || type == "instrument" || type == "midi")
				tracks.push_back(track);
		return tracks;
	}

	void TrackList::showVersions()
	{
		const std::vector<uint64_t> tracks = versioned();
		std::string rml;
		if (!tracks.empty())
		{
			const Json& track = m_display.project().at("tracks").at(m_index.at(tracks.front()));
			for (const Json& version : track.at("versions"))
				rml += R"(<menuitem class="track-version)" +
					std::string(version.at("id") == track.at("activeVersion") ? " checked" : "") + R"(" version=")" +
					std::to_string(version.at("id").get<int>()) + R"(">)" +
					Rml::StringUtilities::EncodeRml(version.at("name").get<std::string>()) + "</menuitem>";
		}
		if (m_versionMenu != nullptr && rml != m_versionRml)
		{
			m_versionRml = rml;
			m_versionMenu->SetInnerRML(rml);
		}
	}

	void TrackList::versionEvent(Rml::Event& event)
	{
		const Rml::Element* item = event.GetTargetElement();
		while (item != nullptr && item != m_versionMenu && !item->HasAttribute("version"))
			item = item->GetParentNode();
		const std::vector<uint64_t> tracks = versioned();
		if (item == nullptr || item == m_versionMenu || tracks.empty())
			return;
		m_binding.send(
			"trackVersion.activate", {{"tracks", tracks}, {"version", item->GetAttribute<int>("version", 1)}});
	}

	void TrackList::openParameters(uint64_t lane)
	{
		const Json* json = laneOf(lane);
		if (m_parameterMenu == nullptr || json == nullptr)
			return;
		// The functions at once, the parameters once automationLane.targets is in.
		const Json* known = m_display.automationTargets(json->at("track").get<uint64_t>());
		const Json targets = known != nullptr ? *known : Json::array();
		m_parameterLane = lane;
		std::string rml = R"(<input type="text" class="automation-search" placeholder="Search"/>)"
						  R"(<menuitem automation-function="remove">Remove Parameter</menuitem>)"
						  R"(<menuitem automation-function="removeUnused">Remove Unused Parameters</menuitem>)";
		if (json->at("target").at("type") == "plugin")
			if (const Json* target = targetOf(*json))
				rml += R"(<menuitem automation-function="onlyPlugin">Show Only Automation Tracks of )" +
					Rml::StringUtilities::EncodeRml(target->at("group").get<std::string>()) + "</menuitem>";
		rml += "<menuseparator/>";
		for (size_t i = 0; i < targets.size(); ++i)
		{
			const Json& target = targets.at(i);
			const std::string group = target.at("group").get<std::string>();
			const std::string name = (group == "Channel" ? "" : group + ": ") + target.at("name").get<std::string>();
			rml += R"(<menuitem class="automation-target)" +
				std::string(target.at("target") == json->at("target") ? " checked" : "") + R"(" target-index=")" +
				std::to_string(i) + R"(">)" + Rml::StringUtilities::EncodeRml(name) + "</menuitem>";
		}
		m_parameterMenu->SetInnerRML(rml);
	}

	void TrackList::parameterEvent(Rml::Event& event)
	{
		const Rml::Element* target = event.GetTargetElement();
		const Json* json = laneOf(m_parameterLane);
		if (json == nullptr || target == nullptr)
			return;
		const auto track = json->at("track").get<uint64_t>();
		if (event.GetId() == Rml::EventId::Change && target->IsClassSet("automation-search"))
		{
			// The search narrows the parameters to those whose name has the text, in any case.
			const auto lower = [](std::string text)
			{
				std::transform(text.begin(), text.end(), text.begin(),
					[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				return text;
			};
			const std::string text = lower(target->GetAttribute<Rml::String>("value", ""));
			for (int i = 0; i < m_parameterMenu->GetNumChildren(); ++i)
				if (Rml::Element* item = m_parameterMenu->GetChild(i); item->IsClassSet("automation-target"))
					setClass(*item, "hidden", lower(item->GetInnerRML()).find(text) == std::string::npos);
			return;
		}
		if (event.GetId() != Rml::EventId::Click)
			return;
		const auto function = target->GetAttribute<Rml::String>("automation-function", "");
		if (function == "remove") // with its points, and the automation track closes (reference manual p. 878)
			m_binding.send("automationLane.remove", {{"lane", m_parameterLane}});
		else if (function == "removeUnused")
			m_display.removeUnusedAutomation(track);
		else if (function == "onlyPlugin")
			m_display.showOnlyPlugin(m_parameterLane);
		else if (target->HasAttribute("target-index"))
			if (const Json* targets = m_display.automationTargets(track))
			{
				const auto index = target->GetAttribute<int>("target-index", 0);
				if (index >= 0 && static_cast<size_t>(index) < targets->size())
					m_display.setAutomationTarget(
						m_parameterLane, targets->at(static_cast<size_t>(index)).at("target"));
			}
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
		if (m_removeDialog && event.GetCurrentElement() == m_removeDialog.get())
		{
			closedRemoveDialog(event);
			return;
		}
		if (!alive())
			return;
		if (m_parameterMenu != nullptr && event.GetCurrentElement() == m_parameterMenu)
		{
			parameterEvent(event);
			return;
		}
		if (m_versionMenu != nullptr && event.GetCurrentElement() == m_versionMenu)
		{
			versionEvent(event);
			return;
		}
		Rml::Element* target = event.GetTargetElement();
		// The automation tracks' rows and Show/Hide Automation (#121).
		const auto automationLane = [this](Rml::Element* element) { return laneAt(element); };
		const auto closest = [this](Rml::Element* element, const char* name)
		{
			for (; element != nullptr && element != m_view; element = element->GetParentNode())
				if (element->IsClassSet(name))
					return element;
			return static_cast<Rml::Element*>(nullptr);
		};
		if (event.GetId() == Rml::EventId::Mousedown && closest(target, "automation-parameter") != nullptr)
			openParameters(automationLane(target)); // before its click opens the menu
		if (event.GetId() == Rml::EventId::Click)
		{
			if (closest(target, "automation-toggle") != nullptr)
			{
				if (const Row* row = rowOf(target))
					m_display.showAutomation(row->id, !m_display.automationShown(row->id));
				return;
			}
			if (Rml::Element* button = closest(target, "automation-read"))
			{
				if (const Json* lane = laneOf(automationLane(button)))
					m_binding.send(
						"automationLane.set", {{"lane", lane->at("id")}, {"read", !lane->at("read").get<bool>()}});
				return;
			}
			if (Rml::Element* button = closest(target, "automation-append"))
			{
				if (const Row* row = rowOf(button))
					m_display.appendAutomation(row->id);
				return;
			}
		}
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
						control(*row, element->GetAttribute<Rml::String>("control", ""),
							held(event, "ctrl_key") || held(event, "meta_key"), held(event, "alt_key"));
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
		{
			const auto key = event.GetParameter<int>("key_identifier", Rml::Input::KI_UNKNOWN);
			if (m_renaming != 0 && target != nullptr && target->GetTagName() == "input")
			{
				if (key == Rml::Input::KI_RETURN || key == Rml::Input::KI_NUMPADENTER || key == Rml::Input::KI_ESCAPE)
				{
					finishRename(key != Rml::Input::KI_ESCAPE);
					event.StopPropagation();
				}
			}
			else if ((key == Rml::Input::KI_UP || key == Rml::Input::KI_DOWN) && inside(target, m_view) &&
				target->GetTagName() != "input" && !inSelect(target) && !held(event, "shift_key") &&
				!held(event, "ctrl_key") && !held(event, "alt_key") && !held(event, "meta_key"))
			{
				// Up and Down in the list step the tracks as their key commands do (a drop-down list keeps them),
				// selecting the first event on the track too; Shift extends the list view's selection.
				step(key == Rml::Input::KI_UP ? -1 : 1);
				event.StopPropagation();
			}
			break;
		}
		case Rml::EventId::Blur:
			if (m_renaming != 0 && target != nullptr && target->GetTagName() == "input")
				finishRename(true);
			break;
		case Rml::EventId::Change:
			// Enter in the name being typed; the selection, which the keys may have moved out of view.
			if (m_renaming != 0 && target != nullptr && target->GetTagName() == "input" &&
				event.GetParameter<bool>("linebreak", false))
			{
				finishRename(true);
			}
			else if (target != nullptr && target->HasAttribute("control"))
			{
				rulerChange(*target, event.GetParameter<Rml::String>("value", ""));
			}
			else if (target == m_view)
			{
				if (const std::vector<uint64_t> tracks = selection(); !tracks.empty())
					reveal(tracks.front());
				showVersions();
			}
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

	void TrackList::control(const Row& row, const std::string& name, bool exclusive, bool defeat)
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
		if (name == "tempoTrack")
		{
			m_binding.send("project.set", {{"tempoTrackActive", !project.value("tempoTrackActive", false)}});
			return;
		}
		if (name == "timeOffsetOn")
		{
			const Json& track = project.at("tracks").at(m_index.at(row.id));
			m_binding.send("track.set", {{"track", row.id}, {"timeOffsetOn", !track.value("timeOffsetOn", false)}});
			return;
		}
		if (name == "setTimeOffset")
		{
			m_binding.send("track.set", {{"track", row.id}, {"timeOffset", "cursor"}});
			return;
		}
		if (name == "timeFormat" || name == "timeOffset")
			return; // they change with what is chosen or typed
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
		Json parameters =
			key == "solo" ? soloClick(*channel, exclusive, defeat) : Json{{key, !channel->value(key, false)}};
		parameters["channel"] = row.channel;
		m_binding.send("channel.set", std::move(parameters));
	}

	void TrackList::rulerChange(Rml::Element& control, const std::string& value)
	{
		const Row* row = rowOf(&control);
		const auto name = control.GetAttribute<Rml::String>("control", "");
		if (row == nullptr || row->type != "ruler")
			return;
		const Json& project = m_display.project();
		const Json& track = project.at("tracks").at(m_index.at(row->id));
		if (name == "timeFormat")
		{
			if (!value.empty() && track.value("timeFormat", std::string()) != value)
				m_binding.send("track.set", {{"track", row->id}, {"timeFormat", value}});
			return;
		}
		if (name != "timeOffset")
			return;
		// A position typed in the track's time format; the display shows the offset again for anything else.
		TimeContext time;
		readTimeContext(project, time);
		const std::optional<TimeFormat> format = timeFormatFromName(track.value("timeFormat", std::string()));
		const std::optional<double> seconds = format ? parseTime(*format, value, time) : std::nullopt;
		if (seconds)
			m_binding.send("track.set", {{"track", row->id}, {"timeOffset", inTimeBase(track, *seconds, time)}});
		else
			control.SetAttribute("value", format ? formatTime(*format, offsetSeconds(track, time), time) : "");
	}

	void TrackList::transaction(const std::string& description, Json commands)
	{
		if (commands.empty())
			return;
		m_binding.send("history.transaction", {{"description", description}, {"commands", std::move(commands)}});
	}

	void TrackList::removeSelected()
	{
		const std::vector<uint64_t> tracks = topSelection();
		if (tracks.empty())
			return;
		// Whether they hold anything, or tracks inside them do (a folder takes the tracks inside it along).
		const std::set<uint64_t> removed(tracks.begin(), tracks.end());
		const auto goes = [this, &removed](const Row& row)
		{
			for (uint64_t track = row.id; track != 0; track = m_tracks[m_index.at(track)].parent)
				if (removed.count(track) != 0)
					return true;
			return false;
		};
		const bool holds =
			std::any_of(m_tracks.begin(), m_tracks.end(), [&goes](const Row& row) { return row.holds && goes(row); });
		if (!holds || !m_warnBeforeRemoving)
		{
			removeTracks(tracks);
			return;
		}
		if (m_removeDialog)
			return; // asking already
		// From the folder of the list's document, modal (reference manual p. 213).
		const std::string path = std::filesystem::u8path(m_root->GetOwnerDocument()->GetSourceURL())
									 .replace_filename("remove-tracks.rml")
									 .generic_u8string();
		Rml::ElementDocument* dialog = m_root->GetContext()->LoadDocument(path);
		if (dialog == nullptr)
		{
			logging::warning("The track list can't open the dialog {}", path);
			return;
		}
		m_removing = tracks;
		m_removeDialog = dialog->GetObserverPtr();
		dialog->AddEventListener(g_dialogCloseEvent, this);
		dialog->Show(Rml::ModalFlag::Modal);
	}

	void TrackList::closedRemoveDialog(Rml::Event& event)
	{
		if (event.GetType() != g_dialogCloseEvent)
			return;
		Rml::Element* dialog = m_removeDialog.get();
		dialog->RemoveEventListener(g_dialogCloseEvent, this);
		m_removeDialog.reset();
		const std::vector<uint64_t> tracks = std::exchange(m_removing, {});
		if (event.GetParameter<Rml::String>("result", "") != "ok" || !alive())
			return;
		if (const Rml::Element* off = dialog->GetElementById("remove-tracks-dont-warn");
			off != nullptr && off->HasAttribute("checked"))
		{
			m_warnBeforeRemoving = false;
			m_binding.send("settings.set", {{"key", "ui.removeTracksWarning"}, {"value", false}});
		}
		// Those that are still there.
		std::vector<uint64_t> still;
		std::copy_if(tracks.begin(), tracks.end(), std::back_inserter(still),
			[this](uint64_t track) { return m_index.count(track) != 0; });
		removeTracks(still);
	}

	void TrackList::removeTracks(const std::vector<uint64_t>& tracks)
	{
		Json commands = Json::array();
		for (const uint64_t track : tracks)
			commands.push_back({{"command", "track.remove"}, {"parameters", {{"track", track}}}});
		transaction("Remove Selected Tracks", std::move(commands));
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
		// The tracks shown, without their automation tracks' lanes.
		std::vector<uint64_t> lanes;
		for (size_t lane = 0; lane < m_display.laneTracks().size(); ++lane)
			if (ownLane(lane))
				lanes.push_back(m_display.laneTracks()[lane]);
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
		// The first event on it too, unless the user setting says Up and Down only select tracks (p. 218).
		if (!m_upDownTracksOnly && !m_display.selection().empty())
			m_display.selectFirstOn(track);
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

	uint64_t TrackList::laneAt(Rml::Element* element) const
	{
		for (; element != nullptr && element != m_view; element = element->GetParentNode())
			if (element->IsClassSet("automation-row"))
				return static_cast<uint64_t>(element->GetAttribute<double>("lane", 0.0));
		return 0;
	}

	std::vector<uint64_t> TrackList::selectedLanes() const
	{
		std::vector<uint64_t> lanes;
		for (const uint64_t lane : m_selectedLanes)
			if (laneOf(lane) != nullptr)
				lanes.push_back(lane);
		return lanes;
	}

	void TrackList::selectLane(Rml::Event& event, uint64_t lane)
	{
		const int button = event.GetParameter<int>("button", 0);
		for (const Rml::Element* part = event.GetTargetElement(); lane != 0 && part != nullptr;
			part = part->GetParentNode())
			if (part->GetTagName() == "button")
				return; // the automation track's buttons
		if (lane == 0)
		{
			if (button == 0)
				m_selectedLanes.clear();
		}
		else if (button == 0 && (held(event, "ctrl_key") || held(event, "meta_key") || held(event, "shift_key")))
		{
			if (m_selectedLanes.erase(lane) == 0)
				m_selectedLanes.insert(lane);
		}
		else if (button == 0 || (button == 1 && m_selectedLanes.count(lane) == 0))
		{
			m_selectedLanes = {lane}; // a right click on one that isn't selected selects it, for the context menu
		}
		showLaneSelection();
	}

	void TrackList::showLaneSelection()
	{
		Rml::ElementList rows;
		m_view->QuerySelectorAll(rows, ".automation-row");
		for (Rml::Element* row : rows)
			setClass(*row, "selected", m_selectedLanes.count(laneAt(row)) != 0);
	}

	void TrackList::mouseDown(Rml::Event& event)
	{
		Rml::Element* target = event.GetTargetElement();
		const Row* row = rowOf(target);
		if (row == nullptr)
			return;
		selectLane(event, laneAt(target));
		// A right click on a track that isn't selected selects it, for the context menu.
		const std::vector<uint64_t> tracks = selection();
		const bool selected = std::find(tracks.begin(), tracks.end(), row->id) != tracks.end();
		if (event.GetParameter<int>("button", 0) == 1 && !selected)
			select({row->id});
		if (event.GetParameter<int>("button", 0) != 0)
			return;
		for (const Rml::Element* part = target; part != nullptr && part->GetTagName() != "listitem";
			part = part->GetParentNode())
			if (part->HasAttribute("control") || part->GetTagName() == "input" || part->IsClassSet("tree-toggle") ||
				part->IsClassSet("automation-rows") || part->IsClassSet("automation-toggle"))
				return; // the control's, or the automation tracks'
		Rml::Element* element = rowElement(row->id);
		const auto y = event.GetParameter<float>("mouse_y", 0.0F);
		m_dragY = y;
		m_dragTrack = row->id;
		// The lower edge of the track's own lane, above its automation tracks.
		const double bottom = ((element->GetAbsoluteTop() - y) / dpRatio()) +
			(m_display.trackHeight(row->id) * m_display.view().trackZoom);
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
			// A track's row with its lanes and automation tracks' lanes (#99, #121).
			const size_t end = rowEnd(lane);
			if (!ownLane(lane) || at >= tops[end])
				continue;
			const Row& row = m_tracks[m_index.at(lanes[lane])];
			const double into = (at - tops[lane]) / (tops[end] - tops[lane]);
			// The middle of a folder's row: into the folder, last (reference manual p. 214).
			if (row.type == "folder" && into > 0.25 && into < 0.75)
				return {row.id, 0};
			if (into < 0.5 || end == lanes.size())
				return into < 0.5 ? std::pair{row.parent, row.id} : std::pair{row.parent, uint64_t{0}};
			// Below the middle: before the next shown track, in its folder, unless that is inside this one.
			const Row& next = m_tracks[m_index.at(lanes[end])];
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
			if ((row.type == "marker" || row.type == "tempo" || row.type == "signature") && parent != 0)
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
