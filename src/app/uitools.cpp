// SPDX-License-Identifier: GPL-3.0-or-later
#include "app/uitools.h"

#include "ui/automation.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace gnubase::app
{
	using engine::Json;

	namespace
	{
		// How long a tool waits for the UI thread, which may be busy, e.g. loading a project.
		constexpr std::chrono::seconds g_uiTimeout{10};
		// ui_wait checks its condition this often, for at most this long.
		constexpr std::chrono::milliseconds g_waitInterval{20};
		constexpr double g_maxWait = 60.0;

		Json object(Json properties, const std::vector<std::string>& required = {})
		{
			Json schema = {{"type", "object"}, {"additionalProperties", false}, {"properties", std::move(properties)}};
			if (!required.empty())
				schema["required"] = required;
			return schema;
		}

		Json windowProperty()
		{
			return {{"type", "integer"}, {"minimum", 1},
				{"description", "The window's id from ui_windows; the main window if left out."}};
		}

		// Keys held while using the mouse.
		Json modifiersProperty(const std::string& description)
		{
			return {{"type", "array"}, {"items", {{"enum", {"ctrl", "shift", "alt", "meta"}}}},
				{"description", description}};
		}

		// Where the mouse goes: an element by selector, or a point.
		Json targetProperties()
		{
			return {{"window", windowProperty()},
				{"selector",
					{{"type", "string"},
						{"description", "A CSS selector; the mouse goes to the centre of the element it matches."}}},
				{"index",
					{{"type", "integer"}, {"minimum", 0},
						{"description", "Which of the elements the selector matches; 0, the first, if left out."}}},
				{"x",
					{{"type", "integer"},
						{"description", "Instead of a selector: the point in the window's pixels, with y."}}},
				{"y", {{"type", "integer"}, {"description", "The point's y, with x."}}}};
		}

		std::optional<ui::WindowId> windowOf(const Json& arguments)
		{
			if (const auto window = arguments.find("window"); window != arguments.end())
				return window->get<ui::WindowId>();
			return std::nullopt;
		}

		ui::Target targetOf(const Json& arguments)
		{
			ui::Target target;
			target.selector = arguments.value("selector", std::string());
			target.index = arguments.value("index", 0);
			if (arguments.contains("x") && arguments.contains("y"))
				target.point = ui::Point{arguments.at("x").get<int>(), arguments.at("y").get<int>()};
			return target;
		}

		ui::MouseButton buttonOf(const Json& arguments)
		{
			const std::string button = arguments.value("button", std::string("left"));
			if (button == "right")
				return ui::MouseButton::Right;
			if (button == "middle")
				return ui::MouseButton::Middle;
			return ui::MouseButton::Left;
		}

		// Whether an element that ui_dom returned is visible and has the text, if one is given.
		bool shows(const Json& element, const std::string& text)
		{
			return element.at("visible").get<bool>() &&
				(text.empty() || element.value("text", std::string()).find(text) != std::string::npos);
		}

		// Waits until an element that the selector matches is visible, or until none is, and returns them.
		Json wait(ui::Shell& shell, const std::shared_ptr<ui::Automation>& automation, const Json& arguments)
		{
			const std::optional<ui::WindowId> window = windowOf(arguments);
			const auto selector = arguments.at("selector").get<std::string>();
			const auto text = arguments.value("text", std::string());
			const bool visible = arguments.value("visible", true);
			const double seconds = arguments.value("seconds", 5.0);
			const auto deadline = std::chrono::steady_clock::now() +
				std::chrono::milliseconds(std::llround(std::clamp(seconds, 0.0, g_maxWait) * 1000.0));
			while (true)
			{
				const Json elements = ui::onUiThread(
					shell, [automation, window, selector] { return automation->dom(window, selector, 0); }, g_uiTimeout)
										  .at("elements");
				Json matching = Json::array();
				for (const Json& element : elements)
					if (visible ? shows(element, text)
								: text.empty() || element.value("text", std::string()).find(text) != std::string::npos)
						matching.push_back(element);
				const bool shown = std::any_of(
					matching.begin(), matching.end(), [&text](const Json& element) { return shows(element, text); });
				if (shown == visible)
					return {{"elements", std::move(matching)}};
				if (std::chrono::steady_clock::now() >= deadline)
				{
					std::string what = selector;
					if (!text.empty())
						what += " with the text \"" + text + '"';
					std::string message = "waited " + Json(seconds).dump() + " s, but ";
					message += visible ? "no visible element matches " + what : what + " is still visible";
					throw std::runtime_error(message);
				}
				std::this_thread::sleep_for(g_waitInterval);
			}
		}
	}

	void addUiTools(mcp::Server& server, ui::Shell& shell)
	{
		const auto automation = std::make_shared<ui::Automation>(shell);
		// Runs work with the arguments on the UI thread.
		const auto run = [&shell, automation](auto work)
		{
			return [&shell, automation, work](const Json& arguments)
			{
				return ui::onUiThread(
					shell, [automation, work, arguments] { return work(*automation, arguments); }, g_uiTimeout);
			};
		};
		const Json elementResult = object({{"element", {{"type", {"object", "null"}}}}}, {"element"});
		const Json focusResult = object({{"focus", {{"type", {"object", "null"}}}}}, {"focus"});

		server.addTool({"ui_windows",
			"Lists GNUbase's open windows: each window's id, whether it's the main window, its size in pixels, its dp "
			"ratio (pixels per density-independent pixel: its monitor's scale times the UI scale) and its documents "
			"(title, source file, whether visible and modal). Coordinates in the other ui_ tools are in the window's "
			"pixels, as in its screenshots.",
			object(Json::object()), object({{"windows", {{"type", "array"}}}}, {"windows"}), true,
			run([](ui::Automation& ui, const Json& /*arguments*/) { return ui.windows(); })});

		server.addTool({"ui_dom",
			"Reads a window's DOM (RmlUi documents): its documents, or with selector the elements that the CSS "
			"selector matches (like querySelectorAll), each as a tree of elements with tag, id, classes, other "
			"attributes, a form control's value, pseudo-classes such as hover, focus or checked, whether it's visible, "
			"its border box in the window's pixels and its children, depth levels deep. Text is a child {\"text\": "
			"...}; an element whose children are left out has its text instead. Use depth 0 to get just the elements.",
			object({{"window", windowProperty()},
				{"selector", {{"type", "string"}, {"description", "A CSS selector."}}},
				{"depth",
					{{"type", "integer"}, {"minimum", 0}, {"description", "Levels of children; all if left out."}}}}),
			object({{"elements", {{"type", "array"}}}}, {"elements"}), true,
			run(
				[](ui::Automation& ui, const Json& arguments)
				{
					return ui.dom(
						windowOf(arguments), arguments.value("selector", std::string()), arguments.value("depth", -1));
				})});

		server.addTool({"ui_screenshot",
			"Takes a screenshot of a window, or with selector of the element it matches (its border box), as a PNG "
			"image in the window's pixels.",
			object(
				{{"window", windowProperty()}, {"selector", {{"type", "string"}, {"description", "A CSS selector."}}},
					{"index", {{"type", "integer"}, {"minimum", 0}}}}),
			nullptr, true, [&shell, automation](const Json& arguments)
			{
				const ui::Image image = ui::onUiThread(
					shell, [automation, arguments]
					{ return automation->screenshot(windowOf(arguments), targetOf(arguments)); }, g_uiTimeout);
				return Json::binary(ui::encodePng(image)); // encoded on this thread, not the UI's
			}});

		server.addTool({"ui_wait",
			"Waits until an element that the CSS selector matches is visible, and has the text if one is given; with "
			"visible false, until none is. Returns the elements the selector matches then (with the text), like "
			"ui_dom with depth 0, or an error after the seconds have passed.",
			object(
				{{"window", windowProperty()}, {"selector", {{"type", "string"}, {"description", "A CSS selector."}}},
					{"visible",
						{{"type", "boolean"},
							{"description", "False to wait until no element is visible; true if left out."}}},
					{"text", {{"type", "string"}, {"description", "A part of the element's text."}}},
					{"seconds",
						{{"type", "number"}, {"minimum", 0}, {"maximum", g_maxWait},
							{"description", "How long to wait at most; 5 if left out."}}}},
				{"selector"}),
			object({{"elements", {{"type", "array"}}}}, {"elements"}), true,
			[&shell, automation](const Json& arguments) { return wait(shell, automation, arguments); }});

		server.addTool({"ui_move",
			"Moves the mouse to the centre of an element or to a point, e.g. to hover over it. Returns the element "
			"now under the mouse.",
			object(targetProperties()), elementResult, false,
			run([](ui::Automation& ui, const Json& arguments)
				{ return ui.move(windowOf(arguments), targetOf(arguments)); })});

		Json clickProperties = targetProperties();
		clickProperties["button"] = {{"enum", {"left", "right", "middle"}}, {"description", "Left if left out."}};
		clickProperties["count"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 3},
			{"description", "2 for a double click; 1 if left out."}};
		clickProperties["modifiers"] = modifiersProperty("Keys held while clicking.");
		server.addTool({"ui_click",
			"Clicks the centre of the element that the CSS selector matches, or a point. The element must be visible. "
			"Returns the element under the mouse, the one that got the click unless another covers it.",
			object(std::move(clickProperties)), elementResult, false,
			run(
				[](ui::Automation& ui, const Json& arguments)
				{
					return ui.click(windowOf(arguments), targetOf(arguments), buttonOf(arguments),
						arguments.value("count", 1), arguments.value("modifiers", std::vector<std::string>()));
				})});

		Json dragTarget = targetProperties();
		dragTarget.erase("window");
		Json from = object(dragTarget);
		from["description"] = "Where to press the button: selector and index, or x and y.";
		Json to = object(dragTarget);
		to["description"] = "Where to release it, the same way.";
		server.addTool({"ui_drag",
			"Drags with the left button from one element or point to another and drops there, e.g. a fader's cap, "
			"finer with shift held. Returns the element under the mouse at the end.",
			object({{"window", windowProperty()}, {"from", std::move(from)}, {"to", std::move(to)},
					   {"modifiers", modifiersProperty("Keys held while dragging.")}},
				{"from", "to"}),
			elementResult, false,
			run(
				[](ui::Automation& ui, const Json& arguments)
				{
					return ui.drag(windowOf(arguments), targetOf(arguments.at("from")), targetOf(arguments.at("to")),
						arguments.value("modifiers", std::vector<std::string>()));
				})});

		Json wheelProperties = targetProperties();
		wheelProperties["dx"] = {{"type", "number"}, {"description", "Lines to the right; 0 if left out."}};
		wheelProperties["dy"] = {{"type", "number"}, {"description", "Lines down; 0 if left out."}};
		wheelProperties["modifiers"] = modifiersProperty("Keys held while turning the wheel.");
		server.addTool({"ui_wheel",
			"Turns the mouse wheel over an element or a point, or where the mouse is, e.g. to scroll a list or turn "
			"a knob.",
			object(std::move(wheelProperties)), elementResult, false,
			run(
				[](ui::Automation& ui, const Json& arguments)
				{
					return ui.wheel(windowOf(arguments), targetOf(arguments), arguments.value("dx", 0.0F),
						arguments.value("dy", 0.0F), arguments.value("modifiers", std::vector<std::string>()));
				})});

		server.addTool({"ui_key",
			"Presses and releases a key with modifiers, written like \"Enter\" or \"Ctrl+Shift+S\": letters, digits, "
			"F1 to F24, Enter, Escape, Tab, Backspace, Delete, Insert, Home, End, PageUp, PageDown, Up, Down, Left, "
			"Right, Space; modifiers Ctrl, Shift, Alt and Meta (Cmd). Keys don't type text: use ui_text for that. "
			"Returns the element with the keyboard focus.",
			object({{"window", windowProperty()}, {"keys", {{"type", "string"}}}}, {"keys"}), focusResult, false,
			run([](ui::Automation& ui, const Json& arguments)
				{ return ui.key(windowOf(arguments), arguments.at("keys").get<std::string>()); })});

		server.addTool({"ui_text",
			"Types text into the element with the keyboard focus, e.g. a text field that ui_click focused. Returns "
			"that element.",
			object({{"window", windowProperty()}, {"text", {{"type", "string"}}}}, {"text"}), focusResult, false,
			run([](ui::Automation& ui, const Json& arguments)
				{ return ui.text(windowOf(arguments), arguments.at("text").get<std::string>()); })});
	}
}
