// SPDX-License-Identifier: GPL-3.0-or-later
// The list editor's side of the engine (#130, docs/features/list-editor.md): the editor views' content, key or list,
// and the selected controller events. tests/listeditor_ui_tests.cpp drives the list editor in the app.
#include "engine/engine.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <utility>

using Catch::Matchers::ContainsSubstring;
using namespace gnubase;
using engine::Json;

namespace
{
	struct Fixture
	{
		Fixture()
		{
			const uint64_t track = call("track.add", {{"type", "midi"}}).at("track").at("id").get<uint64_t>();
			part = call("midiPart.add", {{"track", track}, {"position", 0}, {"length", 1920}})
					   .at("part")
					   .at("id")
					   .get<uint64_t>();
			other = call("midiPart.add", {{"track", track}, {"position", 1920}, {"length", 1920}})
						.at("part")
						.at("id")
						.get<uint64_t>();
			const Json added = call("controller.add",
				{{"part", part},
					{"controllers",
						Json::array({{{"position", 0}, {"number", 7}, {"value", 0.5}},
							{{"position", 480}, {"type", "sysEx"}, {"data", {0xF0, 0x41, 0xF7}}}})}});
			volume = added.at("controllers").at(0).at("id").get<uint64_t>();
			sysEx = added.at("controllers").at(1).at("id").get<uint64_t>();
		}

		Json call(const std::string& command, Json parameters = Json::object())
		{
			const engine::Result result = client.call(command, std::move(parameters));
			INFO(command << " failed: " << result.error);
			REQUIRE(result.ok());
			return result.value;
		}

		std::string fails(const std::string& command, Json parameters)
		{
			const engine::Result result = client.call(command, std::move(parameters));
			REQUIRE(!result.ok());
			return result.error;
		}

		Json selected(uint64_t controller) const { return {{"part", part}, {"controller", controller}}; }

		engine::Engine engine;
		engine::Client client{engine};
		uint64_t part = 0;
		uint64_t other = 0;
		uint64_t volume = 0;
		uint64_t sysEx = 0;
	};
}

TEST_CASE("an editor shows its parts in the key editor or the list editor", "[unit]")
{
	Fixture f;
	CHECK(f.call("editor.get", {{"editor", "window"}}).at("content") == "key");
	// Open List Editor (reference manual p. 1243).
	CHECK(f.call("editor.open", {{"editor", "window"}, {"parts", {f.part}}, {"content", "list"}}).at("content") ==
		"list");
	// Opening other parts, as following the project window's selection does, keeps the content.
	CHECK(f.call("editor.open", {{"editor", "window"}, {"parts", {f.other}}}).at("content") == "list");
	CHECK(f.call("editor.get", {{"editor", "lower"}}).at("content") == "key"); // each editor has its own
	CHECK(f.call("editor.set", {{"editor", "window"}, {"content", "key"}}).at("content") == "key");
	f.call("editor.set", {{"editor", "window"}, {"content", "list"}});
	// A closed editor opens with the key editor again.
	CHECK(f.call("editor.close", {{"editor", "window"}}).at("content") == "key");
	CHECK_THAT(f.fails("editor.open", {{"editor", "window"}, {"parts", {f.part}}, {"content", "drum"}}),
		ContainsSubstring("content"));
}

TEST_CASE("the list editor's selected controller events are view state of the editable parts", "[unit]")
{
	Fixture f;
	f.call("editor.open", {{"editor", "window"}, {"parts", {f.part, f.other}}, {"content", "list"}});
	const Json revision = f.call("project.revision");
	const Json history = f.call("history.get");
	const Json shown = f.call("editor.set",
		{{"editor", "window"},
			{"controllerSelection", {f.selected(f.sysEx), f.selected(f.volume), f.selected(f.sysEx)}}});
	CHECK(shown.at("controllerSelection") == Json::array({f.selected(f.sysEx), f.selected(f.volume)}));
	CHECK(f.call("project.revision") == revision);
	CHECK(f.call("history.get") == history);

	CHECK_THAT(f.fails("editor.set",
				   {{"editor", "window"}, {"controllerSelection", {{{"part", f.other}, {"controller", f.volume}}}}}),
		ContainsSubstring("has no controller"));
	f.call("editor.set", {{"editor", "window"}, {"editingMode", "activePart"}, {"activePart", f.other}});
	CHECK(f.call("editor.get", {{"editor", "window"}}).at("controllerSelection").empty()); // no longer editable
	CHECK_THAT(f.fails("editor.set", {{"editor", "window"}, {"controllerSelection", {f.selected(f.volume)}}}),
		ContainsSubstring("isn't editable"));

	// Removed controller events leave the selection, and so does everything when the editor closes.
	f.call("editor.set", {{"editor", "window"}, {"editingMode", "allParts"}});
	f.call("editor.set", {{"editor", "window"}, {"controllerSelection", {f.selected(f.sysEx), f.selected(f.volume)}}});
	f.call("controller.remove", {{"part", f.part}, {"controllers", {f.sysEx}}});
	CHECK(
		f.call("editor.get", {{"editor", "window"}}).at("controllerSelection") == Json::array({f.selected(f.volume)}));
	CHECK(f.call("editor.close", {{"editor", "window"}}).at("controllerSelection").empty());
}
