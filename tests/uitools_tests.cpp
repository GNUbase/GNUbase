// SPDX-License-Identifier: GPL-3.0-or-later
// The MCP UI tools (#58) on the fake shell, used the way an MCP client uses them: JSON-RPC requests as text on a server
// thread while the UI thread steps the shell. They read the DOM, take screenshots and inject input through
// ui::Automation; tests/window_tests.cpp checks screenshots and input on the GPU renderer.
#include "app/uitools.h"
#include "engine/engine.h"
#include "mcp/server.h"
#include "testutils/fakeui.h"
#include "ui/fonts.h"
#include "ui/renderer.h"
#include "ui/rmlui.h"
#include "ui/shell.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Input.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace gnubase;
using Catch::Matchers::ContainsSubstring;
using engine::Json;
using std::chrono::milliseconds;

namespace
{
	std::string document(const char* name)
	{
		return std::string(GNUBASE_TEST_DATA_DIR) + "/ui/" + name;
	}

	// Opens the rename dialog as a modal document when the button is clicked, as an app's button would.
	struct DialogOpener final : Rml::EventListener
	{
		void ProcessEvent(Rml::Event& event) override
		{
			Rml::ElementDocument* dialog =
				event.GetCurrentElement()->GetContext()->LoadDocument(document("automation-dialog.rml"));
			REQUIRE(dialog != nullptr);
			dialog->Show(Rml::ModalFlag::Modal);
		}
	};

	// Remembers the last event of each type and its parameters.
	struct Recorder final : Rml::EventListener
	{
		void ProcessEvent(Rml::Event& event) override
		{
			const std::string& type = event.GetType();
			Json parameters = {{"target", event.GetTargetElement()->GetId()}};
			for (const auto& [name, value] : event.GetParameters())
				parameters[name] = value.Get<Rml::String>();
			events[type] = parameters;
		}

		Json events = Json::object();
	};

	// Decodes base64, as MCP clients do for an image's data.
	std::vector<uint8_t> fromBase64(const std::string& text)
	{
		const std::string alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::vector<uint8_t> bytes;
		uint32_t bits = 0;
		int count = 0;
		for (const char c : text)
		{
			if (c == '=')
				break;
			const size_t value = alphabet.find(c);
			if (value == std::string::npos)
				FAIL("not base64: " << c);
			bits = (bits << 6U) | static_cast<uint32_t>(value);
			count += 6;
			if (count >= 8)
			{
				count -= 8;
				bytes.push_back(static_cast<uint8_t>(bits >> static_cast<uint32_t>(count)));
			}
		}
		return bytes;
	}

	// The element with the id in a DOM tree that ui_dom returned; null if none.
	const Json* findId(const Json& nodes, const std::string& id)
	{
		for (const Json& node : nodes)
		{
			if (node.value("id", std::string()) == id)
				return &node;
			if (const auto children = node.find("children"); children != node.end())
				if (const Json* found = findId(*children, id))
					return found;
		}
		return nullptr;
	}

	// A 1920x1080 monitor, and a Retina monitor with twice the pixels per point to its right.
	std::vector<ui::Display> displays()
	{
		return {{1, "Primary", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F},
			{2, "Retina", {1920, 0, 1440, 900}, {1920, 0, 1440, 900}, 2.0F}};
	}

	// The fake shell with a 640x480 window that shows automation.rml, and an MCP server with the UI tools. Tool calls
	// run on a thread of their own, as on the HTTP server, while this thread steps the shell like the app's UI thread.
	struct Fixture
	{
		Fixture()
		{
			ui::Fonts(std::filesystem::path(GNUBASE_RESOURCES_DIR) / "fonts").load();
			window = open({100, 100, 640, 480}, "automation.rml");
			main = window->context().GetDocument(0);
			main->GetElementById("open")->AddEventListener(Rml::EventId::Click, &opener);
			app::addUiTools(server, ui.shell);
		}

		ui::Window* open(const ui::Rect& bounds, const char* name)
		{
			ui::WindowSpec spec;
			spec.bounds = bounds;
			ui::Window& opened = ui.shell.open(spec);
			Rml::ElementDocument* loaded = opened.context().LoadDocument(document(name));
			REQUIRE(loaded != nullptr);
			loaded->Show();
			ui.shell.step(milliseconds(0));
			return &opened;
		}

		// Sends the request on another thread and steps the shell until the answer is there.
		Json send(const std::string& tool, const Json& arguments)
		{
			const std::string request = Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
				{"params", {{"name", tool}, {"arguments", arguments}}}}
											.dump();
			std::future<std::optional<std::string>> answer =
				std::async(std::launch::async, [this, request] { return server.handle(std::string_view(request)); });
			while (answer.wait_for(milliseconds(1)) != std::future_status::ready)
				ui.shell.step(milliseconds(0));
			const std::optional<std::string> text = answer.get();
			REQUIRE(text.has_value());
			const Json response = Json::parse(text.value_or("{}"));
			REQUIRE(response.contains("result"));
			return response.at("result");
		}

		// Calls a tool that must succeed; returns its structured result.
		Json call(const std::string& tool, const Json& arguments = Json::object())
		{
			const Json result = send(tool, arguments);
			INFO(result.dump());
			REQUIRE(!result.at("isError").get<bool>());
			return result.value("structuredContent", Json());
		}

		// Calls a tool that must fail; returns its error message.
		std::string fail(const std::string& tool, const Json& arguments)
		{
			const Json result = send(tool, arguments);
			INFO(result.dump());
			REQUIRE(result.at("isError").get<bool>());
			return result.at("content").at(0).at("text").get<std::string>();
		}

		// The size of the PNG that ui_screenshot returned.
		ui::Size screenshot(const Json& arguments)
		{
			const Json result = send("ui_screenshot", arguments);
			INFO(result.dump().substr(0, 500));
			REQUIRE(!result.at("isError").get<bool>());
			const Json& image = result.at("content").at(0);
			CHECK(image.at("type") == "image");
			CHECK(image.at("mimeType") == "image/png");
			const std::optional<ui::Image> png = ui::decodePng(fromBase64(image.at("data").get<std::string>()));
			REQUIRE(png.has_value());
			return {png.value_or(ui::Image{}).width, png.value_or(ui::Image{}).height};
		}

		test::FakeUi ui{displays()};
		engine::Engine engine;
		mcp::Server server{engine};
		DialogOpener opener;
		ui::Window* window = nullptr;
		Rml::ElementDocument* main = nullptr;
	};
}

TEST_CASE("the UI tools are listed, those that only read marked read-only", "[unit]")
{
	Fixture fixture;
	const Json tools = fixture.server.handle(Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}})
						   .value_or(Json())
						   .at("result")
						   .at("tools");
	Json readOnly = Json::object();
	for (const Json& tool : tools)
		readOnly[tool.at("name").get<std::string>()] = tool.at("annotations").at("readOnlyHint");
	for (const char* name : {"ui_windows", "ui_dom", "ui_screenshot", "ui_wait"})
		CHECK(readOnly.value(name, false));
	for (const char* name : {"ui_move", "ui_click", "ui_drag", "ui_drop", "ui_wheel", "ui_key", "ui_text"})
		CHECK(readOnly.value(name, true) == false);
}

TEST_CASE("ui_windows lists the windows with their pixel size, dp ratio and documents", "[unit]")
{
	Fixture fixture;
	const ui::Window* retina = fixture.open({2000, 100, 400, 300}, "boxes.rml");

	const Json windows = fixture.call("ui_windows").at("windows");
	REQUIRE(windows.size() == 2);
	CHECK(windows[0].at("id") == fixture.window->id());
	CHECK(windows[0].at("main") == true);
	CHECK(windows[0].at("width") == 640);
	CHECK(windows[0].at("height") == 480);
	CHECK(windows[0].at("dpRatio") == 1.0);
	CHECK(windows[0].at("documents") ==
		Json::parse(R"([{"title": "Automation", "source": ")" + document("automation.rml") +
			R"(", "visible": true, "modal": false}])"));
	CHECK(windows[1].at("id") == retina->id());
	CHECK(windows[1].at("main") == false);
	CHECK(windows[1].at("width") == 800);
	CHECK(windows[1].at("dpRatio") == 2.0);
}

TEST_CASE("ui_dom reads the DOM: ids, classes, attributes, text, values, visibility and boxes in pixels", "[unit]")
{
	Fixture fixture;
	const Json tree = fixture.call("ui_dom").at("elements");
	REQUIRE(tree.size() == 1); // the window's one document
	CHECK(tree[0].at("tag") == "body");
	const Json* button = findId(tree, "open");
	REQUIRE(button != nullptr);
	CHECK(button->at("tag") == "button");
	CHECK(button->at("classes") == Json::array({"primary", "wide"}));
	CHECK(button->at("attributes") == Json{{"title", "Opens the rename dialog"}});
	CHECK(button->at("box") == Json{{"x", 10}, {"y", 10}, {"width", 120}, {"height", 30}});
	CHECK(button->at("visible") == true);
	CHECK(button->at("children") == Json::array({Json{{"text", "Rename track"}}}));
	const Json* search = findId(tree, "search");
	REQUIRE(search != nullptr);
	CHECK(search->at("value") == "");
	const Json* hidden = findId(tree, "hidden");
	REQUIRE(hidden != nullptr);
	CHECK(hidden->at("visible") == false);

	SECTION("a selector picks elements; depth limits their children, and elements without them have their text")
	{
		const Json items = fixture.call("ui_dom", {{"selector", "li.item"}, {"depth", 0}}).at("elements");
		REQUIRE(items.size() == 5);
		CHECK(items[1].at("text") == "Bass");
		CHECK(items[1].at("classes") == Json::array({"item", "selected"}));
		CHECK(!items[1].contains("children"));
		const Json list = fixture.call("ui_dom", {{"selector", "#items"}, {"depth", 1}}).at("elements");
		REQUIRE(list.size() == 1);
		CHECK(list[0].at("children").size() == 5);
		CHECK(list[0].at("children")[0].at("text") == "Drums");
		CHECK(fixture.call("ui_dom", {{"selector", "#nothing"}}).at("elements").empty());
	}
	SECTION("boxes are in the window's pixels, twice the dp on a Retina monitor")
	{
		const ui::Window* retina = fixture.open({2000, 100, 640, 480}, "automation.rml");
		const Json found = fixture.call("ui_dom", {{"window", retina->id()}, {"selector", "#open"}}).at("elements");
		REQUIRE(found.size() == 1);
		CHECK(found[0].at("box") == Json{{"x", 20}, {"y", 20}, {"width", 240}, {"height", 60}});
	}
}

TEST_CASE("an MCP client opens a dialog by clicking and reads its contents back", "[unit]")
{
	Fixture fixture;
	CHECK(fixture.call("ui_click", {{"selector", "#open"}}).at("element").at("id") == "open");
	const Json dialog = fixture.call("ui_wait", {{"selector", "#rename"}}).at("elements");
	REQUIRE(dialog.size() == 1);
	CHECK(dialog[0].at("visible") == true);
	const Json documents = fixture.call("ui_windows").at("windows")[0].at("documents");
	REQUIRE(documents.size() == 2);
	CHECK(documents[1].at("title") == "Rename track");
	CHECK(documents[1].at("modal") == true);

	const Json contents = fixture.call("ui_dom", {{"selector", "#rename"}}).at("elements");
	const Json* message = findId(contents, "message");
	REQUIRE(message != nullptr);
	CHECK(message->at("children") == Json::array({Json{{"text", "Enter a new name for Audio 01."}}}));
	const Json* name = findId(contents, "name");
	REQUIRE(name != nullptr);
	CHECK(name->at("value") == "Audio 01");

	// The dialog takes typing: select the name and type a new one.
	CHECK(fixture.call("ui_click", {{"selector", "#name"}}).at("element").at("id") == "name");
	fixture.call("ui_key", {{"keys", "Ctrl+A"}});
	CHECK(fixture.call("ui_text", {{"text", "Bass"}}).at("focus").at("id") == "name");
	CHECK(fixture.call("ui_dom", {{"selector", "#name"}}).at("elements")[0].at("value") == "Bass");
}

TEST_CASE("ui_key presses keys with modifiers, and ui_text types into the focused field", "[unit]")
{
	Fixture fixture;
	Recorder recorder;
	fixture.main->AddEventListener(Rml::EventId::Keydown, &recorder, true); // text fields stop keys from bubbling
	fixture.call("ui_click", {{"selector", "#search"}});
	CHECK(fixture.call("ui_text", {{"text", "drums"}}).at("focus").at("id") == "search");
	CHECK(fixture.call("ui_dom", {{"selector", "#search"}}).at("elements")[0].at("value") == "drums");

	fixture.call("ui_key", {{"keys", "Ctrl+Shift+F5"}});
	const Json& pressed = recorder.events.at("keydown");
	CHECK(pressed.at("key_identifier") == std::to_string(Rml::Input::KI_F5));
	CHECK(pressed.at("ctrl_key") == "1");
	CHECK(pressed.at("shift_key") == "1");
	CHECK(pressed.at("alt_key") == "0");
	for (const char* keys : {"Enter", "Escape", "Tab", "Backspace", "Delete", "Home", "End", "PageUp", "PageDown", "Up",
			 "Down", "Left", "Right", "Space", "a", "Z", "0", "9", "F1", "F24", "Alt+Cmd+Q", "Meta+S"})
	{
		INFO(keys);
		fixture.call("ui_key", {{"keys", keys}});
	}
	CHECK_THAT(fixture.fail("ui_key", {{"keys", "Hyper+Q"}}), ContainsSubstring("Hyper"));
	CHECK_THAT(fixture.fail("ui_key", {{"keys", "Flux"}}), ContainsSubstring("Flux"));
}

TEST_CASE("ui_move hovers, ui_wheel scrolls and ui_drag drops one element on another", "[unit]")
{
	Fixture fixture;
	const Json hovered = fixture.call("ui_move", {{"selector", "#open"}}).at("element");
	CHECK(hovered.at("id") == "open");
	CHECK(hovered.at("pseudoClasses") == Json::array({"hover"}));
	CHECK(fixture.call("ui_move", {{"x", 300}, {"y", 130}}).at("element").at("id") == "target");

	Recorder recorder;
	fixture.main->AddEventListener(Rml::EventId::Mousescroll, &recorder);
	fixture.main->AddEventListener(Rml::EventId::Dragdrop, &recorder);
	CHECK(fixture.call("ui_wheel", {{"selector", "#items"}, {"dy", 2}}).at("element").at("tag") == "li");
	CHECK(std::stof(recorder.events.at("mousescroll").at("wheel_delta_y").get<std::string>()) == 2.0F);
	CHECK(std::stof(recorder.events.at("mousescroll").at("wheel_delta_x").get<std::string>()) == 0.0F);

	const Json dropped =
		fixture.call("ui_drag", {{"from", {{"selector", "#handle"}}}, {"to", {{"selector", "#target"}}}});
	CHECK(dropped.at("element").at("id") == "target");
	CHECK(recorder.events.at("dragdrop").at("target") == "target");
}

TEST_CASE("ui_drop drops files on an element, as the window system does", "[unit]")
{
	Fixture fixture;
	Recorder recorder;
	fixture.main->AddEventListener(ui::g_fileDropEvent, &recorder);
	const Json dropped = fixture.call("ui_drop", {{"selector", "#target"}, {"files", {"a.wav", "b.mid"}}});
	CHECK(dropped.at("element").at("id") == "target");
	const Json& event = recorder.events.at(ui::g_fileDropEvent);
	CHECK(event.at("target") == "target");
	CHECK(event.at("files") == "a.wav\nb.mid");
	CHECK(event.at("count") == "2");
	CHECK(event.at("mouse_x") == "290"); // the centre of #target, 250 to 330 by 100 to 180
	CHECK(event.at("mouse_y") == "140");
	CHECK_THAT(
		fixture.fail("ui_drop", {{"selector", "#target"}, {"files", Json::array()}}), ContainsSubstring("needs files"));
}

TEST_CASE("ui_screenshot returns a PNG of a window or of one element", "[unit]")
{
	Fixture fixture;
	CHECK(fixture.screenshot(Json::object()) == ui::Size{640, 480});
	CHECK(fixture.screenshot({{"selector", "#open"}}) == ui::Size{120, 30});
	CHECK(fixture.screenshot({{"selector", "li"}, {"index", 1}}) == ui::Size{190, 20}); // beside the scroll bar
	const ui::Window* retina = fixture.open({2000, 100, 400, 300}, "automation.rml");
	CHECK(fixture.screenshot({{"window", retina->id()}}) == ui::Size{800, 600});
	CHECK(fixture.screenshot({{"window", retina->id()}, {"selector", "#open"}}) == ui::Size{240, 60});
	CHECK_THAT(fixture.fail("ui_screenshot", {{"selector", "#hidden"}}), ContainsSubstring("not visible"));
}

TEST_CASE("ui_wait waits until a condition holds, and fails when it doesn't in time", "[unit]")
{
	Fixture fixture;
	CHECK(fixture.call("ui_wait", {{"selector", "#hidden"}, {"visible", false}}).at("elements").size() == 1);
	CHECK(fixture.call("ui_wait", {{"selector", "#gone"}, {"visible", false}}).at("elements").empty());
	const Json vocals = fixture.call("ui_wait", {{"selector", "li"}, {"text", "Vocals"}}).at("elements");
	REQUIRE(vocals.size() == 1);
	CHECK(vocals[0].at("text") == "Vocals");
	CHECK_THAT(fixture.fail("ui_wait", {{"selector", "#never"}, {"seconds", 0.2}}), ContainsSubstring("#never"));

	// The dialog opens while the client waits for it.
	const std::string request = Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"},
		{"params", {{"name", "ui_wait"}, {"arguments", {{"selector", "#rename"}, {"seconds", 10}}}}}}
									.dump();
	std::future<std::optional<std::string>> answer = std::async(
		std::launch::async, [&fixture, &request] { return fixture.server.handle(std::string_view(request)); });
	for (int i = 0; i < 20; ++i)
	{
		fixture.ui.shell.step(milliseconds(0));
		std::this_thread::sleep_for(milliseconds(5));
	}
	CHECK(answer.wait_for(milliseconds(0)) == std::future_status::timeout);
	fixture.window->context().LoadDocument(document("automation-dialog.rml"))->Show();
	while (answer.wait_for(milliseconds(1)) != std::future_status::ready)
		fixture.ui.shell.step(milliseconds(0));
	const Json result = Json::parse(answer.get().value_or("{}")).at("result");
	CHECK(result.at("isError") == false);
	CHECK(result.at("structuredContent").at("elements")[0].at("id") == "rename");
}

TEST_CASE("the UI tools say what they can't do: no such window, no match, an invisible element", "[unit]")
{
	Fixture fixture;
	CHECK_THAT(fixture.fail("ui_dom", {{"window", 99}}), ContainsSubstring("no window 99"));
	CHECK_THAT(fixture.fail("ui_click", {{"selector", "#missing"}}), ContainsSubstring("no element matches #missing"));
	CHECK_THAT(fixture.fail("ui_click", {{"selector", "li"}, {"index", 9}}), ContainsSubstring("5 elements match li"));
	CHECK_THAT(fixture.fail("ui_click", {{"selector", "#hidden"}}), ContainsSubstring("not visible"));
	CHECK_THAT(fixture.fail("ui_click", Json::object()), ContainsSubstring("selector or x and y"));
	CHECK_THAT(fixture.fail("ui_click", {{"selector", "#open"}, {"button", "fourth"}}), ContainsSubstring("button"));
}

TEST_CASE("a UI tool fails at once when the UI quits before running it", "[unit]")
{
	Fixture fixture;
	const int wakes = fixture.ui.windows.wakes();
	const std::string request =
		Json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", "ui_windows"}}}}.dump();
	std::future<std::optional<std::string>> answer = std::async(
		std::launch::async, [&fixture, &request] { return fixture.server.handle(std::string_view(request)); });
	while (fixture.ui.windows.wakes() == wakes) // the tool posted its work
		std::this_thread::sleep_for(milliseconds(1));
	fixture.ui.shell.quit();
	REQUIRE(answer.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
	const Json result = Json::parse(answer.get().value_or("{}")).at("result");
	CHECK(result.at("isError") == true);
	CHECK_THAT(result.at("content")[0].at("text").get<std::string>(), ContainsSubstring("closed"));
}
