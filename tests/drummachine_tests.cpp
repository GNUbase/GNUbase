// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/engine.h"
#include "engine/graph/graph.h"
#include "engine/processors/builtin.h"
#include "engine/processors/drummachine.h"
#include "engine/time/tempomap.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using namespace gnubase::engine::graph;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::WithinAbs;
using processors::DrumMachine;
using processors::Sample;
using Layer = DrumMachine::Layer;

namespace
{
	constexpr double g_sampleRate = 48000.0;
	constexpr double g_pi = 3.14159265358979323846;

	uint32_t padParameter(int pad, DrumMachine::PadParameter parameter)
	{
		return DrumMachine::padParameter(pad, parameter);
	}

	// A mono sample whose frame i is (i + 1) / 1024: every frame different, so where a voice reads shows.
	std::shared_ptr<const Sample> ramp(int64_t frames)
	{
		auto sample = std::make_shared<Sample>();
		sample->sampleRate = g_sampleRate;
		for (int64_t i = 0; i < frames; ++i)
			sample->left.push_back(static_cast<float>(i + 1) / 1024.0f);
		return sample;
	}

	ProcessEvent parameter(uint32_t offset, uint32_t id, double value)
	{
		ProcessEvent event;
		event.offset = offset;
		event.type = ProcessEvent::Type::Parameter;
		event.parameter = id;
		event.parameterValue = value;
		return event;
	}

	ProcessEvent note(uint32_t offset, bool on, uint8_t pitch, uint16_t velocity = 0xffff)
	{
		ProcessEvent event;
		event.offset = offset;
		event.type = on ? ProcessEvent::Type::NoteOn : ProcessEvent::Type::NoteOff;
		event.key = pitch;
		event.velocity = velocity;
		return event;
	}

	// Runs a drum machine on its own: stereo blocks with the given events, as automation sends them: every pad
	// parameter counts as automated, until setPadValues says otherwise.
	class Bench
	{
	public:
		explicit Bench(DrumMachine& drums, uint32_t frames = 64) : m_drums(drums), m_frames(frames)
		{
			std::set<uint32_t> automated;
			for (uint32_t id = 0; id < DrumMachine::MasterLevel; ++id)
				automated.insert(id);
			drums.setPadValues({}, automated);
			drums.prepareFor(g_sampleRate, frames);
			m_samples.resize(2 * static_cast<size_t>(frames));
			m_pointers = {m_samples.data(), m_samples.data() + frames};
		}

		void add(const ProcessEvent& event) { REQUIRE(m_events.add(event)); }
		void set(int pad, DrumMachine::PadParameter id, double value)
		{
			add(parameter(0, padParameter(pad, id), value));
		}

		// Processes blocks and returns the left and right channels.
		std::pair<std::vector<float>, std::vector<float>> run(int blocks)
		{
			std::vector<float> left;
			std::vector<float> right;
			for (int b = 0; b < blocks; ++b)
			{
				m_out.clear();
				m_drums.process({m_pointers.data(), 2, m_frames, m_events, m_out, TransportBlock{}});
				m_events.clear();
				left.insert(left.end(), m_samples.begin(), m_samples.begin() + m_frames);
				right.insert(right.end(), m_samples.begin() + m_frames, m_samples.end());
			}
			return {left, right};
		}

		std::vector<float> left(int blocks) { return run(blocks).first; }

	private:
		DrumMachine& m_drums;
		uint32_t m_frames;
		std::vector<float> m_samples;
		std::vector<float*> m_pointers;
		EventBuffer m_events{g_maxEventsPerBlock};
		EventBuffer m_out{g_maxEventsPerBlock};
	};

	double peak(const std::vector<float>& values, size_t from = 0, size_t to = SIZE_MAX)
	{
		double highest = 0.0;
		for (size_t i = from; i < std::min(to, values.size()); ++i)
			highest = std::max(highest, static_cast<double>(std::abs(values[i])));
		return highest;
	}
}

TEST_CASE("a drum pad plays its sample at its note, from start to end, tuned and reversed", "[unit]")
{
	DrumMachine drums;
	drums.setSample(36, ramp(100));
	Bench bench(drums);
	bench.set(36, DrumMachine::Instrument, static_cast<double>(Layer::Sample));
	bench.add(note(0, true, 37)); // another pad: nothing
	bench.add(note(10, true, 36));
	const std::vector<float> plain = bench.left(4);
	for (size_t i = 0; i < 10; ++i)
		CHECK(plain[i] == 0.0f);
	for (size_t i = 0; i < 100; ++i)
		CHECK_THAT(plain[10 + i], WithinAbs((i + 1) / 1024.0, 1e-7));
	CHECK(plain[110] == 0.0f);
	CHECK(drums.activeVoices() == 0);

	// A quarter to three quarters, an octave up: every second frame.
	bench.set(36, DrumMachine::Start, 0.25);
	bench.set(36, DrumMachine::End, 0.75);
	bench.set(36, DrumMachine::Tune, 12.0);
	bench.add(note(0, true, 36));
	const std::vector<float> tuned = bench.left(1);
	for (size_t i = 0; i < 25; ++i)
		CHECK_THAT(tuned[i], WithinAbs((25 + (2 * i) + 1) / 1024.0, 1e-7));
	CHECK(tuned[25] == 0.0f);

	// Reversed, from the end.
	bench.set(36, DrumMachine::Start, 0.0);
	bench.set(36, DrumMachine::End, 1.0);
	bench.set(36, DrumMachine::Tune, 0.0);
	bench.set(36, DrumMachine::Reverse, 1.0);
	bench.add(note(0, true, 36));
	const std::vector<float> reversed = bench.left(2);
	for (size_t i = 0; i < 100; ++i)
		CHECK_THAT(reversed[i], WithinAbs((100 - i) / 1024.0, 1e-7));

	// A pad without a sample plays nothing.
	bench.set(40, DrumMachine::Instrument, static_cast<double>(Layer::Sample));
	bench.add(note(0, true, 40));
	CHECK(peak(bench.left(2)) == 0.0);
}

TEST_CASE("a drum pad's level, pan, velocity, mute and solo", "[unit]")
{
	DrumMachine drums;
	drums.setSample(36, ramp(64));
	drums.setSample(38, ramp(64));
	Bench bench(drums);
	for (const int pad : {36, 38})
		bench.set(pad, DrumMachine::Instrument, static_cast<double>(Layer::Sample));
	bench.set(36, DrumMachine::Level, -6.0);
	bench.set(36, DrumMachine::Pan, 0.5);
	bench.add(note(0, true, 36, 0x8000));
	const auto [left, right] = bench.run(1);
	const double gain = std::pow(10.0, -6.0 / 20.0) * 0x8000 / 65535.0;
	CHECK_THAT(left[9], WithinAbs(10.0 / 1024.0 * gain * 0.5, 1e-6));
	CHECK_THAT(right[9], WithinAbs(10.0 / 1024.0 * gain, 1e-6));

	// No velocity to level: full level for a soft note.
	bench.set(36, DrumMachine::VelocityToLevel, 0.0);
	bench.set(36, DrumMachine::Pan, 0.0);
	bench.set(36, DrumMachine::Level, 0.0);
	bench.add(note(0, true, 36, 1));
	CHECK_THAT(bench.left(1)[9], WithinAbs(10.0 / 1024.0, 1e-6));

	// Muted, the pad sounds no more; soloed, only it.
	bench.set(36, DrumMachine::Mute, 1.0);
	bench.add(note(0, true, 36));
	bench.add(note(0, true, 38));
	CHECK_THAT(bench.left(1)[9], WithinAbs(10.0 / 1024.0, 1e-6)); // 38's alone
	bench.set(36, DrumMachine::Mute, 0.0);
	bench.set(38, DrumMachine::Solo, 1.0);
	bench.add(note(0, true, 36));
	bench.add(note(0, true, 38, 0x8000));
	CHECK_THAT(bench.left(1)[9], WithinAbs(10.0 / 1024.0 * 0x8000 / 65535.0, 1e-6)); // 38's alone
}

TEST_CASE("a pad parameter whose lane went ignores that lane's last events", "[unit]")
{
	// The engine hands the drum machine the pads' values without the lane before it stops the lane's automation, so
	// the lane's last events can come after them (#1361).
	DrumMachine drums;
	drums.setSample(36, ramp(64));
	Bench bench(drums);
	const std::map<uint32_t, double> values = {
		{padParameter(36, DrumMachine::Instrument), static_cast<double>(Layer::Sample)}};
	drums.setPadValues(values, {padParameter(36, DrumMachine::Level)});
	bench.set(36, DrumMachine::Level, -96.0);
	bench.add(note(0, true, 36));
	CHECK(peak(bench.left(1)) < 1e-4);

	drums.setPadValues(values, {});
	bench.set(36, DrumMachine::Level, -96.0);
	bench.add(note(0, true, 36));
	CHECK_THAT(bench.left(1)[9], WithinAbs(10.0 / 1024.0, 1e-6));
}

TEST_CASE("pads of an exclusive group cut each other off; the gate ends a pad with its note", "[unit]")
{
	DrumMachine drums;
	Bench bench(drums);
	for (const int pad : {42, 46})
	{
		bench.set(pad, DrumMachine::Instrument, static_cast<double>(Layer::HiHat));
		bench.set(pad, DrumMachine::Decay, 2000.0);
	}
	bench.add(note(0, true, 46));
	bench.add(note(10, false, 46)); // without the gate, a note-off changes nothing
	bench.left(1);
	CHECK(drums.activeVoices() == 1);
	bench.add(note(0, true, 42));
	bench.left(10);
	CHECK(drums.activeVoices() == 2); // no group yet

	for (const int pad : {42, 46})
		bench.set(pad, DrumMachine::ExclusiveGroup, 3.0);
	bench.add(note(0, true, 46));
	bench.left(10); // 5 ms: the others are gone
	CHECK(drums.activeVoices() == 1);
	bench.add(note(0, true, 42));
	bench.left(10);
	CHECK(drums.activeVoices() == 1);

	// The gate: the note-off fades the pad out within 5 ms.
	drums.reset();
	bench.set(42, DrumMachine::Gate, 1.0);
	bench.add(note(0, true, 42));
	bench.add(note(32, false, 42));
	const std::vector<float> gated = bench.left(8);
	CHECK(drums.activeVoices() == 0);
	CHECK(peak(gated, 32 + 240) == 0.0);
	CHECK(peak(gated, 0, 32) > 0.0);
}

TEST_CASE("the percussion synths sound, decay and render the same in any block size", "[unit]")
{
	for (const Layer layer :
		{Layer::Kick, Layer::Snare, Layer::HiHat, Layer::Tom, Layer::Clap, Layer::Cymbal, Layer::Cowbell})
	{
		INFO("layer " << static_cast<int>(layer));
		std::vector<float> renders[2];
		for (const uint32_t frames : {64u, 37u})
		{
			DrumMachine drums;
			Bench bench(drums, frames);
			bench.set(60, DrumMachine::Instrument, static_cast<double>(layer));
			bench.set(60, DrumMachine::Decay, 100.0);
			bench.add(note(0, true, 60));
			renders[frames == 37 ? 1 : 0] = bench.left(static_cast<int>(9600 / frames) + 1);
			renders[frames == 37 ? 1 : 0].resize(9600);
			CHECK(drums.activeVoices() == 0); // -80 dB after 133 ms
		}
		CHECK(renders[0] == renders[1]);
		CHECK(peak(renders[0], 0, 480) > 0.05);
		CHECK(peak(renders[0], 4800) < peak(renders[0], 0, 480) * 0.01);
	}

	// Hold keeps the level before the decay.
	DrumMachine drums;
	Bench bench(drums);
	bench.set(60, DrumMachine::Instrument, static_cast<double>(Layer::Tom));
	bench.set(60, DrumMachine::Decay, 50.0);
	bench.set(60, DrumMachine::Hold, 100.0);
	bench.add(note(0, true, 60));
	const std::vector<float> held = bench.left(150);
	CHECK(peak(held, 4000, 4800) > 0.3);
	CHECK(peak(held, 8000) < 0.01);
}

TEST_CASE("the drum machine's delay and reverb play what the pads send them", "[unit]")
{
	DrumMachine drums;
	drums.setSample(36, ramp(10));
	Bench bench(drums);
	bench.set(36, DrumMachine::Instrument, static_cast<double>(Layer::Sample));
	bench.set(36, DrumMachine::DelaySend, 100.0);
	bench.add(parameter(0, DrumMachine::DelayTime, 10.0)); // 480 frames
	bench.add(parameter(0, DrumMachine::DelayFeedback, 0.0));
	bench.add(note(0, true, 36));
	const std::vector<float> delayed = bench.left(16);
	CHECK_THAT(delayed[480 + 4], WithinAbs(5.0 / 1024.0, 1e-6));
	CHECK(peak(delayed, 20, 470) == 0.0);

	// Bypass All Effects: the dry pad alone.
	drums.reset();
	bench.add(parameter(0, DrumMachine::BypassEffects, 1.0));
	bench.add(note(0, true, 36));
	CHECK(peak(bench.left(16), 20) == 0.0);

	// The reverb rings on after the pad.
	drums.reset();
	bench.add(parameter(0, DrumMachine::BypassEffects, 0.0));
	bench.set(36, DrumMachine::DelaySend, 0.0);
	bench.set(36, DrumMachine::ReverbSend, 100.0);
	bench.add(note(0, true, 36));
	CHECK(peak(bench.left(64), 2000) > 0.0);
}

TEST_CASE("processor.list has the drum machine, an instrument with every pad's parameters", "[unit]")
{
	const processors::BuiltinInfo* info = processors::findBuiltin("drumMachine");
	REQUIRE(info != nullptr);
	CHECK(info->instrument);
	const processors::ParameterInfo* layer =
		processors::findParameter(*info, padParameter(127, DrumMachine::Instrument));
	REQUIRE(layer != nullptr);
	CHECK(layer->name == "Pad 127 instrument");
	CHECK(layer->options.size() == 9);
	CHECK(processors::findParameter(*info, DrumMachine::ReverbLevel) != nullptr);
	CHECK(processors::findParameter(*info, padParameter(0, DrumMachine::ReverbSend)) != nullptr);
}

namespace
{
	namespace fs = std::filesystem;

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

	double instrumentParameter(const Json& channel, uint32_t id)
	{
		for (const Json& item : channel.at("instrumentParameters"))
			if (item.at("parameter") == id)
				return item.at("value").get<double>();
		return -1.0;
	}

	Json padOf(const Json& channel, int pad)
	{
		for (const Json& item : channel.at("pads"))
			if (item.at("pad") == pad)
				return item;
		return nullptr;
	}

	// A project with a mono sample in the pool: a 220 Hz sine, a tenth of a second at 48 kHz, decaying.
	class Session
	{
	public:
		Session()
		{
			run(client, "audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}, {"captureSeconds", 10}});
			gnubase::test::Audio audio;
			audio.channels = 1;
			for (int i = 0; i < 4800; ++i)
				audio.samples.push_back(static_cast<float>(
					0.5 * std::exp(-30.0 * i / g_sampleRate) * std::sin(2.0 * g_pi * 220.0 * i / g_sampleRate)));
			file = folder.path() / "tick.wav";
			gnubase::test::writeWav(file, audio);
			const Json media = run(client, "media.add", {{"path", file.u8string()}});
			clip = run(client, "clip.add", {{"media", media.at("media").at("id")}}).at("clip").at("id").get<ObjectId>();
		}

		gnubase::test::TempDir folder;
		Engine engine{gnubase::audio::makeBackends(), folder.path() / "settings"}; // for track presets
		Client client{engine};
		fs::path file;
		ObjectId clip = 0;
	};

	// Renders the main mix from the project start, interleaved stereo, after what sounded before has faded out.
	gnubase::test::Audio render(Client& client, int64_t frames)
	{
		const auto start = run(client, "audio.render", {{"frames", 48000}}).at("capturedFrames").get<int64_t>();
		run(client, "transport.locate", {{"position", 0}});
		run(client, "transport.play");
		run(client, "audio.render", {{"frames", frames}});
		run(client, "transport.stop");
		const Json parameters = {{"start", start}, {"frames", frames}};
		Json left = parameters;
		left["channel"] = 0;
		Json right = parameters;
		right["channel"] = 1;
		const auto l = run(client, "audio.capture", left).at("samples").get<std::vector<float>>();
		const auto r = run(client, "audio.capture", right).at("samples").get<std::vector<float>>();
		gnubase::test::Audio audio;
		audio.channels = 2;
		audio.sampleRate = static_cast<int>(g_sampleRate);
		for (size_t i = 0; i < l.size(); ++i)
		{
			audio.samples.push_back(l[i]);
			audio.samples.push_back(r[i]);
		}
		return audio;
	}
}

TEST_CASE("drum.create makes a drum track with a kit of synths on the General MIDI notes", "[integration]")
{
	Session session;
	Client& client = session.client;
	const Json created = run(client, "drum.create");
	CHECK(created.at("track").at("name") == "Drum 01");
	CHECK(created.at("track").at("type") == "instrument");
	const Json& channel = created.at("channel");
	CHECK(channel.at("instrument") == "drumMachine");
	// The kit's pads have no names of their own: they go by the General MIDI sounds', which processor.list has.
	CHECK(channel.at("pads").empty());
	const Json list = run(client, "processor.list");
	for (const Json& processor : list.at("processors"))
		if (processor.at("id") == "drumMachine")
		{
			REQUIRE(processor.at("padNames").size() == 128);
			CHECK(processor.at("padNames").at(36) == "Bass Drum 1");
			CHECK(processor.at("padNames").at(56) == "Cowbell");
		}
	CHECK(instrumentParameter(channel, padParameter(36, DrumMachine::Instrument)) == static_cast<double>(Layer::Kick));
	CHECK(instrumentParameter(channel, padParameter(38, DrumMachine::Instrument)) == static_cast<double>(Layer::Snare));
	for (const int hat : {42, 44, 46})
		CHECK(instrumentParameter(channel, padParameter(hat, DrumMachine::ExclusiveGroup)) == 1.0);
	CHECK(instrumentParameter(channel, padParameter(37, DrumMachine::Instrument)) == -1.0); // off, the default

	const Json empty = run(client, "drum.create", {{"kit", false}, {"name", "Beats"}});
	CHECK(empty.at("track").at("name") == "Beats");
	CHECK(empty.at("channel").at("instrumentParameters").empty());
	CHECK(empty.at("channel").at("pads").empty());
	CHECK(run(client, "drum.create").at("track").at("name") == "Drum 02");
	run(client, "history.undo");
	CHECK(run(client, "project.get").at("tracks").size() == 2);
}

TEST_CASE("drum.loadPad, drum.setPad, drum.copyPad and drum.resetPad change the pads", "[integration]")
{
	Session session;
	Client& client = session.client;
	const auto track = run(client, "drum.create").at("track").at("id").get<ObjectId>();

	// A sample onto the kick's pad: its settings go, the sample plays from its start to its end.
	run(client, "parameter.set",
		{{"channel", run(client, "project.get").at("channels").back().at("id")},
			{"target", {{"type", "instrument"}, {"parameter", padParameter(36, DrumMachine::Level)}}},
			{"value", -3.0}});
	Json loaded = run(client, "drum.loadPad", {{"track", track}, {"pad", 36}, {"clip", session.clip}, {"start", 1200}});
	Json channel = loaded.at("channel");
	CHECK(padOf(channel, 36).at("sample") == session.clip);
	CHECK(
		instrumentParameter(channel, padParameter(36, DrumMachine::Instrument)) == static_cast<double>(Layer::Sample));
	CHECK(instrumentParameter(channel, padParameter(36, DrumMachine::Start)) == 0.25);
	CHECK(instrumentParameter(channel, padParameter(36, DrumMachine::Level)) == -1.0);
	CHECK(instrumentParameter(channel, padParameter(36, DrumMachine::Decay)) == -1.0);

	// keepSettings keeps them; a file goes into the pool.
	channel = run(client, "drum.loadPad",
		{{"track", track}, {"pad", 38}, {"path", session.file.u8string()}, {"keepSettings", true}})
				  .at("channel");
	CHECK(padOf(channel, 38).at("sample") != session.clip);
	CHECK(instrumentParameter(channel, padParameter(38, DrumMachine::Decay)) == 220.0);
	CHECK(
		instrumentParameter(channel, padParameter(38, DrumMachine::Instrument)) == static_cast<double>(Layer::Sample));

	channel = run(client, "drum.setPad", {{"track", track}, {"pad", 36}, {"name", "Boom"}}).at("channel");
	CHECK(padOf(channel, 36).at("name") == "Boom");

	// Cut and paste: the pad with its name, sample and settings moves.
	channel = run(client, "drum.copyPad", {{"track", track}, {"pad", 36}, {"to", 60}, {"move", true}}).at("channel");
	CHECK(padOf(channel, 60).at("name") == "Boom");
	CHECK(padOf(channel, 60).at("sample") == session.clip);
	CHECK(instrumentParameter(channel, padParameter(60, DrumMachine::Start)) == 0.25);
	CHECK(padOf(channel, 36).is_null());
	CHECK(instrumentParameter(channel, padParameter(36, DrumMachine::Instrument)) == -1.0);
	channel = run(client, "drum.copyPad", {{"track", track}, {"pad", 60}, {"to", 61}}).at("channel");
	CHECK(padOf(channel, 60).at("name") == "Boom");
	CHECK(padOf(channel, 61).at("name") == "Boom");

	// The clip is in use: clip.remove refuses it.
	CHECK_THAT(reject(client, "clip.remove", {{"clip", session.clip}}), ContainsSubstring("drum track"));

	channel = run(client, "drum.resetPad", {{"track", track}, {"pad", 61}}).at("channel");
	CHECK(padOf(channel, 61).is_null());
	CHECK(instrumentParameter(channel, padParameter(61, DrumMachine::Instrument)) == -1.0);
	channel = run(client, "drum.resetPad", {{"track", track}}).at("channel");
	CHECK(channel.at("pads").empty());
	CHECK(channel.at("instrumentParameters").empty());
	run(client, "history.undo");
	CHECK(padOf(run(client, "project.get").at("channels").back(), 60).at("name") == "Boom");

	const auto audio = run(client, "track.add", {{"type", "audio"}}).at("track").at("id");
	CHECK_THAT(reject(client, "drum.setPad", {{"track", audio}, {"pad", 1}, {"name", "x"}}),
		ContainsSubstring("no drum track"));
	CHECK_THAT(
		reject(client, "drum.loadPad", {{"track", track}, {"pad", 1}, {"clip", 999}}), ContainsSubstring("no clip"));
}

TEST_CASE("a drum track's patterns have a lane per pad, named after it", "[integration]")
{
	Session session;
	Client& client = session.client;
	const auto track = run(client, "drum.create").at("track").at("id").get<ObjectId>();
	const Json pattern = run(client, "pattern.add", {{"track", track}}).at("pattern");
	std::vector<int> pitches;
	for (const Json& lane : pattern.at("lanes"))
		pitches.push_back(lane.at("pitch").get<int>());
	CHECK(pitches == std::vector<int>{36, 38, 39, 42, 44, 45, 46, 48, 49, 50, 56});
	CHECK(pattern.at("lanes").at(0).at("name") == "Bass Drum 1");

	// Renaming a pad renames its lanes; a sample names a pad without a name of its own, as the kit's are.
	const auto lanes = [&] { return run(client, "project.get").at("patterns").at(0).at("lanes"); };
	run(client, "drum.loadPad", {{"track", track}, {"pad", 36}, {"clip", session.clip}});
	CHECK(lanes().at(0).at("name") == "tick");
	run(client, "drum.setPad", {{"track", track}, {"pad", 36}, {"name", "Boom"}});
	CHECK(lanes().at(0).at("name") == "Boom");
	run(client, "drum.setPad", {{"track", track}, {"pad", 36}, {"name", ""}});
	CHECK(lanes().at(0).at("name") == "tick");
	run(client, "drum.resetPad", {{"track", track}, {"pad", 36}});
	CHECK(lanes().at(0).at("name") == "Bass Drum 1");
}

TEST_CASE("a drum track's pads are saved with the project, and a missing clip leaves its pad empty", "[integration]")
{
	Session session;
	Client& client = session.client;
	const auto track = run(client, "drum.create").at("track").at("id").get<ObjectId>();
	run(client, "drum.loadPad", {{"track", track}, {"pad", 40}, {"clip", session.clip}});
	run(client, "drum.setPad", {{"track", track}, {"pad", 41}, {"name", "Shaker"}});
	const fs::path file = session.folder.path() / "drums.dawproject";
	run(client, "project.save", {{"path", file.u8string()}});
	run(client, "project.new");
	run(client, "project.load", {{"path", file.u8string()}});
	const Json channel = run(client, "project.get").at("channels").back();
	CHECK(channel.at("instrument") == "drumMachine");
	CHECK(padOf(channel, 40).at("sample").get<ObjectId>() != 0);
	CHECK(padOf(channel, 41).at("name") == "Shaker");
	CHECK(instrumentParameter(channel, padParameter(36, DrumMachine::Instrument)) == static_cast<double>(Layer::Kick));

	// Another instrument drops the pads.
	const Json changed = run(client, "channel.set", {{"channel", channel.at("id")}, {"instrument", "testSynth"}});
	CHECK(changed.at("channel").at("pads").empty());
}

TEST_CASE("a drum track renders its pads from MIDI notes and pattern events", "[integration]")
{
	Session session;
	Client& client = session.client;
	const Json created = run(client, "drum.create");
	const auto track = created.at("track").at("id").get<ObjectId>();
	run(client, "drum.loadPad", {{"track", track}, {"pad", 37}, {"clip", session.clip}});

	// The sample's pad plays it as it is, from the note's start.
	const int64_t quarter = gnubase::time::g_ticksPerQuarter;
	const Json part =
		run(client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", 4 * quarter}}).at("part");
	run(client, "note.add",
		{{"part", part.at("id")},
			{"notes", Json::array({{{"pitch", 37}, {"position", 0}, {"length", quarter}, {"velocity", 1.0}}})}});
	const gnubase::test::Audio played = render(client, 4800);
	const gnubase::test::Audio sample = gnubase::test::readWav(session.file);
	for (size_t frame = 0; frame < 4800; frame += 37)
	{
		INFO("frame " << frame);
		CHECK_THAT(played.samples[2 * frame], WithinAbs(sample.samples[frame], 1e-6));
		CHECK(played.samples[(2 * frame) + 1] == played.samples[2 * frame]);
	}
	run(client, "midiPart.remove", {{"part", part.at("id")}});

	// A beat of the kit as a pattern event, with the snare in the reverb and the clap in the delay.
	const auto channel = created.at("channel").at("id");
	const auto set = [&](uint32_t id, double value)
	{
		run(client, "parameter.set",
			{{"channel", channel}, {"target", {{"type", "instrument"}, {"parameter", id}}}, {"value", value}});
	};
	set(padParameter(38, DrumMachine::ReverbSend), 30.0);
	set(padParameter(39, DrumMachine::DelaySend), 40.0);
	set(padParameter(46, DrumMachine::Pan), -0.4);
	const Json pattern = run(client, "pattern.add", {{"track", track}}).at("pattern");
	Json lanes = Json::array();
	const std::map<int, std::vector<int>> steps = {
		{36, {0, 7, 8}}, {38, {4, 12}}, {39, {14}}, {42, {0, 2, 4, 6, 8, 10, 12}}, {46, {14}}};
	for (const auto& [pitch, indices] : steps)
	{
		Json lane = {{"pitch", pitch}, {"steps", Json::array()}};
		for (const int index : indices)
			lane["steps"].push_back({{"index", index}, {"velocity", 100}});
		lanes.push_back(lane);
	}
	run(client, "pattern.set", {{"pattern", pattern.at("id")}, {"lanes", lanes}});
	run(client, "midiPart.add",
		{{"track", track}, {"position", 0}, {"length", 4 * quarter}, {"pattern", pattern.at("id")}});
	const gnubase::test::Audio beat = render(client, 96000);
	const gnubase::test::AudioDiff diff = gnubase::test::compareWithGolden("drum-track-beat", beat, 1e-5f);
	INFO(diff.report);
	CHECK(diff.equal);
}

TEST_CASE("a drum pad's automated parameters play their automation, the others their values", "[integration]")
{
	Session session;
	Client& client = session.client;
	const auto track = run(client, "drum.create", {{"kit", false}}).at("track").at("id").get<ObjectId>();
	run(client, "drum.loadPad", {{"track", track}, {"pad", 37}, {"clip", session.clip}});
	const int64_t quarter = gnubase::time::g_ticksPerQuarter;
	const Json part =
		run(client, "midiPart.add", {{"track", track}, {"position", 0}, {"length", 4 * quarter}}).at("part");
	run(client, "note.add",
		{{"part", part.at("id")},
			{"notes",
				Json::array({{{"pitch", 37}, {"position", 0}, {"length", quarter}, {"velocity", 1.0}},
					{{"pitch", 37}, {"position", quarter}, {"length", quarter}, {"velocity", 1.0}}})}});
	const auto peaks = [&]
	{
		const gnubase::test::Audio audio = render(client, 48000);
		double first = 0.0;
		double second = 0.0;
		for (size_t frame = 0; frame < 48000; ++frame)
			(frame < 24000 ? first : second) =
				std::max(frame < 24000 ? first : second, static_cast<double>(std::abs(audio.samples[2 * frame])));
		return std::pair{first, second};
	};
	const auto [plainFirst, plainSecond] = peaks();
	CHECK(plainFirst > 0.4);
	CHECK(plainSecond == plainFirst);

	// The pad's level: silent for the first note, then back up.
	const auto lane = run(client, "automationLane.add",
		{{"track", track}, {"target", {{"type", "instrument"}, {"parameter", padParameter(37, DrumMachine::Level)}}}})
						  .at("lane")
						  .at("id");
	run(client, "automationPoint.add",
		{{"lane", lane},
			{"points",
				{{{"position", 0}, {"value", -96.0}, {"shape", "step"}},
					{{"position", quarter / 2}, {"value", 0.0}, {"shape", "step"}}}}});
	const auto [automatedFirst, automatedSecond] = peaks();
	CHECK(automatedFirst < 1e-4);
	CHECK_THAT(automatedSecond, WithinAbs(plainSecond, 1e-6));

	// Without the automation the value set plays again.
	run(client, "parameter.set",
		{{"channel", run(client, "project.get").at("channels").back().at("id")},
			{"target", {{"type", "instrument"}, {"parameter", padParameter(37, DrumMachine::Level)}}},
			{"value", -6.0}});
	run(client, "automationLane.remove", {{"lane", lane}});
	const auto [setFirst, setSecond] = peaks();
	CHECK_THAT(setFirst, WithinAbs(plainFirst * std::pow(10.0, -6.0 / 20.0), 1e-5));
	CHECK_THAT(setSecond, WithinAbs(setFirst, 1e-6));
}

TEST_CASE(
	"a drum track's pads keep their clips in the pool, and go along with track presets and imports", "[integration]")
{
	Session session;
	Client& client = session.client;
	const auto track = run(client, "drum.create").at("track").at("id").get<ObjectId>();
	run(client, "drum.loadPad", {{"track", track}, {"pad", 40}, {"clip", session.clip}});
	run(client, "drum.setPad", {{"track", track}, {"pad", 41}, {"name", "Shaker"}});
	// A clip a pad plays is used: pool.removeUnused keeps it.
	CHECK(run(client, "pool.get").at("clips").at(0).at("used") == 1);
	CHECK(run(client, "pool.removeUnused").at("clips").empty());

	// The pads with their samples, which come into the pool, with every track a preset gives its sound or adds.
	const auto channelOf = [&](ObjectId id)
	{
		const Json project = run(client, "project.get");
		for (const Json& each : project.at("tracks"))
			if (each.at("id") == id)
				for (const Json& channel : project.at("channels"))
					if (channel.at("id") == each.at("channel"))
						return channel;
		return Json();
	};
	const auto playsTick = [&](const Json& channel)
	{
		INFO(channel.dump());
		CHECK(channel.at("instrument") == "drumMachine");
		CHECK(padOf(channel, 41).at("name") == "Shaker");
		CHECK(instrumentParameter(channel, padParameter(40, DrumMachine::Instrument)) ==
			static_cast<double>(Layer::Sample));
		const Json pad = padOf(channel, 40);
		REQUIRE(pad.is_object());
		const Json project = run(client, "project.get");
		Json media;
		for (const Json& clip : project.at("clips"))
			if (clip.at("id") == pad.at("sample"))
				for (const Json& file : project.at("media"))
					if (file.at("id") == clip.at("media"))
						media = file;
		REQUIRE(media.is_object());
		CHECK(media.at("length") == 4800);
		return fs::u8path(media.at("path").get<std::string>());
	};
	run(client, "track.savePreset", {{"tracks", Json::array({track})}, {"name", "Kit"}});
	const auto kit = run(client, "drum.create").at("track").at("id").get<ObjectId>();
	const auto synth =
		run(client, "track.add", {{"type", "instrument"}, {"instrument", "testSynth"}}).at("track").at("id");
	run(client, "track.loadPreset", {{"tracks", Json::array({kit})}, {"name", "Kit"}});
	run(client, "track.loadPreset", {{"tracks", Json::array({synth})}, {"name", "Kit"}});
	playsTick(channelOf(kit));
	playsTick(channelOf(synth.get<ObjectId>()));
	run(client, "history.undo");
	CHECK(channelOf(synth.get<ObjectId>()).at("pads").empty());
	const Json added = run(client, "track.addFromPreset", {{"name", "Kit"}});
	playsTick(channelOf(added.at("tracks").at(0).get<ObjectId>()));

	// track.import from another project.
	const fs::path file = session.folder.path() / "drums.dawproject";
	run(client, "project.save", {{"path", file.u8string()}});
	run(client, "project.new");
	const Json imported = run(client, "track.import", {{"path", file.u8string()}});
	const fs::path copy = playsTick(channelOf(imported.at("tracks").at(0).get<ObjectId>()));

	// A missing file empties the pads that played it.
	fs::remove(copy);
	CHECK(!run(client, "media.removeMissing").at("clips").empty());
	const Json project = run(client, "project.get");
	for (const Json& channel : project.at("channels"))
		if (channel.at("instrument") == "drumMachine")
		{
			const Json pad = padOf(channel, 40);
			CHECK((pad.is_null() || pad.at("sample") == 0));
			CHECK(padOf(channel, 41).at("name") == "Shaker");
		}
}
