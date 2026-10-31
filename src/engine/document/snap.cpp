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
#include <set>
#include <stdexcept>
#include <utility>

namespace gnubase::engine
{
	namespace
	{
		constexpr const char* g_snapTypeNames[] = {
			"grid", "gridRelative", "events", "shuffle", "cursor", "gridCursor", "eventsCursor", "eventsGridCursor"};
		constexpr const char* g_gridTypeNames[] = {"bar", "beat", "quantize", "adaptToZoom", "1ms", "10ms", "100ms",
			"1000ms", "subframe", "quarterFrame", "halfFrame", "frame", "twoFrames", "second", "1sample", "10samples",
			"100samples", "44100samples"};

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
			if (const double step = stepSeconds(grid, at); step > 0.0)
				return std::llround(
					std::round(static_cast<double>(position) / at.sampleRate / step) * step * at.sampleRate);
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
	}

	const char* snapTypeName(SnapType type)
	{
		return g_snapTypeNames[static_cast<size_t>(type)];
	}

	const char* gridTypeName(GridType grid)
	{
		return g_gridTypeNames[static_cast<size_t>(grid)];
	}

	std::optional<SnapType> snapTypeFromName(const std::string& name)
	{
		return fromName<SnapType>(g_snapTypeNames, name);
	}

	std::optional<GridType> gridTypeFromName(const std::string& name)
	{
		return fromName<GridType>(g_gridTypeNames, name);
	}

	Json toJson(const SnapSettings& settings)
	{
		return {{"on", settings.on}, {"type", snapTypeName(settings.type)}, {"grid", gridTypeName(settings.grid)},
			{"zeroCrossing", settings.zeroCrossing}};
	}

	SnapSettings snapSettingsFrom(const Json& json, SnapSettings settings)
	{
		settings.on = json.value("on", settings.on);
		settings.zeroCrossing = json.value("zeroCrossing", settings.zeroCrossing);
		if (json.contains("type"))
		{
			const std::optional<SnapType> type = snapTypeFromName(json.at("type").get<std::string>());
			if (!type)
				throw std::invalid_argument("unknown snap type " + json.at("type").dump());
			settings.type = *type;
		}
		if (json.contains("grid"))
		{
			const std::optional<GridType> grid = gridTypeFromName(json.at("grid").get<std::string>());
			if (!grid)
				throw std::invalid_argument("unknown grid type " + json.at("grid").dump());
			settings.grid = *grid;
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
			return nearestGridLine(settings.grid, grid, target.position);
		// Grid relative (reference manual p. 106): moves in grid steps, keeping the distance to the nearest line.
		if (musical(settings.grid))
		{
			const time::Ticks from = grid.tempo.toTicks(*target.from, grid.sampleRate);
			const time::Ticks offset = from - nearestTick(settings.grid, grid, from);
			const time::Ticks to = grid.tempo.toTicks(target.position, grid.sampleRate) - offset;
			return {time::TimeBase::Musical, nearestTick(settings.grid, grid, to) + offset};
		}
		const time::Samples offset = *target.from - nearestSample(settings.grid, grid, *target.from);
		return {time::TimeBase::Linear, nearestSample(settings.grid, grid, target.position - offset) + offset};
	}

	SnapGrid snapGrid(const Project& project)
	{
		SnapGrid grid;
		grid.tempo = project.tempo;
		grid.signatures = project.signatures;
		grid.sampleRate = project.sampleRate;
		grid.quantize = project.quantize;
		return grid;
	}

	time::Samples zeroCrossing(const Project& project, const AudioEvent& event, time::Samples at)
	{
		const MediaFile& media = project.media.at(project.clips.at(event.clip).media);
		const EventSpan span = eventSpan(project, event);
		const int64_t frame = event.offset + std::llround(static_cast<double>(at - span.start) / span.ratio);
		const int64_t window = std::llround(g_zeroCrossingWindow * media.sampleRate);
		const int64_t first = std::max<int64_t>(0, frame - window);
		const int64_t last = std::min<int64_t>(media.length - 1, frame + window);
		if (last <= first)
			return at;
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
			return at;
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
		if (!best)
			return at;
		return span.start + std::llround(static_cast<double>(*best - event.offset) * span.ratio);
	}

	void registerSnapCommands(ProjectModel& model, Engine& engine)
	{
		const Json boolean = {{"type", "boolean"}};
		const Json integer = {{"type", "integer"}};
		const Json typeSchema = {{"type", "string"},
			{"enum", std::vector<std::string>(std::begin(g_snapTypeNames), std::end(g_snapTypeNames))}};
		const Json gridSchema = {{"type", "string"},
			{"enum", std::vector<std::string>(std::begin(g_gridTypeNames), std::end(g_gridTypeNames))}};
		const Json settingsProperties = {
			{"on", boolean}, {"type", typeSchema}, {"grid", gridSchema}, {"zeroCrossing", boolean}};
		const Json settingsSchema = {{"type", "object"}, {"properties", settingsProperties},
			{"required", {"on", "type", "grid", "zeroCrossing"}}};
		const std::string settingsText =
			"on: whether snap is on. type: grid (the nearest grid line), gridRelative (moves in grid steps, keeping "
			"the distance to the grid), events (the starts and ends of other events, audio events' snap points and "
			"markers, when near), shuffle (events trade places with their neighbours, see event.shuffle), cursor "
			"(the project cursor, when near), or the combinations gridCursor, eventsCursor and eventsGridCursor. "
			"grid: bar, beat, quantize (the project's quantize grid) or adaptToZoom (a note value that the view "
			"chooses); 1ms, 10ms, 100ms, 1000ms; subframe, quarterFrame, halfFrame, frame, twoFrames, second; "
			"1sample, 10samples, 100samples, 44100samples. zeroCrossing: splitting and resizing audio events in the "
			"project window cut where the audio crosses zero.";

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
				"grids' rate (default 25). Returns position in project samples, and ticks, which are exact when "
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
				SnapGrid grid = snapGrid(project);
				grid.zoomStep = parameters.value("zoomStep", time::Ticks{0});
				const int fps = parameters.value("framesPerSecond", 25);
				require(fps >= 1 && fps <= 1000, "framesPerSecond must be 1 to 1000");
				grid.frameRate = {fps, false, false};
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
					if (edited.count(id) != 0)
						continue;
					const EventSpan span = eventSpan(project, event);
					target.magnets.push_back(span.start);
					target.magnets.push_back(span.end);
					if (event.snapPoint && *event.snapPoint >= event.offset &&
						*event.snapPoint < event.offset + event.length)
						target.magnets.push_back(span.start +
							std::llround(static_cast<double>(*event.snapPoint - event.offset) * span.ratio));
				}
				for (const auto& [id, part] : project.midiParts)
					if (edited.count(id) == 0)
					{
						target.magnets.push_back(time::toSamples(part.position, project.tempo, project.sampleRate));
						target.magnets.push_back(
							time::toSamples({part.position.base, part.position.value + part.length}, project.tempo,
								project.sampleRate));
					}
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
	}
}
