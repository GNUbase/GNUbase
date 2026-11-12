// SPDX-License-Identifier: GPL-3.0-or-later
// UI tests of the widget library (#54, ui/widgets.h): each widget in GNUbase's UI as the app shows it, driven with
// ui::Automation as MCP's UI tools drive it (tests/uiapp.h), and read back from the DOM, from its events and from
// data models bound to it.
#include "uiapp.h"

#include "ui/automation.h"
#include "ui/datamodel.h"
#include "ui/rmlui.h"
#include "ui/widgets.h"
#include "ui/widgetutils.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Input.h>
#include <SDL3/SDL.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase;
using engine::Json;

namespace
{
	ui::Target at(const std::string& selector)
	{
		return {selector, 0, std::nullopt};
	}

	// The one element that the selector matches, as MCP's ui_dom shows it.
	Json element(test::UiApp& app, const std::string& selector)
	{
		const Json found = app.ui().dom(std::nullopt, selector, 0).at("elements");
		REQUIRE(found.size() == 1);
		return found[0];
	}

	// Whether the element's "classes" or "pseudoClasses" has the name.
	bool has(const Json& element, const char* list, const std::string& name)
	{
		const Json names = element.value(list, Json::array());
		return std::find(names.begin(), names.end(), name) != names.end();
	}

	Json click(test::UiApp& app, const std::string& selector)
	{
		return app.ui().click(std::nullopt, at(selector), ui::MouseButton::Left, 1, {});
	}

	// Moves the mouse off the widgets, so that screenshots don't show them hovered.
	void moveAway(test::UiApp& app)
	{
		app.ui().move(std::nullopt, {"", 0, ui::Point{0, 0}});
	}

	// The clicks and changes that reach the document: clicks by the id of the element that got them, or of its nearest
	// ancestor with one, changes by the id of the element that sent them.
	struct Events final : Rml::EventListener
	{
		explicit Events(Rml::ElementDocument& document) : m_document(document)
		{
			document.AddEventListener(Rml::EventId::Click, this);
			document.AddEventListener(Rml::EventId::Change, this);
		}

		~Events() override
		{
			m_document.RemoveEventListener(Rml::EventId::Click, this);
			m_document.RemoveEventListener(Rml::EventId::Change, this);
		}

		Events(const Events&) = delete;
		Events& operator=(const Events&) = delete;
		Events(Events&&) = delete;
		Events& operator=(Events&&) = delete;

		void ProcessEvent(Rml::Event& event) override
		{
			const Rml::Element* element = event.GetTargetElement();
			if (event == Rml::EventId::Change)
			{
				const auto value = event.GetParameters().find("value");
				changes.emplace_back(
					element->GetId(), value != event.GetParameters().end() ? value->second : Rml::Variant());
				return;
			}
			while (element != nullptr && element->GetId().empty())
				element = element->GetParentNode();
			++clicks[element != nullptr ? element->GetId() : std::string()];
		}

		std::map<std::string, int> clicks;
		std::vector<std::pair<std::string, Rml::Variant>> changes;

	private:
		Rml::ElementDocument& m_document;
	};
}

TEST_CASE("buttons are clicked with the mouse, or with Space and Enter when they have the focus", "[ui]")
{
	test::UiApp app;
	bool muted = false;
	ui::DataModel model(app.main().context(), "buttons");
	REQUIRE(model.constructor().Bind("muted", &muted));
	const Events events(app.show("buttons.rml"));

	CHECK(click(app, "#apply").at("element").at("id") == "apply");
	CHECK(events.clicks.at("apply") == 1);
	app.ui().key(std::nullopt, "Space");
	app.ui().key(std::nullopt, "Enter");
	CHECK(events.clicks.at("apply") == 3);

	// A click on a button's icon or text is a click on the button.
	click(app, "#save svg");
	CHECK(events.clicks.at("save") == 1);

	// A disabled button looks disabled, and clicks pass through it.
	CHECK(has(element(app, "#off"), "pseudoClasses", "disabled"));
	click(app, "#off");
	CHECK(events.clicks.count("off") == 0);
}

TEST_CASE("toggle buttons turn on and off with a click, and data-checked binds their state", "[ui]")
{
	test::UiApp app;
	bool muted = false;
	ui::DataModel model(app.main().context(), "buttons");
	REQUIRE(model.constructor().Bind("muted", &muted));
	const Events events(app.show("buttons.rml"));
	const ui::Image off = app.ui().screenshot(std::nullopt, at("#mute"));

	click(app, "#mute");
	CHECK(muted);
	const Json on = element(app, "#mute");
	CHECK(on.at("attributes").contains("checked"));
	CHECK(has(on, "classes", "checked"));
	REQUIRE(events.changes.size() == 1);
	CHECK(events.changes[0].first == "mute");
	CHECK(events.changes[0].second.Get<bool>());
	moveAway(app);
	CHECK_FALSE(test::compareImages(app.ui().screenshot(std::nullopt, at("#mute")), off, {8}).equal);

	// Space toggles the button with the focus.
	app.ui().key(std::nullopt, "Space");
	CHECK_FALSE(muted);
	CHECK_FALSE(has(element(app, "#mute"), "classes", "checked"));
	CHECK(test::compareImages(app.ui().screenshot(std::nullopt, at("#mute")), off, {8}).equal);

	// The button shows the data model's state, e.g. after an engine event.
	muted = true;
	model.changed("muted");
	CHECK(app.waitUntil([&app] { return has(element(app, "#mute"), "classes", "checked"); }));

	// Without a binding a toggle keeps its own state; disabled, it doesn't change.
	click(app, "#loop");
	CHECK(element(app, "#loop").at("attributes").contains("checked"));
	const size_t changes = events.changes.size();
	click(app, "#locked");
	CHECK(events.changes.size() == changes);
	CHECK_FALSE(element(app, "#locked").at("attributes").contains("checked"));
}

namespace
{
	// The page of widgets with a number at dp ratio 1, so that a dp is a pixel, with its data model.
	struct ValuesPage
	{
		explicit ValuesPage(test::UiApp& app) : app(app), model(app.main().context(), "values")
		{
			REQUIRE(model.constructor().Bind("volume", &volume));
			REQUIRE(model.constructor().Bind("gain", &gain));
			document = &app.show("values.rml");
			app.setDpRatio(1.0F);
			events = std::make_unique<Events>(*document);
		}

		// The element's value attribute as a number.
		float value(const std::string& selector) const
		{
			return std::stof(element(app, selector).at("attributes").at("value").get<std::string>());
		}

		// The element's border box: x, y, width and height in pixels.
		Json box(const std::string& selector) const { return element(app, selector).at("box"); }

		// Drags from the centre of the element by dx and dy pixels, holding the modifiers.
		void drag(const std::string& selector, int dx, int dy, const std::vector<std::string>& modifiers = {}) const
		{
			const Json from = box(selector);
			const int x = from.at("x").get<int>() + (from.at("width").get<int>() / 2);
			const int y = from.at("y").get<int>() + (from.at("height").get<int>() / 2);
			app.ui().drag(std::nullopt, {"", 0, ui::Point{x, y}}, {"", 0, ui::Point{x + dx, y + dy}}, modifiers);
		}

		void wheel(const std::string& selector, float lines, const std::vector<std::string>& modifiers = {}) const
		{
			app.ui().wheel(std::nullopt, at(selector), 0.0F, lines, modifiers);
		}

		test::UiApp& app;
		float volume = 0.0F;
		float gain = 0.0F;
		ui::DataModel model;
		Rml::ElementDocument* document = nullptr;
		std::unique_ptr<Events> events;
	};
}

TEST_CASE("knobs turn with drags, the wheel and the arrow keys, finer with Shift, and Ctrl-click resets them", "[ui]")
{
	test::UiApp app;
	const ValuesPage page(app);
	const ui::Image centred = app.ui().screenshot(std::nullopt, at("#pan"));

	// The knob's range, -100 to 100, takes 200 pixels of mouse movement up or down.
	page.drag("#pan", 0, -50);
	CHECK(page.value("#pan") == Catch::Approx(50.0F));
	page.drag("#pan", 0, -50, {"shift"});
	CHECK(page.value("#pan") == Catch::Approx(55.0F));
	CHECK_FALSE(test::compareImages(app.ui().screenshot(std::nullopt, at("#pan")), centred, {8}).equal);

	// The wheel and the arrow keys change it by a step, 1 here.
	page.wheel("#pan", -1.0F);
	page.wheel("#pan", -1.0F, {"shift"});
	CHECK(page.value("#pan") == Catch::Approx(56.1F));
	app.ui().key(std::nullopt, "Up");
	app.ui().key(std::nullopt, "Shift+Down");
	CHECK(page.value("#pan") == Catch::Approx(57.0F));

	// Each change is a change event with the value.
	REQUIRE(!page.events->changes.empty());
	CHECK(page.events->changes.back().first == "pan");
	CHECK(page.events->changes.back().second.Get<float>() == Catch::Approx(57.0F));

	app.ui().click(std::nullopt, at("#pan"), ui::MouseButton::Left, 1, {"ctrl"});
	CHECK(page.value("#pan") == Catch::Approx(0.0F));
	CHECK(test::compareImages(app.ui().screenshot(std::nullopt, at("#pan")), centred, {8}).equal);
	page.drag("#pan", 0, -500);
	CHECK(page.value("#pan") == Catch::Approx(100.0F));
}

TEST_CASE(
	"a value set during a drag waits until the drag ends, and goes if the drag changes the value after it", "[ui]")
{
	test::UiApp app;
	const ValuesPage page(app);
	Rml::Element& pan = *page.document->GetElementById("pan");
	Rml::Context& context = app.main().context();
	const Json box = page.box("#pan");
	const int x = box.at("x").get<int>() + (box.at("width").get<int>() / 2);
	const int y = box.at("y").get<int>() + (box.at("height").get<int>() / 2);
	context.ProcessMouseMove(x, y, 0);
	context.ProcessMouseButtonDown(0, 0);
	context.ProcessMouseMove(x, y - 10, 0);
	const float dragged = page.value("#pan");
	CHECK(dragged > 0.0F);

	// A value that the document sets, such as the engine's answer to an earlier step (#716), doesn't move the knob,
	// and the drag goes on from where the mouse put it.
	pan.SetAttribute("value", "-30");
	CHECK(page.value("#pan") == dragged);
	context.ProcessMouseMove(x, y - 20, 0);
	CHECK(page.value("#pan") == Catch::Approx(dragged + 10.0F));
	// The drag changed the value after it: the end of the drag keeps the drag's.
	context.ProcessMouseButtonUp(0, 0);
	CHECK(page.value("#pan") == Catch::Approx(dragged + 10.0F));

	// A value set after the last change of the drag shows when it ends.
	context.ProcessMouseButtonDown(0, 0);
	context.ProcessMouseMove(x, y - 30, 0);
	const float again = page.value("#pan");
	CHECK(again > dragged + 10.0F);
	pan.SetAttribute("value", "-40");
	CHECK(page.value("#pan") == again);
	context.ProcessMouseButtonUp(0, 0);
	CHECK(page.value("#pan") == -40.0F);

	// Without a drag, it shows at once.
	pan.SetAttribute("value", "5");
	CHECK(page.value("#pan") == 5.0F);

	// A value that the document sets in answer to a change, such as the change rounded, shows at once.
	ui::widgets::Listener rounding(
		[](Rml::Event& event)
		{
			const float tens = std::round(event.GetParameter("value", 0.0F) / 10.0F);
			event.GetTargetElement()->SetAttribute("value", ui::widgets::format(tens * 10.0F));
		});
	pan.AddEventListener(Rml::EventId::Change, &rounding);
	context.ProcessMouseMove(x, y, 0);
	context.ProcessMouseButtonDown(0, 0);
	context.ProcessMouseMove(x, y - 13, 0);
	CHECK(page.value("#pan") == 20.0F);
	context.ProcessMouseButtonUp(0, 0);
	CHECK(page.value("#pan") == 20.0F);
	pan.RemoveEventListener(Rml::EventId::Change, &rounding);
}

TEST_CASE("fader caps follow the mouse, data-value binds faders, and the wheel turns them instead of scrolling", "[ui]")
{
	test::UiApp app;
	ValuesPage page(app);

	// The cap travels 108 of the fader's 120 pixels; -60 to 6 dB is 66 dB, so 54 pixels are 33 dB.
	const Json before = page.box("#volume .fader-cap");
	page.drag("#volume .fader-cap", 0, 54);
	CHECK(page.value("#volume") == Catch::Approx(-33.0F).margin(0.01));
	CHECK(page.volume == Catch::Approx(-33.0F).margin(0.01));
	CHECK(page.box("#volume .fader-cap").at("y").get<int>() - before.at("y").get<int>() == 54);

	// The fader follows its data, e.g. after an engine event.
	page.volume = -60.0F;
	page.model.changed("volume");
	const Json fader = page.box("#volume");
	const int bottom = fader.at("y").get<int>() + fader.at("height").get<int>() - 6;
	CHECK(app.waitUntil(
		[&page, bottom]
		{
			const Json cap = page.box("#volume .fader-cap");
			return cap.at("y").get<int>() + (cap.at("height").get<int>() / 2) == bottom;
		}));

	// Over a fader the wheel turns it; elsewhere it scrolls the page.
	page.wheel("#volume", -10.0F);
	CHECK(page.volume == Catch::Approx(-53.4F).margin(0.01));
	app.shell().step(std::chrono::milliseconds(0));
	Rml::Element& scrolled = *page.document->GetElementById("page");
	CHECK(scrolled.GetScrollTop() == 0.0F);
	app.ui().wheel(std::nullopt, {"", 0, ui::Point{300, 400}}, 0.0F, 3.0F);
	CHECK(app.waitUntil([&scrolled] { return scrolled.GetScrollTop() > 0.0F; }));
}

TEST_CASE("slider thumbs follow the mouse, with the track filled up to them", "[ui]")
{
	test::UiApp app;
	const ValuesPage page(app);

	// The thumb travels 104 of the slider's 120 pixels.
	const Json before = page.box("#send .slider-thumb");
	page.drag("#send .slider-thumb", 52, 0);
	CHECK(page.value("#send") == Catch::Approx(0.75F));
	CHECK(page.box("#send .slider-thumb").at("x").get<int>() - before.at("x").get<int>() == 52);
	CHECK(page.box("#send .slider-fill").at("width").get<int>() == 90);
}

TEST_CASE("value fields are dragged like knobs, and a double click edits them as text", "[ui]")
{
	test::UiApp app;
	const ValuesPage page(app);
	const auto text = [&app] { return element(app, "#gain .valuefield-text").at("text").get<std::string>(); };
	const auto editing = [&app] { return has(element(app, "#gain"), "classes", "editing"); };
	CHECK(text() == "0.0 dB");

	// -24 to 24 dB in 200 pixels.
	page.drag("#gain", 0, -50);
	CHECK(page.gain == Catch::Approx(12.0F));
	CHECK(text() == "12.0 dB");

	// Enter takes the number typed, Escape keeps the value, and so does text that isn't a number.
	const auto type = [&app](const std::string& typed, const std::string& key)
	{
		app.ui().click(std::nullopt, at("#gain"), ui::MouseButton::Left, 2, {});
		app.ui().key(std::nullopt, "Mod+A");
		app.ui().text(std::nullopt, typed);
		if (!key.empty())
			app.ui().key(std::nullopt, key);
	};
	type("-6.5", "Enter");
	CHECK_FALSE(editing());
	CHECK(page.gain == Catch::Approx(-6.5F));
	CHECK(text() == "-6.5 dB");
	type("3", "Escape");
	CHECK(page.gain == Catch::Approx(-6.5F));
	type("loud", "Enter");
	CHECK(page.gain == Catch::Approx(-6.5F));
	CHECK(text() == "-6.5 dB");

	// Leaving the field takes the number too; values outside the range are clamped.
	type("40", "");
	CHECK(editing());
	CHECK(element(app, "#gain input").at("value") == "40");
	app.ui().click(std::nullopt, {"", 0, ui::Point{300, 400}}, ui::MouseButton::Left, 1, {});
	CHECK_FALSE(editing());
	CHECK(page.gain == Catch::Approx(24.0F));

	// The field's own changes have numbers. (Its text field sends RmlUi's change events with the text typed.)
	for (const auto& [id, value] : page.events->changes)
		if (id == "gain")
			CHECK(value.GetType() == Rml::Variant::FLOAT);

	// A disabled field doesn't change.
	page.drag("#locked", 0, -50);
	app.ui().click(std::nullopt, at("#locked"), ui::MouseButton::Left, 2, {});
	CHECK(page.value("#locked") == Catch::Approx(0.5F));
	CHECK_FALSE(has(element(app, "#locked"), "classes", "editing"));
}

TEST_CASE("text displays show their value, and a double click edits it as text", "[ui]")
{
	test::UiApp app;
	const ValuesPage page(app);
	const auto text = [&app] { return element(app, "#position .textdisplay-text").at("text").get<std::string>(); };
	CHECK(text() == "1.1.1.0");
	const auto type = [&app](const std::string& typed, const std::string& key)
	{
		app.ui().click(std::nullopt, at("#position"), ui::MouseButton::Left, 2, {});
		CHECK(has(element(app, "#position"), "classes", "editing"));
		app.ui().key(std::nullopt, "Mod+A");
		app.ui().text(std::nullopt, typed);
		app.ui().key(std::nullopt, key);
		CHECK_FALSE(has(element(app, "#position"), "classes", "editing"));
	};

	// Enter sends the text typed; the document decides what the display shows.
	type("5.1", "Enter");
	std::vector<std::string> changes;
	for (const auto& [id, value] : page.events->changes)
		if (id == "position")
			changes.push_back(value.Get<Rml::String>());
	CHECK(changes == std::vector<std::string>{"5.1"});
	CHECK(text() == "1.1.1.0");
	page.document->GetElementById("position")->SetAttribute("value", "5.1.1.0");
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(text() == "5.1.1.0");

	// Escape sends nothing.
	type("9", "Escape");
	const auto count = [&page]
	{
		size_t found = 0;
		for (const auto& change : page.events->changes)
			found += change.first == "position" ? 1 : 0;
		return found;
	};
	CHECK(count() == 1);

	// Nor do Enter or leaving the field when nothing was typed.
	app.ui().click(std::nullopt, at("#position"), ui::MouseButton::Left, 2, {});
	app.ui().key(std::nullopt, "Enter");
	CHECK_FALSE(has(element(app, "#position"), "classes", "editing"));
	app.ui().click(std::nullopt, at("#position"), ui::MouseButton::Left, 2, {});
	click(app, "#left");
	CHECK_FALSE(has(element(app, "#position"), "classes", "editing"));
	CHECK(count() == 1);

	// Typing the text it started with sends it: the display may show a rounded value, or one that has moved on since.
	type("5.1.1.0", "Enter");
	CHECK(count() == 2);
	CHECK(page.events->changes.back().second.Get<Rml::String>() == "5.1.1.0");

	// A starting text that the text field cleans up, such as one with a tab, isn't typing either.
	page.document->GetElementById("position")->SetAttribute("value", "a\tb");
	app.shell().step(std::chrono::milliseconds(0));
	app.ui().click(std::nullopt, at("#position"), ui::MouseButton::Left, 2, {});
	app.ui().key(std::nullopt, "Enter");
	CHECK_FALSE(has(element(app, "#position"), "classes", "editing"));
	CHECK(count() == 2);
}

TEST_CASE("value fields change when a number is typed, also the one they show, and not without typing", "[ui]")
{
	test::UiApp app;
	ValuesPage page(app);
	const auto text = [&app] { return element(app, "#gain .valuefield-text").at("text").get<std::string>(); };
	const auto changes = [&page]
	{
		size_t found = 0;
		for (const auto& change : page.events->changes)
			found += change.first == "gain" ? 1 : 0;
		return found;
	};

	// A value with more decimals than the field shows stays as it is after Enter or leaving the field untouched.
	page.gain = 1.234F;
	page.model.changed("gain");
	REQUIRE(app.waitUntil([&text] { return text() == "1.2 dB"; }));
	app.ui().click(std::nullopt, at("#gain"), ui::MouseButton::Left, 2, {});
	CHECK(element(app, "#gain input").at("value") == "1.2");
	app.ui().key(std::nullopt, "Enter");
	CHECK_FALSE(has(element(app, "#gain"), "classes", "editing"));
	app.ui().click(std::nullopt, at("#gain"), ui::MouseButton::Left, 2, {});
	app.ui().click(std::nullopt, {"", 0, ui::Point{300, 400}}, ui::MouseButton::Left, 1, {});
	CHECK_FALSE(has(element(app, "#gain"), "classes", "editing"));
	CHECK(changes() == 0);
	CHECK(page.gain == 1.234F);

	// Typing the number it shows takes that number.
	app.ui().click(std::nullopt, at("#gain"), ui::MouseButton::Left, 2, {});
	app.ui().key(std::nullopt, "Mod+A");
	app.ui().text(std::nullopt, "1.2");
	app.ui().key(std::nullopt, "Enter");
	CHECK(changes() == 1);
	CHECK(page.gain == Catch::Approx(1.2F));

	// Typing and then Escape keeps the value, and the next edit starts untouched again.
	page.gain = 1.234F;
	page.model.changed("gain");
	REQUIRE(app.waitUntil([&page] { return page.value("#gain") == 1.234F; }));
	app.ui().click(std::nullopt, at("#gain"), ui::MouseButton::Left, 2, {});
	app.ui().text(std::nullopt, "7");
	app.ui().key(std::nullopt, "Escape");
	app.ui().click(std::nullopt, at("#gain"), ui::MouseButton::Left, 2, {});
	app.ui().key(std::nullopt, "Enter");
	CHECK(changes() == 1);
	CHECK(page.gain == 1.234F);
}

TEST_CASE("meters show the level and the peak in dB", "[ui]")
{
	test::UiApp app;
	const ValuesPage page(app);

	// -60 to 6 dB over 120 pixels: -12 dB is 87 pixels up, the peak at -6 dB 98 pixels up.
	const Json meter = page.box("#left");
	const int bottom = meter.at("y").get<int>() + meter.at("height").get<int>();
	const Json level = page.box("#left .meter-level");
	CHECK(level.at("height").get<int>() == 87);
	CHECK(level.at("y").get<int>() + level.at("height").get<int>() == bottom);
	const Json fill = page.box("#left .meter-fill");
	CHECK(fill.at("height").get<int>() == 120);
	CHECK(fill.at("y").get<int>() + fill.at("height").get<int>() == bottom);
	CHECK(page.box("#left .meter-peak").at("y").get<int>() == bottom - 98);

	// A meter without a level shows none; values arrive as attributes, e.g. once a frame from the engine.
	CHECK(page.box("#right .meter-level").at("height").get<int>() == 0);
	const ui::Image silent = app.ui().screenshot(std::nullopt, at("#right"));
	page.document->GetElementById("right")->SetAttribute("value", "0");
	CHECK(app.waitUntil([&page] { return page.box("#right .meter-level").at("height").get<int>() == 109; }));
	CHECK_FALSE(test::compareImages(app.ui().screenshot(std::nullopt, at("#right")), silent, {8}).equal);
}

namespace
{
	bool visible(test::UiApp& app, const std::string& selector)
	{
		return element(app, selector).at("visible").get<bool>();
	}

	ui::Target point(int x, int y)
	{
		return {"", 0, ui::Point{x, y}};
	}

	// The main window's size in pixels.
	ui::Size windowSize(test::UiApp& app)
	{
		const Json window = app.ui().windows().at("windows").at(0);
		return {window.at("width").get<int>(), window.at("height").get<int>()};
	}

	// Whether the element's border box lies inside the main window.
	bool insideWindow(test::UiApp& app, const std::string& selector)
	{
		const Json box = element(app, selector).at("box");
		const ui::Size window = windowSize(app);
		return box.at("x").get<int>() >= 0 && box.at("y").get<int>() >= 0 &&
			box.at("x").get<int>() + box.at("width").get<int>() <= window.width &&
			box.at("y").get<int>() + box.at("height").get<int>() <= window.height;
	}
}

TEST_CASE("menus open under their button and close on an item, a click elsewhere or Escape", "[ui]")
{
	test::UiApp app;
	const Events events(app.show("popups.rml"));
	CHECK_FALSE(visible(app, "#edit"));

	// The menu opens under its button; another click on the button closes it.
	click(app, "#edit-button");
	REQUIRE(visible(app, "#edit"));
	const Json button = element(app, "#edit-button").at("box");
	const Json menu = element(app, "#edit").at("box");
	CHECK(menu.at("x") == button.at("x"));
	CHECK(menu.at("y").get<int>() == button.at("y").get<int>() + button.at("height").get<int>());
	click(app, "#edit-button");
	CHECK_FALSE(visible(app, "#edit"));

	// A click on an item reaches the item and closes the menu; a disabled item does neither.
	click(app, "#edit-button");
	CHECK(has(element(app, "#redo"), "pseudoClasses", "disabled"));
	click(app, "#redo");
	CHECK(events.clicks.count("redo") == 0);
	CHECK(visible(app, "#edit"));
	click(app, "#undo");
	CHECK(events.clicks.at("undo") == 1);
	CHECK_FALSE(visible(app, "#edit"));

	click(app, "#edit-button");
	app.ui().key(std::nullopt, "Escape");
	CHECK_FALSE(visible(app, "#edit"));
	click(app, "#edit-button");
	click(app, "#plain");
	CHECK_FALSE(visible(app, "#edit"));

	// Down and Up move between the items that aren't disabled, and Enter clicks one.
	click(app, "#edit-button");
	app.ui().key(std::nullopt, "Down");
	app.ui().key(std::nullopt, "Down");
	CHECK(has(element(app, "#cut"), "pseudoClasses", "focus"));
	app.ui().key(std::nullopt, "Enter");
	CHECK(events.clicks.at("cut") == 1);
	CHECK_FALSE(visible(app, "#edit"));
}

TEST_CASE("context menus open at the mouse on a right click, inside the window, with submenus on hover", "[ui]")
{
	test::UiApp app;
	const Events events(app.show("popups.rml"));

	const Json track = element(app, "#track").at("box");
	const int x = track.at("x").get<int>() + 10;
	const int y = track.at("y").get<int>() + 10;
	app.ui().click(std::nullopt, point(x, y), ui::MouseButton::Right, 1, {});
	REQUIRE(visible(app, "#track-menu"));
	CHECK(element(app, "#track-menu").at("box").at("x") == x);
	CHECK(element(app, "#track-menu").at("box").at("y") == y);

	// The submenu shows while the mouse is over its item; a click on that item keeps the menu open.
	CHECK_FALSE(visible(app, "#colors"));
	app.ui().move(std::nullopt, at("#color"));
	CHECK(visible(app, "#colors"));
	click(app, "#color");
	CHECK(visible(app, "#track-menu"));
	click(app, "#blue");
	CHECK(events.clicks.at("blue") == 1);
	CHECK_FALSE(visible(app, "#track-menu"));

	// In the window's corner the menu opens inside the window. Without a contextmenu attribute, or with the left
	// button, none opens.
	app.ui().click(std::nullopt, at("#corner"), ui::MouseButton::Right, 1, {});
	REQUIRE(visible(app, "#track-menu"));
	CHECK(insideWindow(app, "#track-menu"));
	click(app, "#plain");
	app.ui().click(std::nullopt, at("#plain"), ui::MouseButton::Right, 1, {});
	click(app, "#track");
	CHECK_FALSE(visible(app, "#track-menu"));
}

TEST_CASE("a right click with a modifier key opens the element's modifier context menu if it has one", "[ui]")
{
	test::UiApp app;
	app.show("popups.rml");
	app.ui().click(std::nullopt, at("#modified"), ui::MouseButton::Right, 1, {});
	REQUIRE(visible(app, "#track-menu"));
	CHECK(!visible(app, "#edit"));
	app.ui().key(std::nullopt, "Escape");
	for (const char* modifier : {"ctrl", "shift", "alt", "meta"})
	{
		INFO(modifier);
		app.ui().click(std::nullopt, at("#modified"), ui::MouseButton::Right, 1, {modifier});
		CHECK(visible(app, "#edit"));
		CHECK(!visible(app, "#track-menu"));
		app.ui().key(std::nullopt, "Escape");
	}
	// Without one, a right click with a modifier opens the context menu.
	app.ui().click(std::nullopt, at("#track"), ui::MouseButton::Right, 1, {"ctrl"});
	CHECK(visible(app, "#track-menu"));
	CHECK(!visible(app, "#edit"));
}

TEST_CASE("notifications show their text for a moment, again when it is set again", "[ui]")
{
	test::UiApp app;
	Rml::ElementDocument& document = app.show("popups.rml");
	auto* clock = dynamic_cast<ui::SystemInterface*>(Rml::GetSystemInterface());
	REQUIRE(clock != nullptr);
	clock->freeze(true);
	const auto after = [&app, clock](double seconds)
	{
		clock->advance(seconds);
		app.main().context().Update();
		return visible(app, "#message");
	};
	Rml::Element* message = document.GetElementById("message");
	CHECK(!visible(app, "#message"));
	message->SetAttribute("text", "Draw");
	CHECK(after(0.0));
	CHECK(element(app, "#message").at("text") == "Draw");
	CHECK(after(0.4));
	message->SetAttribute("text", "Draw"); // its duration, half a second, starts again
	CHECK(after(0.4));
	CHECK(!after(0.2));
	clock->freeze(false);
}

namespace
{
	bool focused(test::UiApp& app, const std::string& selector)
	{
		return has(element(app, selector), "pseudoClasses", "focus");
	}
}

TEST_CASE("items with a submenu show an arrow, and Right and Left open and close the submenu", "[ui]")
{
	test::UiApp app;
	const Events events(app.show("popups.rml"));

	const Json track = element(app, "#track").at("box");
	app.ui().click(std::nullopt, point(track.at("x").get<int>() + 10, track.at("y").get<int>() + 10),
		ui::MouseButton::Right, 1, {});
	REQUIRE(visible(app, "#track-menu"));
	CHECK(app.ui().dom(std::nullopt, "#color > .menu-arrow", 0).at("elements").size() == 1);
	CHECK(app.ui().dom(std::nullopt, "#rename .menu-arrow", 0).at("elements").empty());
	CHECK(visible(app, "#color > .menu-arrow"));

	// Right on the item opens its submenu with its first item focused, Left closes it and goes back to the item.
	moveAway(app);
	app.ui().key(std::nullopt, "Down");
	app.ui().key(std::nullopt, "Down");
	REQUIRE(focused(app, "#color"));
	app.ui().key(std::nullopt, "Right");
	REQUIRE(visible(app, "#colors"));
	CHECK(focused(app, "#red"));
	app.ui().key(std::nullopt, "Down");
	CHECK(focused(app, "#blue"));
	app.ui().key(std::nullopt, "Left");
	CHECK_FALSE(visible(app, "#colors"));
	CHECK(focused(app, "#color"));
	CHECK(visible(app, "#track-menu"));

	// Right on an item without a submenu, in a menu that isn't in a menu bar, does nothing.
	app.ui().key(std::nullopt, "Up");
	app.ui().key(std::nullopt, "Right");
	CHECK(focused(app, "#rename"));
	CHECK(visible(app, "#track-menu"));

	// Nor does Right on an item whose submenu has nothing to focus.
	app.ui().key(std::nullopt, "Up");
	REQUIRE(focused(app, "#lock"));
	app.ui().key(std::nullopt, "Right");
	CHECK_FALSE(visible(app, "#locks"));
	CHECK(focused(app, "#lock"));
	app.ui().key(std::nullopt, "Down");
	REQUIRE(focused(app, "#rename"));

	// A submenu that keys opened closes when the mouse goes to another item, and with its menu.
	app.ui().key(std::nullopt, "Down");
	app.ui().key(std::nullopt, "Right");
	REQUIRE(visible(app, "#colors"));
	app.ui().move(std::nullopt, at("#rename"));
	CHECK_FALSE(visible(app, "#colors"));
	app.ui().move(std::nullopt, at("#color"));
	CHECK(visible(app, "#colors")); // under the mouse again
	moveAway(app);
	app.ui().key(std::nullopt, "Right");
	app.ui().key(std::nullopt, "Escape");
	CHECK_FALSE(visible(app, "#track-menu"));
	app.ui().click(std::nullopt, point(track.at("x").get<int>() + 10, track.at("y").get<int>() + 10),
		ui::MouseButton::Right, 1, {});
	moveAway(app);
	CHECK_FALSE(visible(app, "#colors"));
	app.ui().key(std::nullopt, "Escape");
}

TEST_CASE("Tab and Shift+Tab move between an open menu's items and don't leave the menu", "[ui]")
{
	test::UiApp app;
	const Events events(app.show("popups.rml"));

	// The document has buttons before and after the menu that Tab would otherwise reach (#524).
	const Json track = element(app, "#track").at("box");
	app.ui().click(std::nullopt, point(track.at("x").get<int>() + 10, track.at("y").get<int>() + 10),
		ui::MouseButton::Right, 1, {});
	REQUIRE(visible(app, "#track-menu"));
	moveAway(app);

	// Tab steps forwards over the disabled item and from the last shown item round to the first, past the hidden
	// one, as Down does.
	app.ui().key(std::nullopt, "Tab");
	CHECK(focused(app, "#rename"));
	app.ui().key(std::nullopt, "Tab");
	CHECK(focused(app, "#color"));
	app.ui().key(std::nullopt, "Tab");
	CHECK(focused(app, "#lock"));
	app.ui().key(std::nullopt, "Tab");
	CHECK(focused(app, "#rename"));

	// Shift+Tab steps backwards, from the first item round to the last shown one, as Up does.
	app.ui().key(std::nullopt, "Shift+Tab");
	CHECK(focused(app, "#lock"));
	app.ui().key(std::nullopt, "Down");
	CHECK(focused(app, "#rename"));
	app.ui().key(std::nullopt, "Up");
	CHECK(focused(app, "#lock"));

	// With a modifier too, round the ends.
	app.ui().key(std::nullopt, "Ctrl+Tab");
	CHECK(focused(app, "#rename"));
	app.ui().key(std::nullopt, "Alt+Shift+Tab");
	CHECK(focused(app, "#lock"));
	app.ui().key(std::nullopt, "Shift+Tab");
	CHECK(focused(app, "#color"));

	// In a submenu that keys opened, they step through the submenu's items.
	app.ui().key(std::nullopt, "Right");
	REQUIRE(focused(app, "#red"));
	app.ui().key(std::nullopt, "Tab");
	CHECK(focused(app, "#blue"));
	app.ui().key(std::nullopt, "Tab");
	CHECK(focused(app, "#red"));
	app.ui().key(std::nullopt, "Shift+Tab");
	CHECK(focused(app, "#blue"));
	CHECK(visible(app, "#colors"));
	CHECK(visible(app, "#track-menu"));

	// The menu still has its keys.
	app.ui().key(std::nullopt, "Left");
	CHECK(focused(app, "#color"));
	app.ui().key(std::nullopt, "Escape");
	CHECK(!visible(app, "#track-menu"));
}

TEST_CASE("menu Tab stops find nested controls and skip controls that cannot take Tab focus", "[ui]")
{
	test::UiApp app;
	app.show("popups.rml");
	click(app, "#controls-button");
	REQUIRE(visible(app, "#controls-menu"));
	CHECK_FALSE(visible(app, "#hidden-control"));
	moveAway(app);
	app.ui().key(std::nullopt, "Tab");
	REQUIRE(focused(app, "#controls-first"));
	// Disabled and hidden inputs are skipped; the button two containers deep is a stop.
	app.ui().key(std::nullopt, "Tab");
	CHECK(focused(app, "#nested-control"));
	// Shown inputs with tab-index: none or focus: none are skipped too.
	app.ui().key(std::nullopt, "Tab");
	CHECK(focused(app, "#controls-last"));
	app.ui().key(std::nullopt, "Tab");
	CHECK(focused(app, "#controls-first"));
	// The same stops in reverse, including wrapping, without leaving the menu.
	for (const char* id : {"#controls-last", "#nested-control", "#controls-first"})
	{
		app.ui().key(std::nullopt, "Shift+Tab");
		CHECK(focused(app, id));
		CHECK(visible(app, "#controls-menu"));
	}
	// Arrow keys continue to visit only menu items, not the nested button.
	app.ui().key(std::nullopt, "Down");
	CHECK(focused(app, "#controls-last"));
	app.ui().key(std::nullopt, "Up");
	CHECK(focused(app, "#controls-first"));
}

TEST_CASE("in a menu bar the mouse and Left and Right move between the open menus", "[ui]")
{
	test::UiApp app;
	const Events events(app.show("popups.rml"));

	// Without an open menu, the mouse over a title opens nothing.
	app.ui().move(std::nullopt, at("#view-title"));
	CHECK_FALSE(visible(app, "#view"));

	// With one open, the mouse over another title opens that title's menu instead, under it; outside the menu bar
	// the menu stays open. A disabled title opens nothing.
	click(app, "#file-title");
	REQUIRE(visible(app, "#file"));
	app.ui().move(std::nullopt, at("#view-title"));
	CHECK_FALSE(visible(app, "#file"));
	REQUIRE(visible(app, "#view"));
	CHECK(element(app, "#view").at("box").at("x") == element(app, "#view-title").at("box").at("x"));
	app.ui().move(std::nullopt, at("#tools-title"));
	CHECK(visible(app, "#view"));
	CHECK_FALSE(visible(app, "#tools"));
	app.ui().move(std::nullopt, at("#plain"));
	CHECK(visible(app, "#view"));
	app.ui().move(std::nullopt, at("#file-title"));
	CHECK(visible(app, "#file"));
	CHECK_FALSE(visible(app, "#view"));
	click(app, "#file-title");
	CHECK_FALSE(visible(app, "#file"));

	// Right and Left open the next and previous title's menu with its first item focused, skipping disabled titles
	// and going round; Right on an item with a submenu opens the submenu, and in the submenu Left goes back.
	click(app, "#file-title");
	moveAway(app);
	app.ui().key(std::nullopt, "Right");
	REQUIRE(visible(app, "#view"));
	CHECK_FALSE(visible(app, "#file"));
	CHECK(focused(app, "#zoom-in"));
	app.ui().key(std::nullopt, "Right");
	CHECK(visible(app, "#help"));
	CHECK(focused(app, "#about"));
	app.ui().key(std::nullopt, "Right");
	CHECK(visible(app, "#file"));
	CHECK(focused(app, "#new"));
	app.ui().key(std::nullopt, "Left");
	CHECK(visible(app, "#help"));
	app.ui().key(std::nullopt, "Right");
	app.ui().key(std::nullopt, "Down");
	REQUIRE(focused(app, "#recent"));
	app.ui().key(std::nullopt, "Right");
	CHECK(visible(app, "#recent-menu"));
	CHECK(focused(app, "#first-recent"));
	CHECK(visible(app, "#file"));

	// In the submenu, Right on an item without a submenu opens the next title's menu too.
	app.ui().key(std::nullopt, "Right");
	CHECK(visible(app, "#view"));
	CHECK_FALSE(visible(app, "#file"));
	app.ui().key(std::nullopt, "Left");
	REQUIRE(visible(app, "#file"));
	CHECK_FALSE(visible(app, "#recent-menu")); // closed with its menu
	app.ui().key(std::nullopt, "Down");
	app.ui().key(std::nullopt, "Right");
	REQUIRE(focused(app, "#first-recent"));
	app.ui().key(std::nullopt, "Left");
	CHECK_FALSE(visible(app, "#recent-menu"));
	CHECK(focused(app, "#recent"));
	app.ui().key(std::nullopt, "Left");
	CHECK(visible(app, "#help"));
	CHECK_FALSE(visible(app, "#file"));

	// Enter clicks the focused item and closes the menu.
	app.ui().key(std::nullopt, "Enter");
	CHECK(events.clicks.at("about") == 1);
	CHECK_FALSE(visible(app, "#help"));
}

TEST_CASE("Alt opens the menu bar's first menu on Windows and Linux, and closes it again", "[ui]")
{
	test::UiApp app;
	[[maybe_unused]] Rml::ElementDocument& document = app.show("popups.rml"); // not on macOS
	click(app, "#plain");

	app.ui().key(std::nullopt, "Alt");
#ifdef __APPLE__
	CHECK_FALSE(visible(app, "#file")); // the system menu bar has the keys there
#else
	REQUIRE(visible(app, "#file"));
	CHECK(focused(app, "#new"));
	app.ui().key(std::nullopt, "Right");
	CHECK(visible(app, "#view"));
	app.ui().key(std::nullopt, "Alt");
	CHECK_FALSE(visible(app, "#view"));
	CHECK_FALSE(focused(app, "#zoom-in")); // the focus left the closed menu

	// A context menu that is open closes.
	app.ui().click(std::nullopt, at("#track"), ui::MouseButton::Right, 1, {});
	REQUIRE(visible(app, "#track-menu"));
	app.ui().key(std::nullopt, "Alt");
	CHECK_FALSE(visible(app, "#track-menu"));
	CHECK(visible(app, "#file"));
	app.ui().key(std::nullopt, "Alt");
	CHECK_FALSE(visible(app, "#file"));

	// Alt only counts alone: not with another key or a click while it's held.
	Rml::Context& context = app.main().context();
	context.ProcessKeyDown(Rml::Input::KI_LMENU, Rml::Input::KM_ALT);
	app.ui().key(std::nullopt, "Alt+N");
	context.ProcessKeyUp(Rml::Input::KI_LMENU, 0);
	CHECK_FALSE(visible(app, "#file"));
	context.ProcessKeyDown(Rml::Input::KI_LMENU, Rml::Input::KM_ALT);
	app.ui().click(std::nullopt, at("#plain"), ui::MouseButton::Left, 1, {"alt"});
	context.ProcessKeyUp(Rml::Input::KI_LMENU, 0);
	CHECK_FALSE(visible(app, "#file"));
	context.ProcessKeyDown(Rml::Input::KI_LMENU, Rml::Input::KM_ALT | Rml::Input::KM_CTRL);
	context.ProcessKeyUp(Rml::Input::KI_LMENU, Rml::Input::KM_CTRL);
	CHECK_FALSE(visible(app, "#file"));

	// Nor when a mouse button goes up or the wheel turns while it's held: Alt during a drag copies events.
	context.ProcessMouseButtonDown(0, 0);
	context.ProcessKeyDown(Rml::Input::KI_LMENU, Rml::Input::KM_ALT);
	context.ProcessMouseButtonUp(0, Rml::Input::KM_ALT);
	context.ProcessKeyUp(Rml::Input::KI_LMENU, 0);
	CHECK_FALSE(visible(app, "#file"));
	context.ProcessKeyDown(Rml::Input::KI_LMENU, Rml::Input::KM_ALT);
	context.ProcessMouseWheel(Rml::Vector2f(0.0F, 1.0F), Rml::Input::KM_ALT);
	context.ProcessKeyUp(Rml::Input::KI_LMENU, 0);
	CHECK_FALSE(visible(app, "#file"));

	// Nor when SDL releases it because the window lost the keyboard, e.g. to Alt+Tab (the tests' hidden windows never
	// have it): the open menu stays open, and Right after it still reaches the menu.
	click(app, "#file-title");
	REQUIRE(visible(app, "#file"));
	SDL_Event event{};
	event.type = SDL_EVENT_KEY_DOWN;
	event.key.windowID = app.main().id();
	event.key.key = SDLK_LALT;
	event.key.scancode = SDL_SCANCODE_LALT;
	event.key.down = true;
	REQUIRE(SDL_PushEvent(&event));
	event.type = SDL_EVENT_KEY_UP;
	event.key.down = false;
	REQUIRE(SDL_PushEvent(&event));
	event.type = SDL_EVENT_KEY_DOWN;
	event.key.key = SDLK_RIGHT;
	event.key.scancode = SDL_SCANCODE_RIGHT;
	event.key.down = true;
	REQUIRE(SDL_PushEvent(&event));
	CHECK(app.waitUntil([&app] { return visible(app, "#view"); }));
	app.ui().key(std::nullopt, "Escape");

	// Nor when the window gets the keyboard with Alt held, e.g. by a window manager's Alt+J: SDL then presses the keys
	// held for the window (on Linux), and Alt released after that isn't Alt pressed alone. The tests' windows never
	// have the keyboard, so Alt is released directly.
	const auto pushAltDown = [&app](SDL_EventType before, Uint64 delay)
	{
		SDL_Event pushed{};
		pushed.type = before;
		pushed.window.windowID = app.main().id();
		pushed.window.timestamp = SDL_GetTicksNS();
		REQUIRE(SDL_PushEvent(&pushed));
		pushed = {};
		pushed.type = SDL_EVENT_KEY_DOWN;
		pushed.key.timestamp = SDL_GetTicksNS() + delay;
		pushed.key.windowID = app.main().id();
		pushed.key.key = SDLK_LALT;
		pushed.key.scancode = SDL_SCANCODE_LALT;
		pushed.key.mod = SDL_KMOD_LALT;
		pushed.key.down = true;
		REQUIRE(SDL_PushEvent(&pushed));
		REQUIRE(app.waitUntil(
			[] { return SDL_PeepEvents(nullptr, 0, SDL_PEEKEVENT, SDL_EVENT_WINDOW_FIRST, SDL_EVENT_KEY_UP) == 0; }));
	};
	pushAltDown(SDL_EVENT_WINDOW_FOCUS_GAINED, 0);
	context.ProcessKeyUp(Rml::Input::KI_LMENU, 0);
	CHECK_FALSE(visible(app, "#file"));
	// Alt pressed after other events, or a while after the window got the keyboard, still counts.
	pushAltDown(SDL_EVENT_WINDOW_EXPOSED, 0);
	context.ProcessKeyUp(Rml::Input::KI_LMENU, 0);
	CHECK(visible(app, "#file"));
	app.ui().key(std::nullopt, "Escape");
	pushAltDown(SDL_EVENT_WINDOW_FOCUS_GAINED, 1'000'000'000);
	context.ProcessKeyUp(Rml::Input::KI_LMENU, 0);
	CHECK(visible(app, "#file"));
	app.ui().key(std::nullopt, "Escape");

	// Nor does it open a hidden menu bar's menu.
	document.GetElementById("menubar")->SetProperty("display", "none");
	app.ui().key(std::nullopt, "Alt");
	CHECK_FALSE(visible(app, "#file"));
#endif
}

TEST_CASE("tooltips show an element's title once the mouse rests on it", "[ui]")
{
	test::UiApp app;
	app.show("popups.rml");

	app.ui().move(std::nullopt, at("#track"));
	CHECK_FALSE(visible(app, "tooltip"));
	REQUIRE(app.waitUntil([&app] { return visible(app, "tooltip"); }));
	CHECK(element(app, "tooltip").at("text") == "Audio 01");
	const Json track = element(app, "#track").at("box");
	CHECK(element(app, "tooltip").at("box").at("y").get<int>() >
		track.at("y").get<int>() + (track.at("height").get<int>() / 2));

	// Over an element without a title there's none; near the window's corner it stays inside; a click hides it.
	app.ui().move(std::nullopt, at("#plain"));
	CHECK_FALSE(visible(app, "tooltip"));
	app.ui().move(std::nullopt, at("#corner"));
	REQUIRE(app.waitUntil([&app] { return visible(app, "tooltip"); }));
	CHECK(app.waitUntil([&app] { return insideWindow(app, "tooltip"); }));
	click(app, "#corner");
	CHECK_FALSE(visible(app, "tooltip"));
}

TEST_CASE("a tooltip doesn't outlive the element it belongs to", "[ui][slow]")
{
	test::UiApp app;
	Rml::ElementDocument& document = app.show("popups.rml");
	const auto remove = [&document](const char* id)
	{
		Rml::Element* element = document.GetElementById(id);
		element->GetParentNode()->RemoveChild(element);
	};

	// Removed while the mouse rests on it, as when a list under the mouse is rendered anew: no tooltip comes.
	app.ui().move(std::nullopt, at("#track"));
	remove("track");
	CHECK_FALSE(app.waitUntil([&app] { return visible(app, "tooltip"); }, std::chrono::seconds(1)));

	// Removed while its tooltip shows: the tooltip goes with it.
	app.ui().move(std::nullopt, at("#corner"));
	REQUIRE(app.waitUntil([&app] { return visible(app, "tooltip"); }));
	remove("corner");
	CHECK(app.waitUntil([&app] { return !visible(app, "tooltip"); }));
}

TEST_CASE("an element that replaces the one under a resting mouse gets its tooltip", "[ui]")
{
	test::UiApp app;
	Rml::ElementDocument& document = app.show("popups.rml");
	Rml::ElementList tooltips;
	document.GetElementsByTagName(tooltips, "tooltip");
	REQUIRE(tooltips.size() == 1);
	const Rml::Element& tooltip = *tooltips.front();
	// Waits with the shell's steps alone: the automation's calls update the context themselves, which would find the
	// new element under the mouse and hide the problem.
	const auto showsAfterSteps = [&app, &tooltip](const char* title)
	{
		const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (std::chrono::steady_clock::now() < end)
		{
			app.shell().step(std::chrono::milliseconds(10));
			if (tooltip.IsClassSet("shown") && tooltip.GetInnerRML() == title)
				return true;
		}
		return false;
	};
	// The element with the id "track" is removed and a new one with the same id, so in the same place, and another
	// title takes its place.
	const auto replace = [&document](const char* title)
	{
		Rml::Element* old = document.GetElementById("track");
		Rml::ElementPtr created = document.CreateElement("div");
		created->SetId("track");
		created->SetClassNames("panel");
		created->SetAttribute("title", title);
		document.AppendChild(std::move(created));
		old->GetParentNode()->RemoveChild(old);
		document.GetContext()->RequestNextUpdate(0.0); // as code that changes elements does
	};

	// Replaced before the tooltip is due.
	app.ui().move(std::nullopt, at("#track"));
	replace("Audio 02");
	CHECK(showsAfterSteps("Audio 02"));

	// Replaced while its tooltip shows: the new one gets its own.
	replace("Audio 03");
	CHECK(showsAfterSteps("Audio 03"));

	// The element under the mouse is taken out and kept, as docking's parked panels are, and another with a title
	// takes its place: the same.
	Rml::Element* old = document.GetElementById("track");
	Rml::ElementPtr created = document.CreateElement("div");
	created->SetId("track");
	created->SetClassNames("panel");
	created->SetAttribute("title", "Audio 04");
	document.AppendChild(std::move(created));
	Rml::ElementPtr parked = old->GetParentNode()->RemoveChild(old);
	document.GetContext()->RequestNextUpdate(0.0);
	CHECK(showsAfterSteps("Audio 04"));
	parked.reset();
}

TEST_CASE("a tooltip asks for one more update when its element is gone, not for update after update", "[ui]")
{
	test::UiApp app;
	Rml::ElementDocument& document = app.show("popups.rml");
	Rml::Context& context = *document.GetContext();
	// The context's updates after an action: the first finds the element gone, the second is the one that asked for
	// and looks for the element under the mouse, and none asks for a third. Checked on the tooltip alone, without
	// waiting for frames: the app draws for other reasons too.
	const auto updateTwice = [&context]
	{
		context.Update();
		context.Update();
	};
	const auto asksNoMore = [&context, &updateTwice]
	{
		updateTwice();
		return context.GetNextUpdateDelay() == std::numeric_limits<double>::infinity();
	};
	const auto remove = [&document](const char* id)
	{
		Rml::Element* element = document.GetElementById(id);
		element->GetParentNode()->RemoveChild(element);
		document.GetContext()->RequestNextUpdate(0.0); // as code that changes elements does
	};

	// Removed with nothing in its place, once before the tooltip is due and once while it shows.
	app.ui().move(std::nullopt, at("#track"));
	remove("track");
	CHECK(asksNoMore());
	CHECK_FALSE(visible(app, "tooltip"));

	app.ui().move(std::nullopt, at("#corner"));
	REQUIRE(app.waitUntil([&app] { return visible(app, "tooltip"); }));
	remove("corner");
	CHECK(asksNoMore());
	CHECK_FALSE(visible(app, "tooltip"));

	// The mouse moved onto a bare part of an ancestor, where the element's tooltip went with a Mouseout and none comes.
	app.ui().move(std::nullopt, at("#titled"));
	app.ui().move(std::nullopt, point(2, 2));
	CHECK(asksNoMore());

	// An element taken out and kept: its :hover goes with no Mouseout that reaches the document.
	app.ui().move(std::nullopt, at("#titled"));
	Rml::Element* titled = document.GetElementById("titled");
	Rml::ElementPtr parked = titled->GetParentNode()->RemoveChild(titled);
	context.RequestNextUpdate(0.0);
	// The group under the mouse now has a title: its tooltip waits for its rest, without update after update.
	updateTwice();
	CHECK(context.GetNextUpdateDelay() > 0.1);
	parked.reset();
}

TEST_CASE("an element without a title that replaces the one under a resting mouse gets no tooltip", "[ui][slow]")
{
	test::UiApp app;
	Rml::ElementDocument& document = app.show("popups.rml");
	Rml::ElementList tooltips;
	document.GetElementsByTagName(tooltips, "tooltip");
	REQUIRE(tooltips.size() == 1);
	const Rml::Element& tooltip = *tooltips.front();
	const auto replaceWithoutTitle = [&document]
	{
		Rml::Element* old = document.GetElementById("track");
		Rml::ElementPtr created = document.CreateElement("div");
		created->SetId("track");
		created->SetClassNames("panel");
		document.AppendChild(std::move(created));
		old->GetParentNode()->RemoveChild(old);
		document.GetContext()->RequestNextUpdate(0.0); // as code that changes elements does
	};
	// Whether the tooltip shows at any step of a second.
	const auto showsWithin = [&app, &tooltip](std::chrono::milliseconds time)
	{
		const auto end = std::chrono::steady_clock::now() + time;
		while (std::chrono::steady_clock::now() < end)
		{
			app.shell().step(std::chrono::milliseconds(10));
			if (tooltip.IsClassSet("shown"))
				return true;
		}
		return false;
	};

	// Replaced before the tooltip is due.
	app.ui().move(std::nullopt, at("#track"));
	replaceWithoutTitle();
	CHECK_FALSE(showsWithin(std::chrono::seconds(1)));

	// Replaced while its tooltip shows: it goes and none comes. A track with a title is back for that.
	Rml::Element* track = document.GetElementById("track");
	track->SetAttribute("title", "Audio 02");
	app.ui().move(std::nullopt, at("#plain"));
	app.ui().move(std::nullopt, at("#track"));
	REQUIRE(app.waitUntil([&tooltip] { return tooltip.IsClassSet("shown"); }));
	replaceWithoutTitle();
	app.shell().step(std::chrono::milliseconds(0));
	CHECK_FALSE(showsWithin(std::chrono::seconds(1)));
	CHECK_FALSE(tooltip.IsClassSet("shown"));
}

TEST_CASE("the tooltip goes when the mouse moves to a bare part of an ancestor of its element", "[ui][slow]")
{
	test::UiApp app;
	Rml::ElementDocument& document = app.show("popups.rml");
	const ui::Target bare = point(2, 2); // the body's own area: nothing covers it

	// Before the tooltip is due: none comes. The ancestor was in the hover chain already, so RmlUi sends no Mouseover.
	app.ui().move(std::nullopt, at("#track"));
	app.ui().move(std::nullopt, bare);
	CHECK_FALSE(app.waitUntil([&app] { return visible(app, "tooltip"); }, std::chrono::seconds(1)));

	// While it shows: it goes.
	app.ui().move(std::nullopt, at("#track"));
	REQUIRE(app.waitUntil([&app] { return visible(app, "tooltip"); }));
	app.ui().move(std::nullopt, bare);
	CHECK(app.waitUntil([&app] { return !visible(app, "tooltip"); }));

	// Within an element with a title the tooltip stays: from its member to its own area, the element keeps :hover.
	app.ui().move(std::nullopt, at("#member"));
	REQUIRE(app.waitUntil([&app] { return visible(app, "tooltip"); }));
	CHECK(element(app, "tooltip").at("text") == "Group");
	const Json group = element(app, "#group").at("box");
	app.ui().move(std::nullopt,
		point(group.at("x").get<int>() + group.at("width").get<int>() - 5,
			group.at("y").get<int>() + group.at("height").get<int>() - 5));
	CHECK_FALSE(app.waitUntil([&app] { return !visible(app, "tooltip"); }, std::chrono::seconds(1)));

	// An element that is taken out from under the mouse and kept, as docking's parked panels are, takes its tooltip
	// with it: RmlUi clears its hover state, but its pointer stays valid.
	app.ui().move(std::nullopt, at("#track"));
	REQUIRE(app.waitUntil([&app] { return visible(app, "tooltip"); }));
	Rml::Element* track = document.GetElementById("track");
	Rml::ElementPtr parked = track->GetParentNode()->RemoveChild(track);
	document.GetContext()->RequestNextUpdate(0.0);
	CHECK(app.waitUntil([&app] { return !visible(app, "tooltip"); }));
	CHECK_FALSE(app.waitUntil([&app] { return visible(app, "tooltip"); }, std::chrono::seconds(1)));
	parked.reset();
}

TEST_CASE("a titled ancestor's tooltip comes after its titled member's has gone", "[ui][slow]")
{
	test::UiApp app;
	Rml::ElementDocument& document = app.show("popups.rml");
	const Json group = element(app, "#group").at("box");
	// A point of the group's own area, covered by none of its members.
	const ui::Target bare = point(group.at("x").get<int>() + group.at("width").get<int>() - 5,
		group.at("y").get<int>() + group.at("height").get<int>() - 5);
	const auto shows = [&app](const char* text)
	{
		return app.waitUntil(
			[&app, text] { return visible(app, "tooltip") && element(app, "tooltip").at("text") == text; });
	};

	// From the member onto the group's own area, which was in the hover chain already, so RmlUi sends no Mouseover:
	// the member's tooltip goes at once and the group's comes after the rest.
	app.ui().move(std::nullopt, at("#titled"));
	REQUIRE(shows("Titled"));
	app.ui().move(std::nullopt, bare);
	CHECK_FALSE(visible(app, "tooltip"));
	CHECK(shows("Group"));

	// The same when the member under the resting mouse is removed, and the tooltip comes under the mouse where it
	// rests, not where it entered the member (RmlUi sends no event for moves within an element).
	const Json box = element(app, "#titled").at("box");
	const int x = box.at("x").get<int>() + (box.at("width").get<int>() / 2);
	const int top = box.at("y").get<int>() + 1;
	const int bottom = box.at("y").get<int>() + box.at("height").get<int>() - 2;
	app.ui().move(std::nullopt, point(x, top));
	app.ui().move(std::nullopt, point(x, bottom));
	REQUIRE(shows("Titled"));
	Rml::Element* titled = document.GetElementById("titled");
	titled->GetParentNode()->RemoveChild(titled);
	document.GetContext()->RequestNextUpdate(0.0);
	CHECK(app.waitUntil([&app] { return !visible(app, "tooltip"); }));
	CHECK(shows("Group"));
	CHECK(
		app.waitUntil([&app, bottom] { return element(app, "tooltip").at("box").at("y").get<int>() >= bottom + 15; }));
}

namespace
{
	// The results of the close events of dialogs.
	struct Closes final : Rml::EventListener
	{
		void ProcessEvent(Rml::Event& event) override
		{
			results.push_back(event.GetParameter<Rml::String>("result", ""));
		}

		std::vector<std::string> results;
	};

	// Loads the dialog from tests/data/ui and shows it modal over the main window's documents.
	Rml::ElementDocument& openDialog(test::UiApp& app, Closes& closes)
	{
		Rml::ElementDocument* dialog =
			app.main().context().LoadDocument(std::string(GNUBASE_TEST_DATA_DIR) + "/ui/dialog.rml");
		REQUIRE(dialog != nullptr);
		dialog->AddEventListener(ui::g_dialogCloseEvent, &closes);
		dialog->Show(Rml::ModalFlag::Modal);
		app.shell().step(std::chrono::milliseconds(0));
		return *dialog;
	}

	bool dialogOpen(test::UiApp& app)
	{
		return !app.ui().dom(std::nullopt, "#rename-dialog", 0).at("elements").empty();
	}
}

TEST_CASE("dialogs open in the middle, move by their title bar, and close with Enter, Escape or a button", "[ui]")
{
	Closes closes; // outlives the documents it listens to
	test::UiApp app;
	app.show("popups.rml");
	openDialog(app, closes);

	const Json box = element(app, "#rename-dialog").at("box");
	const ui::Size window = windowSize(app);
	CHECK(std::abs(box.at("x").get<int>() + (box.at("width").get<int>() / 2) - (window.width / 2)) <= 1);
	CHECK(std::abs(box.at("y").get<int>() + (box.at("height").get<int>() / 2) - (window.height / 2)) <= 1);

	const Json bar = element(app, "#rename-dialog .dialog-titlebar").at("box");
	const int x = bar.at("x").get<int>() + 20;
	const int y = bar.at("y").get<int>() + 10;
	app.ui().drag(std::nullopt, point(x, y), point(x + 40, y + 30));
	const Json moved = element(app, "#rename-dialog").at("box");
	CHECK(moved.at("x").get<int>() == box.at("x").get<int>() + 40);
	CHECK(moved.at("y").get<int>() == box.at("y").get<int>() + 30);

	// Enter in the text field clicks OK, which closes the dialog with its result.
	click(app, "#name");
	app.ui().key(std::nullopt, "Enter");
	CHECK(app.waitUntil([&app] { return !dialogOpen(app); }));
	CHECK(closes.results == std::vector<std::string>{"ok"});

	openDialog(app, closes);
	app.ui().key(std::nullopt, "Escape");
	CHECK(app.waitUntil([&app] { return !dialogOpen(app); }));
	openDialog(app, closes);
	click(app, "#cancel");
	CHECK(app.waitUntil([&app] { return !dialogOpen(app); }));
	CHECK(closes.results == std::vector<std::string>{"ok", "cancel", "cancel"});
}

namespace
{
	// Closes the dialog again from its own close event, and remembers what that returned.
	struct CloseAgain final : Rml::EventListener
	{
		void ProcessEvent(Rml::Event& event) override
		{
			if (auto* document = dynamic_cast<Rml::ElementDocument*>(event.GetCurrentElement()))
				results.push_back(ui::closeDialog(*document, "again"));
		}

		std::vector<bool> results;
	};
}

TEST_CASE("the app closes a dialog with a result as its buttons do, and only once", "[ui]")
{
	Closes closes;
	CloseAgain again;
	test::UiApp app;
	Rml::ElementDocument& page = app.show("popups.rml");
	CHECK(!ui::closeDialog(page, "cancel")); // no <dialog> in it
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(page.IsVisible());

	// Closing already, also while its close event is on its way, it doesn't close again.
	Rml::ElementDocument& dialog = openDialog(app, closes);
	dialog.AddEventListener(ui::g_dialogCloseEvent, &again);
	CHECK(ui::closeDialog(dialog, "discard"));
	CHECK(again.results == std::vector<bool>{false});
	CHECK(!ui::closeDialog(dialog, "cancel"));
	CHECK(app.waitUntil([&app] { return !dialogOpen(app); }));
	CHECK(closes.results == std::vector<std::string>{"discard"});
}

TEST_CASE("Alt leaves a menu bar behind a modal dialog closed", "[ui]")
{
	Closes closes;
	test::UiApp app;
	app.show("popups.rml");
	openDialog(app, closes);
	app.ui().key(std::nullopt, "Alt");
	CHECK_FALSE(visible(app, "#file"));
	app.ui().key(std::nullopt, "Escape");
	CHECK(app.waitUntil([&app] { return !dialogOpen(app); }));
	CHECK(closes.results == std::vector<std::string>{"cancel"});
}

namespace
{
	// The results of the count dialog's close events, with the count it had.
	struct CountCloses final : Rml::EventListener
	{
		void ProcessEvent(Rml::Event& event) override
		{
			results.push_back(event.GetParameter<Rml::String>("result", ""));
			const Rml::Element* count = event.GetTargetElement()->GetOwnerDocument()->GetElementById("count");
			counts.push_back(count != nullptr ? count->GetAttribute<Rml::String>("value", "") : "");
		}

		std::vector<std::string> results;
		std::vector<std::string> counts;
	};

	// Shows the count dialog from tests/data/ui, without the update that follows.
	void openCountDialog(test::UiApp& app, CountCloses& closes)
	{
		Rml::ElementDocument* dialog =
			app.main().context().LoadDocument(std::string(GNUBASE_TEST_DATA_DIR) + "/ui/count-dialog.rml");
		REQUIRE(dialog != nullptr);
		dialog->AddEventListener(ui::g_dialogCloseEvent, &closes);
		dialog->Show(Rml::ModalFlag::Modal);
	}

	bool countDialogOpen(test::UiApp& app)
	{
		return !app.ui().dom(std::nullopt, "#count-dialog", 0).at("elements").empty();
	}
}

TEST_CASE(
	"a value field with autofocus is edited as its dialog opens, and Enter takes the number and clicks OK", "[ui]")
{
	CountCloses closes; // outlives the documents it listens to
	test::UiApp app;
	app.show("popups.rml");

	// Keys typed before the next update, as while the dialog's document loads, still go into the field.
	openCountDialog(app, closes);
	Rml::Context& context = app.main().context();
	context.ProcessKeyDown(Rml::Input::KI_7, 0);
	context.ProcessTextInput("7");
	context.ProcessKeyDown(Rml::Input::KI_RETURN, 0);
	CHECK(app.waitUntil([&app] { return !countDialogOpen(app); }));
	CHECK(closes.results == std::vector<std::string>{"ok"});
	CHECK(closes.counts == std::vector<std::string>{"7"});

	// Escape while it is edited cancels the dialog.
	openCountDialog(app, closes);
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(has(element(app, "#count"), "classes", "editing"));
	app.ui().key(std::nullopt, "Escape");
	CHECK(app.waitUntil([&app] { return !countDialogOpen(app); }));
	CHECK(closes.results == std::vector<std::string>{"ok", "cancel"});

	// A value field without autofocus keeps Enter while it is edited. A click on the one with autofocus doesn't edit
	// it, and Enter, also the numpad's, still clicks OK.
	openCountDialog(app, closes);
	app.shell().step(std::chrono::milliseconds(0));
	app.ui().click(std::nullopt, at("#other"), ui::MouseButton::Left, 2, {});
	app.ui().text(std::nullopt, "5");
	app.ui().key(std::nullopt, "Enter");
	REQUIRE(countDialogOpen(app));
	CHECK(element(app, "#other").at("attributes").at("value") == "5");
	CHECK(!has(element(app, "#other"), "classes", "editing"));
	click(app, "#count");
	CHECK(!has(element(app, "#count"), "classes", "editing"));
	app.ui().key(std::nullopt, "NumEnter");
	CHECK(app.waitUntil([&app] { return !countDialogOpen(app); }));
	CHECK(closes.results == std::vector<std::string>{"ok", "cancel", "ok"});
	CHECK(closes.counts == std::vector<std::string>{"7", "1", "1"});
}

namespace
{
	// The page of views at dp ratio 1, with its data model.
	struct ViewsPage
	{
		explicit ViewsPage(test::UiApp& app) : model(app.main().context(), "views")
		{
			REQUIRE(model.constructor().Bind("selection", &selection));
			document = &app.show("views.rml");
			events = std::make_unique<Events>(*document);
			app.setDpRatio(1.0F);
		}

		Rml::String selection;
		ui::DataModel model;
		Rml::ElementDocument* document = nullptr;
		std::unique_ptr<Events> events;
	};

	bool selected(test::UiApp& app, const std::string& selector)
	{
		return has(element(app, selector), "classes", "selected");
	}

	void ctrlClick(test::UiApp& app, const std::string& selector, const std::string& modifier)
	{
		app.ui().click(std::nullopt, at(selector), ui::MouseButton::Left, 1, {modifier});
	}

	// A key or a click sent to the element itself, before the next update: input from the automation updates the
	// context first.
	void keyNow(Rml::Element& element, Rml::Input::KeyIdentifier key, bool shift = false)
	{
		Rml::Dictionary parameters;
		parameters["key_identifier"] = static_cast<int>(key);
		parameters["shift_key"] = shift;
		element.DispatchEvent(Rml::EventId::Keydown, parameters);
	}

	void clickNow(Rml::Element& element, const char* modifier = nullptr)
	{
		Rml::Dictionary parameters;
		parameters["button"] = 0;
		if (modifier != nullptr)
			parameters[modifier] = true;
		element.DispatchEvent(Rml::EventId::Click, parameters);
	}

	// The element's id, which the test gives every row it makes.
	Rml::Element& byId(const ViewsPage& page, const std::string& id)
	{
		Rml::Element* element = page.document->GetElementById(id);
		REQUIRE(element != nullptr);
		return *element;
	}

	// Appends a row for rebuilt rows: a <listitem> with the prefix and the name as its id, the value and further
	// attributes, and the name as its text.
	void appendRow(std::string& rml, const std::string& prefix, const std::string& name, const std::string& value,
		const std::string& attributes = {})
	{
		rml += R"(<listitem id=")";
		rml += prefix;
		rml += name;
		rml += R"(" value=")";
		rml += value;
		rml += R"(")";
		rml += attributes;
		rml += ">";
		rml += name;
		rml += "</listitem>";
	}

	// Rebuilds the tracks list's rows as they were, with new elements whose ids have the prefix, as a panel does.
	void rebuildTracks(const ViewsPage& page, const std::string& prefix)
	{
		std::string rml;
		for (const auto& [name, value] : std::array<std::pair<const char*, const char*>, 4>{
				 {{"drums", "1"}, {"bass", "2"}, {"keys", "3"}, {"vocals", "4"}}})
			appendRow(rml, prefix, name, value);
		byId(page, "tracks").SetInnerRML(rml);
	}

	// The tracks list's value attribute.
	std::string tracksValue(const ViewsPage& page)
	{
		return byId(page, "tracks").GetAttribute<Rml::String>("value", "");
	}
}

TEST_CASE("tabs show the panel of the tab clicked", "[ui]")
{
	test::UiApp app;
	const ViewsPage page(app);
	CHECK(has(element(app, "#general"), "pseudoClasses", "selected"));
	CHECK(visible(app, "#general-panel"));
	CHECK_FALSE(visible(app, "#routing-panel"));

	click(app, "#routing");
	CHECK(has(element(app, "#routing"), "pseudoClasses", "selected"));
	CHECK(visible(app, "#routing-panel"));
	CHECK_FALSE(visible(app, "#general-panel"));
}

TEST_CASE("list views select rows with clicks and keys, and data-value binds the selection", "[ui]")
{
	test::UiApp app;
	ViewsPage page(app);

	click(app, "#bass");
	CHECK(selected(app, "#bass"));
	CHECK(page.selection == "2");
	REQUIRE(!page.events->changes.empty());
	CHECK(page.events->changes.back().first == "tracks");
	CHECK(page.events->changes.back().second.Get<Rml::String>() == "2");

	// Ctrl-click adds and removes a row, Shift-click selects a range, a plain click one row again.
	ctrlClick(app, "#vocals", "ctrl");
	CHECK(page.selection == "2,4");
	ctrlClick(app, "#bass", "ctrl");
	CHECK(page.selection == "4");
	ctrlClick(app, "#vocals", "shift");
	CHECK(page.selection == "2,3,4");
	click(app, "#drums");
	CHECK(page.selection == "1");

	// Down and Up move the selection, Shift extends it, End goes to the last row.
	app.ui().key(std::nullopt, "Down");
	CHECK(page.selection == "2");
	app.ui().key(std::nullopt, "Shift+Down");
	CHECK(page.selection == "2,3");
	app.ui().key(std::nullopt, "End");
	CHECK(page.selection == "4");
	app.ui().key(std::nullopt, "Up");
	CHECK(page.selection == "3");
	// Page Up and Page Down move by the rows the view shows, here all of them.
	app.ui().key(std::nullopt, "PageUp");
	CHECK(page.selection == "1");
	app.ui().key(std::nullopt, "PageDown");
	CHECK(page.selection == "4");

	// The data model selects rows, e.g. after an engine event.
	page.selection = "1,3";
	page.model.changed("selection");
	CHECK(app.waitUntil(
		[&app] { return selected(app, "#drums") && selected(app, "#keys") && !selected(app, "#vocals"); }));

	// The keys then move from the first row the data model selected, not from the row last clicked or keyed.
	page.selection = "1";
	page.model.changed("selection");
	CHECK(app.waitUntil([&app] { return selected(app, "#drums") && !selected(app, "#keys"); }));
	app.ui().key(std::nullopt, "Down");
	CHECK(page.selection == "2");
}

TEST_CASE("tree views show the children of expanded rows, toggled by click or by the arrow keys", "[ui]")
{
	test::UiApp app;
	const ViewsPage page(app);

	// Project is expanded, Audio isn't: its files are hidden. Children are indented by their level.
	CHECK(visible(app, "#audio"));
	CHECK_FALSE(visible(app, "#kick"));
	CHECK(has(element(app, "#audio"), "classes", "parent"));
	CHECK_FALSE(has(element(app, "#midi"), "classes", "parent"));
	CHECK(element(app, "#audio .tree-indent").at("box").at("width") == 16);

	click(app, "#audio .tree-toggle");
	CHECK(visible(app, "#kick"));
	CHECK(element(app, "#kick .tree-indent").at("box").at("width") == 32);
	CHECK_FALSE(selected(app, "#audio"));

	// A click selects; Left collapses the selected row and Right expands it; Down skips hidden rows.
	click(app, "#audio");
	app.ui().key(std::nullopt, "Left");
	CHECK_FALSE(visible(app, "#kick"));
	app.ui().key(std::nullopt, "Down");
	CHECK(selected(app, "#midi"));
	click(app, "#audio");
	app.ui().key(std::nullopt, "Right");
	CHECK(visible(app, "#snare"));
	CHECK(element(app, "#audio").at("attributes").contains("expanded"));
}

TEST_CASE("table views select their body's rows", "[ui]")
{
	test::UiApp app;
	const ViewsPage page(app);

	click(app, "#row-verse");
	CHECK(selected(app, "#row-verse"));
	CHECK(element(app, "#table").at("attributes").at("value") == "verse");
	click(app, "#header");
	CHECK_FALSE(selected(app, "#header"));
	CHECK(selected(app, "#row-verse"));
}

TEST_CASE("table views with several tables go through their rows in the document's order", "[ui]")
{
	test::UiApp app;
	const ViewsPage page(app);
	// The first table is deeper than the second: the rows' order isn't the order of their depths.
	page.document->GetElementById("table")->SetInnerRML(
		R"(<div><table><tbody><tr id="first" value="first"><td>First</td></tr>)"
		R"(<tr id="second" value="second"><td>Second</td></tr></tbody></table></div>)"
		R"(<table><tbody><tr id="third" value="third"><td>Third</td></tr></tbody></table>)");

	click(app, "#first");
	app.ui().key(std::nullopt, "Down");
	CHECK(selected(app, "#second"));
	app.ui().key(std::nullopt, "Down");
	CHECK(selected(app, "#third"));
	app.ui().key(std::nullopt, "Home");
	CHECK(selected(app, "#first"));
	app.ui().key(std::nullopt, "End");
	CHECK(selected(app, "#third"));
	CHECK(element(app, "#table").at("attributes").at("value") == "third");
}

TEST_CASE("list views with dragscroll scroll while a drag in them is held past their edge", "[ui]")
{
	test::UiApp app;
	const ViewsPage page(app);
	Rml::Element* view = page.document->GetElementById("table");
	REQUIRE(view != nullptr);
	std::string rml = R"(<div id="handle" style="drag: drag; height: 10dp;"/><table><tbody>)";
	for (int i = 0; i < 40; ++i)
		rml += R"(<tr value=")" + std::to_string(i) + R"("><td>Row</td></tr>)";
	view->SetInnerRML(rml + "</tbody></table>");
	view->SetAttribute("dragscroll", "dragscroll");
	const Json handle = element(app, "#handle").at("box");
	const Json box = element(app, "#table").at("box");
	const int x = handle.at("x").get<int>() + 5;
	const int y = handle.at("y").get<int>() + 5;
	const int top = box.at("y").get<int>();
	const int bottom = top + box.at("height").get<int>();
	Rml::Context& context = app.main().context();
	const auto end = [view] { return view->GetScrollHeight() - view->GetClientHeight(); };
	REQUIRE(end() > 0.0F);

	// The mouse held still below the view scrolls it down to its end, above it back up.
	context.ProcessMouseMove(x, y, 0);
	context.ProcessMouseButtonDown(0, 0);
	context.ProcessMouseMove(x, y + 2, 0);
	context.ProcessMouseMove(x, bottom + 60, 0);
	CHECK(app.waitUntil([&] { return view->GetScrollTop() >= end(); }));
	context.ProcessMouseMove(x, top - 60, 0);
	CHECK(app.waitUntil([view] { return view->GetScrollTop() <= 0.0F; }));
	context.ProcessMouseMove(x, bottom + 60, 0);
	context.ProcessMouseButtonUp(0, 0);
	app.waitUntil([] { return false; }, std::chrono::milliseconds(200));
	const float released = view->GetScrollTop();
	CHECK(released < end()); // the drag ended before it got there
	app.waitUntil([] { return false; }, std::chrono::milliseconds(200));
	CHECK(view->GetScrollTop() == released);

	// Without dragscroll, the view stays where it is.
	view->RemoveAttribute("dragscroll");
	view->SetScrollTop(0.0F);
	context.ProcessMouseMove(x, y, 0);
	context.ProcessMouseButtonDown(0, 0);
	context.ProcessMouseMove(x, y + 2, 0);
	context.ProcessMouseMove(x, bottom + 60, 0);
	app.waitUntil([] { return false; }, std::chrono::milliseconds(300));
	CHECK(view->GetScrollTop() == 0.0F);
	context.ProcessMouseButtonUp(0, 0);
}

TEST_CASE("table views keep the selection of rows rebuilt twice between two updates", "[ui]")
{
	test::UiApp app;
	const ViewsPage page(app);
	click(app, "#row-intro");
	REQUIRE(selected(app, "#row-intro"));

	// As a panel that rebuilds its rows whenever their number changes may: first with another number of rows, then
	// with as many as before and the same value, before the view updates. The rows it knew are gone.
	Rml::Element& body = *page.document->GetElementById("table-body");
	body.SetInnerRML(R"(<tr value="intro"><td>Intro</td><td>8 bars</td></tr>)");
	body.SetInnerRML(R"(<tr id="new-intro" value="intro"><td>Intro</td><td>8 bars</td></tr>)"
					 R"(<tr id="new-verse" value="verse"><td>Verse</td><td>16 bars</td></tr>)");
	CHECK(app.waitUntil([&app] { return selected(app, "#new-intro"); }));

	// The keys move from the new row, not from the one that went.
	app.ui().key(std::nullopt, "Down");
	CHECK(app.waitUntil([&app] { return selected(app, "#new-verse"); }));
	CHECK(!selected(app, "#new-intro"));
	CHECK(element(app, "#table").at("attributes").at("value") == "verse");

	// Rebuilt again, and a key comes before the view updates (sent to the view itself: input from the automation
	// updates the context first): it moves from the new row too, so Down stays on the last one.
	body.SetInnerRML(R"(<tr value="verse"><td>Verse</td><td>16 bars</td></tr>)");
	body.SetInnerRML(R"(<tr id="last-intro" value="intro"><td>Intro</td><td>8 bars</td></tr>)"
					 R"(<tr id="last-verse" value="verse"><td>Verse</td><td>16 bars</td></tr>)");
	keyNow(byId(page, "table"), Rml::Input::KI_DOWN);
	CHECK(selected(app, "#last-verse"));
	CHECK(!selected(app, "#last-intro"));
}

TEST_CASE("table views select among rows rebuilt twice between two updates when none was selected", "[ui]")
{
	test::UiApp app;
	const ViewsPage page(app);
	click(app, "#row-intro");
	page.document->GetElementById("table")->SetAttribute("value", "chorus"); // no row has it yet
	CHECK(app.waitUntil([&app] { return !selected(app, "#row-intro"); }));

	Rml::Element& body = *page.document->GetElementById("table-body");
	body.SetInnerRML(R"(<tr value="intro"><td>Intro</td><td>8 bars</td></tr>)");
	body.SetInnerRML(R"(<tr id="new-intro" value="intro"><td>Intro</td><td>8 bars</td></tr>)"
					 R"(<tr id="new-chorus" value="chorus"><td>Chorus</td><td>8 bars</td></tr>)");
	CHECK(app.waitUntil([&app] { return selected(app, "#new-chorus"); }));
	CHECK(!selected(app, "#new-intro"));
}

TEST_CASE("tree views shape rows rebuilt before a key, so the keys skip hidden rows and expand and collapse", "[ui]")
{
	test::UiApp app;
	const ViewsPage page(app);
	Rml::Element& tree = byId(page, "tree");
	click(app, "#audio");
	REQUIRE(tree.GetAttribute<Rml::String>("value", "") == "audio");
	// Rebuilds the tree's rows as a panel does: the same rows with new elements, whose ids have the prefix, so with as
	// many rows and the same value. Project is expanded, Audio as asked.
	const auto rebuild = [&tree](const std::string& prefix, bool audioExpanded)
	{
		std::string rml;
		for (const auto& [name, level] : std::array<std::pair<std::string, const char*>, 5>{
				 {{"project", "0"}, {"audio", "1"}, {"kick", "2"}, {"snare", "2"}, {"midi", "1"}}})
		{
			std::string attributes = R"( level=")";
			attributes += level;
			attributes += name == "project" || (name == "audio" && audioExpanded) ? R"(" expanded="expanded")" : R"(")";
			appendRow(rml, prefix, name, name, attributes);
		}
		tree.SetInnerRML(rml);
	};

	// Down before the next update skips the collapsed Audio's children.
	rebuild("a-", false);
	keyNow(tree, Rml::Input::KI_DOWN);
	CHECK(byId(page, "a-midi").IsClassSet("selected"));
	CHECK(!byId(page, "a-kick").IsClassSet("selected"));
	CHECK(byId(page, "a-kick").IsClassSet("hidden"));
	CHECK(tree.GetAttribute<Rml::String>("value", "") == "midi");

	// Right expands the selected row, Left collapses it, both before the next update.
	click(app, "#a-audio");
	rebuild("b-", false);
	keyNow(tree, Rml::Input::KI_RIGHT);
	CHECK(byId(page, "b-audio").HasAttribute("expanded"));
	CHECK(!byId(page, "b-kick").IsClassSet("hidden"));
	rebuild("c-", true);
	keyNow(tree, Rml::Input::KI_LEFT);
	CHECK(!byId(page, "c-audio").HasAttribute("expanded"));
	CHECK(byId(page, "c-kick").IsClassSet("hidden"));
	CHECK(app.waitUntil([&app] { return !visible(app, "#c-kick") && selected(app, "#c-audio"); }));
}

TEST_CASE("multiple list views extend the selection from rows rebuilt before a key or a click", "[ui]")
{
	test::UiApp app;
	const ViewsPage page(app);
	click(app, "#bass");
	REQUIRE(tracksValue(page) == "2");

	// Shift+Down before the next update extends from the new Bass.
	rebuildTracks(page, "a-");
	keyNow(byId(page, "tracks"), Rml::Input::KI_DOWN, true);
	CHECK(tracksValue(page) == "2,3");
	CHECK(byId(page, "a-bass").IsClassSet("selected"));
	CHECK(byId(page, "a-keys").IsClassSet("selected"));

	// So does a Shift-click, and Ctrl-click adds to the rebuilt selection.
	click(app, "#a-bass");
	rebuildTracks(page, "b-");
	clickNow(byId(page, "b-vocals"), "shift_key");
	CHECK(tracksValue(page) == "2,3,4");
	rebuildTracks(page, "c-");
	clickNow(byId(page, "c-drums"), "ctrl_key");
	CHECK(tracksValue(page) == "1,2,3,4");

	// The next update leaves the keys at the row last clicked in the rebuilt rows, rather than moving them to the
	// first selected row: Up goes from Vocals to Keys.
	rebuildTracks(page, "d-");
	clickNow(byId(page, "d-keys"));
	clickNow(byId(page, "d-vocals"), "ctrl_key");
	REQUIRE(tracksValue(page) == "3,4");
	app.ui().key(std::nullopt, "Up"); // the automation updates the context first
	CHECK(tracksValue(page) == "3");
}

TEST_CASE("multiple list views with shiftdeselects deselect a selected row with Shift-click", "[ui]")
{
	test::UiApp app;
	const ViewsPage page(app);
	Rml::Element& tracks = byId(page, "tracks");
	click(app, "#drums");
	clickNow(byId(page, "keys"), "shift_key");
	REQUIRE(tracksValue(page) == "1,2,3");

	// Without the attribute, Shift-click on a selected row selects the range to it.
	clickNow(byId(page, "bass"), "shift_key");
	CHECK(tracksValue(page) == "1,2");

	// With it, Shift-click on a selected row deselects it and leaves the others selected, and on a row that isn't
	// selected still selects the range from the last one clicked.
	tracks.SetAttribute("shiftdeselects", "");
	clickNow(byId(page, "keys"), "shift_key");
	REQUIRE(tracksValue(page) == "1,2,3");
	clickNow(byId(page, "bass"), "shift_key");
	CHECK(tracksValue(page) == "1,3");
	CHECK(!byId(page, "bass").IsClassSet("selected"));
	clickNow(byId(page, "vocals"), "shift_key");
	CHECK(tracksValue(page) == "1,2,3,4");
}

TEST_CASE("list views move from a value that code set before a key or a click", "[ui]")
{
	test::UiApp app;
	const ViewsPage page(app);
	Rml::Element& tracks = byId(page, "tracks");
	click(app, "#drums");

	// Down moves from the row the code selected, not from the one clicked.
	tracks.SetAttribute("value", "3");
	keyNow(tracks, Rml::Input::KI_DOWN);
	CHECK(tracksValue(page) == "4");

	// Shift-click selects the range from it.
	tracks.SetAttribute("value", "1");
	clickNow(byId(page, "keys"), "shift_key");
	CHECK(tracksValue(page) == "1,2,3");
}

TEST_CASE("list views select a row that code replaced on its own, with as many rows and the same value", "[ui]")
{
	test::UiApp app;
	const ViewsPage page(app);
	click(app, "#vocals");
	ctrlClick(app, "#bass", "ctrl"); // the keys move from Bass
	REQUIRE(tracksValue(page) == "2,4");

	// The second of the two selected rows goes, neither the first row nor the keys' row, and a new one with its value
	// takes its place.
	Rml::Element& tracks = byId(page, "tracks");
	Rml::ElementPtr row = page.document->CreateElement("listitem");
	row->SetId("new-vocals");
	row->SetAttribute("value", "4");
	row->SetInnerRML("Vocals");
	// Outside REQUIRE, whose do-while makes the analyzer see the move as repeated. The old row is destroyed.
	const bool replaced = tracks.ReplaceChild(std::move(row), &byId(page, "vocals")) != nullptr;
	REQUIRE(replaced);
	CHECK(app.waitUntil([&app] { return selected(app, "#new-vocals"); }));
	CHECK(selected(app, "#bass"));
	CHECK(tracksValue(page) == "2,4");
}

TEST_CASE("splitters move the boundary between the elements of a row or a column", "[ui]")
{
	test::UiApp app;
	const ViewsPage page(app);
	CHECK_FALSE(has(element(app, "#split"), "classes", "column"));
	CHECK(has(element(app, "#split-rows"), "classes", "column"));

	const Json split = element(app, "#split").at("box");
	const int x = split.at("x").get<int>() + (split.at("width").get<int>() / 2);
	const int y = split.at("y").get<int>() + (split.at("height").get<int>() / 2);
	app.ui().drag(std::nullopt, point(x, y), point(x + 40, y));
	CHECK(element(app, "#tracks").at("box").at("width") == 190);
	REQUIRE(!page.events->changes.empty());
	CHECK(page.events->changes.back().first == "split");
	CHECK(page.events->changes.back().second.Get<float>() == Catch::Approx(190.0F));

	// Not smaller than nothing.
	app.ui().drag(std::nullopt, point(x + 40, y), point(x - 400, y));
	CHECK(element(app, "#tracks").at("box").at("width") == 0);

	const Json rows = element(app, "#split-rows").at("box");
	const int rowsX = rows.at("x").get<int>() + (rows.at("width").get<int>() / 2);
	const int rowsY = rows.at("y").get<int>() + (rows.at("height").get<int>() / 2);
	app.ui().drag(std::nullopt, point(rowsX, rowsY), point(rowsX, rowsY - 20));
	CHECK(element(app, "#tree").at("box").at("height") == 108); // 120 dp, padding 4 dp above and below
}

namespace
{
	// Whether the middle of the element looks like the color, within 2 of 255 a channel.
	bool looksLike(test::UiApp& app, const std::string& selector, std::array<int, 3> color)
	{
		const ui::Image image = app.ui().screenshot(std::nullopt, at(selector));
		const std::array<uint8_t, 4> pixel = image.pixel(image.width / 2, image.height / 2);
		for (size_t i = 0; i < color.size(); ++i)
			if (std::abs(static_cast<int>(pixel[i]) - color[i]) > 2)
				return false;
		return true;
	}

	// The color picker's value field in that row: 1 to 6 for H, S, V, R, G and B.
	std::string colorField(int row)
	{
		return "#picker .color-value:nth-child(" + std::to_string(row) + ") valuefield";
	}
}

TEST_CASE(
	"color pickers pick from the field and the hue strip, take numbers, and show the old and the new color", "[ui]")
{
	test::UiApp app;
	Rml::String color = "#ff0000";
	ui::DataModel model(app.main().context(), "color");
	REQUIRE(model.constructor().Bind("color", &color));
	const Events events(app.show("color.rml"));
	app.setDpRatio(1.0F);
	const auto number = [&app](int row)
	{ return std::stof(element(app, colorField(row)).at("attributes").at("value").get<std::string>()); };
	CHECK(number(1) == 0.0F);
	CHECK(number(2) == 100.0F);
	CHECK(number(4) == 255.0F);
	CHECK(looksLike(app, "#picker .color-new", {255, 0, 0}));

	// The field picks saturation from left to right and value from bottom to top: a quarter in, halfway down.
	const Json field = element(app, "#picker .color-field").at("box");
	app.ui().click(std::nullopt,
		point(field.at("x").get<int>() + (field.at("width").get<int>() / 4),
			field.at("y").get<int>() + (field.at("height").get<int>() / 2)),
		ui::MouseButton::Left, 1, {});
	CHECK(color == "#806060");
	REQUIRE(!events.changes.empty());
	CHECK(events.changes.back().first == "picker");
	CHECK(events.changes.back().second.Get<Rml::String>() == "#806060");

	// The hue strip picks the hue from top to bottom: halfway is cyan.
	const Json hue = element(app, "#picker .color-hue").at("box");
	const int x = hue.at("x").get<int>() + (hue.at("width").get<int>() / 2);
	app.ui().drag(std::nullopt, point(x, hue.at("y").get<int>() + 10),
		point(x, hue.at("y").get<int>() + (hue.at("height").get<int>() / 2)));
	CHECK(color == "#608080");
	CHECK(number(1) == 180.0F);

	// The numbers: red to 255.
	app.ui().click(std::nullopt, at(colorField(4)), ui::MouseButton::Left, 2, {});
	app.ui().key(std::nullopt, "Mod+A");
	app.ui().text(std::nullopt, "255");
	app.ui().key(std::nullopt, "Enter");
	CHECK(color == "#ff8080");
	CHECK(number(1) == 0.0F);

	// The swatches show the color the picker started with and the new one.
	CHECK(looksLike(app, "#picker .color-old", {255, 0, 0}));
	CHECK(looksLike(app, "#picker .color-new", {255, 128, 128}));

	// The data model sets the color, e.g. a track's.
	color = "#0000ff";
	model.changed("color");
	CHECK(app.waitUntil([&number] { return number(6) == 255.0F && number(1) == 240.0F; }));
	CHECK(looksLike(app, "#picker .color-new", {0, 0, 255}));
}
