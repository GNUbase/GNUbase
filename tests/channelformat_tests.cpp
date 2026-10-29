// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/channelformat.h"
#include "engine/document/rendersnapshot.h"
#include "engine/engine.h"
#include "engine/graph/graph.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cmath>
#include <string>
#include <utility>
#include <vector>

using namespace gnubase::engine;
using Catch::Approx;
using Catch::Matchers::ContainsSubstring;

namespace
{
	// The levels of the reference's downmix tables.
	const double g_minus3 = std::pow(10.0, -3.0103 / 20.0);
	const double g_minus6 = std::pow(10.0, -6.0206 / 20.0);

	constexpr ChannelFormat g_formats[] = {ChannelFormat::Mono, ChannelFormat::Stereo, ChannelFormat::Lrc,
		ChannelFormat::Lrs, ChannelFormat::LrcLfe, ChannelFormat::LrsLfe, ChannelFormat::Lrcs, ChannelFormat::LrcsLfe,
		ChannelFormat::Quadro, ChannelFormat::QuadroLfe, ChannelFormat::Surround50, ChannelFormat::Surround51,
		ChannelFormat::Cine60, ChannelFormat::Music60, ChannelFormat::Surround70, ChannelFormat::Surround71};

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

	// The gains as rows, one per destination channel.
	std::vector<std::vector<double>> rows(ChannelFormat from, ChannelFormat to, double centerGain = g_minus3)
	{
		const std::vector<float> gains = conversionGains(from, to, centerGain);
		const auto columns = static_cast<size_t>(channelCount(from));
		REQUIRE(gains.size() == columns * static_cast<size_t>(channelCount(to)));
		std::vector<std::vector<double>> result;
		for (size_t row = 0; row * columns < gains.size(); ++row)
			result.emplace_back(gains.begin() + static_cast<std::ptrdiff_t>(row * columns),
				gains.begin() + static_cast<std::ptrdiff_t>((row + 1) * columns));
		return result;
	}

	void checkRows(const std::vector<std::vector<double>>& actual, const std::vector<std::vector<double>>& expected)
	{
		REQUIRE(actual.size() == expected.size());
		for (size_t row = 0; row < actual.size(); ++row)
		{
			INFO("destination channel " << row);
			REQUIRE(actual[row].size() == expected[row].size());
			for (size_t column = 0; column < actual[row].size(); ++column)
			{
				INFO("source channel " << column);
				CHECK(actual[row][column] == Approx(expected[row][column]).margin(1e-6));
			}
		}
	}
}

TEST_CASE("channel formats have names, a fixed speaker order and a default per channel count", "[unit]")
{
	CHECK(speakers(ChannelFormat::Surround51) ==
		std::vector<Speaker>{Speaker::L, Speaker::R, Speaker::C, Speaker::Lfe, Speaker::Ls, Speaker::Rs});
	CHECK(speakers(ChannelFormat::Surround71) ==
		std::vector<Speaker>{
			Speaker::L, Speaker::R, Speaker::C, Speaker::Lfe, Speaker::Ls, Speaker::Rs, Speaker::Sl, Speaker::Sr});
	CHECK(speakers(ChannelFormat::Mono) == std::vector<Speaker>{Speaker::M});
	CHECK(channelCount(ChannelFormat::None) == 0);
	CHECK(formatNames().size() == std::size(g_formats));
	for (const ChannelFormat format : g_formats)
	{
		INFO(formatName(format));
		CHECK(formatFromName(formatName(format)) == format);
		CHECK(channelCount(format) == static_cast<int>(speakers(format).size()));
		CHECK(channelCount(format) <= g_maxFormatChannels);
	}
	CHECK(formatName(ChannelFormat::Surround51) == std::string("5.1"));
	CHECK(formatFromName("5.2") == std::nullopt);
	CHECK(defaultFormat(1) == ChannelFormat::Mono);
	CHECK(defaultFormat(2) == ChannelFormat::Stereo);
	CHECK(defaultFormat(6) == ChannelFormat::Surround51);
	CHECK(defaultFormat(8) == ChannelFormat::Surround71);
	CHECK(defaultFormat(9) == ChannelFormat::None);
}

TEST_CASE("5.1 to stereo and 7.1 to 5.1 follow the reference's downmix tables", "[unit]")
{
	// Columns: L R C LFE Ls Rs.
	checkRows(rows(ChannelFormat::Surround51, ChannelFormat::Stereo),
		{{1, 0, g_minus3, g_minus3, g_minus6, 0}, {0, 1, g_minus3, g_minus3, 0, g_minus6}});
	// Columns: L R C LFE Ls Rs Sl Sr.
	checkRows(rows(ChannelFormat::Surround71, ChannelFormat::Surround51),
		{{1, 0, 0, 0, 0, 0, g_minus3, 0}, {0, 1, 0, 0, 0, 0, 0, g_minus3}, {0, 0, 1, 0, 0, 0, 0, 0},
			{0, 0, 0, 1, 0, 0, 0, 0}, {0, 0, 0, 0, 1, 0, g_minus3, 0}, {0, 0, 0, 0, 0, 1, 0, g_minus3}});
}

TEST_CASE("other downmixes chain through the tables", "[unit]")
{
	// 7.1 to stereo is 7.1 to 5.1, then 5.1 to stereo.
	const auto first = rows(ChannelFormat::Surround71, ChannelFormat::Surround51);
	const auto second = rows(ChannelFormat::Surround51, ChannelFormat::Stereo);
	std::vector<std::vector<double>> chained(2, std::vector<double>(8, 0.0));
	for (size_t to = 0; to < 2; ++to)
		for (size_t from = 0; from < 8; ++from)
			for (size_t middle = 0; middle < 6; ++middle)
				chained[to][from] += second[to][middle] * first[middle][from];
	checkRows(rows(ChannelFormat::Surround71, ChannelFormat::Stereo), chained);

	// Without LFE, 5.0 to stereo is the 5.1 table without its LFE column.
	checkRows(rows(ChannelFormat::Surround50, ChannelFormat::Stereo),
		{{1, 0, g_minus3, g_minus6, 0}, {0, 1, g_minus3, 0, g_minus6}});
	// Stereo to mono, and from there 5.1 to mono through stereo.
	checkRows(rows(ChannelFormat::Stereo, ChannelFormat::Mono), {{g_minus3, g_minus3}});
	checkRows(rows(ChannelFormat::Surround51, ChannelFormat::Mono),
		{{g_minus3, g_minus3, 1, 1, g_minus3 * g_minus6, g_minus3 * g_minus6}});
	// A surround center splits into Ls and Rs, and they merge into it.
	checkRows(rows(ChannelFormat::Lrcs, ChannelFormat::Surround50),
		{{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, g_minus3}, {0, 0, 0, g_minus3}});
	checkRows(
		rows(ChannelFormat::Quadro, ChannelFormat::Lrs), {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, g_minus3, g_minus3}});
}

TEST_CASE("mono reaches only C, or L and R at the pan law's center; stereo reaches L and R", "[unit]")
{
	checkRows(rows(ChannelFormat::Mono, ChannelFormat::Surround51), {{0}, {0}, {1}, {0}, {0}, {0}});
	checkRows(rows(ChannelFormat::Mono, ChannelFormat::Stereo, 0.5), {{0.5}, {0.5}});
	checkRows(rows(ChannelFormat::Mono, ChannelFormat::Quadro, 0.5), {{0.5}, {0.5}, {0}, {0}});
	checkRows(rows(ChannelFormat::Stereo, ChannelFormat::Surround51), {{1, 0}, {0, 1}, {0, 0}, {0, 0}, {0, 0}, {0, 0}});
	checkRows(rows(ChannelFormat::Surround51, ChannelFormat::Surround71),
		{{1, 0, 0, 0, 0, 0}, {0, 1, 0, 0, 0, 0}, {0, 0, 1, 0, 0, 0}, {0, 0, 0, 1, 0, 0}, {0, 0, 0, 0, 1, 0},
			{0, 0, 0, 0, 0, 1}, {0, 0, 0, 0, 0, 0}, {0, 0, 0, 0, 0, 0}});
}

TEST_CASE("every format converts to every other without losing a channel", "[unit]")
{
	for (const ChannelFormat from : g_formats)
		for (const ChannelFormat to : g_formats)
		{
			INFO(formatName(from) << " to " << formatName(to));
			const auto gains = rows(from, to);
			for (size_t column = 0; column < static_cast<size_t>(channelCount(from)); ++column)
			{
				double reached = 0.0;
				for (const std::vector<double>& row : gains)
				{
					CHECK(row[column] >= 0.0);
					reached += row[column];
				}
				CHECK(reached > 0.0);
			}
			if (from == to)
				for (size_t row = 0; row < gains.size(); ++row)
					for (size_t column = 0; column < gains[row].size(); ++column)
						CHECK(gains[row][column] == (row == column ? 1.0 : 0.0));
		}
	CHECK(conversionGains(ChannelFormat::None, ChannelFormat::Stereo, g_minus3).empty());
}

TEST_CASE("a 5.1 channel on a stereo bus plays the downmix; mono reaches only the center of 5.1", "[unit]")
{
	using namespace gnubase::engine::graph;
	// A 5.1 input bus feeds a 5.1 channel, which plays on a stereo output bus; a mono input bus feeds a mono channel
	// on a 5.1 output bus.
	RenderSnapshot snapshot;
	snapshot.busses = {{1, BusDirection::Input, ChannelFormat::Surround51},
		{2, BusDirection::Output, ChannelFormat::Stereo}, {3, BusDirection::Input, ChannelFormat::Mono},
		{4, BusDirection::Output, ChannelFormat::Surround51}};
	RenderChannel surround;
	surround.id = 10;
	surround.format = ChannelFormat::Surround51;
	surround.input = 0;
	surround.output = {RenderTarget::Kind::Bus, 1};
	RenderChannel mono;
	mono.id = 11;
	mono.format = ChannelFormat::Mono;
	mono.input = 2;
	mono.output = {RenderTarget::Kind::Bus, 3};
	snapshot.channels = {surround, mono};

	const GraphDesc graph = describe(snapshot);
	CHECK(graph.nodes[0].format == ChannelFormat::Surround51);
	CHECK(graph.nodes[4].format == ChannelFormat::Surround51);
	const CompiledGraph compiled(graph, 48000.0, 16);
	const float levels[] = {1.0f, 2.0f, 4.0f, 8.0f, 16.0f, 32.0f}; // L R C LFE Ls Rs
	for (int c = 0; c < 6; ++c)
		for (uint32_t i = 0; i < 16; ++i)
			compiled.audio(0)[c][i] = levels[c];
	for (uint32_t i = 0; i < 16; ++i)
		compiled.audio(2)[0][i] = 0.5f;
	compiled.process(16);

	CHECK(compiled.audio(1)[0][15] == Approx(1.0 + (g_minus3 * 12.0) + (g_minus6 * 16.0)).epsilon(1e-6));
	CHECK(compiled.audio(1)[1][15] == Approx(2.0 + (g_minus3 * 12.0) + (g_minus6 * 32.0)).epsilon(1e-6));
	for (int c = 0; c < 6; ++c)
		CHECK(compiled.audio(3)[c][15] == (c == 2 ? 0.5f : 0.0f));
}

TEST_CASE("busses and channels take a format; channels pick one by their count", "[unit]")
{
	Engine engine;
	Client client(engine);
	const Json bus = run(client, "bus.add", {{"direction", "output"}, {"name", "Surround"}, {"format", "5.1"}});
	CHECK(bus.at("bus").at("format") == "5.1");
	CHECK(bus.at("bus").at("channels") == 6);
	const Json music = run(client, "bus.add", {{"direction", "output"}, {"name", "Music"}, {"format", "6.0 music"}});
	CHECK(music.at("bus").at("channels") == 6);
	CHECK(run(client, "bus.add", {{"direction", "input"}, {"name", "Mic"}, {"channels", 1}}).at("bus").at("format") ==
		"mono");

	const Json added = run(client, "track.add", {{"type", "audio"}, {"format", "LRCS"}});
	const Json& channel = added.at("channel");
	CHECK(channel.at("format") == "LRCS");
	CHECK(channel.at("channels") == 4);
	const ObjectId id = channel.at("id").get<ObjectId>();
	CHECK(run(client, "channel.set", {{"channel", id}, {"format", "7.1"}}).at("channel").at("channels") == 8);
	run(client, "history.undo"); // one step puts the format back
	CHECK(run(client, "project.get").at("channels")[0].at("format") == "LRCS");
	CHECK(run(client, "channel.set", {{"channel", id}, {"channels", 4}}).at("channel").at("format") == "quadro");
	CHECK(run(client, "track.add", {{"type", "group"}}).at("channel").at("format") == "stereo");
	CHECK(run(client, "track.add", {{"type", "midi"}}).at("channel").at("format") == "none");

	CHECK_THAT(reject(client, "track.add", {{"type", "audio"}, {"format", "5.2"}}), ContainsSubstring("format"));
	CHECK_THAT(reject(client, "track.add", {{"type", "audio"}, {"channels", 9}}), ContainsSubstring("channels"));
	CHECK_THAT(reject(client, "channel.set", {{"channel", id}, {"format", "5.1"}, {"channels", 2}}),
		ContainsSubstring("format"));
	CHECK_THAT(reject(client, "bus.add", {{"direction", "output"}, {"name", "X"}, {"format", "none"}}),
		ContainsSubstring("format"));
}

TEST_CASE("a 5.1 track plays its downmix on the stereo output, through the engine", "[integration]")
{
	// Every device input carries the same sine; the stereo input takes the first two, the 5.1 input the next six.
	const auto render = [](const char* format)
	{
		Engine engine;
		Client client(engine);
		run(client, "audio.setDevice",
			{{"blockSize", 64}, {"inputChannels", 8}, {"captureSeconds", 1},
				{"input", {{"processor", "sine"}, {"parameters", {{"Frequency", 441.0}, {"Level", 0.1}}}}}});
		const Json track = run(client, "track.add", {{"type", "audio"}, {"format", format}});
		if (std::string(format) == "5.1")
		{
			const Json bus = run(client, "bus.add", {{"direction", "input"}, {"name", "5.1 In"}, {"format", "5.1"}});
			run(client, "channel.set", {{"channel", track.at("channel").at("id")}, {"input", bus.at("bus").at("id")}});
		}
		run(client, "audio.render", {{"frames", 1000}});
		std::vector<std::vector<float>> sides;
		sides.reserve(2);
		for (int channel = 0; channel < 2; ++channel)
			sides.push_back(run(client, "audio.capture", {{"channel", channel}, {"start", 0}, {"frames", 1000}})
					.at("samples")
					.get<std::vector<float>>());
		return sides;
	};
	const std::vector<std::vector<float>> sine = render("stereo");
	const std::vector<std::vector<float>> downmix = render("5.1");
	const double level = 1.0 + (2.0 * g_minus3) + g_minus6; // L, C, LFE and Ls into the left side
	float peak = 0.0f;
	size_t wrong = 0;
	for (size_t i = 0; i < 1000; ++i)
	{
		peak = std::max(peak, std::abs(sine[0][i]));
		for (size_t side = 0; side < 2; ++side)
			if (std::abs(downmix[side][i] - (sine[side][i] * level)) > 1e-6)
				++wrong;
	}
	CHECK(peak > 0.09f);
	CHECK(wrong == 0);
}
