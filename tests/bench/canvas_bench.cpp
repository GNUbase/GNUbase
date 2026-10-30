// SPDX-License-Identifier: GPL-3.0-or-later
// The canvas benchmark (#55): 10,000 events on 100 tracks under a ruler, in a window drawn through SDL_GPU with vsync
// in the default theme, as the app draws. Three phases: scrolling a zoomed-in view across and down, scrolling with
// all 10,000 events in view, and zooming with all of them in view, which builds their geometry every frame. For each
// it prints the frames per second against the display's refresh rate and the frames that came later than one
// refresh. It needs a display and a GPU; build a Release configuration for meaningful numbers. Exit code 1 if a phase
// fell behind the refresh rate.
//
//   gnubase-canvas-bench [--seconds <per phase, default 3>]
#include "ui/canvas.h"
#include "ui/fonts.h"
#include "ui/gpurenderer.h"
#include "ui/rmlui.h"
#include "ui/sdlwindowsystem.h"
#include "ui/shell.h"
#include "ui/themes.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

using namespace gnubase;
using Clock = std::chrono::steady_clock;

namespace
{
	constexpr int g_tracks = 100;
	constexpr int g_eventsPerTrack = 100;
	constexpr double g_spacing = 1000.0; // units between events on a track
	constexpr double g_length = 600.0;   // units an event lasts
	constexpr double g_project = g_eventsPerTrack * g_spacing;

	struct Phase
	{
		const char* name;
		// Sets the view for a frame: the elements' attributes.
		std::function<void(int frame, Rml::Element& ruler, Rml::Element& lanes)> view;
	};

	// Frames per second and late frames of one phase.
	struct Result
	{
		int frames = 0;
		int late = 0;
		double fps = 0.0;
		double worstMs = 0.0;
		uint64_t builds = 0;
	};

	Result run(ui::Shell& shell, ui::Window& window, Rml::Element& ruler, ui::EventLanes& lanes, const Phase& phase,
		double seconds, double refresh)
	{
		Result result;
		const uint64_t builds = lanes.builds();
		const Clock::time_point start = Clock::now();
		Clock::time_point last = start;
		while (Clock::now() - start < std::chrono::duration<double>(seconds))
		{
			phase.view(result.frames, ruler, lanes);
			window.requestRender();
			shell.step(std::chrono::milliseconds(0));
			const Clock::time_point now = Clock::now();
			const double ms = std::chrono::duration<double, std::milli>(now - last).count();
			last = now;
			if (result.frames > 0) // the first frame waits for whatever came before
			{
				result.worstMs = std::max(result.worstMs, ms);
				if (refresh > 0.0 && ms > 1500.0 / refresh)
					++result.late;
			}
			++result.frames;
		}
		result.fps = result.frames / std::chrono::duration<double>(last - start).count();
		result.builds = lanes.builds() - builds;
		return result;
	}

	void setView(Rml::Element& ruler, Rml::Element& lanes, double start, double scale)
	{
		for (Rml::Element* element : {&ruler, &lanes})
		{
			element->SetAttribute("start", std::to_string(start));
			element->SetAttribute("scale", std::to_string(scale));
		}
	}
}

int main(int argc, char** argv)
{
	try
	{
		double seconds = 3.0;
		for (int i = 1; i < argc; ++i)
		{
			const std::string option = argv[i];
			if (option != "--seconds" || i + 1 == argc)
			{
				std::fprintf(stderr, "usage: gnubase-canvas-bench [--seconds <per phase>]\n");
				return 2;
			}
			seconds = std::stod(argv[++i]);
		}

		ui::SdlWindowSystem windows;
		ui::GpuRenderer renderer(windows);
		ui::RmlUi rmlui(windows);
		const std::filesystem::path resources(GNUBASE_RESOURCES_DIR);
		ui::Fonts(resources / "fonts").load();
		const ui::Themes themes(resources);
		ui::Shell shell(windows, renderer);
		shell.setTheme("gnubase");
		ui::WindowSpec spec;
		spec.title = "Canvas benchmark";
		spec.size = {1600, 1000};
		ui::Window& window = shell.open(spec);
		Rml::ElementDocument* document = window.context().LoadDocumentFromMemory(R"(<rml><head><style>
			body { position: absolute; left: 0; top: 0; right: 0; bottom: 0; margin: 0; }
			ruler { width: 100%; height: 24dp; }
			event-lanes { width: 100%; height: 960dp; }
		</style></head><body><ruler id="ruler"/><event-lanes id="lanes"/></body></rml>)");
		if (document == nullptr)
			throw std::runtime_error("the benchmark document doesn't load");
		document->Show();
		Rml::Element& ruler = *document->GetElementById("ruler");
		auto& lanes = dynamic_cast<ui::EventLanes&>(*document->GetElementById("lanes"));

		std::vector<ui::EventLanes::Lane> tracks(g_tracks);
		for (int track = 0; track < g_tracks; ++track)
			for (int i = 0; i < g_eventsPerTrack; ++i)
				tracks[track].events.push_back({(i * g_spacing) + ((track * 37) % 400), g_length});
		const auto heights = [&](float height)
		{
			for (ui::EventLanes::Lane& lane : tracks)
				lane.height = height;
			lanes.setLanes(tracks);
		};

		SDL_Window* sdl = ui::SdlWindowSystem::sdlWindow(window.id());
		const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(sdl));
		const double refresh = mode != nullptr ? mode->refresh_rate : 0.0;
		const double width = window.pixelSize().width / window.dpRatio(); // dp; the elements are as wide
		std::printf("SDL_GPU %s, %.0f x %.0f pixels at dp ratio %.2f, display refresh %.2f Hz\n",
			renderer.driver().c_str(), static_cast<double>(window.pixelSize().width),
			static_cast<double>(window.pixelSize().height), static_cast<double>(window.dpRatio()), refresh);

		const std::vector<Phase> phases{
			{"scroll, zoomed in",
				[&](int frame, Rml::Element& r, Rml::Element& l)
				{
					// A tenth of the project across, 4 dp a frame; 16 of the 100 tracks, 2 dp a frame down and up.
					if (frame == 0)
						heights(60.0F);
					const double scale = g_project / 10.0 / width;
					setView(r, l, std::fmod(frame * 4.0 * scale, g_project * 0.9), scale);
					l.SetAttribute("scroll-top", std::to_string(std::abs(std::fmod(frame * 2.0, 10000.0) - 5000.0)));
				}},
			{"scroll, all 10,000 events in view",
				[&](int frame, Rml::Element& r, Rml::Element& l)
				{
					if (frame == 0)
					{
						heights(9.6F);
						l.SetAttribute("scroll-top", "0");
					}
					const double scale = g_project / width;
					setView(r, l, std::fmod(frame * 4.0 * scale, width * scale) - (width * scale / 2.0), scale);
				}},
			{"zoom, all 10,000 events in view",
				[&](int frame, Rml::Element& r, Rml::Element& l)
				{
					// Between the whole project and three quarters of it: every event is built again every frame.
					const double scale = g_project / width * (0.875 + (0.125 * std::sin(frame * 0.05)));
					setView(r, l, 0.0, scale);
				}},
		};

		bool behind = false;
		std::printf("%-36s %8s %8s %10s %8s %8s\n", "phase", "frames", "fps", "worst ms", "late", "builds");
		for (const Phase& phase : phases)
		{
			const Result result = run(shell, window, ruler, lanes, phase, seconds, refresh);
			std::printf("%-36s %8d %8.1f %10.1f %8d %8llu\n", phase.name, result.frames, result.fps, result.worstMs,
				result.late, static_cast<unsigned long long>(result.builds));
			behind = behind || (refresh > 0.0 && result.fps < refresh * 0.95);
		}
		if (behind)
			std::printf("Fell behind the display's refresh rate.\n");
		return behind ? 1 : 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "gnubase-canvas-bench: %s\n", error.what());
		return 3;
	}
}
