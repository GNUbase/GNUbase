// SPDX-License-Identifier: GPL-3.0-or-later
// Lua in the UI's documents (rule 8): the handlers a document registers with gnubase.on run in the app's step, and the
// windows draw what they changed without input (#715, #745).
#include "uiapp.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstdint>
#include <string>

using namespace gnubase;

TEST_CASE("a document's Lua event handler draws what it changed in the step the event arrives in", "[ui]")
{
	test::UiApp app;
	Rml::ElementDocument& document = app.show("lua-event.rml");
	Rml::Element* resets = document.GetElementById("resets");
	REQUIRE(resets != nullptr);
	for (int i = 0; i < 10; ++i)
		app.shell().step(std::chrono::milliseconds(0)); // what opening the document changed

	// No input from here on: another client resets the meters, an event that only the document's handler takes, so
	// nothing but the handler can make the window draw.
	app.call("meter.reset");
	bool counted = false;
	uint64_t drawn = 0;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (!counted && std::chrono::steady_clock::now() < deadline)
	{
		const uint64_t frames = app.main().frames();
		app.shell().step(std::chrono::milliseconds(20));
		counted = resets->GetInnerRML() == "1";
		drawn = app.main().frames() - frames;
	}
	REQUIRE(counted);
	CHECK(drawn == 1);
}
