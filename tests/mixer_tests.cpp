// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/automation.h"
#include "engine/audio/sequencer.h"
#include "engine/document/document.h"
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "engine/graph/graph.h"
#include "engine/midi/midivalues.h"
#include "testutils/testutils.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using Catch::Approx;
using Catch::Matchers::ContainsSubstring;

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
		INFO(command << " succeeded: " << result.value.dump());
		REQUIRE(!result.ok());
		return result.error;
	}

	double gain(double decibels)
	{
		return std::pow(10.0, decibels / 20.0);
	}

	// An audio track that passes a sine from the dummy device's input to the stereo output, so the output shows
	// what its fader and panner do. A mono track gets the sine from a mono input bus.
	class Mixer
	{
	public:
		explicit Mixer(const char* format = "stereo")
		{
			run(client, "audio.setDevice",
				{{"blockSize", 64}, {"inputChannels", 3}, {"outputChannels", 8}, {"captureSeconds", 1},
					{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 441.0}, {"Level", 0.5}}}}}});
			channel = run(client, "track.add", {{"type", "audio"}, {"format", format}})
						  .at("channel")
						  .at("id")
						  .get<ObjectId>();
			if (std::string(format) == "mono")
			{
				const Json bus = run(client, "bus.add", {{"direction", "input"}, {"name", "Mono In"}, {"channels", 1}});
				set({{"input", bus.at("bus").at("id")}});
			}
		}

		Json set(Json parameters)
		{
			parameters["channel"] = channel;
			return run(client, "channel.set", parameters).at("channel");
		}

		// What a device output channel got from the start.
		std::vector<float> output(int index, int64_t frames)
		{
			return run(client, "audio.capture", {{"channel", index}, {"start", 0}, {"frames", frames}})
				.at("samples")
				.get<std::vector<float>>();
		}

		Engine engine;
		Client client{engine};
		ObjectId channel = 0;
	};

	// The sine the input carries, as a stereo track passes it on.
	const std::vector<float>& sine()
	{
		static const std::vector<float> samples = []
		{
			Mixer mixer;
			run(mixer.client, "audio.render", {{"frames", 2000}});
			return mixer.output(0, 2000);
		}();
		return samples;
	}

	// Whether every frame of an output is the sine times the gain at that frame.
	void checkGain(const std::vector<float>& output, const std::function<double(size_t)>& gainAt, size_t from = 0)
	{
		size_t wrong = 0;
		size_t first = 0;
		for (size_t i = from; i < output.size(); ++i)
			if (std::abs(output[i] - (sine()[i] * gainAt(i))) > 1e-6 && wrong++ == 0)
				first = i;
		INFO(wrong << " wrong frames, the first at " << first << ": " << output[first] << " instead of "
				   << sine()[first] * gainAt(first));
		CHECK(wrong == 0);
	}

	void checkGain(const std::vector<float>& output, double constant)
	{
		checkGain(output, [constant](size_t) { return constant; });
	}
}

TEST_CASE("pan laws attenuate the center and keep the sides at 0 dB", "[unit]")
{
	const std::pair<PanLaw, double> laws[] = {{PanLaw::Off, 1.0}, {PanLaw::Minus3, gain(-3.0)},
		{PanLaw::Minus4Point5, gain(-4.5)}, {PanLaw::Minus6, gain(-6.0)}, {PanLaw::EqualPower, std::sqrt(0.5)}};
	for (const auto& [law, center] : laws)
	{
		INFO(panLawName(law));
		CHECK(panLawFromName(panLawName(law)) == law);
		CHECK(panGains(law, 0.0).first == Approx(center));
		CHECK(panGains(law, 0.0).second == Approx(center));
		CHECK(panGains(law, -1.0) == std::pair<double, double>{1.0, 0.0});
		CHECK(panGains(law, 1.0).first == Approx(0.0).margin(1e-12));
		CHECK(panGains(law, 1.0).second == 1.0);
		// Panning to the right, the left side only gets quieter and the right side only louder.
		for (int step = 0; step < 16; ++step)
		{
			const double pan = -1.0 + (step / 8.0);
			CHECK(panGains(law, pan + 0.125).first <= panGains(law, pan).first);
			CHECK(panGains(law, pan + 0.125).second >= panGains(law, pan).second);
		}
	}
	for (int step = 0; step <= 8; ++step)
	{
		const auto [left, right] = panGains(PanLaw::EqualPower, -1.0 + (step / 4.0));
		CHECK((left * left) + (right * right) == Approx(1.0));
	}
	CHECK(panLawFromName("-2dB") == std::nullopt);
}

TEST_CASE("a channel's volume scales its output; the project's maximum volume clamps it", "[integration]")
{
	Mixer mixer;
	CHECK(mixer.set({{"volume", -6.0}}).at("volume") == -6.0);
	run(mixer.client, "audio.render", {{"frames", 2000}});
	checkGain(mixer.output(0, 2000), gain(-6.0));
	checkGain(mixer.output(1, 2000), gain(-6.0));

	CHECK(mixer.set({{"volume", 10.0}}).at("volume") == 6.0);
	run(mixer.client, "project.set", {{"maxVolume", 12.0}});
	CHECK(run(mixer.client, "project.get").at("maxVolume") == 12.0);
	CHECK(mixer.set({{"volume", 10.0}}).at("volume") == 10.0);
	// A lower maximum turns faders above it down, in the same undo step.
	run(mixer.client, "project.set", {{"maxVolume", 6.0}});
	CHECK(run(mixer.client, "project.get").at("channels")[0].at("volume") == 6.0);
	run(mixer.client, "history.undo");
	const Json project = run(mixer.client, "project.get");
	CHECK(project.at("maxVolume") == 12.0);
	CHECK(project.at("channels")[0].at("volume") == 10.0);
	CHECK_THAT(reject(mixer.client, "project.set", {{"maxVolume", 9.0}}), ContainsSubstring("maxVolume"));

	// The bottom of the fader is silence.
	Mixer silent;
	silent.set({{"volume", -200.0}});
	CHECK(run(silent.client, "project.get").at("channels")[0].at("volume") == -144.0);
	run(silent.client, "audio.render", {{"frames", 500}});
	checkGain(silent.output(0, 500), 0.0);
}

TEST_CASE("a mono channel pans onto a stereo bus with the project's pan law", "[integration]")
{
	const std::pair<const char*, double> laws[] = {{"0dB", 1.0}, {"-3dB", gain(-3.0)}, {"-4.5dB", gain(-4.5)},
		{"-6dB", gain(-6.0)}, {"equalPower", std::sqrt(0.5)}};
	for (const auto& [law, center] : laws)
	{
		INFO(law);
		Mixer mixer("mono");
		run(mixer.client, "project.set", {{"panLaw", law}});
		CHECK(run(mixer.client, "project.get").at("panLaw") == law);
		run(mixer.client, "audio.render", {{"frames", 1000}});
		checkGain(mixer.output(0, 1000), center);
		checkGain(mixer.output(1, 1000), center);
	}

	// Hard left: the right side is silent and the left at 0 dB.
	Mixer left("mono");
	left.set({{"pan", -1.0}});
	run(left.client, "audio.render", {{"frames", 1000}});
	checkGain(left.output(0, 1000), 1.0);
	checkGain(left.output(1, 1000), 0.0);

	// Halfway right with equal power: cos and sin of 3/8 pi.
	Mixer right("mono");
	run(right.client, "project.set", {{"panLaw", "equalPower"}});
	right.set({{"pan", 0.5}});
	run(right.client, "audio.render", {{"frames", 1000}});
	checkGain(right.output(0, 1000), std::cos(0.375 * g_pi));
	checkGain(right.output(1, 1000), std::sin(0.375 * g_pi));
}

TEST_CASE("stereo channels pan with the balance or the combined panner; pan bypass centers them", "[integration]")
{
	// Balance hard left: the right side is silent, the left unchanged.
	Mixer balance;
	CHECK(balance.set({{"pan", -1.0}}).at("panner") == "balance");
	run(balance.client, "audio.render", {{"frames", 1000}});
	checkGain(balance.output(0, 1000), 1.0);
	checkGain(balance.output(1, 1000), 0.0);

	// Combined, both positions at the center: left and right sum to mono at the pan law's center, on both sides.
	Mixer combined;
	const Json settings = combined.set({{"panner", "combined"}, {"panLeft", 0.0}, {"panRight", 0.0}});
	CHECK(settings.at("panLeft") == 0.0);
	run(combined.client, "audio.render", {{"frames", 1000}});
	checkGain(combined.output(0, 1000), 2.0 * gain(-3.0));
	checkGain(combined.output(1, 1000), 2.0 * gain(-3.0));

	// Combined at its defaults, hard left and hard right: unchanged.
	Mixer wide;
	CHECK(wide.set({{"panner", "combined"}}).at("panRight") == 1.0);
	run(wide.client, "audio.render", {{"frames", 1000}});
	checkGain(wide.output(0, 1000), 1.0);
	checkGain(wide.output(1, 1000), 1.0);

	// Bypassed, a stereo channel plays hard left and right, a mono one at the center.
	Mixer stereo;
	stereo.set({{"pan", -1.0}, {"panBypass", true}});
	run(stereo.client, "audio.render", {{"frames", 1000}});
	checkGain(stereo.output(1, 1000), 1.0);
	Mixer mono("mono");
	mono.set({{"pan", -1.0}, {"panBypass", true}});
	run(mono.client, "audio.render", {{"frames", 1000}});
	checkGain(mono.output(0, 1000), gain(-3.0));
	checkGain(mono.output(1, 1000), gain(-3.0));

	CHECK_THAT(reject(mono.client, "channel.set", {{"channel", mono.channel}, {"panner", "combined"}}),
		ContainsSubstring("stereo"));
	CHECK_THAT(reject(mono.client, "channel.set", {{"channel", mono.channel}, {"panLeft", 2.0}}),
		ContainsSubstring("panLeft"));
}

TEST_CASE("channels on destinations wider than stereo aren't panned", "[integration]")
{
	// A mono channel on a 5.1 bus (device outputs 2 to 7) reaches only its center, whatever its pan.
	Mixer mixer("mono");
	const Json bus = run(mixer.client, "bus.add", {{"direction", "output"}, {"name", "5.1 Out"}, {"format", "5.1"}});
	mixer.set({{"output", bus.at("bus").at("id")}, {"pan", -1.0}});
	run(mixer.client, "audio.render", {{"frames", 1000}});
	for (int channel = 0; channel < 8; ++channel)
	{
		INFO("output " << channel);
		checkGain(mixer.output(channel, 1000), channel == 4 ? 1.0 : 0.0);
	}
}

TEST_CASE("volume and pan changes during playback ramp instead of jumping", "[integration]")
{
	Mixer mixer;
	run(mixer.client, "transport.play");
	run(mixer.client, "audio.render", {{"frames", 1024}});
	mixer.set({{"volume", -20.0}});
	run(mixer.client, "audio.render", {{"frames", 976}});
	// From the first frame after the change, the gain moves in a straight line for 5 ms (240 frames at 48 kHz).
	const double target = gain(-20.0);
	checkGain(mixer.output(0, 2000),
		[target](size_t i)
		{
			if (i < 1024)
				return 1.0;
			const double x = std::min(1.0, static_cast<double>(i - 1024) / 240.0);
			return 1.0 + ((target - 1.0) * x);
		});
}

TEST_CASE("pan law, maximum volume, panner settings and MIDI volume and pan are saved and undone", "[integration]")
{
	Mixer mixer;
	const Json midi = run(mixer.client, "track.add", {{"type", "midi"}}).at("channel");
	run(mixer.client, "channel.set", {{"channel", midi.at("id")}, {"midiVolume", 70}, {"midiPan", 100}});
	run(mixer.client, "project.set", {{"panLaw", "-6dB"}, {"maxVolume", 12.0}});
	mixer.set({{"panner", "combined"}, {"panLeft", -0.5}, {"panRight", 0.25}, {"panBypass", true}, {"volume", 9.0}});
	const Json before = run(mixer.client, "project.get");
	CHECK(before.at("panLaw") == "-6dB");

	run(mixer.client, "history.undo");
	const Json undone = run(mixer.client, "project.get").at("channels")[0];
	CHECK(undone.at("panner") == "balance");
	CHECK(undone.at("panBypass") == false);
	run(mixer.client, "history.redo");
	CHECK(run(mixer.client, "project.get") == before);

	const gnubase::test::TempDir dir;
	const std::string file = (dir.path() / "mix.dawproject").u8string();
	run(mixer.client, "project.save", {{"path", file}});
	run(mixer.client, "project.new");
	CHECK(run(mixer.client, "project.get").at("panLaw") == "-3dB");
	run(mixer.client, "project.load", {{"path", file}});
	CHECK(run(mixer.client, "project.get") == before);
}

TEST_CASE("a MIDI channel's volume and pan reach its instrument as CC 7 and CC 10", "[unit]")
{
	// Through channel.set into the render snapshot.
	Engine engine;
	Client client(engine);
	const Json midi = run(client, "track.add", {{"type", "midi"}}).at("channel");
	CHECK(midi.at("midiVolume").is_null());
	const auto id = midi.at("id").get<ObjectId>();
	const Json set = run(client, "channel.set", {{"channel", id}, {"midiVolume", 90}, {"midiPan", 32}}).at("channel");
	CHECK(set.at("midiVolume") == 90);
	CHECK(set.at("midiPan") == 32);
	const ObjectId audio = run(client, "track.add", {{"type", "audio"}}).at("channel").at("id").get<ObjectId>();
	CHECK_THAT(reject(client, "channel.set", {{"channel", audio}, {"midiVolume", 90}}),
		ContainsSubstring("only MIDI channels"));
	CHECK_THAT(reject(client, "channel.set", {{"channel", id}, {"midiPan", 128}}), ContainsSubstring("midiPan"));
	CHECK_THAT(reject(client, "channel.set", {{"channel", id}, {"volume", -3.0}}), ContainsSubstring("no volume"));
	RenderChannel render;
	for (const RenderChannel& channel : engine.renderSnapshots().acquire().channels)
		if (channel.id == id)
			render = channel;
	CHECK(render.midiVolume == 90);
	CHECK(render.midiPan == 32);

	// The channel's strip sends them when they are new or change, and again when playback starts.
	auto state = std::make_shared<ChannelState>();
	state->automation.setAutomation(channelAutomation(render, 6.0));
	ChannelStrip strip(state, std::make_shared<MidiSequencer>());
	strip.prepareFor(48000.0, 64);
	const graph::EventBuffer in(graph::g_maxEventsPerBlock);
	graph::EventBuffer out(graph::g_maxEventsPerBlock);
	const graph::TransportSegment segment{0, 64, 0};
	const auto controllers = [&](const graph::TransportBlock& transport)
	{
		out.clear();
		strip.process({nullptr, 0, 64, in, out, transport});
		std::vector<std::pair<int, int>> sent;
		for (const graph::ProcessEvent& event : out)
			if (event.type == graph::ProcessEvent::Type::Controller && event.channel == 0)
				sent.emplace_back(event.key, midi::controllerToMidi1(event.value));
		return sent;
	};
	using Sent = std::vector<std::pair<int, int>>;
	const graph::TransportBlock stopped;
	CHECK(controllers(stopped) == Sent{{7, 90}, {10, 32}});
	CHECK(controllers(stopped).empty());
	render.midiVolume = 100;
	state->automation.setAutomation(channelAutomation(render, 6.0));
	CHECK(controllers(stopped) == Sent{{7, 100}, {10, 32}});
	CHECK(controllers({true, 0, &segment, 1}) == Sent{{7, 100}, {10, 32}});
	render.midiVolume.reset();
	render.midiPan.reset();
	state->automation.setAutomation(channelAutomation(render, 6.0));
	CHECK(controllers(stopped).empty());

	// Undo takes them back.
	run(client, "history.undo"); // the audio track
	run(client, "history.undo");
	CHECK(run(client, "project.get").at("channels")[0].at("midiVolume").is_null());
}
