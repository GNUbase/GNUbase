// SPDX-License-Identifier: GPL-3.0-or-later
// Importing audio and MIDI files into the event display (#92, ui/eventdisplay.h, docs/features/file-import.md): the
// Import Options dialog for audio files and files dropped on the display. The imports themselves are the engine's
// audioFile.import and midiFile.import.
#include "ui/eventdisplay.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>
#include <RmlUi/Core/Event.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::ui
{
	namespace
	{
		using engine::Json;

		bool isMidiFile(const std::string& file)
		{
			std::string extension = std::filesystem::u8path(file).extension().u8string();
			std::transform(extension.begin(), extension.end(), extension.begin(),
				[](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
			return extension == ".mid" || extension == ".midi" || extension == ".smf";
		}

		// The checked attribute gives a widget the state class .checked, disabled the pseudo-class :disabled.
		void setOption(Rml::Element* option, bool checked, bool disabled)
		{
			if (option == nullptr)
				return;
			if (checked)
				option->SetAttribute("checked", "");
			else
				option->RemoveAttribute("checked");
			if (disabled)
				option->SetAttribute("disabled", "");
			else
				option->RemoveAttribute("disabled");
		}
	}

	void EventDisplay::importFiles(const std::vector<std::string>& files, std::optional<uint64_t> track,
		std::optional<int64_t> sample, bool clipsOnly)
	{
		Import import;
		import.track = track;
		import.sample = std::max<int64_t>(sample.value_or(m_cursorSample), 0);
		import.clipsOnly = clipsOnly;
		startImport(files, std::move(import));
	}

	void EventDisplay::importToSelection(const std::vector<std::string>& files, const std::vector<uint64_t>& selection)
	{
		Import import;
		import.sample = std::max<int64_t>(m_cursorSample, 0);
		// The first selected track (reference manual p. 746), if it is an audio track, also inside a folded folder;
		// otherwise new tracks, as without a selected track.
		if (!selection.empty() && m_project.is_object())
		{
			for (const Json& track : m_project.at("tracks"))
			{
				if (track.at("id") == selection.front() && track.at("type") == "audio")
				{
					import.track = selection.front();
					import.selected = true;
				}
			}
		}
		startImport(files, std::move(import));
	}

	void EventDisplay::startImport(const std::vector<std::string>& files, Import import)
	{
		std::vector<std::string> audio;
		std::vector<std::string> midi;
		for (const std::string& file : files)
			(isMidiFile(file) && !import.clipsOnly ? midi : audio).push_back(file);
		if (!midi.empty())
			sendMidiImport(midi, import, false);
		if (audio.empty() || !alive())
			return;
		import.files = std::move(audio);
		// The copy, convert and splitChannels options write into the project folder, which a project has once it is
		// saved. The settings say whether the dialog opens, and with which options.
		const std::weak_ptr<bool> alive = m_alive;
		m_binding.send("project.location", Json::object(),
			[this, alive, import = std::move(import)](const engine::Result& location) mutable
			{
				if (alive.expired() || !location.ok())
					return;
				const bool saved = !location.value.at("file").is_null();
				m_binding.send("settings.list", Json::object(),
					[this, alive, saved, import = std::move(import)](const engine::Result& list)
					{
						if (alive.expired() || !list.ok())
							return;
						const ImportOptions options = importSettings(list.value);
						if (usesSettings(list.value))
							sendImport(import, options, saved);
						else
							openImportOptions(import, options, saved);
					});
			});
	}

	void EventDisplay::dropFiles(Rml::Event& event)
	{
		std::vector<std::string> files;
		std::istringstream lines(event.GetParameter<Rml::String>("files", ""));
		for (std::string line; std::getline(lines, line);)
			if (!line.empty())
				files.push_back(line);
		if (files.empty())
			return;
		// Over the event lanes: the lane's track if it is an audio track, at the time under the mouse.
		const auto mouseX = event.GetParameter<float>("mouse_x", 0.0F);
		const auto mouseY = event.GetParameter<float>("mouse_y", 0.0F);
		const double x = dpFrom(*m_lanes, mouseX);
		const double y = (mouseY - m_lanes->GetAbsoluteTop()) / dpRatio();
		if (x < 0.0 || y < 0.0 || x > width() || y > height())
		{
			importFiles(files);
			return;
		}
		Import import;
		import.sample = std::max<int64_t>(samples(m_view.start + (x * m_view.scale)), 0);
		import.dropped = true;
		// On a lane of a track whose lanes show, on that lane (#99).
		if (const size_t lane = laneAt(y + m_view.top); lane < m_laneTypes.size() && m_laneTypes[lane] == "audio")
		{
			import.track = m_laneTracks[lane];
			import.lane = std::max(m_laneTakes[lane], 0);
		}
		else if (lane < m_laneTypes.size() && (m_laneTypes[lane] == "midi" || m_laneTypes[lane] == "instrument"))
		{
			import.midiTrack = m_laneTracks[lane];
		}
		startImport(files, std::move(import));
	}

	void EventDisplay::openImportOptions(Import import, const ImportOptions& options, bool saved)
	{
		if (m_dialog)
			return; // another import's dialog is open
		openDialog("import-options.rml");
		if (!m_dialog)
			return;
		m_import = std::move(import);
		m_importOptions = options;
		m_importSaved = saved;
		setOption(m_dialog->GetElementById("import-copy"), saved && options.copy, !saved);
		setOption(m_dialog->GetElementById("import-convert"), saved && options.convert, !saved);
		setOption(m_dialog->GetElementById("import-split"), saved && options.split, !saved);
		setOption(m_dialog->GetElementById("import-one-track"), options.oneTrack, !oneTrackApplies(m_import));
		Rml::Element* format = m_dialog->GetElementById("import-name-format");
		if (auto* select = dynamic_cast<Rml::ElementFormControlSelect*>(format))
			select->SetValue(options.nameFormat);
		setOption(format, false, !saved);
		setOption(m_dialog->GetElementById("import-do-not-ask"), false, false);
	}

	void EventDisplay::importAudio(Rml::Element& dialog)
	{
		// The options that could be chosen; the others stay as they were.
		ImportOptions options = m_importOptions;
		const auto option = [&dialog](const char* id, bool& value)
		{
			const Rml::Element* element = dialog.GetElementById(id);
			if (element != nullptr && !element->HasAttribute("disabled"))
				value = element->HasAttribute("checked");
		};
		option("import-copy", options.copy);
		option("import-convert", options.convert);
		option("import-split", options.split);
		option("import-one-track", options.oneTrack);
		if (const auto* select =
				dynamic_cast<Rml::ElementFormControlSelect*>(dialog.GetElementById("import-name-format"));
			select != nullptr && !select->HasAttribute("disabled"))
			options.nameFormat = select->GetValue();
		bool doNotAsk = false;
		option("import-do-not-ask", doNotAsk);

		// The settings keep the choices for the next import, and Do not ask again imports with them from then on.
		const auto store = [this](const char* key, const Json& value, const Json& before)
		{
			if (value != before)
				m_binding.send("settings.set", {{"key", key}, {"value", value}});
		};
		store("editing.importAudioCopy", options.copy, m_importOptions.copy);
		store("editing.importAudioConvert", options.convert, m_importOptions.convert);
		store("editing.importAudioSplitChannels", options.split, m_importOptions.split);
		store("editing.importAudioOneTrack", options.oneTrack, m_importOptions.oneTrack);
		store("editing.importAudioNameFormat", options.nameFormat, m_importOptions.nameFormat);
		if (doNotAsk)
			m_binding.send("settings.set", {{"key", "editing.onImportAudio"}, {"value", "useSettings"}});
		sendImport(m_import, options, m_importSaved);
	}

	bool EventDisplay::oneTrackApplies(const Import& import)
	{
		const bool droppedOnTrack = import.track && !import.selected;
		return import.files.size() > 1 && !droppedOnTrack && !import.clipsOnly;
	}

	EventDisplay::ImportOptions EventDisplay::importSettings(const Json& list)
	{
		ImportOptions options;
		for (const Json& setting : list.at("settings"))
		{
			const std::string key = setting.at("key").get<std::string>();
			const Json& value = setting.at("value");
			if (key == "editing.importAudioCopy")
				options.copy = value.get<bool>();
			else if (key == "editing.importAudioConvert")
				options.convert = value.get<bool>();
			else if (key == "editing.importAudioSplitChannels")
				options.split = value.get<bool>();
			else if (key == "editing.importAudioOneTrack")
				options.oneTrack = value.get<bool>();
			else if (key == "editing.importAudioNameFormat")
				options.nameFormat = value.get<std::string>();
		}
		return options;
	}

	bool EventDisplay::usesSettings(const Json& list)
	{
		for (const Json& setting : list.at("settings"))
			if (setting.at("key") == "editing.onImportAudio")
				return setting.at("value") == "useSettings";
		return false;
	}

	void EventDisplay::sendImport(const Import& import, const ImportOptions& options, bool saved)
	{
		// Copy, Convert and Split Channels need the project's folder; Import Files on One Track needs several files
		// that may go on one track.
		const bool split = saved && options.split;
		Json parameters = {{"files", import.files}, {"position", import.sample}, {"copy", saved && options.copy},
			{"convert", saved && options.convert}, {"splitChannels", split}};
		if (split)
			parameters["nameFormat"] = options.nameFormat;
		if (import.clipsOnly)
		{
			parameters["clipsOnly"] = true;
		}
		else
		{
			// Files dropped on a track all go on it; the selected track takes the first file, or all of them with
			// Import Files on One Track. Files that are split go on new tracks either way.
			if (import.track)
			{
				parameters["track"] = *import.track;
				parameters["lane"] = import.lane;
			}
			if ((options.oneTrack && oneTrackApplies(import)) || (import.track && !import.selected))
				parameters["oneTrack"] = true;
		}
		m_binding.send("audioFile.import", parameters);
	}

	void EventDisplay::importMidi(const std::vector<std::string>& files, bool newProject)
	{
		sendMidiImport(files, Import(), newProject);
	}

	void EventDisplay::sendMidiImport(const std::vector<std::string>& files, const Import& import, bool newProject)
	{
		const std::weak_ptr<bool> alive = m_alive;
		m_binding.send("settings.list", Json::object(),
			[this, alive, files, import, newProject](const engine::Result& list)
			{
				if (alive.expired() || !list.ok())
					return;
				Json settings = Json::object();
				for (const Json& setting : list.value.at("settings"))
					settings[setting.at("key").get<std::string>()] = setting.at("value");
				// The file's tempo replaces the project's unless it is merged into the open project with Ignore Master
				// Track Events on Merge.
				Json parameters = {{"trackType", settings.at("midi.importDestination")},
					{"tempo", newProject || !settings.at("midi.importIgnoreTempo").get<bool>()},
					{"markers", settings.at("midi.importMarkers")},
					{"dissolve", settings.at("midi.importDissolveFormat0")},
					{"extractPatch", settings.at("midi.importExtractPatch")},
					{"extractVolumePan", settings.at("midi.importExtractVolumePan")},
					{"automation", settings.at("midi.importAutomation")}};
				if (import.dropped && settings.at("midi.importSinglePart").get<bool>())
				{
					parameters["singlePart"] = true;
					if (import.midiTrack)
						parameters["track"] = *import.midiTrack;
				}
				const auto send = [this, files, parameters](int64_t position)
				{
					for (const std::string& file : files)
					{
						Json fileParameters = parameters;
						fileParameters["position"] = position;
						fileParameters["path"] = file;
						m_binding.send("midiFile.import", fileParameters);
					}
				};
				if (import.dropped || !settings.at("midi.importToLeftLocator").get<bool>())
				{
					send(import.dropped ? import.sample : 0);
					return;
				}
				m_binding.send("transport.get", Json::object(),
					[alive, send](const engine::Result& transport)
					{
						if (!alive.expired() && transport.ok())
							send(transport.value.at("left").get<int64_t>());
					});
			});
	}
}
