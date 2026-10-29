// SPDX-License-Identifier: GPL-3.0-or-later
#include "elements.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/ID.h>
#include <RmlUi/Core/Mesh.h>
#include <RmlUi/Core/MeshUtilities.h>
#include <RmlUi/Core/PropertyIdSet.h>
#include <RmlUi/Core/RenderManager.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gnubase::spike
{
	namespace
	{
		Rml::ColourbPremultiplied colour(const Rml::Element& element)
		{
			const Rml::ComputedValues& values = element.GetComputedValues();
			return values.color().ToPremultiplied(values.opacity());
		}
	}

	float amplitude(float position)
	{
		// Phrases of notes: an envelope that decays after each attack, with some noise on top.
		const float note = std::fmod(position, 180.0F) / 180.0F;
		const float envelope = std::exp(-4.0F * note) * (0.6F + (0.4F * std::sin(position * 0.013F)));
		auto hash = static_cast<uint32_t>(position) * 2654435761U;
		hash ^= hash >> 15U;
		const float noise = static_cast<float>(hash & 0xffffU) / 65535.0F;
		return std::clamp(envelope * (0.55F + (0.45F * noise)), 0.0F, 1.0F);
	}

	void Waveform::setPhase(float phase)
	{
		m_phase = phase;
		m_dirty = true;
	}

	void Waveform::OnResize()
	{
		Element::OnResize();
		m_dirty = true;
	}

	void Waveform::OnPropertyChange(const Rml::PropertyIdSet& changed)
	{
		Element::OnPropertyChange(changed);
		if (changed.Contains(Rml::PropertyId::Color) || changed.Contains(Rml::PropertyId::Opacity))
			m_dirty = true;
	}

	void Waveform::OnRender()
	{
		if (m_dirty)
		{
			const Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
			const float middle = size.y / 2.0F;
			const Rml::ColourbPremultiplied fill = colour(*this);
			const float dp = GetContext()->GetDensityIndependentPixelRatio(); // the signal is laid out in dp
			Rml::Mesh mesh;
			const int columns = static_cast<int>(size.x);
			mesh.vertices.reserve(static_cast<size_t>(columns) * 4);
			mesh.indices.reserve(static_cast<size_t>(columns) * 6);
			for (int column = 0; column < columns; ++column)
			{
				const float height = std::max(1.0F, amplitude((static_cast<float>(column) / dp) + m_phase) * size.y);
				Rml::MeshUtilities::GenerateQuad(
					mesh, {static_cast<float>(column), middle - (height / 2.0F)}, {1.0F, height}, fill);
			}
			m_geometry = GetRenderManager()->MakeGeometry(std::move(mesh));
			m_dirty = false;
		}
		m_geometry.Render(GetAbsoluteOffset(Rml::BoxArea::Content));
	}

	void WaveformImage::OnResize()
	{
		Element::OnResize();
		m_dirty = true;
	}

	void WaveformImage::OnRender()
	{
		if (m_dirty)
		{
			const Rml::Vector2f size = GetBox().GetSize(Rml::BoxArea::Content);
			const Rml::Vector2i pixels(std::max(1, static_cast<int>(size.x)), std::max(1, static_cast<int>(size.y)));
			const Rml::ColourbPremultiplied fill = colour(*this);
			const float dp = GetContext()->GetDensityIndependentPixelRatio();
			m_texture = GetRenderManager()->MakeCallbackTexture(
				[pixels, fill, dp](const Rml::CallbackTextureInterface& texture)
				{
					std::vector<Rml::byte> rgba(static_cast<size_t>(pixels.x) * static_cast<size_t>(pixels.y) * 4, 0);
					const float middle = static_cast<float>(pixels.y) / 2.0F;
					for (int column = 0; column < pixels.x; ++column)
					{
						const float half = amplitude(static_cast<float>(column) / dp) * middle;
						const int top = static_cast<int>(middle - half);
						const int bottom = std::min(pixels.y - 1, static_cast<int>(middle + half));
						for (int row = top; row <= bottom; ++row)
						{
							const size_t index = ((static_cast<size_t>(row) * static_cast<size_t>(pixels.x)) +
													 static_cast<size_t>(column)) *
								4;
							rgba[index] = fill.red;
							rgba[index + 1] = fill.green;
							rgba[index + 2] = fill.blue;
							rgba[index + 3] = fill.alpha;
						}
					}
					return texture.GenerateTexture(rgba, pixels);
				});
			Rml::Mesh mesh;
			Rml::MeshUtilities::GenerateQuad(
				mesh, {0.0F, 0.0F}, size, Rml::ColourbPremultiplied(255, 255, 255, 255), {0.0F, 0.0F}, {1.0F, 1.0F});
			m_geometry = GetRenderManager()->MakeGeometry(std::move(mesh));
			m_dirty = false;
		}
		m_geometry.Render(GetAbsoluteOffset(Rml::BoxArea::Content), m_texture);
	}

	void registerElements()
	{
		// RmlUi keeps pointers to the instancers until it shuts down.
		static Rml::ElementInstancerGeneric<Waveform> waveform;
		static Rml::ElementInstancerGeneric<WaveformImage> waveformImage;
		Rml::Factory::RegisterElementInstancer("waveform", &waveform);
		Rml::Factory::RegisterElementInstancer("waveform-image", &waveformImage);
	}
}
