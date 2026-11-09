// SPDX-License-Identifier: GPL-3.0-or-later
// Tempo matching and warping (#144, reference manual pp. 648-658): a clip's warp settings are in the clip, so each
// command is one undo step and changes nothing if it fails (ProjectModel::addEdit). Playback warps in real time on the
// disk threads (dsp::WarpReader); flattening renders the same into a new file on the command thread.
#include "engine/document/warp.h"

#include "engine/audio/playback.h"
#include "engine/audiofile/audiofile.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/media/backup.h"
#include "engine/time/tempomap.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		namespace fs = std::filesystem;

		constexpr double g_mapStep = 4096.0;
		constexpr double g_maxTempo = 999.0;

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		Json objectSchema(Json properties, const std::vector<std::string>& required = {})
		{
			return {{"type", "object"}, {"properties", std::move(properties)}, {"required", required},
				{"additionalProperties", false}};
		}

		const MediaFile& mediaOf(const Project& project, const Clip& clip)
		{
			return project.media.at(clip.media);
		}

		Clip& clipOf(Project& project, const Json& parameters)
		{
			const auto id = parameters.at("clip").get<ObjectId>();
			const auto found = project.clips.find(id);
			require(found != project.clips.end(), "no clip " + std::to_string(id));
			return found->second;
		}

		// The free warp markers as a map from clip frames to the clip frames they play at.
		std::vector<dsp::WarpPoint> markerMap(const ClipWarp& warp, double length)
		{
			std::vector<dsp::WarpPoint> map{{0.0, 0.0}};
			for (const WarpMarker& marker : warp.markers)
				if (static_cast<double>(marker.position) < length && static_cast<double>(marker.target) < length)
					map.push_back({static_cast<double>(marker.position), static_cast<double>(marker.target)});
			map.push_back({length, length});
			return map;
		}

		// The definition grid: clip frames to beats from the grid start, linear between the beats edited and at the
		// clip's tempo beyond them.
		class Definition
		{
		public:
			Definition(const ClipWarp& warp, double tempo, double sampleRate)
				: m_framesPerBeat(sampleRate * 60.0 / tempo)
			{
				m_anchors.push_back({static_cast<double>(warp.gridStart), 0.0});
				for (const BeatEdit& edit : warp.beats)
					m_anchors.push_back({static_cast<double>(edit.frame), static_cast<double>(edit.beat)});
			}

			double beatAt(double frame) const
			{
				if (frame <= m_anchors.front().source || m_anchors.size() == 1)
					return m_anchors.front().target + ((frame - m_anchors.front().source) / m_framesPerBeat);
				if (frame >= m_anchors.back().source)
					return m_anchors.back().target + ((frame - m_anchors.back().source) / m_framesPerBeat);
				return dsp::warpTarget(m_anchors, frame);
			}

			double frameAt(double beat) const
			{
				if (beat <= m_anchors.front().target || m_anchors.size() == 1)
					return m_anchors.front().source + ((beat - m_anchors.front().target) * m_framesPerBeat);
				if (beat >= m_anchors.back().target)
					return m_anchors.back().source + ((beat - m_anchors.back().target) * m_framesPerBeat);
				return dsp::warpSource(m_anchors, beat);
			}

			const std::vector<dsp::WarpPoint>& anchors() const { return m_anchors; }

		private:
			double m_framesPerBeat;
			std::vector<dsp::WarpPoint> m_anchors; // source: the clip frame, target: the beat
		};

		// Where an event's clip frames play, relative to the event's start.
		class EventWarp
		{
		public:
			EventWarp(const Project& project, const AudioEvent& event)
				: m_project(project), m_clip(project.clips.at(event.clip)),
				  m_markers(markerMap(m_clip.warp, static_cast<double>(mediaOf(project, m_clip).length))),
				  m_definition(m_clip.warp, definitionTempo(project, m_clip), mediaOf(project, m_clip).sampleRate),
				  m_ratio(project.sampleRate / mediaOf(project, m_clip).sampleRate)
			{
				const double offset = warpedFrame(static_cast<double>(event.offset));
				m_startTicks = static_cast<double>(time::toTicks(event.position, project.tempo, project.sampleRate));
				m_startBeat = m_definition.beatAt(offset);
				m_startFrame = offset;
				m_startSamples = samplesAt(0.0);
			}

			double warpedFrame(double frame) const { return dsp::warpTarget(m_markers, frame); }

			// Project samples after the event's start.
			double offset(double frame) const
			{
				const double warpedAt = warpedFrame(frame);
				if (!m_clip.warp.musical)
					return (warpedAt - m_startFrame) * m_ratio;
				return samplesAt(m_definition.beatAt(warpedAt) - m_startBeat) - m_startSamples;
			}

			// The clip frames to sample the map at: those of the markers and edited beats among them.
			std::vector<double> breaks() const
			{
				std::vector<double> result;
				result.reserve(m_markers.size() + m_definition.anchors().size());
				for (const dsp::WarpPoint& point : m_markers)
					result.push_back(point.source);
				if (m_clip.warp.musical)
					for (const dsp::WarpPoint& anchor : m_definition.anchors())
						result.push_back(dsp::warpSource(m_markers, anchor.source));
				return result;
			}

			double ratio() const { return m_ratio; }

		private:
			double samplesAt(double beats) const
			{
				const auto ticks = std::llround(m_startTicks + (beats * static_cast<double>(time::g_ticksPerQuarter)));
				return m_project.tempo.toSeconds(ticks) * m_project.sampleRate;
			}

			const Project& m_project;
			const Clip& m_clip;
			std::vector<dsp::WarpPoint> m_markers;
			Definition m_definition;
			double m_ratio;
			double m_startTicks = 0.0;
			double m_startBeat = 0.0;
			double m_startFrame = 0.0;
			double m_startSamples = 0.0;
		};

		Json warpResult(const Clip& clip, CommandContext& context)
		{
			Json json = toJson(clip);
			Json warp = json.at("warp");
			context.emit("clip.changed", {{"clip", std::move(json)}});
			return {{"clip", clip.id}, {"warp", std::move(warp)}};
		}

		// Gives a clip without a tempo one, for its definition grid.
		void defineTempo(const Project& project, Clip& clip)
		{
			if (clip.warp.tempo <= 0.0)
				clip.warp.tempo = definitionTempo(project, clip);
		}

		// Checks that the beats edited come one after the other in time, after the grid start.
		void checkBeats(const ClipWarp& warp, time::Samples length)
		{
			time::Samples frame = warp.gridStart;
			int64_t beat = 0;
			for (const BeatEdit& edit : warp.beats)
			{
				require(edit.beat > beat && edit.frame > frame,
					"beat " + std::to_string(edit.beat) + " must come after beat " + std::to_string(beat) +
						" and the grid start");
				beat = edit.beat;
				frame = edit.frame;
			}
			require(warp.gridStart >= 0 && warp.gridStart < length && frame < length,
				"the grid must be within the clip (0 to " + std::to_string(length - 1) + ")");
		}

		void sortBeats(ClipWarp& warp)
		{
			std::sort(warp.beats.begin(), warp.beats.end(),
				[](const BeatEdit& a, const BeatEdit& b) { return a.beat < b.beat; });
		}

		// Real-time warping stretches audio up to 4 times and squeezes it to a quarter (dsp::WarpReader): no section
		// between two markers, or a marker and the clip's end, may go beyond.
		void checkRatio(const WarpMarker& from, const WarpMarker& to)
		{
			const double ratio =
				static_cast<double>(to.target - from.target) / static_cast<double>(to.position - from.position);
			require(ratio >= dsp::g_minRealtimeRatio && ratio <= dsp::g_maxRealtimeRatio,
				"warp markers can stretch the audio up to 4 times as long and squeeze it to a quarter: the audio "
				"from " +
					std::to_string(from.position) + " to " + std::to_string(to.position) + " would play " +
					std::to_string(ratio) + " times as long");
		}

		void checkMarkers(ClipWarp& warp, time::Samples length)
		{
			std::sort(warp.markers.begin(), warp.markers.end(),
				[](const WarpMarker& a, const WarpMarker& b) { return a.position < b.position; });
			WarpMarker before;
			for (const WarpMarker& marker : warp.markers)
			{
				require(marker.position > 0 && marker.position < length && marker.target > 0 && marker.target < length,
					"a warp marker and where it goes must be within the clip (1 to " + std::to_string(length - 1) +
						")");
				require(marker.position > before.position && marker.target > before.target,
					"warp markers can't pass each other: the marker at " + std::to_string(marker.position) +
						" must go after " + std::to_string(before.target));
				checkRatio(before, marker);
				before = marker;
			}
			checkRatio(before, {length, length});
		}

		WarpMarker& markerAt(ClipWarp& warp, time::Samples position)
		{
			const auto found = std::find_if(warp.markers.begin(), warp.markers.end(),
				[position](const WarpMarker& marker) { return marker.position == position; });
			require(found != warp.markers.end(), "no warp marker at " + std::to_string(position));
			return *found;
		}

		std::vector<ObjectId> eventList(const Project& project, const Json& parameters)
		{
			std::vector<ObjectId> events;
			for (const Json& id : parameters.at("events"))
			{
				const auto event = id.get<ObjectId>();
				require(project.audioEvents.count(event) != 0, "no audio event " + std::to_string(event));
				if (std::find(events.begin(), events.end(), event) == events.end())
					events.push_back(event);
			}
			return events;
		}

		// A file in folder named after name that isn't there yet.
		fs::path newFile(const fs::path& folder, const std::string& name)
		{
			fs::path path = folder / fs::u8path(name + ".wav");
			std::error_code error;
			for (int n = 2; fs::exists(path, error); ++n)
				path = folder / fs::u8path(name + " " + std::to_string(n) + ".wav");
			return path;
		}

		// Renders what the event plays of its clip, warped as it plays and with the clip's fades where playback puts
		// them (warpedClipEdges), into a 32-bit float WAV file.
		AudioFileInfo renderWarped(const MediaFile& media, const Clip& clip, std::vector<dsp::WarpPoint> map,
			dsp::StretchPreset preset, const fs::path& path)
		{
			const WarpedClipEdges edges = warpedClipEdges(clip, media.length, map);
			const GainRamp fadeIn{edges.start, edges.fadeInEnd, clip.fadeIn.shape, false};
			const GainRamp fadeOut{edges.fadeOutStart, edges.end, clip.fadeOut.shape, true};
			dsp::WarpReader reader(AudioFileReader::open(media.path), std::move(map), preset);
			const AudioFileInfo& info = reader.info();
			fs::create_directories(path.parent_path());
			WriteOptions options;
			options.sampleFormat = SampleFormat::Float32;
			options.sampleRate = info.sampleRate;
			options.channels = info.channels;
			const std::unique_ptr<AudioFileWriter> writer = AudioFileWriter::create(path.u8string(), options);
			constexpr size_t block = 4096;
			std::vector<float> buffer(block * static_cast<size_t>(info.channels));
			const auto channels = static_cast<size_t>(info.channels);
			time::Samples left = info.frames;
			while (left > 0)
			{
				const size_t got = reader.read(buffer.data(), std::min<size_t>(block, static_cast<size_t>(left)));
				if (got == 0)
					break;
				const time::Samples at = info.frames - left;
				for (size_t f = 0; f < got; ++f)
				{
					const time::Samples frame = at + static_cast<time::Samples>(f);
					double gain = 1.0;
					if (edges.fadeInEnd > edges.start)
						gain *= fadeIn.gainAt(frame);
					if (edges.end > edges.fadeOutStart)
						gain *= fadeOut.gainAt(frame);
					for (size_t c = 0; c < channels; ++c)
						buffer[(f * channels) + c] *= static_cast<float>(gain);
				}
				writer->write(buffer.data(), got);
				left -= static_cast<time::Samples>(got);
			}
			writer->close();
			AudioFileInfo written = info;
			written.frames = info.frames - left;
			return written;
		}
	}

	bool warped(const Clip& clip)
	{
		return clip.warp.musical || !clip.warp.markers.empty();
	}

	double definitionTempo(const Project& project, const Clip& clip)
	{
		if (clip.warp.tempo > 0.0)
			return clip.warp.tempo;
		std::optional<time::Ticks> first;
		for (const auto& [id, event] : project.audioEvents)
			if (event.clip == clip.id)
			{
				const time::Ticks at = time::toTicks(event.position, project.tempo, project.sampleRate);
				first = std::min(first.value_or(at), at);
			}
		return project.tempo.bpmAt(first.value_or(0));
	}

	double warpedOffset(const Project& project, const AudioEvent& event, double frame)
	{
		return EventWarp(project, event).offset(frame);
	}

	double warpedFrameAt(const Project& project, const AudioEvent& event, double offset)
	{
		// The offset grows with the frame: bracket it, widening from the event, then halve the bracket.
		const EventWarp warp(project, event);
		auto low = static_cast<double>(event.offset);
		double high = low;
		double step = std::max(1.0, static_cast<double>(event.length));
		while (warp.offset(high) < offset && step < 1e15)
		{
			high += step;
			step *= 2.0;
		}
		step = std::max(1.0, static_cast<double>(event.length));
		while (warp.offset(low) > offset && step < 1e15)
		{
			low -= step;
			step *= 2.0;
		}
		for (int i = 0; i < 100 && high - low > 1e-6; ++i)
		{
			const double middle = (low + high) / 2.0;
			if (warp.offset(middle) < offset)
				low = middle;
			else
				high = middle;
		}
		return (low + high) / 2.0;
	}

	std::vector<dsp::WarpPoint> warpMap(const Project& project, const AudioEvent& event)
	{
		const EventWarp warp(project, event);
		const auto start = static_cast<double>(event.offset);
		const double end = start + static_cast<double>(event.length);
		std::vector<double> frames;
		for (int64_t step = 0; start + (static_cast<double>(step) * g_mapStep) < end; ++step)
			frames.push_back(start + (static_cast<double>(step) * g_mapStep));
		frames.push_back(end);
		for (const double at : warp.breaks())
			if (at > start && at < end)
				frames.push_back(at);
		std::sort(frames.begin(), frames.end());
		std::vector<dsp::WarpPoint> map;
		for (const double frame : frames)
		{
			const double target = warp.offset(frame) / warp.ratio();
			if (map.empty() || (frame > map.back().source && target > map.back().target))
				map.push_back({frame, target});
		}
		if (map.size() < 2) // an event without length
			map.push_back({end + 1.0, 1.0});
		return map;
	}

	WarpedClipEdges warpedClipEdges(const Clip& clip, time::Samples length, const std::vector<dsp::WarpPoint>& map)
	{
		const auto at = [&map](time::Samples frame)
		{ return static_cast<time::Samples>(std::llround(dsp::warpTarget(map, static_cast<double>(frame)))); };
		return {at(0), at(clip.fadeIn.length), at(length - clip.fadeOut.length), at(length)};
	}

	void ProjectModel::registerWarpCommands(Engine& engine)
	{
		const Json integer = {{"type", "integer"}};
		const Json number = {{"type", "number"}};
		const Json boolean = {{"type", "boolean"}};
		const Json preset = {{"type", "string"}, {"enum", dsp::stretchPresetNames()}};
		const Json ids = {{"type", "array"}, {"items", integer}};
		const Json warpSchema = objectSchema(
			{{"musical", boolean}, {"tempo", number}, {"beatsPerBar", integer}, {"gridStart", integer},
				{"beats",
					{{"type", "array"},
						{"items", objectSchema({{"beat", integer}, {"frame", integer}}, {"beat", "frame"})}}},
				{"markers",
					{{"type", "array"},
						{"items", objectSchema({{"position", integer}, {"target", integer}}, {"position", "target"})}}},
				{"preset", preset}},
			{"musical", "tempo", "beatsPerBar", "gridStart", "beats", "markers", "preset"});
		const Json resultSchema = objectSchema({{"clip", integer}, {"warp", warpSchema}}, {"clip", "warp"});
		const std::string warpText =
			"A clip's warp has musical (musical mode: its beats follow the project's tempo), its definition: tempo "
			"(BPM of quarter notes, 0 until one is set), beatsPerBar, gridStart (the clip frame of beat 0) and beats "
			"(the beats, counted in quarters from the grid start, that Auto Adjust or Manual Adjust put elsewhere "
			"than the tempo would, each at a clip frame), its free warp markers (each plays the audio at its "
			"position where the clip frame target would play without it) and preset, the time stretch algorithm of "
			"real-time warping. Emits clip.changed. Returns the clip's id and its warp.";

		addEdit(engine,
			{"clip.setWarp", 1,
				"Changes a clip's musical mode and definition (the Definition and Warp sections of the sample "
				"editor, the pool's Musical Mode column, reference manual pp. 616, 648-650): musical turns musical "
				"mode on or off, which in a clip without a tempo takes the project's at the start of its first event "
				"so that nothing changes until the tempo does; tempo (1-999 BPM); bars and beats, its length from the "
				"grid start to its end in bars and quarters, which sets the tempo that makes it that long; "
				"beatsPerBar (1-64); gridStart (a clip frame; the edited beats move with it); preset (the algorithm "
				"preset of real-time warping). What isn't given stays. " +
					warpText,
				objectSchema(
					{{"clip", integer}, {"musical", boolean}, {"tempo", number}, {"bars", integer}, {"beats", integer},
						{"beatsPerBar", integer}, {"gridStart", integer}, {"preset", preset}},
					{"clip"}),
				resultSchema, false},
			"Set musical mode",
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				Clip& clip = clipOf(project, parameters);
				ClipWarp& warp = clip.warp;
				const time::Samples length = mediaOf(project, clip).length;
				warp.beatsPerBar = parameters.value("beatsPerBar", warp.beatsPerBar);
				require(warp.beatsPerBar >= 1 && warp.beatsPerBar <= 64, "beatsPerBar must be from 1 to 64");
				if (parameters.contains("gridStart"))
				{
					const auto start = parameters.at("gridStart").get<time::Samples>();
					for (BeatEdit& edit : warp.beats)
						edit.frame += start - warp.gridStart;
					warp.gridStart = start;
				}
				warp.preset = parameters.value("preset", warp.preset);
				if (parameters.contains("tempo"))
				{
					warp.tempo = parameters.at("tempo").get<double>();
					require(warp.tempo >= 1.0 && warp.tempo <= g_maxTempo, "tempo must be from 1 to 999 BPM");
				}
				if (parameters.contains("bars") || parameters.contains("beats"))
				{
					const int64_t quarters = (parameters.value("bars", int64_t{0}) * warp.beatsPerBar) +
						parameters.value("beats", int64_t{0});
					require(quarters > 0, "bars and beats must make a length");
					warp.tempo = static_cast<double>(quarters) * 60.0 * mediaOf(project, clip).sampleRate /
						static_cast<double>(length - warp.gridStart);
					require(warp.tempo >= 1.0 && warp.tempo <= g_maxTempo,
						"that length makes a tempo of " + std::to_string(warp.tempo) + " BPM, not 1 to 999");
					warp.beats.clear();
				}
				warp.musical = parameters.value("musical", warp.musical);
				if (warp.musical)
					defineTempo(project, clip);
				checkBeats(warp, length);
				return warpResult(clip, context);
			});

		addEdit(engine,
			{"clip.autoAdjust", 1,
				"Extracts a clip's definition grid from its hitpoints (Auto Adjust, reference manual pp. 651-652): "
				"the section from start to end (clip frames; default the whole clip) is taken to start and end at a "
				"barline and to be bars bars long (default: the whole bars nearest its length at the clip's tempo, or "
				"the project's without one). That makes the clip's tempo and grid start; every beat with a hitpoint "
				"that counts within a quarter beat of it is then edited to be at that hitpoint. The snap point of "
				"event, "
				"an audio event of the clip such as the one the sample editor shows, moves to the start of the "
				"section (emitting audioEvent.changed). Fails without such hitpoints. " +
					warpText,
				objectSchema(
					{{"clip", integer}, {"event", integer}, {"start", integer}, {"end", integer}, {"bars", integer}},
					{"clip"}),
				resultSchema, false},
			"Auto adjust",
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				Clip& clip = clipOf(project, parameters);
				const MediaFile& media = mediaOf(project, clip);
				const auto start = parameters.value("start", time::Samples{0});
				const auto end = parameters.value("end", media.length);
				require(start >= 0 && start < end && end <= media.length,
					"the section must be within the clip (0 to " + std::to_string(media.length) + ")");
				const std::vector<bool> active = activeHitpoints(clip);
				std::vector<time::Samples> hits;
				for (size_t i = 0; i < clip.hitpoints.size(); ++i)
					if (active[i] && clip.hitpoints[i].position >= start && clip.hitpoints[i].position < end)
						hits.push_back(clip.hitpoints[i].position);
				require(!hits.empty(), "the clip has no hitpoints there: detect them first (clip.detectHitpoints)");
				ClipWarp& warp = clip.warp;
				const auto frames = static_cast<double>(end - start);
				const double tempo = definitionTempo(project, clip);
				const int64_t bars = parameters.value("bars",
					std::max<int64_t>(1,
						std::llround(
							frames * tempo / 60.0 / media.sampleRate / static_cast<double>(warp.beatsPerBar))));
				require(bars >= 1, "bars must be 1 or more");
				const int64_t quarters = bars * warp.beatsPerBar;
				const double beatFrames = frames / static_cast<double>(quarters);
				warp.tempo = media.sampleRate * 60.0 / beatFrames;
				require(warp.tempo >= 1.0 && warp.tempo <= g_maxTempo,
					"that makes a tempo of " + std::to_string(warp.tempo) + " BPM, not 1 to 999");
				warp.gridStart = start;
				warp.beats.clear();
				for (int64_t beat = 1; beat < quarters; ++beat)
				{
					const double expected = static_cast<double>(start) + (static_cast<double>(beat) * beatFrames);
					const auto nearest = std::min_element(hits.begin(), hits.end(),
						[expected](time::Samples a, time::Samples b)
						{
							return std::abs(static_cast<double>(a) - expected) <
								std::abs(static_cast<double>(b) - expected);
						});
					if (std::abs(static_cast<double>(*nearest) - expected) < beatFrames / 4.0 &&
						*nearest != std::llround(expected) &&
						(warp.beats.empty() || *nearest > warp.beats.back().frame))
						warp.beats.push_back({beat, *nearest});
				}
				checkBeats(warp, media.length);
				if (parameters.contains("event"))
				{
					const auto id = parameters.at("event").get<ObjectId>();
					const auto event = project.audioEvents.find(id);
					require(event != project.audioEvents.end() && event->second.clip == clip.id,
						"no audio event " + std::to_string(id) + " of clip " + std::to_string(clip.id));
					event->second.snapPoint = start;
					context.emit("audioEvent.changed", {{"event", toJson(event->second)}});
				}
				return warpResult(clip, context);
			});

		addEdit(engine,
			{"clip.adjustGrid", 1,
				"Corrects a clip's definition grid by hand (Manual Adjust, reference manual pp. 652-654), moving a "
				"line of the grid to the clip frame frame. mode start: the grid start, and the beats edited with it "
				"(Set Grid Start); stretch: makes beat beat (default the second bar's first) be at frame by changing "
				"the tempo, dropping the beats edited, so that every bar is as long (Stretch Grid); bar: puts beat "
				"beat at frame and the beats edited after it with it (Set Bar Position, Move Following Bars); beat: "
				"puts beat beat at frame alone (Set Beat Position); remove: drops the edit of beat beat (Remove Beat "
				"Edit). A clip without a tempo gets the project's first. " +
					warpText,
				objectSchema(
					{{"clip", integer},
						{"mode", {{"type", "string"}, {"enum", {"start", "stretch", "bar", "beat", "remove"}}}},
						{"beat", integer}, {"frame", integer}},
					{"clip", "mode"}),
				resultSchema, false},
			"Adjust grid",
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				Clip& clip = clipOf(project, parameters);
				const MediaFile& media = mediaOf(project, clip);
				ClipWarp& warp = clip.warp;
				defineTempo(project, clip);
				const std::string mode = parameters.at("mode").get<std::string>();
				const int64_t beat = parameters.value("beat", int64_t{warp.beatsPerBar});
				require(mode == "start" || beat >= 1, "beat must be 1 or more");
				require(mode == "remove" || parameters.contains("frame"), "give the frame to move the line to");
				const auto frame = parameters.value("frame", time::Samples{0});
				const auto edit = std::find_if(
					warp.beats.begin(), warp.beats.end(), [beat](const BeatEdit& e) { return e.beat == beat; });
				if (mode == "start")
				{
					for (BeatEdit& each : warp.beats)
						each.frame += frame - warp.gridStart;
					warp.gridStart = frame;
				}
				else if (mode == "stretch")
				{
					require(frame > warp.gridStart, "the beat must be after the grid start");
					warp.tempo = static_cast<double>(beat) * 60.0 * media.sampleRate /
						static_cast<double>(frame - warp.gridStart);
					require(warp.tempo >= 1.0 && warp.tempo <= g_maxTempo,
						"that makes a tempo of " + std::to_string(warp.tempo) + " BPM, not 1 to 999");
					warp.beats.clear();
				}
				else if (mode == "bar")
				{
					const Definition definition(warp, warp.tempo, media.sampleRate);
					const time::Samples by = frame - std::llround(definition.frameAt(static_cast<double>(beat)));
					for (BeatEdit& each : warp.beats)
						if (each.beat > beat)
							each.frame += by;
					if (edit != warp.beats.end())
						edit->frame = frame;
					else
						warp.beats.push_back({beat, frame});
				}
				else if (mode == "beat")
				{
					if (edit != warp.beats.end())
						edit->frame = frame;
					else
						warp.beats.push_back({beat, frame});
				}
				else
				{
					require(edit != warp.beats.end(), "beat " + std::to_string(beat) + " isn't edited");
					warp.beats.erase(edit);
				}
				sortBeats(warp);
				checkBeats(warp, media.length);
				return warpResult(clip, context);
			});

		addEdit(engine,
			{"clip.addWarpMarker", 1,
				"Inserts a free warp marker into a clip at position (a clip frame) that plays it at target, default "
				"position (Free Warp, reference manual pp. 654-655). Markers can't pass each other. " +
					warpText,
				objectSchema({{"clip", integer}, {"position", integer}, {"target", integer}}, {"clip", "position"}),
				resultSchema, false},
			"Insert warp marker",
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				Clip& clip = clipOf(project, parameters);
				const auto position = parameters.at("position").get<time::Samples>();
				require(std::none_of(clip.warp.markers.begin(), clip.warp.markers.end(),
							[position](const WarpMarker& marker) { return marker.position == position; }),
					"there is a warp marker at " + std::to_string(position) + " already");
				clip.warp.markers.push_back({position, parameters.value("target", position)});
				checkMarkers(clip.warp, mediaOf(project, clip).length);
				return warpResult(clip, context);
			});

		addEdit(engine,
			{"clip.setWarpMarker", 1,
				"Moves the free warp marker of a clip at position so that it plays at target, a clip frame "
				"(Correcting Warp Marker Positions, reference manual p. 656). Markers can't pass each other. " +
					warpText,
				objectSchema(
					{{"clip", integer}, {"position", integer}, {"target", integer}}, {"clip", "position", "target"}),
				resultSchema, false},
			"Move warp marker",
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				Clip& clip = clipOf(project, parameters);
				markerAt(clip.warp, parameters.at("position").get<time::Samples>()).target =
					parameters.at("target").get<time::Samples>();
				checkMarkers(clip.warp, mediaOf(project, clip).length);
				return warpResult(clip, context);
			});

		addEdit(engine,
			{"clip.removeWarpMarkers", 1,
				"Removes the free warp markers of a clip at positions (Deleting Warp Markers, reference manual p. "
				"656), or without positions all of them, which resets the free warp edits and leaves musical mode "
				"and the definition as they are (Reset, p. 656). " +
					warpText,
				objectSchema({{"clip", integer}, {"positions", ids}}, {"clip"}), resultSchema, false},
			"Remove warp markers",
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				Clip& clip = clipOf(project, parameters);
				if (!parameters.contains("positions"))
					clip.warp.markers.clear();
				for (const Json& position : parameters.value("positions", Json::array()))
				{
					const WarpMarker& marker = markerAt(clip.warp, position.get<time::Samples>());
					clip.warp.markers.erase(clip.warp.markers.begin() + (&marker - clip.warp.markers.data()));
				}
				return warpResult(clip, context);
			});

		addEdit(engine,
			{"event.unstretch", 1,
				"Removes all real-time stretching from the clips of audio events (Unstretch Audio, reference manual "
				"pp. 657-658): turns musical mode off and removes their free warp markers, keeping their "
				"definitions. Every event of those clips plays its audio unstretched again. Emits clip.changed for "
				"each clip that changes. Returns the clips changed.",
				objectSchema({{"events", ids}}, {"events"}), objectSchema({{"clips", ids}}, {"clips"}), false},
			"Unstretch audio",
			[](Project& project, CommandContext& context)
			{
				Json clips = Json::array();
				for (const ObjectId id : eventList(project, context.parameters()))
				{
					Clip& clip = project.clips.at(project.audioEvents.at(id).clip);
					if (!warped(clip))
						continue;
					clip.warp.musical = false;
					clip.warp.markers.clear();
					context.emit("clip.changed", {{"clip", toJson(clip)}});
					clips.push_back(clip.id);
				}
				return Json{{"clips", std::move(clips)}};
			});

		addEdit(engine,
			{"event.flatten", 1,
				"Flattens the real-time processing of audio events (Flatten Real Time Processing, reference manual p. "
				"657): renders what each event of a warped clip plays, as it plays it now, into a 32-bit float WAV "
				"file in the project's media folder (for a project without a file, the folder its recordings go to) "
				"with the algorithm preset (default the clip's), named after the clip with Flattened, and makes the "
				"event play a new clip of it from its start, with its volume curve and snap point where its audio "
				"is. It then no longer follows tempo changes or warp edits. Events of clips that aren't warped stay; "
				"fails if none is. Emits media.added, clip.added and audioEvent.changed. Returns the events "
				"flattened and their new media files and clips.",
				objectSchema({{"events", ids}, {"preset", preset}}, {"events"}),
				objectSchema({{"events", ids}, {"media", ids}, {"clips", ids}}, {"events", "media", "clips"}), false},
			"Flatten real-time processing",
			[this](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				std::vector<ObjectId> events;
				for (const ObjectId id : eventList(project, parameters))
					if (warped(project.clips.at(project.audioEvents.at(id).clip)))
						events.push_back(id);
				require(!events.empty(), "none of the events plays a warped clip");
				const fs::path folder =
					m_file.empty() ? unsavedMediaFolder(m_settingsDirectory) : mediaFolderFor(fs::u8path(m_file));
				Json flattened = Json::array();
				Json media = Json::array();
				Json clips = Json::array();
				// A failure undoes the command: the files rendered before it go too.
				std::vector<fs::path> written;
				const auto fail = [&written](const char* message)
				{
					for (const fs::path& each : written)
					{
						std::error_code error;
						fs::remove(each, error);
					}
					throw CommandError(message);
				};
				for (const ObjectId id : events)
				{
					AudioEvent& event = project.audioEvents.at(id);
					const Clip& source = project.clips.at(event.clip);
					const std::vector<dsp::WarpPoint> map = warpMap(project, event);
					const dsp::StretchPreset algorithm =
						dsp::stretchPresetFromString(parameters.value("preset", source.warp.preset))
							.value_or(dsp::StretchPreset::Quality);
					const fs::path path = newFile(folder, source.name + " Flattened");
					MediaFile file;
					written.push_back(path);
					try
					{
						const AudioFileInfo info = renderWarped(mediaOf(project, source), source, map, algorithm, path);
						file = {project.newId(), path.u8string(), info.sampleRate, info.channels, info.frames};
					}
					catch (const AudioFileError& error)
					{
						fail(error.what());
					}
					catch (const std::invalid_argument& error)
					{
						fail(error.what());
					}
					catch (const fs::filesystem_error& error)
					{
						fail(error.what());
					}
					project.media[file.id] = file;
					context.emit("media.added", {{"media", toJson(file)}});
					Clip clip;
					clip.id = project.newId();
					clip.name = source.name + " Flattened";
					clip.media = file.id;
					clip.folder = source.folder;
					const auto moved = [&map](time::Samples frame)
					{ return std::llround(dsp::warpTarget(map, static_cast<double>(frame))); };
					for (VolumePoint& point : event.volumeCurve)
						point.position = moved(point.position);
					if (event.snapPoint)
						event.snapPoint = moved(*event.snapPoint);
					// The event's fades play as long as they did, in frames of what it played (warpEvent); the clip's
					// are in the file.
					event.fadeIn.length = std::min(event.fadeIn.length, file.length);
					event.fadeOut.length = std::min(event.fadeOut.length, file.length - event.fadeIn.length);
					event.clip = clip.id;
					event.offset = 0;
					event.length = file.length;
					project.clips[clip.id] = clip;
					context.emit("clip.added", {{"clip", toJson(clip)}});
					context.emit("audioEvent.changed", {{"event", toJson(event)}});
					flattened.push_back(id);
					media.push_back(file.id);
					clips.push_back(clip.id);
				}
				return Json{{"events", std::move(flattened)}, {"media", std::move(media)}, {"clips", std::move(clips)}};
			});
	}
}
