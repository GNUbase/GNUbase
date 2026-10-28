// SPDX-License-Identifier: GPL-3.0-or-later
// The commands that measure, compare and record the dummy device's captured output (#28): audio.analyze,
// audio.compare and audio.record. They read the capture on the command thread, like audio.capture.
#include "engine/analysis/analysis.h"
#include "engine/audio/audioengine.h"
#include "engine/audiofile/audiofile.h"
#include "engine/engine.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		constexpr int64_t g_maxPeaks = 20;

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		std::string channelCount(size_t count)
		{
			return std::to_string(count) + (count == 1 ? " channel" : " channels");
		}

		Json decibels(double level)
		{
			const std::optional<double> db = analysis::decibels(level);
			return db ? Json(*db) : Json(nullptr);
		}

		Json levels(int channel, const analysis::Levels& levels)
		{
			return {{"channel", channel}, {"peak", levels.peak}, {"peakDb", decibels(levels.peak)}, {"rms", levels.rms},
				{"rmsDb", decibels(levels.rms)}};
		}

		Json spectrum(const analysis::Spectrum& spectrum)
		{
			Json peaks = Json::array();
			for (const analysis::SpectralPeak& peak : spectrum.peaks)
				peaks.push_back({{"frequency", peak.frequency}, {"level", peak.level}});
			Json bands = Json::array();
			for (const analysis::Band& band : spectrum.bands)
				bands.push_back(
					{{"frequency", band.frequency}, {"level", band.level ? Json(*band.level) : Json(nullptr)}});
			return {{"peaks", peaks}, {"bands", bands}};
		}

		// The part of the capture a command reads: channels (default every output), start and frames (default to the
		// end of the capture, or defaultFrames).
		struct Selection
		{
			std::vector<int> channels;
			size_t start = 0;
			size_t frames = 0;
		};

		Selection select(const audio::DummyDevice& device, const Json& parameters, std::optional<size_t> defaultFrames)
		{
			Selection selection;
			const int outputs = device.format().outputChannels;
			if (const auto channels = parameters.find("channels"); channels != parameters.end())
			{
				for (const Json& channel : *channels)
				{
					const auto index = channel.get<int64_t>();
					require(index >= 0 && index < outputs, "no output channel " + std::to_string(index));
					selection.channels.push_back(static_cast<int>(index));
				}
			}
			else
			{
				for (int channel = 0; channel < outputs; ++channel)
					selection.channels.push_back(channel);
			}
			require(!selection.channels.empty(), "there are no channels to read");

			const size_t captured = device.capturedFrames();
			const auto start = parameters.value("start", int64_t{0});
			require(start >= 0 && static_cast<size_t>(start) <= captured,
				"start must be from 0 to the " + std::to_string(captured) + " captured frames");
			selection.start = static_cast<size_t>(start);
			const size_t available = captured - selection.start;
			const auto frames = parameters.value("frames", static_cast<int64_t>(defaultFrames.value_or(available)));
			require(frames >= 0 && static_cast<size_t>(frames) <= available,
				"there are " + std::to_string(available) + " captured frames from start, not " +
					std::to_string(frames));
			selection.frames = static_cast<size_t>(frames);
			return selection;
		}

		std::vector<std::vector<float>> read(const audio::DummyDevice& device, const Selection& selection)
		{
			std::vector<std::vector<float>> channels;
			channels.reserve(selection.channels.size());
			for (const int channel : selection.channels)
				channels.push_back(device.captured(channel, selection.start, selection.frames));
			return channels;
		}

		SampleFormat sampleFormat(const std::string& name)
		{
			if (name == "int16")
				return SampleFormat::Int16;
			if (name == "int24")
				return SampleFormat::Int24;
			if (name == "int32")
				return SampleFormat::Int32;
			return SampleFormat::Float32;
		}

		Json selectionSchema(Json properties = Json::object(), const Json& required = Json::array())
		{
			properties["channels"] = {{"type", "array"}, {"items", {{"type", "integer"}}}};
			properties["start"] = {{"type", "integer"}};
			properties["frames"] = {{"type", "integer"}};
			return {{"type", "object"}, {"additionalProperties", false}, {"properties", properties},
				{"required", required}};
		}

		const char* const g_levelsSchema = R"({"type": "object",
			"required": ["channel", "peak", "peakDb", "rms", "rmsDb"],
			"properties": {"channel": {"type": "integer"}, "peak": {"type": "number"},
				"peakDb": {"type": ["number", "null"]}, "rms": {"type": "number"}, "rmsDb": {"type": ["number", "null"]}}})";
	}

	void AudioEngine::registerAnalysisCommands(Engine& engine)
	{
		const auto device = [this]() -> const audio::DummyDevice& { return requireDummy(); };

		Json analyzedChannel = Json::parse(g_levelsSchema);
		analyzedChannel["required"].push_back("spectrum");
		analyzedChannel["properties"]["spectrum"] = Json::parse(R"({"type": "object", "required": ["peaks", "bands"],
			"properties": {
				"peaks": {"type": "array", "items": {"type": "object", "required": ["frequency", "level"],
					"properties": {"frequency": {"type": "number"}, "level": {"type": "number"}}}},
				"bands": {"type": "array", "items": {"type": "object", "required": ["frequency", "level"],
					"properties": {"frequency": {"type": "number"}, "level": {"type": ["number", "null"]}}}}}})");
		const Json analyzed = {{"type", "object"},
			{"required", {"sampleRate", "start", "frames", "loudness", "channels"}},
			{"properties",
				{{"sampleRate", {{"type", "number"}}}, {"start", {{"type", "integer"}}},
					{"frames", {{"type", "integer"}}}, {"loudness", {{"type", {"number", "null"}}}},
					{"channels", {{"type", "array"}, {"items", analyzedChannel}}}}}};
		engine.registerCommand(
			{"audio.analyze", 1,
				"Measures the captured output of channels (default: every output) from frame start for frames frames "
				"(default: to the end of the capture). Per channel: the peak and RMS level, and a spectrum summary "
				"with "
				"the strongest frequencies (at most peaks, default 5, up to 20) and the power of octave bands. For the "
				"channels together: the integrated loudness in LUFS (ITU-R BS.1770), null below 400 ms. Levels are "
				"linear and in dBFS (a full-scale sine's peak is 0 dBFS, its RMS -3 dBFS); dBFS is null for silence.",
				selectionSchema({{"peaks", {{"type", "integer"}}}}), analyzed, true},
			[device](CommandContext& context)
			{
				const audio::DummyDevice& dummy = device();
				const Json& parameters = context.parameters();
				const Selection selection = select(dummy, parameters, std::nullopt);
				const auto peaks = parameters.value("peaks", int64_t{5});
				require(peaks >= 0 && peaks <= g_maxPeaks, "peaks must be from 0 to " + std::to_string(g_maxPeaks));
				const double sampleRate = dummy.format().sampleRate;
				const std::vector<std::vector<float>> audio = read(dummy, selection);
				Json channels = Json::array();
				for (size_t i = 0; i < audio.size(); ++i)
				{
					Json measured = levels(selection.channels[i], analysis::levels(audio[i]));
					measured["spectrum"] =
						spectrum(analysis::spectrum(audio[i], sampleRate, static_cast<size_t>(peaks)));
					channels.push_back(std::move(measured));
				}
				const std::optional<double> loudness = analysis::integratedLoudness(audio, sampleRate);
				return Json{{"sampleRate", sampleRate}, {"start", selection.start}, {"frames", selection.frames},
					{"loudness", loudness ? Json(*loudness) : Json(nullptr)}, {"channels", channels}};
			});

		const Json compared = {{"type", "object"}, {"required", {"frames", "peak", "peakDb", "channels"}},
			{"properties",
				{{"frames", {{"type", "integer"}}}, {"peak", {{"type", "number"}}},
					{"peakDb", {{"type", {"number", "null"}}}},
					{"channels", {{"type", "array"}, {"items", Json::parse(g_levelsSchema)}}}}}};
		engine.registerCommand(
			{"audio.compare", 1,
				"Null test: subtracts an audio file, reference, from the captured output and measures what remains. "
				"The file's channels are compared with channels (default: every output) in order, from frame start, "
				"for "
				"frames frames (default: the file's length); its sample rate must be the device's. peak is the largest "
				"difference of all channels, 0 if they cancel completely.",
				selectionSchema({{"reference", {{"type", "string"}}}}, {"reference"}), compared, true},
			[device](CommandContext& context)
			{
				const audio::DummyDevice& dummy = device();
				const Json& parameters = context.parameters();
				const std::string path = parameters.at("reference").get<std::string>();
				std::unique_ptr<AudioFileReader> reader;
				try
				{
					reader = AudioFileReader::open(path);
				}
				catch (const AudioFileError& error)
				{
					throw CommandError("can't read " + path + ": " + error.what());
				}
				const AudioFileInfo& info = reader->info();
				const Selection selection = select(dummy, parameters, static_cast<size_t>(info.frames));
				const size_t channelsCompared = selection.channels.size();
				require(info.sampleRate == dummy.format().sampleRate,
					"the reference's sample rate is " + Json(info.sampleRate).dump() + " Hz, the device's " +
						Json(dummy.format().sampleRate).dump() + " Hz");
				require(static_cast<size_t>(info.channels) == channelsCompared,
					"the reference has " + channelCount(static_cast<size_t>(info.channels)) + ", but " +
						channelCount(channelsCompared) + " are compared");
				require(static_cast<size_t>(info.frames) >= selection.frames,
					"the reference has only " + std::to_string(info.frames) + " frames");

				std::vector<float> reference(selection.frames * channelsCompared);
				const size_t frames = reader->read(reference.data(), selection.frames);
				require(frames == selection.frames, "can't read " + path + ": it ends early");
				const std::vector<std::vector<float>> audio = read(dummy, selection);
				double peak = 0.0;
				Json channels = Json::array();
				for (size_t i = 0; i < channelsCompared; ++i)
				{
					std::vector<float> difference(selection.frames);
					for (size_t frame = 0; frame < selection.frames; ++frame)
						difference[frame] = audio[i][frame] - reference[(frame * channelsCompared) + i];
					const analysis::Levels remaining = analysis::levels(difference);
					peak = std::max(peak, remaining.peak);
					channels.push_back(levels(selection.channels[i], remaining));
				}
				return Json{
					{"frames", selection.frames}, {"peak", peak}, {"peakDb", decibels(peak)}, {"channels", channels}};
			});

		const Json recorded =
			Json::parse(R"({"type": "object", "required": ["path", "frames", "channels", "sampleRate"],
			"properties": {"path": {"type": "string"}, "frames": {"type": "integer"},
				"channels": {"type": "array", "items": {"type": "integer"}}, "sampleRate": {"type": "number"}}})");
		engine.registerCommand(
			{"audio.record", 1,
				"Writes the captured output of channels (default: every output) from frame start for frames frames "
				"(default: to the end of the capture) to a WAV file at path, with samples in format (default float32). "
				"Returns the file's absolute path.",
				selectionSchema({{"path", {{"type", "string"}}},
									{"format", {{"type", "string"}, {"enum", {"float32", "int16", "int24", "int32"}}}}},
					{"path"}),
				recorded, true},
			[device](CommandContext& context)
			{
				const audio::DummyDevice& dummy = device();
				const Json& parameters = context.parameters();
				const Selection selection = select(dummy, parameters, std::nullopt);
				const std::string path = parameters.at("path").get<std::string>();
				const std::vector<std::vector<float>> audio = read(dummy, selection);
				const size_t channels = audio.size();
				std::vector<float> interleaved(selection.frames * channels);
				for (size_t i = 0; i < channels; ++i)
				{
					for (size_t frame = 0; frame < selection.frames; ++frame)
						interleaved[(frame * channels) + i] = audio[i][frame];
				}

				WriteOptions options;
				options.format = AudioFormat::Wav;
				options.sampleFormat = sampleFormat(parameters.value("format", std::string("float32")));
				options.sampleRate = dummy.format().sampleRate;
				options.channels = static_cast<int>(channels);
				try
				{
					const std::unique_ptr<AudioFileWriter> writer = AudioFileWriter::create(path, options);
					writer->write(interleaved.data(), selection.frames);
					writer->close();
				}
				catch (const AudioFileError& error)
				{
					throw CommandError("can't write " + path + ": " + error.what());
				}
				return Json{{"path", std::filesystem::absolute(std::filesystem::u8path(path)).u8string()},
					{"frames", selection.frames}, {"channels", selection.channels}, {"sampleRate", options.sampleRate}};
			});
	}
}
