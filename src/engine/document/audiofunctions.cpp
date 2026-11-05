// SPDX-License-Identifier: GPL-3.0-or-later
// The audio functions (#147, reference manual pp. 603-609): Detect Silence, the Spectrum Analyzer and Statistics. They
// read the clip's file on the command thread (src/engine/analysis/statistics.h). event.stripSilence makes its events
// and regions with the commands that make such things, run as part of it (ProjectModel::runEdit), so that it is one
// undo step.
#include "engine/document/audiofunctions.h"

#include "engine/analysis/analysis.h"
#include "engine/analysis/statistics.h"
#include "engine/audio/meters.h"
#include "engine/audiofile/audiofile.h"
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/engine.h"

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

		// Reads the frames of a media file from range.start to range.end in blocks of interleaved frames: feed(frames,
		// count) with the file's channels.
		template <typename Feed> int readRange(const MediaFile& media, Range range, Feed feed)
		{
			try
			{
				const std::unique_ptr<AudioFileReader> reader = AudioFileReader::open(media.path);
				const int channels = std::max(reader->info().channels, 1);
				feed(nullptr, 0, channels); // tells the channels first
				reader->seek(range.start);
				std::vector<float> buffer(g_readFrames * static_cast<size_t>(channels));
				for (time::Samples at = range.start; at < range.end;)
				{
					const auto wanted = static_cast<size_t>(std::min<time::Samples>(g_readFrames, range.end - at));
					const size_t read = reader->read(buffer.data(), wanted);
					if (read == 0)
						break;
					feed(buffer.data(), read, channels);
					at += static_cast<time::Samples>(read);
				}
				return channels;
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
		std::vector<analysis::Section> detectSilence(
			const MediaFile& media, Range range, const analysis::SilenceSettings& settings)
		{
			std::optional<analysis::SilenceDetector> detector;
			readRange(media, range,
				[&](const float* frames, size_t count, int channels)
				{
					if (!detector)
						detector.emplace(media.sampleRate, channels, settings);
					detector->feed(frames, count);
				});
			std::vector<analysis::Section> sections = detector->finish();
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
			return span.start + std::llround(static_cast<double>(frame - event.offset) * span.ratio);
		}

		const char* const g_settingsText =
			"A section opens at the first frame where a sample of any channel reaches openThreshold (dBFS, default "
			"-20) and closes after the last frame that reaches closeThreshold (dBFS, default -30, at most "
			"openThreshold) before 20 ms below it. Silences shorter than minimumClosed (ms, default 100) between "
			"sections are kept, sections shorter than minimumOpen (ms, default 10) dropped; preRoll and postRoll (ms, "
			"default 0) start and end the rest earlier and later, joining those that then overlap.";
	}

	void registerAudioFunctionCommands(ProjectModel& model, Engine& engine)
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
		const auto rangeSchema = [&](Json more)
		{
			Json properties = {{"clip", integer}, {"start", integer}, {"end", integer}};
			properties.update(more);
			return objectSchema(std::move(properties), {"clip"});
		};
		const std::string rangeText = "The audio of clip from frame start to end (default: the whole clip).";

		engine.registerCommand(
			{"clip.detectSilence", 1,
				"Finds the sections of a clip that aren't silent (the Detect Silence dialog's Analyze, reference "
				"manual pp. 603-606). " +
					rangeText + " " + g_settingsText +
					" Reads the clip's file. Returns the clip and its sections, start to end in clip frames, by start.",
				rangeSchema(settingsProperties), objectSchema({{"clip", integer}, {"sections", sectionsSchema}}), true},
			[&model](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const Project& project = model.project();
				const Clip& clip = clipOf(project, parameters);
				const Range range = rangeOf(project, clip, parameters);
				const analysis::SilenceSettings settings = silenceSettings(parameters);
				return Json{{"clip", clip.id},
					{"sections", sectionsJson(detectSilence(project.media.at(clip.media), range, settings))}};
			});

		Json stripProperties = settingsProperties;
		stripProperties.update({{"events", ids}, {"strip", boolean}, {"regions", boolean}, {"regionName", string},
			{"autoNumberStart", integer}, {"fades", number}});
		model.addEdit(engine,
			{"event.stripSilence", 1,
				"Processes the sections of audio events that aren't silent (the Detect Silence dialog's Process, "
				"reference manual pp. 603-606), as clip.detectSilence finds them in the section of the clip each "
				"plays. " +
					std::string(g_settingsText) +
					" strip (Strip Silence, default true) splits each event at the sections' edges and removes the "
					"silent parts between them; regions (Add as Regions) adds a region of the clip for each section, "
					"named regionName (default Region) and a number from autoNumberStart (default 1) up, across the "
					"events; fades (Apply Fades, ms) gives the events that remain a fade in and a fade out of that "
					"length, at most half their length each. An event that strip would remove completely, or that is "
					"locked, is an error. Returns the events that remain and the regions, and the number of "
					"sections.",
				objectSchema(stripProperties, {"events"}),
				objectSchema(
					{{"events", ids}, {"regions", ids}, {"sections", integer}}, {"events", "regions", "sections"}),
				false},
			"Detect silence",
			[&model](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const analysis::SilenceSettings settings = silenceSettings(parameters);
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
					const std::vector<analysis::Section> sections =
						detectSilence(media, {event.offset, event.offset + event.length}, settings);
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
						if (fades > 0.0)
						{
							const AudioEvent& part = project.audioEvents.at(piece);
							const time::Samples length =
								std::min(std::llround(fades / 1000.0 * media.sampleRate), part.length / 2);
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
		engine.registerCommand(
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
			[&model](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const Project& project = model.project();
				const Clip& clip = clipOf(project, parameters);
				const Range range = rangeOf(project, clip, parameters);
				const MediaFile& media = project.media.at(clip.media);
				std::vector<analysis::PowerSpectrum> spectra;
				readRange(media, range,
					[&spectra](const float* frames, size_t count, int channels)
					{
						spectra.resize(static_cast<size_t>(channels));
						for (size_t c = 0; c < spectra.size() && frames != nullptr; ++c)
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
				return Json{{"clip", clip.id}, {"sampleRate", media.sampleRate}, {"frequencies", frequencies},
					{"channels", channels}};
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
		engine.registerCommand(
			{"clip.statistics", 1,
				"The statistics of a clip (the Statistics window, reference manual p. 609). " + rangeText +
					" Per channel: minSample and maxSample, the lowest and highest sample; peak, the largest absolute "
					"sample; truePeak, the largest absolute level between the samples too, 4 times oversampled; "
					"dcOffset, the mean of the samples, also in percent of full scale; bitDepth, the bits the samples "
					"need (32 for floating point beyond 24 bits, 0 for silence); pitch, the strongest frequency in Hz; "
					"averageRms, the RMS of every sample, and maxRms, the highest RMS of 50 ms windows 10 ms apart, "
					"both in dB as AES17 has it, where a full-scale sine reads 0 dB. The levels are linear and in "
					"dBFS (Db; minSampleDb and maxSampleDb of the samples' absolute values); null for silence. "
					"maxRmsAllChannels is the highest maxRms. loudness (EBU R 128, every channel weighted like front "
					"left and right): maxMomentary (400 ms) and maxShortTerm (3 s) in LUFS, ungated; integrated in "
					"LUFS (ITU-R BS.1770); range in LU (EBU Tech 3342); maxTruePeak in dBTP; null for silence or "
					"audio shorter than their windows. Reads the clip's file.",
				rangeSchema(Json::object()),
				objectSchema({{"clip", integer}, {"sampleRate", number}, {"frames", integer},
								 {"channels", {{"type", "array"}, {"items", statisticsChannel}}},
								 {"maxRmsAllChannels", nullable}, {"loudness", loudnessSchema}},
					{"clip", "sampleRate", "frames", "channels", "maxRmsAllChannels", "loudness"}),
				true},
			[&model](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const Project& project = model.project();
				const Clip& clip = clipOf(project, parameters);
				const Range range = rangeOf(project, clip, parameters);
				const MediaFile& media = project.media.at(clip.media);
				std::optional<analysis::StatisticsMeter> meter;
				LoudnessMeter loudness;
				loudness.prepare(media.sampleRate);
				std::vector<std::vector<float>> planar;
				int64_t frames = 0;
				const auto measure = [&](const float* samples, size_t count, bool measuring)
				{
					std::vector<const float*> pointers;
					for (size_t c = 0; c < planar.size(); ++c)
					{
						planar[c].resize(count);
						for (size_t i = 0; i < count; ++i)
							planar[c][i] = samples != nullptr ? samples[(i * planar.size()) + c] : 0.0F;
						pointers.push_back(planar[c].data());
					}
					loudness.process(
						pointers.data(), static_cast<int>(planar.size()), static_cast<uint32_t>(count), measuring);
				};
				readRange(media, range,
					[&](const float* samples, size_t count, int channels)
					{
						if (!meter)
						{
							meter.emplace(media.sampleRate, channels);
							planar.resize(static_cast<size_t>(std::min(channels, g_maxMeterChannels)));
						}
						if (count == 0)
							return;
						meter->feed(samples, count);
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
				const std::vector<analysis::ChannelStatistics> statistics = meter->finish();
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
				return Json{{"clip", clip.id}, {"sampleRate", media.sampleRate}, {"frames", frames},
					{"channels", channels}, {"maxRmsAllChannels", aes17(maxRms)},
					{"loudness",
						{{"maxMomentary", orNull(measured.momentaryMax)},
							{"maxShortTerm", orNull(measured.shortTermMax)},
							{"integrated", orNull(measured.integrated)}, {"range", orNull(measured.range)},
							{"maxTruePeak", decibels(maxTruePeak)}}}};
			});
	}
}
