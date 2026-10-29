// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <RmlUi/Core/CallbackTexture.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Geometry.h>

// Custom elements that draw the way GNUbase's editors will: waveforms as geometry built from data, and as a texture.
// Their colour comes from the RCSS color property.
namespace gnubase::spike
{
	// <waveform>: a synthetic waveform, one quad per pixel column, rebuilt when it scrolls (the phase changes).
	class Waveform : public Rml::Element
	{
	public:
		explicit Waveform(const Rml::String& tag) : Rml::Element(tag) {}

		// Scrolls the waveform to another part of the signal.
		void setPhase(float phase);

	protected:
		void OnRender() override;
		void OnResize() override;
		void OnPropertyChange(const Rml::PropertyIdSet& changed) override;

	private:
		Rml::Geometry m_geometry;
		float m_phase = 0.0F;
		bool m_dirty = true;
	};

	// <waveform-image>: the same waveform drawn into a texture once, then shown as one textured quad.
	class WaveformImage : public Rml::Element
	{
	public:
		explicit WaveformImage(const Rml::String& tag) : Rml::Element(tag) {}

	protected:
		void OnRender() override;
		void OnResize() override;

	private:
		Rml::CallbackTexture m_texture;
		Rml::Geometry m_geometry;
		bool m_dirty = true;
	};

	// Registers the elements with RmlUi; call after Rml::Initialise.
	void registerElements();

	// The waveform's amplitude, 0 to 1, at a position in the signal.
	float amplitude(float position);
}
