// SPDX-License-Identifier: GPL-3.0-or-later
// The UI shell (#48) on fake monitors: placing windows, their dp ratios, monitors that come and go, closing and
// drawing only when something changed. tests/window_tests.cpp runs real windows through SDL_GPU.
#include "testutils/fakeui.h"
#include "ui/displays.h"
#include "ui/rmlui.h"
#include "ui/shell.h"

#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/Input.h>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace gnubase;
using std::chrono::milliseconds;

namespace
{
	// A 1920x1080 primary monitor with a menu bar, and a 2560x1440 Retina monitor to its right.
	ui::Display primary()
	{
		return {1, "Primary", {0, 0, 1920, 1080}, {0, 25, 1920, 1055}, 1.0F};
	}

	ui::Display retina()
	{
		return {2, "Retina", {1920, 0, 2560, 1440}, {1920, 0, 2560, 1400}, 2.0F};
	}

	std::string document(const char* name)
	{
		return std::string(GNUBASE_TEST_DATA_DIR) + "/ui/" + name;
	}

	using Ui = test::FakeUi;

	// Loads the document from tests/data/ui into the window and shows it.
	Rml::ElementDocument& show(ui::Window& window, const char* name)
	{
		Rml::ElementDocument* loaded = window.context().LoadDocument(document(name));
		REQUIRE(loaded != nullptr);
		loaded->Show();
		window.requestRender();
		return *loaded;
	}

	ui::WindowSpec at(const ui::Rect& bounds)
	{
		ui::WindowSpec spec;
		spec.bounds = bounds;
		return spec;
	}

	// An event parameter of the type it was given, read without RmlUi's type conversion.
	template <typename T> T parameter(const Rml::Event& event, const char* name)
	{
		const Rml::Dictionary& parameters = event.GetParameters();
		const auto it = parameters.find(name);
		REQUIRE(it != parameters.end());
		return it->second.GetReference<T>();
	}

	// Remembers the last file drop and the element it was dropped on.
	struct DropListener final : Rml::EventListener
	{
		void ProcessEvent(Rml::Event& event) override
		{
			target = event.GetTargetElement()->GetId();
			files = parameter<Rml::String>(event, "files");
			count = parameter<int>(event, "count");
			position = {parameter<int>(event, "mouse_x"), parameter<int>(event, "mouse_y")};
		}

		std::string target;
		std::string files;
		int count = 0;
		ui::Point position;
	};

	Rml::ElementFormControlInput& field(Rml::ElementDocument& document, const char* id)
	{
		auto* input = dynamic_cast<Rml::ElementFormControlInput*>(document.GetElementById(id));
		REQUIRE(input != nullptr);
		return *input;
	}

	float boxWidth(ui::Window& window)
	{
		Rml::Element* box = window.context().GetDocument(0)->GetElementById("box");
		REQUIRE(box != nullptr);
		return box->GetOffsetWidth();
	}
}

TEST_CASE("a window is on the monitor it overlaps most", "[unit]")
{
	const std::vector<ui::Display> displays{primary(), retina()};
	CHECK(ui::displayOf({100, 100, 800, 600}, displays)->id == 1);
	CHECK(ui::displayOf({2000, 100, 800, 600}, displays)->id == 2);
	CHECK(ui::displayOf({1500, 100, 800, 600}, displays)->id == 1); // 420 on the left, 380 on the right
	CHECK(ui::displayOf({1700, 100, 800, 600}, displays)->id == 2); // 220 on the left, 580 on the right
	CHECK(ui::displayOf({5000, 100, 800, 600}, displays) == nullptr);
}

TEST_CASE("a window stays where it is while its title bar can be reached", "[unit]")
{
	const std::vector<ui::Display> displays{primary(), retina()};
	CHECK(ui::reachable({100, 100, 800, 600}, displays));
	CHECK(ui::reachable({1800, 100, 800, 600}, displays));       // across both monitors
	CHECK(ui::reachable({-700, 100, 800, 600}, displays));       // 100 wide on the primary
	CHECK_FALSE(ui::reachable({-760, 100, 800, 600}, displays)); // 40 wide: too little to grab
	CHECK_FALSE(ui::reachable({100, 0, 800, 600}, displays));    // its title bar is under the menu bar
	CHECK_FALSE(ui::reachable({100, -300, 800, 600}, displays)); // above the screen
	CHECK_FALSE(ui::reachable({5000, 100, 800, 600}, displays)); // on a monitor that's gone
	CHECK(ui::place({100, 100, 800, 600}, displays) == ui::Rect{100, 100, 800, 600});
}

TEST_CASE("an unreachable window is centred on the primary monitor, shrunk to fit", "[unit]")
{
	const std::vector<ui::Display> displays{primary(), retina()};
	CHECK(ui::place({5000, 100, 800, 600}, displays) == ui::Rect{560, 252, 800, 600});
	CHECK(ui::place({5000, 100, 3000, 2000}, displays) == ui::Rect{0, 25, 1920, 1055});
	CHECK(ui::place({5000, 100, 800, 600}, {}) == ui::Rect{5000, 100, 800, 600}); // no monitor: nowhere to go
}

TEST_CASE("a window's dp ratio is its monitor's scale times the user's UI scale", "[unit]")
{
	Ui ui({retina(), primary()});
	ui::Window& window = ui.shell.open(at({2000, 100, 800, 600}));
	CHECK(window.dpRatio() == 2.0F);
	CHECK(window.pixelSize() == ui::Size{1600, 1200});
	CHECK(window.context().GetDensityIndependentPixelRatio() == 2.0F);
	CHECK(window.context().GetDimensions() == Rml::Vector2i(1600, 1200));
	show(window, "boxes.rml");
	ui.shell.step(milliseconds(0));
	CHECK(boxWidth(window) == 200.0F); // 100 dp

	ui.shell.setUiScale(1.5F);
	CHECK(window.dpRatio() == 3.0F);
	CHECK(window.context().GetDensityIndependentPixelRatio() == 3.0F);
	CHECK(window.renderRequested());
	ui.shell.step(milliseconds(0));
	CHECK(boxWidth(window) == 300.0F);
}

TEST_CASE("windows on monitors with different scales lay out at their own scale", "[unit]")
{
	Ui ui({primary(), retina()});
	ui::Window& main = ui.shell.open(at({100, 100, 800, 600}));
	ui::Window& mixer = ui.shell.open(at({2000, 100, 800, 600}));
	show(main, "boxes.rml");
	show(mixer, "boxes.rml");
	ui.shell.step(milliseconds(0));
	CHECK(main.dpRatio() == 1.0F);
	CHECK(mixer.dpRatio() == 2.0F);
	CHECK(boxWidth(main) == 100.0F);
	CHECK(boxWidth(mixer) == 200.0F);

	SECTION("a window moved to the other monitor lays out again at that monitor's scale")
	{
		const int framesBefore = ui.renderer.presents(mixer.id());
		ui.windows.drag(mixer.id(), {300, 200, 800, 600});
		ui.shell.step(milliseconds(0));
		CHECK(mixer.dpRatio() == 1.0F);
		CHECK(mixer.pixelSize() == ui::Size{800, 600});
		CHECK(mixer.context().GetDimensions() == Rml::Vector2i(800, 600));
		CHECK(boxWidth(mixer) == 100.0F);
		CHECK(ui.renderer.presents(mixer.id()) == framesBefore + 1);
		CHECK(main.dpRatio() == 1.0F);
	}

	SECTION("a window moved within its monitor isn't drawn again")
	{
		const int framesBefore = ui.renderer.presents(mixer.id());
		ui.windows.drag(mixer.id(), {2400, 300, 800, 600});
		ui.shell.step(milliseconds(0));
		CHECK(mixer.dpRatio() == 2.0F);
		CHECK(ui.renderer.presents(mixer.id()) == framesBefore);
	}

	SECTION("a monitor that changes its scale re-lays out its windows")
	{
		ui::Display scaled = primary();
		scaled.scale = 1.25F;
		ui.windows.setDisplays({scaled, retina()});
		ui.shell.step(milliseconds(0));
		CHECK(main.dpRatio() == 1.25F);
		CHECK(boxWidth(main) == 125.0F);
		CHECK(mixer.dpRatio() == 2.0F);
	}
}

TEST_CASE("windows on a monitor that is disconnected move onto a connected one", "[unit]")
{
	Ui ui({primary(), retina()});
	const ui::Window& main = ui.shell.open(at({100, 100, 800, 600}));
	const ui::Window& mixer = ui.shell.open(at({2000, 100, 800, 600}));
	ui.shell.step(milliseconds(0));

	ui.windows.setDisplays({primary()});
	ui.shell.step(milliseconds(0));
	CHECK(ui.windows.bounds(mixer.id()) == ui::Rect{560, 252, 800, 600});
	CHECK(mixer.dpRatio() == 1.0F);
	CHECK(mixer.pixelSize() == ui::Size{800, 600});
	CHECK(ui.windows.bounds(main.id()) == ui::Rect{100, 100, 800, 600});

	// Connecting a monitor moves nothing.
	ui.windows.setDisplays({primary(), retina()});
	ui.shell.step(milliseconds(0));
	CHECK(ui.windows.bounds(mixer.id()) == ui::Rect{560, 252, 800, 600});
}

TEST_CASE("windows open on a connected monitor", "[unit]")
{
	Ui ui({primary()});

	SECTION("a window saved on a monitor that's gone opens on the primary one")
	{
		const ui::Window& window = ui.shell.open(at({3000, 200, 800, 600}));
		CHECK(ui.windows.spec(window.id()).bounds == ui::Rect{560, 252, 800, 600});
	}

	SECTION("a window without a place opens centred on the primary monitor")
	{
		ui::WindowSpec spec;
		spec.size = {1000, 500};
		const ui::Window& window = ui.shell.open(spec);
		CHECK(ui.windows.spec(window.id()).bounds == ui::Rect{460, 302, 1000, 500});
	}

	SECTION("a window without a size takes 80 % of the primary monitor")
	{
		ui::WindowSpec spec;
		spec.size = {};
		const ui::Window& window = ui.shell.open(spec);
		CHECK(ui.windows.spec(window.id()).bounds == ui::Rect{192, 130, 1536, 844});
	}
}

TEST_CASE("each window has its own RmlUi context, which gets the window's input", "[unit]")
{
	Ui ui({primary()});
	const ui::Window& main = ui.shell.open(at({100, 100, 800, 600}));
	const ui::Window& other = ui.shell.open(at({200, 200, 400, 300}));
	CHECK(main.main());
	CHECK_FALSE(other.main());
	CHECK(&main.context() != &other.context());
	CHECK(ui.windows.inputTarget(main.id()) == &main.context());
	CHECK(ui.windows.inputTarget(other.id()) == &other.context());
	CHECK(ui.renderer.attached(main.id()));
	CHECK(ui.renderer.attached(other.id()));
	CHECK(Rml::GetNumContexts() == 2);
}

TEST_CASE("closing the main window quits; closing another only closes it", "[unit]")
{
	Ui ui({primary()});
	const ui::WindowId main = ui.shell.open(at({100, 100, 800, 600})).id();
	const ui::WindowId other = ui.shell.open(at({200, 200, 400, 300})).id();

	ui.windows.push({ui::WindowEventKind::CloseRequested, other});
	ui.shell.step(milliseconds(0));
	CHECK(ui.shell.running());
	CHECK(ui.shell.find(other) == nullptr);
	CHECK_FALSE(ui.windows.exists(other));
	CHECK_FALSE(ui.renderer.attached(other));
	CHECK(Rml::GetNumContexts() == 1);

	ui.windows.push({ui::WindowEventKind::CloseRequested, main});
	ui.shell.step(milliseconds(0));
	CHECK_FALSE(ui.shell.running());
	CHECK(ui.windows.windowCount() == 0);
	CHECK(Rml::GetNumContexts() == 0);
	CHECK(ui.shell.run() == 0); // returns at once
}

TEST_CASE("a quit request closes every window", "[unit]")
{
	Ui ui({primary()});
	ui.shell.open(at({100, 100, 800, 600}));
	ui.shell.open(at({200, 200, 400, 300}));
	ui.windows.push({ui::WindowEventKind::Quit, 0});
	CHECK(ui.shell.run() == 0);
	CHECK_FALSE(ui.shell.running());
	CHECK(ui.windows.windowCount() == 0);
}

TEST_CASE("windows are drawn only when something changed", "[unit]")
{
	Ui ui({primary()});
	ui::Window& window = ui.shell.open(at({100, 100, 800, 600}));
	show(window, "boxes.rml");
	CHECK(ui.shell.idleTimeout() == milliseconds(0));
	ui.shell.step(milliseconds(0));
	CHECK(window.frames() == 1);
	CHECK(ui.renderer.presents(window.id()) == 1);

	// Nothing changed: the shell sleeps until the next event.
	CHECK_FALSE(ui.shell.idleTimeout().has_value());
	ui.shell.step(milliseconds(0));
	CHECK(window.frames() == 1);

	window.requestRender();
	ui.shell.step(milliseconds(0));
	CHECK(window.frames() == 2);

	ui.windows.push({ui::WindowEventKind::Input, window.id()});
	ui.shell.step(milliseconds(0));
	CHECK(window.frames() == 3);

	ui.windows.push({ui::WindowEventKind::Exposed, window.id()});
	ui.shell.step(milliseconds(0));
	CHECK(window.frames() == 4);
}

TEST_CASE("a window keeps drawing while its document animates", "[unit]")
{
	Ui ui({primary()});
	ui::Window& window = ui.shell.open(at({100, 100, 800, 600}));
	Rml::ElementDocument& pulse = show(window, "pulse.rml");
	ui.shell.step(milliseconds(0));
	const std::optional<milliseconds> timeout = ui.shell.idleTimeout();
	CHECK(timeout.value_or(milliseconds::max()) <= milliseconds(20)); // empty would wait for the next event

	pulse.Close();
	window.requestRender();
	ui.shell.step(milliseconds(0));
	ui.shell.step(milliseconds(0));
	CHECK_FALSE(ui.shell.idleTimeout().has_value());
}

TEST_CASE("step tasks run in every step, and once a frame while they have values to follow", "[unit]")
{
	Ui ui({primary()});
	const ui::Window& window = ui.shell.open(at({100, 100, 800, 600}));
	ui.shell.step(milliseconds(0));
	int runs = 0;
	bool following = false;
	ui.shell.eachStep(
		[&]
		{
			++runs;
			return following;
		});
	ui.shell.step(milliseconds(0));
	ui.shell.step(milliseconds(0));
	CHECK(runs == 2);
	CHECK_FALSE(ui.shell.idleTimeout().has_value()); // nothing to follow: sleep until the next event
	CHECK(window.frames() == 1);                     // and nothing to draw

	following = true;
	ui.shell.step(milliseconds(0));
	const std::optional<milliseconds> timeout = ui.shell.idleTimeout();
	REQUIRE(timeout.has_value());
	CHECK(timeout.value_or(milliseconds::max()) <= ui::g_framePeriod);
	CHECK(window.frames() == 1); // following values draws nothing by itself

	following = false;
	ui.shell.step(milliseconds(0));
	CHECK_FALSE(ui.shell.idleTimeout().has_value());
}

TEST_CASE("files dropped on a window go to the element under the mouse", "[unit]")
{
	DropListener listener; // outlives the documents it listens to
	Ui ui({retina()});
	ui::Window& window = ui.shell.open(at({2000, 100, 800, 600}));
	Rml::ElementDocument& document = show(window, "drop.rml");
	ui.shell.step(milliseconds(0));
	document.AddEventListener(ui::g_fileDropEvent, &listener);

	ui::WindowEvent drop{ui::WindowEventKind::FilesDropped, window.id()};
	drop.position = {300, 40}; // pixels: 150 by 20 dp at dp ratio 2, on "right"
	drop.files = {"/music/kick.wav", "/music/snare.flac"};
	ui.windows.push(drop);
	ui.shell.step(milliseconds(0));
	CHECK(listener.target == "right");
	CHECK(listener.files == "/music/kick.wav\n/music/snare.flac");
	CHECK(listener.count == 2);
	CHECK(listener.position == ui::Point{300, 40});
	CHECK(window.frames() == 2); // drawn again, since the drop may have changed it

	drop.position = {20, 20};
	drop.files = {"/music/bass.wav"};
	ui.windows.push(drop);
	ui.shell.step(milliseconds(0));
	CHECK(listener.target == "left");
	CHECK(listener.files == "/music/bass.wav");
	CHECK(listener.count == 1);
}

TEST_CASE("a text field turns its window's text input on while it has the focus", "[unit]")
{
	Ui ui({primary()});
	ui::Window& main = ui.shell.open(at({100, 100, 800, 600}));
	ui::Window& other = ui.shell.open(at({200, 200, 400, 300}));
	Rml::ElementDocument& document = show(main, "input.rml");
	show(other, "input.rml");
	ui.shell.step(milliseconds(0));
	CHECK_FALSE(ui.windows.textInputActive(main.id()));

	field(document, "name").Focus();
	CHECK(ui.windows.textInputActive(main.id()));
	CHECK_FALSE(ui.windows.textInputActive(other.id()));

	field(document, "other").Focus();
	CHECK(ui.windows.textInputActive(main.id()));

	field(document, "other").Blur();
	CHECK_FALSE(ui.windows.textInputActive(main.id()));
}

TEST_CASE("text fields copy to and paste from the system clipboard", "[unit]")
{
	Ui ui({primary()});
	ui::Window& window = ui.shell.open(at({100, 100, 800, 600}));
	Rml::ElementDocument& document = show(window, "input.rml");
	ui.shell.step(milliseconds(0));
	Rml::ElementFormControlInput& name = field(document, "name");
	name.Focus();
	Rml::Context& context = window.context();

	context.ProcessKeyDown(Rml::Input::KI_A, Rml::Input::KM_CTRL);
	context.ProcessKeyDown(Rml::Input::KI_C, Rml::Input::KM_CTRL);
	CHECK(ui.windows.clipboardText() == "hello");

	ui.windows.setClipboardText("world");
	context.ProcessKeyDown(Rml::Input::KI_A, Rml::Input::KM_CTRL);
	context.ProcessKeyDown(Rml::Input::KI_V, Rml::Input::KM_CTRL);
	CHECK(name.GetValue() == "world");
}

TEST_CASE("file dialogs answer on the UI thread with what the user chose", "[unit]")
{
	Ui ui({primary()});
	const ui::Window& window = ui.shell.open(at({100, 100, 800, 600}));
	std::vector<std::pair<std::string, ui::FileDialogResult>> results;
	const auto record = [&results](const char* name)
	{ return [&results, name](const ui::FileDialogResult& result) { results.emplace_back(name, result); }; };

	ui::FileDialog import;
	import.title = "Import audio";
	import.filters = {{"Audio files", "wav;aif;flac"}, {"All files", "*"}};
	import.many = true;
	import.parent = window.id();
	ui.shell.showFileDialog(import, record("import"));
	ui::FileDialog save;
	save.kind = ui::FileDialogKind::Save;
	save.location = "/projects/song.dawproject";
	ui.shell.showFileDialog(save, record("save"));
	ui::FileDialog folder;
	folder.kind = ui::FileDialogKind::Folder;
	ui.shell.showFileDialog(folder, record("folder"));

	// The window system shows them as asked; nothing returns until the user answers.
	REQUIRE(ui.windows.fileDialogs().size() == 3);
	auto shown = ui.windows.fileDialogs().begin();
	const uint64_t importId = shown->first;
	CHECK(shown->second.kind == ui::FileDialogKind::Open);
	CHECK(shown->second.title == "Import audio");
	CHECK(shown->second.filters.size() == 2);
	CHECK(shown->second.filters.at(0).extensions == "wav;aif;flac");
	CHECK(shown->second.many);
	CHECK(shown->second.parent == window.id());
	const uint64_t saveId = (++shown)->first;
	CHECK(shown->second.kind == ui::FileDialogKind::Save);
	CHECK(shown->second.location == std::filesystem::path("/projects/song.dawproject"));
	const uint64_t folderId = (++shown)->first;
	CHECK(shown->second.kind == ui::FileDialogKind::Folder);
	ui.shell.step(milliseconds(0));
	CHECK(results.empty());

	ui.windows.answerFileDialog(saveId, {});
	ui.windows.answerFileDialog(importId, {"/audio/kick.wav", "/audio/snare.flac"});
	ui.windows.answerFileDialog(folderId, {}, "no file dialog on this system");
	ui.shell.step(milliseconds(0));
	REQUIRE(results.size() == 3);
	CHECK(results[0].first == "save");
	CHECK(results[0].second.cancelled());
	CHECK(results[1].first == "import");
	CHECK(results[1].second.files == std::vector<std::filesystem::path>{"/audio/kick.wav", "/audio/snare.flac"});
	CHECK_FALSE(results[1].second.cancelled());
	CHECK(results[2].first == "folder");
	CHECK(results[2].second.error == "no file dialog on this system");
	CHECK_FALSE(results[2].second.cancelled());
}

TEST_CASE("work posted from any thread runs on the UI thread in the shell's next step, which it wakes", "[unit]")
{
	Ui ui{{primary()}};
	ui.shell.open(at({100, 100, 800, 600}));
	const std::thread::id uiThread = std::this_thread::get_id();
	std::thread::id ranOn;
	int runs = 0;
	std::thread engine(
		[&]
		{
			ui.shell.post(
				[&]
				{
					ranOn = std::this_thread::get_id();
					++runs;
				});
		});
	engine.join();
	CHECK(runs == 0); // not before the shell steps
	CHECK(ui.windows.wakes() == 1);

	ui.shell.step(milliseconds(0));
	CHECK(runs == 1);
	CHECK(ranOn == uiThread);
	ui.shell.step(milliseconds(0));
	CHECK(runs == 1); // once
}

TEST_CASE("the theme applies in every window, also in windows opened later, without loading documents again", "[unit]")
{
	Ui ui{{primary()}};
	const char* rml = R"(<rml><head><style>
		body { color: #000000; }
		@media (theme: day) { body { color: #ff0000; } }
		@media (theme: night) { body { color: #0000ff; } }
	</style></head><body/></rml>)";
	const auto load = [&](ui::Window& window)
	{
		Rml::ElementDocument* document = window.context().LoadDocumentFromMemory(rml);
		REQUIRE(document != nullptr);
		document->Show();
		return document;
	};
	ui::Window& first = ui.shell.open(at({100, 100, 800, 600}));
	Rml::ElementDocument* document = load(first);
	ui.shell.step(milliseconds(0));
	CHECK(ui.shell.theme().empty());
	CHECK(document->GetComputedValues().color() == Rml::Colourb(0, 0, 0));

	ui.shell.setTheme("day");
	CHECK(ui.shell.theme() == "day");
	CHECK(first.renderRequested());
	ui.shell.step(milliseconds(0));
	CHECK(document->GetComputedValues().color() == Rml::Colourb(255, 0, 0));
	ui.shell.setTheme("night");
	ui.shell.step(milliseconds(0));
	CHECK(first.context().GetDocument(0) == document); // the same document
	CHECK(document->GetComputedValues().color() == Rml::Colourb(0, 0, 255));

	ui::Window& second = ui.shell.open(at({200, 200, 800, 600}));
	const Rml::ElementDocument* other = load(second);
	ui.shell.step(milliseconds(0));
	CHECK(other->GetComputedValues().color() == Rml::Colourb(0, 0, 255));
}
