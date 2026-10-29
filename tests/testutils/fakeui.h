// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ui/renderer.h"
#include "ui/windowsystem.h"

#include <RmlUi/Core/RenderInterface.h>

#include <deque>
#include <map>

// A window system and a renderer without an OS or a GPU, for testing the UI shell (ui/shell.h) with any monitors.
namespace gnubase::test
{
	// Monitors and windows in memory. Window coordinates are points, as on macOS: a window's pixel size is its size
	// times the scale of the monitor it is on (the one it overlaps most). Like a real window system, changes made
	// through the WindowSystem interface or by the test queue the events the OS would send.
	class FakeWindowSystem final : public ui::WindowSystem
	{
	public:
		explicit FakeWindowSystem(std::vector<ui::Display> displays);

		// Connects, disconnects or changes monitors; queues DisplaysChanged, and ScaleChanged and Resized for the
		// windows whose monitor scale changed.
		void setDisplays(std::vector<ui::Display> displays);

		// Moves a window as the user would by dragging it.
		void drag(ui::WindowId window, const ui::Rect& bounds);

		// Queues an event, e.g. CloseRequested or Quit.
		void push(ui::WindowEvent event);

		bool exists(ui::WindowId window) const { return m_windows.count(window) != 0; }
		size_t windowCount() const { return m_windows.size(); }
		Rml::Context* inputTarget(ui::WindowId window) const;
		ui::WindowSpec spec(ui::WindowId window) const { return m_windows.at(window).spec; }

		std::vector<ui::Display> displays() const override { return m_displays; }
		ui::WindowId createWindow(const ui::WindowSpec& spec) override;
		void destroyWindow(ui::WindowId window) override;
		ui::Rect bounds(ui::WindowId window) const override { return m_windows.at(window).bounds; }
		void setBounds(ui::WindowId window, const ui::Rect& bounds) override;
		ui::Size pixelSize(ui::WindowId window) const override;
		float scale(ui::WindowId window) const override { return m_windows.at(window).scale; }
		void setInputTarget(ui::WindowId window, Rml::Context* context) override;
		std::optional<ui::WindowEvent> nextEvent(std::optional<std::chrono::milliseconds> timeout) override;

	private:
		struct FakeWindow
		{
			ui::WindowSpec spec;
			ui::Rect bounds;
			float scale = 1.0F; // of the monitor it was last on
			Rml::Context* input = nullptr;
		};

		// Moves the window and queues Moved, and ScaleChanged and Resized if they changed.
		void move(ui::WindowId window, const ui::Rect& bounds);

		std::vector<ui::Display> m_displays;
		std::map<ui::WindowId, FakeWindow> m_windows;
		std::deque<ui::WindowEvent> m_events;
		ui::WindowId m_nextId = 1;
	};

	// Draws nothing; counts what it was asked to do. Captures are blank images at the window's pixel size.
	class NullRenderer final : public ui::Renderer, private Rml::RenderInterface
	{
	public:
		explicit NullRenderer(const ui::WindowSystem& windows) : m_windows(windows) {}

		void attach(ui::WindowId window) override { ++m_attached[window]; }
		void detach(ui::WindowId window) override { m_attached.erase(window); }
		Rml::RenderInterface& renderInterface() override { return *this; }
		bool present(ui::WindowId window, Rml::Context& context) override;
		std::optional<ui::Image> capture(ui::WindowId window, Rml::Context& context) override;

		bool attached(ui::WindowId window) const { return m_attached.count(window) != 0; }
		int presents(ui::WindowId window) const;

	private:
		Rml::CompiledGeometryHandle CompileGeometry(
			Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
		void RenderGeometry(Rml::CompiledGeometryHandle /*geometry*/, Rml::Vector2f /*translation*/,
			Rml::TextureHandle /*texture*/) override
		{
		}
		void ReleaseGeometry(Rml::CompiledGeometryHandle /*geometry*/) override {}
		Rml::TextureHandle LoadTexture(Rml::Vector2i& /*dimensions*/, const Rml::String& /*source*/) override
		{
			return 0;
		}
		Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> /*source*/, Rml::Vector2i /*dimensions*/) override
		{
			return 1;
		}
		void ReleaseTexture(Rml::TextureHandle /*texture*/) override {}
		void EnableScissorRegion(bool /*enable*/) override {}
		void SetScissorRegion(Rml::Rectanglei /*region*/) override {}

		const ui::WindowSystem& m_windows;
		std::map<ui::WindowId, int> m_attached;
		std::map<ui::WindowId, int> m_presents;
		Rml::CompiledGeometryHandle m_nextGeometry = 1;
	};
}
