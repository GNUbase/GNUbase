// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/document.h"
#include "engine/engine.h"
#include "engine/graph/events.h"
#include "engine/midi/logical.h"
#include "engine/midi/midivalues.h"
#include "engine/processors/midieffects.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using namespace gnubase::engine::midi;
using Catch::Matchers::ContainsSubstring;

namespace
{
	// 120 BPM at 48 kHz: a beat is 24000 samples. Project ticks per logical tick (480 to a quarter).
	constexpr int64_t g_beatTicks = 1720320;
	constexpr int64_t g_tick = g_beatTicks / 480;

	Json run(Client& client, const std::string& command, Json parameters = Json::object())
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	std::string reject(Client& client, const std::string& command, Json parameters)
	{
		const Result result = client.call(command, std::move(parameters));
		INFO(command << " succeeded: " << result.value.dump());
		REQUIRE(!result.ok());
		return result.error;
	}

	Json filter(const char* target, const char* condition, Json p1, double p2 = 0.0)
	{
		return {{"target", target}, {"condition", condition}, {"parameter1", std::move(p1)}, {"parameter2", p2}};
	}

	Json action(const char* target, const char* operation, Json p1 = 0, Json p2 = 0)
	{
		return {
			{"target", target}, {"operation", operation}, {"parameter1", std::move(p1)}, {"parameter2", std::move(p2)}};
	}

	Json rules(std::vector<Json> filters, const char* function, std::vector<Json> actions = {})
	{
		return {{"filters", Json(std::move(filters))}, {"function", function}, {"actions", Json(std::move(actions))}};
	}

	LogicalRules parsed(const Json& json)
	{
		return logicalRulesFromJson(json);
	}

	bool found(const Json& json, const LogicalEvent& event)
	{
		const LogicalRules r = parsed(json);
		return matches(r.filters.data(), r.filters.size(), event);
	}

	LogicalEvent note(int pitch, int velocity = 100, int channel = 1)
	{
		LogicalEvent event;
		event.type = LogicalType::Note;
		event.subtype = pitch;
		event.mainValue = velocity;
		event.channel = channel;
		event.length = 120;
		return event;
	}

	LogicalEvent cc(int number, int value, int channel = 1)
	{
		LogicalEvent event;
		event.type = LogicalType::Controller;
		event.subtype = number;
		event.mainValue = value;
		event.channel = channel;
		return event;
	}

	Json readJson(const std::filesystem::path& path)
	{
		std::ifstream in(path);
		return Json::parse(in);
	}
}

TEST_CASE("filter lines: conditions, and before or, and brackets", "[unit]")
{
	const Json isNote = filter("type", "equal", "note");
	CHECK(found(rules({}, "transform"), cc(1, 2))); // no filter finds everything
	CHECK(found(rules({isNote}, "transform"), note(60)));
	CHECK(!found(rules({isNote}, "transform"), cc(1, 2)));
	CHECK(found(rules({filter("type", "unequal", "note")}, "transform"), cc(1, 2)));

	const LogicalEvent c3 = note(60, 64, 2);
	CHECK(found(rules({filter("subtype", "equal", 60)}, "transform"), c3));
	CHECK(found(rules({filter("subtype", "unequal", 61)}, "transform"), c3));
	CHECK(found(rules({filter("mainValue", "bigger", 63)}, "transform"), c3));
	CHECK(!found(rules({filter("mainValue", "bigger", 64)}, "transform"), c3));
	CHECK(found(rules({filter("mainValue", "biggerOrEqual", 64)}, "transform"), c3));
	CHECK(found(rules({filter("channel", "less", 3)}, "transform"), c3));
	CHECK(found(rules({filter("channel", "lessOrEqual", 2)}, "transform"), c3));
	CHECK(found(rules({filter("subtype", "insideRange", 60, 72)}, "transform"), c3));
	CHECK(!found(rules({filter("subtype", "outsideRange", 60, 72)}, "transform"), c3));
	CHECK(found(rules({filter("length", "equal", 120)}, "transform"), c3));
	CHECK(found(rules({filter("subtype", "noteIsEqualTo", 0)}, "transform"), note(84)));
	CHECK(!found(rules({filter("subtype", "noteIsEqualTo", 0)}, "transform"), note(85)));

	// note and C3 or channel 2: (note and C3) or channel 2, so a controller on channel 2 is found.
	Json pitch = filter("subtype", "equal", 60);
	pitch["join"] = "or";
	const Json channel2 = filter("channel", "equal", 2);
	CHECK(found(rules({isNote, pitch, channel2}, "transform"), cc(60, 0, 2)));
	CHECK(!found(rules({isNote, pitch, channel2}, "transform"), cc(60, 0, 1)));
	// note and (C3 or channel 2): only notes.
	Json bracketed = pitch;
	bracketed["open"] = 1;
	Json closing = channel2;
	closing["close"] = 1;
	CHECK(!found(rules({isNote, bracketed, closing}, "transform"), cc(60, 0, 2)));
	CHECK(found(rules({isNote, bracketed, closing}, "transform"), note(61, 100, 2)));
	CHECK(found(rules({isNote, bracketed, closing}, "transform"), note(60, 100, 1)));
	CHECK(!found(rules({isNote, bracketed, closing}, "transform"), note(61, 100, 1)));
	// Nested: ((C3 or D3) and channel 1) or channel 3.
	Json c = filter("subtype", "equal", 60);
	c["open"] = 2;
	c["join"] = "or";
	Json d = filter("subtype", "equal", 62);
	d["close"] = 1;
	Json one = filter("channel", "equal", 1);
	one["close"] = 1;
	one["join"] = "or";
	const Json three = filter("channel", "equal", 3);
	const Json nested = rules({c, d, one, three}, "transform");
	CHECK(found(nested, note(62, 100, 1)));
	CHECK(!found(nested, note(62, 100, 2)));
	CHECK(found(nested, note(70, 100, 3)));
}

TEST_CASE("actions: operations, ranges and repeatable random values", "[unit]")
{
	const auto changed = [](const std::vector<Json>& actions, LogicalEvent event, uint64_t seed = 0)
	{
		const LogicalRules r = parsed(rules({}, "transform", actions));
		LogicalRandom random{seed};
		const bool kept = transform(r.actions.data(), r.actions.size(), event, random);
		return std::make_pair(kept, event);
	};
	CHECK(changed({action("subtype", "add", 12)}, note(60)).second.subtype == 72);
	CHECK(changed({action("subtype", "subtract", 12)}, note(60)).second.subtype == 48);
	CHECK(changed({action("mainValue", "multiplyBy", 0.5)}, note(60, 100)).second.mainValue == 50);
	CHECK(changed({action("mainValue", "divideBy", 4)}, note(60, 100)).second.mainValue == 25);
	CHECK(changed({action("mainValue", "divideBy", 0)}, note(60, 100)).second.mainValue == 100);
	CHECK(changed({action("mainValue", "roundBy", 16)}, note(60, 100)).second.mainValue == 96);
	CHECK(changed({action("channel", "setFixed", 5)}, note(60)).second.channel == 5);
	CHECK(changed({action("mainValue", "useSubtype")}, note(60, 100)).second.mainValue == 60);
	CHECK(changed({action("subtype", "useMainValue")}, note(60, 100)).second.subtype == 100);
	CHECK(changed({action("subtype", "mirror", 60)}, note(64)).second.subtype == 56);
	CHECK(changed({action("subtype", "transposeToScale", 0, "major")}, note(61)).second.subtype == 60);
	CHECK(changed({action("subtype", "transposeToScale", 2, "major")}, note(60)).second.subtype == 59); // B or C#
	CHECK(changed({action("type", "setFixed", "aftertouch")}, cc(1, 5)).second.type == LogicalType::Aftertouch);
	LogicalEvent moved = note(60);
	moved.position = 480;
	CHECK(changed({action("position", "addLength")}, moved).second.position == 600);
	CHECK(changed({action("position", "roundBy", 120)}, moved).second.position == 480);

	// Ranges: pitches beyond MIDI's delete the note, other values stay in theirs.
	CHECK(!changed({action("subtype", "add", 12)}, note(120)).first);
	CHECK(changed({action("subtype", "add", 12)}, cc(120, 0)).second.subtype == 127);
	CHECK(changed({action("mainValue", "add", 100)}, note(60, 100)).second.mainValue == 127);
	CHECK(changed({action("channel", "add", 30)}, note(60)).second.channel == 16);
	CHECK(changed({action("length", "setFixed", -5)}, note(60)).second.length == 1);
	// A note shorter than a tick keeps its length unless an action changes it (#966).
	LogicalEvent tiny = note(60);
	tiny.length = 0.25;
	CHECK(changed({action("subtype", "add", 12)}, tiny).second.length == 0.25);
	CHECK(changed({action("length", "multiplyBy", 2)}, tiny).second.length == 1);
	// Positions and lengths stay finite, so that multiplying by 0 afterwards gives 0.
	const std::vector<Json> far(5, action("position", "multiplyBy", 1e9));
	const double farthest = changed(far, moved).second.position;
	CHECK(std::isfinite(farthest));
	CHECK(farthest > 1e12);
	std::vector<Json> back = far;
	back.push_back(action("position", "multiplyBy", 0));
	CHECK(changed(back, moved).second.position == 0);

	// The same seed gives the same values; they lie in the range given in either order.
	std::set<int> values;
	for (uint64_t seed = 0; seed < 200; ++seed)
	{
		const int value = changed({action("mainValue", "setRandom", 90, 80)}, note(60), seed).second.mainValue;
		CHECK(value == changed({action("mainValue", "setRandom", 80, 90)}, note(60), seed).second.mainValue);
		CHECK(value >= 80);
		CHECK(value <= 90);
		values.insert(value);
		const int relative =
			changed({action("mainValue", "setRelativeRandom", -3, 3)}, note(60, 50), seed).second.mainValue;
		CHECK(relative >= 47);
		CHECK(relative <= 53);
	}
	CHECK(values.size() == 11);
}

TEST_CASE("rules as JSON: every factory preset round-trips, and wrong rules are refused", "[unit]")
{
	std::set<std::string> names;
	for (const LogicalPreset& preset : factoryLogicalPresets())
	{
		INFO(preset.name);
		CHECK(names.insert(preset.name).second);
		const Json json = toJson(preset.rules);
		CHECK(logicalRulesFromJson(json) == preset.rules);
		CHECK(toJson(logicalRulesFromJson(json)) == json);
	}
	CHECK(names.count("Init") == 1);

	const auto refused = [](const Json& json, const std::string& message)
	{
		INFO(json.dump());
		CHECK_THROWS_WITH(logicalRulesFromJson(json), ContainsSubstring(message));
	};
	Json open = filter("subtype", "equal", 60);
	open["open"] = 1;
	refused(rules({open}, "transform"), "not closed");
	Json close = filter("subtype", "equal", 60);
	close["close"] = 1;
	refused(rules({close}, "transform"), "wasn't opened");
	// Up to three brackets nest (#966).
	Json three = filter("subtype", "equal", 60);
	three["open"] = 3;
	three["close"] = 3;
	CHECK_NOTHROW(logicalRulesFromJson(rules({three}, "transform")));
	Json outer = filter("subtype", "equal", 60);
	outer["open"] = 3;
	Json inner = filter("subtype", "equal", 61);
	inner["open"] = 1;
	inner["close"] = 1;
	Json last = filter("subtype", "equal", 62);
	last["close"] = 3;
	refused(rules({outer, inner, last}, "transform"), "at most 3 deep");
	refused(rules({filter("type", "bigger", "note")}, "transform"), "equal or unequal");
	refused(rules({filter("type", "equal", "chord")}, "transform"), "chord is unknown");
	refused(rules({filter("type", "equal", 3)}, "transform"), "event type");
	refused(rules({filter("mainValue", "noteIsEqualTo", 3)}, "transform"), "subtype only");
	refused(rules({filter("subtype", "equal", "C3")}, "transform"), "must be a number");
	refused(rules({}, "transform", {action("mainValue", "useMainValue")}), "subtype only");
	refused(rules({}, "transform", {action("subtype", "useSubtype")}), "main value only");
	refused(rules({}, "transform", {action("type", "add", 1)}), "fixed value");
	refused(rules({}, "transform", {action("subtype", "transposeToScale", 0, "nope")}), "scale type");
	refused(rules({}, "shuffle"), "rules.function must be one of");
	refused(Json{{"filters", Json::array()}, {"extra", 1}}, "extra");
}

TEST_CASE("factory presets find the events they are meant for", "[unit]")
{
	const auto event = [](LogicalType type, int subtype = 0, int mainValue = 0, double length = 0.0)
	{
		LogicalEvent e;
		e.type = type;
		e.subtype = subtype;
		e.mainValue = mainValue;
		e.length = length;
		return e;
	};
	const auto finds = [](const std::string& name, const LogicalEvent& e)
	{
		for (const LogicalPreset& preset : factoryLogicalPresets())
			if (preset.name == name)
				return matches(preset.rules.filters.data(), preset.rules.filters.size(), e);
		INFO("no factory preset " << name);
		CHECK(false);
		return false;
	};
	using Y = LogicalType;
	CHECK(finds("Delete/Short notes", event(Y::Note, 60, 100, 30)));
	CHECK(!finds("Delete/Short notes", event(Y::Note, 60, 100, 60)));
	CHECK(finds("Delete/Soft notes", event(Y::Note, 60, 10)));
	CHECK(!finds("Delete/Soft notes", event(Y::Note, 60, 20)));
	CHECK(finds("Delete/Controllers", event(Y::Controller, 7, 100)));
	CHECK(finds("Delete/Aftertouch", event(Y::Aftertouch, 50)));
	CHECK(finds("Delete/Aftertouch", event(Y::PolyPressure, 60, 50)));
	CHECK(!finds("Delete/Aftertouch", event(Y::Controller, 1, 50)));
	CHECK(finds("Delete/Pitch bend", event(Y::PitchBend, 0, 64)));
	CHECK(finds("Delete/Program changes", event(Y::Program, 5)));
	CHECK(finds("Transform/Octave up", event(Y::Note, 60, 100)));
	CHECK(!finds("Transform/Octave up", event(Y::Controller, 60, 100)));
	CHECK(finds("Transform/Aftertouch to modulation", event(Y::Aftertouch, 50)));
	CHECK(finds("Transform/Modulation to aftertouch", event(Y::Controller, 1, 50)));
	CHECK(!finds("Transform/Modulation to aftertouch", event(Y::Controller, 7, 50)));
	CHECK(finds("Select/Notes above C3", event(Y::Note, 61, 100)));
	CHECK(!finds("Select/Notes above C3", event(Y::Note, 60, 100)));
	CHECK(finds("Input/Split below C3 to channel 2", event(Y::Note, 59, 100)));
	CHECK(!finds("Input/Split below C3 to channel 2", event(Y::Note, 60, 100)));
	CHECK(finds("Input/Sustain pedal to bass drum", event(Y::Controller, 64, 127)));
	CHECK(!finds("Input/Sustain pedal to bass drum", event(Y::Controller, 1, 127)));
}

namespace
{
	// An instrument track with a part of four beats from 0, its instrument the note probe.
	struct Fixture
	{
		explicit Fixture(const std::filesystem::path& settings = {}) : engine(gnubase::audio::makeBackends(), settings)
		{
			const Json added = run(client, "track.add", {{"type", "instrument"}});
			channel = added.at("channel").at("id").get<ObjectId>();
			track = added.at("track").at("id").get<ObjectId>();
			part = run(client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", 4 * g_beatTicks}})
					   .at("part")
					   .at("id")
					   .get<ObjectId>();
		}

		// Notes at a position and length in logical ticks, with pitch and MIDI 1.0 velocity; returns their ids.
		std::vector<ObjectId> add(const std::vector<std::vector<int>>& notes)
		{
			Json list = Json::array();
			for (const std::vector<int>& n : notes)
				list.push_back({{"position", n[0] * g_tick}, {"length", n[1] * g_tick}, {"pitch", n[2]},
					{"velocity", n[3] / 127.0}});
			std::vector<ObjectId> ids;
			const Json added = run(client, "note.add", {{"part", part}, {"notes", std::move(list)}});
			for (const Json& n : added.at("notes"))
				ids.push_back(n.at("id").get<ObjectId>());
			return ids;
		}

		Json partJson()
		{
			const Json project = run(client, "project.get");
			for (const Json& p : project.at("midiParts"))
				if (p.at("id") == part)
					return p;
			return {};
		}

		// The part's notes as "pitch/MIDI velocity@position" in logical ticks, in id order.
		std::vector<std::string> notes()
		{
			std::vector<std::string> result;
			const Json json = partJson();
			for (const Json& n : json.at("notes"))
			{
				const int velocity = static_cast<int>(std::lround(n.at("velocity").get<double>() * 127.0));
				result.push_back(std::to_string(n.at("pitch").get<int>()) + "/" + std::to_string(velocity) + "@" +
					std::to_string(n.at("position").get<int64_t>() / g_tick));
			}
			return result;
		}

		Engine engine;
		Client client{engine};
		ObjectId channel = 0;
		ObjectId track = 0;
		ObjectId part = 0;
	};
}

TEST_CASE("the MIDI rule commands tell callers how deep brackets nest", "[integration]")
{
	// The generated Lua and MCP references show the refusal callers can get (#1203).
	const Fixture f;
	std::vector<std::string> described;
	for (const CommandInfo& info : f.engine.commands())
		if (info.description.find("nest at most 3 deep") != std::string::npos)
			described.push_back(info.name);
	for (const char* name : {"midi.applyRules", "midi.findByRules", "midi.setInputTransformer"})
	{
		INFO(name);
		CHECK(std::find(described.begin(), described.end(), name) != described.end());
	}
}

TEST_CASE("midi.applyRules applies rules and presets to parts in one undo step", "[integration]")
{
	Fixture f;
	const std::vector<ObjectId> ids = f.add({{0, 30, 60, 100}, {120, 240, 64, 50}, {240, 120, 67, 10}});
	const Json isNote = filter("type", "equal", "note");

	SECTION("a factory preset deletes short notes")
	{
		const Json result =
			run(f.client, "midi.applyRules", {{"parts", Json::array({f.part})}, {"preset", "Delete/Short notes"}});
		CHECK(result.at("parts") == Json::array({f.part}));
		CHECK(result.at("found") == 1);
		CHECK(f.notes() == std::vector<std::string>{"64/50@120", "67/10@240"});
		run(f.client, "history.undo");
		CHECK(f.notes().size() == 3);
	}
	SECTION("transform changes what the filters find")
	{
		run(f.client, "midi.applyRules",
			{{"parts", Json::array({f.part})},
				{"rules",
					rules({isNote, filter("mainValue", "less", 60)}, "transform",
						{action("subtype", "add", 12), action("mainValue", "setFixed", 90)})}});
		CHECK(f.notes() == std::vector<std::string>{"60/100@0", "76/90@120", "79/90@240"});
	}
	SECTION("positions are project positions in ticks of 480 to a quarter note")
	{
		run(f.client, "midi.applyRules",
			{{"parts", Json::array({f.part})},
				{"rules",
					rules({filter("position", "biggerOrEqual", 120)}, "transform", {action("position", "add", 60)})}});
		CHECK(f.notes() == std::vector<std::string>{"60/100@0", "64/50@180", "67/10@300"});
		run(f.client, "midi.applyRules",
			{{"parts", Json::array({f.part})},
				{"rules", rules({}, "transform", {action("position", "subtract", 1000)})}});
		CHECK(f.notes() == std::vector<std::string>{"60/100@0", "64/50@0", "67/10@0"}); // not before the part
	}
	SECTION("insert adds changed copies, insert exclusive deletes what it doesn't find")
	{
		run(f.client, "midi.applyRules", {{"parts", Json::array({f.part})}, {"preset", "Insert/Double an octave up"}});
		CHECK(f.notes() ==
			std::vector<std::string>{"60/100@0", "64/50@120", "67/10@240", "72/100@0", "76/50@120", "79/10@240"});
		run(f.client, "midi.applyRules",
			{{"parts", Json::array({f.part})},
				{"rules", rules({filter("subtype", "bigger", 70)}, "insertExclusive", {action("subtype", "add", 1)})}});
		CHECK(f.notes() == std::vector<std::string>{"73/100@0", "77/50@120", "80/10@240"});
	}
	SECTION("only the notes given")
	{
		run(f.client, "midi.applyRules",
			{{"parts", Json::array({f.part})}, {"notes", Json::array({ids[1]})}, {"preset", "Transform/Octave up"}});
		CHECK(f.notes() == std::vector<std::string>{"60/100@0", "76/50@120", "67/10@240"});
	}
	SECTION("pitches beyond MIDI's delete notes; random values repeat with the seed")
	{
		run(f.client, "midi.applyRules",
			{{"parts", Json::array({f.part})}, {"rules", rules({}, "transform", {action("subtype", "add", 62)})}});
		CHECK(f.notes() == std::vector<std::string>{"122/100@0", "126/50@120"});
		const Json humanize = {
			{"parts", Json::array({f.part})}, {"preset", "Transform/Humanize velocities"}, {"seed", 7}};
		run(f.client, "midi.applyRules", humanize);
		const std::vector<std::string> first = f.notes();
		run(f.client, "history.undo");
		run(f.client, "midi.applyRules", humanize);
		CHECK(f.notes() == first);
	}
	SECTION("without a seed, each Apply gives new random values")
	{
		// Three velocities humanized by up to 10 agree between two Applies about once in 9000: of four Applies, the
		// last three agree with the first about once in 10^12 (#1203).
		const Json humanize = {{"parts", Json::array({f.part})}, {"preset", "Transform/Humanize velocities"}};
		std::vector<std::vector<std::string>> applied;
		for (int i = 0; i < 4; ++i)
		{
			run(f.client, "midi.applyRules", humanize);
			applied.push_back(f.notes());
			run(f.client, "history.undo");
		}
		CHECK(std::any_of(applied.begin() + 1, applied.end(),
			[&applied](const std::vector<std::string>& notes) { return notes != applied.front(); }));
	}
	SECTION("very short notes keep their length; positions stop at the longest project")
	{
		const Json added = run(f.client, "note.add",
			{{"part", f.part},
				{"notes", Json::array({{{"position", 0}, {"length", 1}, {"pitch", 50}, {"velocity", 0.5}}})}});
		const auto tiny = added.at("notes")[0].at("id").get<ObjectId>();
		run(f.client, "midi.applyRules", {{"parts", Json::array({f.part})}, {"preset", "Transform/Octave up"}});
		const auto noteOf = [&f](ObjectId id)
		{
			const Json part = f.partJson();
			for (const Json& n : part.at("notes"))
				if (n.at("id") == id)
					return n;
			return Json();
		};
		CHECK(noteOf(tiny).at("pitch") == 62);
		CHECK(noteOf(tiny).at("length") == 1);
		const std::vector<Json> far(4, action("position", "multiplyBy", 1e9));
		run(f.client, "midi.applyRules",
			{{"parts", Json::array({f.part})}, {"notes", Json::array({ids[1]})},
				{"rules", rules({}, "transform", far)}});
		// 2^48 samples at 48 kHz and 120 BPM.
		const double longest = std::ldexp(1.0, 48) / 48000.0 * 2.0 * static_cast<double>(g_beatTicks);
		const auto position = noteOf(ids[1]).at("position").get<int64_t>();
		CHECK(static_cast<double>(position) <= longest);
		CHECK(static_cast<double>(position) > longest - static_cast<double>(g_beatTicks));
	}
	SECTION("type changes that parts refuse leave the events as they are")
	{
		const Json toController =
			rules({isNote}, "transform", {action("subtype", "setFixed", 1), action("type", "setFixed", "controller")});
		run(f.client, "midi.applyRules", {{"parts", Json::array({f.part})}, {"rules", toController}});
		CHECK(f.notes() == std::vector<std::string>{"60/100@0", "64/50@120", "67/10@240"});
		Json copies = toController;
		copies["function"] = "insert";
		run(f.client, "midi.applyRules", {{"parts", Json::array({f.part})}, {"rules", copies}});
		CHECK(f.notes().size() == 3);
		run(f.client, "controller.add",
			{{"part", f.part},
				{"controllers", {{{"position", 0}, {"type", "controller"}, {"number", 64}, {"value", 1.0}}}}});
		run(f.client, "midi.applyRules",
			{{"parts", Json::array({f.part})}, {"preset", "Input/Sustain pedal to bass drum"}});
		const Json controllers = f.partJson().at("controllers");
		REQUIRE(controllers.size() == 1);
		CHECK(controllers[0].at("number") == 64);
		CHECK(f.notes().size() == 3);
	}
	SECTION("select finds and changes nothing")
	{
		const Json result =
			run(f.client, "midi.findByRules", {{"parts", Json::array({f.part})}, {"preset", "Select/Notes above C3"}});
		CHECK(result.at("notes") == Json{ids[1], ids[2]});
		CHECK(result.at("controllers") == Json::array());
		CHECK(f.notes().size() == 3);
		CHECK_THAT(reject(f.client, "midi.applyRules",
					   {{"parts", Json::array({f.part})}, {"preset", "Select/Notes above C3"}}),
			ContainsSubstring("midi.findByRules"));
	}
	SECTION("refused")
	{
		CHECK_THAT(reject(f.client, "midi.applyRules", {{"parts", Json::array({f.part})}}),
			ContainsSubstring("rules or preset"));
		CHECK_THAT(reject(f.client, "midi.applyRules", {{"parts", Json::array({f.part})}, {"preset", "Nope"}}),
			ContainsSubstring("no MIDI rule preset"));
		CHECK_THAT(
			reject(f.client, "midi.applyRules",
				{{"parts", Json::array({f.part})}, {"rules", rules({filter("type", "less", "note")}, "delete")}}),
			ContainsSubstring("equal or unequal"));
		CHECK_THAT(reject(f.client, "midi.applyRules", {{"parts", Json::array({999})}, {"preset", "Init"}}),
			ContainsSubstring("no MIDI part"));
	}
}

TEST_CASE("midi.applyRules changes controllers, pressure and pitch bend, and their types", "[integration]")
{
	Fixture f;
	run(f.client, "controller.add",
		{{"part", f.part},
			{"controllers",
				{{{"position", 0}, {"type", "channelPressure"}, {"value", 0.5}},
					{{"position", 0}, {"type", "controller"}, {"number", 7}, {"value", 1.0}},
					{{"position", 0}, {"type", "pitchBend"}, {"value", 0.5}}}}});
	run(f.client, "midi.applyRules",
		{{"parts", Json::array({f.part})}, {"preset", "Transform/Aftertouch to modulation"}});
	run(f.client, "midi.applyRules",
		{{"parts", Json::array({f.part})},
			{"rules", rules({filter("type", "equal", "pitchBend")}, "transform", {action("mainValue", "add", 1)})}});
	run(f.client, "midi.applyRules",
		{{"parts", Json::array({f.part})}, {"preset", "Delete/Controllers"}, {"notes", Json::array()}});
	const Json controllers = f.partJson().at("controllers");
	REQUIRE(controllers.size() == 3); // with notes given, controllers stay
	std::set<std::string> seen;
	for (const Json& c : controllers)
		seen.insert(c.at("type").get<std::string>() + " " + std::to_string(c.value("number", 0)));
	CHECK(seen == std::set<std::string>{"controller 1", "controller 7", "pitchBend 0"});
	run(f.client, "midi.applyRules", {{"parts", Json::array({f.part})}, {"preset", "Delete/Controllers"}});
	REQUIRE(f.partJson().at("controllers").size() == 1);
	CHECK(f.partJson().at("controllers")[0].at("value").get<double>() > 0.5); // the bend went up a step
}

TEST_CASE("midi.applyRules: Delete/Aftertouch removes channel pressure and poly pressure", "[integration]")
{
	Fixture f;
	run(f.client, "controller.add",
		{{"part", f.part},
			{"controllers",
				{{{"position", 0}, {"type", "channelPressure"}, {"value", 0.5}},
					{{"position", 0}, {"type", "polyPressure"}, {"number", 60}, {"value", 0.5}},
					{{"position", 0}, {"type", "controller"}, {"number", 7}, {"value", 1.0}}}}});
	const Json result =
		run(f.client, "midi.applyRules", {{"parts", Json::array({f.part})}, {"preset", "Delete/Aftertouch"}});
	CHECK(result.at("found") == 2);
	const Json controllers = f.partJson().at("controllers");
	REQUIRE(controllers.size() == 1);
	CHECK(controllers[0].at("type") == "controller");
}

TEST_CASE("MIDI rule presets: factory presets, and the user's kept in the settings folder", "[integration]")
{
	const gnubase::test::TempDir dir;
	const std::filesystem::path file = dir.path() / "midi-rule-presets.json";
	const Json octave = rules({filter("type", "equal", "note")}, "transform", {action("subtype", "add", 12)});
	{
		Fixture f(dir.path());
		const Json listed = run(f.client, "midiRules.presets");
		CHECK(listed.at("presets").size() == factoryLogicalPresets().size());
		CHECK(listed.at("presets")[0].at("name") == "Init");
		CHECK(listed.at("presets")[0].at("factory") == true);
		CHECK(listed.at("readOnly") == false);
		CHECK(!std::filesystem::exists(file)); // factory presets aren't written

		run(f.client, "midiRules.savePreset", {{"name", "Mine/Up"}, {"rules", octave}});
		run(f.client, "midiRules.savePreset", {{"name", "Gone"}, {"rules", rules({}, "delete")}});
		run(f.client, "midiRules.removePreset", {{"name", "Gone"}});
		CHECK_THAT(reject(f.client, "midiRules.savePreset", {{"name", "Init"}, {"rules", octave}}),
			ContainsSubstring("factory preset"));
		CHECK_THAT(
			reject(f.client, "midiRules.savePreset", {{"name", ""}, {"rules", octave}}), ContainsSubstring("empty"));
		CHECK_THAT(reject(f.client, "midiRules.removePreset", {{"name", "Init"}}), ContainsSubstring("factory"));
		CHECK_THAT(
			reject(f.client, "midiRules.removePreset", {{"name", "Gone"}}), ContainsSubstring("no MIDI rule preset"));
		CHECK_THAT(reject(f.client, "midiRules.savePreset",
					   {{"name", "Bad"}, {"rules", rules({filter("type", "less", "note")}, "delete")}}),
			ContainsSubstring("equal or unequal"));
	}
	const Json stored = readJson(file).at("values").at("midiRules.presets");
	REQUIRE(stored.size() == 1);
	CHECK(stored[0].at("name") == "Mine/Up");

	// A preset this GNUbase can't read is skipped and stays in the file.
	Json document = readJson(file);
	document["values"]["midiRules.presets"].push_back({{"name", "Broken"}, {"rules", {{"function", "shuffle"}}}});
	std::ofstream(file) << document.dump();
	{
		Fixture next(dir.path());
		const Json listed = run(next.client, "midiRules.presets");
		const Json& last = listed.at("presets").back();
		CHECK(last.at("name") == "Mine/Up");
		CHECK(last.at("factory") == false);
		CHECK(last.at("rules") == toJson(logicalRulesFromJson(octave)));
		REQUIRE(listed.at("problems").size() == 1);
		CHECK_THAT(listed.at("problems")[0].get<std::string>(), ContainsSubstring("skipped preset 2"));
		next.add({{0, 120, 60, 100}});
		run(next.client, "midi.applyRules", {{"parts", Json::array({next.part})}, {"preset", "Mine/Up"}});
		CHECK(next.notes() == std::vector<std::string>{"72/100@0"});
		run(next.client, "midiRules.savePreset", {{"name", "Mine/Down"}, {"rules", octave}});
	}
	CHECK(readJson(file).at("values").at("midiRules.presets").size() == 3); // Broken stays
}

namespace
{
	// Plays MIDI into the fixture's track and reads what its instrument played.
	struct LiveFixture : Fixture
	{
		LiveFixture()
		{
			run(client, "audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}, {"captureSeconds", 1}});
			run(client, "channel.set", {{"channel", channel}, {"instrument", "noteProbe"}});
			run(client, "midi.capture", {{"start", true}});
		}

		// Injects the messages at frame 0 and 100, renders and returns what the instrument played, a line per message
		// in hex.
		std::string play(std::vector<std::vector<int>> first, std::vector<std::vector<int>> second = {})
		{
			Json messages = Json::array();
			for (std::vector<int>& bytes : first)
				messages.push_back({{"position", 0}, {"bytes", std::move(bytes)}});
			for (std::vector<int>& bytes : second)
				messages.push_back({{"position", 100}, {"bytes", std::move(bytes)}});
			run(client, "midi.inject", {{"channel", channel}, {"messages", messages}});
			run(client, "audio.render", {{"frames", 256}});
			std::string text;
			const Json capture = run(client, "midi.capture");
			for (const Json& c : capture.at("captured"))
			{
				for (const int byte : c.at("bytes").get<std::vector<int>>())
				{
					char hex[4] = {};
					std::snprintf(hex, sizeof hex, "%02x ", byte);
					text += hex;
				}
				text.back() = '\n';
			}
			return text;
		}

		void transformer(const Json& json)
		{
			run(client, "channel.set", {{"channel", channel}, {"inputTransformer", json}});
		}
	};

	Json module(Json rules, bool on = true)
	{
		return {{"on", on}, {"rules", std::move(rules)}};
	}
}

TEST_CASE("an input transformer that inserts past its buffers' capacity ends every note it plays", "[unit]")
{
	using graph::ProcessEvent;
	constexpr size_t capacity = 16;
	processors::InputTransformerProcessor processor(capacity);
	graph::EventBuffer events(capacity);
	const std::vector<LogicalRules> modules = {
		logicalRulesFromJson(rules({filter("type", "equal", "note")}, "insert", {action("subtype", "add", 1)}))};
	const auto event = [](ProcessEvent::Type type, int pitch)
	{
		ProcessEvent e;
		e.type = type;
		e.key = static_cast<uint8_t>(pitch);
		e.velocity = 40000;
		return e;
	};
	// Twice as many notes as fit, played back in reverse order.
	for (int pitch = 0; pitch < static_cast<int>(capacity); ++pitch)
		events.add(event(ProcessEvent::Type::NoteOn, pitch * 2));
	processor.process(events, modules);
	std::multiset<int> on;
	for (const ProcessEvent& e : events)
		if (e.type == ProcessEvent::Type::NoteOn)
			on.insert(e.key);
	CHECK(on.size() == capacity);
	events.clear();
	for (int pitch = static_cast<int>(capacity) - 1; pitch >= 0; --pitch)
		events.add(event(ProcessEvent::Type::NoteOff, pitch * 2));
	processor.process(events, modules);
	std::multiset<int> off;
	for (const ProcessEvent& e : events)
		if (e.type == ProcessEvent::Type::NoteOff)
			off.insert(e.key);
	CHECK(off == on);
}

TEST_CASE("input transformers change what a track's MIDI input plays", "[integration]")
{
	LiveFixture f;
	const Json isNote = filter("type", "equal", "note");
	const Json up = rules({isNote}, "transform", {action("subtype", "add", 12)});

	SECTION("off by default; on in track mode, the note-off following its note-on")
	{
		CHECK(f.play({{0x90, 60, 100}}, {{0x80, 60, 0}}) == "90 3c 64\n80 3c 00\n");
		f.transformer({{"mode", "track"}, {"modules", Json::array({module(up)})}});
		CHECK(f.play({{0x90, 60, 100}}) == "90 48 64\n");
		// The note changes while it is held: its note-off still ends the note it started.
		f.transformer(
			{{"modules", Json::array({module(rules({isNote}, "transform", {action("subtype", "add", 1)}))})}});
		CHECK(f.play({}, {{0x80, 60, 0}}) == "80 48 00\n");
		f.transformer({{"mode", "off"}});
		CHECK(f.play({{0x90, 60, 100}}, {{0x80, 60, 0}}) == "90 3c 64\n80 3c 00\n");
	}
	SECTION("modules in order, those that are off left out")
	{
		f.transformer({{"mode", "track"},
			{"modules",
				{module(up), module(rules({isNote}, "transform", {action("subtype", "add", 1)}), false),
					module(rules({isNote}, "transform", {action("channel", "setFixed", 2)}))}}});
		CHECK(f.play({{0x90, 60, 100}}, {{0x80, 60, 0}}) == "91 48 64\n81 48 00\n");
	}
	SECTION("filter (delete), insert and insert exclusive")
	{
		f.transformer({{"mode", "track"},
			{"modules", Json::array({module(rules({filter("type", "equal", "pitchBend")}, "delete"))})}});
		CHECK(f.play({{0xE0, 0, 80}, {0xB0, 1, 5}}) == "b0 01 05\n");
		f.transformer({{"modules", Json::array({module(rules({isNote}, "insert", {action("subtype", "add", 7)}))})}});
		CHECK(f.play({{0x90, 60, 100}}, {{0x80, 60, 0}}) == "90 3c 64\n90 43 64\n80 3c 00\n80 43 00\n");
		f.transformer({{"modules",
			Json::array({module(rules({isNote}, "insertExclusive", {action("mainValue", "setFixed", 1)}))})}});
		CHECK(f.play({{0x90, 60, 100}, {0xB0, 1, 5}}, {{0x80, 60, 0}}) == "90 3c 01\n80 3c 00\n");
	}
	SECTION("the rules filter the poly pressure of a held note, which otherwise follows its note-on")
	{
		f.transformer({{"mode", "track"},
			{"modules", Json::array({module(rules({filter("type", "equal", "polyPressure")}, "delete"))})}});
		CHECK(f.play({{0x90, 60, 100}}, {{0xA0, 60, 50}}) == "90 3c 64\n");
		CHECK(f.play({{0xA0, 60, 50}}, {{0x80, 60, 0}}) == "80 3c 00\n");
		f.transformer({{"modules", Json::array({module(up)})}});
		CHECK(f.play({{0x90, 60, 100}}, {{0xA0, 60, 50}}) == "90 48 64\na0 48 32\n");
		CHECK(f.play({}, {{0x80, 60, 0}}) == "80 48 00\n");
	}
	SECTION("presets turn events into others: a sustain pedal plays a bass drum, aftertouch modulation")
	{
		const Json presets = run(f.client, "midiRules.presets").at("presets");
		const auto preset = [&presets](const std::string& name)
		{
			for (const Json& p : presets)
				if (p.at("name") == name)
					return p.at("rules");
			return Json();
		};
		f.transformer(
			{{"mode", "track"}, {"modules", Json::array({module(preset("Input/Sustain pedal to bass drum"))})}});
		CHECK(f.play({{0xB0, 64, 127}}, {{0xB0, 64, 0}}) == "90 24 7f\n80 24 40\n");
		f.transformer({{"modules", Json::array({module(preset("Transform/Aftertouch to modulation"))})}});
		CHECK(f.play({{0xD0, 99}}) == "b0 01 63\n");
	}
	SECTION("project mode shares the project's modules, which are part of the project")
	{
		run(f.client, "midi.setInputTransformer", {{"modules", Json::array({module(up)})}});
		f.transformer({{"mode", "project"}, {"modules", Json::array({module(rules({}, "delete"))})}});
		CHECK(f.play({{0x90, 60, 100}}, {{0x80, 60, 0}}) == "90 48 64\n80 48 00\n");
		const Json project = run(f.client, "project.get");
		CHECK(project.at("inputTransformer")[0].at("rules") == toJson(logicalRulesFromJson(up)));
		run(f.client, "history.undo"); // the track's mode
		run(f.client, "history.undo"); // the project's modules
		CHECK(run(f.client, "project.get").at("inputTransformer")[0].at("on") == false);
		CHECK_THAT(
			reject(f.client, "midi.setInputTransformer", {{"modules", Json::array({module(rules({}, "select"))})}}),
			ContainsSubstring("don't select"));
		CHECK_THAT(reject(f.client, "midi.setInputTransformer",
					   {{"modules", Json::array({module(up), module(up), module(up), module(up), module(up)})}}),
			ContainsSubstring("at most 4"));
		// The message names the module that the engine refuses (#1143).
		Json unclosed = filter("type", "equal", "note");
		unclosed["open"] = 1;
		CHECK_THAT(reject(f.client, "midi.setInputTransformer",
					   {{"modules", Json::array({module(up), module(rules({unclosed}, "delete"))})}}),
			ContainsSubstring("modules[1].rules.filters: a bracket is opened and not closed"));
		// Live events have no position or length.
		for (const Json& timed :
			{rules({filter("length", "less", 60)}, "delete"), rules({filter("position", "bigger", 0)}, "delete"),
				rules({}, "transform", {action("position", "roundBy", 120)}),
				rules({}, "transform", {action("length", "setFixed", 60)})})
		{
			INFO(timed.dump());
			CHECK_THAT(reject(f.client, "midi.setInputTransformer", {{"modules", Json::array({module(timed)})}}),
				ContainsSubstring("no position or length"));
			CHECK_THAT(reject(f.client, "channel.set",
						   {{"channel", f.channel},
							   {"inputTransformer", {{"mode", "track"}, {"modules", Json::array({module(timed)})}}}}),
				ContainsSubstring("no position or length"));
		}
	}
	SECTION("saved with the project")
	{
		const gnubase::test::TempDir dir;
		const std::string path = (dir.path() / "t.dawproject").u8string();
		run(f.client, "midi.setInputTransformer", {{"modules", Json::array({module(up)})}});
		f.transformer({{"mode", "track"}, {"modules", Json::array({Json::object(), module(up)})}});
		const Json before = run(f.client, "project.get");
		run(f.client, "project.save", {{"path", path}});
		run(f.client, "project.new");
		run(f.client, "project.load", {{"path", path}});
		const Json after = run(f.client, "project.get");
		CHECK(after.at("inputTransformer") == before.at("inputTransformer"));
		for (const Json& channel : after.at("channels"))
			if (channel.at("id") == f.channel)
				CHECK(channel.at("inputTransformer").at("modules")[1] == module(toJson(logicalRulesFromJson(up))));
	}
	SECTION("only MIDI and instrument tracks have one")
	{
		const ObjectId audio = run(f.client, "track.add", {{"type", "audio"}}).at("channel").at("id").get<ObjectId>();
		CHECK_THAT(reject(f.client, "channel.set", {{"channel", audio}, {"inputTransformer", {{"mode", "track"}}}}),
			ContainsSubstring("only MIDI and instrument tracks"));
		CHECK_THAT(
			reject(f.client, "channel.set", {{"channel", f.channel}, {"inputTransformer", {{"mode", "sometimes"}}}}),
			ContainsSubstring("sometimes"));
	}
}
