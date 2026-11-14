// SPDX-License-Identifier: GPL-3.0-or-later
// MPE input (#1514, reference manual pp. 1295-1298): each MIDI input port has its MPE setup, MPE's lower or upper zone
// or channel rotation with its pitch range, Use for Tuning and initial values, and the port's MPE input reaches
// recording and the instruments as per-note expression.
#include "engine/document/document.h"
#include "engine/engine.h"
#include "engine/midi/midivalues.h"
#include "engine/midi/mpeinput.h"
#include "platform/audio/device.h"
#include "platform/hosttime.h"
#include "platform/midi/dummyport.h"
#include "plugin/events.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;
using Type = graph::ProcessEvent::Type;

namespace
{
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

	Json waitFor(Client& client, const std::string& name)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline)
		{
			const std::optional<Message> next = client.wait(std::chrono::milliseconds(50));
			const auto* event = next ? std::get_if<Event>(&*next) : nullptr;
			if (event != nullptr && event->name == name)
				return event->data;
		}
		return nullptr;
	}

	int64_t milli(double value)
	{
		return std::llround(value * 1000.0);
	}

	// A MIDI 1.0 message's event, as the MIDI input makes it.
	graph::ProcessEvent event(Type type, uint8_t channel, uint8_t key, uint8_t value = 0)
	{
		graph::ProcessEvent result;
		result.type = type;
		result.channel = channel;
		result.key = key;
		if (type == Type::NoteOn || type == Type::NoteOff)
			result.velocity = midi::velocityFromMidi1(100);
		else if (type == Type::PitchBend)
			result.value = midi::pitchBendFromMidi1(static_cast<uint16_t>(value << 7U));
		else
			result.value = midi::controllerFromMidi1(value);
		return result;
	}

	// What the input made of the events, one line each: "on 1 60", "tuning 1 60 12000" (values * 1000), "bend 0".
	std::vector<std::string> through(
		midi::MpeInput& input, const midi::MpeSetup& setup, const std::vector<graph::ProcessEvent>& events)
	{
		graph::EventBuffer out(64);
		for (const graph::ProcessEvent& each : events)
			input.process(setup, each, out);
		std::vector<std::string> lines;
		for (const graph::ProcessEvent& each : out)
		{
			const std::string at = std::to_string(each.channel) + " ";
			switch (each.type)
			{
			case Type::NoteOn:
				lines.push_back("on " + at + std::to_string(each.key));
				break;
			case Type::NoteOff:
				lines.push_back("off " + at + std::to_string(each.key));
				break;
			case Type::PitchBend:
				lines.push_back("bend " + std::to_string(each.channel));
				break;
			case Type::ChannelPressure:
				lines.push_back("pressure " + std::to_string(each.channel));
				break;
			case Type::Controller:
				lines.push_back("cc " + at + std::to_string(each.key));
				break;
			case Type::NoteExpression:
				lines.push_back(std::string(g_expressionNames.at(each.parameter)) + " " + at +
					std::to_string(each.key) + " " + std::to_string(milli(each.parameterValue)));
				break;
			default:
				lines.emplace_back("other");
			}
		}
		return lines;
	}

	using Lines = std::vector<std::string>;

	// What a MIDI 1.0 value records as: a controller's 0-1 and a pitch bend's semitones, * 1000.
	std::string unit(uint8_t value)
	{
		return std::to_string(milli(midi::controllerToUnit(midi::controllerFromMidi1(value))));
	}

	std::string semitones(uint8_t msb, double range)
	{
		return std::to_string(
			milli(midi::bendToSemitones(midi::pitchBendFromMidi1(static_cast<uint16_t>(msb << 7U)), range)));
	}
}

TEST_CASE("MPE's lower zone makes the notes' channels' pitch bend, pressure and CC 74 their expression", "[unit]")
{
	midi::MpeSetup setup;
	setup.mode = midi::MpeSetup::Mode::Lower;
	setup.pitchRange = 24;
	midi::MpeInput input;
	// Before the note-on: the channel's state, which follows the note-on as its initial values.
	CHECK(through(input, setup,
			  {event(Type::PitchBend, 1, 0, 96), event(Type::ChannelPressure, 1, 0, 40),
				  event(Type::Controller, 1, 74, 64), event(Type::NoteOn, 1, 60)}) ==
		Lines{
			"on 1 60", "tuning 1 60 " + semitones(96, 24), "pressure 1 60 " + unit(40), "brightness 1 60 " + unit(64)});
	CHECK(semitones(96, 24) == "12001");
	// While it sounds, each message is a point of the notes on its channel; the master channel's and the others stay.
	CHECK(through(input, setup,
			  {event(Type::Controller, 1, 74, 100), event(Type::Controller, 1, 1, 10), event(Type::PitchBend, 0, 0, 80),
				  event(Type::ChannelPressure, 2, 0, 50), event(Type::NoteOff, 1, 60),
				  event(Type::PitchBend, 1, 0, 64)}) ==
		Lines{"brightness 1 60 " + unit(100), "cc 1 1", "bend 0", "off 1 60"});

	// Fewer member channels: the channels above them are no notes' own.
	setup.channels = 2;
	CHECK(through(input, setup, {event(Type::ChannelPressure, 2, 0, 50), event(Type::ChannelPressure, 3, 0, 50)}) ==
		Lines{"pressure 3"});
	CHECK(setup.notesChannel(1));
	CHECK(setup.notesChannel(2));
	CHECK_FALSE(setup.notesChannel(0));
	CHECK_FALSE(setup.notesChannel(3));
}

TEST_CASE("MPE's upper zone has its master channel on 16 and the notes' channels below it", "[unit]")
{
	midi::MpeSetup setup;
	setup.mode = midi::MpeSetup::Mode::Upper;
	setup.channels = 3;
	CHECK_FALSE(setup.notesChannel(15));
	CHECK(setup.notesChannel(14));
	CHECK(setup.notesChannel(12));
	CHECK_FALSE(setup.notesChannel(11));
	CHECK_FALSE(setup.notesChannel(0));
	midi::MpeInput input;
	CHECK(through(input, setup,
			  {event(Type::NoteOn, 14, 62), event(Type::PitchBend, 14, 0, 80), event(Type::PitchBend, 15, 0, 80)}) ==
		Lines{"on 14 62", "tuning 14 62 " + semitones(80, 48), "bend 15"});
	// A zone has at most 15 member channels.
	setup.channels = 16;
	CHECK_FALSE(setup.notesChannel(15));
	CHECK(setup.notesChannel(0));
}

TEST_CASE("channel rotation gives every used channel to its notes, without a master channel", "[unit]")
{
	midi::MpeSetup setup;
	setup.mode = midi::MpeSetup::Mode::Rotation;
	setup.firstChannel = 3;
	setup.channels = 4;
	CHECK_FALSE(setup.notesChannel(1));
	CHECK(setup.notesChannel(2));
	CHECK(setup.notesChannel(5));
	CHECK_FALSE(setup.notesChannel(6));
	midi::MpeInput input;
	CHECK(through(input, setup,
			  {event(Type::NoteOn, 2, 60), event(Type::ChannelPressure, 2, 0, 90),
				  event(Type::ChannelPressure, 0, 0, 90)}) ==
		Lines{"on 2 60", "pressure 2 60 " + unit(90), "pressure 0"});
	// The used channels end at 16.
	setup.firstChannel = 14;
	CHECK(setup.notesChannel(15));
	CHECK_FALSE(setup.notesChannel(0));
	// All 16.
	setup.firstChannel = 1;
	setup.channels = 16;
	CHECK(setup.notesChannel(0));
	CHECK(setup.notesChannel(15));
}

TEST_CASE("Send Initial Value starts each note at a fixed value instead of the channel's latest", "[unit]")
{
	midi::MpeSetup setup;
	setup.mode = midi::MpeSetup::Mode::Lower;
	setup.initialX = midi::MpeSetup::Initial::Center;
	setup.initialY = midi::MpeSetup::Initial::Max;
	midi::MpeInput input;
	// The bend and CC 74 that the previous note left don't carry over; pressure, without the setting, does.
	CHECK(through(input, setup,
			  {event(Type::NoteOn, 1, 60), event(Type::PitchBend, 1, 0, 96), event(Type::Controller, 1, 74, 10),
				  event(Type::ChannelPressure, 1, 0, 30), event(Type::NoteOff, 1, 60), event(Type::NoteOn, 1, 64)}) ==
		Lines{"on 1 60", "tuning 1 60 0", "brightness 1 60 1000", "tuning 1 60 " + semitones(96, 48),
			"brightness 1 60 " + unit(10), "pressure 1 60 " + unit(30), "off 1 60", "on 1 64", "tuning 1 64 0",
			"pressure 1 64 " + unit(30), "brightness 1 64 1000"});
	setup.initialY = midi::MpeSetup::Initial::Min;
	input.reset();
	CHECK(
		through(input, setup, {event(Type::NoteOn, 2, 60)}) == Lines{"on 2 60", "tuning 2 60 0", "brightness 2 60 0"});
	setup.initialY = midi::MpeSetup::Initial::Center;
	input.reset();
	CHECK(through(input, setup, {event(Type::NoteOn, 2, 60)}) ==
		Lines{"on 2 60", "tuning 2 60 0", "brightness 2 60 " + unit(64)});
}

TEST_CASE("without Use for Tuning the notes' pitch bend stays pitch bend", "[unit]")
{
	midi::MpeSetup setup;
	setup.mode = midi::MpeSetup::Mode::Lower;
	setup.tuning = false;
	setup.initialX = midi::MpeSetup::Initial::Center; // only for tuning
	midi::MpeInput input;
	CHECK(through(input, setup,
			  {event(Type::PitchBend, 1, 0, 96), event(Type::NoteOn, 1, 60), event(Type::PitchBend, 1, 0, 80),
				  event(Type::ChannelPressure, 1, 0, 30)}) ==
		Lines{"bend 1", "on 1 60", "bend 1", "pressure 1 60 " + unit(30)});
}

TEST_CASE("an MPE setup reads its JSON with defaults and packs into 32 bits", "[unit]")
{
	const midi::MpeSetup defaults;
	CHECK(defaults.toJson() == Json::parse(R"({"mode": "off", "channels": 15, "firstChannel": 1, "pitchRange": 48,
		"tuning": true, "initialX": "off", "initialY": "off"})"));
	CHECK(midi::MpeSetup::fromJson(Json::object()) == defaults);
	const midi::MpeSetup setup = midi::MpeSetup::fromJson(Json::parse(R"({"mode": "rotation", "channels": 16,
		"firstChannel": 16, "pitchRange": 96, "tuning": false, "initialX": "center", "initialY": "max"})"));
	CHECK(setup.mode == midi::MpeSetup::Mode::Rotation);
	CHECK(setup.channels == 16);
	CHECK(setup.firstChannel == 16);
	CHECK(setup.pitchRange == 96);
	CHECK_FALSE(setup.tuning);
	CHECK(setup.initialX == midi::MpeSetup::Initial::Center);
	CHECK(setup.initialY == midi::MpeSetup::Initial::Max);
	const midi::MpeSetup unpacked = midi::MpeSetup::unpack(setup.pack());
	CHECK(unpacked.toJson() == setup.toJson());
	CHECK(midi::MpeSetup::unpack(defaults.pack()).toJson() == defaults.toJson());
	// Out of range values are clamped.
	CHECK(midi::MpeSetup::fromJson({{"pitchRange", 200}, {"channels", 0}}).toJson().at("pitchRange") == 96);
	CHECK(midi::MpeSetup::fromJson({{"pitchRange", 200}, {"channels", 0}}).toJson().at("channels") == 1);
}

namespace
{
	constexpr gnubase::HostTime g_second = 1000000000;

	// A record-enabled instrument track playing the note probe, on the dummy device, with two MIDI keyboards: MPE and
	// Keys.
	struct Setup
	{
		Setup() : Setup(std::make_unique<gnubase::midi::DummyBackend>()) {}

		explicit Setup(std::unique_ptr<gnubase::midi::DummyBackend> dummy)
			: backend(*dummy), engine(gnubase::audio::makeBackends(), {}, backends(std::move(dummy)))
		{
			run(client, "audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}, {"captureSeconds", 1}});
			const Json added = run(client, "track.add", {{"type", "instrument"}});
			track = added.at("track").at("id").get<ObjectId>();
			channel = added.at("channel").at("id").get<ObjectId>();
			run(client, "channel.set", {{"channel", channel}, {"instrument", "noteProbe"}, {"recordEnable", true}});
			run(client, "midi.ports"); // opens the inputs
			mpe = backend.openOutput("MPE");
			keys = backend.openOutput("Keys");
		}

		static std::vector<std::unique_ptr<gnubase::midi::Backend>> backends(
			std::unique_ptr<gnubase::midi::DummyBackend> dummy)
		{
			dummy->addLoopback("MPE");
			dummy->addLoopback("Keys");
			std::vector<std::unique_ptr<gnubase::midi::Backend>> result;
			result.push_back(std::move(dummy));
			return result;
		}

		// Sends the messages on the port, which play at the next block's start, and renders frames.
		void play(gnubase::midi::Output& port, const std::vector<std::vector<uint8_t>>& messages, int frames = 64)
		{
			for (const std::vector<uint8_t>& bytes : messages)
			{
				gnubase::audio::MidiMessage message;
				message.size = static_cast<uint8_t>(bytes.size());
				std::copy(bytes.begin(), bytes.end(), message.bytes.begin());
				port.send(message, gnubase::hostTime() - g_second);
			}
			render(frames);
		}

		void render(int64_t frames)
		{
			run(client, "audio.render", {{"frames", frames}});
			position += frames;
		}

		// Renders until the device is at the frame, a block's start.
		void renderTo(int64_t frame)
		{
			REQUIRE(frame >= position);
			if (frame > position)
				render(frame - position);
		}

		gnubase::midi::DummyBackend& backend;
		Engine engine;
		Client client{engine};
		ObjectId track = 0;
		ObjectId channel = 0;
		std::unique_ptr<gnubase::midi::Output> mpe;
		std::unique_ptr<gnubase::midi::Output> keys;
		int64_t position = 0; // the device's
	};

	// The MIDI port, or null.
	Json port(const Json& ports, const std::string& id, bool input)
	{
		for (const Json& entry : ports.at("ports"))
			if (entry.at("id") == id && entry.at("input") == input)
				return entry;
		return nullptr;
	}

	// What the instruments played, as their bytes.
	std::vector<std::vector<int64_t>> captured(Client& client)
	{
		std::vector<std::vector<int64_t>> played;
		const Json capture = run(client, "midi.capture");
		for (const Json& each : capture.at("captured"))
			played.push_back(each.at("bytes").get<std::vector<int64_t>>());
		return played;
	}

	// A note's expression points as [type, position, value * 1000 rounded], sorted.
	using Points = std::vector<std::tuple<std::string, int64_t, int64_t>>;

	Points pointsOf(const Json& note)
	{
		Points points;
		for (const Json& point : note.value("expressions", Json::array()))
			points.emplace_back(point.at("type").get<std::string>(), point.at("position").get<int64_t>(),
				milli(point.at("value").get<double>()));
		std::sort(points.begin(), points.end());
		return points;
	}

	int64_t semitonesOf(uint8_t msb, double range)
	{
		return milli(midi::bendToSemitones(midi::pitchBendFromMidi1(static_cast<uint16_t>(msb << 7U)), range));
	}

	// 120 BPM at 48 kHz: a sample is 71.68 ticks, rounded as recording rounds.
	int64_t ticksOf(int64_t samples)
	{
		return std::llround(static_cast<double>(samples) * 1720320.0 / 24000.0);
	}

	int64_t unitOf(uint8_t value)
	{
		return milli(midi::controllerToUnit(midi::controllerFromMidi1(value)));
	}
}

TEST_CASE("midi.setPort sets an input's MPE setup, which midi.ports lists and midi.portSettings keeps", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	const Json ports = run(client, "midi.ports");
	CHECK(port(ports, "MPE", true).at("mpe") == midi::MpeSetup().toJson());
	CHECK_FALSE(port(ports, "MPE", false).contains("mpe")); // outputs have none

	// What a call leaves out stays as it was.
	run(client, "midi.setPort", {{"id", "MPE"}, {"input", true}, {"mpe", {{"mode", "upper"}, {"pitchRange", 24}}}});
	const Json changed = run(client, "midi.setPort", {{"id", "MPE"}, {"input", true}, {"mpe", {{"channels", 7}}}});
	CHECK(changed.at("mpe").at("mode") == "upper");
	CHECK(changed.at("mpe").at("pitchRange") == 24);
	CHECK(changed.at("mpe").at("channels") == 7);
	CHECK(port(run(client, "midi.ports"), "MPE", true).at("mpe") == changed.at("mpe"));
	CHECK(port(run(client, "midi.ports"), "Keys", true).at("mpe").at("mode") == "off");
	const Json settings = run(client, "settings.get", {{"key", "midi.portSettings"}}).at("value");
	CHECK(settings.at("dummy/in/MPE").at("mpe").at("mode") == "upper");

	// Back to the defaults, the port's entry goes.
	run(client, "midi.setPort", {{"id", "MPE"}, {"input", true}, {"mpe", midi::MpeSetup().toJson()}});
	CHECK(run(client, "settings.get", {{"key", "midi.portSettings"}}).at("value") == Json::object());

	CHECK_THAT(reject(client, "midi.setPort", {{"id", "MPE"}, {"input", false}, {"mpe", {{"mode", "lower"}}}}),
		ContainsSubstring("only inputs"));
	CHECK_THAT(reject(client, "midi.setPort", {{"id", "MPE"}, {"input", true}, {"mpe", {{"mode", "middle"}}}}),
		ContainsSubstring("mode"));
	CHECK_THAT(reject(client, "midi.setPort", {{"id", "MPE"}, {"input", true}, {"mpe", {{"initialX", "max"}}}}),
		ContainsSubstring("initialX"));
}

TEST_CASE("an MPE port's input plays to the instrument as per-note expression, another port's as MIDI", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	run(client, "midi.setPort", {{"id", "MPE"}, {"input", true}, {"mpe", {{"mode", "lower"}, {"pitchRange", 24}}}});
	run(client, "audio.render", {{"frames", 64}});
	run(client, "midi.capture", {{"start", true}});
	// A bend of a quarter up before the note-on: 12 semitones by the port's pitch range of 24, which the instrument
	// gets as the note's tuning, here as MPE's 48 semitones: an eighth up.
	setup.play(*setup.mpe, {{0xE1, 0, 96}, {0x91, 60, 100}});
	setup.play(*setup.mpe, {{0xD1, 40}, {0xB0, 7, 100}});
	setup.play(*setup.mpe, {{0x81, 60, 0}});
	CHECK(captured(client) ==
		std::vector<std::vector<int64_t>>{{0xE1, 0, 80}, {0x91, 60, 100}, {0xD1, 40}, {0xB0, 7, 100}, {0x81, 60, 0}});
	// The keyboard without MPE plays its pitch bend as it is.
	setup.play(*setup.keys, {{0xE1, 0, 96}, {0x91, 60, 100}});
	setup.play(*setup.keys, {{0x81, 60, 0}});
	CHECK(captured(client) == std::vector<std::vector<int64_t>>{{0xE1, 0, 96}, {0x91, 60, 100}, {0x81, 60, 0}});
}

TEST_CASE("an MPE port's expression follows its note to the key and channel the track plays it on", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	run(client, "midi.setPort", {{"id", "MPE"}, {"input", true}, {"mpe", {{"mode", "lower"}, {"pitchRange", 24}}}});
	// The initial bend comes before its note-on only where it is that note's expression: same key and channel.
	SECTION("the track's MIDI modifiers transpose the note")
	{
		run(client, "channel.set", {{"channel", setup.channel}, {"modifiers", {{"transpose", 12}}}});
		run(client, "audio.render", {{"frames", 64}});
		run(client, "midi.capture", {{"start", true}});
		setup.play(*setup.mpe, {{0xE1, 0, 96}, {0x91, 60, 100}});
		setup.play(*setup.mpe, {{0xD1, 40}});
		setup.play(*setup.mpe, {{0x81, 60, 0}});
		CHECK(captured(client) ==
			std::vector<std::vector<int64_t>>{{0xE1, 0, 80}, {0x91, 72, 100}, {0xD1, 40}, {0x81, 72, 0}});
	}
	SECTION("a MIDI track plays it on its MIDI channel")
	{
		// The MIDI track plays the instrument, record-enabled in its place.
		const auto midi = run(client, "track.add", {{"type", "midi"}}).at("channel").at("id").get<ObjectId>();
		run(client, "channel.set", {{"channel", setup.channel}, {"recordEnable", false}});
		run(client, "channel.set",
			{{"channel", midi}, {"output", setup.channel}, {"midiChannel", 4}, {"recordEnable", true}});
		run(client, "audio.render", {{"frames", 64}});
		run(client, "midi.capture", {{"start", true}});
		setup.play(*setup.mpe, {{0xE1, 0, 96}, {0x91, 60, 100}});
		setup.play(*setup.mpe, {{0xD1, 40}});
		setup.play(*setup.mpe, {{0x81, 60, 0}});
		CHECK(captured(client) ==
			std::vector<std::vector<int64_t>>{{0xE4, 0, 80}, {0x94, 60, 100}, {0xD4, 40}, {0x84, 60, 0}});
	}
}

TEST_CASE("recording records an MPE port's input as the notes' expression", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	run(client, "settings.set", {{"key", "midi.snapPartsToBars"}, {"value", false}});
	run(client, "midi.setPort",
		{{"id", "MPE"}, {"input", true}, {"mpe", {{"mode", "upper"}, {"pitchRange", 24}, {"initialY", "center"}}}});
	run(client, "transport.record");
	run(client, "audio.render", {{"frames", 64}});
	// The upper zone: the master channel 16's messages record as controller events, the notes' channels' as their
	// expression; CC 74 starts at its center.
	setup.play(*setup.mpe, {{0xEE, 0, 96}, {0x9E, 60, 100}}, 640);
	setup.play(*setup.mpe, {{0xEE, 0, 80}, {0xDE, 50}, {0xBF, 1, 64}}, 640);
	setup.play(*setup.mpe, {{0x8E, 60, 0}}, 640);
	run(client, "transport.stop");
	run(client, "audio.render", {{"frames", 640}});
	REQUIRE(waitFor(client, "recording.added").is_object());
	const Json part = run(client, "project.get").at("midiParts").at(0);
	REQUIRE(part.at("notes").size() == 1);
	const Json& note = part.at("notes")[0];
	CHECK(note.at("channel") == 14);
	const Points points = pointsOf(note);
	REQUIRE(points.size() == 4);
	const int64_t later = std::get<1>(points[1]); // where the second messages came
	CHECK(later > 0);
	CHECK(points ==
		Points{{"brightness", 0, unitOf(64)}, {"pressure", later, unitOf(50)}, {"tuning", 0, semitonesOf(96, 24)},
			{"tuning", later, semitonesOf(80, 24)}});
	REQUIRE(part.at("controllers").size() == 1);
	CHECK(part.at("controllers")[0].at("channel") == 15);
}

TEST_CASE("retrospective recording keeps an MPE port's input as the notes' expression", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	run(client, "midi.setPort",
		{{"id", "MPE"}, {"input", true}, {"mpe", {{"mode", "rotation"}, {"channels", 16}, {"pitchRange", 24}}}});
	run(client, "channel.set", {{"channel", setup.channel}, {"recordEnable", false}, {"monitor", true}});
	// Channel rotation: channel 1 is a note's too.
	setup.play(*setup.mpe, {{0xE0, 0, 96}, {0x90, 60, 100}}, 640);
	setup.play(*setup.mpe, {{0x80, 60, 0}}, 640);
	run(client, "midi.insertRetrospective", {{"track", setup.track}});
	const Json part = run(client, "project.get").at("midiParts").at(0);
	REQUIRE(part.at("notes").size() == 1);
	CHECK(part.at("controllers").empty());
	CHECK(pointsOf(part.at("notes").at(0)) == Points{{"tuning", 0, semitonesOf(96, 24)}});
}

TEST_CASE("MPE input is recorded as per-note expression and played back to an MPE instrument", "[integration]")
{
	Setup setup;
	Client& client = setup.client;
	run(client, "settings.set", {{"key", "midi.snapPartsToBars"}, {"value", false}});
	run(client, "midi.setPort", {{"id", "MPE"}, {"input", true}, {"mpe", {{"mode", "lower"}}}});
	REQUIRE(run(client, "audio.get").at("position") == 0);
	CHECK(run(client, "transport.record").at("recording") == true);
	// MPE's lower zone: channel 1 (0) is the master channel, the notes come on channels 2 to 16, each with its own
	// pitch bend, channel pressure and CC 74, sent before its note-on for its initial state. Each message plays at a
	// block's start.
	setup.renderTo(896);
	setup.play(*setup.mpe, {{0xE1, 0, 96}, {0xD1, 40}, {0xB1, 74, 64}});
	setup.play(*setup.mpe, {{0x91, 60, 100}});
	setup.renderTo(1472);
	setup.play(*setup.mpe, {{0x92, 64, 90}});
	setup.renderTo(1984);
	setup.play(*setup.mpe, {{0xE1, 0, 80}});
	setup.renderTo(2240);
	setup.play(*setup.mpe, {{0xD2, 100}});
	setup.renderTo(2368);
	setup.play(*setup.mpe, {{0xB0, 1, 64}});
	setup.renderTo(2496);
	setup.play(*setup.mpe, {{0xB1, 74, 100}});
	setup.renderTo(3008);
	setup.play(*setup.mpe, {{0x81, 60, 0}});
	// No note sounds on channel 2: its state for the next note, nothing recorded.
	setup.renderTo(3200);
	setup.play(*setup.mpe, {{0xE1, 0, 64}});
	setup.renderTo(3520);
	setup.play(*setup.mpe, {{0x82, 64, 0}});
	setup.renderTo(4032);
	run(client, "transport.stop");
	run(client, "audio.render", {{"frames", 640}});
	REQUIRE(waitFor(client, "recording.added").is_object());

	const Json parts = run(client, "project.get").at("midiParts");
	REQUIRE(parts.size() == 1);
	const Json& part = parts[0];
	const int64_t partStart = part.at("position").get<int64_t>();
	REQUIRE(part.at("notes").size() == 2);
	Json first = part.at("notes")[0];
	Json second = part.at("notes")[1];
	if (first.at("pitch") != 60)
		std::swap(first, second);
	CHECK(first.at("position").get<int64_t>() + partStart == ticksOf(960));
	CHECK(first.at("channel") == 1);
	// Pitch bend is tuning in semitones, by the MPE pitch range of 48; channel pressure is pressure, CC 74 brightness.
	CHECK(pointsOf(first) ==
		Points{{"brightness", 0, unitOf(64)}, {"brightness", ticksOf(1536), unitOf(100)}, {"pressure", 0, unitOf(40)},
			{"tuning", 0, semitonesOf(96, 48)}, {"tuning", ticksOf(1024), semitonesOf(80, 48)}});
	CHECK(semitonesOf(96, 48) == 24003); // a quarter of 14 bits up, by MIDI 2.0's scaling
	CHECK(unitOf(64) == 500);
	CHECK(second.at("channel") == 2);
	CHECK(pointsOf(second) == Points{{"pressure", ticksOf(768), unitOf(100)}});
	// The master channel's messages stay controller events.
	REQUIRE(part.at("controllers").size() == 1);
	CHECK(part.at("controllers")[0].at("type") == "controller");
	CHECK(part.at("controllers")[0].at("number") == 1);

	// Played back, the instrument gets each note on its channel with its expression as that channel's pitch bend,
	// channel pressure and CC 74, where it was played. The second note wasn't pressed before its note-on: it starts
	// without pressure, MPE's initial value.
	const auto start = run(client, "audio.get").at("position").get<int64_t>();
	run(client, "midi.capture", {{"start", true}});
	run(client, "transport.locate", {{"position", 0}});
	run(client, "transport.play");
	run(client, "audio.render", {{"frames", 4000}});
	std::vector<std::vector<int64_t>> played;
	const Json capture = run(client, "midi.capture");
	for (const Json& each : capture.at("captured"))
	{
		std::vector<int64_t> entry{each.at("position").get<int64_t>() - start};
		for (const Json& byte : each.at("bytes"))
			entry.push_back(byte.get<int64_t>());
		played.push_back(std::move(entry));
	}
	std::sort(played.begin(), played.end());
	CHECK(played ==
		std::vector<std::vector<int64_t>>{{960, 0x91, 60, 100}, {960, 0xB1, 74, 64}, {960, 0xD1, 40},
			{960, 0xE1, 0, 96}, {1472, 0x92, 64, 90}, {1472, 0xD2, 0}, {1984, 0xE1, 0, 80}, {2240, 0xD2, 100},
			{2368, 0xB0, 1, 64}, {2496, 0xB1, 74, 100}, {3008, 0x81, 60, 0}, {3520, 0x82, 64, 0}});
}
