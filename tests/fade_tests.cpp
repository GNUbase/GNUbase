// SPDX-License-Identifier: GPL-3.0-or-later
// Fades and crossfades (#138): the curves, the commands, and playback that applies them sample by sample.
#include "engine/audio/fades.h"
#include "engine/document/document.h"
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

namespace
{
	constexpr double g_pi = 3.14159265358979323846;

	// The curves as the documentation gives them, written out again: gain from 0 to 1 over x from 0 to 1.
	double curve(const std::string& shape, double x)
	{
		x = std::clamp(x, 0.0, 1.0);
		const auto exponential = [](double v) { return (std::exp(3.0 * v) - 1.0) / (std::exp(3.0) - 1.0); };
		if (shape == "exponential")
			return exponential(x);
		if (shape == "logarithmic")
			return 1.0 - exponential(1.0 - x);
		if (shape == "sCurve")
			return (1.0 - std::cos(g_pi * x)) / 2.0;
		if (shape == "equalPower")
			return std::sin(g_pi * x / 2.0);
		return x;
	}

	// A fade in over [start, start + length), a fade out over [end - length, end), at a project sample.
	double fadeIn(const std::string& shape, int64_t at, int64_t start, int64_t length)
	{
		if (at < start)
			return 0.0;
		return curve(shape, static_cast<double>(at - start) / static_cast<double>(length));
	}

	double fadeOut(const std::string& shape, int64_t at, int64_t end, int64_t length)
	{
		if (at >= end)
			return 0.0;
		return curve(shape, static_cast<double>(end - at) / static_cast<double>(length));
	}

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
		REQUIRE(!result.ok());
		return result.error;
	}

	ObjectId idOf(const Json& result, const char* kind)
	{
		return result.at(kind).at("id").get<ObjectId>();
	}

	// A clip whose sample at frame f, channel c is value(f, c).
	std::string writeClip(const gnubase::test::TempDir& dir, const std::string& name, int channels, int64_t frames,
		const std::function<float(int64_t, int)>& value)
	{
		gnubase::test::Audio audio;
		audio.channels = channels;
		for (int64_t f = 0; f < frames; ++f)
			for (int c = 0; c < channels; ++c)
				audio.samples.push_back(value(f, c));
		const std::filesystem::path file = dir.path() / (name + ".wav");
		gnubase::test::writeWav(file, audio);
		return file.u8string();
	}

	float one(int64_t /*frame*/, int /*channel*/)
	{
		return 1.0f;
	}

	// Exact in float: (frame + 1) / 65536.
	float ramp(int64_t frame, int /*channel*/ = 0)
	{
		return static_cast<float>(frame + 1) / 65536.0f;
	}

	float sine(double frequency, int64_t frame)
	{
		return static_cast<float>(0.5 * std::sin(2.0 * g_pi * frequency * static_cast<double>(frame) / 48000.0));
	}

	// An engine with a dummy device and linear audio tracks, at 48 kHz.
	class Session
	{
	public:
		explicit Session(int blockSize = 64)
		{
			run(client, "audio.setDevice", {{"blockSize", blockSize}, {"inputChannels", 0}, {"captureSeconds", 5}});
		}

		ObjectId addTrack(int channels = 2)
		{
			return idOf(
				run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}, {"channels", channels}}), "track");
		}

		ObjectId addClip(const std::string& path)
		{
			return idOf(run(client, "clip.add", {{"media", idOf(run(client, "media.add", {{"path", path}}), "media")}}),
				"clip");
		}

		ObjectId addEvent(ObjectId track, ObjectId clip, int64_t position, int64_t length, int64_t offset = 0)
		{
			return idOf(
				run(client, "audioEvent.add",
					{{"track", track}, {"clip", clip}, {"position", position}, {"offset", offset}, {"length", length}}),
				"event");
		}

		// Plays frames from position and returns what each output channel got.
		std::vector<std::vector<float>> render(int64_t frames, int64_t position = 0)
		{
			run(client, "transport.locate", {{"position", position}});
			run(client, "transport.play");
			run(client, "audio.render", {{"frames", frames}});
			run(client, "transport.stop");
			std::vector<std::vector<float>> output;
			output.reserve(2);
			for (int channel = 0; channel < 2; ++channel)
				output.push_back(
					run(client, "audio.capture", {{"channel", channel}, {"start", m_rendered}, {"frames", frames}})
						.at("samples")
						.get<std::vector<float>>());
			m_rendered += frames;
			CHECK(run(client, "audio.get").at("playbackDropouts") == 0);
			return output;
		}

		Engine engine;
		Client client{engine};

	private:
		int64_t m_rendered = 0; // the device's capture keeps every render
	};

	// Checks every sample against expected(position), reporting the first wrong one.
	void checkSamples(const std::vector<float>& output, const std::function<double(int64_t)>& expected,
		double tolerance = 1e-6, int64_t first = 0)
	{
		size_t wrong = 0;
		size_t at = 0;
		for (size_t i = 0; i < output.size(); ++i)
			if (std::abs(output[i] - expected(first + static_cast<int64_t>(i))) > tolerance && wrong++ == 0)
				at = i;
		INFO(wrong << " wrong samples, the first at position " << (first + static_cast<int64_t>(at)) << ": "
				   << output[at] << " instead of " << expected(first + static_cast<int64_t>(at)));
		CHECK(wrong == 0);
	}

	gnubase::test::Audio mono(const std::vector<float>& samples)
	{
		gnubase::test::Audio audio;
		audio.channels = 1;
		audio.samples = samples;
		return audio;
	}
}

TEST_CASE("fade curves rise from 0 to 1 with the documented shapes", "[unit]")
{
	const std::vector<std::pair<FadeShape, std::string>> shapes = {{FadeShape::Linear, "linear"},
		{FadeShape::Exponential, "exponential"}, {FadeShape::Logarithmic, "logarithmic"}, {FadeShape::SCurve, "sCurve"},
		{FadeShape::EqualPower, "equalPower"}};
	for (const auto& [shape, name] : shapes)
	{
		INFO(name);
		CHECK(fadeGain(shape, 0.0) == 0.0);
		CHECK_THAT(fadeGain(shape, 1.0), WithinAbs(1.0, 1e-15));
		CHECK(fadeGain(shape, -1.0) == 0.0);
		CHECK_THAT(fadeGain(shape, 2.0), WithinAbs(1.0, 1e-15));
		double previous = 0.0;
		for (int i = 1; i <= 1000; ++i)
		{
			const double x = i / 1000.0;
			const double gain = fadeGain(shape, x);
			REQUIRE(gain > previous);
			REQUIRE_THAT(gain, WithinAbs(curve(name, x), 1e-12));
			previous = gain;
		}
	}
	// Half way: linear and sCurve -6 dB, equal power -3 dB, exponential about -15 dB, logarithmic about -1.7 dB.
	CHECK(fadeGain(FadeShape::Linear, 0.5) == 0.5);
	CHECK_THAT(fadeGain(FadeShape::SCurve, 0.5), WithinAbs(0.5, 1e-15));
	CHECK_THAT(fadeGain(FadeShape::EqualPower, 0.5), WithinAbs(std::sqrt(0.5), 1e-15));
	CHECK_THAT(20.0 * std::log10(fadeGain(FadeShape::Exponential, 0.5)), WithinAbs(-14.78, 0.01));
	CHECK_THAT(20.0 * std::log10(fadeGain(FadeShape::Logarithmic, 0.5)), WithinAbs(-1.75, 0.01));
}

TEST_CASE("mirrored fade shapes add up to 1, and equal power fades keep the power at 1", "[unit]")
{
	for (const FadeShape shape : {FadeShape::Linear, FadeShape::Exponential, FadeShape::Logarithmic, FadeShape::SCurve})
	{
		FadeShape mirror = FadeShape::EqualPower;
		REQUIRE(mirrorShape(shape, mirror));
		for (int i = 0; i <= 100; ++i)
		{
			// A fade in at x and the mirrored fade out at the same moment, whose own progress is 1 - x.
			const double x = i / 100.0;
			CHECK_THAT(fadeGain(shape, x) + fadeGain(mirror, 1.0 - x), WithinAbs(1.0, 1e-12));
		}
	}
	FadeShape mirror = FadeShape::Linear;
	// Not CHECK_FALSE: Catch2's ContinueOnFailure | FalseTest trips clang-analyzer's EnumCastOutOfRange check
	CHECK(!mirrorShape(FadeShape::EqualPower, mirror));
	for (int i = 0; i <= 100; ++i)
	{
		const double x = i / 100.0;
		const double in = fadeGain(FadeShape::EqualPower, x);
		const double out = fadeGain(FadeShape::EqualPower, 1.0 - x);
		CHECK_THAT((in * in) + (out * out), WithinAbs(1.0, 1e-12));
	}
}

TEST_CASE("audio events and clips have fades, which the commands check and undo restores", "[unit]")
{
	const gnubase::test::TempDir dir;
	Session session;
	Client& client = session.client;
	const ObjectId track = session.addTrack();
	const ObjectId clip = session.addClip(writeClip(dir, "one", 1, 10000, one));
	const ObjectId event = session.addEvent(track, clip, 0, 4000);

	const Json added = run(client, "project.get").at("audioEvents")[0];
	CHECK(added.at("fadeIn") == Json{{"length", 0}, {"shape", "linear"}});
	CHECK(added.at("fadeOut") == Json{{"length", 0}, {"shape", "linear"}});
	CHECK(added.at("crossfade").is_null());

	const Json changed = run(client, "audioEvent.setFades",
		{{"event", event}, {"fadeIn", {{"length", 1000}, {"shape", "sCurve"}}}, {"fadeOut", {{"length", 500}}}})
							 .at("event");
	CHECK(changed.at("fadeIn") == Json{{"length", 1000}, {"shape", "sCurve"}});
	CHECK(changed.at("fadeOut") == Json{{"length", 500}, {"shape", "linear"}});
	// What isn't given stays.
	CHECK(run(client, "audioEvent.setFades", {{"event", event}, {"fadeOut", {{"shape", "exponential"}}}})
			  .at("event")
			  .at("fadeIn") == Json{{"length", 1000}, {"shape", "sCurve"}});

	CHECK_THAT(reject(client, "audioEvent.setFades", {{"event", event}, {"fadeIn", {{"length", 3600}}}}),
		ContainsSubstring("fit into the event"));
	CHECK_THAT(reject(client, "audioEvent.setFades", {{"event", event}, {"fadeIn", {{"length", -1}}}}),
		ContainsSubstring("must not be negative"));
	CHECK(!client.call("audioEvent.setFades", {{"event", event}, {"fadeIn", {{"shape", "cubic"}}}}).ok());
	CHECK_THAT(reject(client, "audioEvent.setFades", {{"event", 999}}), ContainsSubstring("no audio event 999"));

	const Json clipFades =
		run(client, "clip.setFades", {{"clip", clip}, {"fadeIn", {{"length", 2000}, {"shape", "equalPower"}}}})
			.at("clip");
	CHECK(clipFades.at("fadeIn") == Json{{"length", 2000}, {"shape", "equalPower"}});
	CHECK_THAT(reject(client, "clip.setFades", {{"clip", clip}, {"fadeOut", {{"length", 9000}}}}),
		ContainsSubstring("fit into the clip"));

	// Each command is an undo step.
	run(client, "history.undo", {{"steps", 2}});
	const Json undone = run(client, "project.get");
	CHECK(undone.at("audioEvents")[0].at("fadeOut") == Json{{"length", 500}, {"shape", "linear"}});
	CHECK(undone.at("clips")[0].at("fadeIn") == Json{{"length", 0}, {"shape", "linear"}});
	run(client, "history.undo");
	CHECK(run(client, "project.get").at("audioEvents")[0] == added);
}

TEST_CASE("crossfades join overlapping events on a track", "[unit]")
{
	const gnubase::test::TempDir dir;
	Session session;
	Client& client = session.client;
	const ObjectId track = session.addTrack();
	const ObjectId other = session.addTrack();
	const ObjectId clip = session.addClip(writeClip(dir, "one", 1, 10000, one));
	const ObjectId a = session.addEvent(track, clip, 0, 3000);
	const ObjectId b = session.addEvent(track, clip, 2000, 3000);
	const ObjectId c = session.addEvent(track, clip, 5000, 1000); // right after b, without overlap
	const ObjectId d = session.addEvent(other, clip, 2500, 1000);

	// The default: symmetric over the whole overlap, equal gain and linear.
	const Json crossfade = run(client, "crossfade.set", {{"from", a}, {"to", b}}).at("event").at("crossfade");
	CHECK(crossfade ==
		Json{{"from", a}, {"mode", "equalGain"}, {"shape", "linear"}, {"outShape", "linear"}, {"length", 0},
			{"outLength", 0}});
	// Equal gain mirrors the fade in; equal power is quarter sines; custom takes both shapes.
	CHECK(run(client, "crossfade.set", {{"from", a}, {"to", b}, {"shape", "exponential"}})
			  .at("event")
			  .at("crossfade")
			  .at("outShape") == "logarithmic");
	const Json power =
		run(client, "crossfade.set", {{"from", a}, {"to", b}, {"mode", "equalPower"}}).at("event").at("crossfade");
	CHECK(power.at("shape") == "equalPower");
	CHECK(power.at("outShape") == "equalPower");
	const Json custom = run(client, "crossfade.set",
		{{"from", a}, {"to", b}, {"mode", "custom"}, {"shape", "sCurve"}, {"outShape", "exponential"}, {"length", 400},
			{"outLength", 1000}})
							.at("event")
							.at("crossfade");
	CHECK(custom ==
		Json{{"from", a}, {"mode", "custom"}, {"shape", "sCurve"}, {"outShape", "exponential"}, {"length", 400},
			{"outLength", 1000}});

	CHECK_THAT(reject(client, "crossfade.set", {{"from", b}, {"to", c}}), ContainsSubstring("must start inside"));
	CHECK_THAT(reject(client, "crossfade.set", {{"from", b}, {"to", a}}), ContainsSubstring("must start inside"));
	CHECK_THAT(reject(client, "crossfade.set", {{"from", a}, {"to", d}}), ContainsSubstring("same track"));
	CHECK_THAT(reject(client, "crossfade.set", {{"from", a}, {"to", a}}), ContainsSubstring("itself"));
	CHECK_THAT(reject(client, "crossfade.set", {{"from", a}, {"to", b}, {"length", 1001}}),
		ContainsSubstring("length must be 0 (the whole overlap) to 1000"));
	CHECK_THAT(
		reject(client, "crossfade.set", {{"from", a}, {"to", b}, {"mode", "equalGain"}, {"shape", "equalPower"}}),
		ContainsSubstring("equal gain"));
	CHECK_THAT(reject(client, "crossfade.set", {{"from", a}, {"to", b}, {"mode", "equalGain"}, {"outShape", "linear"}}),
		ContainsSubstring("outShape"));
	CHECK_THAT(reject(client, "crossfade.set", {{"from", a}, {"to", b}, {"mode", "equalPower"}, {"shape", "linear"}}),
		ContainsSubstring("quarter sines"));

	// Removing either event of a crossfade removes it.
	run(client, "audioEvent.remove", {{"event", a}});
	CHECK(run(client, "project.get").at("audioEvents")[0].at("crossfade").is_null());
	run(client, "history.undo");
	CHECK(run(client, "project.get").at("audioEvents")[1].at("crossfade").at("from") == a);
	run(client, "crossfade.remove", {{"event", a}});
	CHECK(run(client, "project.get").at("audioEvents")[1].at("crossfade").is_null());
	CHECK_THAT(reject(client, "crossfade.remove", {{"event", a}}), ContainsSubstring("has no crossfade"));
}

TEST_CASE("auto fade settings belong to the project or to an audio track", "[unit]")
{
	Session session;
	Client& client = session.client;
	const ObjectId track = session.addTrack();
	const ObjectId midi = idOf(run(client, "track.add", {{"type", "midi"}}), "track");
	const Json defaults = run(client, "project.get").at("autoFades");
	CHECK(defaults ==
		Json{{"fadeIn", false}, {"fadeOut", false}, {"crossfades", false}, {"length", 10.0}, {"fadeInShape", "linear"},
			{"fadeOutShape", "linear"}, {"crossfadeMode", "equalGain"}, {"crossfadeShape", "linear"}});

	const Json project = run(client, "autoFades.set", {{"fadeIn", true}, {"length", 5.0}}).at("autoFades");
	CHECK(project.at("fadeIn") == true);
	CHECK(project.at("length") == 5.0);
	CHECK(run(client, "project.get").at("tracks")[0].at("autoFades").is_null());

	// A track's own settings start from the project's.
	const Json own =
		run(client, "autoFades.set", {{"track", track}, {"crossfades", true}, {"crossfadeMode", "equalPower"}})
			.at("autoFades");
	CHECK(own.at("fadeIn") == true);
	CHECK(own.at("crossfades") == true);
	CHECK(run(client, "project.get").at("tracks")[0].at("autoFades") == own);
	CHECK(run(client, "autoFades.set", {{"track", track}, {"useProject", true}}).at("autoFades") == project);
	CHECK(run(client, "project.get").at("tracks")[0].at("autoFades").is_null());

	CHECK_THAT(reject(client, "autoFades.set", {{"length", 0.5}}), ContainsSubstring("1 to 500 ms"));
	CHECK_THAT(reject(client, "autoFades.set", {{"length", 501.0}}), ContainsSubstring("1 to 500 ms"));
	CHECK_THAT(reject(client, "autoFades.set", {{"crossfadeMode", "custom"}}), ContainsSubstring("equal gain or"));
	CHECK_THAT(reject(client, "autoFades.set", {{"track", midi}, {"fadeIn", true}}), ContainsSubstring("audio tracks"));
	CHECK_THAT(reject(client, "autoFades.set", {{"useProject", true}}), ContainsSubstring("for tracks"));
	CHECK_THAT(reject(client, "autoFades.set", {{"track", track}, {"useProject", true}, {"fadeIn", true}}),
		ContainsSubstring("useProject"));

	// Undo puts the project's settings back and announces them.
	run(client, "history.undo", {{"steps", 2}});
	while (client.poll())
	{
	}
	run(client, "history.undo");
	CHECK(run(client, "project.get").at("autoFades") == defaults);
	bool announced = false;
	while (const auto message = client.poll())
		if (const auto* event = std::get_if<Event>(&*message))
			announced = announced || (event->name == "project.changed" && event->data.at("autoFades") == defaults);
	CHECK(announced);
}

TEST_CASE("fades shape an event's start and end sample by sample, for any block size", "[integration]")
{
	const int blockSize = GENERATE(1, 64, 333);
	const std::string shape = GENERATE("linear", "exponential", "logarithmic", "sCurve", "equalPower");
	INFO("block size " << blockSize << ", " << shape);
	const gnubase::test::TempDir dir;
	Session session(blockSize);
	const ObjectId track = session.addTrack();
	const ObjectId clip = session.addClip(writeClip(dir, "one", 1, 10000, one));
	const ObjectId event = session.addEvent(track, clip, 500, 4000, 2000);
	run(session.client, "audioEvent.setFades",
		{{"event", event}, {"fadeIn", {{"length", 1500}, {"shape", shape}}},
			{"fadeOut", {{"length", 1000}, {"shape", shape}}}});
	run(session.client, "audioEvent.set", {{"event", event}, {"volume", -6.0}});

	const auto output = session.render(5000);
	const double volume = std::pow(10.0, -6.0 / 20.0);
	checkSamples(output[0],
		[&](int64_t at)
		{
			if (at < 500 || at >= 4500)
				return 0.0;
			return volume * fadeIn(shape, at, 500, 1500) * fadeOut(shape, at, 4500, 1000);
		});
	CHECK(output[1] == output[0]);
}

TEST_CASE("equal gain and equal power crossfades keep the gain or the power constant", "[integration]")
{
	const int blockSize = GENERATE(1, 100);
	INFO("block size " << blockSize);
	const gnubase::test::TempDir dir;
	Session session(blockSize);
	const ObjectId track = session.addTrack();
	// The event fading out plays on the left, the one fading in on the right.
	const ObjectId left =
		session.addClip(writeClip(dir, "left", 2, 5000, [](int64_t, int c) { return c == 0 ? 1.0f : 0.0f; }));
	const ObjectId right =
		session.addClip(writeClip(dir, "right", 2, 5000, [](int64_t, int c) { return c == 1 ? 1.0f : 0.0f; }));
	const ObjectId a = session.addEvent(track, left, 0, 3000);
	const ObjectId b = session.addEvent(track, right, 2000, 3000);

	SECTION("equal gain, linear and exponential")
	{
		for (const std::string shape : {"linear", "exponential", "sCurve"})
		{
			INFO(shape);
			run(session.client, "crossfade.set", {{"from", a}, {"to", b}, {"mode", "equalGain"}, {"shape", shape}});
			const auto output = session.render(5000);
			checkSamples(output[1], [&](int64_t at) { return at < 5000 ? fadeIn(shape, at, 2000, 1000) : 0.0; });
			for (size_t i = 0; i < 5000; ++i)
				REQUIRE_THAT(output[0][i] + output[1][i], WithinAbs(1.0, 1e-6));
		}
	}
	SECTION("equal power")
	{
		run(session.client, "crossfade.set", {{"from", a}, {"to", b}, {"mode", "equalPower"}});
		const auto output = session.render(5000);
		checkSamples(output[0], [&](int64_t at) { return fadeOut("equalPower", at, 3000, 1000); });
		checkSamples(output[1], [&](int64_t at) { return at < 5000 ? fadeIn("equalPower", at, 2000, 1000) : 0.0; });
		for (size_t i = 0; i < 5000; ++i)
		{
			const double l = output[0][i];
			const double r = output[1][i];
			REQUIRE_THAT((l * l) + (r * r), WithinAbs(1.0, 1e-6));
		}
	}
	SECTION("asymmetric, with shapes of its own")
	{
		run(session.client, "crossfade.set",
			{{"from", a}, {"to", b}, {"mode", "custom"}, {"shape", "sCurve"}, {"outShape", "logarithmic"},
				{"length", 400}, {"outLength", 700}});
		const auto output = session.render(5000);
		checkSamples(output[0], [&](int64_t at) { return fadeOut("logarithmic", at, 3000, 700); });
		checkSamples(output[1], [&](int64_t at) { return at < 5000 ? fadeIn("sCurve", at, 2000, 400) : 0.0; });
	}
	SECTION("the event in front can be the one fading out")
	{
		const ObjectId c = session.addEvent(track, left, 0, 3000); // added after b, so in front of it
		run(session.client, "audioEvent.remove", {{"event", a}});
		run(session.client, "crossfade.set", {{"from", c}, {"to", b}});
		const auto output = session.render(5000);
		checkSamples(output[0], [&](int64_t at) { return fadeOut("linear", at, 3000, 1000); });
		checkSamples(output[1], [&](int64_t at) { return at < 5000 ? fadeIn("linear", at, 2000, 1000) : 0.0; });
	}
}

TEST_CASE("a crossfade of a clip with itself plays it seamlessly, also from inside and in cycles", "[integration]")
{
	const int blockSize = GENERATE(1, 64, 100, 333);
	INFO("block size " << blockSize);
	const gnubase::test::TempDir dir;
	Session session(blockSize);
	const ObjectId track = session.addTrack();
	const ObjectId clip = session.addClip(writeClip(dir, "ramp", 1, 5000, ramp));
	// As after splitting an event at 1000 and dragging both ends over the cut: the same frames overlap.
	const ObjectId a = session.addEvent(track, clip, 0, 1100);
	const ObjectId b = session.addEvent(track, clip, 900, 1100, 900);
	run(session.client, "crossfade.set", {{"from", a}, {"to", b}});
	const auto expected = [](int64_t at) { return at < 2000 ? ramp(at) : 0.0; };

	checkSamples(session.render(2500)[0], expected);
	checkSamples(session.render(1000, 1000)[0], expected, 1e-6, 1000);
	// Cycles that play across the crossfade, entering it after each jump.
	// Cycles that jump into and out of the crossfade.
	run(session.client, "transport.set", {{"left", 850}, {"right", 1150}, {"cycle", true}});
	const auto output = session.render(1000, 800);
	std::vector<int64_t> positions;
	for (int64_t at = 800; positions.size() < 1000;)
	{
		positions.push_back(at);
		at = at + 1 == 1150 ? 850 : at + 1;
	}
	for (size_t i = 0; i < positions.size(); ++i)
		REQUIRE_THAT(output[0][i], WithinAbs(ramp(positions[i]), 1e-6));
}

TEST_CASE("auto fades and crossfades apply where events have none of their own", "[integration]")
{
	const gnubase::test::TempDir dir;
	Session session(64);
	Client& client = session.client;
	const ObjectId track = session.addTrack();
	const ObjectId other = session.addTrack();
	const ObjectId left =
		session.addClip(writeClip(dir, "left", 2, 9000, [](int64_t, int c) { return c == 0 ? 1.0f : 0.0f; }));
	const ObjectId right =
		session.addClip(writeClip(dir, "right", 2, 9000, [](int64_t, int c) { return c == 1 ? 1.0f : 0.0f; }));
	constexpr int64_t n = 480; // 10 ms at 48 kHz

	SECTION("auto fades, and an event's own fade instead")
	{
		run(client, "autoFades.set", {{"fadeIn", true}, {"fadeOut", true}, {"fadeOutShape", "sCurve"}});
		const ObjectId event = session.addEvent(track, left, 1000, 3000);
		run(client, "audioEvent.setFades", {{"event", event}, {"fadeIn", {{"length", 200}}}});
		const auto output = session.render(5000);
		checkSamples(output[0], [&](int64_t at)
			{ return at < 4000 ? fadeIn("linear", at, 1000, 200) * fadeOut("sCurve", at, 4000, n) : 0.0; });

		// A track with settings of its own.
		run(client, "autoFades.set", {{"track", track}, {"fadeOut", false}});
		checkSamples(
			session.render(5000)[0], [&](int64_t at) { return at < 4000 ? fadeIn("linear", at, 1000, 200) : 0.0; });
	}
	SECTION("auto crossfades where an event starts on top of another and ends above one")
	{
		run(client, "autoFades.set", {{"track", track}, {"crossfades", true}});
		run(client, "autoFades.set", {{"track", other}, {"crossfades", false}});
		session.addEvent(track, left, 0, 3000);
		session.addEvent(track, right, 2000, 3000); // starts on top of the first
		session.addEvent(track, right, 6000, 3000); // behind the next one
		session.addEvent(track, left, 6500, 1000);  // starts and ends on top of it
		const auto output = session.render(9000);
		checkSamples(output[0],
			[&](int64_t at)
			{
				if (at < 2000)
					return 1.0;
				if (at < 2000 + n)
					return fadeOut("linear", at, 2000 + n, n);
				if (at < 6500)
					return 0.0;
				return at < 7500 ? fadeIn("linear", at, 6500, n) * fadeOut("linear", at, 7500, n) : 0.0;
			});
		checkSamples(output[1],
			[&](int64_t at)
			{
				if (at < 2000)
					return 0.0;
				if (at < 5000)
					return fadeIn("linear", at, 2000, n);
				if (at < 6000)
					return 0.0;
				if (at < 6500)
					return 1.0;
				if (at < 7500)
					return fadeOut("linear", at, 6500 + n, n) + fadeIn("linear", at, 7500 - n, n);
				return 1.0;
			});
	}
}

TEST_CASE("an event fading out plays on behind every event in front of it until its fade ends", "[integration]")
{
	const int blockSize = GENERATE(1, 64, 333);
	INFO("block size " << blockSize);
	const gnubase::test::TempDir dir;
	Session session(blockSize);
	Client& client = session.client;
	const ObjectId track = session.addTrack();
	const ObjectId clip = session.addClip(writeClip(dir, "one", 1, 10000, one));

	SECTION("crossfades into an event that crossfades on into a third one")
	{
		// a fades out under b until 1000, but c takes the front from b at 800.
		const ObjectId a = session.addEvent(track, clip, 0, 1000);
		const ObjectId b = session.addEvent(track, clip, 500, 700);
		const ObjectId c = session.addEvent(track, clip, 800, 1200);
		run(client, "crossfade.set", {{"from", a}, {"to", b}});
		run(client, "crossfade.set", {{"from", b}, {"to", c}});
		const auto output = session.render(2500);
		const auto gainA = [](int64_t at) { return at < 1000 ? fadeOut("linear", at, 1000, 500) : 0.0; };
		const auto gainB = [](int64_t at)
		{ return at >= 500 && at < 1200 ? fadeIn("linear", at, 500, 500) * fadeOut("linear", at, 1200, 400) : 0.0; };
		const auto gainC = [](int64_t at) { return at < 2000 ? fadeIn("linear", at, 800, 400) : 0.0; };
		checkSamples(output[0], [&](int64_t at) { return gainA(at) + gainB(at) + gainC(at); });
		// Where two of them play, their gains add up to 1; where all three do, the third one comes on top of that,
		// rising from 0 and falling back to 0 without a jump.
		for (int64_t at = 0; at < 2000; ++at)
			if (at < 800 || at >= 1000)
				REQUIRE_THAT(output[0][static_cast<size_t>(at)], WithinAbs(1.0, 1e-6));
		for (size_t i = 1; i < 2000; ++i)
			REQUIRE(std::abs(output[0][i] - output[0][i - 1]) < 0.01f);
	}
	SECTION("auto crossfades of events that start within the auto fade length")
	{
		constexpr int64_t n = 480; // 10 ms at 48 kHz
		run(client, "autoFades.set", {{"crossfades", true}});
		session.addEvent(track, clip, 0, 3000);
		session.addEvent(track, clip, 1000, 3000);
		session.addEvent(track, clip, 1200, 3800);
		const auto output = session.render(5500);
		// The first one plays on behind the third one while the second one does, and fades out with it, so the
		// gains add up to 1 throughout.
		checkSamples(output[0],
			[&](int64_t at)
			{
				if (at >= 5000)
					return 0.0;
				const double behindThird = at < 1200 ? 1.0 : fadeOut("linear", at, 1200 + n, n);
				const double x = at < 1000 + n ? fadeOut("linear", at, 1000 + n, n) * behindThird : 0.0;
				const double y = at >= 1000 && at < 1200 + n ? fadeIn("linear", at, 1000, n) * behindThird : 0.0;
				const double z = at >= 1200 ? fadeIn("linear", at, 1200, n) : 0.0;
				return x + y + z;
			});
		for (size_t i = 0; i < 5000; ++i)
			REQUIRE_THAT(output[0][i], WithinAbs(1.0, 1e-6));
	}
}

TEST_CASE("an event on top of a crossfade hides the events it joins", "[integration]")
{
	// A new take over a crossfaded join: where it is in front, the crossfaded events play behind it only as far as
	// its own transitions reach.
	const int blockSize = GENERATE(1, 64, 333);
	INFO("block size " << blockSize);
	const gnubase::test::TempDir dir;
	Session session(blockSize);
	Client& client = session.client;
	const ObjectId track = session.addTrack();
	// Each event plays a clip of its own level, so the output shows which ones play: exact in float.
	const auto level = [&](const std::string& name, float value)
	{ return session.addClip(writeClip(dir, name, 1, 3000, [value](int64_t, int) { return value; })); };
	const ObjectId first = level("first", 0.125f);
	const ObjectId second = level("second", 0.25f);
	const ObjectId take = level("take", 0.5f);
	const ObjectId a = session.addEvent(track, first, 0, 1000);
	const ObjectId b = session.addEvent(track, second, 500, 1500);
	run(client, "crossfade.set", {{"from", a}, {"to", b}});
	// The crossfaded pair alone: the sum of the two where they play together.
	const auto pair = [](int64_t at)
	{
		const double gainA = at < 1000 ? fadeOut("linear", at, 1000, 500) : 0.0;
		const double gainB = at >= 500 && at < 2000 ? fadeIn("linear", at, 500, 500) : 0.0;
		return (0.125 * gainA) + (0.25 * gainB);
	};

	SECTION("without auto crossfades")
	{
		session.addEvent(track, take, 400, 700); // over the whole crossfade
		checkSamples(session.render(2100)[0], [&](int64_t at) { return at >= 400 && at < 1100 ? 0.5 : pair(at); });
	}
	SECTION("with auto crossfades, over the whole crossfade")
	{
		constexpr int64_t n = 48; // 1 ms at 48 kHz
		run(client, "autoFades.set", {{"track", track}, {"crossfades", true}, {"length", 1.0}});
		session.addEvent(track, take, 400, 700);
		// The take crossfades with the first event where it starts and with the second one where it ends; neither
		// plays under it in between.
		checkSamples(session.render(2100)[0],
			[&](int64_t at)
			{
				if (at < 400 || at >= 1100)
					return pair(at);
				const double gainTake = fadeIn("linear", at, 400, n) * fadeOut("linear", at, 1100, n);
				const double gainA = at < 400 + n ? fadeOut("linear", at, 400 + n, n) : 0.0;
				const double gainB = at >= 1100 - n ? fadeIn("linear", at, 1100 - n, n) : 0.0;
				return (0.5 * gainTake) + (0.125 * gainA) + (0.25 * gainB);
			});
	}
	SECTION("with auto crossfades, inside the crossfade")
	{
		constexpr int64_t n = 48;
		run(client, "autoFades.set", {{"track", track}, {"crossfades", true}, {"length", 1.0}});
		session.addEvent(track, take, 600, 300);
		// Both crossfaded events fade out under the take where it starts, stay silent, and fade in again where it
		// ends, so nothing jumps.
		const auto output = session.render(2100);
		checkSamples(output[0],
			[&](int64_t at)
			{
				if (at < 600 || at >= 900)
					return pair(at);
				const double gainTake = fadeIn("linear", at, 600, n) * fadeOut("linear", at, 900, n);
				double behind = 0.0;
				if (at < 600 + n)
					behind = fadeOut("linear", at, 600 + n, n);
				else if (at >= 900 - n)
					behind = fadeIn("linear", at, 900 - n, n);
				return (0.5 * gainTake) + (pair(at) * behind);
			});
		for (size_t i = 1; i < 2000; ++i)
			REQUIRE(std::abs(output[0][i] - output[0][i - 1]) < 0.02f);
	}
}

TEST_CASE("an event's own fade where it starts or ends on top of another is the auto crossfade's", "[integration]")
{
	// Auto fades don't stack with an event's own fades: where an event has a fade of its own, it replaces the auto
	// fade or the auto crossfade's fade at that end, and the event behind fades with its mirror image.
	const gnubase::test::TempDir dir;
	Session session(64);
	Client& client = session.client;
	const ObjectId track = session.addTrack();
	const ObjectId left =
		session.addClip(writeClip(dir, "left", 2, 9000, [](int64_t, int c) { return c == 0 ? 1.0f : 0.0f; }));
	const ObjectId right =
		session.addClip(writeClip(dir, "right", 2, 9000, [](int64_t, int c) { return c == 1 ? 1.0f : 0.0f; }));
	run(client, "autoFades.set", {{"crossfades", true}, {"crossfadeMode", "equalPower"}});
	session.addEvent(track, left, 0, 3000);
	const ObjectId starts = session.addEvent(track, right, 1000, 2500); // a long fade in on top of the first
	run(client, "audioEvent.setFades", {{"event", starts}, {"fadeIn", {{"length", 1000}, {"shape", "exponential"}}}});
	session.addEvent(track, right, 5000, 4000);
	const ObjectId ends = session.addEvent(track, left, 6000, 1000); // ends on top of the one before, fading out
	run(client, "audioEvent.setFades", {{"event", ends}, {"fadeOut", {{"length", 300}, {"shape", "sCurve"}}}});
	const auto output = session.render(9000);
	checkSamples(output[0],
		[&](int64_t at)
		{
			if (at < 1000)
				return 1.0;
			if (at < 3000)
				return fadeOut("logarithmic", at, 2000, 1000);
			if (at < 6000 || at >= 7000)
				return 0.0;
			return fadeIn("equalPower", at, 6000, 480) * fadeOut("sCurve", at, 7000, 300);
		});
	checkSamples(output[1],
		[&](int64_t at)
		{
			if (at < 1000)
				return 0.0;
			if (at < 3500)
				return fadeIn("exponential", at, 1000, 1000);
			if (at < 5000)
				return 0.0;
			if (at < 6000)
				return 1.0;
			if (at < 6700)
				return fadeOut("equalPower", at, 6000 + 480, 480);
			return at < 7000 ? fadeIn("sCurve", at, 6700, 300) : 1.0;
		});
}

TEST_CASE("renders are bit for bit the same for every block size where several events play at once", "[integration]")
{
	// The layers of a track add up in a fixed order, so the float sums don't depend on where blocks start.
	const auto render = [](int blockSize)
	{
		const gnubase::test::TempDir dir;
		Session session(blockSize);
		Client& client = session.client;
		const ObjectId track = session.addTrack();
		const ObjectId low =
			session.addClip(writeClip(dir, "low", 1, 30000, [](int64_t f, int) { return sine(331.0, f); }));
		const ObjectId high =
			session.addClip(writeClip(dir, "high", 1, 30000, [](int64_t f, int) { return sine(557.0, f) * 0.7f; }));
		// Crossfades into an event that crossfades on.
		const ObjectId a = session.addEvent(track, low, 0, 3000);
		const ObjectId b = session.addEvent(track, high, 1000, 2600, 100);
		const ObjectId c = session.addEvent(track, low, 2000, 3000, 1234);
		run(client, "crossfade.set", {{"from", a}, {"to", b}, {"mode", "equalPower"}});
		run(client, "crossfade.set", {{"from", b}, {"to", c}, {"mode", "custom"}, {"shape", "sCurve"}});
		// Events that each start on top of the one before within the auto crossfade length, so that up to four
		// play at once.
		run(client, "autoFades.set", {{"track", track}, {"crossfades", true}, {"length", 100.0}});
		for (int64_t i = 0; i < 4; ++i)
			session.addEvent(track, i % 2 == 0 ? low : high, 6000 + (1000 * i), 14000 - (1000 * i), 10 * i);
		return session.render(21000)[0];
	};
	const std::vector<float> reference = render(1);
	for (const int blockSize : {7, 64, 100, 333, 1024})
	{
		INFO("block size " << blockSize);
		const std::vector<float> output = render(blockSize);
		REQUIRE(output.size() == reference.size());
		size_t different = 0;
		for (size_t i = 0; i < output.size(); ++i)
			different += output[i] != reference[i] ? 1 : 0;
		CHECK(different == 0);
	}
}

TEST_CASE("clip fades apply to every event of the clip, at the clip's start and end", "[integration]")
{
	const gnubase::test::TempDir dir;
	Session session(64);
	const ObjectId track = session.addTrack();
	const ObjectId clip = session.addClip(writeClip(dir, "one", 1, 4000, one));
	run(session.client, "clip.setFades",
		{{"clip", clip}, {"fadeIn", {{"length", 1000}}}, {"fadeOut", {{"length", 800}, {"shape", "equalPower"}}}});
	session.addEvent(track, clip, 0, 2000);
	session.addEvent(track, clip, 5000, 3500, 500); // from clip frame 500 to the clip's end
	const auto output = session.render(9000);
	checkSamples(output[0],
		[&](int64_t at)
		{
			if (at < 2000)
				return fadeIn("linear", at, 0, 1000);
			if (at < 5000 || at >= 8500)
				return 0.0;
			return fadeIn("linear", at, 4500, 1000) * fadeOut("equalPower", at, 8500, 800);
		});
}

TEST_CASE("a render of every fade shape matches its golden file", "[integration]")
{
	const gnubase::test::TempDir dir;
	Session session(256);
	const ObjectId track = session.addTrack();
	const ObjectId clip =
		session.addClip(writeClip(dir, "sine", 1, 48000, [](int64_t f, int) { return sine(441.0, f); }));
	int64_t position = 0;
	for (const std::string shape : {"linear", "exponential", "logarithmic", "sCurve", "equalPower"})
	{
		const ObjectId event = session.addEvent(track, clip, position, 6000, position);
		run(session.client, "audioEvent.setFades",
			{{"event", event}, {"fadeIn", {{"length", 2500}, {"shape", shape}}},
				{"fadeOut", {{"length", 2500}, {"shape", shape}}}});
		position += 7000;
	}
	const auto output = session.render(position);
	const gnubase::test::AudioDiff diff = gnubase::test::compareWithGolden("fade-shapes", mono(output[0]), 1e-6f);
	INFO(diff.report);
	CHECK(diff.equal);
}

TEST_CASE("a render of equal power crossfades matches its golden file", "[integration]")
{
	const gnubase::test::TempDir dir;
	Session session(256);
	const ObjectId track = session.addTrack();
	const ObjectId low =
		session.addClip(writeClip(dir, "low", 1, 48000, [](int64_t f, int) { return sine(330.0, f); }));
	const ObjectId high =
		session.addClip(writeClip(dir, "high", 1, 48000, [](int64_t f, int) { return sine(550.0, f); }));
	const ObjectId a = session.addEvent(track, low, 0, 12000);
	const ObjectId b = session.addEvent(track, high, 8000, 12000);
	const ObjectId c = session.addEvent(track, low, 19000, 6000, 3000);
	run(session.client, "crossfade.set", {{"from", a}, {"to", b}, {"mode", "equalPower"}});
	run(session.client, "crossfade.set", {{"from", b}, {"to", c}, {"mode", "equalPower"}});
	const auto output = session.render(26000);
	const gnubase::test::AudioDiff diff =
		gnubase::test::compareWithGolden("crossfade-equal-power", mono(output[0]), 1e-6f);
	INFO(diff.report);
	CHECK(diff.equal);
}
