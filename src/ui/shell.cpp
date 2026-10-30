// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/shell.h"

#include "platform/log.h"
#include "ui/rmlui.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <utility>

namespace gnubase::ui
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		// RmlUi's delay in seconds as a duration; empty if it's infinite, i.e. no update is needed.
		std::optional<Clock::duration> durationOf(double seconds)
		{
			constexpr double longest = 24.0 * 60.0 * 60.0; // a day; longer delays are as good as none
			if (!(seconds <= longest))
				return std::nullopt;
			return std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(std::max(seconds, 0.0)));
		}

		// 80 % of the primary monitor's usable area, for a window opened without a size.
		Size defaultSize(const std::vector<Display>& displays)
		{
			if (displays.empty())
				return {1280, 800};
			const Rect& area = displays.front().usable;
			return {area.width * 4 / 5, area.height * 4 / 5};
		}

		// Tells the element under the drop's position that files were dropped on it.
		void drop(Window& window, const WindowEvent& event)
		{
			Rml::Element* target = window.context().GetElementAtPoint(
				{static_cast<float>(event.position.x), static_cast<float>(event.position.y)});
			if (target == nullptr || event.files.empty())
				return; // no document there
			std::string files;
			for (const std::filesystem::path& file : event.files)
			{
				if (!files.empty())
					files += '\n';
				files += file.u8string();
			}
			target->DispatchEvent(g_fileDropEvent,
				{{"files", Rml::Variant(files)}, {"count", Rml::Variant(static_cast<int>(event.files.size()))},
					{"mouse_x", Rml::Variant(event.position.x)}, {"mouse_y", Rml::Variant(event.position.y)}});
			window.requestRender();
		}
	}

	Shell::Shell(WindowSystem& windows, Renderer& renderer) : m_windows(windows), m_renderer(renderer)
	{
	}

	Shell::~Shell()
	{
		quit();
	}

	Window& Shell::open(const WindowSpec& spec)
	{
		const std::vector<Display> displays = m_windows.displays();
		WindowSpec placed = spec;
		if (spec.bounds)
		{
			placed.bounds = place(*spec.bounds, displays);
		}
		else
		{
			const Size size = spec.size.width > 0 && spec.size.height > 0 ? spec.size : defaultSize(displays);
			placed.bounds = displays.empty() ? Rect{0, 0, size.width, size.height} : centred(size, displays.front());
		}

		auto window = std::make_unique<Window>();
		window->m_id = m_windows.createWindow(placed);
		window->m_main = m_open.empty();
		window->m_contextName = "window-" + std::to_string(window->m_id);
		try
		{
			m_renderer.attach(window->m_id);
			window->m_pixelSize = m_windows.pixelSize(window->m_id);
			window->m_context =
				Rml::CreateContext(window->m_contextName, {window->m_pixelSize.width, window->m_pixelSize.height},
					&m_renderer.renderInterface(), m_windows.textInputHandler(window->m_id));
			if (window->m_context == nullptr)
				throw std::runtime_error("RmlUi cannot create a context for the window");
		}
		catch (...)
		{
			m_renderer.detach(window->m_id);
			m_windows.destroyWindow(window->m_id);
			throw;
		}
		window->m_dpRatio = m_windows.scale(window->m_id) * m_uiScale;
		window->m_context->SetDensityIndependentPixelRatio(window->m_dpRatio);
		if (!m_theme.empty())
			window->m_context->ActivateTheme(m_theme, true);
		m_windows.setInputTarget(window->m_id, window->m_context);
		m_running = true;

		Window& opened = *window;
		m_open.emplace(opened.m_id, std::move(window));
		logging::info("Window {} opened: {}x{} pixels, dp ratio {}", opened.m_id, opened.m_pixelSize.width,
			opened.m_pixelSize.height, opened.m_dpRatio);
		return opened;
	}

	void Shell::close(WindowId id)
	{
		const auto it = m_open.find(id);
		if (it == m_open.end())
			return;
		m_windows.setInputTarget(id, nullptr);
		Rml::RemoveContext(it->second->m_contextName);
		m_renderer.detach(id);
		m_windows.destroyWindow(id);
		m_open.erase(it);
		logging::info("Window {} closed", id);
	}

	Window* Shell::find(WindowId id)
	{
		const auto it = m_open.find(id);
		return it == m_open.end() ? nullptr : it->second.get();
	}

	std::vector<Window*> Shell::windows()
	{
		std::vector<Window*> result;
		result.reserve(m_open.size());
		for (auto& [id, window] : m_open)
			result.push_back(window.get());
		return result;
	}

	void Shell::setUiScale(float scale)
	{
		m_uiScale = scale;
		for (auto& [id, window] : m_open)
			refresh(*window);
	}

	void Shell::setTheme(const std::string& name)
	{
		if (name == m_theme)
			return;
		for (auto& [id, window] : m_open)
		{
			if (!m_theme.empty())
				window->m_context->ActivateTheme(m_theme, false);
			if (!name.empty())
				window->m_context->ActivateTheme(name, true);
			window->m_renderRequested = true;
		}
		m_theme = name;
		logging::info("Theme {}", name.empty() ? "none" : name);
	}

	void Shell::post(std::function<void()> task)
	{
		{
			const std::scoped_lock lock(m_postedMutex);
			m_posted.push_back(std::move(task));
		}
		m_windows.wake();
	}

	void Shell::quit()
	{
		// The main window last, since it was opened first.
		while (!m_open.empty())
			close(std::prev(m_open.end())->first);
		m_running = false;
		// Nothing runs posted work any more: drop it, so that threads waiting for it learn so at once.
		std::vector<std::function<void()>> posted;
		{
			const std::scoped_lock lock(m_postedMutex);
			posted.swap(m_posted);
		}
	}

	void Shell::refresh(Window& window)
	{
		const float ratio = m_windows.scale(window.m_id) * m_uiScale;
		const Size pixels = m_windows.pixelSize(window.m_id);
		if (ratio != window.m_dpRatio)
		{
			window.m_dpRatio = ratio;
			window.m_context->SetDensityIndependentPixelRatio(ratio);
			window.m_renderRequested = true;
		}
		if (!(pixels == window.m_pixelSize))
		{
			window.m_pixelSize = pixels;
			window.m_context->SetDimensions({pixels.width, pixels.height});
			window.m_renderRequested = true;
		}
	}

	void Shell::handle(const WindowEvent& event)
	{
		switch (event.kind)
		{
		case WindowEventKind::Quit:
			quit();
			return;
		case WindowEventKind::Wake:
			return; // step runs the posted work

		case WindowEventKind::FileDialogClosed:
		{
			const auto it = m_dialogs.find(event.dialog);
			if (it == m_dialogs.end())
				return;
			const std::function<void(const FileDialogResult&)> done = std::move(it->second);
			m_dialogs.erase(it);
			if (!event.error.empty())
				logging::warning("File dialog failed: {}", event.error);
			done(FileDialogResult{event.files, event.error});
			return;
		}
		case WindowEventKind::DisplaysChanged:
		{
			const std::vector<Display> displays = m_windows.displays();
			for (auto& [id, window] : m_open)
			{
				const Rect bounds = m_windows.bounds(id);
				const Rect placed = place(bounds, displays);
				if (!(placed == bounds))
				{
					logging::info("Window {} moved onto a connected monitor", id);
					m_windows.setBounds(id, placed);
				}
				refresh(*window);
			}
			return;
		}
		default:
			break;
		}

		Window* window = find(event.window);
		if (window == nullptr)
			return;
		switch (event.kind)
		{
		case WindowEventKind::CloseRequested:
			if (window->m_main)
				quit();
			else
				close(event.window);
			break;
		case WindowEventKind::Moved:
		case WindowEventKind::Resized:
		case WindowEventKind::ScaleChanged:
			refresh(*window);
			break;
		case WindowEventKind::Exposed:
		case WindowEventKind::Input:
			window->m_renderRequested = true;
			break;
		case WindowEventKind::FilesDropped:
			drop(*window, event);
			break;
		default:
			break;
		}
	}

	void Shell::draw(Window& window)
	{
		window.m_context->Update();
		if (!m_renderer.present(window.m_id, *window.m_context))
			logging::warning("Window {} couldn't be drawn", window.m_id);
		++window.m_frames;
		window.m_renderRequested = false;
		const std::optional<Clock::duration> delay = durationOf(window.m_context->GetNextUpdateDelay());
		window.m_nextUpdate = delay ? Clock::now() + *delay : Clock::time_point::max();
	}

	Shell::Clock::time_point Shell::nextUpdate(const Window& window, Clock::time_point now)
	{
		const std::optional<Clock::duration> delay = durationOf(window.m_context->GetNextUpdateDelay());
		return delay ? std::min(window.m_nextUpdate, now + *delay) : window.m_nextUpdate;
	}

	void Shell::step(std::optional<std::chrono::milliseconds> timeout)
	{
		if (m_stepping)
			throw std::logic_error("the UI can't step inside one of its steps, e.g. wait in its own event handlers");
		m_stepping = true;
		const struct Stepped
		{
			bool& stepping;
			~Stepped() { stepping = false; }
		} stepped{m_stepping};

		std::optional<WindowEvent> event = m_windows.nextEvent(timeout);
		while (event && m_running)
		{
			handle(*event);
			event = m_windows.nextEvent(std::chrono::milliseconds(0));
		}

		std::vector<std::function<void()>> posted;
		{
			const std::scoped_lock lock(m_postedMutex);
			posted.swap(m_posted);
		}
		for (const std::function<void()>& task : posted)
			task();

		Clock::time_point now = Clock::now();
		for (size_t i = 0; i < m_tasks.size() && m_running; ++i) // a task may add tasks
		{
			if (m_tasks[i].next > now)
				continue;
			m_tasks[i].next = now + m_tasks[i].interval;
			const std::function<void()> task = m_tasks[i].run;
			task();
		}
		bool following = false;
		for (size_t i = 0; i < m_stepTasks.size() && m_running; ++i)
		{
			const std::function<bool()> task = m_stepTasks[i];
			following = task() || following;
		}
		m_nextFrame = following ? Clock::now() + g_framePeriod : Clock::time_point::max();

		now = Clock::now();
		for (auto& [id, window] : m_open)
			if (window->m_renderRequested || nextUpdate(*window, now) <= now)
				draw(*window);
	}

	void Shell::every(std::chrono::milliseconds interval, std::function<void()> task)
	{
		m_tasks.push_back({interval, std::move(task), Clock::now() + interval});
	}

	void Shell::eachStep(std::function<bool()> task)
	{
		m_stepTasks.push_back(std::move(task));
	}

	std::optional<std::chrono::milliseconds> Shell::idleTimeout() const
	{
		const Clock::time_point now = Clock::now();
		Clock::time_point next = m_nextFrame;
		for (const auto& [id, window] : m_open)
		{
			if (window->m_renderRequested)
				return std::chrono::milliseconds(0);
			next = std::min(next, nextUpdate(*window, now));
		}
		for (const Task& task : m_tasks)
			next = std::min(next, task.next);
		if (next == Clock::time_point::max())
			return std::nullopt;
		const auto wait = std::chrono::ceil<std::chrono::milliseconds>(next - now);
		return std::max(wait, std::chrono::milliseconds(0));
	}

	int Shell::run()
	{
		while (m_running && !m_open.empty())
			step(idleTimeout());
		return 0;
	}

	void Shell::showFileDialog(const FileDialog& dialog, std::function<void(const FileDialogResult&)> done)
	{
		const uint64_t id = m_nextDialog++;
		m_dialogs.emplace(id, std::move(done));
		m_windows.showFileDialog(id, dialog);
	}

	std::optional<Image> Shell::screenshot(WindowId id)
	{
		Window* window = find(id);
		if (window == nullptr)
			return std::nullopt;
		window->m_context->Update();
		return m_renderer.capture(id, *window->m_context);
	}
}
