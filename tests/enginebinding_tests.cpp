// SPDX-License-Identifier: GPL-3.0-or-later
// The UI-engine binding (#50): the UI sends commands and follows events through an engine client, on the UI thread,
// and reads values that change faster than it draws, such as the playback position, once a frame. One test drives a
// document through MCP and the document's clicks into the engine's command log; the last ones check which windows draw
// what arrived (#715).
#include "engine/engine.h"
#include "mcp/server.h"
#include "testutils/fakeui.h"
#include "ui/datamodel.h"
#include "ui/enginebinding.h"
#include "ui/shell.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace gnubase;
using engine::Json;
using std::chrono::milliseconds;

namespace
{
	// Updates the binding until done() holds; fails after ten seconds.
	void updateUntil(ui::EngineBinding& binding, const std::function<bool()>& done)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (!done() && std::chrono::steady_clock::now() < deadline)
		{
			binding.update();
			std::this_thread::sleep_for(milliseconds(1));
		}
		REQUIRE(done());
	}

	// An MCP client of an in-process server, with messages as JSON text as over a transport.
	class McpClient
	{
	public:
		explicit McpClient(engine::Engine& engine) : m_server(engine)
		{
			request("initialize",
				{{"protocolVersion", "2025-06-18"}, {"capabilities", Json::object()},
					{"clientInfo", {{"name", "gnubase-tests"}, {"version", "1"}}}});
		}

		// Calls a tool that must succeed; returns its result.
		Json call(const std::string& tool, const Json& arguments = Json::object())
		{
			const Json result = request("tools/call", {{"name", tool}, {"arguments", arguments}}).at("result");
			INFO(tool << ": " << result.dump());
			REQUIRE(result.at("isError") == false);
			return result.value("structuredContent", Json::object());
		}

	private:
		Json request(const std::string& method, const Json& params)
		{
			const std::string text =
				Json{{"jsonrpc", "2.0"}, {"id", ++m_id}, {"method", method}, {"params", params}}.dump();
			const std::optional<std::string> response = m_server.handle(std::string_view(text));
			REQUIRE(response.has_value());
			return Json::parse(response.value_or("{}"));
		}

		mcp::Server m_server;
		int m_id = 0;
	};
}

TEST_CASE("command results reach the UI thread when the binding updates, after waking it", "[unit]")
{
	engine::Engine engine;
	std::atomic<int> wakes{0};
	ui::EngineBinding binding(engine, [&wakes] { ++wakes; });
	std::optional<engine::Result> answer;
	std::thread::id answeredOn;
	binding.send("engine.version", Json::object(),
		[&](const engine::Result& result)
		{
			answer = result;
			answeredOn = std::this_thread::get_id();
		});
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (wakes == 0 && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(milliseconds(1));
	REQUIRE(wakes > 0);
	CHECK_FALSE(answer.has_value()); // only in update
	binding.update();
	REQUIRE(answer.has_value());
	const engine::Result version = answer.value_or(engine::Result{});
	CHECK(version.ok());
	CHECK(version.value.at("version") == engine::version());
	CHECK(answeredOn == std::this_thread::get_id());

	// Messages that arrive before the next update wake the UI once.
	engine::Client other(engine);
	REQUIRE(other.call("transport.locate", {{"position", 1}}).ok()); // its event reaches the binding first
	REQUIRE(other.call("transport.locate", {{"position", 2}}).ok());
	CHECK(wakes == 2);
	binding.update();
	REQUIRE(other.call("transport.locate", {{"position", 3}}).ok());
	CHECK(wakes == 3);

	// A failure reaches its handler too; one without a handler is only logged.
	std::optional<engine::Result> failure;
	binding.send("test.unknown", Json::object(), [&failure](const engine::Result& result) { failure = result; });
	binding.send("test.unknown");
	updateUntil(binding, [&failure] { return failure.has_value(); });
	CHECK_FALSE(failure.value_or(engine::Result{}).ok());
}

TEST_CASE("the UI follows the events of every client while it subscribes", "[unit]")
{
	engine::Engine engine;
	ui::EngineBinding binding(engine);
	std::vector<Json> changes;
	std::optional<ui::Subscription> subscription =
		binding.on("transport.changed", [&changes](const engine::Event& event) { changes.push_back(event.data); });
	int once = 0;
	std::optional<ui::Subscription> first;
	first = binding.on("transport.changed",
		[&](const engine::Event&)
		{
			++once;
			first.reset(); // a handler may end its own subscription
		});
	engine::Client other(engine, "mcp");

	REQUIRE(other.call("transport.locate", {{"position", 4800}}).ok());
	updateUntil(binding, [&changes] { return !changes.empty(); });
	CHECK(changes.at(0).at("position") == 4800);
	CHECK(once == 1);

	subscription.reset();
	// A command's events reach every client before its result reaches the sender, so the binding has it already.
	REQUIRE(other.call("transport.locate", {{"position", 9600}}).ok());
	binding.update();
	CHECK(changes.size() == 1);
	CHECK(once == 1);
}

TEST_CASE("the UI hears what commands of a name that it sent did, whoever sent them", "[unit]")
{
	engine::Engine engine;
	ui::EngineBinding binding(engine);
	std::vector<engine::Result> results;
	std::optional<ui::Subscription> subscription =
		binding.onResult("test.unknown", [&results](const engine::Result& result) { results.push_back(result); });
	bool done = false;
	binding.send("test.unknown");
	binding.send("test.unknown", Json::object(), [&done](const engine::Result&) { done = true; });
	binding.send("engine.version");
	updateUntil(binding, [&done, &results] { return done && results.size() == 2; });
	CHECK_FALSE(results.at(0).ok());
	CHECK_FALSE(results.at(0).error.empty());

	// Other clients' commands and ended subscriptions don't count.
	REQUIRE_FALSE(engine::Client(engine).call("test.unknown", Json::object()).ok());
	subscription.reset();
	binding.send("test.unknown", Json::object(), [&done](const engine::Result&) { done = false; });
	updateUntil(binding, [&done] { return !done; });
	CHECK(results.size() == 2);
}

TEST_CASE("watched values reach the UI once an update, and only when they changed", "[unit]")
{
	engine::Engine engine;
	ui::EngineBinding binding(engine);
	CHECK_FALSE(binding.update()); // nothing to follow

	int value = 1;
	std::vector<int> seen;
	std::optional<ui::Subscription> watch = binding.watch<int>(
		nullptr, [&value] { return value; }, [&seen](const int& changed) { seen.push_back(changed); });
	CHECK(binding.update()); // follows a value now
	CHECK(binding.update());
	CHECK(seen == std::vector<int>{1});

	value = 2;
	value = 3; // between two frames: the UI only sees the last
	binding.update();
	CHECK(seen == std::vector<int>{1, 3});

	watch.reset();
	value = 4;
	CHECK_FALSE(binding.update());
	CHECK(seen == std::vector<int>{1, 3});
}

TEST_CASE("state changed through MCP shows in the UI, and UI actions reach the engine's command log", "[integration]")
{
	engine::Engine engine;
	test::FakeUi fake({{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}});
	ui::WindowSpec spec;
	spec.bounds = ui::Rect{100, 100, 800, 600};
	const ui::Window& window = fake.shell.open(spec);
	ui::EngineBinding binding(engine, [&fake] { fake.shell.wake(); });
	fake.shell.eachStep([&binding] { return binding.update(); });

	// A transport view: it shows whether the transport plays from the engine's events and the position read once a
	// frame, and its button sends transport.toggle.
	bool playing = false;
	int64_t position = 0;
	ui::DataModel model(window.context(), "transport");
	REQUIRE(model.constructor().Bind("playing", &playing));
	REQUIRE(model.constructor().Bind("position", &position));
	REQUIRE(model.constructor().BindEventCallback("toggle",
		[&binding](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { binding.send("transport.toggle"); }));
	const ui::Subscription changed = binding.on("transport.changed",
		[&](const engine::Event& event)
		{
			playing = event.data.at("playing").get<bool>();
			model.changed("playing");
		});
	const ui::Subscription positions = binding.watch<int64_t>(
		nullptr, [&engine] { return engine.playbackPosition(); },
		[&](const int64_t& now)
		{
			position = now;
			model.changed("position");
		});
	Rml::ElementDocument* document = window.context().LoadDocumentFromMemory(R"(<rml><body data-model="transport">
		<p id="state">{{playing ? 'playing' : 'stopped'}}</p><p id="position">{{position}}</p>
		<button id="toggle" data-event-click="toggle">Play</button></body></rml>)");
	REQUIRE(document != nullptr);
	document->Show();
	fake.shell.step(milliseconds(0));
	Rml::Element* state = document->GetElementById("state");
	Rml::Element* cursor = document->GetElementById("position");
	REQUIRE(state != nullptr);
	REQUIRE(cursor != nullptr);
	CHECK(state->GetInnerRML() == "stopped");
	CHECK(cursor->GetInnerRML() == "0");

	McpClient mcp(engine);
	const uint64_t frames = window.frames();
	mcp.call("transport_locate", {{"position", 4800}});
	CHECK(fake.windows.wakes() > 0);
	fake.shell.step(milliseconds(0));
	CHECK(cursor->GetInnerRML() == "4800");
	CHECK(window.frames() == frames + 1);

	Rml::Element* toggle = document->GetElementById("toggle");
	REQUIRE(toggle != nullptr);
	toggle->Click();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (state->GetInnerRML() != "playing" && std::chrono::steady_clock::now() < deadline)
		fake.shell.step(milliseconds(1));
	CHECK(state->GetInnerRML() == "playing");

	const Json log = mcp.call("engine_commandLog").at("commands");
	REQUIRE(log.size() == 2);
	CHECK(log[0].at("client") == "mcp");
	CHECK(log[0].at("command") == "transport.locate");
	CHECK(log[1].at("client") == "ui");
	CHECK(log[1].at("command") == "transport.toggle");
}

namespace
{
	ui::WindowSpec bounds(int x, int y, int width, int height)
	{
		ui::WindowSpec spec;
		spec.bounds = ui::Rect{x, y, width, height};
		return spec;
	}
}

TEST_CASE("a watched value draws the window that shows it when it changes, and no other window", "[unit]")
{
	engine::Engine engine;
	test::FakeUi fake({{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}});
	const ui::Window& shows = fake.shell.open(bounds(100, 100, 800, 600));
	const ui::Window& other = fake.shell.open(bounds(1000, 100, 400, 300));
	ui::EngineBinding binding(engine, [&fake] { fake.shell.wake(); }, [&fake] { fake.shell.requestRender(); });
	fake.shell.eachStep([&binding] { return binding.update(); });
	Rml::ElementDocument* document = shows.context().LoadDocumentFromMemory(
		R"(<rml><body><div id="cursor" style="position: absolute; left: 0dp; width: 1dp; height: 10dp;"/></body></rml>)");
	REQUIRE(document != nullptr);
	document->Show();
	Rml::Element* cursor = document->GetElementById("cursor");
	REQUIRE(cursor != nullptr);
	fake.shell.step(milliseconds(0));
	const uint64_t frames = shows.frames();
	const uint64_t otherFrames = other.frames();

	int64_t position = 0;
	const ui::Subscription watch = binding.watch<int64_t>(
		cursor, [&position] { return position; }, [cursor](const int64_t& now)
		{ cursor->SetProperty(Rml::PropertyId::Left, Rml::Property(static_cast<float>(now), Rml::Unit::DP)); });
	fake.shell.step(milliseconds(0)); // the first value
	CHECK(shows.frames() == frames + 1);
	fake.shell.step(milliseconds(0)); // unchanged: the shell follows the value once a frame, but draws nothing
	CHECK(shows.frames() == frames + 1);
	CHECK(fake.shell.idleTimeout().has_value());

	// No input: the window draws the value in the step it changed in, where the element shows it.
	position = 120;
	fake.shell.step(milliseconds(0));
	CHECK(shows.frames() == frames + 2);
	CHECK(cursor->GetAbsoluteLeft() == 120.0F);
	CHECK(other.frames() == otherFrames);
}

TEST_CASE("events, results and posted work draw every window in the step they reach the UI's handlers", "[integration]")
{
	engine::Engine engine;
	test::FakeUi fake({{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}});
	const ui::Window& window = fake.shell.open(bounds(100, 100, 800, 600));
	const ui::Window& other = fake.shell.open(bounds(1000, 100, 400, 300));
	ui::EngineBinding binding(engine, [&fake] { fake.shell.wake(); }, [&fake] { fake.shell.requestRender(); });
	fake.shell.eachStep([&binding] { return binding.update(); });
	Rml::ElementDocument* document =
		window.context().LoadDocumentFromMemory(R"(<rml><body><p id="state">stopped</p></body></rml>)");
	REQUIRE(document != nullptr);
	document->Show();
	Rml::Element* state = document->GetElementById("state");
	REQUIRE(state != nullptr);
	// A view that changes its element on an event, without asking for its window to be drawn.
	std::optional<ui::Subscription> changed = binding.on("transport.changed", [state](const engine::Event& event)
		{ state->SetInnerRML(std::to_string(event.data.at("position").get<int64_t>())); });
	fake.shell.step(milliseconds(0));
	uint64_t frames = window.frames();
	uint64_t otherFrames = other.frames();
	const auto drewOnce = [&]
	{
		CHECK(window.frames() == frames + 1);
		CHECK(other.frames() == otherFrames + 1);
		frames = window.frames();
		otherFrames = other.frames();
	};

	// An event, here from another client: MCP.
	McpClient mcp(engine);
	mcp.call("transport_locate", {{"position", 4800}});
	fake.shell.step(milliseconds(0));
	CHECK(state->GetInnerRML() == "4800");
	drewOnce();
	fake.shell.step(milliseconds(0)); // nothing new: the UI sleeps
	CHECK(window.frames() == frames);
	CHECK(!fake.shell.idleTimeout().has_value());

	// A result.
	bool answered = false;
	binding.send("transport.get", Json::object(),
		[&answered, state](const engine::Result&)
		{
			state->SetInnerRML("answered");
			answered = true;
		});
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!answered && std::chrono::steady_clock::now() < deadline)
	{
		frames = window.frames();
		otherFrames = other.frames();
		fake.shell.step(milliseconds(1));
	}
	REQUIRE(answered);
	drewOnce();

	// A result that only an onResult handler takes, as the pool's reports of its commands do.
	{
		const ui::Subscription reported =
			binding.onResult("transport.get", [state](const engine::Result&) { state->SetInnerRML("reported"); });
		binding.send("transport.get");
		while (binding.waiting("transport.get") > 0 && std::chrono::steady_clock::now() < deadline)
		{
			frames = window.frames();
			otherFrames = other.frames();
			fake.shell.step(milliseconds(1));
		}
		REQUIRE(binding.waiting("transport.get") == 0);
		CHECK(state->GetInnerRML() == "reported");
		drewOnce();
	}

	// Work posted to the UI thread.
	fake.shell.post([state] { state->SetInnerRML("posted"); });
	fake.shell.step(milliseconds(0));
	drewOnce();

	// Events that no handler takes, and results that nothing waits for, draw nothing.
	changed.reset();
	mcp.call("transport_locate", {{"position", 9600}});
	binding.send("transport.set", {{"metronome", true}});
	while (binding.waiting("transport.set") > 0 && std::chrono::steady_clock::now() < deadline)
		fake.shell.step(milliseconds(1));
	CHECK(binding.waiting("transport.set") == 0);
	CHECK(window.frames() == frames);
	CHECK(other.frames() == otherFrames);

	// A quiet query's done draws only what it asks for: nothing when nothing changed, else the window it changed.
	const auto query = [&](bool change)
	{
		binding.sendQuietly("transport.get", Json::object(),
			[change, state](const engine::Result&)
			{
				if (!change)
					return;
				state->SetInnerRML("queried");
				state->GetContext()->RequestNextUpdate(0);
			});
		while (binding.waiting("transport.get") > 0 && std::chrono::steady_clock::now() < deadline)
			fake.shell.step(milliseconds(1));
		REQUIRE(binding.waiting("transport.get") == 0);
	};
	query(false);
	CHECK(window.frames() == frames);
	CHECK(other.frames() == otherFrames);
	query(true);
	CHECK(state->GetInnerRML() == "queried");
	CHECK(window.frames() == frames + 1);
	CHECK(other.frames() == otherFrames);
}
