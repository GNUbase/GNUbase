// SPDX-License-Identifier: GPL-3.0-or-later
// gnubase-render-spike: renders an RmlUi document through one render backend, interactively, into a screenshot, or as
// a benchmark with many meters and waveforms (README.md).
#include "backend.h"
#include "elements.h"
#include "image.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/FontEngineInterface.h>
#include <RmlUi_Platform_SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
	using namespace gnubase::spike;
	using Clock = std::chrono::steady_clock;

	struct Options
	{
		std::string backend;
		std::string gpuDriver; // SDL_GPU: vulkan, direct3d12 or metal; empty for SDL's choice
		std::string document = "features";
		std::string screenshot;
		int benchmarkFrames = 0;
		int meters = 256;
		int waveforms = 48;
		float scale = 0.0F; // dp ratio; 0 for the window's display scale
		WindowOptions window;
	};

	constexpr const char* g_usage = R"(usage: gnubase-render-spike --backend gl3|gpu [options]
  --gpu-driver NAME     SDL_GPU driver: vulkan, direct3d12 or metal (default: SDL's choice)
  --document NAME       features or benchmark, or the path of an RML file (default: features)
  --screenshot FILE     render offscreen, write FILE (PNG) and exit
  --benchmark FRAMES    time FRAMES frames without vsync and print the results as JSON
  --meters N            meters in the benchmark document (default 256)
  --waveforms N         waveforms in the benchmark document (default 48)
  --scale RATIO         dp ratio (default: the monitor's scale); the window grows with it
  --size WxH            window size in dp (default 960x640)
)";

	int number(const std::string& option, const char* value)
	{
		try
		{
			return std::stoi(value);
		}
		catch (const std::exception&)
		{
			throw std::invalid_argument(option + " needs a number");
		}
	}

	Options parse(int argc, char** argv)
	{
		Options options;
		options.window.width = 960;
		options.window.height = 640;
		for (int i = 1; i < argc; ++i)
		{
			const std::string option = argv[i];
			if (i + 1 >= argc)
				throw std::invalid_argument(option + " needs a value");
			const char* value = argv[++i];
			if (option == "--size")
			{
				const std::string size = value;
				const size_t x = size.find('x');
				if (x == std::string::npos)
					throw std::invalid_argument("--size needs WxH");
				options.window.width = number(option, size.substr(0, x).c_str());
				options.window.height = number(option, size.substr(x + 1).c_str());
				continue;
			}
			if (option == "--backend")
				options.backend = value;
			else if (option == "--gpu-driver")
				options.gpuDriver = value;
			else if (option == "--document")
				options.document = value;
			else if (option == "--screenshot")
				options.screenshot = value;
			else if (option == "--benchmark")
				options.benchmarkFrames = number(option, value);
			else if (option == "--meters")
				options.meters = number(option, value);
			else if (option == "--waveforms")
				options.waveforms = number(option, value);
			else if (option == "--scale")
				options.scale = std::stof(value);
			else
				throw std::invalid_argument("unknown option " + option);
		}
		if (options.backend != "gl3" && options.backend != "gpu")
			throw std::invalid_argument("--backend must be gl3 or gpu");
		return options;
	}

	std::string documentPath(const std::string& name)
	{
		if (name == "features" || name == "benchmark")
			return std::string(GNUBASE_SPIKE_DOCUMENTS) + "/" + name + ".rml";
		return name;
	}

	// Fills the benchmark document's #meters and #waveforms.
	struct Benchmark
	{
		std::vector<Rml::Element*> levels;
		std::vector<Waveform*> waveforms;

		Benchmark(Rml::ElementDocument& document, int meters, int waveformCount)
		{
			if (Rml::Element* strip = document.GetElementById("meters"))
				for (int i = 0; i < meters; ++i)
				{
					Rml::Element* meter = strip->AppendChild(document.CreateElement("div"));
					meter->SetClass("meter", true);
					levels.push_back(meter->AppendChild(document.CreateElement("div")));
					levels.back()->SetClass("level", true);
				}
			if (Rml::Element* lanes = document.GetElementById("waveforms"))
				for (int i = 0; i < waveformCount; ++i)
					waveforms.push_back(static_cast<Waveform*>(lanes->AppendChild(document.CreateElement("waveform"))));
		}

		// Moves every meter and scrolls every waveform, as during playback.
		void step(int frame)
		{
			for (size_t i = 0; i < levels.size(); ++i)
			{
				const float level =
					0.5F + (0.5F * std::sin((static_cast<float>(frame) * 0.07F) + static_cast<float>(i)));
				levels[i]->SetProperty(Rml::PropertyId::Height, Rml::Property(level * 100.0F, Rml::Unit::PERCENT));
			}
			for (size_t i = 0; i < waveforms.size(); ++i)
				waveforms[i]->setPhase((static_cast<float>(frame) * 4.0F) + (static_cast<float>(i) * 97.0F));
		}
	};

	double percentile(std::vector<double> values, double fraction)
	{
		if (values.empty())
			return 0.0;
		std::sort(values.begin(), values.end());
		return values[static_cast<size_t>(fraction * static_cast<double>(values.size() - 1))];
	}

	int run(const Options& options)
	{
		if (!SDL_Init(SDL_INIT_VIDEO))
			throw std::runtime_error(std::string("SDL cannot start its video subsystem: ") + SDL_GetError());

		WindowOptions window = options.window;
		window.vsync = options.benchmarkFrames == 0;
		if (options.scale > 0.0F)
		{
			window.width = static_cast<int>(static_cast<float>(window.width) * options.scale);
			window.height = static_cast<int>(static_cast<float>(window.height) * options.scale);
		}
		std::string error;
		std::unique_ptr<Backend> backend =
			options.backend == "gl3" ? makeGl3Backend(window, error) : makeGpuBackend(window, options.gpuDriver, error);
		if (!backend)
			throw std::runtime_error(error);

		SystemInterface_SDL system(backend->window());
		Rml::FontEngineInterface noFonts; // text waits for FreeType (#207)
		Rml::SetSystemInterface(&system);
		Rml::SetFontEngineInterface(&noFonts);
		Rml::SetRenderInterface(&backend->renderInterface());
		Rml::Initialise();
		registerElements();
		Rml::Context* context = Rml::CreateContext("main", backend->pixelSize());
		const float ratio = options.scale > 0.0F ? options.scale : SDL_GetWindowDisplayScale(backend->window());
		context->SetDensityIndependentPixelRatio(ratio);
		Rml::ElementDocument* document = context->LoadDocument(documentPath(options.document));
		if (document == nullptr)
			throw std::runtime_error("cannot load " + documentPath(options.document));
		document->Show();
		Benchmark benchmark(*document, options.document == "benchmark" ? options.meters : 0,
			options.document == "benchmark" ? options.waveforms : 0);

		const auto draw = [context] { context->Render(); };
		int status = EXIT_SUCCESS;
		const Rml::Vector2i size = backend->pixelSize();
		std::cout << R"({"backend": ")" << backend->name() << R"(", "driver": ")" << backend->driver()
				  << R"(", "pixels": [)" << size.x << ", " << size.y << "], \"dpRatio\": " << ratio << "}\n";

		if (!options.screenshot.empty())
		{
			Image image;
			for (int frame = 0; frame < 3; ++frame) // let geometry and textures settle
			{
				benchmark.step(frame);
				context->Update();
				if (!backend->render(draw, &image))
					throw std::runtime_error("cannot render offscreen");
			}
			if (!writePng(image, options.screenshot))
				throw std::runtime_error("cannot write " + options.screenshot);
		}
		else if (options.benchmarkFrames > 0)
		{
			constexpr int warmup = 30;
			std::vector<double> frames;
			std::vector<double> cpu;
			Clock::time_point start;
			for (int frame = 0; frame < warmup + options.benchmarkFrames; ++frame)
			{
				if (frame == warmup)
				{
					backend->finish();
					start = Clock::now();
				}
				const Clock::time_point begin = Clock::now();
				benchmark.step(frame);
				context->Update();
				Clock::time_point submitted;
				if (!backend->render(
						[&]
						{
							draw();
							submitted = Clock::now();
						},
						nullptr))
					throw std::runtime_error("cannot render a frame");
				const Clock::time_point end = Clock::now();
				if (frame >= warmup)
				{
					frames.push_back(std::chrono::duration<double, std::milli>(end - begin).count());
					cpu.push_back(std::chrono::duration<double, std::milli>(submitted - begin).count());
				}
			}
			backend->finish();
			const double total = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
			std::cout << R"({"benchmark": {"frames": )" << options.benchmarkFrames << R"(, "meters": )"
					  << benchmark.levels.size() << R"(, "waveforms": )" << benchmark.waveforms.size()
					  << R"(, "msPerFrame": )" << total / options.benchmarkFrames << R"(, "medianMs": )"
					  << percentile(frames, 0.5) << R"(, "p95Ms": )" << percentile(frames, 0.95)
					  << R"(, "cpuMedianMs": )" << percentile(cpu, 0.5) << "}}\n";
		}
		else
		{
			for (bool running = true; running;)
			{
				SDL_Event event;
				while (SDL_PollEvent(&event))
				{
					if (event.type == SDL_EVENT_QUIT)
						running = false;
					else if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
						context->SetDimensions(backend->pixelSize());
					else
						RmlSDL::InputEventHandler(context, backend->window(), event);
				}
				context->Update();
				if (!backend->render(draw, nullptr))
				{
					status = EXIT_FAILURE;
					break;
				}
			}
		}

		Rml::Shutdown();
		backend.reset();
		SDL_Quit();
		return status;
	}
}

int main(int argc, char** argv)
{
	try
	{
		return run(parse(argc, argv));
	}
	catch (const std::invalid_argument& error)
	{
		std::cerr << "gnubase-render-spike: " << error.what() << "\n\n" << g_usage;
		return 2;
	}
	catch (const std::exception& error)
	{
		std::cerr << "gnubase-render-spike: " << error.what() << '\n';
		return 1;
	}
}
