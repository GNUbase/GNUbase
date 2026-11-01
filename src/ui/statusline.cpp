// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/statusline.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/StringUtilities.h>

#include <array>
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
		: m_recordFormat(field(root, "record-format")), m_lostDevice(field(root, "lost-device")),
		  m_changed(binding.on("audio.changed", [this](const engine::Event& event) { show(event.data); }))
	{
		binding.send("audio.get", engine::Json::object(),
			[this, alive = std::weak_ptr<bool>(m_alive)](const engine::Result& result)
			{
				if (!alive.expired() && result.ok())
					show(result.value);
			});
	}

	void StatusLine::show(const engine::Json& status)
	{
		if (m_recordFormat)
		{
			// The reference's "Record: 44.1 kHz"; the bit depth joins it with the project setup (#78).
			std::array<char, 32> rate{};
			std::snprintf(rate.data(), rate.size(), "%g", status.at("projectSampleRate").get<double>() / 1000.0);
			m_recordFormat->SetInnerRML("Record: " + std::string(rate.data()) + " kHz");
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
					Rml::StringUtilities::EncodeRml("Audio Device Lost: " + lost.at("device").get<std::string>()));
				m_lostDevice->SetAttribute("title", lost.at("reason").get<std::string>());
			}
		}
	}
}
