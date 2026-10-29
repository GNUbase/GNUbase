// SPDX-License-Identifier: GPL-3.0-or-later
#include "testutils/fakeui.h"

#include <RmlUi/Core/Context.h>

#include <cmath>
#include <utility>

namespace gnubase::test
{
	FakeWindowSystem::FakeWindowSystem(std::vector<ui::Display> displays) : m_displays(std::move(displays))
	{
	}

	void FakeWindowSystem::setDisplays(std::vector<ui::Display> displays)
	{
		m_displays = std::move(displays);
		m_events.emplace_back(ui::WindowEventKind::DisplaysChanged);
		for (auto& [id, window] : m_windows)
			move(id, window.bounds); // a monitor's scale may have changed under the window
	}

	void FakeWindowSystem::drag(ui::WindowId window, const ui::Rect& bounds)
	{
		move(window, bounds);
	}

	void FakeWindowSystem::push(const ui::WindowEvent& event)
	{
		m_events.push_back(event);
	}

	Rml::Context* FakeWindowSystem::inputTarget(ui::WindowId window) const
	{
		const auto it = m_windows.find(window);
		return it == m_windows.end() ? nullptr : it->second.input;
	}

	ui::WindowId FakeWindowSystem::createWindow(const ui::WindowSpec& spec)
	{
		const ui::WindowId id = m_nextId++;
		FakeWindow& window = m_windows[id];
		window.spec = spec;
		window.bounds = spec.bounds.value_or(ui::Rect{0, 0, spec.size.width, spec.size.height});
		if (const ui::Display* display = ui::displayOf(window.bounds, m_displays))
			window.scale = display->scale;
		return id;
	}

	void FakeWindowSystem::destroyWindow(ui::WindowId window)
	{
		m_windows.erase(window);
	}

	void FakeWindowSystem::setBounds(ui::WindowId window, const ui::Rect& bounds)
	{
		move(window, bounds);
	}

	void FakeWindowSystem::move(ui::WindowId window, const ui::Rect& bounds)
	{
		FakeWindow& fake = m_windows.at(window);
		const ui::Size pixels = pixelSize(window);
		const float scale = fake.scale;
		const bool moved = !(bounds == fake.bounds);
		fake.bounds = bounds;
		if (const ui::Display* display = ui::displayOf(bounds, m_displays))
			fake.scale = display->scale;
		if (moved)
			m_events.emplace_back(ui::WindowEventKind::Moved, window);
		if (fake.scale != scale)
			m_events.emplace_back(ui::WindowEventKind::ScaleChanged, window);
		if (!(pixelSize(window) == pixels))
			m_events.emplace_back(ui::WindowEventKind::Resized, window);
	}

	ui::Size FakeWindowSystem::pixelSize(ui::WindowId window) const
	{
		const FakeWindow& fake = m_windows.at(window);
		return {static_cast<int>(std::lround(static_cast<float>(fake.bounds.width) * fake.scale)),
			static_cast<int>(std::lround(static_cast<float>(fake.bounds.height) * fake.scale))};
	}

	void FakeWindowSystem::setInputTarget(ui::WindowId window, Rml::Context* context)
	{
		m_windows.at(window).input = context;
	}

	Rml::TextInputHandler* FakeWindowSystem::textInputHandler(ui::WindowId window)
	{
		return m_windows.at(window).textInput.get();
	}

	bool FakeWindowSystem::textInputActive(ui::WindowId window) const
	{
		return m_windows.at(window).textInput->active != nullptr;
	}

	void FakeWindowSystem::TextInput::OnDeactivate(Rml::TextInputContext* input)
	{
		if (input == active)
			active = nullptr;
	}

	void FakeWindowSystem::answerFileDialog(uint64_t id, std::vector<std::filesystem::path> files, std::string error)
	{
		m_dialogs.erase(id);
		ui::WindowEvent event{ui::WindowEventKind::FileDialogClosed};
		event.dialog = id;
		event.files = std::move(files);
		event.error = std::move(error);
		m_events.push_back(std::move(event));
	}

	std::optional<ui::WindowEvent> FakeWindowSystem::nextEvent(std::optional<std::chrono::milliseconds> /*timeout*/)
	{
		if (m_events.empty())
			return std::nullopt; // nothing else will happen, so don't wait
		ui::WindowEvent event = std::move(m_events.front());
		m_events.pop_front();
		return event;
	}

	bool NullRenderer::present(ui::WindowId window, Rml::Context& context)
	{
		context.Render();
		++m_presents[window];
		return attached(window);
	}

	std::optional<ui::Image> NullRenderer::capture(ui::WindowId window, Rml::Context& context)
	{
		if (!attached(window))
			return std::nullopt;
		context.Render();
		const ui::Size size = m_windows.pixelSize(window);
		return ui::Image{size.width, size.height,
			std::vector<uint8_t>(static_cast<size_t>(size.width) * static_cast<size_t>(size.height) * 4)};
	}

	int NullRenderer::presents(ui::WindowId window) const
	{
		const auto it = m_presents.find(window);
		return it == m_presents.end() ? 0 : it->second;
	}

	Rml::CompiledGeometryHandle NullRenderer::CompileGeometry(
		Rml::Span<const Rml::Vertex> /*vertices*/, Rml::Span<const int> /*indices*/)
	{
		return m_nextGeometry++;
	}
}
