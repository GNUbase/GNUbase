// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/schema.h"
#include "ui/renderer.h"
#include "ui/shell.h"

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// UI automation (#58): reads the UI back and drives it as a user would, for MCP clients (app/uitools.h), the C++ test
// harness (#59) and Lua (#208). docs/features/ui-automation.md describes it.
namespace gnubase::ui
{
	// Where the mouse goes: the centre of the element that a selector matches (the index-th match), or a point in the
	// window's pixels. Neither means where the mouse is.
	struct Target
	{
		std::string selector;
		int index = 0;
		std::optional<Point> point;

		bool empty() const { return selector.empty() && !point; }
	};

	enum class MouseButton : uint8_t
	{
		Left,
		Right,
		Middle,
	};

	// Reads windows, documents and elements as JSON and injects input into a window's RmlUi context, the way the
	// window system delivers the user's. Coordinates and boxes are in the window's pixels, as in its screenshots. A
	// window that isn't given is the main window. Call every function on the UI thread (onUiThread runs them from
	// others); they throw std::invalid_argument or std::runtime_error, saying why, for what they can't do.
	class Automation
	{
	public:
		explicit Automation(Shell& shell) : m_shell(shell) {}

		// {"windows": [...]}: each window's id, whether it's the main window, its pixel size, its dp ratio and its
		// documents (title, source, visible, modal).
		engine::Json windows();

		// {"elements": [...]}: the window's documents, or the elements the CSS selector matches in them, as trees of
		// elements with tag, id, classes, the other attributes, a form control's value, pseudo-classes such as hover
		// or checked, whether they're visible, their border box and their children, depth levels deep (all if it's
		// negative). Text is a child {"text": ...}; an element whose children are left out has its text instead.
		engine::Json dom(std::optional<WindowId> window, const std::string& selector, int depth);

		// The window as it looks, or the element's border box in it. The element must be visible.
		Image screenshot(std::optional<WindowId> window, const Target& element);

		// Moves the mouse. Returns {"element": ...}, the element now under it, or null.
		engine::Json move(std::optional<WindowId> window, const Target& to);

		// Moves the mouse to the target and clicks count times, holding the modifiers ("ctrl", "shift", "alt",
		// "meta"). Returns the element under the mouse, as move does.
		engine::Json click(std::optional<WindowId> window, const Target& target, MouseButton button, int count,
			const std::vector<std::string>& modifiers);

		// Presses the left button at from, moves to to in steps and releases it there, holding the modifiers. Returns
		// the element under the mouse at the end.
		engine::Json drag(std::optional<WindowId> window, const Target& from, const Target& to,
			const std::vector<std::string>& modifiers = {});

		// Turns the mouse wheel by dx and dy lines (positive is right and down) at the target, or where the mouse is,
		// holding the modifiers.
		engine::Json wheel(std::optional<WindowId> window, const Target& at, float dx, float dy,
			const std::vector<std::string>& modifiers = {});

		// Presses and releases a key with modifiers, written like "Enter", "Ctrl+S" or "Shift+Alt+F5": letters,
		// digits, F1 to F24, Enter, Escape, Tab, Backspace, Delete, Insert, Home, End, PageUp, PageDown, Up, Down,
		// Left, Right, Space; modifiers Ctrl, Shift, Alt and Meta (or Cmd). Returns {"focus": ...}, the element with
		// the keyboard focus, or null.
		engine::Json key(std::optional<WindowId> window, const std::string& keys);

		// Types the UTF-8 text into the element with the focus. Returns the focus, as key does.
		engine::Json text(std::optional<WindowId> window, const std::string& text);

	private:
		// The window, or the main window.
		Window& resolve(std::optional<WindowId> window);

		Shell& m_shell;
	};

	// Runs task on the UI thread through Shell::post and returns what it returned, or throws what it threw. Throws
	// std::runtime_error if the UI thread doesn't run it within the timeout, or if the shell quits first. The task may
	// still run after a timeout, so it must not refer to the caller's variables: capture them by value.
	template <typename Task> auto onUiThread(Shell& shell, Task task, std::chrono::milliseconds timeout)
	{
		using Result = decltype(task());
		auto job = std::make_shared<std::packaged_task<Result()>>(std::move(task));
		std::future<Result> result = job->get_future();
		shell.post([job = std::move(job)] { (*job)(); }); // dropped unrun, it breaks the promise
		if (result.wait_for(timeout) != std::future_status::ready)
			throw std::runtime_error("the UI didn't answer within " + std::to_string(timeout.count()) + " ms");
		try
		{
			return result.get();
		}
		catch (const std::future_error&)
		{
			throw std::runtime_error("the UI closed before it could answer");
		}
	}
}
