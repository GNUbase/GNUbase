// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/recorder.h"

#include "engine/media/backup.h"
#include "platform/log.h"
#include "platform/rt/realtime.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <string_view>
#include <system_error>
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
	}

	WriteOptions recordFormat(const Project& project)
	{
		WriteOptions options;
		options.format = AudioFormat::Wav;
		options.sampleFormat = SampleFormat::Float32;
		options.sampleRate = project.sampleRate;
		return options;
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
		if (m_recording)
			end();
		m_channels = std::max(channels, 0);
		m_maxFrames = maxFrames;
		m_sending = false;
		m_chunks.clear();
		m_free.reset();
		m_filled.reset();
		m_history.assign(static_cast<size_t>(m_next.preRecordFrames) * static_cast<size_t>(m_channels), 0.0f);
		m_historyFrames = 0;
		if (maxFrames == 0)
			return;
		const uint32_t count = std::max(g_minChunks, g_queuedFrames / maxFrames);
		m_chunks.resize(count);
		m_free = std::make_unique<rt::SpscQueue<Chunk*>>(count);
		m_filled = std::make_unique<rt::SpscQueue<Chunk*>>(count);
		for (Chunk& each : m_chunks)
		{
			each.samples.resize(static_cast<size_t>(maxFrames) * static_cast<size_t>(m_channels));
			m_free->tryPush(&each);
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
		m_next = std::move(setup);
	}

	void Recorder::process(const audio::DeviceBlock& block, const PlayedPositions& played, int64_t offset) noexcept
	{
		if (!m_filled)
			return;
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
		float* out = chunk->samples.data();
		for (uint32_t i = 0; i < frames; ++i)
			for (int c = 0; c < m_channels; ++c)
				*out++ = block.inputs[c][offset + i];
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
			handle(*chunk);
			m_free->tryPush(chunk);
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
		for (const RecordTrack& track : m_current.tracks)
		{
			try
			{
				const std::string path =
					createFreeFile(m_current.folder, fs::u8path(fileName(track.name) + ".wav")).u8string();
				WriteOptions options = m_current.format;
				options.channels = static_cast<int>(track.ports.size());
				m_files.push_back({track, path, AudioFileWriter::create(path, options)});
			}
			catch (const std::exception& error)
			{
				fail("can't record track " + track.name + ": " + error.what());
			}
		}

		// Audio pre-record: the frames that came in just before, if none is missing in between.
		const size_t capacity = m_channels > 0 ? m_history.size() / static_cast<size_t>(m_channels) : 0;
		const size_t before = m_historyEnd == chunk.deviceFrame ? m_historyFrames : 0;
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
		if (!tracks.empty() && m_notify)
			m_notify("recording.finished",
				{{"project", m_current.project}, {"mode", g_audioRecordModes.at(static_cast<size_t>(m_current.mode))},
					{"tracks", std::move(tracks)}});
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
