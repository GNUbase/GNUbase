// SPDX-License-Identifier: GPL-3.0-or-later
// Event volume curves (#139): the commands, and playback that applies them sample by sample.
#include "engine/audio/fades.h"
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;

namespace
{
	constexpr double g_pi = 3.14159265358979323846;

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

	double gain(double db)
	{
		return std::pow(10.0, db / 20.0);
	}

	// The curve as the documentation gives it, written out again: from point to point the gain goes from one's to
	// the next's, bent by the first one's curve c as (1 - e^(-6cx)) / (1 - e^(-6c)); level before the first point and
	// after the last.
	struct Point
	{
		int64_t position = 0;
		double volume = 0.0;
		double curve = 0.0;
	};

	double curveGain(const std::vector<Point>& points, int64_t at)
	{
		if (points.empty())
			return 1.0;
		if (at < points.front().position)
			return gain(points.front().volume);
		for (size_t i = 0; i + 1 < points.size(); ++i)
		{
			const Point& a = points[i];
			const Point& b = points[i + 1];
			if (at >= b.position)
				continue;
			const double x = static_cast<double>(at - a.position) / static_cast<double>(b.position - a.position);
			const double k = 6.0 * a.curve;
			const double bend = std::abs(k) < 1e-9 ? x : (1.0 - std::exp(-k * x)) / (1.0 - std::exp(-k));
			return gain(a.volume) + ((gain(b.volume) - gain(a.volume)) * bend);
		}
		return gain(points.back().volume);
	}

	Json pointsJson(const std::vector<Point>& points)
	{
		Json json = Json::array();
		for (const Point& point : points)
			json.push_back({{"position", point.position}, {"volume", point.volume}, {"curve", point.curve}});
		return json;
	}

	std::string writeOnes(const gnubase::test::TempDir& dir, int64_t frames)
	{
		gnubase::test::Audio audio;
		audio.channels = 1;
		audio.samples.assign(static_cast<size_t>(frames), 1.0f);
		const std::filesystem::path file = dir.path() / "one.wav";
		gnubase::test::writeWav(file, audio);
		return file.u8string();
	}

	std::string writeSine(const gnubase::test::TempDir& dir, int64_t frames)
	{
		gnubase::test::Audio audio;
		audio.channels = 1;
		for (int64_t f = 0; f < frames; ++f)
			audio.samples.push_back(
				static_cast<float>(0.5 * std::sin(2.0 * g_pi * 441.0 * static_cast<double>(f) / 48000.0)));
		const std::filesystem::path file = dir.path() / "sine.wav";
		gnubase::test::writeWav(file, audio);
		return file.u8string();
	}

	// An engine with a dummy device and a linear audio track, at 48 kHz.
	class Session
	{
	public:
		explicit Session(int blockSize = 64)
		{
			run(client, "audio.setDevice", {{"blockSize", blockSize}, {"inputChannels", 0}, {"captureSeconds", 5}});
			track =
				idOf(run(client, "track.add", {{"type", "audio"}, {"timeBase", "linear"}, {"channels", 2}}), "track");
		}

		ObjectId addClip(const std::string& path)
		{
			return idOf(run(client, "clip.add", {{"media", idOf(run(client, "media.add", {{"path", path}}), "media")}}),
				"clip");
		}

		ObjectId addEvent(ObjectId clip, int64_t position, int64_t length, int64_t offset = 0)
		{
			return idOf(
				run(client, "audioEvent.add",
					{{"track", track}, {"clip", clip}, {"position", position}, {"offset", offset}, {"length", length}}),
				"event");
		}

		std::vector<float> render(int64_t frames)
		{
			run(client, "transport.locate", {{"position", 0}});
			run(client, "transport.play");
			run(client, "audio.render", {{"frames", frames}});
			run(client, "transport.stop");
			std::vector<float> output =
				run(client, "audio.capture", {{"channel", 0}, {"start", m_rendered}, {"frames", frames}})
					.at("samples")
					.get<std::vector<float>>();
			m_rendered += frames;
			CHECK(run(client, "audio.get").at("playbackDropouts") == 0);
			return output;
		}

		Engine engine;
		Client client{engine};
		ObjectId track = 0;

	private:
		int64_t m_rendered = 0;
	};

	template <typename Expected> void checkSamples(const std::vector<float>& output, Expected expected)
	{
		size_t wrong = 0;
		size_t at = 0;
		for (size_t i = 0; i < output.size(); ++i)
			if (std::abs(output[i] - expected(static_cast<int64_t>(i))) > 1e-5 && wrong++ == 0)
				at = i;
		INFO(wrong << " wrong samples, the first at " << at << ": " << output[at] << " instead of "
				   << expected(static_cast<int64_t>(at)));
		CHECK(wrong == 0);
	}
}

TEST_CASE("audio events have volume curves, which the commands check and undo restores", "[unit]")
{
	const gnubase::test::TempDir dir;
	Session session;
	Client& client = session.client;
	const ObjectId clip = session.addClip(writeOnes(dir, 10000));
	const ObjectId event = session.addEvent(clip, 0, 4000, 1000);
	CHECK(run(client, "project.get").at("audioEvents")[0].at("volumeCurve") == Json::array());

	// Points come back by position, with a curve of 0 unless given.
	const Json changed = run(client, "audioEvent.setVolumeCurve",
		{{"event", event},
			{"points",
				{{{"position", 3000}, {"volume", -12.0}}, {{"position", 1500}, {"volume", 3.0}, {"curve", 0.5}}}}})
							 .at("event");
	CHECK(changed.at("volumeCurve") ==
		Json::parse(R"([{"position":1500,"volume":3.0,"curve":0.5},{"position":3000,"volume":-12.0,"curve":0.0}])"));

	CHECK_THAT(reject(client, "audioEvent.setVolumeCurve",
				   {{"event", event}, {"points", {{{"position", 10001}, {"volume", 0.0}}}}}),
		ContainsSubstring("within the clip"));
	CHECK_THAT(reject(client, "audioEvent.setVolumeCurve",
				   {{"event", event}, {"points", {{{"position", 0}, {"volume", 7.0}}}}}),
		ContainsSubstring("-144 to 6 dB"));
	CHECK_THAT(reject(client, "audioEvent.setVolumeCurve",
				   {{"event", event}, {"points", {{{"position", 0}, {"volume", 0.0}, {"curve", 2.0}}}}}),
		ContainsSubstring("-1 to 1"));
	CHECK_THAT(reject(client, "audioEvent.setVolumeCurve", {{"event", 999}, {"points", Json::array()}}),
		ContainsSubstring("no audio event 999"));

	// Remove Volume Curve.
	CHECK(run(client, "event.removeVolumeCurve", {{"events", {event}}}).at("events") == Json{event});
	CHECK(run(client, "project.get").at("audioEvents")[0].at("volumeCurve") == Json::array());

	// Each command is an undo step.
	run(client, "history.undo");
	CHECK(run(client, "project.get").at("audioEvents")[0].at("volumeCurve") == changed.at("volumeCurve"));
	run(client, "history.undo");
	CHECK(run(client, "project.get").at("audioEvents")[0].at("volumeCurve") == Json::array());
}

TEST_CASE("range.volumeCurve sets the volume inside a range and keeps the curve outside it", "[unit]")
{
	const gnubase::test::TempDir dir;
	Session session;
	Client& client = session.client;
	const ObjectId clip = session.addClip(writeOnes(dir, 10000));
	// The event plays clip frames 1000 to 9000 from project sample 500.
	const ObjectId event = session.addEvent(clip, 500, 8000, 1000);
	const std::vector<Point> before = {{2000, 0.0, 0.6}, {6000, -20.0, 0.0}, {8000, 0.0, 0.0}};
	run(client, "audioEvent.setVolumeCurve", {{"event", event}, {"points", pointsJson(before)}});

	// From project sample 3500 to 6500: clip frames 4000 to 7000.
	const Json result = run(
		client, "range.volumeCurve", {{"start", 3500}, {"end", 6500}, {"tracks", {session.track}}, {"volume", -6.0}});
	CHECK(result.at("events") == Json{event});
	const Json curve = run(client, "project.get").at("audioEvents")[0].at("volumeCurve");
	std::vector<Point> after;
	for (const Json& point : curve)
		after.push_back(
			{point.at("position").get<int64_t>(), point.at("volume").get<double>(), point.at("curve").get<double>()});
	for (int frame = 0; frame <= 10000; frame += 50)
	{
		INFO("clip frame " << frame);
		const double expected = frame >= 4000 && frame < 7000 ? gain(-6.0) : curveGain(before, frame);
		CHECK_THAT(curveGain(after, frame), WithinAbs(expected, 1e-9));
	}

	// Remove Volume Curve on a range: the original level there.
	run(client, "range.volumeCurve", {{"start", 3500}, {"end", 6500}});
	const Json removed = run(client, "project.get").at("audioEvents")[0].at("volumeCurve");
	std::vector<Point> reset;
	for (const Json& point : removed)
		reset.push_back(
			{point.at("position").get<int64_t>(), point.at("volume").get<double>(), point.at("curve").get<double>()});
	CHECK_THAT(curveGain(reset, 5000), WithinAbs(1.0, 1e-9));
	CHECK_THAT(curveGain(reset, 3000), WithinAbs(curveGain(before, 3000), 1e-9));

	CHECK_THAT(
		reject(client, "range.volumeCurve", {{"start", 9500}, {"end", 9600}}), ContainsSubstring("no audio event"));
	CHECK_THAT(reject(client, "range.volumeCurve", {{"start", 600}, {"end", 600}}), ContainsSubstring("empty"));
}

TEST_CASE("a volume curve shapes an event's gain sample by sample, for any block size", "[integration]")
{
	const int blockSize = GENERATE(1, 64, 333);
	INFO("block size " << blockSize);
	const gnubase::test::TempDir dir;
	Session session(blockSize);
	const ObjectId clip = session.addClip(writeOnes(dir, 10000));
	const ObjectId event = session.addEvent(clip, 500, 4000, 2000);
	// Clip frames: the event plays 2000 to 6000. Points before and after it hold their level.
	const std::vector<Point> points = {
		{1000, -6.0, 0.0}, {2600, 0.0, 0.8}, {3600, -24.0, -0.5}, {3600, 3.0, 0.0}, {5000, -3.0, 0.0}};
	run(session.client, "audioEvent.setVolumeCurve", {{"event", event}, {"points", pointsJson(points)}});
	run(session.client, "audioEvent.set", {{"event", event}, {"volume", -6.0}});
	run(session.client, "audioEvent.setFades", {{"event", event}, {"fadeOut", {{"length", 400}}}});

	const auto output = session.render(5000);
	checkSamples(output,
		[&](int64_t at)
		{
			if (at < 500 || at >= 4500)
				return 0.0;
			const double fade = at >= 4100 ? static_cast<double>(4500 - at) / 400.0 : 1.0;
			return gain(-6.0) * curveGain(points, at - 500 + 2000) * fade;
		});

	// The curve is the event's: it moves with it, and a split leaves each half the part it plays.
	run(session.client, "audioEvent.setFades", {{"event", event}, {"fadeOut", {{"length", 0}}}});
	run(session.client, "audioEvent.set", {{"event", event}, {"position", 0}});
	run(session.client, "event.split", {{"events", {event}}, {"positions", {2000}}});
	const auto moved = session.render(5000);
	checkSamples(moved, [&](int64_t at) { return at >= 4000 ? 0.0 : gain(-6.0) * curveGain(points, at + 2000); });
}

TEST_CASE("glue keeps how split events played, whatever their volumes differ by", "[integration]")
{
	const gnubase::test::TempDir dir;
	Session session;
	const ObjectId clip = session.addClip(writeOnes(dir, 10000));
	const ObjectId event = session.addEvent(clip, 0, 8000);
	const Json pieces = run(session.client, "event.split", {{"events", {event}}, {"positions", {4000}}}).at("events");
	REQUIRE(pieces.size() == 1);
	// The first half is 12 dB below the second, more than a curve point's +6 dB.
	const std::vector<Point> points = {{6000, 0.0, 0.0}, {7000, 6.0, 0.0}};
	run(session.client, "audioEvent.set", {{"event", event}, {"volume", -12.0}});
	run(session.client, "audioEvent.setVolumeCurve", {{"event", pieces[0]}, {"points", pointsJson(points)}});
	const auto expected = [&points](int64_t at)
	{
		if (at >= 8000)
			return 0.0;
		return at < 4000 ? gain(-12.0) : curveGain(points, at);
	};
	checkSamples(session.render(9000), expected);

	run(session.client, "event.glue", {{"events", {event, pieces[0]}}});
	const Json glued = run(session.client, "project.get").at("audioEvents");
	REQUIRE(glued.size() == 1);
	CHECK(glued[0].at("volume") == 0.0);
	checkSamples(session.render(9000), expected);
}

TEST_CASE("a render of volume curves matches its golden file", "[integration]")
{
	const gnubase::test::TempDir dir;
	Session session(256);
	const ObjectId clip = session.addClip(writeSine(dir, 48000));
	const ObjectId a = session.addEvent(clip, 0, 12000);
	run(session.client, "audioEvent.setVolumeCurve",
		{{"event", a},
			{"points", pointsJson({{0, -30.0, 0.7}, {4000, 0.0, 0.0}, {8000, -12.0, -0.7}, {12000, 6.0, 0.0}})}});
	const ObjectId b = session.addEvent(clip, 14000, 12000, 14000);
	run(session.client, "audioEvent.set", {{"event", b}, {"volume", -3.0}});
	run(session.client, "audioEvent.setVolumeCurve",
		{{"event", b}, {"points", pointsJson({{16000, 0.0, 0.0}, {20000, -144.0, 0.0}, {20000, 0.0, 0.0}})}});
	gnubase::test::Audio audio;
	audio.channels = 1;
	audio.samples = session.render(28000);
	const gnubase::test::AudioDiff diff = gnubase::test::compareWithGolden("volume-curves", audio, 1e-6f);
	INFO(diff.report);
	CHECK(diff.equal);
}
