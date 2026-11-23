// SPDX-License-Identifier: GPL-3.0-or-later
// The audio functions (#147, reference manual pp. 603-609): Detect Silence, the Spectrum Analyzer and Statistics. They
// read the clip's file on job threads (src/engine/analysis/statistics.h). event.stripSilence makes its events
// and regions with the commands that make such things, run as part of it (ProjectModel::runEdit), so that it is one
// undo step.
#include "engine/document/audiofunctions.h"

#include "engine/analysis/analysis.h"
#include "engine/analysis/statistics.h"
#include "engine/audio/meters.h"
#include "engine/audiofile/audiofile.h"
#include "engine/document/channelformat.h"
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/engine.h"
#include "engine/jobs/jobs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		constexpr size_t g_readFrames = 65536;
		constexpr double g_minThreshold = -120.0; // dBFS

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

		Json orNull(const std::optional<double>& value)
		{
			return value ? Json(*value) : Json(nullptr);
		}

		Json decibels(double level)
		{
			return orNull(analysis::decibels(level));
		}

		const Clip& clipOf(const Project& project, const Json& parameters)
		{
			const auto id = parameters.at("clip").get<ObjectId>();
			const auto found = project.clips.find(id);
			require(found != project.clips.end(), "no clip " + std::to_string(id));
			return found->second;
		}

		struct Range
		{
			time::Samples start = 0;
			time::Samples end = 0;
		};

		// start and end in the clip's frames, by default the whole clip.
		Range rangeOf(const Project& project, const Clip& clip, const Json& parameters)
		{
			const time::Samples length = project.media.at(clip.media).length;
			const Range range = {parameters.value("start", time::Samples{0}), parameters.value("end", length)};
			require(range.start >= 0 && range.start < range.end && range.end <= length,
				"start and end must be within the clip (0 to " + std::to_string(length) + "), start before end");
			return range;
		}

		std::unique_ptr<AudioFileReader> openMedia(const MediaFile& media)
		{
			try
			{
				return AudioFileReader::open(media.path);
			}
			catch (const AudioFileError& error)
			{
				throw CommandError("can't read " + media.path + ": " + error.what());
			}
		}

		int channelsOf(const AudioFileReader& reader)
		{
			return std::max(reader.info().channels, 1);
		}

		// Reads the frames of a media file from range.start to range.end in blocks of interleaved frames: feed(frames,
		// count).
		template <typename Feed> void readRange(AudioFileReader& reader, const MediaFile& media, Range range,
			JobContext& job, Feed feed, double from = 0.0, double share = 1.0)
		{
			try
			{
				reader.seek(range.start);
				std::vector<float> buffer(g_readFrames * static_cast<size_t>(channelsOf(reader)));
				for (time::Samples at = range.start; at < range.end;)
				{
					job.checkCancelled();
					const auto wanted = static_cast<size_t>(std::min<time::Samples>(g_readFrames, range.end - at));
					const size_t read = reader.read(buffer.data(), wanted);
					require(read != 0, "file ends before the requested range");
					feed(buffer.data(), read);
					at += static_cast<time::Samples>(read);
					const double fraction =
						static_cast<double>(at - range.start) / static_cast<double>(range.end - range.start);
					job.progress(0.95 * (from + (share * fraction)), media.path);
				}
			}
			catch (const AudioFileError& error)
			{
				throw CommandError("can't read " + media.path + ": " + error.what());
			}
		}

		analysis::SilenceSettings silenceSettings(const Json& parameters)
		{
			analysis::SilenceSettings settings;
			settings.openThreshold = parameters.value("openThreshold", settings.openThreshold);
			settings.closeThreshold = parameters.value("closeThreshold", settings.closeThreshold);
			require(settings.openThreshold >= g_minThreshold && settings.openThreshold <= 0.0 &&
					settings.closeThreshold >= g_minThreshold && settings.closeThreshold <= 0.0,
				"openThreshold and closeThreshold must be from -120 to 0 dBFS");
			require(settings.closeThreshold <= settings.openThreshold,
				"closeThreshold must not be higher than openThreshold");
			const auto seconds = [&parameters](const char* name, double fallback)
			{
				const double milliseconds = parameters.value(name, fallback * 1000.0);
				require(milliseconds >= 0.0, std::string(name) + " must not be negative");
				return milliseconds / 1000.0;
			};
			settings.minimumOpen = seconds("minimumOpen", settings.minimumOpen);
			settings.minimumClosed = seconds("minimumClosed", settings.minimumClosed);
			settings.preRoll = seconds("preRoll", settings.preRoll);
			settings.postRoll = seconds("postRoll", settings.postRoll);
			return settings;
		}

		// The sections of a clip's range that aren't silent, in clip frames.
		std::vector<analysis::Section> detectSilence(const MediaFile& media, Range range,
			const analysis::SilenceSettings& settings, JobContext& job, double from = 0.0, double share = 1.0)
		{
			const std::unique_ptr<AudioFileReader> reader = openMedia(media);
			analysis::SilenceDetector detector(media.sampleRate, channelsOf(*reader), settings);
			readRange(
				*reader, media, range, job,
				[&detector](const float* frames, size_t count) { detector.feed(frames, count); }, from, share);
			std::vector<analysis::Section> sections = detector.finish();
			for (analysis::Section& section : sections)
			{
				section.start += range.start;
				section.end += range.start;
			}
			return sections;
		}

		Json sectionsJson(const std::vector<analysis::Section>& sections)
		{
			Json json = Json::array();
			for (const analysis::Section& section : sections)
				json.push_back({{"start", section.start}, {"end", section.end}});
			return json;
		}

		// Where a clip frame of an event plays, in project samples.
		time::Samples sampleOf(const Project& project, const AudioEvent& event, time::Samples frame)
		{
			const EventSpan span = eventSpan(project, event);
			return span.start + std::llround(eventSamples(project, event, static_cast<double>(frame - event.offset)));
		}

		const char* const g_settingsText =
			"A section opens at the first frame where a sample of any channel reaches openThreshold (dBFS, default "
			"-20) and closes after the last frame that reaches closeThreshold (dBFS, default -30, at most "
			"openThreshold) before 20 ms below it. Once open, a section stays open for at least minimumOpen (ms, "
			"default 10), joining what sounds within it; once closed, none opens before minimumClosed (ms, default "
			"100) has passed. preRoll and postRoll (ms, default 0) start and end the sections earlier and later, "
			"joining those that then overlap.";
	}

	void registerAudioFunctionCommands(ProjectModel& model, Engine& engine, Jobs& jobs)
	{
		const Json integer = {{"type", "integer"}};
		const Json number = {{"type", "number"}};
		const Json nullable = {{"type", {"number", "null"}}};
		const Json boolean = {{"type", "boolean"}};
		const Json string = {{"type", "string"}};
		const Json ids = {{"type", "array"}, {"items", integer}};
		const Json sectionsSchema = {
			{"type", "array"}, {"items", objectSchema({{"start", integer}, {"end", integer}}, {"start", "end"})}};
		const Json settingsProperties = {{"openThreshold", number}, {"closeThreshold", number}, {"minimumOpen", number},
			{"minimumClosed", number}, {"preRoll", number}, {"postRoll", number}};
		const auto rangeSchema = [&](const Json& more)
		{
			Json properties = {{"clip", integer}, {"start", integer}, {"end", integer}};
			properties.update(more);
			return objectSchema(std::move(properties), {"clip"});
		};
		const std::string rangeText = "The audio of clip from frame start to end (default: the whole clip).";

		// Copy the source on the command thread; only the job reads and measures its audio.
		const auto addAnalysis = [&](CommandInfo info, auto analyze)
		{
			std::string kind = info.name;
			Json resultSchema = info.result;
			info.version = kind == "clip.estimatePitch" ? 1 : 2;
			info.description +=
				" Returns a job at once; job.progress reports progress, job.cancel cancels it, "
				"and job.finished/job.get contain the measurements in job.result. The source is captured at start.";
			Json jobSchema = Jobs::schema();
			jobSchema["properties"]["result"] = info.result;
			jobSchema["properties"]["result"]["type"] = {"object", "null"};
			info.result = objectSchema({{"job", jobSchema}}, {"job"});
			engine.registerCommand(std::move(info),
				[&model, &jobs, kind = std::move(kind), analyze, resultSchema = std::move(resultSchema)](
					CommandContext& context)
				{
					const Json& parameters = context.parameters();
					const Project& project = model.project();
					const Clip& clip = clipOf(project, parameters);
					const Range range = rangeOf(project, clip, parameters);
					if (kind == "clip.detectSilence")
						silenceSettings(parameters);
					return Json{{"job",
						jobs.start(kind,
							[id = clip.id, media = project.media.at(clip.media), range, parameters = parameters,
								analyze, resultSchema = resultSchema](JobContext& job)
							{
								job.progress(0.0, media.path);
								job.checkCancelled();
								Json result = analyze(id, media, range, parameters, job);
								const std::string problem = validate(result, resultSchema, "result");
								require(problem.empty(), problem);
								job.checkCancelled();
								return result;
							})}};
				});
		};

		addAnalysis(
			{"clip.detectSilence", 1,
				"Finds the sections of a clip that aren't silent (the Detect Silence dialog's Analyze, reference "
				"manual pp. 603-606). " +
					rangeText + " " + g_settingsText +
					" Reads the clip's file. Returns the clip and its sections, start to end in clip frames, by start.",
				rangeSchema(settingsProperties), objectSchema({{"clip", integer}, {"sections", sectionsSchema}}), true},
			[](ObjectId clip, const MediaFile& media, Range range, const Json& parameters, JobContext& job)
			{
				return Json{{"clip", clip},
					{"sections", sectionsJson(detectSilence(media, range, silenceSettings(parameters), job))}};
			});

		Json stripProperties = settingsProperties;
		stripProperties.update({{"events", ids}, {"strip", boolean}, {"regions", boolean}, {"regionName", string},
			{"autoNumberStart", integer}, {"fades", number}, {"start", integer}, {"end", integer}});
		engine.registerCommand(
			{"event.stripSilence", 2,
				"Detects silence in events as a cancellable job with job.progress, then runs "
				"event.applySilenceSections "
				"on the command thread, one undo step. Uses clip.detectSilence's settings and "
				"event.applySilenceSections' strip, regions, regionName, autoNumberStart and fades options. "
				"Optional start/end clip frames limit processing to that range within each event, preserving audio "
				"outside it. Both must be supplied together. "
				"Returns a job; its result is the edit's result. Fails if the project changed meanwhile. " +
					std::string(g_settingsText),
				objectSchema(stripProperties, {"events"}), objectSchema({{"job", Jobs::schema()}}, {"job"}), false},
			[&model, &jobs](CommandContext& context)
			{
				Json parameters = context.parameters();
				const auto settings = silenceSettings(parameters);
				require(parameters.value("fades", 0.0) >= 0.0, "fades must not be negative");
				const Project& project = model.project();
				std::vector<std::pair<MediaFile, Range>> sources;
				std::vector<ObjectId> ids;
				for (const Json& item : parameters.at("events"))
				{
					const auto id = item.get<ObjectId>();
					const auto found = project.audioEvents.find(id);
					require(found != project.audioEvents.end(), "no audio event " + std::to_string(id));
					require(std::find(ids.begin(), ids.end(), id) == ids.end(), "duplicate audio event");
					ids.push_back(id);
					const AudioEvent& event = found->second;
					require(!parameters.value("strip", true) || !event.lock,
						"audio event " + std::to_string(id) + " is locked");
					require(parameters.contains("start") == parameters.contains("end"), "give both start and end");
					const Range range{
						parameters.value("start", event.offset), parameters.value("end", event.offset + event.length)};
					require(range.start >= event.offset && range.start < range.end &&
							range.end <= event.offset + event.length,
						"start and end must be within the event");
					sources.emplace_back(project.media.at(project.clips.at(event.clip).media), range);
				}
				require(!sources.empty(), "events must not be empty");
				parameters["revision"] = context.call("project.revision").at("revision");
				for (const char* key :
					{"openThreshold", "closeThreshold", "minimumOpen", "minimumClosed", "preRoll", "postRoll"})
					parameters.erase(key);
				return Json{{"job",
					jobs.start("event.stripSilence",
						[sources, settings, parameters](JobContext& job) mutable
						{
							job.progress(0.0, sources.front().first.path);
							job.checkCancelled();
							parameters["sections"] = Json::array();
							const double share = 1.0 / static_cast<double>(sources.size());
							for (size_t i = 0; i < sources.size(); ++i)
								parameters["sections"].push_back(sectionsJson(detectSilence(sources[i].first,
									sources[i].second, settings, job, static_cast<double>(i) * share, share)));
							job.checkCancelled();
							return job.command("event.applySilenceSections", parameters);
						})}};
			});
		const Json applyProperties = {{"events", ids}, {"strip", boolean}, {"regions", boolean}, {"regionName", string},
			{"autoNumberStart", integer}, {"fades", number}, {"revision", integer}, {"start", integer},
			{"end", integer}, {"sections", {{"type", "array"}, {"items", sectionsSchema}}}};
		model.addEdit(engine,
			{"event.applySilenceSections", 1,
				"Processes the sections of audio events that aren't silent (the Detect Silence dialog's Process, "
				"reference manual pp. 603-606), as clip.detectSilence finds them in the section of the clip each "
				"plays. Takes sections already measured, one array per event in the same order, and revision "
				"from project.revision; fails if the project changed meanwhile. Does no file I/O. "
				" strip (Strip Silence, default true) splits each event at the sections' edges and removes the "
				"silent parts between them; regions (Add as Regions) adds a region of the clip for each section, "
				"named regionName (default Region) and a number from autoNumberStart (default 1) up, across the "
				"events; fades (Apply Fades, ms) gives the events that remain a fade in and a fade out of that "
				"length, at most half of what each plays for in frames at the clip's sample rate (its length, "
				"unless it plays warped). An event that strip would remove completely, or that is locked, is an "
				"error. Optional start/end clip frames bound processing within each event, as in event.stripSilence; "
				"sections must lie within that range. Returns the events that remain and the regions, and the number "
				"of sections.",
				objectSchema(applyProperties, {"events", "sections", "revision"}),
				objectSchema(
					{{"events", ids}, {"regions", ids}, {"sections", integer}}, {"events", "regions", "sections"}),
				false},
			"Detect silence",
			[&model](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				require(context.call("project.revision").at("revision") == parameters.at("revision"),
					"project changed meanwhile; analyze again");
				require(parameters.at("sections").size() == parameters.at("events").size(),
					"sections must have one array per event");
				size_t index = 0;
				const bool strip = parameters.value("strip", true);
				const bool regions = parameters.value("regions", false);
				const std::string name = parameters.value("regionName", std::string("Region"));
				int64_t number = parameters.value("autoNumberStart", int64_t{1});
				const double fades = parameters.value("fades", 0.0);
				require(fades >= 0.0, "fades must not be negative");
				const auto run = [&](const char* command, const Json& commandParameters)
				{ return model.runEdit(command, project, commandParameters, context); };

				Json kept = Json::array();
				Json made = Json::array();
				int64_t count = 0;
				for (const Json& item : parameters.at("events"))
				{
					const auto id = item.get<ObjectId>();
					const auto found = project.audioEvents.find(id);
					require(found != project.audioEvents.end(), "no audio event " + std::to_string(id));
					const AudioEvent event = found->second;
					const MediaFile& media = project.media.at(project.clips.at(event.clip).media);
					require(parameters.contains("start") == parameters.contains("end"), "give both start and end");
					const Range range{
						parameters.value("start", event.offset), parameters.value("end", event.offset + event.length)};
					require(range.start >= event.offset && range.start < range.end &&
							range.end <= event.offset + event.length,
						"start and end must be within the event");
					std::vector<analysis::Section> sections;
					time::Samples previous = range.start;
					for (const Json& given : parameters.at("sections").at(index++))
					{
						const analysis::Section section{
							given.at("start").get<time::Samples>(), given.at("end").get<time::Samples>()};
						require(section.start >= previous && section.start < section.end && section.end <= range.end,
							"sections must be ordered within the event");
						previous = section.end;
						sections.push_back(section);
					}
					count += static_cast<int64_t>(sections.size());
					if (regions)
						for (const analysis::Section& section : sections)
							made.push_back(run("clip.addRegion",
								{{"clip", event.clip}, {"start", section.start}, {"end", section.end},
									{"name", name + " " + std::to_string(number++)}})
									.at("region")
									.at("id"));

					std::vector<ObjectId> pieces = {id};
					if (strip)
					{
						require(!event.lock, "audio event " + std::to_string(id) + " is locked");
						if (range.start > event.offset)
							sections.insert(sections.begin(), {event.offset, range.start});
						if (range.end < event.offset + event.length)
							sections.push_back({range.end, event.offset + event.length});
						require(!sections.empty(), "audio event " + std::to_string(id) + " is silent");
						Json positions = Json::array();
						for (const analysis::Section& section : sections)
							for (const time::Samples edge : {section.start, section.end})
								if (edge > event.offset && edge < event.offset + event.length)
									positions.push_back(sampleOf(project, event, edge));
						if (!positions.empty())
						{
							const Json split = run("event.split", {{"events", {id}}, {"positions", positions}});
							for (const Json& piece : split.at("events"))
								pieces.push_back(piece.get<ObjectId>());
						}
						Json silent = Json::array();
						std::vector<ObjectId> sounding;
						for (const ObjectId piece : pieces)
						{
							const AudioEvent& part = project.audioEvents.at(piece);
							const time::Samples middle = part.offset + (part.length / 2);
							const bool inside =
								std::any_of(sections.begin(), sections.end(), [middle](const analysis::Section& section)
									{ return middle >= section.start && middle < section.end; });
							if (inside)
								sounding.push_back(piece);
							else
								silent.push_back(piece);
						}
						if (!silent.empty())
							run("event.remove", {{"events", silent}});
						pieces = sounding;
					}
					for (const ObjectId piece : pieces)
					{
						const AudioEvent& keptPart = project.audioEvents.at(piece);
						const auto middle = keptPart.offset + (keptPart.length / 2);
						if (fades > 0.0 && middle >= range.start && middle < range.end)
						{
							const AudioEvent& part = project.audioEvents.at(piece);
							const time::Samples length = std::min<time::Samples>(
								std::llround(fades / 1000.0 * media.sampleRate), fadeRoom(project, part) / 2);
							run("audioEvent.setFades",
								{{"event", piece}, {"fadeIn", {{"length", length}}},
									{"fadeOut", {{"length", length}}}});
						}
						kept.push_back(piece);
					}
				}
				return Json{{"events", kept}, {"regions", made}, {"sections", count}};
			});

		const Json spectrumChannel = objectSchema(
			{{"channel", integer}, {"levels", {{"type", "array"}, {"items", nullable}}}}, {"channel", "levels"});
		addAnalysis(
			{"clip.spectrum", 1,
				"The average spectrum of a clip (the Spectrum Analyzer, reference manual pp. 607-608). " + rangeText +
					" Welch's method with 4-term Blackman-Harris windows of 8192 samples that overlap by half (shorter "
					"audio is one zero-padded window). Its frequencies go from 20 Hz up to the Nyquist frequency in "
					"steps of 1/24 octave; each channel's levels has a level in dBFS at each, the strongest of the "
					"frequencies within 1/48 octave of it, where a sine reads its peak level (a full-scale sine "
					"0 dBFS); null for silence. Reads the clip's file.",
				rangeSchema(Json::object()),
				objectSchema(
					{{"clip", integer}, {"sampleRate", number}, {"frequencies", {{"type", "array"}, {"items", number}}},
						{"channels", {{"type", "array"}, {"items", spectrumChannel}}}},
					{"clip", "sampleRate", "frequencies", "channels"}),
				true},
			[](ObjectId clip, const MediaFile& media, Range range, const Json&, JobContext& job)
			{
				const std::unique_ptr<AudioFileReader> reader = openMedia(media);
				std::vector<analysis::PowerSpectrum> spectra(static_cast<size_t>(channelsOf(*reader)));
				readRange(*reader, media, range, job,
					[&spectra](const float* frames, size_t count)
					{
						for (size_t c = 0; c < spectra.size(); ++c)
							spectra[c].feed(frames + c, count, spectra.size());
					});
				Json frequencies = Json::array();
				Json channels = Json::array();
				for (size_t c = 0; c < spectra.size(); ++c)
				{
					const analysis::SpectrumCurve curve =
						analysis::spectrumCurve(spectra[c].finish(), media.sampleRate);
					if (c == 0)
						frequencies = curve.frequencies;
					Json levels = Json::array();
					for (const std::optional<double>& level : curve.levels)
						levels.push_back(orNull(level));
					channels.push_back({{"channel", c}, {"levels", std::move(levels)}});
				}
				return Json{{"clip", clip}, {"sampleRate", media.sampleRate}, {"frequencies", frequencies},
					{"channels", channels}};
			});

		addAnalysis(
			{"clip.estimatePitch", 1,
				"Estimates a monophonic fundamental per channel in Hz, from 40 to 2000 Hz, including sounds whose "
				"overtones are louder. " +
					rangeText +
					" Uses periodicity in the loudest analysis window (about 0.17 s); null for silence, nonperiodic "
					"or too short audio. This is an estimate for single notes, not polyphonic transcription.",
				rangeSchema(Json::object()),
				objectSchema(
					{{"clip", integer}, {"frames", integer},
						{"channels",
							{{"type", "array"},
								{"items",
									objectSchema({{"channel", integer}, {"pitch", nullable}}, {"channel", "pitch"})}}}},
					{"clip", "frames", "channels"}),
				true},
			[](ObjectId clip, const MediaFile& media, Range range, const Json&, JobContext& job)
			{
				const auto reader = openMedia(media);
				std::vector<analysis::PitchEstimator> estimators;
				estimators.reserve(static_cast<size_t>(channelsOf(*reader)));
				for (int c = 0; c < channelsOf(*reader); ++c)
					estimators.emplace_back(media.sampleRate);
				readRange(*reader, media, range, job,
					[&](const float* frames, size_t count)
					{
						for (size_t c = 0; c < estimators.size(); ++c)
							estimators[c].feed(frames + c, count, estimators.size());
					});
				Json channels = Json::array();
				for (size_t c = 0; c < estimators.size(); ++c)
				{
					job.checkCancelled();
					channels.push_back({{"channel", c}, {"pitch", orNull(estimators[c].finish())}});
				}
				return Json{{"clip", clip}, {"frames", range.end - range.start}, {"channels", channels}};
			});

		Json statisticsChannel = {{"type", "object"}, {"properties", Json::object()}, {"required", Json::array()},
			{"additionalProperties", false}};
		for (const char* name : {"channel", "bitDepth"})
		{
			statisticsChannel["properties"][name] = integer;
			statisticsChannel["required"].push_back(name);
		}
		for (const char* name : {"minSample", "maxSample", "peak", "truePeak", "dcOffset", "dcOffsetPercent"})
		{
			statisticsChannel["properties"][name] = number;
			statisticsChannel["required"].push_back(name);
		}
		for (const char* name :
			{"minSampleDb", "maxSampleDb", "peakDb", "truePeakDb", "dcOffsetDb", "pitch", "averageRms", "maxRms"})
		{
			statisticsChannel["properties"][name] = nullable;
			statisticsChannel["required"].push_back(name);
		}
		const Json loudnessSchema =
			objectSchema({{"maxMomentary", nullable}, {"maxShortTerm", nullable}, {"integrated", nullable},
							 {"range", nullable}, {"maxTruePeak", nullable}},
				{"maxMomentary", "maxShortTerm", "integrated", "range", "maxTruePeak"});
		addAnalysis(
			{"clip.statistics", 1,
				"The statistics of a clip (the Statistics window, reference manual p. 609). " + rangeText +
					" Per channel: minSample and maxSample, the lowest and highest sample; peak, the largest absolute "
					"sample; truePeak, the largest absolute level between the samples too, 4 times oversampled; "
					"dcOffset, the mean of the samples, also in percent of full scale; bitDepth, the bits the samples "
					"need (32 for floating point beyond 24 bits, 0 for silence); pitch, the monophonic fundamental "
					"in Hz as clip.estimatePitch measures it; "
					"averageRms, the RMS of every sample, and maxRms, the highest RMS of 50 ms windows 10 ms apart, "
					"both in dB as AES17 has it, where a full-scale sine reads 0 dB. The levels are linear and in "
					"dBFS (Db; minSampleDb and maxSampleDb of the samples' absolute values); null for silence. "
					"maxRmsAllChannels is the highest maxRms. loudness (EBU R 128, using the default channel format "
					"for the file's channel count, front weight 1, surround weight 1.41, LFE excluded; unknown "
					"formats weight every channel 1): maxMomentary (400 ms) and maxShortTerm (3 s) in LUFS, ungated; "
					"integrated in "
					"LUFS (ITU-R BS.1770); range in LU (EBU Tech 3342); maxTruePeak in dBTP; null for silence or "
					"audio shorter than their windows. Reads the clip's file.",
				rangeSchema(Json::object()),
				objectSchema({{"clip", integer}, {"sampleRate", number}, {"frames", integer},
								 {"channels", {{"type", "array"}, {"items", statisticsChannel}}},
								 {"maxRmsAllChannels", nullable}, {"loudness", loudnessSchema}},
					{"clip", "sampleRate", "frames", "channels", "maxRmsAllChannels", "loudness"}),
				true},
			[](ObjectId clip, const MediaFile& media, Range range, const Json&, JobContext& job)
			{
				const std::unique_ptr<AudioFileReader> reader = openMedia(media);
				const auto channelCount = static_cast<size_t>(channelsOf(*reader));
				analysis::StatisticsMeter meter(media.sampleRate, static_cast<int>(channelCount));
				LoudnessMeter loudness;
				loudness.prepare(media.sampleRate);
				std::vector<std::vector<float>> planar(std::min<size_t>(channelCount, g_maxMeterChannels));
				std::array<double, g_maxMeterChannels> weights;
				weights.fill(1.0);
				const auto& arrangement = speakers(defaultFormat(static_cast<int>(channelCount)));
				for (size_t c = 0; c < arrangement.size(); ++c)
				{
					const Speaker speaker = arrangement[c];
					if (speaker == Speaker::Lfe)
						weights[c] = 0.0;
					else if (speaker == Speaker::Ls || speaker == Speaker::Rs || speaker == Speaker::Cs ||
						speaker == Speaker::Sl || speaker == Speaker::Sr)
						weights[c] = 1.41;
				}
				int64_t frames = 0;
				// The loudness meter's channels of interleaved frames, or silence without them.
				const auto measure = [&](const float* samples, size_t count, bool measuring)
				{
					std::vector<const float*> pointers;
					for (size_t c = 0; c < planar.size(); ++c)
					{
						planar[c].resize(count);
						for (size_t i = 0; i < count; ++i)
							planar[c][i] = samples != nullptr ? samples[(i * channelCount) + c] : 0.0F;
						pointers.push_back(planar[c].data());
					}
					loudness.process(pointers.data(), static_cast<int>(planar.size()), static_cast<uint32_t>(count),
						measuring, weights.data());
				};
				readRange(*reader, media, range, job,
					[&](const float* samples, size_t count)
					{
						meter.feed(samples, count);
						measure(samples, count, true);
						frames += static_cast<int64_t>(count);
					});
				const LoudnessReading measured = loudness.read();
				// The true peak of the last step's samples, and of the points that the interpolator still lags behind:
				// silence up to the next step and one more, without measuring, read after each.
				std::array<float, g_maxMeterChannels> truePeaks = measured.truePeakMax;
				const auto step = std::max<int64_t>(1, std::llround(0.1 * media.sampleRate));
				for (const int64_t silence : {(step - (frames % step)) % step, step})
				{
					if (silence == 0)
						continue;
					measure(nullptr, static_cast<size_t>(silence), false);
					const LoudnessReading after = loudness.read();
					for (size_t c = 0; c < planar.size(); ++c)
						truePeaks[c] = std::max(truePeaks[c], after.truePeak[c]);
				}
				const std::vector<analysis::ChannelStatistics> statistics = meter.finish();
				const auto aes17 = [frames](double rms)
				{ return frames == 0 ? Json(nullptr) : decibels(rms * std::sqrt(2.0)); };
				Json channels = Json::array();
				double maxRms = 0.0;
				double maxTruePeak = 0.0;
				for (size_t c = 0; c < statistics.size(); ++c)
				{
					const analysis::ChannelStatistics& channel = statistics[c];
					// Channels beyond the meter's have their sample peak.
					const double truePeak =
						c < planar.size() ? std::max(static_cast<double>(truePeaks[c]), channel.peak) : channel.peak;
					maxRms = std::max(maxRms, channel.maxRms);
					maxTruePeak = std::max(maxTruePeak, truePeak);
					channels.push_back({{"channel", c}, {"minSample", channel.minSample},
						{"minSampleDb", decibels(std::abs(channel.minSample))}, {"maxSample", channel.maxSample},
						{"maxSampleDb", decibels(std::abs(channel.maxSample))}, {"peak", channel.peak},
						{"peakDb", decibels(channel.peak)}, {"truePeak", truePeak}, {"truePeakDb", decibels(truePeak)},
						{"dcOffset", channel.dcOffset}, {"dcOffsetPercent", channel.dcOffset * 100.0},
						{"dcOffsetDb", decibels(std::abs(channel.dcOffset))}, {"bitDepth", channel.bitDepth},
						{"pitch", orNull(channel.pitch)}, {"averageRms", aes17(channel.averageRms)},
						{"maxRms", aes17(channel.maxRms)}});
				}
				return Json{{"clip", clip}, {"sampleRate", media.sampleRate}, {"frames", frames},
					{"channels", channels}, {"maxRmsAllChannels", aes17(maxRms)},
					{"loudness",
						{{"maxMomentary", orNull(measured.momentaryMax)},
							{"maxShortTerm", orNull(measured.shortTermMax)},
							{"integrated", orNull(measured.integrated)}, {"range", orNull(measured.range)},
							{"maxTruePeak", decibels(maxTruePeak)}}}};
			});
	}
}
