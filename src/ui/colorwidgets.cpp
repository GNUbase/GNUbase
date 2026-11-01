// SPDX-License-Identifier: GPL-3.0-or-later
// The color picker (ui/widgets.h).
#include "ui/widgets.h"

#include "ui/rmlui.h"
#include "ui/widgetutils.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Factory.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace gnubase::ui
{
	namespace
	{
		using widgets::appendPart;
		using widgets::format;
		using widgets::number;

		// A color as hue (0 to 360), saturation and value (0 to 1).
		struct Hsv
		{
			float h = 0.0F;
			float s = 0.0F;
			float v = 0.0F;
		};

		using Rgb = std::array<int, 3>; // 0 to 255 each

		Rgb toRgb(const Hsv& color)
		{
			const float h = std::fmod(color.h, 360.0F) / 60.0F;
			const float c = color.v * color.s;
			const float x = c * (1.0F - std::fabs(std::fmod(h, 2.0F) - 1.0F));
			const float m = color.v - c;
			std::array<float, 3> rgb{};
			switch (static_cast<int>(h))
			{
			case 0:
				rgb = {c, x, 0.0F};
				break;
			case 1:
				rgb = {x, c, 0.0F};
				break;
			case 2:
				rgb = {0.0F, c, x};
				break;
			case 3:
				rgb = {0.0F, x, c};
				break;
			case 4:
				rgb = {x, 0.0F, c};
				break;
			default:
				rgb = {c, 0.0F, x};
				break;
			}
			Rgb result{};
			for (size_t i = 0; i < 3; ++i)
				result[i] = static_cast<int>(std::lround((rgb[i] + m) * 255.0F));
			return result;
		}

		Hsv toHsv(const Rgb& color)
		{
			const float r = static_cast<float>(color[0]) / 255.0F;
			const float g = static_cast<float>(color[1]) / 255.0F;
			const float b = static_cast<float>(color[2]) / 255.0F;
			const float most = std::max({r, g, b});
			const float range = most - std::min({r, g, b});
			float h = 0.0F;
			if (range > 0.0F)
			{
				if (most == r)
					h = 60.0F * std::fmod(((g - b) / range) + 6.0F, 6.0F);
				else if (most == g)
					h = 60.0F * (((b - r) / range) + 2.0F);
				else
					h = 60.0F * (((r - g) / range) + 4.0F);
			}
			return {h, most > 0.0F ? range / most : 0.0F, most};
		}

		std::string hex(const Rgb& color)
		{
			std::array<char, 8> text{};
			std::snprintf(text.data(), text.size(), "#%02x%02x%02x", color[0], color[1], color[2]);
			return text.data();
		}

		// "#rrggbb", or nothing if that's not what the text is.
		std::optional<Rgb> parseHex(const std::string& text)
		{
			if (text.size() != 7 || text[0] != '#' ||
				text.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos)
				return std::nullopt;
			const auto channel = [&text](size_t at) { return std::stoi(text.substr(at, 2), nullptr, 16); };
			return Rgb{channel(1), channel(3), channel(5)};
		}

		// <colorpicker>: picks a color, its value attribute "#rrggbb" (reference manual pp. 118-119). The color field
		// picks saturation (left to right) and value (bottom to top) for the hue that the hue strip picks; both follow
		// the mouse from a click and through a drag. Value fields edit hue, saturation and value, and red, green and
		// blue, as numbers. Two swatches show the color the picker started with and the new one. Each change sets the
		// value attribute and sends a change event with it, so data-value binds the picker; the value fields inside
		// send their own change events, with numbers.
		class ColorPicker final : public Rml::Element
		{
		public:
			explicit ColorPicker(const Rml::String& tag) : Rml::Element(tag)
			{
				m_field = appendPart(*this, "color-field");
				m_fieldThumb = appendPart(*m_field, "color-field-thumb");
				m_hue = appendPart(*this, "color-hue");
				m_hueThumb = appendPart(*m_hue, "color-hue-thumb");
				Rml::Element* swatches = appendPart(*this, "color-swatches");
				m_old = appendPart(*swatches, "color-old");
				m_new = appendPart(*swatches, "color-new");
				Rml::Element* values = appendPart(*this, "color-values");
				const std::array<std::pair<const char*, int>, 6> fields = {
					{{"H", 360}, {"S", 100}, {"V", 100}, {"R", 255}, {"G", 255}, {"B", 255}}};
				for (size_t i = 0; i < fields.size(); ++i)
				{
					Rml::Element* row = appendPart(*values, "color-value");
					appendPart(*row, "color-value-label")->SetInnerRML(fields[i].first);
					Rml::XMLAttributes attributes;
					attributes["max"] = fields[i].second;
					attributes["step"] = 1;
					attributes["decimals"] = 0;
					m_fields[i] =
						row->AppendChild(Rml::Factory::InstanceElement(row, "valuefield", "valuefield", attributes));
				}
				for (const Rml::EventId id : {Rml::EventId::Drag, Rml::EventId::Change})
					AddEventListener(id, &m_listener);
				show();
			}

			~ColorPicker() override
			{
				for (const Rml::EventId id : {Rml::EventId::Drag, Rml::EventId::Change})
					RemoveEventListener(id, &m_listener);
			}

			ColorPicker(const ColorPicker&) = delete;
			ColorPicker& operator=(const ColorPicker&) = delete;
			ColorPicker(ColorPicker&&) = delete;
			ColorPicker& operator=(ColorPicker&&) = delete;

			void ProcessDefaultAction(Rml::Event& event) override
			{
				Rml::Element::ProcessDefaultAction(event);
				if (event == Rml::EventId::Mousedown && event.GetParameter("button", 0) == 0)
					pick(event);
			}

		protected:
			void OnAttributeChange(const Rml::ElementAttributes& changed) override
			{
				Rml::Element::OnAttributeChange(changed);
				if (changed.count("value") == 0)
					return;
				const std::optional<Rgb> rgb = parseHex(GetAttribute<Rml::String>("value", ""));
				if (!rgb || hex(*rgb) == hex(toRgb(m_color)))
					return; // keeps the hue of a grey the user picked
				m_color = toHsv(*rgb);
				if (!m_old->HasAttribute("color"))
					m_old->SetAttribute("color", hex(*rgb));
				show();
			}

		private:
			void listen(Rml::Event& event)
			{
				if (event == Rml::EventId::Drag)
				{
					pick(event);
					return;
				}
				// A value field changed.
				size_t index = 0;
				while (index < m_fields.size() && m_fields[index] != event.GetTargetElement())
					++index;
				if (index == m_fields.size())
					return;
				const float value = event.GetParameter("value", 0.0F);
				if (index < 3)
				{
					Hsv color = m_color;
					if (index == 0)
						color.h = value;
					else if (index == 1)
						color.s = value / 100.0F;
					else
						color.v = value / 100.0F;
					change(color);
				}
				else
				{
					Rgb rgb = toRgb(m_color);
					rgb[index - 3] = static_cast<int>(std::lround(value));
					Hsv color = toHsv(rgb);
					if (color.s == 0.0F || color.v == 0.0F)
						color.h = m_color.h; // a grey keeps the hue it had
					change(color);
				}
			}

			// The mouse is on the color field or the hue strip: that part picks.
			void pick(const Rml::Event& event)
			{
				for (Rml::Element* part : {m_field, m_hue})
				{
					bool inside = false;
					for (const Rml::Element* element = event.GetTargetElement(); element != nullptr && !inside;
						element = element->GetParentNode())
						inside = element == part;
					if (!inside)
						continue;
					const Rml::Vector2f origin = part->GetAbsoluteOffset(Rml::BoxArea::Border);
					const Rml::Vector2f size = part->GetBox().GetSize(Rml::BoxArea::Border);
					const float x = std::clamp((event.GetParameter("mouse_x", 0.0F) - origin.x) / size.x, 0.0F, 1.0F);
					const float y = std::clamp((event.GetParameter("mouse_y", 0.0F) - origin.y) / size.y, 0.0F, 1.0F);
					Hsv color = m_color;
					if (part == m_field)
					{
						color.s = x;
						color.v = 1.0F - y;
					}
					else
					{
						color.h = y * 360.0F;
					}
					change(color);
				}
			}

			void change(const Hsv& color)
			{
				m_color = {std::clamp(color.h, 0.0F, 360.0F), std::clamp(color.s, 0.0F, 1.0F),
					std::clamp(color.v, 0.0F, 1.0F)};
				const std::string value = hex(toRgb(m_color));
				SetAttribute("value", value);
				show();
				Rml::Dictionary parameters;
				parameters["value"] = value;
				DispatchEvent(Rml::EventId::Change, parameters);
			}

			// Shows the color: the thumbs where it is, the field in its hue, the swatches and the numbers.
			void show()
			{
				const Rgb rgb = toRgb(m_color);
				m_fieldThumb->SetProperty("left", format(m_color.s * 100.0F) + "%");
				m_fieldThumb->SetProperty("top", format((1.0F - m_color.v) * 100.0F) + "%");
				m_hueThumb->SetProperty("top", format(m_color.h / 3.6F) + "%");
				m_field->SetProperty("background-color", hex(toRgb({m_color.h, 1.0F, 1.0F})));
				m_new->SetProperty("background-color", hex(rgb));
				m_old->SetProperty("background-color", m_old->GetAttribute<Rml::String>("color", hex(rgb)));
				const std::array<float, 6> numbers = {m_color.h, m_color.s * 100.0F, m_color.v * 100.0F,
					static_cast<float>(rgb[0]), static_cast<float>(rgb[1]), static_cast<float>(rgb[2])};
				for (size_t i = 0; i < numbers.size(); ++i)
					if (std::fabs(number(*m_fields[i], "value", -1.0F) - numbers[i]) >= 0.5F)
						m_fields[i]->SetAttribute("value", format(std::round(numbers[i])));
			}

			Hsv m_color;
			Rml::Element* m_field = nullptr;
			Rml::Element* m_fieldThumb = nullptr;
			Rml::Element* m_hue = nullptr;
			Rml::Element* m_hueThumb = nullptr;
			Rml::Element* m_old = nullptr; // the color the picker started with, in its color attribute
			Rml::Element* m_new = nullptr;
			std::array<Rml::Element*, 6> m_fields{};
			widgets::Listener m_listener{[this](Rml::Event& event) { listen(event); }};
		};
	}

	void registerColorWidgets(RmlUi& rmlui)
	{
		rmlui.registerElement<ColorPicker>("colorpicker");
	}
}
