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

TEST_CASE("the steps of one control go one at a time, the newest of a gesture replacing those that wait", "[unit]")
{
	engine::Engine engine;
	ui::EngineBinding binding(engine);
	engine::Client client(engine);
	const Json channel = client.call("track.add", {{"type", "audio"}}).value.at("channel").at("id");
	const auto set = [&channel](const char* key, double value)
	{ return Json::array({Json{{"command", "channel.set"}, {"parameters", {{"channel", channel}, {key, value}}}}}); };
	const auto position = [&client] { return client.call("history.get").value.at("position").get<int64_t>(); };
	const auto read = [&client](const char* key)
	{ return client.call("project.get").value.at("channels").at(0).at(key).get<double>(); };
	REQUIRE(client.call("project.get").value.at("channels").at(0).at("id") == channel);
	const int64_t before = position();

	// A drag's steps: one is in flight, the newest waits, and the others never go (#716).
	std::vector<double> answered;
	for (int step = 1; step <= 10; ++step)
		binding.sendStep(7, "volume", "Change Volume", set("volume", -step),
			[&answered, step](const engine::Result& result)
			{
				CHECK(result.ok());
				answered.push_back(-step);
			});
	CHECK(binding.waiting("history.transaction") == 1);
	CHECK(binding.sending("volume"));
	CHECK(!binding.sending("pan"));
	updateUntil(binding, [&binding] { return !binding.sending("volume"); });
	CHECK(answered == std::vector<double>{-1.0, -10.0});
	CHECK(read("volume") == -10.0);
	CHECK(position() == before + 1); // one undo step

	// Steps without a gesture are steps of their own: none is replaced. Another control's don't wait for them.
	for (int step = 1; step <= 3; ++step)
		binding.sendStep(0, "volume", "Change Volume", set("volume", -20.0 - step));
	binding.sendStep(0, "pan", "Change Pan", set("pan", 0.5));
	CHECK(binding.waiting("history.transaction") == 2);
	updateUntil(binding, [&binding] { return !binding.sending("volume") && !binding.sending("pan"); });
	CHECK(read("volume") == -23.0);
	CHECK(read("pan") == 0.5);
	CHECK(position() == before + 5);

	// Another gesture's step doesn't replace the last one's: each ends where it was left.
	binding.sendStep(8, "volume", "Change Volume", set("volume", -30.0));
	binding.sendStep(8, "volume", "Change Volume", set("volume", -31.0));
	binding.sendStep(9, "volume", "Change Volume", set("volume", -40.0));
	updateUntil(binding, [&binding] { return !binding.sending("volume"); });
	CHECK(position() == before + 7);
	REQUIRE(client.call("history.undo").ok());
	CHECK(read("volume") == -31.0);

	// A refused step reaches its done, and the next one goes after it.
	std::optional<engine::Result> refused;
	const Json unknown = Json::array({Json{{"command", "channel.set"}, {"parameters", {{"channel", 999999}}}}});
	binding.sendStep(
		10, "volume", "Change Volume", unknown, [&refused](const engine::Result& result) { refused = result; });
	binding.sendStep(11, "volume", "Change Volume", set("volume", -50.0));
	updateUntil(binding, [&binding] { return !binding.sending("volume"); });
	REQUIRE(refused.has_value());
	CHECK(!refused.value_or(engine::Result{}).ok());
	CHECK(read("volume") == -50.0);

	// An answer handed on before the last step that the engine took is older than the control; one after isn't.
	CHECK(binding.ahead("volume", 0));
	CHECK(!binding.ahead("volume", binding.handed()));
	CHECK(!binding.ahead("pan", binding.handed()));

	// A command that isn't a query goes after the steps that wait: an undo right after a drag undoes all of it.
	const int64_t dragged = position();
	for (int step = 1; step <= 5; ++step)
		binding.sendStep(12, "volume", "Change Volume", set("volume", -60.0 + step));
	CHECK(binding.waiting("history.transaction") == 1);
	binding.send("history.undo");
	CHECK(binding.waiting("history.transaction") == 2);
	updateUntil(binding, [&binding] { return !binding.sending("volume") && binding.waiting("history.undo") == 0; });
	CHECK(read("volume") == -50.0);
	CHECK(position() == dragged);
	// A query doesn't wait for them.
	binding.sendStep(13, "volume", "Change Volume", set("volume", -1.0));
	binding.sendStep(13, "volume", "Change Volume", set("volume", -2.0));
	binding.send("project.get");
	CHECK(binding.waiting("history.transaction") == 1);
	updateUntil(binding, [&binding] { return !binding.sending("volume"); });
	CHECK(read("volume") == -2.0);

	// A gesture's steps under two merge names, such as the info line's for two selections (#1556): the newest of a
	// name replaces only a waiting step of that name, so the first name's last value still goes, as an undo step of
	// its own.
	const int64_t merged = position();
	binding.sendStep(14, "field", "Change Volume", set("volume", -3.0), {}, nullptr, "one");
	binding.sendStep(14, "field", "Change Volume", set("volume", -4.0), {}, nullptr, "one");
	binding.sendStep(14, "field", "Change Pan", set("pan", -0.25), {}, nullptr, "two");
	updateUntil(binding, [&binding] { return !binding.sending("field"); });
	CHECK(read("volume") == -4.0);
	CHECK(read("pan") == -0.25);
	CHECK(position() == merged + 2);
	REQUIRE(client.call("history.undo").ok());
	CHECK(read("volume") == -4.0);
	CHECK(read("pan") == 0.5);
}

TEST_CASE("a control's steps leave no record behind once a read after their last answer was handed on", "[unit]")
{
	engine::Engine engine;
	ui::EngineBinding binding(engine);
	engine::Client client(engine);
	const Json channel = client.call("track.add", {{"type", "audio"}}).value.at("channel").at("id");
	const auto set = [&channel](const char* key, double value)
	{ return Json::array({Json{{"command", "channel.set"}, {"parameters", {{"channel", channel}, {key, value}}}}}); };
	const auto read = [&client](const char* key)
	{ return client.call("project.get").value.at("channels").at(0).at(key).get<double>(); };
	const Json unknown = Json::array({Json{{"command", "channel.set"}, {"parameters", {{"channel", 999999}}}}});
	// A read through the binding, as the event display reads the project after each history.changed; returns the
	// handed() that the views keep with it.
	const auto readProject = [&binding]
	{
		uint64_t handed = 0;
		binding.send(
			"project.get", Json::object(), [&binding, &handed](const engine::Result&) { handed = binding.handed(); });
		updateUntil(binding, [&handed] { return handed != 0; });
		return handed;
	};

	// A drag: the newest step replaces the waiting one, and the last is taken. The record stays until a read after
	// the last answer is in, so that a view showing an older read again meanwhile, e.g. the inspector after a track
	// change, leaves the control as it is (#1305).
	binding.sendStep(7, "volume", "Change Volume", set("volume", -1.0));
	binding.sendStep(7, "volume", "Change Volume", set("volume", -2.0));
	updateUntil(binding, [&binding] { return !binding.sending("volume"); });
	CHECK(read("volume") == -2.0);
	binding.update();
	binding.update();
	CHECK(binding.ahead("volume", 0)); // still ahead of a read from before the drag
	const uint64_t fresh = readProject();
	CHECK(!binding.ahead("volume", fresh));
	CHECK(binding.ahead("volume", 0)); // the views showing what this update handed on still ask about it
	// Another project read out keeps it too: the view that sent it may show an older read until it is in.
	binding.send("project.get");
	binding.update();
	CHECK(binding.ahead("volume", 0));
	updateUntil(binding, [&binding] { return binding.waiting("project.get") == 0; });
	binding.update(); // the next one lets the record go (#746)
	CHECK(!binding.ahead("volume", 0));
	CHECK(!binding.sending("volume"));

	// A read sent before the last step is answered before it: the record stays for the next one.
	uint64_t early = 0;
	binding.send(
		"project.get", Json::object(), [&binding, &early](const engine::Result&) { early = binding.handed(); });
	binding.sendStep(7, "pan", "Change Pan", set("pan", 0.5));
	updateUntil(binding, [&binding] { return !binding.sending("pan"); });
	CHECK(read("pan") == 0.5);
	CHECK(early != 0);
	binding.update();
	CHECK(binding.ahead("pan", early));
	readProject();
	binding.update();
	CHECK(!binding.ahead("pan", 0));

	// A refused last step: its record goes too; an earlier taken step keeps the control ahead until then.
	binding.sendStep(8, "volume", "Change Volume", set("volume", -3.0));
	binding.sendStep(9, "volume", "Change Volume", unknown);
	updateUntil(binding, [&binding] { return !binding.sending("volume"); });
	binding.update();
	CHECK(binding.ahead("volume", 0));
	readProject();
	binding.update();
	CHECK(!binding.ahead("volume", 0));
	CHECK(read("volume") == -3.0); // the refused step changed nothing

	// A failed project read is none: the view that sent it shows its older read still, so the record stays (#1370).
	binding.sendStep(11, "pan", "Change Pan", set("pan", -0.5));
	updateUntil(binding, [&binding] { return !binding.sending("pan"); });
	bool failed = false;
	binding.send("project.get", {{"unknown", 1}}, [&failed](const engine::Result& result) { failed = !result.ok(); });
	updateUntil(binding, [&failed] { return failed; });
	binding.update();
	CHECK(binding.ahead("pan", 0));
	readProject();
	binding.update();
	CHECK(!binding.ahead("pan", 0));

	// A later gesture makes the record again and works as before.
	binding.sendStep(10, "volume", "Change Volume", set("volume", -4.0));
	updateUntil(binding, [&binding] { return !binding.sending("volume"); });
	CHECK(read("volume") == -4.0);
	CHECK(binding.ahead("volume", 0));
}

TEST_CASE("a control's record keeps what the control shows while it is ahead, and lets it go with the record", "[unit]")
{
	engine::Engine engine;
	ui::EngineBinding binding(engine);
	engine::Client client(engine);
	const Json channel = client.call("track.add", {{"type", "audio"}}).value.at("channel").at("id");
	const auto set = [&channel](const char* key, double value)
	{ return Json::array({Json{{"command", "channel.set"}, {"parameters", {{"channel", channel}, {key, value}}}}}); };
	const auto readProject = [&binding]
	{
		uint64_t handed = 0;
		binding.send(
			"project.get", Json::object(), [&binding, &handed](const engine::Result&) { handed = binding.handed(); });
		updateUntil(binding, [&handed] { return handed != 0; });
		return handed;
	};

	// A drag of two events' values: each step's values are merged into what the record keeps, the newest of each.
	binding.sendStep(7, "volume", "Change Volume", set("volume", -1.0), {}, Json{{"1", "-1"}, {"2", "-3"}});
	binding.sendStep(7, "volume", "Change Volume", set("volume", -2.0), {}, Json{{"1", "-2"}});
	const Json dragged = {{"1", "-2"}, {"2", "-3"}};
	REQUIRE(binding.shown("volume", 0) != nullptr);
	CHECK(*binding.shown("volume", 0) == dragged);
	CHECK(binding.shown("pan", 0) == nullptr);
	updateUntil(binding, [&binding] { return !binding.sending("volume"); });
	binding.update();
	REQUIRE(binding.shown("volume", 0) != nullptr); // still ahead of a read from before the drag
	CHECK(*binding.shown("volume", 0) == dragged);
	const uint64_t fresh = readProject();
	CHECK(binding.shown("volume", fresh) == nullptr);
	binding.update();
	CHECK(binding.shown("volume", 0) == nullptr); // gone with the record (#1471)

	// A later gesture keeps its own; a value that isn't an object replaces what is kept, and a step without one
	// keeps nothing.
	binding.sendStep(8, "volume", "Change Volume", set("volume", -4.0), {}, Json{{"1", "-4"}});
	binding.sendStep(8, "volume", "Change Volume", set("volume", -5.0), {}, -5.0);
	REQUIRE(binding.shown("volume", 0) != nullptr);
	CHECK(*binding.shown("volume", 0) == -5.0);
	binding.sendStep(0, "pan", "Change Pan", set("pan", 0.5));
	CHECK(binding.ahead("pan", 0));
	CHECK(binding.shown("pan", 0) == nullptr);
	CHECK(binding.settle(std::chrono::seconds(10)));

	// A step taken and no read since: the next step keeps what the record has, as the project may not have it yet.
	binding.sendStep(9, "transpose", "Change Transpose", set("volume", -6.0), {}, Json{{"1", "-6"}});
	updateUntil(binding, [&binding] { return !binding.sending("transpose"); });
	binding.sendStep(10, "transpose", "Change Transpose", set("volume", -7.0), {}, Json{{"2", "-7"}});
	REQUIRE(binding.shown("transpose", 0) != nullptr);
	CHECK(*binding.shown("transpose", 0) == Json{{"1", "-6"}, {"2", "-7"}});
	// Once a read after its last taken step is in, the project has what the record kept, and an undo since may have
	// changed it: the next step starts the record's values afresh, also before the record goes (#1550).
	updateUntil(binding, [&binding] { return !binding.sending("transpose"); });
	readProject();
	binding.sendStep(11, "transpose", "Change Transpose", set("volume", -8.0), {}, Json{{"2", "-8"}});
	REQUIRE(binding.shown("transpose", 0) != nullptr);
	CHECK(*binding.shown("transpose", 0) == Json{{"2", "-8"}});
	CHECK(binding.settle(std::chrono::seconds(10)));
}

TEST_CASE(
	"settling sends the steps that wait and hands on results until no command that changes things is out", "[unit]")
{
	engine::Engine engine;
	ui::EngineBinding binding(engine);
	engine::Client client(engine);
	const Json channel = client.call("track.add", {{"type", "audio"}}).value.at("channel").at("id");
	const auto volume = [&channel](double value)
	{
		return Json::array(
			{Json{{"command", "channel.set"}, {"parameters", {{"channel", channel}, {"volume", value}}}}});
	};
	const auto position = [&client] { return client.call("history.get").value.at("position").get<int64_t>(); };
	const int64_t before = position();

	// A step in flight, one that waits, and a command that a done sends.
	binding.sendStep(0, "volume", "Change Volume", volume(-1.0));
	binding.sendStep(0, "volume", "Change Volume", volume(-2.0));
	bool followed = false;
	binding.send("track.add", {{"type", "midi"}}, [&binding, &followed](const engine::Result& /*result*/)
		{ binding.send("track.add", {{"type", "audio"}}, [&followed](const engine::Result&) { followed = true; }); });
	binding.send("project.get"); // a query doesn't keep it waiting
	CHECK(binding.settle(std::chrono::seconds(10)));
	CHECK(followed);
	CHECK(!binding.sending("volume"));
	CHECK(binding.waiting("track.add") == 0);
	CHECK(position() == before + 4);

	// Only a step in flight, one that waits and a query: settling sends the one that waits, too.
	binding.sendStep(0, "volume", "Change Volume", volume(-3.0));
	binding.sendStep(0, "volume", "Change Volume", volume(-4.0));
	binding.send("project.get");
	CHECK(binding.settle(std::chrono::seconds(10)));
	CHECK(!binding.sending("volume"));
	CHECK(position() == before + 6);
	CHECK(client.call("project.get").value.at("channels").at(0).at("volume") == -4.0);
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

TEST_CASE("a watched value draws its window only when apply says it changed what an element shows", "[unit]")
{
	engine::Engine engine;
	test::FakeUi fake({{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}});
	const ui::Window& shows = fake.shell.open(bounds(100, 100, 800, 600));
	ui::EngineBinding binding(engine, [&fake] { fake.shell.wake(); }, [&fake] { fake.shell.requestRender(); });
	fake.shell.eachStep([&binding] { return binding.update(); });
	Rml::ElementDocument* document =
		shows.context().LoadDocumentFromMemory(R"(<rml><body><p id="beat"/></body></rml>)");
	REQUIRE(document != nullptr);
	document->Show();
	Rml::Element* beat = document->GetElementById("beat");
	REQUIRE(beat != nullptr);
	fake.shell.step(milliseconds(0));

	// A display that shows the beat of a position in samples, which changes far more often than the beat does: as a
	// time display does in a coarse format during playback.
	int64_t position = 0;
	const ui::Subscription watch = binding.watch<int64_t>(
		beat, [&position] { return position; },
		[beat](const int64_t& now)
		{
			const std::string text = std::to_string(now / 1000);
			if (beat->GetInnerRML() == text)
				return false;
			beat->SetInnerRML(text);
			return true;
		});
	fake.shell.step(milliseconds(0)); // the first value
	const uint64_t frames = shows.frames();
	CHECK(beat->GetInnerRML() == "0");

	// The value changes, what the element shows doesn't: the window stays idle.
	for (const int64_t now : {100, 200, 999})
	{
		position = now;
		fake.shell.step(milliseconds(0));
		CHECK(shows.frames() == frames);
	}
	position = 1000;
	fake.shell.step(milliseconds(0));
	CHECK(beat->GetInnerRML() == "1");
	CHECK(shows.frames() == frames + 1);
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

TEST_CASE("settling waits for a query that what the user did waits for, and for what its answer sends", "[unit]")
{
	// As the event display's actions wait for snap.zeroCrossings (#1086): settling returns only once the action that
	// the answer runs has been sent and answered. The query takes a while, so that settling starts before its answer.
	engine::Engine engine;
	engine.registerCommand(
		{"test.slowRead", 1, "Reads slowly.", engine::CommandInfo{}.parameters, Json::object(), true},
		[](engine::CommandContext&)
		{
			std::this_thread::sleep_for(milliseconds(200));
			return Json::object();
		});
	ui::EngineBinding binding(engine);
	engine::Client client(engine);
	const auto tracks = [&client] { return client.call("project.get").value.at("tracks").size(); };
	bool answered = false;
	binding.sendAwaited("test.slowRead", Json::object(),
		[&binding, &answered](const engine::Result& /*result*/)
		{
			answered = true;
			binding.send("track.add", {{"type", "audio"}});
		});
	CHECK(binding.settle(std::chrono::seconds(10)));
	CHECK(answered);
	CHECK(binding.waiting("track.add") == 0);
	CHECK(tracks() == 1);
}

TEST_CASE("what the user does while an awaited query is out runs after its answer, in order", "[unit]")
{
	engine::Engine engine;
	engine.registerCommand(
		{"test.slowRead", 1, "Reads slowly.", engine::CommandInfo{}.parameters, Json::object(), true},
		[](engine::CommandContext&)
		{
			std::this_thread::sleep_for(milliseconds(200));
			return Json::object();
		});
	ui::EngineBinding binding(engine);
	std::vector<std::string> ran;
	// Without an awaited query out, at once.
	binding.afterAwaited([&ran] { ran.emplace_back("at once"); });
	CHECK(ran == std::vector<std::string>{"at once"});
	CHECK(!binding.holding());

	// While one is out, after its done, in the order given; what a held one runs itself runs at once.
	ran.clear();
	binding.sendAwaited(
		"test.slowRead", Json::object(), [&ran](const engine::Result& /*result*/) { ran.emplace_back("answer"); });
	binding.afterAwaited(
		[&binding, &ran]
		{
			ran.emplace_back("first");
			binding.afterAwaited([&ran] { ran.emplace_back("inside first"); });
		});
	binding.afterAwaited([&ran] { ran.emplace_back("second"); });
	CHECK(ran.empty());
	CHECK(binding.holding());
	updateUntil(binding, [&binding] { return !binding.holding(); });
	CHECK(ran == std::vector<std::string>{"answer", "first", "inside first", "second"});

	// A held one that sends an awaited query again holds those after it until that is answered too.
	ran.clear();
	binding.sendAwaited("test.slowRead", Json::object(), {});
	binding.afterAwaited(
		[&binding, &ran]
		{
			ran.emplace_back("first");
			binding.sendAwaited("test.slowRead", Json::object(),
				[&ran](const engine::Result& /*result*/) { ran.emplace_back("again"); });
		});
	binding.afterAwaited([&ran] { ran.emplace_back("second"); });
	updateUntil(binding, [&binding] { return !binding.holding(); });
	CHECK(ran == std::vector<std::string>{"first", "again", "second"});

	// releaseHeld runs them without waiting.
	ran.clear();
	binding.sendAwaited("test.slowRead", Json::object(), {});
	binding.afterAwaited([&ran] { ran.emplace_back("released"); });
	binding.releaseHeld();
	CHECK(ran == std::vector<std::string>{"released"});
	CHECK(binding.settle(std::chrono::seconds(10)));
}

TEST_CASE("awaiting what is held waits for the awaited answers only, not for other commands out", "[unit]")
{
	// As a press in the event display does (#1179): a slow command that isn't a query, sent after the awaited query,
	// is still out once the held action has run, and awaitHeld returns then rather than waiting for it.
	engine::Engine engine;
	engine.registerCommand(
		{"test.slowRead", 1, "Reads slowly.", engine::CommandInfo{}.parameters, Json::object(), true},
		[](engine::CommandContext&)
		{
			std::this_thread::sleep_for(milliseconds(100));
			return Json::object();
		});
	engine.registerCommand(
		{"test.slowWrite", 1, "Writes slowly.", engine::CommandInfo{}.parameters, Json::object(), false},
		[](engine::CommandContext&)
		{
			std::this_thread::sleep_for(milliseconds(1500));
			return Json::object();
		});
	ui::EngineBinding binding(engine);
	// Nothing held: at once.
	CHECK(binding.awaitHeld(std::chrono::seconds(0)));

	bool ran = false;
	binding.sendAwaited("test.slowRead", Json::object(), {});
	binding.send("test.slowWrite");
	binding.afterAwaited([&ran] { ran = true; });
	CHECK(binding.awaitHeld(std::chrono::seconds(10)));
	CHECK(ran);
	CHECK(binding.waiting("test.slowWrite") == 1);

	// What is still held after the timeout stays held.
	ran = false;
	binding.sendAwaited("test.slowRead", Json::object(), {});
	binding.afterAwaited([&ran] { ran = true; });
	CHECK(!binding.awaitHeld(std::chrono::milliseconds(0)));
	CHECK(!ran);
	CHECK(binding.holding());
	CHECK(binding.settle(std::chrono::seconds(10)));
	CHECK(ran);
}
