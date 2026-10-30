// SPDX-License-Identifier: GPL-3.0-or-later
// UI tests of the widget library (#54, ui/widgets.h): each widget in GNUbase's UI as the app shows it, driven with
// ui::Automation as MCP's UI tools drive it (tests/uiapp.h), and read back from the DOM, from its events and from
// data models bound to it.
#include "uiapp.h"

#include "ui/automation.h"
#include "ui/datamodel.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/EventListener.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
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
		app.ui().key(std::nullopt, "Ctrl+A");
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
