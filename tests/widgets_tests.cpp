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
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
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

	// The clicks and changes that reach the document, by the id of the element that got them, or of its nearest
	// ancestor with one.
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
			while (element != nullptr && element->GetId().empty())
				element = element->GetParentNode();
			const std::string id = element != nullptr ? element->GetId() : std::string();
			if (event == Rml::EventId::Click)
			{
				++clicks[id];
			}
			else
			{
				const auto value = event.GetParameters().find("value");
				changes.emplace_back(id, value != event.GetParameters().end() ? value->second : Rml::Variant());
			}
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
