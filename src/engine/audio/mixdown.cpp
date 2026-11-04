// SPDX-License-Identifier: GPL-3.0-or-later
// Exporting audio mixdowns (#115, docs/features/mixdown.md). mixdown.export renders a range offline, block by block
// as audio.render does, and writes what busses and channels put out to audio files; mixdown.quickExport writes the
// main mix of every event, with the tail of the effects. Each file is written twice: first as it is rendered, at
// 32-bit float into a part file next to it, then from there into its format, normalized and dithered.
#include "engine/audio/audioengine.h"
#include "engine/audiofile/audiofile.h"
#include "engine/audiofile/resampler.h"
#include "engine/document/rendersnapshot.h"
#include "engine/dsp/dither.h"
#include "engine/engine.h"
#include "engine/plugins/slotinstances.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		namespace fs = std::filesystem;

		constexpr double g_maxTailSeconds = 30.0;
		// Quick export's tail ends after the last sample that a 24-bit file doesn't hold as 0.
		constexpr float g_audible = 0.5f / 8388608.0f;
		constexpr size_t g_chunk = 4096; // frames per read and write of the second pass

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		struct FileFormat
		{
			AudioFormat format = AudioFormat::Wav;
			SampleFormat sampleFormat = SampleFormat::Int24;
			double sampleRate = 48000.0;
			bool dither = false;
			std::optional<double> normalize; // the peak in dBFS
			bool overwrite = false;
		};

		// A bus or channel being exported.
		struct Target
		{
			ObjectId object = 0;
			std::string name;
			int node = -1;
			int channels = 0;
			fs::path path;
			fs::path part;
			std::unique_ptr<Resampler> resampler;
			std::unique_ptr<AudioFileWriter> writer; // of the part file
			std::vector<float> block;                // interleaved
			std::vector<float> converted;
			float peak = 0.0f;
			int64_t frames = 0;   // in the part file
			int64_t audible = 0;  // frames up to the last audible one
			bool created = false; // the file, which it removes if the export fails
		};

		const char* extensionOf(AudioFormat format)
		{
			switch (format)
			{
			case AudioFormat::Aiff:
				return ".aif";
			case AudioFormat::Flac:
				return ".flac";
			default:
				return ".wav";
			}
		}

		FileFormat formatFrom(const Json& parameters, double projectRate)
		{
			FileFormat result;
			const std::string format = parameters.value("format", std::string("wav"));
			if (format == "aiff")
				result.format = AudioFormat::Aiff;
			else if (format == "flac")
				result.format = AudioFormat::Flac;
			const std::string samples = parameters.value("sampleFormat", std::string("int24"));
			constexpr std::array<std::pair<const char*, SampleFormat>, 5> names = {
				{{"int16", SampleFormat::Int16}, {"int24", SampleFormat::Int24}, {"int32", SampleFormat::Int32},
					{"float32", SampleFormat::Float32}, {"float64", SampleFormat::Float64}}};
			for (const auto& [name, value] : names)
				if (samples == name)
					result.sampleFormat = value;
			const bool floating =
				result.sampleFormat == SampleFormat::Float32 || result.sampleFormat == SampleFormat::Float64;
			require(result.format != AudioFormat::Aiff || !floating, "AIFF files take int16, int24 or int32 samples");
			require(result.format != AudioFormat::Flac || result.sampleFormat == SampleFormat::Int16 ||
					result.sampleFormat == SampleFormat::Int24,
				"FLAC files take int16 or int24 samples");
			result.sampleRate = parameters.value("sampleRate", projectRate);
			require(result.sampleRate >= 8000.0 && result.sampleRate <= 768000.0,
				"sampleRate must be from 8000 to 768000 Hz");
			result.dither = parameters.value("dither", false) &&
				(result.sampleFormat == SampleFormat::Int16 || result.sampleFormat == SampleFormat::Int24);
			if (const auto normalize = parameters.find("normalize"); normalize != parameters.end())
			{
				result.normalize = normalize->get<double>();
				require(*result.normalize >= -60.0 && *result.normalize <= 0.0, "normalize must be from -60 to 0 dB");
			}
			result.overwrite = parameters.value("conflicts", std::string("unique")) == "overwrite";
			return result;
		}

		// A file name from a track or bus name: characters that some file systems refuse become _.
		std::string fileName(std::string name)
		{
			for (char& c : name)
				if (static_cast<unsigned char>(c) < 32 ||
					std::string_view(R"(<>:"/\|?*)").find(c) != std::string_view::npos)
					c = '_';
			return name;
		}

		// The file for name in folder: with unique, the first of "name.ext", "name (2).ext", ... that doesn't exist and
		// isn't taken by another file of this export.
		fs::path claim(const fs::path& folder, const std::string& name, const char* extension, bool overwrite,
			const std::set<fs::path>& taken)
		{
			for (int number = 1;; ++number)
			{
				const std::string suffix = number == 1 ? "" : " (" + std::to_string(number) + ")";
				const fs::path path = folder / fs::u8path(fileName(name) + suffix + extension);
				std::error_code error;
				if (taken.count(path) == 0 && (overwrite || !fs::exists(path, error)))
					return path;
			}
		}

		// The name of a bus or of a track's channel in project.get's project.
		std::optional<std::string> nameOf(const Json& project, ObjectId object)
		{
			for (const Json& bus : project.at("busses"))
				if (bus.at("id") == object)
					return bus.at("name").get<std::string>();
			for (const Json& track : project.at("tracks"))
				if (track.at("channel") == object)
					return track.at("name").get<std::string>();
			return std::nullopt;
		}

		// Second pass: the part file into the target's file, at gain, dithered, at most frames frames.
		void finish(const Target& target, const FileFormat& format, int64_t frames)
		{
			const std::unique_ptr<AudioFileReader> reader = AudioFileReader::open(target.part.u8string());
			WriteOptions options;
			options.format = format.format;
			options.sampleFormat = format.sampleFormat;
			options.sampleRate = format.sampleRate;
			options.channels = target.channels;
			const std::unique_ptr<AudioFileWriter> writer = AudioFileWriter::create(target.path.u8string(), options);
			const float gain = format.normalize && target.peak > 0.0f
				? static_cast<float>(std::pow(10.0, *format.normalize / 20.0) / target.peak)
				: 1.0f;
			const bool dither = format.dither && target.channels <= dsp::g_maxChannels;
			dsp::Dither dsp;
			dsp.prepare(format.sampleRate, static_cast<uint32_t>(g_chunk));
			dsp.setParameter(dsp::Dither::Bits, format.sampleFormat == SampleFormat::Int16 ? 16.0 : 24.0);
			const auto channels = static_cast<size_t>(target.channels);
			std::vector<float> interleaved(g_chunk * channels);
			std::vector<float> planar(dither ? g_chunk * channels : 0);
			std::vector<float*> pointers(dither ? channels : 0);
			for (size_t c = 0; c < pointers.size(); ++c)
				pointers[c] = planar.data() + (c * g_chunk);
			for (int64_t done = 0; done < frames;)
			{
				const size_t read =
					reader->read(interleaved.data(), std::min<size_t>(g_chunk, static_cast<size_t>(frames - done)));
				if (read == 0)
					break;
				if (gain != 1.0f)
					for (size_t i = 0; i < read * channels; ++i)
						interleaved[i] *= gain;
				if (dither)
				{
					for (size_t i = 0; i < read; ++i)
						for (size_t c = 0; c < channels; ++c)
							pointers[c][i] = interleaved[(i * channels) + c];
					dsp.process(pointers.data(), target.channels, 0, static_cast<uint32_t>(read));
					for (size_t i = 0; i < read; ++i)
						for (size_t c = 0; c < channels; ++c)
							interleaved[(i * channels) + c] = pointers[c][i];
				}
				writer->write(interleaved.data(), read);
				done += static_cast<int64_t>(read);
			}
			writer->close();
		}

		// Removes the part files and, unless the export is done, the files it wrote.
		struct Cleanup
		{
			std::vector<Target>& targets;
			bool done = false;

			Cleanup(const Cleanup&) = delete;
			Cleanup& operator=(const Cleanup&) = delete;
			~Cleanup()
			{
				std::error_code error;
				for (Target& target : targets)
				{
					target.writer.reset(); // closes a part file that is still open
					fs::remove(target.part, error);
					if (!done && target.created)
						fs::remove(target.path, error);
				}
			}
		};
	}

	void AudioEngine::registerMixdownCommands(Engine& engine)
	{
		// Renders [start, end) and tail frames more of the targets, which have objects and paths, into their files.
		// The transport, its settings and the device are as before afterwards.
		const auto exportRange = [this](CommandContext& context, std::vector<Target>& targets, time::Samples start,
									 time::Samples end, time::Samples tail, const FileFormat& format)
		{
			require(m_device != nullptr && m_programs, "there is no audio device; set one with audio.setDevice");
			require(!m_transport.recording(), "stop recording first");
			const double projectRate = m_snapshots.latest().sampleRate;
			// The graph and its processors run at the device's rate, the events at the project's.
			require(m_device->format().sampleRate == projectRate,
				"the device runs at " + std::to_string(std::lround(m_device->format().sampleRate)) +
					" Hz, not at the project's sample rate of " + std::to_string(std::lround(projectRate)) +
					" Hz; set the device to the project's sample rate");
			const graph::CompiledGraph& graph = m_programs->latest().graph;
			for (Target& target : targets)
			{
				target.node = graph.find(target.object);
				target.channels = target.node >= 0 ? graph.outputChannels(target.node) : 0;
				require(target.channels > 0, target.name + " has no audio");
			}
			Cleanup cleanup{targets};
			try
			{
				for (Target& target : targets)
				{
					target.part = fs::u8path(target.path.u8string() + ".part");
					WriteOptions options;
					options.sampleFormat = SampleFormat::Float32;
					options.sampleRate = format.sampleRate;
					options.channels = target.channels;
					target.writer = AudioFileWriter::create(target.part.u8string(), options);
					target.resampler = std::make_unique<Resampler>(
						projectRate, format.sampleRate, target.channels, m_device->format().blockSize);
					target.block.resize(static_cast<size_t>(target.channels) * m_device->format().blockSize);
				}
				const auto write = [](Target& target)
				{
					const auto channels = static_cast<size_t>(target.channels);
					const size_t frames = target.converted.size() / channels;
					for (size_t i = 0; i < target.converted.size(); ++i)
					{
						const float level = std::abs(target.converted[i]);
						target.peak = std::max(target.peak, level);
						if (level >= g_audible)
							target.audible = target.frames + static_cast<int64_t>(i / channels) + 1;
					}
					target.writer->write(target.converted.data(), frames);
					target.frames += static_cast<int64_t>(frames);
					target.converted.clear();
				};

				// Rendered as by transport.locate, transport.play and audio.render, without cycle, pre-roll, punch
				// in or out and the click.
				const bool running = m_device->running();
				if (running)
					stopDevice();
				const TransportSettings settings = m_transport.settings();
				const time::Samples cursor = m_transport.position();
				const bool playing = m_transport.playing();
				TransportSettings plain = settings;
				plain.cycle = plain.preRoll = plain.postRoll = plain.punchIn = plain.punchOut = false;
				plain.returnToStart = plain.metronome = false;
				plain.startMode = StartMode::Cursor;
				// Plugins render offline, so a host process waits for their blocks as long as they take.
				const auto offline = [this](bool on)
				{
					if (m_slots)
						m_slots->setProcessMode(on ? plugin::ProcessMode::Offline : plugin::ProcessMode::Realtime);
				};
				const auto restore = [&]
				{
					offline(false);
					sent(m_transport.stop());
					sent(m_transport.set(settings));
					cue(cursor, false, m_device->format().blockSize, true);
					sent(m_transport.locate(cursor));
					if (running)
						startDevice();
					if (playing)
						context.emit("transport.changed", transportStatus());
				};
				try
				{
					offline(true);
					sent(m_transport.set(plain));
					if (playing)
						sent(m_transport.stop());
					cue(start, false, m_device->format().blockSize, true);
					sent(m_transport.locate(start));
					play();
					render(static_cast<uint64_t>(end - start + tail),
						[&](uint32_t frames)
						{
							for (Target& target : targets)
							{
								float* const* audio = graph.audio(target.node);
								const auto channels = static_cast<size_t>(target.channels);
								for (uint32_t i = 0; i < frames; ++i)
									for (size_t c = 0; c < channels; ++c)
										target.block[(i * channels) + c] = audio[c][i];
								target.resampler->process(target.block.data(), frames, target.converted);
								write(target);
							}
						});
				}
				catch (...)
				{
					restore();
					throw;
				}
				restore();

				const int64_t length = Resampler::outputLength(end - start, projectRate, format.sampleRate);
				Json files = Json::array();
				for (Target& target : targets)
				{
					target.resampler->finish(target.converted);
					write(target);
					target.writer->close();
					target.writer.reset();
					const int64_t frames = tail > 0 ? std::max(length, target.audible) : target.frames;
					target.created = true;
					finish(target, format, frames);
					files.push_back({{"channel", target.object}, {"name", target.name},
						{"path", fs::absolute(target.path).u8string()}, {"frames", frames}});
				}
				cleanup.done = true;
				return Json{{"files", std::move(files)}};
			}
			catch (const AudioFileError& error)
			{
				throw CommandError(std::string("can't write the mixdown: ") + error.what());
			}
		};

		const Json filesSchema = Json::parse(R"({"type": "object", "required": ["files"], "properties": {
			"files": {"type": "array", "items": {"type": "object", "required": ["channel", "name", "path", "frames"],
				"properties": {"channel": {"type": "integer"}, "name": {"type": "string"}, "path": {"type": "string"},
				"frames": {"type": "integer"}}}}}})");
		engine.registerCommand(
			{"mixdown.export", 1,
				"Exports an audio mixdown (reference manual pp. 1479-1502): renders the range from start to end, in "
				"project samples (default: the left to the right locator), offline as transport.locate, "
				"transport.play and audio.render would, without cycle, pre-roll and click, and writes what each of "
				"channels (output busses and audio, instrument, group and effect channels, after their fader and "
				"panner; default: the main mix) puts out into a file of its own in folder, which is created if "
				"needed. The file is called name, or name - <channel name> for several channels, with the format's "
				"extension. format: wav (the default), aiff or flac. sampleFormat: int16, int24 (the default), int32, "
				"float32 or float64; AIFF takes the integer ones, FLAC int16 and int24. sampleRate: the file's, "
				"converted from the project's (the default). dither: TPDF dither for int16 and int24. normalize: the "
				"file's peak in dBFS (-60 to 0). conflicts: unique (the default) adds a number to a name that is "
				"taken, overwrite replaces the file. The transport and its settings are as before afterwards; a device "
				"that runs stops meanwhile, and plugins render in offline mode. Refused while the device runs at "
				"another sample rate than the project. Returns the files written, each with its channel, name, absolute path and "
				"frames.",
				Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["folder", "name"],
					"properties": {"folder": {"type": "string"}, "name": {"type": "string", "minLength": 1},
					"channels": {"type": "array", "items": {"type": "integer"}},
					"start": {"type": "integer", "minimum": 0}, "end": {"type": "integer", "minimum": 0},
					"format": {"type": "string", "enum": ["wav", "aiff", "flac"]},
					"sampleFormat": {"type": "string", "enum": ["int16", "int24", "int32", "float32", "float64"]},
					"sampleRate": {"type": "number"}, "dither": {"type": "boolean"}, "normalize": {"type": "number"},
					"conflicts": {"type": "string", "enum": ["unique", "overwrite"]}}})"),
				filesSchema, false},
			[this, exportRange](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const RenderSnapshot& snapshot = m_snapshots.latest();
				const FileFormat format = formatFrom(parameters, snapshot.sampleRate);
				const time::Samples start = parameters.value("start", m_transport.settings().left);
				const time::Samples end = parameters.value("end", m_transport.settings().right);
				require(end > start, "the range is empty: end must be after start");
				std::vector<ObjectId> objects;
				if (const auto channels = parameters.find("channels"); channels != parameters.end())
					objects = channels->get<std::vector<ObjectId>>();
				else
					for (const RenderBus& bus : snapshot.busses)
						if (bus.mainMix)
							objects.push_back(bus.id);
				require(!objects.empty(), "there is no main mix; give channels");
				const std::set<ObjectId> unique(objects.begin(), objects.end());
				require(unique.size() == objects.size(), "channels names a channel twice");

				const Json project = context.call("project.get");
				const fs::path folder = fs::u8path(parameters.at("folder").get<std::string>());
				const std::string name = parameters.at("name").get<std::string>();
				std::vector<Target> targets(objects.size());
				std::set<fs::path> taken;
				for (size_t i = 0; i < objects.size(); ++i)
				{
					const std::optional<std::string> found = nameOf(project, objects[i]);
					require(found.has_value(), "there is no bus or channel " + std::to_string(objects[i]));
					targets[i].object = objects[i];
					targets[i].name = *found;
					targets[i].path = claim(folder, objects.size() == 1 ? name : name + " - " + targets[i].name,
						extensionOf(format.format), format.overwrite, taken);
					taken.insert(targets[i].path);
				}
				std::error_code error;
				fs::create_directories(folder, error);
				require(!error, "can't create the folder " + folder.u8string() + ": " + error.message());
				return exportRange(context, targets, start, end, 0, format);
			});
		engine.registerCommand(
			{"mixdown.quickExport", 1,
				"Quick audio export (reference manual pp. 1478-1479): writes the main mix of the whole project, from "
				"the start of the first event to the end of the last, with the tail of the effects after it (up to 30 "
				"s, to the last sample a 24-bit file doesn't hold as silence), to the WAV file at path with 24-bit "
				"samples at the project's sample rate, as mixdown.export does, and is refused when it is. An existing file "
				"is replaced. Returns "
				"its absolute path and frames.",
				Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["path"],
					"properties": {"path": {"type": "string", "minLength": 1}}})"),
				Json::parse(R"({"type": "object", "required": ["path", "frames"], "properties": {
					"path": {"type": "string"}, "frames": {"type": "integer"}}})"),
				false},
			[this, exportRange](CommandContext& context)
			{
				const RenderSnapshot& snapshot = m_snapshots.latest();
				std::optional<time::Samples> first;
				for (const RenderChannel& channel : snapshot.channels)
				{
					for (const RenderAudioEvent& event : channel.audioEvents)
						first = std::min(first.value_or(event.start), event.start);
					for (const RenderNote& note : channel.notes)
						first = std::min(first.value_or(note.start), note.start);
				}
				const time::Samples end = projectEnd();
				require(first.has_value() && end > *first, "the project has no events to export");
				FileFormat format;
				format.sampleRate = snapshot.sampleRate;
				format.overwrite = true;
				std::vector<Target> targets(1);
				for (const RenderBus& bus : snapshot.busses)
					if (bus.mainMix)
						targets[0].object = bus.id;
				require(targets[0].object != 0, "there is no main mix");
				targets[0].name = "Main Mix";
				targets[0].path = fs::u8path(context.parameters().at("path").get<std::string>());
				std::error_code error;
				if (targets[0].path.has_parent_path())
					fs::create_directories(targets[0].path.parent_path(), error);
				const Json result = exportRange(context, targets, *first, end,
					static_cast<time::Samples>(std::llround(g_maxTailSeconds * snapshot.sampleRate)), format);
				const Json& file = result.at("files").at(0);
				return Json{{"path", file.at("path")}, {"frames", file.at("frames")}};
			});
	}
}
