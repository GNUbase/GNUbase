// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/statusline.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/StringUtilities.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace gnubase::ui
{
	namespace
	{
		Rml::ObserverPtr<Rml::Element> field(Rml::Element& root, const char* id)
		{
			Rml::Element* element = root.GetElementById(id);
			return element == nullptr ? Rml::ObserverPtr<Rml::Element>() : element->GetObserverPtr();
		}
	}

	StatusLine::StatusLine(EngineBinding& binding, Rml::Element& root)
		: m_binding(binding), m_recordFormat(field(root, "record-format")), m_lostDevice(field(root, "lost-device")),
		  m_recordTime(field(root, "record-time")),
		  m_changed(binding.on("audio.changed", [this](const engine::Event& event) { show(event.data); }))
	{
		binding.send("audio.get", engine::Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (!alive.expired() && result.ok())
					show(result.value);
			});
		update();
	}

	void StatusLine::update()
	{
		if (!m_recordTime || m_binding.waiting("recording.remainingTime") > 0)
			return;
		m_binding.send("recording.remainingTime", engine::Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (!alive.expired() && result.ok())
					showRecordTime(result.value);
			});
	}

	void StatusLine::showRecordTime(const engine::Json& time)
	{
		if (!m_recordTime)
			return;
		// "52 h 07 min", or under an hour "4 min 09 s"; "-" while no track would record.
		std::string text = "-";
		if (time.at("seconds").is_number())
		{
			const auto seconds = static_cast<int64_t>(time.at("seconds").get<double>());
			const auto twoDigits = [](int64_t value) { return (value < 10 ? "0" : "") + std::to_string(value); };
			text = seconds >= 3600 ? std::to_string(seconds / 3600) + " h " + twoDigits((seconds / 60) % 60) + " min"
								   : std::to_string(seconds / 60) + " min " + twoDigits(seconds % 60) + " s";
		}
		const std::string shown = Rml::StringUtilities::EncodeRml("Max. Record Time: " + text);
		if (m_recordTime->GetInnerRML() != shown)
			m_recordTime->SetInnerRML(shown);
	}

	void StatusLine::show(const engine::Json& status)
	{
		if (m_recordFormat)
		{
			// "Record: 44.1 kHz - 24 bit", or "- 32 bit float"; the engine names bit depths int24 and float32.
			std::array<char, 32> rate{};
			std::snprintf(rate.data(), rate.size(), "%g", status.at("projectSampleRate").get<double>() / 1000.0);
			const std::string depth = status.at("recordBitDepth").get<std::string>();
			const bool floating = depth.rfind("float", 0) == 0;
			m_recordFormat->SetInnerRML(Rml::StringUtilities::EncodeRml("Record: " + std::string(rate.data()) +
				" kHz - " + depth.substr(floating ? 5 : 3) + (floating ? " bit float" : " bit")));
			const engine::Json& device = status.at("device");
			m_recordFormat->SetClass("mismatch", !device.is_null() && device.value("sampleRateMismatch", false));
		}
		if (m_lostDevice)
		{
			const engine::Json& lost = status.at("lost");
			m_lostDevice->SetClass("hidden", lost.is_null());
			if (!lost.is_null())
			{
				m_lostDevice->SetInnerRML(
					Rml::StringUtilities::EncodeRml("Audio Device Lost: " + lost.at("name").get<std::string>()));
				// The id (WASAPI GUIDs, a CoreAudio UID) means little to users, but tells devices of one name apart. A
				// device without a name is shown by its id already.
				std::string title = lost.at("reason").get<std::string>();
				if (lost.at("name") != lost.at("device"))
					title += " (" + lost.at("device").get<std::string>() + ")";
				m_lostDevice->SetAttribute("title", title);
			}
		}
	}
}
