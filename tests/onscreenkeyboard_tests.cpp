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
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_scancode.h>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
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
	const ui::OnScreenKeyboard& keyboard = *app.appUi().keyboard();
	CHECK(keyboard.note(0) == 60); // the lowest key is C3 at the usual octave
	CHECK(keyElement(app, 28).IsClassSet("white-key"));
	CHECK(keyElement(app, 27).IsClassSet("black-key"));

	// A few commands keep their keys: Num/ turns cycle on.
	app.ui().key(std::nullopt, "Num/");
	REQUIRE(app.waitUntil([&app] { return app.call("transport.get").at("cycle").get<bool>(); }));
	app.call("transport.set", {{"cycle", false}});
	// The commands keep their keys, not the keys their commands (#691): with Cycle on Alt+Y, Num/ does nothing, and
	// Alt+Y turns cycle on.
	app.keys().customize(
		Json::array({{{"command", "transport.toggle"}, {"parameters", {{"setting", "cycle"}}}, {"keys", {"Alt+Y"}}}}),
		Json::array());
	app.ui().key(std::nullopt, "Num/");
	render(app);
	CHECK(!app.call("transport.get").at("cycle").get<bool>());
	app.ui().key(std::nullopt, "Alt+Y");
	REQUIRE(app.waitUntil([&app] { return app.call("transport.get").at("cycle").get<bool>(); }));
	app.call("transport.set", {{"cycle", false}});
	app.keys().customize(Json::array(), Json::array());

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

	// The mouse button going up out of the window, where no mouseup arrives, ends the note too: E's (key 16).
	Rml::Element& e = keyElement(app, 16);
	context.ProcessMouseMove(static_cast<int>(e.GetAbsoluteLeft() + (e.GetOffsetWidth() / 2.0F)),
		static_cast<int>(e.GetAbsoluteTop() + (e.GetOffsetHeight() * 0.8F)), 0);
	context.ProcessMouseButtonDown(0, 0);
	CHECK(e.IsClassSet("pressed"));
	render(app);
	context.ProcessMouseMove(-50, -50, 0);
	context.ProcessMouseLeave();
	context.ProcessMouseButtonUp(0, 0);
	CHECK(!e.IsClassSet("pressed"));
	render(app);
	render(app);
	render(app);
	app.call("transport.stop");
	render(app);
	REQUIRE(app.waitUntil(
		[&app, track] { return recordedNotes(app.call("project.get"), track).size() == 3; }, std::chrono::seconds(10)));
	const Json project = app.call("project.get");
	CHECK(recordedNotes(project, track) == std::vector<std::vector<int>>{{60, 100}, {86, 64}, {88, 64}});
	// It lasts as long as the button was held, as W's did, not until the recording stopped.
	std::vector<int64_t> lengths;
	for (const Json& part : project.at("midiParts"))
		if (part.at("track") == track)
			for (const Json& note : part.at("notes"))
				lengths.push_back(note.at("length").get<int64_t>());
	REQUIRE(lengths.size() == 3);
	CHECK(lengths[2] < lengths[1] * 2);

	// Alt+K keeps its key command and closes it again; the octave and velocity stay for the next time.
	app.ui().key(std::nullopt, "Alt+K");
	REQUIRE(app.waitUntil([&app] { return app.appUi().keyboard() == nullptr; }));
	app.ui().key(std::nullopt, "Alt+K");
	REQUIRE(app.waitUntil([&app] { return app.appUi().keyboard() != nullptr; }));
	CHECK(app.appUi().keyboard()->note(0) == 72);

	// The sliders dragged with the mouse (#691): the thumb follows it, and a value between two steps rounds to one.
	const ui::WindowId window = app.appUi().keyboardWindow();
	const auto dragTo = [&app, window](const std::string& slider, float share)
	{
		const Json box = app.ui().dom(window, slider, 0).at("elements").at(0).at("box");
		const ui::Point to{box.at("x").get<int>() + static_cast<int>(box.at("width").get<float>() * share),
			box.at("y").get<int>() + (box.at("height").get<int>() / 2)};
		app.ui().drag(window, {slider, 0, std::nullopt}, {"", 0, to});
		app.shell().step(std::chrono::milliseconds(0));
		return app.ui().dom(window, slider + "-value", 0).at("elements").at(0).value("text", std::string());
	};
	CHECK(dragTo("#octave", 1.0F) == "6");
	CHECK(app.appUi().keyboard()->note(0) == 96);
	const std::string octave = dragTo("#octave", 0.7F);
	REQUIRE(octave.size() == 1);
	CHECK(app.appUi().keyboard()->note(0) == 12 * (std::stoi(octave) + 2));
	CHECK(dragTo("#velocity", 0.0F) == "1");
}

TEST_CASE("the on-screen keyboard plays computer keys by their places, labels them as the layout has them and keeps "
		  "its octave",
	"[ui]")
{
	using namespace Rml::Input;
	test::UiApp app({900, 640});
	app.appUi().showProject();
	app.ui().key(std::nullopt, "Alt+K");
	REQUIRE(app.waitUntil([&app] { return app.appUi().keyboard() != nullptr; }));

	// Each key shows what the layout has at its computer key's place (reference manual p. 333): the lowest, Z's.
	const std::string label = app.shell().keyLabel(KI_Z);
	Rml::Element* lowest = keyElement(app, 0).QuerySelector(".key-label");
	REQUIRE(lowest != nullptr);
	CHECK(lowest->GetInnerRML() == (label.empty() ? "Z" : label));
	// After a keymap change the keys show what the window system then reports: here a stale label goes.
	lowest->SetInnerRML("?");
	SDL_Event keymap{};
	keymap.type = SDL_EVENT_KEYMAP_CHANGED;
	REQUIRE(SDL_PushEvent(&keymap));
	REQUIRE(app.waitUntil(
		[&app, lowest]
		{
			const std::string now = app.shell().keyLabel(KI_Z);
			return lowest->GetInnerRML() == (now.empty() ? "Z" : now);
		}));

	// A German layout's Y, at a US keyboard's Z, plays the lowest key, not the upper row's Y (key 21), and its release
	// ends it (#691).
	app.shell().unusedKey(app.main(), ui::KeyPress{KI_Y, 0, KI_Z});
	CHECK(keyElement(app, 0).IsClassSet("pressed"));
	CHECK(!keyElement(app, 21).IsClassSet("pressed"));
	app.shell().keyReleased(app.main(), ui::KeyPress{KI_Y, 0, KI_Z});
	CHECK(!keyElement(app, 0).IsClassSet("pressed"));
	// A key that the layout's key codes don't name, such as a French layout's 2 (é), plays by its place too.
	app.shell().unusedKey(app.main(), ui::KeyPress{KI_UNKNOWN, 0, KI_2});
	CHECK(keyElement(app, 13).IsClassSet("pressed"));
	app.shell().keyReleased(app.main(), ui::KeyPress{KI_UNKNOWN, 0, KI_2});
	CHECK(!keyElement(app, 13).IsClassSet("pressed"));
	// Keys outside the two rows play nothing, whatever the layout has there: a French layout's M at a US keyboard's
	// semicolon is not the US M (key 11). The Z pressed after it shows that the window has handled both.
	const auto sdlKey = [&app](SDL_EventType type, SDL_Keycode key, SDL_Scancode place)
	{
		SDL_Event event{};
		event.type = type;
		event.key.windowID = app.main().id();
		event.key.key = key;
		event.key.scancode = place;
		event.key.down = type == SDL_EVENT_KEY_DOWN;
		REQUIRE(SDL_PushEvent(&event));
	};
	sdlKey(SDL_EVENT_KEY_DOWN, SDLK_M, SDL_SCANCODE_SEMICOLON);
	sdlKey(SDL_EVENT_KEY_DOWN, SDLK_Z, SDL_SCANCODE_Z);
	REQUIRE(app.waitUntil([&app] { return keyElement(app, 0).IsClassSet("pressed"); }));
	CHECK(!keyElement(app, 11).IsClassSet("pressed"));
	sdlKey(SDL_EVENT_KEY_UP, SDLK_M, SDL_SCANCODE_SEMICOLON);
	sdlKey(SDL_EVENT_KEY_UP, SDLK_Z, SDL_SCANCODE_Z);
	REQUIRE(app.waitUntil([&app] { return !keyElement(app, 0).IsClassSet("pressed"); }));
	// M without a place, as from ui::Automation, counts as typed on a US keyboard.
	app.shell().unusedKey(app.main(), ui::KeyPress{KI_M, 0});
	CHECK(keyElement(app, 11).IsClassSet("pressed"));
	app.shell().keyReleased(app.main(), ui::KeyPress{KI_M, 0});
	CHECK(!keyElement(app, 11).IsClassSet("pressed"));
	// A key that RmlUi can't map and has no place goes up: no note ends. The window losing the keyboard focus ends
	// every one (#569).
	app.shell().unusedKey(app.main(), ui::KeyPress{KI_M, 0});
	app.shell().keyReleased(app.main(), ui::KeyPress{KI_UNKNOWN, 0});
	CHECK(keyElement(app, 11).IsClassSet("pressed"));
	ui::KeyPress lost;
	lost.focusLost = true;
	app.shell().keyReleased(app.main(), lost);
	CHECK(!keyElement(app, 11).IsClassSet("pressed"));

	// The octave and velocity are user settings, which last across restarts.
	Rml::ElementDocument& document = *keyElement(app, 0).GetOwnerDocument();
	REQUIRE(document.GetElementById("octave") != nullptr);
	document.GetElementById("octave")->DispatchEvent(Rml::EventId::Change, {{"value", Rml::Variant(5.0F)}});
	REQUIRE(document.GetElementById("velocity") != nullptr);
	document.GetElementById("velocity")->DispatchEvent(Rml::EventId::Change, {{"value", Rml::Variant(80.0F)}});
	// Saved once the change ends, as a slider's drag does when the mouse button goes up, not for each step.
	const auto writes = [&app]
	{
		const Json log = app.call("engine.commandLog").at("commands");
		return std::count_if(log.begin(), log.end(),
			[](const Json& entry)
			{
				return entry.at("command") == "settings.set" &&
					entry.at("parameters").value("key", std::string()) == "ui.onScreenKeyboard";
			});
	};
	render(app);
	CHECK(writes() == 0);
	document.DispatchEvent(Rml::EventId::Mouseup, {{"button", Rml::Variant(0)}});
	const Json saved = {{"octave", 5}, {"velocity", 80}};
	REQUIRE(app.waitUntil(
		[&app, &saved] { return app.call("settings.get", {{"key", "ui.onScreenKeyboard"}}).at("value") == saved; }));
	CHECK(writes() == 1);
	app.restart();
	app.appUi().showProject();
	app.ui().key(std::nullopt, "Alt+K");
	REQUIRE(app.waitUntil([&app] { return app.appUi().keyboard() != nullptr; }));
	CHECK(app.appUi().keyboard()->note(0) == 84);
	const Json velocity = app.ui().dom(app.appUi().keyboardWindow(), "#velocity-value", 0).at("elements");
	REQUIRE(velocity.size() == 1);
	CHECK(velocity[0].value("text", std::string()) == "80");
}
