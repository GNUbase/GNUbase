// SPDX-License-Identifier: GPL-3.0-or-later
// Snap and grid (#87, reference manual pp. 105-109). docs/features/snap.md describes it.
#include "engine/document/snap.h"

#include "engine/audiofile/audiofile.h"
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/engine.h"
#include "engine/midi/quantize.h"
#include "platform/log.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		constexpr const char* g_snapTypeNames[] = {
			"grid", "gridRelative", "events", "shuffle", "cursor", "gridCursor", "eventsCursor", "eventsGridCursor"};
		constexpr const char* g_gridTypeNames[] = {"bar", "beat", "quantize", "adaptToZoom", "1ms", "10ms", "100ms",
			"1000ms", "subframe", "quarterFrame", "halfFrame", "frame", "twoFrames", "second", "1sample", "10samples",
			"100samples", "44100samples"};
		constexpr const char* g_gridFormatNames[] = {"barsBeats", "seconds", "timecode", "samples", "userFrames"};
		static_assert(std::size(g_gridFormatNames) == g_gridFormatCount);
		// The format of each grid type; the frames are also userFrames'.
		constexpr GridFormat g_gridFormats[] = {GridFormat::BarsBeats, GridFormat::BarsBeats, GridFormat::BarsBeats,
			GridFormat::BarsBeats, GridFormat::Seconds, GridFormat::Seconds, GridFormat::Seconds, GridFormat::Seconds,
			GridFormat::Timecode, GridFormat::Timecode, GridFormat::Timecode, GridFormat::Timecode,
			GridFormat::Timecode, GridFormat::Timecode, GridFormat::Samples, GridFormat::Samples, GridFormat::Samples,
			GridFormat::Samples};
		static_assert(std::size(g_gridFormats) == std::size(g_gridTypeNames));

		constexpr double g_zeroCrossingWindow = 0.05; // seconds either side

		bool musical(GridType grid)
		{
			return grid == GridType::Bar || grid == GridType::Beat || grid == GridType::Quantize ||
				grid == GridType::AdaptToZoom;
		}

		// A musical grid's step at tick: the bar there, or a step from the bar's start.
		struct Bar
		{
			time::Ticks start = 0;
			time::Ticks end = 0;
		};

		Bar barAt(const SnapGrid& at, time::Ticks tick)
		{
			const int64_t bar = at.signatures.toBarsBeats(tick).bar;
			return {at.signatures.barStart(bar), at.signatures.barStart(bar + 1)};
		}

		// Beat and adapt to zoom: steps from the bar's start; 0 is the bar itself.
		time::Ticks stepTicks(GridType grid, const SnapGrid& at, time::Ticks tick)
		{
			const Bar bar = barAt(at, tick);
			if (grid == GridType::Beat)
				return 4 * time::g_ticksPerQuarter / at.signatures.signatureAt(tick).denominator;
			if (grid == GridType::AdaptToZoom && at.zoomStep > 0 && at.zoomStep < bar.end - bar.start)
				return at.zoomStep;
			return 0;
		}

		time::Ticks nearestTick(GridType grid, const SnapGrid& at, time::Ticks tick)
		{
			if (grid == GridType::Quantize)
				return midi::QuantizeGrid(at.quantize, at.signatures).nearest(tick).position;
			const Bar bar = barAt(at, tick);
			time::Ticks before = bar.start;
			time::Ticks after = bar.end;
			if (const time::Ticks step = stepTicks(grid, at, tick); step > 0)
			{
				before = bar.start + ((tick - bar.start) / step * step);
				after = std::min(before + step, bar.end);
			}
			return tick - before < after - tick ? before : after; // at equal distance the later one
		}

		// The linear grids' step, in seconds; 0 for the sample grids.
		double stepSeconds(GridType grid, const SnapGrid& at)
		{
			const double frame = (at.frameRate.pullDown ? 1.001 : 1.0) / at.frameRate.framesPerSecond;
			switch (grid)
			{
			case GridType::Ms1:
				return 0.001;
			case GridType::Ms10:
				return 0.01;
			case GridType::Ms100:
				return 0.1;
			case GridType::Ms1000:
			case GridType::Second:
				return 1.0;
			case GridType::Subframe:
				return frame / 80.0;
			case GridType::QuarterFrame:
				return frame / 4.0;
			case GridType::HalfFrame:
				return frame / 2.0;
			case GridType::Frame:
				return frame;
			case GridType::TwoFrames:
				return 2.0 * frame;
			default:
				return 0.0;
			}
		}

		int64_t stepSamples(GridType grid)
		{
			switch (grid)
			{
			case GridType::Samples10:
				return 10;
			case GridType::Samples100:
				return 100;
			case GridType::Samples44100:
				return 44100;
			default:
				return 1;
			}
		}

		time::Samples nearestSample(GridType grid, const SnapGrid& at, time::Samples position)
		{
			// The linear grids count from the start time, as the ruler's marks do (#537); the first line is the first
			// after the project start.
			if (const double step = stepSeconds(grid, at); step > 0.0)
			{
				const double first = std::ceil((at.startTime / step) - 1e-6); // a start time on a line is that line
				const double line = std::max(
					first, std::round(((static_cast<double>(position) / at.sampleRate) + at.startTime) / step));
				return std::llround(((line * step) - at.startTime) * at.sampleRate);
			}
			const int64_t step = stepSamples(grid);
			return std::llround(std::round(static_cast<double>(position) / static_cast<double>(step))) * step;
		}

		bool hasGrid(SnapType type)
		{
			return type == SnapType::Grid || type == SnapType::GridRelative || type == SnapType::GridCursor ||
				type == SnapType::EventsGridCursor;
		}

		bool hasEvents(SnapType type)
		{
			return type == SnapType::Events || type == SnapType::EventsCursor || type == SnapType::EventsGridCursor;
		}

		bool hasCursor(SnapType type)
		{
			return type == SnapType::Cursor || type == SnapType::GridCursor || type == SnapType::EventsCursor ||
				type == SnapType::EventsGridCursor;
		}

		template <typename T, size_t N>
		std::optional<T> fromName(const char* const (&names)[N], const std::string& name)
		{
			const auto found =
				std::find_if(std::begin(names), std::end(names), [&name](const char* each) { return name == each; });
			if (found == std::end(names))
				return std::nullopt;
			return static_cast<T>(std::distance(std::begin(names), found));
		}

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		void forgetNewerGrid(SnapSettings& settings, const std::string& format)
		{
			auto& newer = settings.newerGrids;
			newer.erase(
				std::remove_if(newer.begin(), newer.end(),
					[&format](const std::pair<std::string, std::string>& entry) { return entry.first == format; }),
				newer.end());
		}
	}

	bool hasGrid(GridFormat format, GridType grid)
	{
		const GridFormat of = g_gridFormats[static_cast<size_t>(grid)];
		return of == format || (of == GridFormat::Timecode && format == GridFormat::UserFrames);
	}

	std::vector<GridType> gridTypes(GridFormat format)
	{
		std::vector<GridType> result;
		for (size_t i = 0; i < std::size(g_gridFormats); ++i)
			if (hasGrid(format, static_cast<GridType>(i)))
				result.push_back(static_cast<GridType>(i));
		return result;
	}

	void SnapSettings::setGrid(GridType grid)
	{
		if (!hasGrid(format, grid))
			format = g_gridFormats[static_cast<size_t>(grid)];
		setGrid(format, grid);
	}

	void SnapSettings::setGrid(GridFormat of, GridType grid)
	{
		// Another grid type chosen here replaces a newer build's; the same one again (snap.set with what snap.get
		// returned) keeps it.
		if (grids.at(static_cast<size_t>(of)) != grid)
			forgetNewerGrid(*this, g_gridFormatNames[static_cast<size_t>(of)]);
		grids.at(static_cast<size_t>(of)) = grid;
	}

	const char* snapTypeName(SnapType type)
	{
		return g_snapTypeNames[static_cast<size_t>(type)];
	}

	const char* gridTypeName(GridType grid)
	{
		return g_gridTypeNames[static_cast<size_t>(grid)];
	}

	const char* gridFormatName(GridFormat format)
	{
		return g_gridFormatNames[static_cast<size_t>(format)];
	}

	std::optional<SnapType> snapTypeFromName(const std::string& name)
	{
		return fromName<SnapType>(g_snapTypeNames, name);
	}

	std::optional<GridType> gridTypeFromName(const std::string& name)
	{
		return fromName<GridType>(g_gridTypeNames, name);
	}

	std::optional<GridFormat> gridFormatFromName(const std::string& name)
	{
		return fromName<GridFormat>(g_gridFormatNames, name);
	}

	Json toJson(const SnapSettings& settings, bool saving)
	{
		Json grids = Json::object();
		for (size_t i = 0; i < g_gridFormatCount; ++i)
			grids[g_gridFormatNames[i]] = gridTypeName(settings.grids.at(i));
		if (saving)
			for (const auto& [format, grid] : settings.newerGrids)
				grids[format] = grid;
		return {{"on", settings.on}, {"type", snapTypeName(settings.type)}, {"format", gridFormatName(settings.format)},
			{"grid", gridTypeName(settings.grid())}, {"grids", grids}, {"zeroCrossing", settings.zeroCrossing}};
	}

	namespace
	{
		// A name, from a file that may have anything there.
		std::string nameFrom(const Json& name)
		{
			if (!name.is_string())
				throw std::invalid_argument("not a name: " + name.dump());
			return name.get<std::string>();
		}

		GridType gridFrom(const Json& name)
		{
			const std::optional<GridType> grid = gridTypeFromName(nameFrom(name));
			if (!grid)
				throw std::invalid_argument("unknown grid type " + name.dump());
			return *grid;
		}

		void requireGrid(GridFormat format, GridType grid)
		{
			if (!hasGrid(format, grid))
				throw std::invalid_argument(
					std::string("the grid type ") + gridTypeName(grid) + " isn't one of " + gridFormatName(format));
		}
	}

	SnapSettings snapSettingsFrom(const Json& json, SnapSettings settings, bool loading)
	{
		// Loading, a name that isn't known (a newer build's) or a grid type that isn't its format's is left out.
		const auto refuse = [loading](const std::invalid_argument& error)
		{
			if (!loading)
				throw error;
		};
		if (!json.is_object())
		{
			refuse(std::invalid_argument("snap settings aren't an object: " + json.dump()));
			return settings;
		}
		for (const auto& [name, value] : {std::pair<const char*, bool*>{"on", &settings.on},
				 std::pair<const char*, bool*>{"zeroCrossing", &settings.zeroCrossing}})
			if (const auto found = json.find(name); found != json.end())
			{
				if (found->is_boolean())
					*value = found->get<bool>();
				else
					refuse(std::invalid_argument(std::string(name) + " isn't true or false: " + found->dump()));
			}
		if (json.contains("type"))
			try
			{
				const std::optional<SnapType> type = snapTypeFromName(nameFrom(json.at("type")));
				if (!type)
					throw std::invalid_argument("unknown snap type " + json.at("type").dump());
				settings.type = *type;
			}
			catch (const std::invalid_argument& error)
			{
				refuse(error);
			}
		Json grids = json.value("grids", Json::object());
		if (!grids.is_object())
		{
			refuse(std::invalid_argument("grids isn't an object: " + grids.dump()));
			grids = Json::object();
		}
		std::vector<std::string> taken; // the grids entries applied or kept
		for (const auto& [name, grid] : grids.items())
			try
			{
				const std::optional<GridFormat> format = gridFormatFromName(name);
				// Loading, a newer build's entry, with a format or grid type this build doesn't know, is kept for it.
				if (loading && grid.is_string() && (!format || !gridTypeFromName(grid.get<std::string>())))
				{
					forgetNewerGrid(settings, name);
					settings.newerGrids.emplace_back(name, grid.get<std::string>());
					taken.push_back(name);
					continue;
				}
				if (!format)
					throw std::invalid_argument("unknown time format " + name);
				const GridType type = gridFrom(grid);
				requireGrid(*format, type);
				settings.setGrid(*format, type);
				taken.push_back(name);
			}
			catch (const std::invalid_argument& error)
			{
				refuse(error);
			}
		std::optional<GridFormat> format;
		if (json.contains("format"))
			try
			{
				format = gridFormatFromName(nameFrom(json.at("format")));
				if (!format)
					throw std::invalid_argument("unknown time format " + json.at("format").dump());
				settings.format = *format;
			}
			catch (const std::invalid_argument& error)
			{
				refuse(error);
			}
		if (!json.contains("grid") || (json.contains("format") && !format)) // or an unknown format's grid type
			return settings;
		// Loading, grids has the format's grid type; grid is for builds from before #413, which read only that.
		if (loading && format && std::find(taken.begin(), taken.end(), gridFormatName(*format)) != taken.end())
			return settings;
		try
		{
			const GridType grid = gridFrom(json.at("grid"));
			if (format)
				requireGrid(*format, grid);
			settings.setGrid(grid);
		}
		catch (const std::invalid_argument& error)
		{
			refuse(error);
		}
		return settings;
	}

	time::Position nearestGridLine(GridType grid, const SnapGrid& at, time::Samples position)
	{
		if (musical(grid))
			return {time::TimeBase::Musical, nearestTick(grid, at, at.tempo.toTicks(position, at.sampleRate))};
		return {time::TimeBase::Linear, nearestSample(grid, at, position)};
	}

	time::Position gridStep(GridType grid, const SnapGrid& at, time::Samples position)
	{
		if (!musical(grid))
		{
			const double step = stepSeconds(grid, at);
			return {time::TimeBase::Linear,
				step > 0.0 ? std::max<int64_t>(1, std::llround(step * at.sampleRate)) : stepSamples(grid)};
		}
		const time::Ticks tick = at.tempo.toTicks(position, at.sampleRate);
		if (grid == GridType::Quantize)
			return {time::TimeBase::Musical,
				std::max<time::Ticks>(1, std::llround(midi::QuantizeGrid(at.quantize, at.signatures).step()))};
		const Bar bar = barAt(at, tick);
		const time::Ticks step = stepTicks(grid, at, tick);
		return {time::TimeBase::Musical, step > 0 ? step : bar.end - bar.start};
	}

	time::Position snap(const SnapSettings& settings, const SnapGrid& grid, const SnapTarget& target)
	{
		const time::Position unchanged{time::TimeBase::Linear, target.position};
		if (!settings.on || settings.type == SnapType::Shuffle)
			return unchanged;
		// Magnets and the cursor within range, the nearest first.
		std::optional<time::Samples> best;
		const auto consider = [&best, &target](time::Samples magnet)
		{
			const time::Samples distance = std::abs(magnet - target.position);
			if (distance <= target.range && (!best || distance < std::abs(*best - target.position)))
				best = magnet;
		};
		if (hasEvents(settings.type))
			for (const time::Samples magnet : target.magnets)
				consider(magnet);
		if (hasCursor(settings.type))
			consider(target.cursor);
		if (best)
			return {time::TimeBase::Linear, *best};
		if (!hasGrid(settings.type))
			return unchanged;
		if (settings.type != SnapType::GridRelative || !target.from)
			return nearestGridLine(settings.grid(), grid, target.position);
		// Grid relative (reference manual p. 106): moves in grid steps, keeping the distance to the nearest line.
		if (musical(settings.grid()))
		{
			const time::Ticks from = grid.tempo.toTicks(*target.from, grid.sampleRate);
			const time::Ticks offset = from - nearestTick(settings.grid(), grid, from);
			const time::Ticks to = grid.tempo.toTicks(target.position, grid.sampleRate) - offset;
			return {time::TimeBase::Musical, nearestTick(settings.grid(), grid, to) + offset};
		}
		const time::Samples offset = *target.from - nearestSample(settings.grid(), grid, *target.from);
		return {time::TimeBase::Linear, nearestSample(settings.grid(), grid, target.position - offset) + offset};
	}

	SnapGrid snapGrid(const Project& project, GridFormat format)
	{
		SnapGrid grid;
		grid.tempo = project.tempo;
		grid.signatures = project.signatures;
		grid.sampleRate = project.sampleRate;
		grid.quantize = project.quantize;
		grid.frameRate = format == GridFormat::UserFrames ? time::FrameRate{time::g_userFramesPerSecond, false, false}
														  : project.setup.frameRate;
		grid.startTime = project.setup.startTime;
		return grid;
	}

	time::Samples zeroCrossingFrame(
		const MediaFile& media, time::Samples frame, time::Samples first, time::Samples last)
	{
		const int64_t window = std::llround(g_zeroCrossingWindow * media.sampleRate);
		first = std::max<int64_t>(first, frame - window);
		last = std::min<int64_t>(last, frame + window);
		if (last <= first)
			return frame;
		std::vector<float> mix(static_cast<size_t>(last - first + 1));
		try
		{
			const std::unique_ptr<AudioFileReader> reader = AudioFileReader::open(media.path);
			const auto channels = static_cast<size_t>(reader->info().channels);
			std::vector<float> samples(mix.size() * channels);
			reader->seek(first);
			const size_t read = reader->read(samples.data(), mix.size());
			mix.resize(read);
			for (size_t i = 0; i < read; ++i)
				for (size_t c = 0; c < channels; ++c)
					mix[i] += samples[(i * channels) + c];
		}
		catch (const AudioFileError& error)
		{
			logging::warning("Snap to zero crossing can't read {}: {}", media.path, error.what());
			return frame;
		}
		// The frame of each sign change nearer to zero; the one nearest to frame wins.
		std::optional<int64_t> best;
		for (size_t i = 1; i < mix.size(); ++i)
		{
			if ((mix[i - 1] > 0.0F && mix[i] > 0.0F) || (mix[i - 1] < 0.0F && mix[i] < 0.0F))
				continue;
			const int64_t found = first + static_cast<int64_t>(std::abs(mix[i - 1]) <= std::abs(mix[i]) ? i - 1 : i);
			if (!best || std::abs(found - frame) < std::abs(*best - frame))
				best = found;
		}
		return best.value_or(frame);
	}

	namespace
	{
		// zeroCrossing within the frames first to last of the event's file.
		time::Samples crossingInFrames(
			const Project& project, const AudioEvent& event, time::Samples at, int64_t first, int64_t last)
		{
			const MediaFile& media = project.media.at(project.clips.at(event.clip).media);
			const EventSpan span = eventSpan(project, event);
			const int64_t frame =
				event.offset + std::llround(eventFrames(project, event, static_cast<double>(at - span.start)));
			const int64_t found = zeroCrossingFrame(media, frame, first, last);
			if (found == frame)
				return at;
			return span.start + std::llround(eventSamples(project, event, static_cast<double>(found - event.offset)));
		}
	}

	time::Samples zeroCrossing(const Project& project, const AudioEvent& event, time::Samples at, bool inside)
	{
		if (inside)
			return crossingInFrames(project, event, at, event.offset + 1, event.offset + event.length - 1);
		return crossingInFrames(project, event, at, 0, project.media.at(project.clips.at(event.clip).media).length - 1);
	}

	time::Samples zeroCrossingBetween(
		const Project& project, const AudioEvent& event, time::Samples at, time::Samples from, time::Samples to)
	{
		// The frames that play strictly between from and to, and inside the event.
		const EventSpan span = eventSpan(project, event);
		const auto frameAt = [&project, &event, &span](time::Samples position)
		{
			return static_cast<double>(event.offset) +
				eventFrames(project, event, static_cast<double>(position - span.start));
		};
		const int64_t first = std::max<int64_t>(event.offset + 1, static_cast<int64_t>(std::floor(frameAt(from))) + 1);
		const int64_t last =
			std::min<int64_t>(event.offset + event.length - 1, static_cast<int64_t>(std::ceil(frameAt(to))) - 1);
		return crossingInFrames(project, event, at, first, last);
	}

	void registerSnapCommands(ProjectModel& model, Engine& engine)
	{
		const Json boolean = {{"type", "boolean"}};
		const Json integer = {{"type", "integer"}};
		const Json typeSchema = {{"type", "string"},
			{"enum", std::vector<std::string>(std::begin(g_snapTypeNames), std::end(g_snapTypeNames))}};
		const Json gridSchema = {{"type", "string"},
			{"enum", std::vector<std::string>(std::begin(g_gridTypeNames), std::end(g_gridTypeNames))}};
		const Json formatSchema = {{"type", "string"},
			{"enum", std::vector<std::string>(std::begin(g_gridFormatNames), std::end(g_gridFormatNames))}};
		Json gridsProperties = Json::object();
		for (const char* format : g_gridFormatNames)
			gridsProperties[format] = gridSchema;
		const Json gridsSchema = {{"type", "object"}, {"properties", gridsProperties}, {"additionalProperties", false}};
		const Json settingsProperties = {{"on", boolean}, {"type", typeSchema}, {"format", formatSchema},
			{"grid", gridSchema}, {"grids", gridsSchema}, {"zeroCrossing", boolean}};
		Json settingsSchema = {{"type", "object"}, {"properties", settingsProperties},
			{"required", {"on", "type", "format", "grid", "grids", "zeroCrossing"}}};
		settingsSchema["properties"]["grids"]["required"] =
			std::vector<std::string>(std::begin(g_gridFormatNames), std::end(g_gridFormatNames));
		const std::string settingsText =
			"on: whether snap is on. type: grid (the nearest grid line), gridRelative (moves in grid steps, keeping "
			"the distance to the grid), events (the starts and ends of other events, audio events' snap points and "
			"markers, when near), shuffle (events trade places with their neighbours, see event.shuffle), cursor "
			"(the project cursor, when near), or the combinations gridCursor, eventsCursor and eventsGridCursor. "
			"format: the time format the grid follows, the project window's ruler: barsBeats, seconds, timecode, "
			"samples or userFrames. grid: the grid type in use, the format's: for barsBeats bar, beat, quantize (the "
			"project's quantize grid) or adaptToZoom (a note value that the view chooses); for seconds 1ms, 10ms, "
			"100ms, 1000ms; for timecode and userFrames subframe, quarterFrame, halfFrame, frame, twoFrames, second; "
			"for samples 1sample, 10samples, 100samples, 44100samples. A grid type without a format switches to its "
			"format (the frames to timecode unless userFrames is in use). grids: the grid type each format keeps, "
			"by default bar, 100ms, frame, 100samples and frame. zeroCrossing: splitting and resizing audio events, "
			"and the edges of selection ranges, in the project window go where the audio crosses zero.";

		engine.registerCommand(
			{"snap.get", 1, "The project's snap settings. " + settingsText,
				{{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}}, settingsSchema,
				true},
			[&model](CommandContext&) { return toJson(model.project().snap); });

		model.addEdit(engine,
			{"snap.set", 1,
				"Changes the project's snap settings; what isn't given stays. " + settingsText +
					" Settings are not undo steps; they are saved with the project. Emits snap.changed.",
				{{"type", "object"}, {"properties", settingsProperties}, {"additionalProperties", false}},
				settingsSchema, false},
			"Change snap settings",
			[](Project& project, CommandContext& context)
			{
				project.snap = snapSettingsFrom(context.parameters(), project.snap);
				Json result = toJson(project.snap);
				context.emit("snap.changed", result);
				return result;
			});

		model.addEdit(engine,
			{"snap.position", 1,
				"Snaps a position (project samples) as editing in the project window does, with the project's snap "
				"settings, changed by settings: where a point that would go to position goes. from is where it was, "
				"for gridRelative; events are the events being edited, whose edges and snap points don't attract; "
				"range is how near (project samples, default a tenth of a second) events, markers and the cursor "
				"attract; zoomStep is adaptToZoom's note value in ticks (default: bars); framesPerSecond the frame "
				"grids' rate (default: the project's frameRate, see project.set, and for userFrames 30, as in the "
				"project window). The seconds and frame grids count from the project's startTime, as the ruler "
				"does. Returns position in project samples, and ticks, which are exact when "
				"musical, that is when a bar, beat, quantize or zoom grid line put it there.",
				{{"type", "object"},
					{"properties",
						{{"position", integer}, {"from", integer}, {"events", {{"type", "array"}, {"items", integer}}},
							{"range", integer}, {"zoomStep", integer}, {"framesPerSecond", integer},
							{"settings",
								{{"type", "object"}, {"properties", settingsProperties},
									{"additionalProperties", false}}}}},
					{"required", {"position"}}, {"additionalProperties", false}},
				{{"type", "object"}, {"properties", {{"position", integer}, {"ticks", integer}, {"musical", boolean}}},
					{"required", {"position", "ticks", "musical"}}},
				true},
			{},
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const SnapSettings settings =
					snapSettingsFrom(parameters.value("settings", Json::object()), project.snap);
				SnapGrid grid = snapGrid(project, settings.format);
				grid.zoomStep = parameters.value("zoomStep", time::Ticks{0});
				if (parameters.contains("framesPerSecond"))
				{
					const int fps = parameters.at("framesPerSecond").get<int>();
					require(fps >= 1 && fps <= 1000, "framesPerSecond must be 1 to 1000");
					grid.frameRate = {fps, false, false};
				}
				SnapTarget target;
				target.position = parameters.at("position").get<time::Samples>();
				if (parameters.contains("from"))
					target.from = parameters.at("from").get<time::Samples>();
				target.range = parameters.value("range", std::llround(project.sampleRate / 10.0));
				target.cursor = context.call("transport.get").at("position").get<time::Samples>();
				const auto ids = parameters.value("events", std::vector<ObjectId>());
				const std::set<ObjectId> edited(ids.begin(), ids.end());
				for (const auto& [id, event] : project.audioEvents)
				{
					if (edited.count(id) != 0 || edited.count(event.part) != 0) // an edited part's events move with it
						continue;
					const EventSpan span = eventSpan(project, event);
					target.magnets.push_back(span.start);
					target.magnets.push_back(span.end);
					if (event.snapPoint && *event.snapPoint >= event.offset &&
						*event.snapPoint < event.offset + event.length)
						target.magnets.push_back(span.start +
							std::llround(
								eventSamples(project, event, static_cast<double>(*event.snapPoint - event.offset))));
				}
				const auto partEdges = [&](ObjectId id, const time::Position& position, int64_t length)
				{
					if (edited.count(id) != 0)
						return;
					target.magnets.push_back(time::toSamples(position, project.tempo, project.sampleRate));
					target.magnets.push_back(
						time::toSamples({position.base, position.value + length}, project.tempo, project.sampleRate));
				};
				for (const auto& [id, part] : project.midiParts)
					partEdges(id, part.position, part.length);
				for (const auto& [id, part] : project.audioParts)
					partEdges(id, part.position, part.length);
				for (const auto& [id, marker] : project.markers)
				{
					target.magnets.push_back(time::toSamples(marker.position, project.tempo, project.sampleRate));
					if (marker.type == MarkerType::Cycle)
						target.magnets.push_back(time::toSamples(marker.end, project.tempo, project.sampleRate));
				}
				const time::Position snapped = snap(settings, grid, target);
				return Json{{"position", time::toSamples(snapped, project.tempo, project.sampleRate)},
					{"ticks", time::toTicks(snapped, project.tempo, project.sampleRate)},
					{"musical", snapped.base == time::TimeBase::Musical}};
			});

		const Json integers = {{"type", "array"}, {"items", integer}};
		model.addEdit(engine,
			{"snap.zeroCrossings", 1,
				"Where Snap to Zero Crossing moves positions (project samples) on tracks, as the edges of a selection "
				"range: each to the nearest point within 50 ms where the audio event playing there crosses zero, on "
				"the first of the tracks where one plays, and where that event plays: inside it and its audio part, "
				"and not under an event in front of it. The event that plays is unmuted, within its audio part, which "
				"isn't muted, and where events overlap the one in front (event.toFront). A position where no audio "
				"event plays, or without a zero crossing near, stays. Returns positions in the same order.",
				{{"type", "object"}, {"properties", {{"positions", integers}, {"tracks", integers}}},
					{"required", {"positions", "tracks"}}, {"additionalProperties", false}},
				{{"type", "object"}, {"properties", {{"positions", integers}}}, {"required", {"positions"}}}, true},
			{},
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const auto tracks = parameters.at("tracks").get<std::vector<ObjectId>>();
				// Where the audio events on each of the tracks play (#99): unmuted, within their audio part, which
				// isn't muted.
				struct Plays
				{
					const AudioEvent* event = nullptr;
					time::Samples start = 0;
					time::Samples end = 0;
				};
				std::vector<std::vector<Plays>> onTracks(tracks.size());
				for (const auto& [id, event] : project.audioEvents)
				{
					const auto track = std::find(tracks.begin(), tracks.end(), event.track);
					if (track == tracks.end() || event.mute)
						continue;
					EventSpan span = eventSpan(project, event);
					if (const auto part = project.audioParts.find(event.part); part != project.audioParts.end())
					{
						if (part->second.mute)
							continue;
						const time::Position& from = part->second.position;
						span.start = std::max(span.start, time::toSamples(from, project.tempo, project.sampleRate));
						span.end = std::min(span.end,
							time::toSamples(
								{from.base, from.value + part->second.length}, project.tempo, project.sampleRate));
					}
					if (span.start < span.end)
						onTracks[static_cast<size_t>(std::distance(tracks.begin(), track))].push_back(
							{&event, span.start, span.end});
				}
				Json positions = Json::array();
				for (const Json& position : parameters.at("positions"))
				{
					const auto at = position.get<time::Samples>();
					std::optional<time::Samples> moved;
					for (const std::vector<Plays>& plays : onTracks)
					{
						// The event that plays there: of those there, the one in front.
						const Plays* playing = nullptr;
						for (const Plays& each : plays)
							if (each.start < at && at < each.end &&
								(playing == nullptr || frontOf(*each.event) > frontOf(*playing->event)))
								playing = &each;
						if (playing == nullptr)
							continue;
						// It plays from where the nearest event in front of it before the position ends to where the
						// nearest one after it starts.
						time::Samples from = playing->start;
						time::Samples to = playing->end;
						for (const Plays& each : plays)
						{
							if (frontOf(*each.event) <= frontOf(*playing->event))
								continue;
							if (each.end <= at)
								from = std::max(from, each.end);
							else if (each.start >= at)
								to = std::min(to, each.start);
						}
						moved = zeroCrossingBetween(project, *playing->event, at, from, to);
						break;
					}
					positions.push_back(moved.value_or(at));
				}
				return Json{{"positions", std::move(positions)}};
			});
	}
}
