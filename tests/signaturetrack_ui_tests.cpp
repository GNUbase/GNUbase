// SPDX-License-Identifier: GPL-3.0-or-later
// The signature track in the UI (#156): the time signature events on the signature track's lane in the event display
// on the fake window system, following a real engine, the ruler across signature changes, and the time signature
// display of the tempo track editor in the app's UI.
#include "engine/engine.h"
#include "engine/time/signaturemap.h"
#include "testutils/fakeui.h"
#include "ui/automation.h"
#include "ui/canvas.h"
#include "ui/enginebinding.h"
#include "ui/eventdisplay.h"
#include "ui/fonts.h"
#include "ui/rmlui.h"
#include "ui/shell.h"
#include "ui/signaturetrack.h"
#include "ui/timeformat.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase;
using Catch::Approx;
using engine::Json;
using std::chrono::milliseconds;

namespace
{
	ui::Target point(double x, double y)
	{
		return {"", 0, ui::Point{static_cast<int>(x), static_cast<int>(y)}};
	}

	ui::Target centre(Rml::Element& element)
	{
		return point(element.GetAbsoluteLeft() + (element.GetOffsetWidth() / 2.0F),
			element.GetAbsoluteTop() + (element.GetOffsetHeight() / 2.0F));
	}

	Json change(int64_t bar, int numerator, int denominator)
	{
		return {{"bar", bar}, {"numerator", numerator}, {"denominator", denominator}};
	}

	// The event display with its signature lane, filling a 1000 by 400 window of the fake UI at a monitor scale of 1,
	// so that a dp is a pixel, following an engine on the dummy device.
	struct Fixture
	{
		Fixture()
		{
			ui.shell.eachStep([this] { return binding.update(); });
			step();
		}

		static ui::WindowSpec spec()
		{
			ui::Fonts(std::filesystem::path(GNUBASE_RESOURCES_DIR) / "fonts").load();
			ui::WindowSpec spec;
			spec.bounds = ui::Rect{0, 0, 1000, 400};
			return spec;
		}

		Rml::ElementDocument& load() const
		{
			const std::string base = std::string(GNUBASE_RESOURCES_DIR) + "/styles/base.rcss";
			Rml::ElementDocument* document =
				window.context().LoadDocumentFromMemory(R"(<rml><head><link type="text/rcss" href=")" + base +
					R"("/><style>body { position: absolute; left: 0; top: 0; right: 0; bottom: 0; font-family: Inter; }
				#display { position: absolute; left: 0; top: 0; right: 0; bottom: 0; }</style></head>
				<body><div id="display" class="event-display"/></body></rml>)");
			REQUIRE(document != nullptr);
			document->Show();
			return *document;
		}

		Json call(const std::string& command, const Json& parameters = Json::object())
		{
			const engine::Result result = engine::Client(engine).call(command, parameters);
			INFO(command << ": " << result.error);
			REQUIRE(result.ok());
			return result.value;
		}

		void step() { ui.shell.step(milliseconds(0)); }

		bool waitUntil(const std::function<bool()>& done)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!done())
			{
				if (std::chrono::steady_clock::now() >= deadline)
					return false;
				ui.shell.step(milliseconds(5));
			}
			step();
			return true;
		}

		Json signatures() { return call("project.get").at("signatures"); }

		Rml::ElementList events() const
		{
			Rml::ElementList found;
			document.QuerySelectorAll(found, ".signature-row .signature-event");
			return found;
		}

		Rml::Element& row() const
		{
			Rml::Element* found = document.QuerySelector(".signature-row");
			REQUIRE(found != nullptr);
			return *found;
		}

		// A point on the row, at a time in seconds.
		ui::Target at(double seconds) const
		{
			const ui::TimelineView& view = display.view();
			Rml::Element& element = row();
			return point(element.GetAbsoluteLeft() + ((seconds - view.start) / view.scale),
				element.GetAbsoluteTop() + (element.GetOffsetHeight() / 2.0F));
		}

		void click(const ui::Target& target)
		{
			automation.click(std::nullopt, target, ui::MouseButton::Left, 1, {});
			step();
		}

		engine::Engine engine;
		test::FakeUi ui{{{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}}};
		ui::Window& window = ui.shell.open(spec());
		ui::EngineBinding binding{engine, [this] { ui.shell.wake(); }};
		Rml::ElementDocument& document = load();
		ui::EventDisplay display{engine, binding, *document.GetElementById("display")};
		ui::SignatureLane lane{display, binding};
		ui::Automation automation{ui.shell};
	};

	Rml::ElementDocument& editorDocument(test::UiApp& app)
	{
		ui::Window* window = app.shell().find(app.appUi().tempoWindow());
		REQUIRE(window != nullptr);
		Rml::ElementDocument* document = window->context().GetDocument(0);
		REQUIRE(document != nullptr);
		return *document;
	}
}

TEST_CASE("signatures are typed as numerator and denominator", "[unit]")
{
	CHECK(ui::parseSignature("3/4") == std::make_pair(3, 4));
	CHECK(ui::parseSignature(" 7 / 8 ") == std::make_pair(7, 8));
	CHECK(!ui::parseSignature("3"));
	CHECK(!ui::parseSignature("3.5/4"));
	CHECK(!ui::parseSignature("x/4"));
}

TEST_CASE("the bars and beats ruler follows signature changes", "[unit]")
{
	// 4/4 for two bars, then 3/4: at 120 BPM bars start at 0, 2 and 4 seconds, then every 1.5 seconds.
	ui::TimeContext context;
	context.signatures = time::SignatureMap({{0, {4, 4}}, {2, {3, 4}}});
	std::vector<ui::RulerMark> marks;
	ui::rulerScale(ui::TimeFormat::BarsBeats, context).marks(0.0, 7.5, 0.1, marks);
	std::vector<std::pair<std::string, double>> bars;
	std::vector<double> beats;
	for (const ui::RulerMark& mark : marks)
		if (const auto dots = std::count(mark.label.begin(), mark.label.end(), '.'); !mark.label.empty() && dots == 0)
			bars.emplace_back(mark.label, mark.position);
		else if (dots == 1 && mark.label.rfind("4.", 0) == 0)
			beats.push_back(mark.position);
	CHECK(bars ==
		std::vector<std::pair<std::string, double>>{{"1", 0.0}, {"2", 2.0}, {"3", 4.0}, {"4", 5.5}, {"5", 7.0}});
	CHECK(beats == std::vector<double>{6.0, 6.5}); // bar 4 has three beats
}

TEST_CASE("the signature lane shows the time signature events, where clicks add, edit, move and erase them", "[unit]")
{
	Fixture fixture;
	fixture.call("track.add", {{"type", "audio"}});
	fixture.call("track.add", {{"type", "signature"}});
	fixture.call("signature.add", change(2, 3, 4));
	// Once the display has the signature track's lane.
	REQUIRE(fixture.waitUntil(
		[&fixture] { return fixture.events().size() == 2 && fixture.row().GetOffsetHeight() > 0.0F; }));

	// Over the signature track's lane, the second one, at the bars' starts.
	Rml::Element& row = fixture.row();
	CHECK(!row.IsClassSet("hidden"));
	const std::vector<double>& tops = fixture.display.laneTops();
	CHECK(row.GetAbsoluteTop() - fixture.document.QuerySelector("event-lanes.events")->GetAbsoluteTop() ==
		Approx(tops[1]));
	CHECK(row.GetOffsetHeight() == Approx(tops[2] - tops[1]));
	const ui::TimelineView& view = fixture.display.view();
	Rml::Element& second = *fixture.events()[1];
	CHECK(second.GetInnerRML() == "3/4");
	CHECK(second.GetAbsoluteLeft() - row.GetAbsoluteLeft() == Approx((4.0 - view.start) / view.scale).margin(1.0));

	// The draw tool adds an event at the nearest bar, bar 5 at 7 seconds, and opens it: Enter sets what is typed.
	fixture.display.setTool(ui::Tool::Draw);
	fixture.click(fixture.at(6.5));
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.signatures().size() == 3; }));
	CHECK(fixture.signatures()[2] == change(4, 3, 4));
	auto* field = dynamic_cast<Rml::ElementFormControl*>(fixture.document.QuerySelector(".signature-field"));
	REQUIRE(field != nullptr);
	REQUIRE(fixture.waitUntil([field] { return field->IsClassSet("shown"); }));
	CHECK(field->GetValue() == "3/4");
	fixture.automation.key(std::nullopt, "Mod+A");
	fixture.automation.text(std::nullopt, "5/8");
	fixture.automation.key(std::nullopt, "Enter");
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.signatures()[2] == change(4, 5, 8); }));
	CHECK(!field->IsClassSet("shown"));

	// A double click opens an event too.
	fixture.display.setTool(ui::Tool::ObjectSelection);
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.events().size() == 3; }));
	fixture.automation.click(std::nullopt, centre(*fixture.events()[1]), ui::MouseButton::Left, 2, {});
	fixture.step();
	REQUIRE(fixture.waitUntil([field] { return field->IsClassSet("shown"); }));
	CHECK(field->GetValue() == "3/4");
	fixture.automation.key(std::nullopt, "Mod+A");
	fixture.automation.text(std::nullopt, "6/8");
	fixture.automation.key(std::nullopt, "Enter");
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.signatures()[1] == change(2, 6, 8); }));

	// A drag moves the selected event to the nearest bar: from bar 5 at 7 seconds to 8, nearest to bar 6.
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.events().size() == 3; }));
	const double from = fixture.display.view().start +
		((fixture.events()[2]->GetAbsoluteLeft() - fixture.row().GetAbsoluteLeft()) * fixture.display.view().scale);
	fixture.automation.drag(std::nullopt, centre(*fixture.events()[2]), fixture.at(from + 1.0));
	fixture.step();
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.signatures()[2].at("bar") != 4; }));
	CHECK(fixture.signatures()[2] == change(5, 5, 8));
	CHECK(fixture.lane.row().selection() == std::vector<int64_t>{fixture.signatures()[2].at("bar").get<int64_t>()});

	// The erase tool removes events, but not the first.
	fixture.display.setTool(ui::Tool::Erase);
	fixture.click(centre(*fixture.events()[0]));
	fixture.click(centre(*fixture.events()[2]));
	REQUIRE(fixture.waitUntil([&fixture] { return fixture.signatures().size() == 2; }));
	CHECK(fixture.signatures()[0].at("bar") == 0);

	// Without a signature track the row is hidden.
	const uint64_t signatureTrack = fixture.call("project.get").at("tracks")[1].at("id").get<uint64_t>();
	fixture.call("track.remove", {{"track", signatureTrack}});
	REQUIRE(fixture.waitUntil([&row] { return row.IsClassSet("hidden"); }));
}

TEST_CASE("two presses on a time signature event open it only less than half a second apart", "[unit]")
{
	Fixture fixture;
	fixture.call("track.add", {{"type", "signature"}});
	fixture.call("signature.add", change(2, 3, 4));
	REQUIRE(fixture.waitUntil(
		[&fixture] { return fixture.events().size() == 2 && fixture.row().GetOffsetHeight() > 0.0F; }));
	const auto* field = dynamic_cast<Rml::ElementFormControl*>(fixture.document.QuerySelector(".signature-field"));
	REQUIRE(field != nullptr);
	// RmlUi's clock, which times the presses, stands still but for advance().
	auto* clock = dynamic_cast<ui::SystemInterface*>(Rml::GetSystemInterface());
	REQUIRE(clock != nullptr);
	clock->freeze(true);
	const std::shared_ptr<void> thaw(nullptr, [clock](void*) { clock->freeze(false); });

	clock->advance(1.0);
	fixture.click(centre(*fixture.events()[1]));
	clock->advance(0.6);
	fixture.click(centre(*fixture.events()[1]));
	CHECK(!field->IsClassSet("shown"));
	clock->advance(0.4);
	fixture.click(centre(*fixture.events()[1]));
	CHECK(fixture.waitUntil([field] { return field->IsClassSet("shown"); }));
}

TEST_CASE("the tempo track editor's time signature display adds and edits time signature events", "[ui]")
{
	test::UiApp app{{1000, 600}};
	app.appUi().showProject();
	app.call("signature.add", change(2, 3, 4));
	app.ui().key(std::nullopt, "Mod+T");
	REQUIRE(app.waitUntil([&app] { return app.appUi().tempoEditor() != nullptr; }));
	const ui::WindowId window = app.appUi().tempoWindow();
	Rml::ElementDocument& document = editorDocument(app);
	const auto events = [&document]
	{
		Rml::ElementList found;
		document.QuerySelectorAll(found, "#signature-area .signature-event");
		return found;
	};
	REQUIRE(app.waitUntil([&events] { return events().size() == 2; }));

	// A click on an event selects it and shows it on the info line, where its signature changes.
	app.ui().click(window, centre(*events()[1]), ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&document] { return !document.GetElementById("signature-info")->IsClassSet("hidden"); }));
	CHECK(document.GetElementById("signature-position")->GetInnerRML() == "3");
	app.ui().click(window, {"#signature-value", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	app.ui().key(window, "Mod+A");
	app.ui().text(window, "7/8");
	app.ui().key(window, "Enter");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("signatures")[1] == change(2, 7, 8); }));

	// The draw tool adds one; Delete removes the selected ones.
	app.ui().click(window, {"#tempo-tool-draw", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	Rml::Element* area = document.GetElementById("signature-area");
	REQUIRE(area != nullptr);
	app.ui().click(window,
		point(area->GetAbsoluteLeft() + (area->GetOffsetWidth() * 0.6F),
			area->GetAbsoluteTop() + (area->GetOffsetHeight() / 2.0F)),
		ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("signatures").size() == 3; }));
	app.ui().click(window, {"#tempo-tool-select", 0, std::nullopt}, ui::MouseButton::Left, 1, {});
	REQUIRE(app.waitUntil([&events] { return events().size() == 3; }));
	// A press elsewhere in the row closes the field the new event opened. Over the whole project, bar 3 covers bar
	// 1's event.
	app.ui().click(window, centre(*events()[1]), ui::MouseButton::Left, 1, {});
	CHECK(!document.QuerySelector("#signature-area .signature-field")->IsClassSet("shown"));
	app.ui().click(window, centre(*events()[2]), ui::MouseButton::Left, 1, {"ctrl"});
	CHECK(app.appUi().tempoEditor()->signatures()->selection().size() == 2);
	app.ui().key(window, "Delete");
	REQUIRE(app.waitUntil([&app] { return app.call("project.get").at("signatures").size() == 1; }));
}
