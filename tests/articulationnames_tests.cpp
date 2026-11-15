// SPDX-License-Identifier: GPL-3.0-or-later
// The articulation names and pop-up menus of the key editor's info line and the list editor (ui/articulationnames.h,
// docs/features/articulation-maps.md).
#include "ui/articulationnames.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using namespace gnubase;
using engine::Json;

TEST_CASE("articulation names are joined and split at commas, as the info line and the list editor show them", "[unit]")
{
	CHECK(ui::joinNames({}).empty());
	CHECK(ui::joinNames({"Staccato", "Pizzicato"}) == "Staccato, Pizzicato");
	CHECK(ui::splitNames(" Staccato ,Pizzicato,, ") == std::vector<std::string>{"Staccato", "Pizzicato"});
	CHECK(ui::splitNames("").empty());
}

TEST_CASE("the info line and the list editor know the articulations a track's map doesn't have", "[unit]")
{
	// #1580, reference manual p. 1291: they show a warning sign for them.
	const Json map = {{"id", 7},
		{"articulations",
			{{{"name", "Arco"}, {"type", "direction"}, {"group", 0}},
				{{"name", "Staccato"}, {"type", "attribute"}, {"group", 1}}}}};
	const Json project = {
		{"tracks", {{{"id", 1}, {"articulationMap", 7}}, {{"id", 2}}}}, {"articulationMaps", Json::array({map})}};
	REQUIRE(ui::trackArticulationMap(project, 1) != nullptr);
	CHECK(*ui::trackArticulationMap(project, 1) == map);
	CHECK(ui::trackArticulationMap(project, 2) == nullptr);
	const Json* found = ui::trackArticulationMap(project, 1);
	CHECK_FALSE(ui::unknownArticulations(found, {}, "attribute"));
	CHECK_FALSE(ui::unknownArticulations(found, {"Staccato"}, "attribute"));
	CHECK(ui::unknownArticulations(found, {"Staccato", "Legato"}, "attribute"));
	// A direction's name is no attribute, and the other way round.
	CHECK(ui::unknownArticulations(found, {"Arco"}, "attribute"));
	CHECK_FALSE(ui::unknownArticulations(found, {"Arco"}, "direction"));
	CHECK(ui::unknownArticulations(found, {"Staccato"}, "direction"));
	// Without a map, every name is unknown.
	CHECK(ui::unknownArticulations(nullptr, {"Staccato"}, "attribute"));
	CHECK_FALSE(ui::unknownArticulations(nullptr, {}, "attribute"));
}

TEST_CASE("the pop-up menus of the attributes list the map's and toggle one of each group", "[unit]")
{
	// #1580, reference manual pp. 1279, 1290-1291.
	const Json map = {{"articulations",
		{{{"name", "Arco"}, {"type", "direction"}, {"group", 0}},
			{{"name", "Staccato"}, {"type", "attribute"}, {"group", 1}},
			{{"name", "Tenuto"}, {"type", "attribute"}, {"group", 1}},
			{{"name", "Accent & More"}, {"type", "attribute"}, {"group", 2}}}}};
	CHECK(ui::articulationItems(&map, "attribute", {"Tenuto"}) ==
		R"(<menuitem type="toggle" articulation="Staccato">Staccato</menuitem>)"
		R"(<menuitem type="toggle" articulation="Tenuto" checked>Tenuto</menuitem>)"
		R"(<menuitem type="toggle" articulation="Accent &amp; More">Accent &amp; More</menuitem>)");
	CHECK(ui::articulationItems(&map, "direction", {}) ==
		R"(<menuitem type="toggle" articulation="Arco">Arco</menuitem>)");
	CHECK(ui::articulationItems(nullptr, "attribute", {}) == "<menuitem disabled>No Articulation Map</menuitem>");
	// One of each group: Tenuto takes Staccato's place, other names stay; chosen again, it goes.
	using Names = std::vector<std::string>;
	CHECK(ui::toggleAttribute(map, {"Staccato", "Legato", "Accent & More"}, "Tenuto") ==
		Names{"Legato", "Accent & More", "Tenuto"});
	CHECK(ui::toggleAttribute(map, {"Tenuto", "Legato"}, "Tenuto") == Names{"Legato"});
	CHECK(ui::toggleAttribute(map, {}, "Staccato") == Names{"Staccato"});
}
