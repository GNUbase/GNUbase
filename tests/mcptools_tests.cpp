// SPDX-License-Identifier: GPL-3.0-or-later
// The tools for projects, tracks, parts and events, the transport and undo, used the way an MCP client uses them
// (#27): JSON-RPC requests as text, tool results back, and the project checked with project_get.
#include "engine/engine.h"
#include "mcp/server.h"
#include "testutils/testutils.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using Catch::Matchers::ContainsSubstring;
using gnubase::engine::Engine;
using gnubase::engine::Json;

namespace
{
	// An MCP client of an in-process server. Messages go through JSON text, as over a transport.
	class McpClient
	{
	public:
		explicit McpClient(Engine& engine) : m_server(engine)
		{
			const Json initialized = request("initialize",
				{{"protocolVersion", "2025-06-18"}, {"capabilities", Json::object()},
					{"clientInfo", {{"name", "gnubase-tests"}, {"version", "1"}}}});
			REQUIRE(initialized.contains("result"));
			CHECK(!m_server.handle(Json::parse(R"({"jsonrpc": "2.0", "method": "notifications/initialized"})"))
					.has_value());
		}

		// Calls a tool that must succeed; returns its result.
		Json call(const std::string& tool, const Json& arguments = Json::object())
		{
			const Json result = callTool(tool, arguments);
			INFO(tool << ": " << result.dump());
			REQUIRE(result.at("isError") == false);
			return result.value("structuredContent", Json::object());
		}

		// Calls a tool that must fail; returns why.
		std::string fails(const std::string& tool, const Json& arguments = Json::object())
		{
			const Json result = callTool(tool, arguments);
			INFO(tool << ": " << result.dump());
			REQUIRE(result.at("isError") == true);
			return result.at("content").at(0).at("text").get<std::string>();
		}

		Json project() { return call("project_get"); }

	private:
		Json callTool(const std::string& tool, const Json& arguments)
		{
			const Json response = request("tools/call", {{"name", tool}, {"arguments", arguments}});
			INFO(tool << ": " << response.dump());
			REQUIRE(response.contains("result"));
			return response.at("result");
		}

		Json request(const std::string& method, const Json& params)
		{
			const std::string text =
				Json{{"jsonrpc", "2.0"}, {"id", ++m_id}, {"method", method}, {"params", params}}.dump();
			const std::optional<Json> response = m_server.handle(Json::parse(text));
			REQUIRE(response.has_value());
			return Json::parse(response.value_or(Json()).dump());
		}

		gnubase::mcp::Server m_server;
		int m_id = 0;
	};

	std::vector<std::string> trackNames(const Json& project)
	{
		std::vector<std::string> names;
		for (const Json& track : project.at("tracks"))
			names.push_back(track.at("name").get<std::string>());
		return names;
	}

	int64_t idOf(const Json& object)
	{
		return object.at("id").get<int64_t>();
	}
}

TEST_CASE("MCP clients create, save and open projects", "[integration]")
{
	const gnubase::test::TempDir dir;
	Engine engine;
	McpClient mcp(engine);
	mcp.call("project_new");
	mcp.call("project_set", {{"name", "Demo"}});
	mcp.call("track_add", {{"type", "audio"}, {"name", "Vocals"}});
	const std::string path = (dir.path() / "demo.dawproject").u8string();
	mcp.call("project_save", {{"path", path}});
	CHECK(std::filesystem::exists(std::filesystem::u8path(path)));

	mcp.call("project_new");
	CHECK(mcp.project().at("name") == "Untitled");
	CHECK(mcp.project().at("tracks").empty());

	mcp.call("project_load", {{"path", path}});
	const Json project = mcp.project();
	CHECK(project.at("name") == "Demo");
	CHECK(trackNames(project) == std::vector<std::string>{"Vocals"});
	CHECK_THAT(mcp.fails("project_load", {{"path", (dir.path() / "missing.dawproject").u8string()}}),
		ContainsSubstring("missing.dawproject"));
}

TEST_CASE("MCP clients list, add, change, move and remove tracks", "[integration]")
{
	Engine engine;
	McpClient mcp(engine);
	const Json audio = mcp.call("track_add", {{"type", "audio"}});
	const Json folder = mcp.call("track_add", {{"type", "folder"}, {"name", "Drums"}});
	const Json midi = mcp.call("track_add", {{"type", "midi"}});
	CHECK(audio.at("channel").is_object());
	CHECK(folder.at("channel").is_null()); // folders get a channel only with groupChannel
	CHECK(trackNames(mcp.project()) == std::vector<std::string>{"Audio 01", "Drums", "MIDI 01"});

	const int64_t kick = idOf(audio.at("track"));
	const Json changed = mcp.call("track_set", {{"track", kick}, {"name", "Kick"}, {"timeBase", "linear"}});
	CHECK(changed.at("track").at("name") == "Kick");
	CHECK(changed.at("track").at("timeBase") == "linear");

	mcp.call("track_move", {{"track", kick}, {"parent", idOf(folder.at("track"))}, {"index", 0}});
	const Json moved = mcp.project();
	CHECK(trackNames(moved) == std::vector<std::string>{"Drums", "Kick", "MIDI 01"});
	CHECK(moved.at("tracks").at(1).at("parent") == idOf(folder.at("track")));

	mcp.call("track_remove", {{"track", idOf(midi.at("track"))}});
	CHECK(trackNames(mcp.project()) == std::vector<std::string>{"Drums", "Kick"});

	CHECK_THAT(mcp.fails("track_remove", {{"track", 999}}), ContainsSubstring("999"));
	CHECK_THAT(mcp.fails("track_add", {{"type", "nonsense"}}), ContainsSubstring("parameters.type"));
}

TEST_CASE("MCP clients add, change and remove MIDI parts, notes and controllers", "[integration]")
{
	Engine engine;
	McpClient mcp(engine);
	const int64_t track = idOf(mcp.call("track_add", {{"type", "instrument"}}).at("track"));
	const Json part =
		mcp.call("midiPart_add", {{"track", track}, {"position", 1920}, {"length", 3840}, {"name", "Verse"}})
			.at("part");
	const int64_t partId = idOf(part);
	CHECK(part.at("name") == "Verse");
	CHECK(part.at("position") == 1920);
	CHECK(mcp.call("midiPart_set", {{"part", partId}, {"length", 7680}}).at("part").at("length") == 7680);

	// Notes
	const Json notes = mcp.call("note_add",
							  {{"part", partId},
								  {"notes",
									  {{{"position", 0}, {"length", 480}, {"pitch", 60}, {"velocity", 1.0}},
										  {{"position", 480}, {"length", 480}, {"pitch", 64}, {"velocity", 0.0}}}}})
						   .at("notes");
	REQUIRE(notes.size() == 2);
	const Json changed =
		mcp.call("note_set", {{"part", partId}, {"notes", {{{"id", idOf(notes[0])}, {"pitch", 62}, {"length", 960}}}}})
			.at("notes");
	REQUIRE(changed.size() == 1);
	CHECK(changed[0].at("pitch") == 62);
	CHECK(changed[0].at("length") == 960);
	CHECK(changed[0].at("position") == 0);   // unchanged
	CHECK(changed[0].at("velocity") == 1.0); // unchanged
	mcp.call("note_remove", {{"part", partId}, {"notes", {idOf(notes[1])}}});
	Json stored = mcp.project().at("midiParts").at(0);
	REQUIRE(stored.at("notes").size() == 1);
	CHECK(stored.at("notes").at(0).at("pitch") == 62);

	// Controllers
	const Json controllers =
		mcp.call("controller_add",
			   {{"part", partId}, {"controllers", {{{"position", 0}, {"number", 7}, {"value", 1.0}}}}})
			.at("controllers");
	REQUIRE(controllers.size() == 1);
	const Json changedController =
		mcp.call("controller_set",
			   {{"part", partId}, {"controllers", {{{"id", idOf(controllers[0])}, {"number", 11}, {"value", 0.0}}}}})
			.at("controllers");
	CHECK(changedController.at(0).at("number") == 11);
	CHECK(changedController.at(0).at("value") == 0.0);
	CHECK(mcp.project().at("midiParts").at(0).at("controllers").at(0).at("number") == 11);
	mcp.call("controller_remove", {{"part", partId}, {"controllers", {idOf(controllers[0])}}});
	CHECK(mcp.project().at("midiParts").at(0).at("controllers").empty());

	mcp.call("midiPart_remove", {{"part", partId}});
	CHECK(mcp.project().at("midiParts").empty());
	CHECK_THAT(mcp.fails("note_set", {{"part", partId}, {"notes", Json::array()}}), ContainsSubstring("MIDI part"));
}

TEST_CASE("MCP clients add, change and remove audio events", "[integration]")
{
	const gnubase::test::TempDir dir;
	const std::filesystem::path file = dir.path() / "tone.wav";
	gnubase::test::writeWav(file, {std::vector<float>(48000, 0.25f), 1, 48000});
	Engine engine;
	McpClient mcp(engine);
	const int64_t track = idOf(mcp.call("track_add", {{"type", "audio"}, {"timeBase", "linear"}}).at("track"));
	const int64_t media = idOf(mcp.call("media_add", {{"path", file.u8string()}}).at("media"));
	const int64_t clip = idOf(mcp.call("clip_add", {{"media", media}}).at("clip"));

	const Json event = mcp.call("audioEvent_add",
							  {{"track", track}, {"clip", clip}, {"position", 0}, {"offset", 100}, {"length", 1000}})
						   .at("event");
	CHECK(event.at("offset") == 100);
	CHECK(event.at("length") == 1000);
	const Json changed =
		mcp.call("audioEvent_set", {{"event", idOf(event)}, {"position", 4800}, {"length", 2000}}).at("event");
	CHECK(changed.at("position") == 4800);
	CHECK(changed.at("length") == 2000);
	CHECK(changed.at("offset") == 100);
	CHECK(mcp.project().at("audioEvents").size() == 1);

	mcp.fails("clip_remove", {{"clip", clip}}); // an audio event still uses it
	mcp.call("audioEvent_remove", {{"event", idOf(event)}});
	mcp.call("clip_remove", {{"clip", clip}});
	mcp.call("media_remove", {{"media", media}});
	const Json project = mcp.project();
	CHECK(project.at("audioEvents").empty());
	CHECK(project.at("clips").empty());
	CHECK(project.at("media").empty());
}

TEST_CASE("MCP clients play, stop, locate and cycle the transport", "[integration]")
{
	Engine engine;
	McpClient mcp(engine);
	mcp.call("audio_setDevice", {{"blockSize", 128}});
	Json state = mcp.call("transport_set", {{"left", 0}, {"right", 4800}, {"cycle", true}});
	CHECK(state.at("cycle") == true);
	CHECK(state.at("right") == 4800);

	CHECK(mcp.call("transport_play").at("playing") == true);
	mcp.call("audio_render", {{"frames", 10000}});
	CHECK(mcp.call("transport_get").at("position") == 400); // 10000 frames in a cycle of 4800
	state = mcp.call("transport_stop");
	CHECK(state.at("playing") == false);
	CHECK(state.at("position") == 400);
	CHECK(mcp.call("transport_toggle").at("playing") == true);
	CHECK(mcp.call("transport_toggle").at("playing") == false);

	CHECK(mcp.call("transport_locate", {{"position", 1234}}).at("position") == 1234);
	CHECK(mcp.call("transport_locate", {{"target", "rightLocator"}}).at("position") == 4800);
	state = mcp.call("transport_exchangeLocators");
	CHECK(state.at("left") == 4800);
	CHECK(state.at("right") == 0);
	mcp.fails("transport_locate", {{"target", "nowhere"}});
}

TEST_CASE("MCP clients undo and redo, step by step and in transactions", "[integration]")
{
	Engine engine;
	McpClient mcp(engine);
	mcp.call("track_add", {{"type", "audio"}});
	mcp.call("track_add", {{"type", "midi"}});
	const Json history = mcp.call("history_get");
	CHECK(history.at("position") == 2);
	REQUIRE(history.at("steps").size() == 2);
	CHECK(history.at("steps").at(1).at("action") == "Add track");

	CHECK(mcp.call("history_undo").at("position") == 1);
	CHECK(trackNames(mcp.project()) == std::vector<std::string>{"Audio 01"});
	CHECK(mcp.call("history_redo").at("position") == 2);
	CHECK(trackNames(mcp.project()) == std::vector<std::string>{"Audio 01", "MIDI 01"});
	CHECK(mcp.call("history_undo", {{"steps", 2}}).at("position") == 0);
	CHECK(mcp.project().at("tracks").empty());

	// A transaction is one step; if a command in it fails, nothing changes.
	const Json add = {{"command", "track.add"}, {"parameters", {{"type", "audio"}}}};
	const Json results =
		mcp.call("history_transaction", {{"description", "Two tracks"}, {"commands", {add, add}}}).at("results");
	CHECK(results.size() == 2);
	CHECK(mcp.project().at("tracks").size() == 2);
	CHECK(mcp.call("history_get").at("steps").back().at("action") == "Two tracks");
	mcp.call("history_undo");
	CHECK(mcp.project().at("tracks").empty());
	const Json broken = {{"command", "track.add"}, {"parameters", {{"type", "nonsense"}}}};
	mcp.fails("history_transaction", {{"description", "Broken"}, {"commands", {add, broken}}});
	CHECK(mcp.project().at("tracks").empty());

	// Changed notes are undone too.
	const int64_t track = idOf(mcp.call("track_add", {{"type", "midi"}}).at("track"));
	const int64_t part =
		idOf(mcp.call("midiPart_add", {{"track", track}, {"position", 0}, {"length", 1920}}).at("part"));
	const Json note =
		mcp.call("note_add",
			   {{"part", part}, {"notes", {{{"position", 0}, {"length", 480}, {"pitch", 60}, {"velocity", 1.0}}}}})
			.at("notes")
			.at(0);
	mcp.call("note_set", {{"part", part}, {"notes", {{{"id", idOf(note)}, {"pitch", 67}}}}});
	CHECK(mcp.call("history_get").at("steps").back().at("action") == "Change notes");
	mcp.call("history_undo");
	CHECK(mcp.project().at("midiParts").at(0).at("notes").at(0).at("pitch") == 60);
}
