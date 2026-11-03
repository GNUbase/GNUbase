// SPDX-License-Identifier: GPL-3.0-or-later
// The on-screen keyboard (#103, reference manual pp. 332-334, docs/features/onscreen-keyboard.md): midi.keyboard plays
// into all MIDI inputs, which tracks that monitor or are record-enabled play and record, and the on-screen keyboard
// window plays it with the computer keyboard and the mouse.
#include "engine/document/document.h"
#include "engine/engine.h"
#include "platform/audio/device.h"
#include "ui/automation.h"
#include "ui/onscreenkeyboard.h"
#include "uiapp.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace gnubase;
using Catch::Matchers::ContainsSubstring;
using engine::Json;

namespace
{
	Json run(engine::Client& client, const std::string& command, Json parameters = Json::object())
	{
		const engine::Result result = client.call(command, std::move(parameters));
		INFO(command << " failed: " << result.error);
		REQUIRE(result.ok());
		return result.value;
	}

	std::string reject(engine::Client& client, const std::string& command, Json parameters)
	{
		const engine::Result result = client.call(command, std::move(parameters));
		INFO(command << " succeeded: " << result.value.dump());
		REQUIRE(!result.ok());
		return result.error;
	}

	Json keyboard(std::vector<int> bytes)
	{
		return {{"messages", Json::array({{{"bytes", std::move(bytes)}}})}};
	}

	// The note-ons and note-offs the instruments played, as [status, pitch, velocity].
	std::vector<std::vector<int>> notesPlayed(const Json& capture)
	{
		std::vector<std::vector<int>> notes;
		for (const Json& captured : capture.at("captured"))
		{
			const Json& bytes = captured.at("bytes");
			const int status = bytes.at(0).get<int>() & 0xF0;
			if (status == 0x90 || status == 0x80)
				notes.push_back({status, bytes.at(1).get<int>(), bytes.at(2).get<int>()});
		}
		return notes;
	}

	// Waits for the event, null after 10 s.
	Json waitFor(engine::Client& client, const std::string& name)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (std::chrono::steady_clock::now() < deadline)
		{
			const std::optional<engine::Message> next = client.wait(std::chrono::milliseconds(50));
			const auto* event = next ? std::get_if<engine::Event>(&*next) : nullptr;
			if (event != nullptr && event->name == name)
				return event->data;
		}
		return nullptr;
	}

	// The notes of the track's MIDI parts, as [pitch, velocity in MIDI 1.0].
	std::vector<std::vector<int>> recordedNotes(const Json& project, uint64_t track)
	{
		std::vector<std::vector<int>> notes;
		for (const Json& part : project.at("midiParts"))
			if (part.at("track") == track)
				for (const Json& note : part.at("notes"))
					notes.push_back({note.at("pitch").get<int>(),
						static_cast<int>(std::lround(note.at("velocity").get<double>() * 127.0))});
		return notes;
	}
}

TEST_CASE("midi.keyboard plays into all MIDI inputs: tracks that monitor or are record-enabled play and record it",
	"[integration]")
{
	engine::Engine engine(audio::makeBackends(), {}, {});
	engine::Client client(engine);
	run(client, "audio.setDevice", {{"blockSize", 64}, {"inputChannels", 0}});
	const Json added = run(client, "track.add", {{"type", "instrument"}});
	const auto track = added.at("track").at("id").get<uint64_t>();
	const auto channel = added.at("channel").at("id").get<uint64_t>();
	run(client, "channel.set", {{"channel", channel}, {"instrument", "noteProbe"}});
	const auto played = [&](const Json& changes)
	{
		Json parameters = changes;
		parameters["channel"] = channel;
		run(client, "channel.set", parameters);
		run(client, "audio.render", {{"frames", 64}}); // the new graph
		run(client, "midi.capture", {{"start", true}});
		run(client, "midi.keyboard", keyboard({0x90, 60, 100}));
		run(client, "audio.render", {{"frames", 64}});
		run(client, "midi.keyboard", keyboard({0x80, 60, 64}));
		run(client, "audio.render", {{"frames", 64}});
		return notesPlayed(run(client, "midi.capture"));
	};
	CHECK(played({}).empty()); // neither monitoring nor record-enabled
	CHECK(played({{"monitor", true}}) == std::vector<std::vector<int>>{{0x90, 60, 100}, {0x80, 60, 64}});
	CHECK(played({{"midiInput", "none"}}).empty());
	CHECK(played({{"midiInput", "all"}, {"monitor", false}, {"recordEnable", true}}).size() == 2);

	// What the record-enabled track plays, it records.
	run(client, "transport.record");
	run(client, "midi.keyboard", keyboard({0x90, 64, 90}));
	run(client, "audio.render", {{"frames", 640}});
	run(client, "midi.keyboard", keyboard({0x80, 64, 0}));
	run(client, "audio.render", {{"frames", 640}});
	run(client, "transport.stop");
	run(client, "audio.render", {{"frames", 640}});
	REQUIRE(waitFor(client, "recording.added").is_object());
	CHECK(recordedNotes(run(client, "project.get"), track) == std::vector<std::vector<int>>{{64, 90}});

	// Only MIDI 1.0 channel messages.
	CHECK_THAT(reject(client, "midi.keyboard", keyboard({0xF8})), ContainsSubstring("not a MIDI 1.0 channel message"));
	CHECK_THAT(reject(client, "midi.keyboard", keyboard({0x90, 60})), ContainsSubstring("has 3 bytes"));
	CHECK_THAT(reject(client, "midi.keyboard", keyboard({0x90, 60, 200})), ContainsSubstring("below 0x80"));
}

namespace
{
	// Renders a tenth of a second on the dummy device, which UI tests drive offline, after the commands the UI sent.
	void render(test::UiApp& app)
	{
		app.call("audio.render", {{"frames", 4800}});
	}

	Rml::Element& keyElement(test::UiApp& app, int key)
	{
		ui::Window* window = app.shell().find(app.appUi().keyboardWindow());
		REQUIRE(window != nullptr);
		Rml::ElementDocument* document = window->context().GetDocument(0);
		REQUIRE(document != nullptr);
		Rml::Element* element = document->QuerySelector("[note=\"" + std::to_string(key) + "\"]");
		REQUIRE(element != nullptr);
		return *element;
	}
}

TEST_CASE("notes played on the on-screen keyboard with the computer keyboard and the mouse are recorded", "[ui]")
{
	test::UiApp app({900, 640});
	app.appUi().showProject();
	const Json added = app.call("track.add", {{"type", "instrument"}});
	const auto track = added.at("track").at("id").get<uint64_t>();
	app.call("channel.set",
		{{"channel", added.at("channel").at("id")}, {"instrument", "noteProbe"}, {"recordEnable", true}});

	// Alt+K opens it in a window of its own, with its 29 keys.
	app.ui().key(std::nullopt, "Alt+K");
	REQUIRE(app.waitUntil([&app] { return app.appUi().keyboard() != nullptr; }));
	ui::OnScreenKeyboard& keyboard = *app.appUi().keyboard();
	CHECK(keyboard.note(0) == 60); // the lowest key is C3 at the usual octave
	CHECK(keyElement(app, 28).IsClassSet("white-key"));
	CHECK(keyElement(app, 27).IsClassSet("black-key"));

	// A few keys keep their key commands: Num/ turns cycle on.
	app.ui().key(std::nullopt, "Num/");
	REQUIRE(app.waitUntil([&app] { return app.call("transport.get").at("cycle").get<bool>(); }));
	app.call("transport.set", {{"cycle", false}});

	app.call("transport.record");
	// Z plays the lowest key while it is held, lit. A key that would run another key command plays nothing and runs
	// nothing: Num0 would stop.
	app.ui().key(std::nullopt, "Z", ui::KeyAction::Down);
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(keyElement(app, 0).IsClassSet("pressed"));
	app.ui().key(std::nullopt, "Num0");
	render(app);
	app.ui().key(std::nullopt, "Z", ui::KeyAction::Up);
	app.shell().step(std::chrono::milliseconds(0));
	CHECK(!keyElement(app, 0).IsClassSet("pressed"));
	render(app);
	CHECK(app.call("transport.get").at("recording") == true);

	// The mouse plays the key it holds: W's (key 14) an octave up after the octave went up, at the velocity.
	Rml::Element& w = keyElement(app, 14);
	Rml::ElementDocument& document = *w.GetOwnerDocument();
	REQUIRE(document.GetElementById("octave") != nullptr);
	document.GetElementById("octave")->DispatchEvent(Rml::EventId::Change, {{"value", Rml::Variant(4.0F)}});
	REQUIRE(document.GetElementById("velocity") != nullptr);
	document.GetElementById("velocity")->DispatchEvent(Rml::EventId::Change, {{"value", Rml::Variant(64.0F)}});
	CHECK(keyboard.note(14) == 86);
	CHECK(document.GetElementById("octave-value")->GetInnerRML() == "4");
	Rml::Context& context = *document.GetContext();
	context.Update();
	context.ProcessMouseMove(static_cast<int>(w.GetAbsoluteLeft() + (w.GetOffsetWidth() / 2.0F)),
		static_cast<int>(w.GetAbsoluteTop() + (w.GetOffsetHeight() * 0.8F)), 0); // below the black keys
	context.ProcessMouseButtonDown(0, 0);
	CHECK(w.IsClassSet("pressed"));
	render(app);
	context.ProcessMouseButtonUp(0, 0);
	CHECK(!w.IsClassSet("pressed"));
	render(app);
	app.call("transport.stop");
	render(app);
	REQUIRE(app.waitUntil(
		[&app, track] { return recordedNotes(app.call("project.get"), track).size() == 2; }, std::chrono::seconds(10)));
	CHECK(recordedNotes(app.call("project.get"), track) == std::vector<std::vector<int>>{{60, 100}, {86, 64}});

	// Alt+K keeps its key command and closes it again; the octave and velocity stay for the next time.
	app.ui().key(std::nullopt, "Alt+K");
	REQUIRE(app.waitUntil([&app] { return app.appUi().keyboard() == nullptr; }));
	app.ui().key(std::nullopt, "Alt+K");
	REQUIRE(app.waitUntil([&app] { return app.appUi().keyboard() != nullptr; }));
	CHECK(app.appUi().keyboard()->note(0) == 72);
}
