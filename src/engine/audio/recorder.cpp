// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/recorder.h"

#include "engine/document/serialize.h"
#include "engine/media/backup.h"
#include "platform/log.h"
#include "platform/rt/realtime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <ctime>
#include <exception>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

namespace gnubase::engine
{
	namespace
	{
		namespace fs = std::filesystem;

		// How long the disk thread sleeps when it finds no chunk.
		constexpr std::chrono::milliseconds g_idle{2};
		// The chunks hold this many frames in all, at least g_minChunks of them: over a second at 48 kHz.
		constexpr uint32_t g_queuedFrames = 65536;
		constexpr uint32_t g_minChunks = 32;
		// MIDI chunks: one per block while MIDI tracks record or MIDI input comes, each with up to this many events.
		constexpr uint32_t g_midiChunks = 256;
		constexpr size_t g_eventsPerChunk = 256;

		using EventType = graph::ProcessEvent::Type;

		// A track name as a file name: characters that some file system doesn't take become "_".
		std::string fileName(const std::string& name)
		{
			std::string result = name;
			for (char& c : result)
				if (static_cast<unsigned char>(c) < 32 || std::string_view("<>:\"/\\|?*").find(c) != std::string::npos)
					c = '_';
			while (!result.empty() && (result.back() == '.' || result.back() == ' '))
				result.pop_back(); // Windows drops them
			return result.empty() ? "Audio" : result;
		}

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

		// The local date and time now as the broadcast chunk takes them: yyyy-mm-dd and hh:mm:ss.
		std::pair<std::string, std::string> originationNow()
		{
			const std::time_t now = std::time(nullptr);
			std::tm local{};
#ifdef _WIN32
			localtime_s(&local, &now);
#else
			localtime_r(&now, &local);
#endif
			std::array<char, 16> date{};
			std::array<char, 16> time{};
			std::strftime(date.data(), date.size(), "%Y-%m-%d", &local);
			std::strftime(time.data(), time.size(), "%H:%M:%S", &local);
			return {date.data(), time.data()};
		}
	}

	WriteOptions recordFormat(const Project& project)
	{
		WriteOptions options;
		const std::string& type = project.setup.recordFileType;
		if (type == "aiff")
			options.format = AudioFormat::Aiff;
		else if (type == "flac")
			options.format = AudioFormat::Flac;
		else
			options.format = AudioFormat::Wav;
		const std::string& depth = project.setup.recordBitDepth;
		if (depth == "int16")
			options.sampleFormat = SampleFormat::Int16;
		else if (depth == "int24")
			options.sampleFormat = SampleFormat::Int24;
		else if (depth == "int32")
			options.sampleFormat = SampleFormat::Int32;
		else if (depth == "float64")
			options.sampleFormat = SampleFormat::Float64;
		else
			options.sampleFormat = SampleFormat::Float32;
		options.sampleRate = project.sampleRate;
		if (type == "broadcastWav")
		{
			// The recorder adds the description, the origination time and the first sample's project position.
			BroadcastInfo broadcast;
			broadcast.originator = "GNUbase";
			broadcast.timeReference = static_cast<uint64_t>(std::llround(project.setup.startTime * project.sampleRate));
			options.broadcast = broadcast;
		}
		return options;
	}

	MidiRecordFilter MidiRecordFilter::fromJson(const Json& json)
	{
		MidiRecordFilter filter;
		for (const Json& type : json.value("types", Json::array()))
		{
			const auto found = std::find(g_midiFilterTypes.begin(), g_midiFilterTypes.end(), type.get<std::string>());
			if (found == g_midiFilterTypes.begin())
				filter.types.set(static_cast<size_t>(EventType::NoteOn)).set(static_cast<size_t>(EventType::NoteOff));
			else if (found != g_midiFilterTypes.end())
				filter.types.set(static_cast<size_t>(found - g_midiFilterTypes.begin()) + 1);
		}
		for (const Json& channel : json.value("channels", Json::array()))
			if (const int number = channel.get<int>(); number >= 1 && number <= 16)
				filter.channels.set(static_cast<size_t>(number - 1));
		for (const Json& controller : json.value("controllers", Json::array()))
			if (const int number = controller.get<int>(); number >= 0 && number < 128)
				filter.controllers.set(static_cast<size_t>(number));
		return filter;
	}

	bool MidiRecordFilter::records(const graph::ProcessEvent& event) const
	{
		return !types.test(static_cast<size_t>(event.type)) && !channels.test(event.channel % 16U) &&
			!(event.type == EventType::Controller && controllers.test(event.key % 128U));
	}

	void MidiTakeBuilder::add(time::Samples position, const graph::ProcessEvent& event)
	{
		m_last = std::max(m_last, position);
		const uint8_t channel = event.channel % 16U;
		if (event.type == EventType::NoteOn || event.type == EventType::NoteOff)
		{
			Sounding& sounding = m_sounding[(size_t{channel} * 128) + (event.key % 128U)];
			if (sounding.position >= 0)
				m_notes.push_back({{"position", sounding.position},
					{"length", std::max<time::Samples>(position - sounding.position, 1)}, {"pitch", event.key % 128U},
					{"velocity", sounding.velocity / g_maxVelocity},
					{"releaseVelocity",
						(event.type == EventType::NoteOff ? event.velocity : g_defaultReleaseVelocity) / g_maxVelocity},
					{"channel", channel}});
			sounding.position = event.type == EventType::NoteOn ? position : -1;
			sounding.velocity = event.velocity;
			return;
		}
		Controller controller;
		controller.channel = channel;
		controller.value = event.value;
		switch (event.type)
		{
		case EventType::Controller:
			controller.type = ControllerType::Control;
			controller.number = event.key % 128U;
			break;
		case EventType::PitchBend:
			controller.type = ControllerType::PitchBend;
			break;
		case EventType::ChannelPressure:
			controller.type = ControllerType::ChannelPressure;
			break;
		case EventType::PolyPressure:
			controller.type = ControllerType::PolyPressure;
			controller.number = event.key % 128U;
			break;
		case EventType::Program:
			controller.type = ControllerType::Program;
			controller.number = event.key % 128U;
			break;
		default: // SysEx and parameter changes don't come from MIDI input
			return;
		}
		Json json = toJson(controller);
		json.erase("id");
		json["position"] = position;
		m_controllers.push_back(std::move(json));
	}

	void MidiTakeBuilder::endNotes(time::Samples position)
	{
		graph::ProcessEvent off;
		off.type = EventType::NoteOff;
		off.velocity = g_defaultReleaseVelocity;
		for (size_t index = 0; index < m_sounding.size(); ++index)
			if (m_sounding[index].position >= 0)
			{
				off.channel = static_cast<uint8_t>(index / 128);
				off.key = static_cast<uint8_t>(index % 128);
				add(position, off);
			}
	}

	Recorder::Recorder(Notify notify) : m_notify(std::move(notify))
	{
		m_thread = std::thread([this] { run(); });
	}

	Recorder::~Recorder()
	{
		{
			const std::scoped_lock lock(m_mutex);
			m_notify = nullptr; // the engine is going: the files stay, but nothing is added to the project
			try
			{
				if (m_midiRecording)
					endMidi();
				if (m_recording)
					end();
			}
			catch (...) // NOLINT(bugprone-empty-catch): out of memory while closing; the files stay as they are
			{
			}
			m_stopping = true;
		}
		m_wake.notify_all();
		m_thread.join();
	}

	void Recorder::prepare(int channels, uint32_t maxFrames)
	{
		rt::assertNotRealtime("engine::Recorder::prepare");
		const std::scoped_lock lock(m_mutex);
		drain(); // what the audio thread sent before it stopped
		if (m_midiRecording)
			endMidi();
		if (m_recording)
			end();
		m_channels = std::max(channels, 0);
		m_maxFrames = maxFrames;
		m_sending = false;
		m_midiSending = false;
		m_chunks.clear();
		m_midiChunks.clear();
		m_free.reset();
		m_midiFree.reset();
		m_filled.reset();
		m_history.assign(static_cast<size_t>(m_next.preRecordFrames) * static_cast<size_t>(m_channels), 0.0f);
		m_historyFrames = 0;
		if (maxFrames == 0)
			return;
		const uint32_t count = std::max(g_minChunks, g_queuedFrames / maxFrames);
		m_chunks.resize(count);
		m_midiChunks.resize(g_midiChunks);
		m_free = std::make_unique<rt::SpscQueue<Chunk*>>(count);
		m_midiFree = std::make_unique<rt::SpscQueue<Chunk*>>(g_midiChunks);
		m_filled = std::make_unique<rt::SpscQueue<Chunk*>>(count + g_midiChunks);
		for (Chunk& each : m_chunks)
		{
			each.samples.resize(static_cast<size_t>(maxFrames) * static_cast<size_t>(m_channels));
			m_free->tryPush(&each);
		}
		for (Chunk& each : m_midiChunks)
		{
			each.midi = true;
			each.events.resize(g_eventsPerChunk);
			m_midiFree->tryPush(&each);
		}
	}

	void Recorder::setup(RecordSetup setup)
	{
		rt::assertNotRealtime("engine::Recorder::setup");
		const std::scoped_lock lock(m_mutex);
		drain(); // a recording that the audio thread started already starts with the setup it started with
		if (setup.preRecordFrames != m_next.preRecordFrames)
		{
			m_history.assign(static_cast<size_t>(setup.preRecordFrames) * static_cast<size_t>(m_channels), 0.0f);
			m_historyFrames = 0;
		}
		m_armed = !setup.tracks.empty();
		m_midiArmed = !setup.midiTracks.empty();
		m_next = std::move(setup);
	}

	std::vector<RetrospectiveEvent> Recorder::retrospective(ObjectId channel)
	{
		rt::assertNotRealtime("engine::Recorder::retrospective");
		const std::scoped_lock lock(m_mutex);
		drain();
		const auto found = m_retrospective.find(channel);
		if (found == m_retrospective.end())
			return {};
		found->second.taken = true;
		return {found->second.events.begin(), found->second.events.end()};
	}

	void Recorder::clearRetrospective(ObjectId channel)
	{
		rt::assertNotRealtime("engine::Recorder::clearRetrospective");
		const std::scoped_lock lock(m_mutex);
		drain();
		if (channel == 0)
			m_retrospective.clear();
		else
			m_retrospective.erase(channel);
	}

	void Recorder::process(const audio::DeviceBlock& block, const PlayedPositions& played, int64_t offset,
		const MidiTap::Sequencers& sequencers) noexcept
	{
		if (!m_filled)
			return;
		processMidi(block, played, sequencers); // first: a MIDI recording ends before the audio one it goes with
		if (!m_armed.load(std::memory_order_relaxed))
		{
			// Disarmed while recording: the disk thread ends the recording on a chunk that isn't recorded.
			if (m_sending && send(block, 0, 0, -1))
				m_sending = false;
			return;
		}
		// Runs of frames that are recorded at consecutive positions, or not recorded, one chunk each.
		const auto deviceFrame = static_cast<int64_t>(block.position);
		uint32_t start = 0;
		time::Samples first = played.recordedAt(deviceFrame - offset);
		for (uint32_t i = 1; i <= block.frames; ++i)
		{
			const time::Samples position = i < block.frames ? played.recordedAt(deviceFrame + i - offset) : -1;
			if (i < block.frames && (first < 0 ? position < 0 : position == first + (i - start)))
				continue;
			if (send(block, start, i - start, first))
				m_sending = first >= 0;
			else
				m_dropped.fetch_add(i - start, std::memory_order_relaxed);
			start = i;
			first = position;
		}
	}

	bool Recorder::send(
		const audio::DeviceBlock& block, uint32_t offset, uint32_t frames, time::Samples position) noexcept
	{
		Chunk* chunk = nullptr;
		if (!m_free->tryPop(chunk))
			return false;
		chunk->deviceFrame = block.position + offset;
		chunk->position = position;
		chunk->frames = frames;
		size_t n = 0;
		for (uint32_t i = 0; i < frames; ++i)
			for (int c = 0; c < m_channels; ++c)
				chunk->samples[n++] = block.inputs[c][offset + i];
		m_filled->tryPush(chunk); // there is room for every chunk
		return true;
	}

	void Recorder::processMidi(
		const audio::DeviceBlock& block, const PlayedPositions& played, const MidiTap::Sequencers& sequencers) noexcept
	{
		// Runs of frames recorded at consecutive positions, or not recorded, as for audio but without the record
		// offset: MIDI input belongs where the transport plays as it comes. A run is sent only for its input, where
		// the positions jump (a recording starts, a cycle lap) or where a recording ends: the disk thread counts
		// the frames in between, so a recording takes few chunks.
		const bool armed = m_midiArmed.load(std::memory_order_relaxed);
		const auto deviceFrame = static_cast<int64_t>(block.position);
		const auto recordedAt = [&](uint32_t i)
		{ return armed ? played.recordedAt(deviceFrame + i) : time::Samples{-1}; };
		const auto hasInput = [&sequencers](uint32_t from, uint32_t to)
		{
			for (const auto& entry : sequencers)
				for (const graph::ProcessEvent& event : entry.second->input())
					if (event.offset >= from && event.offset < to)
						return true;
			return false;
		};
		uint32_t start = 0;
		time::Samples first = recordedAt(0);
		for (uint32_t i = 1; i <= block.frames; ++i)
		{
			const time::Samples position = i < block.frames ? recordedAt(i) : -1;
			if (i < block.frames && (first < 0 ? position < 0 : position == first + (i - start)))
				continue;
			const auto runFrame = static_cast<uint64_t>(deviceFrame) + start;
			const bool goesOn = first >= 0 && m_midiSending && first == m_midiNext && runFrame == m_midiNextFrame;
			const bool needed = first >= 0 ? !goesOn : m_midiSending;
			if (needed || hasInput(start, i))
			{
				if (sendMidi(block, played, sequencers, start, i - start, first))
					m_midiSending = first >= 0;
				else if (needed)
					m_midiSending = false; // the next recorded run is sent as a start
			}
			m_midiNext = first >= 0 ? first + (i - start) : -1;
			m_midiNextFrame = runFrame + (i - start);
			start = i;
			first = position;
		}
	}

	bool Recorder::sendMidi(const audio::DeviceBlock& block, const PlayedPositions& played,
		const MidiTap::Sequencers& sequencers, uint32_t offset, uint32_t frames, time::Samples position) noexcept
	{
		Chunk* chunk = nullptr;
		if (!m_midiFree->tryPop(chunk))
			return false;
		chunk->deviceFrame = block.position + offset;
		chunk->position = position;
		chunk->frames = frames;
		chunk->eventCount = 0;
		for (const auto& [channel, sequencer] : sequencers)
			for (const graph::ProcessEvent& event : sequencer->input())
			{
				// ponytail: events beyond a chunk's room are dropped; a continuation chunk if a block ever has more.
				if (event.offset < offset || event.offset - offset >= frames || event.type == EventType::SysEx ||
					event.type == EventType::Parameter || chunk->eventCount == chunk->events.size())
					continue;
				chunk->events[chunk->eventCount++] = {channel, event.offset - offset,
					played.at(static_cast<int64_t>(block.position + event.offset)), event};
			}
		m_filled->tryPush(chunk); // there is room for every chunk
		return true;
	}

	void Recorder::run()
	{
		std::unique_lock lock(m_mutex);
		while (!m_stopping)
		{
			// ponytail: polls, since the audio thread can't wake it; a semaphore if 2 ms ever matters.
			if (!drain())
				m_wake.wait_for(lock, g_idle);
		}
	}

	bool Recorder::drain()
	{
		bool handled = false;
		Chunk* chunk = nullptr;
		while (m_filled && m_filled->tryPop(chunk))
		{
			if (chunk->midi)
			{
				handleMidi(*chunk);
				m_midiFree->tryPush(chunk);
			}
			else
			{
				handle(*chunk);
				m_free->tryPush(chunk);
			}
			handled = true;
		}
		return handled;
	}

	void Recorder::handle(const Chunk& chunk)
	{
		if (chunk.position < 0)
		{
			if (m_recording)
				end();
			keepHistory(chunk);
			return;
		}
		if (!m_recording)
		{
			begin(chunk);
		}
		else if (const Take& take = m_takes.back(); chunk.position != take.position + take.length)
		{
			// Chunks the audio thread lost leave a gap in the device frames and the positions alike: silence there.
			// Otherwise playback jumped, at the end of a cycle lap or a locate: a new take.
			const time::Samples takeEnd = take.position + take.length;
			const auto gap = static_cast<time::Samples>(chunk.deviceFrame - m_deviceEnd);
			if (chunk.position > takeEnd && chunk.position - takeEnd == gap && chunk.deviceFrame > m_deviceEnd)
			{
				const std::vector<float> silence(static_cast<size_t>(gap) * static_cast<size_t>(m_channels), 0.0f);
				write(silence.data(), static_cast<uint32_t>(gap));
				m_takes.back().length += gap;
			}
			else
			{
				m_takes.push_back({chunk.position, m_written, 0});
			}
		}
		write(chunk.samples.data(), chunk.frames);
		m_takes.back().length += chunk.frames;
		m_deviceEnd = chunk.deviceFrame + chunk.frames;
	}

	void Recorder::begin(const Chunk& chunk)
	{
		m_current = m_next;
		m_recording = true;
		m_files.clear();
		m_takes.clear();
		m_written = 0;
		if (!m_current.tracks.empty())
		{
			std::error_code error;
			fs::create_directories(m_current.folder, error);
			if (error)
				fail("can't create the record folder " + m_current.folder.u8string() + ": " + error.message());
		}
		// Audio pre-record: the frames that came in just before, if none is missing in between.
		const size_t capacity = m_channels > 0 ? m_history.size() / static_cast<size_t>(m_channels) : 0;
		const size_t before = capacity > 0 && m_historyEnd == chunk.deviceFrame ? m_historyFrames : 0;
		if (m_current.format.broadcast)
		{
			// The project start time plus the first frame's position, pre-recorded input included, not before midnight.
			BroadcastInfo& broadcast = *m_current.format.broadcast;
			std::tie(broadcast.originationDate, broadcast.originationTime) = originationNow();
			const auto first = static_cast<time::Samples>(broadcast.timeReference) + chunk.position -
				static_cast<time::Samples>(before);
			broadcast.timeReference = static_cast<uint64_t>(std::max<time::Samples>(first, 0));
		}
		for (const RecordTrack& track : m_current.tracks)
		{
			try
			{
				const std::string path = createFreeFile(
					m_current.folder, fs::u8path(fileName(track.name) + extensionOf(m_current.format.format)))
											 .u8string();
				WriteOptions options = m_current.format;
				options.channels = static_cast<int>(track.ports.size());
				if (options.broadcast)
					options.broadcast->description = track.name.substr(0, 256);
				m_files.push_back({track, path, AudioFileWriter::create(path, options)});
			}
			catch (const std::exception& error)
			{
				fail("can't record track " + track.name + ": " + error.what());
			}
		}

		for (size_t done = 0; done < before;)
		{
			const size_t index = (m_historyStart + done) % capacity;
			const size_t frames = std::min(before - done, capacity - index);
			write(m_history.data() + (index * static_cast<size_t>(m_channels)), static_cast<uint32_t>(frames));
			done += frames;
		}
		m_historyFrames = 0;
		m_takes.push_back({chunk.position, m_written, 0});
	}

	void Recorder::write(const float* samples, uint32_t frames)
	{
		const auto channels = static_cast<size_t>(m_channels);
		for (auto file = m_files.begin(); file != m_files.end();)
		{
			const std::vector<int>& ports = file->track.ports;
			m_trackSamples.assign(static_cast<size_t>(frames) * ports.size(), 0.0f);
			for (size_t c = 0; c < ports.size(); ++c)
				if (ports[c] >= 0 && static_cast<size_t>(ports[c]) < channels)
					for (size_t i = 0; i < frames; ++i)
						m_trackSamples[(i * ports.size()) + c] =
							samples[(i * channels) + static_cast<size_t>(ports[c])];
			try
			{
				file->writer->write(m_trackSamples.data(), frames);
				++file;
			}
			catch (const std::exception& error)
			{
				fail("can't write " + file->path + ": " + error.what());
				file = m_files.erase(file);
			}
		}
		m_written += frames;
	}

	void Recorder::end()
	{
		m_recording = false;
		Json tracks = Json::array();
		for (File& file : m_files)
		{
			try
			{
				file.writer->close();
			}
			catch (const std::exception& error)
			{
				fail("can't write " + file.path + ": " + error.what());
				continue;
			}
			Json takes = Json::array();
			for (const Take& take : m_takes)
				if (take.length > 0)
					takes.push_back({{"position", take.position}, {"offset", take.offset}, {"length", take.length}});
			tracks.push_back({{"track", file.track.track}, {"path", file.path}, {"takes", std::move(takes)}});
		}
		m_files.clear();
		Json data = {
			{"project", m_current.project}, {"mode", g_audioRecordModes.at(static_cast<size_t>(m_current.mode))}};
		if (!m_pendingMidi.empty())
		{
			for (Json& track : m_pendingMidi)
				tracks.push_back(std::move(track));
			m_pendingMidi = Json::array();
			data["midiMode"] = g_midiRecordModes.at(static_cast<size_t>(m_pendingMidiSetup.midiMode));
			data["snapToBars"] = m_pendingMidiSetup.snapToBars;
		}
		data["tracks"] = std::move(tracks);
		if (!data.at("tracks").empty() && m_notify)
			m_notify("recording.finished", data);
	}

	void Recorder::handleMidi(const Chunk& chunk)
	{
		// A take's offset is the device frame it started at: the audio thread leaves out the chunks in between that
		// have no input, so it reaches to where the next chunk starts, or at least past the last one.
		const auto frame = static_cast<time::Samples>(chunk.deviceFrame);
		if (m_midiRecording)
		{
			Take& take = m_midiTakes.back();
			if (chunk.position < 0 || chunk.position != take.position + (frame - take.offset))
				take.length = std::max(take.length, frame - take.offset);
		}
		if (chunk.position < 0)
		{
			if (m_midiRecording)
				endMidi();
			for (size_t i = 0; i < chunk.eventCount; ++i)
				keepRetrospective(chunk.events[i], chunk.deviceFrame);
			return;
		}
		if (!m_midiRecording)
		{
			beginMidi(chunk);
		}
		else if (const Take& take = m_midiTakes.back(); chunk.position != take.position + (frame - take.offset))
		{
			// Playback jumped, at the end of a cycle lap or a locate: the notes that sound end with the take before.
			for (auto& [channel, builder] : m_midiBuilders)
				builder.endNotes(take.position + take.length);
			m_midiTakes.push_back({chunk.position, frame, 0});
		}
		Take& take = m_midiTakes.back();
		take.length = std::max(take.length, frame + chunk.frames - take.offset);
		for (size_t i = 0; i < chunk.eventCount; ++i)
		{
			const MidiInput& input = chunk.events[i];
			const auto builder = m_midiBuilders.find(input.channel);
			if (builder == m_midiBuilders.end())
				keepRetrospective(input, chunk.deviceFrame);
			else if (m_midiCurrent.midiFilter.records(input.event))
				builder->second.add(chunk.position + input.offset, input.event);
		}
	}

	void Recorder::beginMidi(const Chunk& chunk)
	{
		m_midiCurrent = m_next;
		m_midiRecording = true;
		m_midiTakes.assign(1, {chunk.position, static_cast<time::Samples>(chunk.deviceFrame), 0});
		m_midiBuilders.clear();
		for (const MidiRecordTrack& track : m_midiCurrent.midiTracks)
			m_midiBuilders[track.channel];
		// The catch range: input that came just before recording started belongs to its start.
		if (m_midiCurrent.catchFrames <= 0)
			return;
		for (auto& [channel, builder] : m_midiBuilders)
		{
			const auto found = m_retrospective.find(channel);
			if (found == m_retrospective.end())
				continue;
			for (const RetrospectiveEvent& kept : found->second.events)
				if (kept.deviceFrame < chunk.deviceFrame &&
					kept.deviceFrame + static_cast<uint64_t>(m_midiCurrent.catchFrames) >= chunk.deviceFrame &&
					m_midiCurrent.midiFilter.records(kept.event))
					builder.add(chunk.position, kept.event);
		}
	}

	void Recorder::endMidi()
	{
		m_midiRecording = false;
		const Take& last = m_midiTakes.back();
		Json takes = Json::array();
		for (const Take& take : m_midiTakes)
			if (take.length > 0)
				takes.push_back({{"position", take.position}, {"length", take.length}});
		Json tracks = Json::array();
		for (const MidiRecordTrack& track : m_midiCurrent.midiTracks)
		{
			MidiTakeBuilder& builder = m_midiBuilders[track.channel];
			builder.endNotes(last.position + last.length);
			if (!builder.empty() && !takes.empty())
				tracks.push_back({{"track", track.track}, {"takes", takes}, {"notes", builder.notes()},
					{"controllers", builder.controllers()}});
		}
		m_midiBuilders.clear();
		if (tracks.empty())
			return;
		if (m_recording)
		{
			// The audio recording ends later, by the record offset: one recording.add for both.
			for (Json& track : tracks)
				m_pendingMidi.push_back(std::move(track));
			m_pendingMidiSetup = m_midiCurrent;
			return;
		}
		if (m_notify)
			m_notify("recording.finished",
				{{"project", m_midiCurrent.project},
					{"midiMode", g_midiRecordModes.at(static_cast<size_t>(m_midiCurrent.midiMode))},
					{"snapToBars", m_midiCurrent.snapToBars}, {"tracks", std::move(tracks)}});
	}

	void Recorder::keepRetrospective(const MidiInput& input, uint64_t deviceFrame)
	{
		if (m_next.retrospective == 0)
			return;
		Retrospective& buffer = m_retrospective[input.channel];
		const uint64_t frame = deviceFrame + input.offset;
		// Input after the buffer was inserted, or in stop mode after a pause, starts it afresh.
		if (buffer.taken ||
			(input.played < 0 && !buffer.events.empty() && buffer.events.back().position < 0 &&
				frame > buffer.events.back().deviceFrame + static_cast<uint64_t>(m_next.pauseFrames)))
			buffer.events.clear();
		buffer.taken = false;
		buffer.events.push_back({frame, input.played, input.event});
		while (buffer.events.size() > m_next.retrospective)
			buffer.events.pop_front();
	}

	void Recorder::keepHistory(const Chunk& chunk)
	{
		const size_t capacity = m_channels > 0 ? m_history.size() / static_cast<size_t>(m_channels) : 0;
		if (capacity == 0 || chunk.frames == 0)
			return;
		if (chunk.deviceFrame != m_historyEnd)
			m_historyFrames = 0; // frames are missing in between
		const auto channels = static_cast<size_t>(m_channels);
		for (size_t i = 0; i < chunk.frames; ++i)
		{
			const size_t index = (m_historyStart + m_historyFrames) % capacity;
			std::copy_n(chunk.samples.data() + (i * channels), channels, m_history.data() + (index * channels));
			if (m_historyFrames < capacity)
				++m_historyFrames;
			else
				m_historyStart = (m_historyStart + 1) % capacity;
		}
		m_historyEnd = chunk.deviceFrame + chunk.frames;
	}

	void Recorder::fail(const std::string& reason)
	{
		logging::error("Recording: {}", reason);
		if (m_notify)
			m_notify("recording.failed", {{"reason", reason}});
	}
}
