// SPDX-License-Identifier: GPL-3.0-or-later
// Exporting audio mixdowns (#115, docs/features/mixdown.md). mixdown.export renders a range offline, block by block
// as audio.render does, and writes what busses and channels put out to audio files; mixdown.quickExport writes the
// main mix of every event, with the tail of the effects. Each file is written twice: first as it is rendered, at
// 32-bit float into a part file next to it, then from there into its format, normalized and dithered.
#include "engine/audio/audioengine.h"
#include "engine/audiofile/audiofile.h"
#include "engine/audiofile/resampler.h"
#include "engine/document/channelformat.h"
#include "engine/document/rendersnapshot.h"
#include "engine/document/serialize.h"
#include "engine/dsp/dither.h"
#include "engine/engine.h"
#include "engine/jobs/jobs.h"
#include "engine/media/backup.h"
#include "engine/plugins/hostmanager.h"
#include "engine/plugins/slotinstances.h"
#include "engine/settings/usersettings.h"

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
			std::optional<BroadcastInfo> broadcast;
		};

		struct PreparedMixdown
		{
			RenderSnapshot snapshot;
			Json project;
			Json settings;
			Json parameters;
			std::set<ObjectId> activeSlots;
		};

		// Keep the upstream closure of the selected outputs, including sends, MIDI routes and child busses.
		// Formats and panners are already compiled, so removing a downstream destination cannot change them.
		RenderSnapshot renderSnapshot(const RenderSnapshot& snapshot, const std::vector<ObjectId>& targets)
		{
			const graph::GraphDesc graph = graph::describe(snapshot);
			std::vector<std::vector<int>> inputs(graph.nodes.size());
			for (const auto* edges : {&graph.audio, &graph.events})
				for (const auto& edge : *edges)
					inputs.at(static_cast<size_t>(edge.to)).push_back(edge.from);
			std::vector<bool> needed(graph.nodes.size(), false);
			std::vector<size_t> pending;
			for (size_t i = 0; i < snapshot.busses.size() + snapshot.channels.size(); ++i)
				if (std::find(targets.begin(), targets.end(), graph.nodes[i].object) != targets.end())
					pending.push_back(i);
			std::set<ObjectId> objects;
			while (!pending.empty())
			{
				const size_t node = pending.back();
				pending.pop_back();
				if (needed[node])
					continue;
				needed[node] = true;
				objects.insert(graph.nodes[node].object);
				for (const int input : inputs[node])
					pending.push_back(static_cast<size_t>(input));
			}
			// Every field as it is, so that one added later isn't left at its default (#1850), but the three lists.
			RenderSnapshot result = snapshot;
			result.busses.clear();
			result.channels.clear();
			result.media.clear();
			std::vector<int> busses(snapshot.busses.size(), -1);
			std::vector<int> channels(snapshot.channels.size(), -1);
			std::vector<int> media(snapshot.media.size(), -1);
			for (size_t i = 0; i < snapshot.busses.size(); ++i)
				if (objects.count(snapshot.busses[i].id) != 0)
				{
					busses[i] = static_cast<int>(result.busses.size());
					result.busses.push_back(snapshot.busses[i]);
				}
			for (size_t i = 0; i < snapshot.channels.size(); ++i)
				if (objects.count(snapshot.channels[i].id) != 0)
				{
					channels[i] = static_cast<int>(result.channels.size());
					result.channels.push_back(snapshot.channels[i]);
				}
			for (auto& bus : result.busses)
				if (bus.parent >= 0)
					bus.parent = busses.at(static_cast<size_t>(bus.parent));
			for (auto& channel : result.channels)
			{
				if (channel.input >= 0)
					channel.input = busses.at(static_cast<size_t>(channel.input));
				if (channel.output.kind != RenderTarget::Kind::None)
				{
					const auto& indices = channel.output.kind == RenderTarget::Kind::Bus ? busses : channels;
					channel.output.index = indices.at(static_cast<size_t>(channel.output.index));
					if (channel.output.index < 0)
						channel.output.kind = RenderTarget::Kind::None;
				}
				for (size_t i = 0; i < channel.midiSendOutputs.size(); ++i)
					if (int& output = channel.midiSendOutputs[i]; output >= 0)
					{
						output = channels.at(static_cast<size_t>(output));
						if (output < 0)
							channel.midiSends.slots[i].on = false; // never reroute an omitted send to the main output
					}
				for (auto& send : channel.sends)
					send.destination = channels.at(static_cast<size_t>(send.destination));
				channel.sends.erase(std::remove_if(channel.sends.begin(), channel.sends.end(),
										[](const RenderSend& send) { return send.destination < 0; }),
					channel.sends.end());
				for (auto& event : channel.audioEvents)
				{
					int& index = media.at(static_cast<size_t>(event.media));
					if (index < 0)
					{
						index = static_cast<int>(result.media.size());
						result.media.push_back(snapshot.media.at(static_cast<size_t>(event.media)));
					}
					event.media = index;
				}
			}
			return result;
		}

		// A bus or channel being exported.
		struct Target
		{
			ObjectId object = 0;
			std::string name;
			int node = -1;
			int channels = 0;
			fs::path path;
			fs::path part;
			fs::path ready;
			std::unique_ptr<Resampler> resampler;
			std::unique_ptr<AudioFileWriter> writer; // of the part file
			std::vector<float> block;                // interleaved
			std::vector<float> converted;
			std::shared_ptr<PreFaderTap> tap; // where its audio comes from when rendered from inside its strip
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
			require(result.sampleRate >= g_lowestSampleRate && result.sampleRate <= g_highestSampleRate,
				"sampleRate must be from " + std::to_string(static_cast<int>(g_lowestSampleRate)) + " to " +
					std::to_string(static_cast<int>(g_highestSampleRate)) + " Hz");
			result.dither = parameters.value("dither", false) &&
				(result.sampleFormat == SampleFormat::Int16 || result.sampleFormat == SampleFormat::Int24);
			if (const auto normalize = parameters.find("normalize"); normalize != parameters.end())
			{
				result.normalize = normalize->get<double>();
				require(*result.normalize >= -60.0 && *result.normalize <= 0.0, "normalize must be from -60 to 0 dB");
			}
			result.overwrite = parameters.value("conflicts", std::string("unique")) == "overwrite";
			if (const auto chunk = parameters.find("broadcastWave"); chunk != parameters.end())
			{
				require(result.format == AudioFormat::Wav, "a Broadcast Wave chunk goes into WAV files only");
				result.broadcast =
					broadcastNow(chunk->value("description", std::string()), chunk->value("author", std::string()),
						chunk->value("reference", std::string()), chunk->value("timeReference", uint64_t{0}));
			}
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

		// A slot's plugin and its channel in project.get's project, for errors: e.g. "Gain and pan on Guitar".
		std::string slotName(const Json& project, ObjectId slot)
		{
			for (const Json& found : project.at("slots"))
				if (found.at("id") == slot)
				{
					const std::string name = found.value("name", std::string());
					return (name.empty() ? found.at("plugin").get<std::string>() : name) + " on " +
						nameOf(project, found.at("channel").get<ObjectId>()).value_or("its channel");
				}
			return "plugin slot " + std::to_string(slot);
		}

		// Second pass: the part file into the target's file, at gain, dithered, at most frames frames.
		void finish(Target& target, const FileFormat& format, int64_t frames, JobContext* job)
		{
			const std::unique_ptr<AudioFileReader> reader = AudioFileReader::open(target.part.u8string());
			WriteOptions options;
			options.format = format.format;
			options.sampleFormat = format.sampleFormat;
			options.sampleRate = format.sampleRate;
			options.channels = target.channels;
			options.broadcast = format.broadcast;
			target.ready =
				createFreeFile(target.path.parent_path(), fs::u8path(target.path.filename().u8string() + ".writing"));
			const std::unique_ptr<AudioFileWriter> writer = AudioFileWriter::create(target.ready.u8string(), options);
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
				if (job != nullptr)
				{
					job->checkCancelled();
					job->progress(0.8 + (0.19 * static_cast<double>(done) / static_cast<double>(frames)), target.name);
				}
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
			if (job != nullptr)
				job->checkCancelled();
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
					fs::remove(target.ready, error);
					if (!done && target.created)
						fs::remove(target.path, error);
				}
			}
		};
	}

	Json AudioEngine::mixdown(const Json& parameters, const Json& project, JobContext* job)
	{
		// Renders [start, end) and tail frames more of the targets, which have objects and paths, into their files:
		// what they output, or with point, what their channel's strip has there (render in place and freeze). The
		// transport, its settings and the device are as before afterwards.
		const auto exportRange = [this, job](CommandContext& context, std::vector<Target>& targets, time::Samples start,
									 time::Samples end, time::Samples tail, const FileFormat& format,
									 std::optional<RenderPoint> point)
		{
			require(m_device != nullptr && m_programs, "there is no audio device; set one with audio.setDevice");
			require(!m_transport.recording() && !m_transport.countingIn(), "stop recording first");
			const double projectRate = m_snapshots.latest().sampleRate;
			// The graph and its processors run at the device's rate, the events at the project's.
			require(m_device->format().sampleRate == projectRate,
				"the device runs at " + std::to_string(std::lround(m_device->format().sampleRate)) +
					" Hz, not at the project's sample rate of " + std::to_string(std::lround(projectRate)) +
					" Hz; set the device to the project's sample rate");
			for (Target& target : targets)
			{
				if (point)
				{
					const std::vector<RenderChannel>& channels = m_snapshots.latest().channels;
					const auto channel = std::find_if(channels.begin(), channels.end(),
						[&target](const RenderChannel& candidate) { return candidate.id == target.object; });
					require(channel != channels.end(), target.name + " is a bus: only channels render from a point");
					// After the panner, in the format it makes: a mono channel's stereo on a stereo output.
					const ChannelFormat panned = pannerOutput(channel->panner);
					target.channels = channelCount(
						*point == RenderPoint::Panner && panned != ChannelFormat::None ? panned : channel->format);
					target.tap = std::make_shared<PreFaderTap>(target.channels);
				}
				else
				{
					const graph::CompiledGraph& graph = m_programs->latest().graph;
					target.node = graph.find(target.object);
					target.channels = target.node >= 0 ? graph.outputChannels(target.node) : 0;
				}
				require(target.channels > 0, target.name + " has no audio");
			}
			Cleanup cleanup{targets};
			try
			{
				for (Target& target : targets)
				{
					target.part = createFreeFile(
						target.path.parent_path(), fs::u8path(target.path.filename().u8string() + ".part"));
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
				// The strips of channels rendered from a point leave their audio in the targets' taps.
				const auto tap = [this, &targets, point](bool on)
				{
					if (!point)
						return;
					m_renderTaps.clear();
					for (const Target& target : targets)
						if (on)
							m_renderTaps[target.object] = {*point, target.tap};
					rebuild();
				};
				const auto restore = [&]
				{
					tap(false);
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
					tap(true);
					sent(m_transport.set(plain));
					if (playing)
						sent(m_transport.stop());
					cue(start, false, m_device->format().blockSize, true);
					sent(m_transport.locate(start));
					play();
					const graph::CompiledGraph& graph = m_programs->latest().graph;
					uint64_t done = 0;
					const auto total = static_cast<uint64_t>(end - start + tail);
					render(total,
						[&](uint32_t frames)
						{
							if (job != nullptr)
							{
								job->checkCancelled();
								job->progress(
									0.8 * static_cast<double>(done) / static_cast<double>(total), "Rendering audio");
							}
							done += frames;
							for (Target& target : targets)
							{
								const auto channels = static_cast<size_t>(target.channels);
								for (size_t c = 0; c < channels; ++c)
								{
									const float* audio = target.tap ? target.tap->channel(static_cast<int>(c))
																	: graph.audio(target.node)[c];
									for (uint32_t i = 0; i < frames; ++i)
										target.block[(i * channels) + c] = audio[i];
								}
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
					finish(target, format, frames, job);
					files.push_back({{"channel", target.object}, {"name", target.name},
						{"path", fs::absolute(target.path).u8string()}, {"frames", frames}});
				}
				if (job != nullptr)
					job->checkCancelled();
				for (const Target& target : targets)
				{
					// Once publication begins this job completes; cancellation is checked before it.
					if (format.overwrite || target.created)
					{
						std::error_code error;
						fs::remove(target.path, error);
					}
					fs::rename(target.ready, target.path);
				}

				cleanup.done = true;
				return Json{{"files", std::move(files)}};
			}
			catch (const AudioFileError& error)
			{
				throw CommandError(std::string("can't write the mixdown: ") + error.what());
			}
		};

		CommandContext context(parameters);
		{
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

			const fs::path folder = fs::u8path(parameters.at("folder").get<std::string>());
			const std::string name = parameters.value("name", std::string());
			const auto path = parameters.find("path"); // mixdown.quickExport's file, the one target's
			std::vector<Target> targets(objects.size());
			std::set<fs::path> taken;
			for (size_t i = 0; i < objects.size(); ++i)
			{
				const std::optional<std::string> found = nameOf(project, objects[i]);
				if (!found)
					throw CommandError("there is no bus or channel " + std::to_string(objects[i]));
				targets[i].object = objects[i];
				targets[i].name = *found;
				targets[i].path = path != parameters.end()
					? fs::u8path(path->get<std::string>())
					: claim(folder, objects.size() == 1 ? name : name + " - " + targets[i].name,
						  extensionOf(format.format), format.overwrite, taken);
				taken.insert(targets[i].path);
			}
			std::optional<RenderPoint> point;
			const std::string from = parameters.value("point", std::string("output"));
			if (from == "source")
				point = RenderPoint::Source;
			else if (from == "inserts")
				point = RenderPoint::Inserts;
			else if (from == "strip")
				point = RenderPoint::Strip;
			else if (from == "panner")
				point = RenderPoint::Panner;
			const auto tail =
				static_cast<time::Samples>(std::llround(parameters.value("tail", 0.0) * snapshot.sampleRate));
			std::error_code error;
			if (!folder.empty())
				fs::create_directories(folder, error);
			require(!error, "can't create the folder " + folder.u8string() + ": " + error.message());
			Cleanup reserved{targets};
			for (Target& target : targets)
			{
				if (!format.overwrite)
				{
					target.path = createFreeFile(folder, target.path.filename());
					target.created = true;
				}
			}
			Json result = exportRange(context, targets, start, end, tail, format, point);
			reserved.done = true;
			return result;
		}
	}
	Jobs::Work AudioEngine::mixdownWork(const Json& parameters, const Json& project)
	{
		require(m_device != nullptr && m_programs, "there is no audio device; set one with audio.setDevice");
		require(!m_transport.recording() && !m_transport.countingIn(), "stop recording first");
		const RenderSnapshot& snapshot = m_snapshots.latest();
		require(m_device->format().sampleRate == snapshot.sampleRate,
			"the device runs at " + std::to_string(std::lround(m_device->format().sampleRate)) +
				" Hz, not at the project sample rate of " + std::to_string(std::lround(snapshot.sampleRate)) + " Hz");
		const FileFormat format = formatFrom(parameters, snapshot.sampleRate);
		Json request = parameters;
		request["start"] = parameters.value("start", m_transport.settings().left);
		if (const auto chunk = request.find("broadcastWave"); chunk != request.end())
		{
			// The preferences' strings by default (reference manual p. 1615), and where the file starts, in samples
			// since midnight at the file's rate.
			chunk->emplace("description", m_settings.get(settings::g_audioBroadcastDescription));
			chunk->emplace("author", m_settings.get(settings::g_audioBroadcastAuthor));
			chunk->emplace("reference", m_settings.get(settings::g_audioBroadcastReference));
			const double seconds =
				(request.at("start").get<double>() / snapshot.sampleRate) + project.value("startTime", 0.0);
			(*chunk)["timeReference"] = std::llround(std::max(seconds, 0.0) * format.sampleRate);
		}
		request["end"] = parameters.value("end", m_transport.settings().right);
		require(request.at("end").get<int64_t>() > request.at("start").get<int64_t>(),
			"the range is empty: end must be after start");
		std::vector<ObjectId> objects;
		if (request.contains("channels"))
			objects = request.at("channels").get<std::vector<ObjectId>>();
		else
			for (const auto& bus : snapshot.busses)
				if (bus.mainMix)
					objects.push_back(bus.id);
		require(!objects.empty(), "there is no main mix; give channels");
		const std::set<ObjectId> unique(objects.begin(), objects.end());
		require(unique.size() == objects.size(), "channels names a channel twice");
		const bool fromPoint = request.value("point", std::string("output")) != "output";
		for (const auto object : objects)
		{
			const std::optional<std::string> name = nameOf(project, object);
			require(name.has_value(), "there is no bus or channel " + std::to_string(object));
			const int node = m_programs->latest().graph.find(object);
			require(node >= 0 && m_programs->latest().graph.outputChannels(node) > 0, "channel has no audio");
			// Before the job creates the folder (#725).
			require(!fromPoint ||
					std::any_of(snapshot.channels.begin(), snapshot.channels.end(),
						[object](const RenderChannel& channel) { return channel.id == object; }),
				name.value_or("") + " is a bus: only channels render from a point");
		}
		request["channels"] = objects;
		const auto prepared = std::make_shared<PreparedMixdown>();
		if (request.contains("renderEvents"))
		{
			// Private render-in-place input: isolate the material before compiling overlaps, MIDI and tails.
			const auto selected = request.at("renderEvents").get<std::set<ObjectId>>();
			Project input = projectFromJson(project);
			for (auto it = input.audioEvents.begin(); it != input.audioEvents.end();)
				if (selected.count(it->first) == 0 && selected.count(it->second.part) == 0)
					it = input.audioEvents.erase(it);
				else
					++it;
			for (auto it = input.midiParts.begin(); it != input.midiParts.end();)
				if (selected.count(it->first) == 0)
					it = input.midiParts.erase(it);
				else
					++it;
			prepared->snapshot = renderSnapshot(*compile(input, snapshot.revision), objects);
		}
		else
		{
			prepared->snapshot = renderSnapshot(snapshot, objects);
		}
		prepared->project = project;
		prepared->settings = m_settings.values();
		prepared->parameters = request;
		std::set<std::string> paths;
		for (const auto& channel : prepared->snapshot.channels)
			for (const auto& path : channel.samples)
				if (!path.empty())
					paths.insert(path);
		// The render needs the plugins that play live, and those still loading, which will (#725).
		for (const auto& channel : prepared->snapshot.channels)
			for (const auto& slot : channel.slots)
				if (m_slots && slot.active &&
					(m_slots->status(slot.id) == "active" || m_slots->status(slot.id) == "loading"))
					prepared->activeSlots.insert(slot.id);
		SampleLoader* samples = &m_sampleLoader;
		const auto lease = std::make_shared<SampleLoader::Lease>(*samples, std::move(paths));
		const auto sampleWait = m_offlineSampleWait.load();
		const auto factory = m_renderPluginFactory;
		const MidiChase chase = m_chase;
		const uint32_t blockSize = m_device->format().blockSize;
		plugins::HostManager* hosts = &m_engine->pluginHosts();
		return [prepared, blockSize, hosts, samples, lease, sampleWait, chase, factory](JobContext& job)
		{
			(void)lease; // keeps this snapshot's sample reads alive through completion
			job.checkCancelled();
			rt::SnapshotPublisher<RenderSnapshot> snapshots(std::make_unique<RenderSnapshot>(prepared->snapshot));
			settings::Store settings(settings::userFormat());
			settings.set(prepared->settings);
			std::vector<std::unique_ptr<audio::Backend>> backends;
			backends.push_back(std::make_unique<audio::DummyBackend>());
			AudioEngine renderer(snapshots, settings, std::move(backends), {});
			renderer.m_renderSamples = samples;
			renderer.m_renderJob = &job;
			renderer.setOfflineSampleWait(sampleWait);
			renderer.setChase(chase);
			renderer.m_multiProcessing = false;
			renderer.m_threads = 1;
			renderer.m_slots =
				std::make_unique<plugins::SlotInstances>(*hosts, [] {}, [](uint64_t) { return true; }, factory);
			renderer.m_slots->setProcessMode(plugin::ProcessMode::Offline); // before they are made and activated
			renderer.setDevice({{"blockSize", blockSize}, {"sampleRate", prepared->snapshot.sampleRate},
				{"captureSeconds", 0}, {"inputChannels", 0}});
			// Wait on the plugin job queue, then publish their ready processors in this render's graph.
			hosts->run([] {}).get();
			job.checkCancelled();
			renderer.rebuild();
			for (const ObjectId slot : prepared->activeSlots)
				require(renderer.m_slots->status(slot) == "active",
					"the plugin " + slotName(prepared->project, slot) + " couldn't be made for the render");
			return renderer.mixdown(prepared->parameters, prepared->project, &job);
		};
	}

	void AudioEngine::registerMixdownCommands(Engine& engine)
	{
		engine.registerCommand(
			{"mixdown.export", 2,
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
				"taken, overwrite replaces the file. broadcastWave gives WAV files a Broadcast Wave chunk: its "
				"description, author and reference default to the preferences audio.broadcastDescription, "
				"audio.broadcastAuthor and audio.broadcastReference and are cut to 256, 32 and 32 bytes; it has the "
				"date and time of the export and as its time reference where the file starts plus the project's start "
				"time. point renders channels from inside their strip instead of after "
				"the fader and panner (output, the default), in the channel's format, as render in place and freeze "
				"do: source is what plays on the channel, before its pre section and plugin slots; inserts is after "
				"the slots, before the strip's other sections; strip is after those, before the fader; panner is after "
				"the panner, in the format it makes, without the fader and the mute. tail renders "
				"up to that many seconds (0 to 60) after end, ending after the last sample a 24-bit file doesn't hold "
				"as silence. The transport and its settings are as before afterwards; a device "
				"continues running, and separate plugin instances render in offline mode. Refused while the device "
				"runs at "
				"another sample rate than the project. Returns the files written, each with its channel, name, "
				"absolute path and frames in the job result. Returns a cancellable job immediately; progress and "
				"completion "
				"are job.progress and job.finished events. Rendering uses a private starting snapshot and leaves "
				"playback running.",
				Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["folder", "name"],
					"properties": {"folder": {"type": "string"}, "name": {"type": "string", "minLength": 1},
					"channels": {"type": "array", "items": {"type": "integer"}},
					"start": {"type": "integer", "minimum": 0}, "end": {"type": "integer", "minimum": 0},
					"format": {"type": "string", "enum": ["wav", "aiff", "flac"]},
					"sampleFormat": {"type": "string", "enum": ["int16", "int24", "int32", "float32", "float64"]},
					"sampleRate": {"type": "number"}, "dither": {"type": "boolean"}, "normalize": {"type": "number"},
					"conflicts": {"type": "string", "enum": ["unique", "overwrite"]},
					"broadcastWave": {"type": "object", "additionalProperties": false, "properties": {
						"description": {"type": "string"}, "author": {"type": "string"},
						"reference": {"type": "string"}}},
											"point": {"type": "string", "enum": ["output", "panner", "strip", "inserts", "source"]},
											"tail": {"type": "number", "minimum": 0, "maximum": 60}}})"),
				{{"type", "object"}, {"properties", {{"job", Jobs::schema()}}}, {"required", {"job"}}}, false},
			[this, &engine](CommandContext& context)
			{
				return engine.startJob(
					"mixdown.export", mixdownWork(context.parameters(), context.call("project.get")));
			});
		engine.registerCommand(
			{"mixdown.quickExport", 2,
				"Quick audio export (reference manual pp. 1478-1479): writes the main mix of the whole project, from "
				"the start of the first event to the end of the last, with the tail of the effects after it (up to 30 "
				"s, to the last sample a 24-bit file doesn't hold as silence), to the WAV file at path with 24-bit "
				"samples at the project's sample rate, as mixdown.export does, with a private render that leaves the "
				"live graph, plugins and transport as they are. Refused for a project without events, while recording, "
				"without an audio device, and while the device runs at another sample rate than the project. An "
				"existing file is replaced. Returns a cancellable job immediately; progress and completion are "
				"job.progress and job.finished events, and the job's result is the file's absolute path and frames. "
				"The job fails if a plugin that plays or is still loading live can't be made for the render.",
				Json::parse(R"({"type": "object", "additionalProperties": false, "required": ["path"],
					"properties": {"path": {"type": "string", "minLength": 1}}})"),
				{{"type", "object"}, {"properties", {{"job", Jobs::schema()}}}, {"required", {"job"}}}, false},
			[this, &engine](CommandContext& context)
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
				if (!first || end <= *first)
					throw CommandError("the project has no events to export");
				// mixdown.export's private render as a job, so that the command thread doesn't wait for the plugin
				// hosts while its plugins are made and destroyed (#718).
				const fs::path path = fs::u8path(context.parameters().at("path").get<std::string>());
				const Json request = {{"path", path.u8string()}, {"folder", path.parent_path().u8string()},
					{"start", *first}, {"end", end}, {"tail", g_maxTailSeconds}, {"conflicts", "overwrite"}};
				return engine.startJob("mixdown.quickExport",
					[work = mixdownWork(request, context.call("project.get"))](JobContext& job)
					{
						const Json file = work(job).at("files").at(0);
						return Json{{"path", file.at("path")}, {"frames", file.at("frames")}};
					});
			});
	}
}
