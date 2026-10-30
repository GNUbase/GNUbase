// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/document.h"
#include "engine/engine.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <miniz.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
namespace test = gnubase::test;
using Catch::Matchers::ContainsSubstring;

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

	// The project's busses or channels, by value: a range-for over a member of a temporary would dangle.
	Json projectList(Client& client, const char* key)
	{
		return run(client, "project.get").at(key);
	}

	ObjectId addBus(Client& client, Json parameters)
	{
		return run(client, "bus.add", std::move(parameters)).at("bus").at("id").get<ObjectId>();
	}

	Json bus(Client& client, ObjectId id)
	{
		const Json busses = projectList(client, "busses");
		const auto entry = std::find_if(busses.begin(), busses.end(), [id](const Json& b) { return b.at("id") == id; });
		INFO("no bus " << id);
		REQUIRE(entry != busses.end());
		return *entry;
	}

	std::vector<ObjectId> busIds(Client& client, const char* direction)
	{
		std::vector<ObjectId> ids;
		for (const Json& entry : projectList(client, "busses"))
			if (entry.at("direction").get<std::string>() == direction)
				ids.push_back(entry.at("id").get<ObjectId>());
		return ids;
	}

	ObjectId mainMix(Client& client)
	{
		for (const Json& entry : projectList(client, "busses"))
			if (entry.at("mainMix") == true)
				return entry.at("id").get<ObjectId>();
		return 0;
	}

	// The dummy device with a quiet sine on every input.
	void openDevice(Client& client, int inputs, int outputs)
	{
		run(client, "audio.setDevice",
			{{"blockSize", 64}, {"inputChannels", inputs}, {"outputChannels", outputs}, {"captureSeconds", 1},
				{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 441.0}, {"Level", 0.1}}}}}});
	}

	// An audio track on the first input bus, which passes the sine on to output (a bus id).
	ObjectId addTrack(Client& client, ObjectId output, const char* format = "stereo")
	{
		const Json track = run(client, "track.add", {{"type", "audio"}, {"format", format}});
		const ObjectId channel = track.at("channel").at("id").get<ObjectId>();
		run(client, "channel.set", {{"channel", channel}, {"output", output}});
		return channel;
	}

	std::vector<float> capture(Client& client, int channel, int frames, int64_t start = 0)
	{
		return run(client, "audio.capture", {{"channel", channel}, {"start", start}, {"frames", frames}})
			.at("samples")
			.get<std::vector<float>>();
	}

	float peak(const std::vector<float>& samples)
	{
		float result = 0.0f;
		for (const float sample : samples)
			result = std::max(result, std::abs(sample));
		return result;
	}

	// The peak of each device output in the next frames rendered.
	std::vector<float> outputPeaks(Client& client, int outputs, int frames = 1000)
	{
		const auto start = run(client, "audio.get").at("capturedFrames").get<int64_t>();
		run(client, "audio.render", {{"frames", frames}});
		std::vector<float> peaks;
		peaks.reserve(static_cast<size_t>(outputs));
		for (int channel = 0; channel < outputs; ++channel)
			peaks.push_back(peak(capture(client, channel, frames, start)));
		return peaks;
	}

	constexpr float g_sinePeak = 0.1f;

	bool sounds(float level, float times = 1.0f)
	{
		return std::abs(level - (g_sinePeak * times)) < 0.002f;
	}

	Json names(std::initializer_list<const char*> ports)
	{
		Json result = Json::array();
		for (const char* port : ports)
			result.push_back(port);
		return result;
	}
}

TEST_CASE("busses use device ports by name, one per channel", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId out =
		addBus(client, {{"direction", "output"}, {"format", "stereo"}, {"ports", names({"Output 3", "Output 4"})}});
	CHECK(bus(client, out).at("ports") == names({"Output 3", "Output 4"}));
	CHECK(bus(client, busIds(client, "output")[0]).at("ports") == Json::array()); // the new project's: automatic

	run(client, "bus.set", {{"bus", out}, {"ports", names({"Output 1", ""})}});
	CHECK(bus(client, out).at("ports") == names({"Output 1", ""}));
	run(client, "history.undo");
	CHECK(bus(client, out).at("ports") == names({"Output 3", "Output 4"}));
	run(client, "bus.set", {{"bus", out}, {"ports", Json::array()}});
	CHECK(bus(client, out).at("ports") == Json::array());

	CHECK_THAT(reject(client, "bus.set", {{"bus", out}, {"ports", names({"Output 1"})}}), ContainsSubstring("2 ports"));
	CHECK_THAT(reject(client, "bus.add", {{"direction", "input"}, {"format", "mono"}, {"ports", names({"A", "B"})}}),
		ContainsSubstring("1 port"));
}

TEST_CASE("a bus without a name is named after its format and direction", "[unit]")
{
	Engine engine;
	Client client(engine);
	CHECK(bus(client, addBus(client, {{"direction", "input"}})).at("name") == "Stereo In 2");
	CHECK(bus(client, addBus(client, {{"direction", "output"}, {"format", "5.1"}})).at("name") == "5.1 Out");
	CHECK(bus(client, addBus(client, {{"direction", "output"}, {"channels", 1}})).at("name") == "Mono Out");
	CHECK(bus(client, addBus(client, {{"direction", "output"}, {"channels", 1}})).at("name") == "Mono Out 2");
	CHECK_THAT(reject(client, "bus.add", {{"format", "stereo"}}), ContainsSubstring("direction"));
}

TEST_CASE("the main mix is the default output and moves on when it is removed", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId stereoOut = busIds(client, "output")[0];
	CHECK(mainMix(client) == stereoOut);
	const ObjectId other = addBus(client, {{"direction", "output"}, {"name", "Other"}});
	CHECK(bus(client, other).at("mainMix") == false);

	run(client, "bus.set", {{"bus", other}, {"mainMix", true}});
	CHECK(mainMix(client) == other);
	CHECK(bus(client, stereoOut).at("mainMix") == false);
	const Json track = run(client, "track.add", {{"type", "audio"}});
	CHECK(track.at("channel").at("output") == other);

	run(client, "bus.remove", {{"bus", other}});
	CHECK(mainMix(client) == stereoOut);
	run(client, "history.undo");
	CHECK(mainMix(client) == other);

	CHECK_THAT(reject(client, "bus.set", {{"bus", other}, {"mainMix", false}}), ContainsSubstring("main mix"));
	CHECK_THAT(reject(client, "bus.set", {{"bus", busIds(client, "input")[0]}, {"mainMix", true}}),
		ContainsSubstring("output"));

	// The first output bus of a project without any becomes its main mix.
	run(client, "bus.remove", {{"bus", stereoOut}});
	run(client, "bus.remove", {{"bus", other}});
	CHECK(mainMix(client) == 0);
	const ObjectId first = addBus(client, {{"direction", "output"}});
	CHECK(mainMix(client) == first);
}

TEST_CASE("child busses take channels of their parent", "[unit]")
{
	Engine engine;
	Client client(engine);
	const ObjectId surround = addBus(client, {{"direction", "output"}, {"name", "Surround"}, {"format", "5.1"}});
	const ObjectId front = addBus(client, {{"parent", surround}, {"format", "stereo"}});
	const Json frontBus = bus(client, front);
	CHECK(frontBus.at("direction") == "output");
	CHECK(frontBus.at("parent") == surround);
	CHECK(frontBus.at("parentChannels") == Json::array({0, 1}));
	CHECK(frontBus.at("name") == "Stereo (L R)");
	const ObjectId rear = addBus(client, {{"parent", surround}, {"format", "stereo"}, {"parentChannels", {4, 5}}});
	CHECK(bus(client, rear).at("name") == "Stereo (Ls Rs)");
	CHECK(bus(client, addBus(client, {{"parent", surround}, {"format", "LRC"}})).at("parentChannels") ==
		Json::array({0, 1, 2}));

	CHECK_THAT(reject(client, "bus.add", {{"parent", front}, {"channels", 1}}), ContainsSubstring("child"));
	CHECK_THAT(reject(client, "bus.add", {{"parent", surround}, {"format", "5.1"}}), ContainsSubstring("fewer"));
	CHECK_THAT(reject(client, "bus.add", {{"parent", surround}, {"format", "stereo"}, {"parentChannels", {4, 6}}}),
		ContainsSubstring("parentChannels"));
	CHECK_THAT(reject(client, "bus.add", {{"parent", surround}, {"format", "stereo"}, {"parentChannels", {4, 4}}}),
		ContainsSubstring("parentChannels"));
	CHECK_THAT(reject(client, "bus.add", {{"parent", surround}, {"format", "stereo"}, {"parentChannels", {4}}}),
		ContainsSubstring("parentChannels"));
	CHECK_THAT(
		reject(client, "bus.add", {{"parent", surround}, {"direction", "input"}}), ContainsSubstring("direction"));
	CHECK_THAT(reject(client, "bus.add", {{"parent", surround}, {"format", "LRS"}}), ContainsSubstring("speakers"));
	CHECK_THAT(reject(client, "bus.set", {{"bus", rear}, {"ports", names({"Output 1", "Output 2"})}}),
		ContainsSubstring("parent"));
	CHECK_THAT(reject(client, "bus.set", {{"bus", rear}, {"mainMix", true}}), ContainsSubstring("child"));

	// A track routed to a child bus; the graph adds the child to its parent.
	const ObjectId channel = addTrack(client, rear);
	const Json graph = run(client, "graph.get");
	const auto index = [&graph](ObjectId object)
	{
		const Json& nodes = graph.at("nodes");
		for (size_t i = 0; i < nodes.size(); ++i)
			if (nodes[i].at("object") == object)
				return static_cast<int>(i);
		return -1;
	};
	CHECK(graph.at("nodes")[static_cast<size_t>(index(surround))].at("audioInputs") ==
		Json::array({index(front), index(rear), index(front) + 2}));
	CHECK(graph.at("nodes")[static_cast<size_t>(index(rear))].at("audioInputs") == Json::array({index(channel)}));

	// Removing the parent removes its children and disconnects what used them.
	run(client, "bus.remove", {{"bus", surround}});
	CHECK(busIds(client, "output").size() == 1);
	for (const Json& entry : projectList(client, "channels"))
		if (entry.at("id") == channel)
			CHECK(entry.at("output") == 0);
}

TEST_CASE("several busses are renamed with incrementing numbers or letters", "[unit]")
{
	Engine engine;
	Client client(engine);
	std::vector<ObjectId> ids = busIds(client, "output");
	for (int i = 0; i < 3; ++i)
		ids.push_back(addBus(client, {{"direction", "output"}}));
	const auto nameOf = [&client](ObjectId id) { return bus(client, id).at("name").get<std::string>(); };

	// From the bus whose name was edited to the bottom, then from the top.
	run(client, "bus.rename", {{"busses", {ids[2], ids[0], ids[3]}}, {"name", "Out 09"}});
	CHECK(nameOf(ids[2]) == "Out 09");
	CHECK(nameOf(ids[3]) == "Out 10");
	CHECK(nameOf(ids[0]) == "Out 11");
	CHECK(nameOf(ids[1]) == "Stereo Out 2");

	run(client, "bus.rename", {{"busses", {ids[1], ids[2]}}, {"name", "Bus Y"}});
	CHECK(nameOf(ids[1]) == "Bus Y");
	CHECK(nameOf(ids[2]) == "Bus Z");
	run(client, "history.undo");
	CHECK(nameOf(ids[1]) == "Stereo Out 2");

	CHECK_THAT(reject(client, "bus.rename", {{"busses", {ids[0], ids[1], ids[2]}}, {"name", "Bus Y"}}),
		ContainsSubstring("Z"));
	CHECK_THAT(
		reject(client, "bus.rename", {{"busses", {ids[0], ids[1]}}, {"name", "Bus"}}), ContainsSubstring("number"));
	CHECK_THAT(reject(client, "bus.rename", {{"busses", {ids[0], 999}}, {"name", "Bus 1"}}), ContainsSubstring("999"));
}

TEST_CASE("output busses play on their ports, and busses that share a port add up there", "[integration]")
{
	Engine engine;
	Client client(engine);
	openDevice(client, 2, 8);
	const ObjectId stereoOut = busIds(client, "output")[0];
	const ObjectId rear = addBus(client, {{"direction", "output"}, {"ports", names({"Output 3", "Output 4"})}});
	addTrack(client, stereoOut);
	addTrack(client, rear);
	std::vector<float> peaks = outputPeaks(client, 8);
	CHECK(sounds(peaks[0]));
	CHECK(sounds(peaks[1]));
	CHECK(sounds(peaks[2]));
	CHECK(sounds(peaks[3]));
	CHECK(peaks[4] == 0.0f);

	// The same port for both: they add up, and Output 3 falls silent.
	run(client, "bus.set", {{"bus", rear}, {"ports", names({"Output 1", "Output 6"})}});
	peaks = outputPeaks(client, 8);
	CHECK(sounds(peaks[0], 2.0f));
	CHECK(sounds(peaks[1]));
	CHECK(peaks[2] == 0.0f);
	CHECK(sounds(peaks[5]));

	// A child bus plays on its parent's ports, where its channels are.
	const ObjectId surround = addBus(client,
		{{"direction", "output"}, {"format", "5.1"},
			{"ports", names({"Output 3", "Output 4", "Output 5", "Output 6", "Output 7", "Output 8"})}});
	run(client, "bus.set", {{"bus", rear}, {"ports", names({"", ""})}});
	const ObjectId back = addBus(client, {{"parent", surround}, {"format", "stereo"}, {"parentChannels", {4, 5}}});
	addTrack(client, back);
	peaks = outputPeaks(client, 8);
	CHECK(sounds(peaks[0]));
	CHECK(peaks[2] == 0.0f);
	CHECK(peaks[5] == 0.0f);
	CHECK(sounds(peaks[6]));
	CHECK(sounds(peaks[7]));
}

TEST_CASE("input busses take their ports, input child busses their parent's channels", "[integration]")
{
	Engine engine;
	Client client(engine);
	openDevice(client, 4, 2);
	const ObjectId stereoIn = busIds(client, "input")[0];
	const ObjectId channel = addTrack(client, busIds(client, "output")[0]);
	CHECK(sounds(outputPeaks(client, 2)[0]));

	// A port the device doesn't have is silent; one it has plays.
	run(client, "bus.set", {{"bus", stereoIn}, {"ports", names({"Input 9", "Input 10"})}});
	CHECK(outputPeaks(client, 2)[0] == 0.0f);
	run(client, "bus.set", {{"bus", stereoIn}, {"ports", names({"Input 4", ""})}});
	std::vector<float> peaks = outputPeaks(client, 2);
	CHECK(sounds(peaks[0]));
	CHECK(peaks[1] == 0.0f);

	const ObjectId quadro = addBus(
		client, {{"direction", "input"}, {"format", "quadro"}, {"ports", names({"", "", "Input 1", "Input 2"})}});
	const ObjectId rear = addBus(client, {{"parent", quadro}, {"format", "stereo"}, {"parentChannels", {2, 3}}});
	run(client, "channel.set", {{"channel", channel}, {"input", rear}});
	peaks = outputPeaks(client, 2);
	CHECK(sounds(peaks[0]));
	CHECK(sounds(peaks[1]));
}

TEST_CASE("audio.ports lists the device's ports with their busses, and missing ports", "[integration]")
{
	Engine engine;
	Client client(engine);
	openDevice(client, 2, 4);
	const ObjectId stereoIn = busIds(client, "input")[0];
	const ObjectId stereoOut = busIds(client, "output")[0];
	const ObjectId cue = addBus(client, {{"direction", "output"}, {"ports", names({"Output 1", "Output 9"})}});
	const ObjectId front = addBus(client, {{"parent", cue}, {"format", "mono"}, {"parentChannels", {1}}});

	const Json ports = run(client, "audio.ports");
	REQUIRE(ports.at("inputs").size() == 2);
	CHECK(ports.at("inputs")[0] == Json{{"name", "Input 1"}, {"busses", {stereoIn}}});
	REQUIRE(ports.at("outputs").size() == 4);
	CHECK(ports.at("outputs")[0] == Json{{"name", "Output 1"}, {"busses", {stereoOut, cue}}});
	CHECK(ports.at("outputs")[2] == Json{{"name", "Output 3"}, {"busses", Json::array()}});
	CHECK(ports.at("busses") ==
		Json::array({{{"bus", stereoIn}, {"ports", names({"Input 1", "Input 2"})}},
			{{"bus", stereoOut}, {"ports", names({"Output 1", "Output 2"})}},
			{{"bus", cue}, {"ports", {"Output 1", nullptr}}}, {{"bus", front}, {"ports", {nullptr}}}}));
	CHECK(ports.at("missing") == Json::array({{{"name", "Output 9"}, {"direction", "output"}, {"busses", {cue}}}}));
}

TEST_CASE("a project opened on a device with fewer ports finds its missing ports and remaps them", "[integration]")
{
	const test::TempDir dir;
	const std::string file = (dir.path() / "ports.dawproject").u8string();
	Engine engine;
	Client client(engine);
	openDevice(client, 2, 8);
	const ObjectId main = busIds(client, "output")[0];
	run(client, "bus.set", {{"bus", main}, {"ports", names({"Output 1", "Output 2"})}});
	const ObjectId b =
		addBus(client, {{"direction", "output"}, {"name", "B"}, {"ports", names({"Output 3", "Output 4"})}});
	const ObjectId c =
		addBus(client, {{"direction", "output"}, {"name", "C"}, {"ports", names({"Output 5", "Output 6"})}});
	for (const ObjectId output : {main, b, c})
		addTrack(client, output);
	std::vector<float> peaks = outputPeaks(client, 8);
	CHECK(sounds(peaks[5]));
	run(client, "project.save", {{"path", file}});

	// On two outputs, B and C have no ports: they are listed and silent, and the project keeps their names.
	run(client, "project.new");
	openDevice(client, 2, 2);
	run(client, "project.load", {{"path", file}});
	const Json ports = run(client, "audio.ports");
	CHECK(ports.at("missing") ==
		Json::array({{{"name", "Output 3"}, {"direction", "output"}, {"busses", {b}}},
			{{"name", "Output 4"}, {"direction", "output"}, {"busses", {b}}},
			{{"name", "Output 5"}, {"direction", "output"}, {"busses", {c}}},
			{{"name", "Output 6"}, {"direction", "output"}, {"busses", {c}}}}));
	CHECK(bus(client, b).at("ports") == names({"Output 3", "Output 4"}));
	peaks = outputPeaks(client, 2);
	CHECK(sounds(peaks[0]));
	CHECK(sounds(peaks[1]));

	// Re-routed to the two outputs there are, all three busses play on them.
	run(client, "bus.remapPorts",
		{{"direction", "output"},
			{"ports",
				{{"Output 3", "Output 1"}, {"Output 4", "Output 2"}, {"Output 5", "Output 1"},
					{"Output 6", "Output 2"}}}});
	CHECK(run(client, "audio.ports").at("missing") == Json::array());
	CHECK(bus(client, c).at("ports") == names({"Output 1", "Output 2"}));
	openDevice(client, 2, 2); // from the start
	run(client, "audio.render", {{"frames", 4800}});
	test::Audio audio;
	audio.channels = 2;
	const std::vector<float> left = capture(client, 0, 4800);
	const std::vector<float> right = capture(client, 1, 4800);
	for (size_t i = 0; i < left.size(); ++i)
	{
		audio.samples.push_back(left[i]);
		audio.samples.push_back(right[i]);
	}
	CHECK(sounds(peak(left), 3.0f));
	const test::AudioDiff diff =
		test::compareWithGolden("bus-ports-remapped", audio, 1e-5f); // sin() differs by platform
	INFO(diff.report);
	CHECK(diff.equal);
	run(client, "history.undo");
	CHECK(run(client, "audio.ports").at("missing").size() == 4);

	// Back on the device it was made on, the project finds every port again.
	openDevice(client, 2, 8);
	CHECK(run(client, "audio.ports").at("missing") == Json::array());
	CHECK(sounds(outputPeaks(client, 8)[5]));

	CHECK_THAT(reject(client, "bus.remapPorts", {{"direction", "output"}, {"ports", {{"Output 3", 7}}}}),
		ContainsSubstring("string"));
}

TEST_CASE("busses, ports, child busses and the main mix are saved with the project", "[integration]")
{
	const test::TempDir dir;
	const std::string file = (dir.path() / "busses.dawproject").u8string();
	Engine engine;
	Client client(engine);
	const ObjectId surround = addBus(client,
		{{"direction", "output"}, {"name", "Surround"}, {"format", "5.1"},
			{"ports", names({"A", "B", "C", "D", "E", "F"})}});
	const ObjectId rear = addBus(client, {{"parent", surround}, {"format", "stereo"}, {"parentChannels", {4, 5}}});
	run(client, "bus.set", {{"bus", surround}, {"mainMix", true}});
	addTrack(client, rear);
	const Json before = run(client, "project.get").at("busses");
	run(client, "project.save", {{"path", file}});
	run(client, "project.new");
	run(client, "project.load", {{"path", file}});
	CHECK(run(client, "project.get").at("busses") == before);

	// project.xml: the main mix is the first master channel, and the child's track goes to its parent.
	mz_zip_archive zip{};
	REQUIRE(mz_zip_reader_init_file(&zip, file.c_str(), 0));
	size_t size = 0;
	void* data = mz_zip_reader_extract_file_to_heap(&zip, "project.xml", &size, 0);
	REQUIRE(data != nullptr);
	const std::string xml(static_cast<const char*>(data), size);
	mz_free(data);
	mz_zip_reader_end(&zip);
	const auto firstMaster = xml.find("role=\"master\"");
	REQUIRE(firstMaster != std::string::npos);
	CHECK(xml.rfind("name=\"Surround\"", firstMaster) != std::string::npos);
	CHECK(xml.rfind("name=\"Stereo Out\"", firstMaster) == std::string::npos);
	CHECK(xml.find("id=\"o" + std::to_string(rear) + "\"") == std::string::npos);
	CHECK(xml.find("destination=\"o" + std::to_string(surround) + "\"") != std::string::npos);
}

TEST_CASE("bus presets: standard, from the device's ports, and the user's own", "[integration]")
{
	Engine engine;
	Client client(engine);
	openDevice(client, 4, 2);
	const auto presets = [&client](const char* direction)
	{ return run(client, "bus.presets", {{"direction", direction}}).at("presets"); };
	const auto preset = [](const Json& list, const std::string& name)
	{
		const auto entry =
			std::find_if(list.begin(), list.end(), [&name](const Json& p) { return p.at("name") == name; });
		INFO("no preset " << name);
		REQUIRE(entry != list.end());
		return *entry;
	};

	const Json inputs = presets("input");
	CHECK(preset(inputs, "Stereo").at("kind") == "standard");
	CHECK(preset(inputs, "5.1").at("busses")[0].at("format") == "5.1");
	const Json monos = preset(inputs, "4 Mono Ins");
	CHECK(monos.at("kind") == "hardware");
	REQUIRE(monos.at("busses").size() == 4);
	CHECK(monos.at("busses")[3] ==
		Json{{"name", "Mono In 4"}, {"format", "mono"}, {"ports", names({"Input 4"})}, {"children", Json::array()}});
	CHECK(preset(inputs, "2 Stereo Ins").at("busses")[1].at("ports") == names({"Input 3", "Input 4"}));
	CHECK(preset(presets("output"), "1 Stereo Out").at("busses").size() == 1);

	// Applying a preset replaces the busses; channels move to the bus at the same place.
	const ObjectId channel = addTrack(client, busIds(client, "output")[0]);
	run(client, "bus.applyPreset", {{"direction", "input"}, {"name", "2 Stereo Ins"}});
	const std::vector<ObjectId> applied = busIds(client, "input");
	REQUIRE(applied.size() == 2);
	CHECK(bus(client, applied[1]).at("name") == "Stereo In 2");
	const auto inputOf = [&client, channel]
	{
		for (const Json& entry : projectList(client, "channels"))
			if (entry.at("id") == channel)
				return entry.at("input").get<ObjectId>();
		return ObjectId{0};
	};
	CHECK(inputOf() == applied[0]);
	CHECK(sounds(outputPeaks(client, 2)[0]));

	// The user's own presets, and Default for new projects.
	const ObjectId extra = addBus(client, {{"direction", "input"}, {"name", "Extra"}, {"format", "quadro"}});
	addBus(client, {{"parent", extra}, {"format", "stereo"}, {"parentChannels", {2, 3}}});
	run(client, "bus.savePreset", {{"direction", "input"}, {"name", "Default"}});
	const Json mine = preset(presets("input"), "Default");
	CHECK(mine.at("kind") == "user");
	REQUIRE(mine.at("busses").size() == 3);
	CHECK(mine.at("busses")[2].at("children")[0] ==
		Json{{"name", "Stereo (Ls Rs)"}, {"format", "stereo"}, {"parentChannels", {2, 3}}});
	run(client, "project.new");
	const Json busses = run(client, "project.get").at("busses");
	REQUIRE(busses.size() == 5); // three inputs, one child and the stereo output
	CHECK(busses[2].at("name") == "Extra");
	CHECK(busses[3].at("parent") == busses[2].at("id"));
	CHECK(busses[1].at("ports") == names({"Input 3", "Input 4"}));

	run(client, "bus.applyPreset", {{"direction", "input"}, {"name", "Mono"}});
	CHECK(busIds(client, "input").size() == 1);
	run(client, "history.undo");
	CHECK(busIds(client, "input").size() == 4);

	run(client, "bus.removePreset", {{"direction", "input"}, {"name", "Default"}});
	CHECK_THAT(
		reject(client, "bus.removePreset", {{"direction", "input"}, {"name", "Stereo"}}), ContainsSubstring("user"));
	CHECK_THAT(
		reject(client, "bus.applyPreset", {{"direction", "input"}, {"name", "Default"}}), ContainsSubstring("Default"));
}
