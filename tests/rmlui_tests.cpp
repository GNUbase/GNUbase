// SPDX-License-Identifier: GPL-3.0-or-later
// RmlUi in GNUbase (#49): text in fonts loaded through FreeType, files with any name, custom elements, data models,
// live reload and the shell's periodic tasks, on the fake window system. tests/window_tests.cpp draws a sample
// document through SDL_GPU and compares it with a golden image.
#include "testutils/fakeui.h"
#include "testutils/testutils.h"
#include "ui/datamodel.h"
#include "ui/livereload.h"
#include "ui/rmlui.h"
#include "ui/shell.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace gnubase;
using std::chrono::milliseconds;

namespace
{
	std::filesystem::path font(const char* name)
	{
		return std::filesystem::path(GNUBASE_TEST_FONTS_DIR) / name;
	}

	// The fake UI with one window.
	struct OneWindow
	{
		static ui::WindowSpec spec()
		{
			ui::WindowSpec spec;
			spec.bounds = ui::Rect{100, 100, 800, 600};
			return spec;
		}

		test::FakeUi ui{{{1, "Monitor", {0, 0, 1920, 1080}, {0, 0, 1920, 1080}, 1.0F}}};
		ui::Window& window = ui.shell.open(spec());
	};

	Rml::ElementDocument& show(ui::Window& window, const std::string& rml)
	{
		Rml::ElementDocument* document = window.context().LoadDocumentFromMemory(rml);
		REQUIRE(document != nullptr);
		document->Show();
		return *document;
	}

	// The element with the id in any of the window's documents.
	Rml::Element& find(const ui::Window& window, const char* id)
	{
		Rml::Element* found = nullptr;
		for (int i = 0; i < window.context().GetNumDocuments() && found == nullptr; ++i)
			found = window.context().GetDocument(i)->GetElementById(id);
		REQUIRE(found != nullptr);
		return *found;
	}

	void write(const std::filesystem::path& file, const std::string& text)
	{
		std::ofstream out(file, std::ios::binary);
		out << text;
		out.close();
		REQUIRE(out.good());
	}

	// Rewrites the file and moves its modification time on, so that the change shows even on file systems that keep
	// times in whole seconds.
	void change(const std::filesystem::path& file, const std::string& text)
	{
		const std::filesystem::file_time_type before = std::filesystem::last_write_time(file);
		write(file, text);
		std::filesystem::last_write_time(file, before + std::chrono::seconds(2));
	}

	// An element that GNUbase code changes: it asks its window to draw through RmlUi.
	class Lamp final : public Rml::Element
	{
	public:
		explicit Lamp(const Rml::String& tag) : Rml::Element(tag) {}

		void light()
		{
			SetClass("lit", true);
			if (Rml::Context* context = GetContext())
				context->RequestNextUpdate(0);
		}
	};
}

TEST_CASE("text is laid out in fonts loaded through FreeType", "[unit]")
{
	OneWindow fixture;
	REQUIRE(ui::RmlUi::loadFont(font("LatoLatin-Regular.ttf")));
	CHECK_FALSE(ui::RmlUi::loadFont(font("missing.ttf")));
	show(fixture.window, R"(<rml><head><style>
			body { display: block; font-family: LatoLatin; font-size: 20dp; }
			span { display: inline-block; }
		</style></head>
		<body><span id="short">Hi</span><span id="long">Hello, world</span></body></rml>)");
	fixture.ui.shell.step(milliseconds(0));
	const float shortWidth = find(fixture.window, "short").GetClientWidth();
	const float longWidth = find(fixture.window, "long").GetClientWidth();
	CHECK(shortWidth > 10.0F);
	CHECK(longWidth > 4.0F * shortWidth);
	CHECK(find(fixture.window, "long").GetClientHeight() >= 20.0F);

	// At twice the scale FreeType renders the glyphs at twice the size, so the text is about twice as wide.
	fixture.ui.shell.setUiScale(2.0F);
	fixture.ui.shell.step(milliseconds(0));
	CHECK(find(fixture.window, "long").GetClientWidth() == Catch::Approx(2.0F * longWidth).margin(4.0F));
}

TEST_CASE("documents, style sheets and fonts load from paths with any characters", "[unit]")
{
	OneWindow fixture;
	const test::TempDir temp;
	const std::filesystem::path folder = temp.path() / std::filesystem::u8path("Größe 音 Ordner");
	std::filesystem::create_directories(folder);
	write(folder / std::filesystem::u8path("Ölfarbe.rcss"), "#box { display: block; width: 123dp; height: 10dp; }");
	write(folder / std::filesystem::u8path("Übersicht.rml"),
		R"(<rml><head><link type="text/rcss" href="Ölfarbe.rcss"/></head><body><div id="box"/></body></rml>)");
	std::filesystem::copy_file(font("LatoLatin-Regular.ttf"), folder / std::filesystem::u8path("Schrift ß.ttf"));

	CHECK(ui::RmlUi::loadFont(folder / std::filesystem::u8path("Schrift ß.ttf")));
	Rml::ElementDocument* document =
		fixture.window.context().LoadDocument((folder / std::filesystem::u8path("Übersicht.rml")).generic_u8string());
	REQUIRE(document != nullptr);
	document->Show();
	fixture.ui.shell.step(milliseconds(0));
	CHECK(find(fixture.window, "box").GetClientWidth() == 123.0F);
	CHECK(fixture.ui.rmlui.files().hasRead(folder / std::filesystem::u8path("Ölfarbe.rcss")));
}

TEST_CASE("the file interface reports each change of a file RmlUi read once", "[unit]")
{
	OneWindow fixture;
	const test::TempDir temp;
	const std::filesystem::path file = temp.path() / "panel.rml";
	write(file, "<rml><body/></rml>");
	REQUIRE(fixture.window.context().LoadDocument(file.generic_u8string()) != nullptr);
	ui::FileInterface& files = fixture.ui.rmlui.files();
	CHECK(files.changedFiles().empty());

	change(file, "<rml><body><p/></body></rml>");
	const std::vector<std::filesystem::path> changed = files.changedFiles();
	REQUIRE(changed.size() == 1);
	CHECK(std::filesystem::equivalent(changed.front(), file));
	CHECK(files.changedFiles().empty());

	// While an editor saves a file it may be missing for a moment; that's not a change yet.
	std::filesystem::remove(file);
	CHECK(files.changedFiles().empty());
}

TEST_CASE("documents get registered custom elements, which can ask their window to draw", "[unit]")
{
	OneWindow fixture;
	fixture.ui.rmlui.registerElement<Lamp>("test-lamp");
	show(fixture.window, R"(<rml><body><test-lamp id="lamp"/></body></rml>)");
	auto* lamp = dynamic_cast<Lamp*>(&find(fixture.window, "lamp"));
	REQUIRE(lamp != nullptr);
	fixture.ui.shell.step(milliseconds(0));
	CHECK(fixture.window.frames() == 1);
	CHECK_FALSE(fixture.ui.shell.idleTimeout().has_value());

	lamp->light();
	CHECK(fixture.ui.shell.idleTimeout() == milliseconds(0));
	fixture.ui.shell.step(milliseconds(0));
	CHECK(fixture.window.frames() == 2);
	CHECK_FALSE(fixture.ui.shell.idleTimeout().has_value());
}

TEST_CASE("a data model shows its variables and draws the window again when they change", "[unit]")
{
	OneWindow fixture;
	int tracks = 3;
	{
		ui::DataModel model(fixture.window.context(), "project");
		REQUIRE(model.constructor().Bind("tracks", &tracks));
		show(fixture.window, R"(<rml><body data-model="project"><p id="count">{{tracks}} tracks</p></body></rml>)");
		fixture.ui.shell.step(milliseconds(0));
		CHECK(find(fixture.window, "count").GetInnerRML() == "3 tracks");
		CHECK_FALSE(fixture.ui.shell.idleTimeout().has_value());

		tracks = 4;
		model.changed("tracks");
		CHECK(fixture.ui.shell.idleTimeout() == milliseconds(0));
		fixture.ui.shell.step(milliseconds(0));
		CHECK(fixture.window.frames() == 2);
		CHECK(find(fixture.window, "count").GetInnerRML() == "4 tracks");

		CHECK_THROWS_AS(ui::DataModel(fixture.window.context(), "project"), std::runtime_error);
	}
	// The model was removed with the object, so the name is free again.
	const ui::DataModel again(fixture.window.context(), "project");
	CHECK(again.name() == "project");
}

TEST_CASE("a data model whose window closed first does nothing", "[unit]")
{
	OneWindow fixture;
	int tracks = 3;
	const ui::Window& other = fixture.ui.shell.open(OneWindow::spec());
	ui::DataModel model(other.context(), "project");
	REQUIRE(model.constructor().Bind("tracks", &tracks));
	fixture.ui.shell.close(other.id());
	model.changed("tracks");
	model.changedAll();
}

TEST_CASE("live reload loads documents again when their RML or RCSS files change", "[unit]")
{
	OneWindow fixture;
	const test::TempDir temp;
	const std::filesystem::path rml = temp.path() / "panel.rml";
	const std::filesystem::path rcss = temp.path() / "panel.rcss";
	write(rcss, "body { display: block; } #box { display: block; width: 100dp; height: 10dp; }");
	write(rml, R"(<rml><head><link type="text/rcss" href="panel.rcss"/></head>
		<body data-model="panel"><div id="box"/><p id="name">{{name}}</p></body></rml>)");
	std::string name = "Audio 1";
	ui::DataModel model(fixture.window.context(), "panel");
	REQUIRE(model.constructor().Bind("name", &name));
	Rml::ElementDocument* panel = fixture.window.context().LoadDocument(rml.generic_u8string());
	REQUIRE(panel != nullptr);
	panel->Show();
	Rml::ElementDocument* hidden = fixture.window.context().LoadDocument(rml.generic_u8string());
	REQUIRE(hidden != nullptr);
	show(fixture.window, R"(<rml><body><p id="memory">from memory</p></body></rml>)");
	fixture.ui.shell.step(milliseconds(0));
	ui::FileInterface& files = fixture.ui.rmlui.files();
	CHECK(ui::reloadChangedDocuments(files, fixture.ui.shell) == 0); // nothing changed

	SECTION("a style sheet changes")
	{
		change(rcss, "body { display: block; } #box { display: block; width: 150dp; height: 10dp; }");
		CHECK(ui::reloadChangedDocuments(files, fixture.ui.shell) == 2);
		CHECK(fixture.window.renderRequested());
		fixture.ui.shell.step(milliseconds(0));
		CHECK(find(fixture.window, "box").GetClientWidth() == 150.0F);
	}
	SECTION("a document changes")
	{
		change(rml, R"(<rml><head><link type="text/rcss" href="panel.rcss"/></head>
			<body data-model="panel"><div id="box"/><h1 id="title">{{name}}</h1></body></rml>)");
		CHECK(ui::reloadChangedDocuments(files, fixture.ui.shell) == 2);
		fixture.ui.shell.step(milliseconds(0));
		CHECK(find(fixture.window, "title").GetInnerRML() == "Audio 1"); // still bound to the data model
	}
	// The documents are loaded again in their places: shown or hidden as before, and the one from memory kept.
	REQUIRE(fixture.window.context().GetNumDocuments() == 3);
	CHECK(fixture.window.context().GetDocument(0)->GetElementById("memory") != nullptr);
	CHECK(fixture.window.context().GetDocument(1)->IsVisible());
	CHECK_FALSE(fixture.window.context().GetDocument(2)->IsVisible());
	CHECK(ui::reloadChangedDocuments(files, fixture.ui.shell) == 0);
}

TEST_CASE("the shell runs periodic tasks between its waits for events", "[unit]")
{
	OneWindow fixture;
	fixture.ui.shell.step(milliseconds(0));
	int runs = 0;
	fixture.ui.shell.every(milliseconds(5), [&runs] { ++runs; });
	const std::optional<milliseconds> timeout = fixture.ui.shell.idleTimeout();
	REQUIRE(timeout.has_value());
	CHECK(timeout.value_or(milliseconds::max()) <= milliseconds(5));

	std::this_thread::sleep_for(milliseconds(6));
	fixture.ui.shell.step(milliseconds(0));
	CHECK(runs == 1);
	fixture.ui.shell.step(milliseconds(0));
	CHECK(runs == 1); // not due again yet
}
