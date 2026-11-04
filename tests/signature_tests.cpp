// SPDX-License-Identifier: GPL-3.0-or-later
// The signature track and its time signature events (#156).
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "engine/time/signaturemap.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;
using gnubase::time::g_ticksPerQuarter;

namespace
{
	// At the default 120 BPM and 48 kHz, a quarter note lasts 24000 samples.
	constexpr int64_t g_quarterSamples = 24000;

	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	std::string reject(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " succeeded: " << result.value.dump());
		REQUIRE(!result.ok());
		return result.error;
	}

	ObjectId addTrack(Client& client, Json parameters)
	{
		return run(client, "track.add", std::move(parameters)).at("track").at("id").get<ObjectId>();
	}

	Json change(int64_t bar, int numerator, int denominator)
	{
		return {{"bar", bar}, {"numerator", numerator}, {"denominator", denominator}};
	}

	std::vector<std::string> eventNames(Client& client)
	{
		std::vector<std::string> names;
		while (const auto message = client.poll())
			if (const auto* event = std::get_if<Event>(&*message))
				names.push_back(event->name);
		return names;
	}

	gnubase::time::SignatureMap signatures(Client& client)
	{
		std::vector<gnubase::time::SignatureChange> changes;
		const Json project = run(client, "project.get");
		for (const Json& item : project.at("signatures"))
			changes.push_back({item.at("bar").get<int64_t>(),
				{item.at("numerator").get<int>(), item.at("denominator").get<int>()}});
		return gnubase::time::SignatureMap(changes);
	}
}

TEST_CASE("a project has one signature track at most, without a channel, at the top level", "[unit]")
{
	Engine engine;
	Client client(engine);
	const Json added = run(client, "track.add", {{"type", "signature"}});
	CHECK(added.at("track").at("name") == "Signature");
	CHECK(added.at("channel").is_null());
	const ObjectId signature = added.at("track").at("id").get<ObjectId>();
	CHECK(engine.renderSnapshots().acquire().channels.empty());
	CHECK_THAT(reject(client, "track.add", {{"type", "signature"}}), ContainsSubstring("one signature track"));
	CHECK_THAT(reject(client, "track.duplicate", {{"track", signature}}), ContainsSubstring("one signature track"));
	const ObjectId folder = addTrack(client, {{"type", "folder"}});
	CHECK_THAT(reject(client, "track.move", {{"track", signature}, {"parent", folder}, {"index", 0}}),
		ContainsSubstring("folder"));

	// Removing the signature track keeps the signatures: the track only shows them.
	run(client, "signature.add", change(4, 3, 4));
	run(client, "track.remove", {{"track", signature}});
	CHECK(run(client, "project.get").at("signatures").size() == 2);
	CHECK_THAT(
		reject(client, "track.add", {{"type", "signature"}, {"parent", folder}}), ContainsSubstring("folder"));
}

TEST_CASE("time signature events change the bars from theirs on, and the first one stays", "[unit]")
{
	Engine engine;
	Client client(engine);
	eventNames(client);
	const Json added = run(client, "signature.add", change(2, 3, 4));
	CHECK(added.at("bar") == 2);
	CHECK(added.at("signatures") == Json::array({change(0, 4, 4), change(2, 3, 4)}));
	CHECK(eventNames(client) == std::vector<std::string>{"signature.changed", "history.changed"});
	// Bars 1 and 2 in 4/4, from bar 3 in 3/4.
	CHECK(signatures(client).barStart(3) == (8 + 3) * g_ticksPerQuarter);

	// An event at a bar that has one changes it; an equal signature is an event of its own.
	run(client, "signature.add", change(2, 6, 8));
	run(client, "signature.add", change(5, 6, 8));
	CHECK(run(client, "project.get").at("signatures") ==
		Json::array({change(0, 4, 4), change(2, 6, 8), change(5, 6, 8)}));

	CHECK_THAT(reject(client, "signature.add", change(3, 0, 4)), ContainsSubstring("numerator"));
	CHECK_THAT(reject(client, "signature.add", change(3, 100, 4)), ContainsSubstring("numerator"));
	CHECK_THAT(reject(client, "signature.add", change(3, 4, 3)), ContainsSubstring("denominator"));
	CHECK_THAT(reject(client, "signature.add", change(-1, 4, 4)), ContainsSubstring("negative"));
	CHECK_THAT(reject(client, "signature.remove", {{"bars", {0}}}), ContainsSubstring("first"));
	CHECK_THAT(reject(client, "signature.set", {{"events", {{{"bar", 0}, {"to", 1}}}}}), ContainsSubstring("first"));
	CHECK_THAT(reject(client, "signature.set", {{"events", {{{"bar", 2}, {"to", 0}}}}}), ContainsSubstring("first"));
	CHECK_THAT(reject(client, "signature.set", {{"events", {{{"bar", 3}, {"numerator", 2}}}}}),
		ContainsSubstring("no time signature event"));
	CHECK_THAT(reject(client, "signature.set", {{"events", {{{"bar", 2}, {"to", 5}}}}}),
		ContainsSubstring("two time signature events"));

	// Changes happen at once: two events can swap places. The first one's signature changes.
	run(client, "signature.set",
		{{"events",
			{{{"bar", 2}, {"to", 5}}, {{"bar", 5}, {"to", 2}, {"numerator", 5}, {"denominator", 4}},
				{{"bar", 0}, {"numerator", 2}, {"denominator", 2}}}}});
	CHECK(run(client, "project.get").at("signatures") ==
		Json::array({change(0, 2, 2), change(2, 5, 4), change(5, 6, 8)}));

	run(client, "signature.remove", {{"bars", {2, 5}}});
	CHECK(run(client, "project.get").at("signatures") == Json::array({change(0, 2, 2)}));
	// Each command is an undo step.
	run(client, "history.undo");
	CHECK(run(client, "project.get").at("signatures").size() == 3);
}

TEST_CASE("a time signature event at the cursor goes to the cursor's bar", "[unit]")
{
	Engine engine;
	Client client(engine);
	run(client, "signature.add", change(1, 3, 4));
	// Six quarters: in bar 2, which is 3/4 and starts at quarter 4.
	run(client, "transport.locate", {{"position", 6 * g_quarterSamples}});
	CHECK(run(client, "signature.add", {{"bar", "cursor"}, {"numerator", 7}, {"denominator", 8}}).at("bar") == 1);
	// Bar 2 is 7/8 now: three and a half quarters.
	run(client, "transport.locate", {{"position", 8 * g_quarterSamples}});
	CHECK(run(client, "signature.add", {{"bar", "cursor"}, {"numerator", 2}, {"denominator", 4}}).at("bar") == 2);
	CHECK(run(client, "project.get").at("signatures") ==
		Json::array({change(0, 4, 4), change(1, 7, 8), change(2, 2, 4)}));
}

TEST_CASE("a pickup bar is a short first bar, and events keep their positions when the bars change", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId track = addTrack(client, {{"type", "midi"}});
	const ObjectId part = run(client, "midiPart.add",
		{{"track", track}, {"position", 4 * g_ticksPerQuarter}, {"length", 4 * g_ticksPerQuarter}})
							  .at("part")
							  .at("id")
							  .get<ObjectId>();
	run(client, "note.add",
		{{"part", part},
			{"notes", {{{"position", 0}, {"length", g_ticksPerQuarter}, {"pitch", 60}, {"velocity", 1.0}}}}});
	// A pickup of one quarter, then 4/4.
	run(client, "project.set", {{"signature", {{"numerator", 1}, {"denominator", 4}}}});
	run(client, "signature.add", change(1, 4, 4));
	const gnubase::time::SignatureMap bars = signatures(client);
	CHECK(bars.barStart(1) == g_ticksPerQuarter);
	CHECK(bars.barStart(2) == 5 * g_ticksPerQuarter);
	CHECK(bars.toBarsBeats(4 * g_ticksPerQuarter).bar == 1);
	CHECK(bars.toBarsBeats(4 * g_ticksPerQuarter).beat == 3);
	// Events are in ticks: the bars move under them.
	const RenderSnapshot& snapshot = engine.renderSnapshots().acquire();
	const auto channel = std::find_if(snapshot.channels.begin(), snapshot.channels.end(),
		[track](const RenderChannel& item) { return item.track == track; });
	REQUIRE(channel != snapshot.channels.end());
	CHECK(channel->notes.at(0).start == 4 * g_quarterSamples);
}

TEST_CASE("the signature track and the time signature events are saved with the project", "[integration]")
{
	const gnubase::test::TempDir dir;
	const std::string file = (dir.path() / "signatures.dawproject").u8string();
	Engine engine;
	Client client(engine);
	addTrack(client, {{"type", "signature"}});
	run(client, "signature.add", change(2, 7, 8));
	run(client, "signature.add", change(4, 3, 4));
	run(client, "project.save", {{"path", file}});
	const Json before = run(client, "project.get");
	run(client, "project.new");
	run(client, "project.load", {{"path", file}});
	const Json after = run(client, "project.get");
	CHECK(after.at("signatures") == before.at("signatures"));
	CHECK(after.at("tracks").at(0).at("type") == "signature");
}
