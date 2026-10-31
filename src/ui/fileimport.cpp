// SPDX-License-Identifier: GPL-3.0-or-later
// Importing audio and MIDI files into the event display (#92, ui/eventdisplay.h, docs/features/file-import.md): the
// Import Options dialog for audio files and files dropped on the display. The imports themselves are the engine's
// audioFile.import and midiFile.import.
#include "ui/eventdisplay.h"

#include <RmlUi/Core/Element.h>
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

	void EventDisplay::importFiles(
		const std::vector<std::string>& files, std::optional<uint64_t> track, std::optional<int64_t> sample)
	{
		std::vector<std::string> audio;
		for (const std::string& file : files)
		{
			if (isMidiFile(file))
				m_binding.send("midiFile.import", {{"path", file}});
			else
				audio.push_back(file);
		}
		if (audio.empty() || !alive() || m_dialog)
			return;
		m_import = {std::move(audio), track, std::max<int64_t>(sample.value_or(m_cursorSample), 0)};
		// Copying, converting and splitting write into the project folder, which a project has once it is saved.
		m_binding.send("project.location", Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (!alive.expired() && result.ok())
					openImportOptions(!result.value.at("file").is_null());
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
		std::optional<uint64_t> track;
		const auto lane = static_cast<size_t>((y + m_view.top) / (g_trackHeight * m_view.trackZoom));
		if (lane < m_laneTypes.size() && m_laneTypes[lane] == "audio")
			track = m_laneTracks[lane];
		importFiles(files, track, samples(m_view.start + (x * m_view.scale)));
	}

	void EventDisplay::openImportOptions(bool saved)
	{
		openDialog("import-options.rml");
		if (!m_dialog)
			return;
		setOption(m_dialog->GetElementById("import-copy"), saved && m_importCopy, !saved);
		setOption(m_dialog->GetElementById("import-convert"), saved && m_importConvert, !saved);
		setOption(m_dialog->GetElementById("import-split"), saved && m_importSplit, !saved);
		setOption(m_dialog->GetElementById("import-one-track"), m_importOneTrack,
			m_import.files.size() < 2 || m_import.track.has_value());
	}

	void EventDisplay::importAudio(Rml::Element& dialog)
	{
		// The options that could be chosen are remembered for the next import.
		const auto option = [&dialog](const char* id, bool& remembered)
		{
			const Rml::Element* element = dialog.GetElementById(id);
			if (element == nullptr || element->HasAttribute("disabled"))
				return false;
			remembered = element->HasAttribute("checked");
			return remembered;
		};
		const bool copy = option("import-copy", m_importCopy);
		const bool convert = option("import-convert", m_importConvert);
		const bool split = option("import-split", m_importSplit);
		const bool oneTrack = option("import-one-track", m_importOneTrack);
		Json parameters = {{"files", m_import.files}, {"position", m_import.sample}, {"copy", copy},
			{"convert", convert}, {"splitChannels", split}};
		if (!split && m_import.track)
			parameters["track"] = *m_import.track;
		else if (!split && oneTrack)
			parameters["oneTrack"] = true;
		m_binding.send("audioFile.import", parameters);
	}
}
