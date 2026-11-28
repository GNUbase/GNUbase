// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/recorder.h"

#include "engine/document/serialize.h"
#include "engine/media/backup.h"
#include "engine/midi/midivalues.h"
#include "platform/log.h"
#include "platform/rt/realtime.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <ctime>
#include <exception>
#include <set>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>

namespace gnubase::engine
{
	namespace
	{
		namespace fs = std::filesystem;

		// How long the disk thread sleeps when it finds no chunk: while a track is armed or records, and otherwise,
		// when only MIDI input for the retrospective buffers can come (the MIDI chunks hold far longer than that).
		constexpr std::chrono::milliseconds g_idle{2};
		constexpr std::chrono::milliseconds g_standby{50};
		// The pre-recorded frames go to the files in pieces of this many.
		constexpr size_t g_historyPiece = 4096;
		// The chunks hold this many frames in all, at least g_minChunks of them: over a second at 48 kHz.
		constexpr uint32_t g_queuedFrames = 65536;
		constexpr uint32_t g_minChunks = 32;
		// MIDI chunks: one per block while MIDI tracks record or MIDI input comes, each with up to this many events.
		constexpr uint32_t g_midiChunks = 256;
		constexpr size_t g_eventsPerChunk = 256;

		using EventType = graph::ProcessEvent::Type;

		// text cut to at most bytes between two UTF-8 characters, not in one (#580).
		std::string cut(const std::string& text, size_t bytes)
		{
			if (text.size() <= bytes)
				return text;
			while (bytes > 0 && (static_cast<unsigned char>(text[bytes]) & 0xc0) == 0x80)
				--bytes; // a continuation byte
			return text.substr(0, bytes);
		}

		// A track name as a file name: characters that some file system doesn't take become "_", and 200 bytes at
		// most leave room for a number and the extension in the 255 that file systems take.
		std::string fileName(const std::string& name)
		{
			std::string result = cut(name, 200);
			for (char& c : result)
				if (static_cast<unsigned char>(c) < 32 || std::string_view("<>:\"/\\|?*").find(c) != std::string::npos)
					c = '_';
			while (!result.empty() && (result.back() == '.' || result.back() == ' '))
				result.pop_back(); // Windows drops them
			if (result.empty())
				return "Audio";
			// Windows' device names, whatever the extension and the case, and spaces before it: "con.wav" is no file.
			const size_t stem = std::min(result.find('.'), result.size());
			std::string device = result.substr(0, stem);
			while (!device.empty() && device.back() == ' ')
				device.pop_back();
			std::transform(device.begin(), device.end(), device.begin(),
				[](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
			const bool numbered = device.size() == 4 &&
				(device.compare(0, 3, "COM") == 0 || device.compare(0, 3, "LPT") == 0) && device[3] >= '1' &&
				device[3] <= '9';
			if (numbered || device == "CON" || device == "PRN" || device == "AUX" || device == "NUL")
				result.insert(stem, "_");
			return result;
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

	BroadcastInfo broadcastNow(
		const std::string& description, const std::string& author, const std::string& reference, uint64_t timeReference)
	{
		BroadcastInfo info{cut(description, 256), cut(author, 32), cut(reference, 32), {}, {}, timeReference};
		std::tie(info.originationDate, info.originationTime) = originationNow();
		return info;
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
			// The recorder adds the origination time and the first sample's project position, and the track name as the
			// description if it has none.
			BroadcastInfo broadcast;
			broadcast.originator = "GNUbase";
			broadcast.timeReference = static_cast<uint64_t>(std::llround(project.setup.startTime * project.sampleRate));
			options.broadcast = broadcast;
		}
		else if (options.format != AudioFormat::Flac)
		{
			options.text = FileText{}; // the same strings in a Wave or AIFF file's text chunks (p. 1615)
		}
		return options;
	}

	MidiRecordFilter MidiRecordFilter::fromJson(const Json& json)
	{
		MidiRecordFilter filter;
		for (const Json& type : json.value("types", Json::array()))
		{
			const auto index = static_cast<size_t>(
				std::find(g_midiFilterTypes.begin(), g_midiFilterTypes.end(), type.get<std::string>()) -
				g_midiFilterTypes.begin());
			if (index == 0)
				filter.types.set(static_cast<size_t>(EventType::NoteOn)).set(static_cast<size_t>(EventType::NoteOff));
			else if (index < g_midiFilterTypes.size())
				filter.types.set(index + 1);
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
		// Note expression is no MIDI message type: only its channel filters it.
		const auto type = static_cast<size_t>(event.type);
		return (type >= types.size() || !types.test(type)) && !channels.test(event.channel % 16U) &&
			(event.type != EventType::Controller || !controllers.test(event.key % 128U));
	}

	void MidiTakeBuilder::add(time::Samples position, const graph::ProcessEvent& event)
	{
		m_last = std::max(m_last, position);
		const uint8_t channel = event.channel % 16U;
		if (event.type == EventType::NoteOn || event.type == EventType::NoteOff)
		{
			Sounding& sounding = m_sounding[(size_t{channel} * 128) + (event.key % 128U)];
			if (sounding.position >= 0)
			{
				m_notes.push_back({{"position", sounding.position},
					{"length", std::max<time::Samples>(position - sounding.position, 1)}, {"pitch", event.key % 128U},
					{"velocity", sounding.velocity / g_maxVelocity},
					{"releaseVelocity",
						(event.type == EventType::NoteOff ? event.velocity : g_defaultReleaseVelocity) / g_maxVelocity},
					{"channel", channel}, {"take", m_take}});
				if (!sounding.expressions.empty())
					m_notes.back()["expressions"] = std::move(sounding.expressions);
			}
			sounding.position = event.type == EventType::NoteOn ? position : -1;
			sounding.velocity = event.velocity;
			sounding.expressions = Json::array();
			return;
		}
		if (event.type == EventType::NoteExpression)
		{
			Sounding& sounding = m_sounding[(size_t{channel} * 128) + (event.key % 128U)];
			if (sounding.position >= 0 && event.parameter < g_expressionNames.size())
				sounding.expressions.push_back({{"type", g_expressionNames.at(event.parameter)}, {"position", position},
					{"value", event.parameterValue}});
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
		json["take"] = m_take;
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

	Recorder::Recorder(Notify notify, CreateWriter createWriter)
		: m_notify(std::move(notify)), m_createWriter(std::move(createWriter))
	{
		m_thread = std::thread([this] { run(); });
	}

	Recorder::~Recorder()
	{
		{
			std::unique_lock lock(m_mutex);
			m_wake.wait(lock, [this] { return !m_opening; });
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
		std::unique_lock lock(m_mutex);
		m_wake.wait(lock, [this] { return !m_opening; }); // the chunks go away
		drain(true);                                      // what the audio thread sent before it stopped
		applySetups(m_latestSetup.load(std::memory_order_acquire));
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
		m_inFlight.store(0, std::memory_order_relaxed);
		resetHistory();
		// The next device counts its frames from 0 again: what was kept so far is no input just before its recordings,
		// and its frames are counted on after it.
		for (auto& entry : m_retrospective)
		{
			entry.second.recorded = entry.second.events.size();
			entry.second.stoppedBefore = static_cast<size_t>(std::count_if(entry.second.events.begin(),
				entry.second.events.end(), [](const RetrospectiveEvent& kept) { return kept.position < 0; }));
			if (!entry.second.events.empty())
				m_frameBase = std::max(m_frameBase, entry.second.events.back().deviceFrame + 1);
		}
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
		m_wake.notify_all(); // the disk thread polls from now on
	}

	void Recorder::setup(RecordSetup setup)
	{
		rt::assertNotRealtime("engine::Recorder::setup");
		// Numbered instead of applied here: the chunks sent before it still get the setup before, but the disk thread
		// can hold m_mutex for a while.
		const bool armed = !setup.tracks.empty();
		const bool midiArmed = !setup.midiTracks.empty();
		{
			const std::scoped_lock lock(m_setupMutex);
			m_setups.emplace_back(++m_setupCount, std::move(setup));
			m_latestSetup.store(m_setupCount, std::memory_order_release);
		}
		// After the number: the chunks that this arms get it.
		m_armed.store(armed, std::memory_order_release);
		m_midiArmed.store(midiArmed, std::memory_order_release);
		m_wake.notify_all(); // from its standby: the chunks come more often now
	}

	bool Recorder::waitForChunks(std::chrono::milliseconds timeout)
	{
		rt::assertNotRealtime("engine::Recorder::waitForChunks");
		if (!m_filled ||
			m_inFlight.load(std::memory_order_acquire) <= std::min(m_chunks.size(), m_midiChunks.size()) / 2)
			return true;
		m_wake.notify_all();
		const auto deadline = std::chrono::steady_clock::now() + timeout;
		while (m_inFlight.load(std::memory_order_acquire) > 0)
		{
			if (std::chrono::steady_clock::now() >= deadline)
				return false;
			std::this_thread::sleep_for(std::chrono::microseconds(200));
		}
		return true;
	}

	void Recorder::applySetups(uint64_t number)
	{
		if (m_opening || number <= m_nextNumber)
			return;
		RecordSetup next;
		uint64_t nextNumber = 0;
		{
			const std::scoped_lock lock(m_setupMutex);
			for (; !m_setups.empty() && m_setups.front().first <= number; m_setups.pop_front())
			{
				nextNumber = m_setups.front().first;
				next = std::move(m_setups.front().second);
			}
		}
		if (nextNumber == 0)
			return;
		const int64_t preRecordFrames = m_next.preRecordFrames;
		m_next = std::move(next);
		m_nextNumber = nextNumber;
		if (m_next.preRecordFrames != preRecordFrames || historyPorts() != m_historyPorts)
			resetHistory();
	}

	std::vector<int> Recorder::historyPorts() const
	{
		std::vector<int> ports;
		if (m_next.preRecordFrames > 0)
			for (const RecordTrack& track : m_next.tracks)
				for (const int port : track.ports)
					if (port >= 0 && port < m_channels)
						ports.push_back(port);
		std::sort(ports.begin(), ports.end());
		ports.erase(std::unique(ports.begin(), ports.end()), ports.end());
		return ports;
	}

	void Recorder::resetHistory()
	{
		m_historyPorts = historyPorts();
		m_historyFrames = 0;
		m_history = {}; // before the next one is allocated
		try
		{
			m_history.assign(static_cast<size_t>(m_next.preRecordFrames) * m_historyPorts.size(), 0.0f);
		}
		catch (const std::exception& error)
		{
			m_history = {};
			fail("not enough memory for the audio pre-record of " + std::to_string(m_next.preRecordFrames) +
				" frames: " + error.what());
		}
	}

	std::vector<RetrospectiveEvent> Recorder::retrospective(ObjectId channel)
	{
		rt::assertNotRealtime("engine::Recorder::retrospective");
		const std::scoped_lock lock(m_mutex);
		drain(false);
		const auto found = m_retrospective.find(channel);
		if (found == m_retrospective.end() || m_next.retrospective == 0) // then it keeps only the catch range
			return {};
		return {found->second.events.begin(), found->second.events.end()};
	}

	void Recorder::inserted(ObjectId channel, uint64_t deviceFrame)
	{
		rt::assertNotRealtime("engine::Recorder::inserted");
		const std::scoped_lock lock(m_mutex);
		if (const auto found = m_retrospective.find(channel); found != m_retrospective.end())
			found->second.inserted = deviceFrame;
	}

	void Recorder::clearRetrospective(ObjectId channel)
	{
		rt::assertNotRealtime("engine::Recorder::clearRetrospective");
		const std::scoped_lock lock(m_mutex);
		drain(false);
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
		if (!m_armed.load(std::memory_order_acquire))
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
		chunk->setup = m_latestSetup.load(std::memory_order_relaxed); // at least the one that armed it (acquire)
		size_t n = 0;
		for (uint32_t i = 0; i < frames; ++i)
			for (int c = 0; c < m_channels; ++c)
				chunk->samples[n++] = block.inputs[c][offset + i];
		m_inFlight.fetch_add(1, std::memory_order_relaxed);
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
		const bool armed = m_midiArmed.load(std::memory_order_acquire);
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
		// Drum sounds auditioned in the drum editor (midi.inject drumSounds) play but are no MIDI input: neither
		// recorded nor kept for retrospective recording.
		const auto input = [offset, frames](const graph::ProcessEvent& event)
		{
			return event.offset >= offset && event.offset - offset < frames && event.type != EventType::SysEx &&
				event.type != EventType::Parameter && !event.drumSound;
		};
		size_t count = 0;
		for (const auto& entry : sequencers)
			count +=
				static_cast<size_t>(std::count_if(entry.second->input().begin(), entry.second->input().end(), input));
		// What one chunk can't hold goes on in the next ones, with the same frames: the disk thread takes them as one.
		size_t sent = 0;
		do
		{
			Chunk* chunk = nullptr;
			if (!m_midiFree->tryPop(chunk))
			{
				m_droppedMidi.fetch_add(count - sent, std::memory_order_relaxed);
				return sent > 0;
			}
			chunk->deviceFrame = block.position + offset;
			chunk->position = position;
			chunk->frames = frames;
			chunk->setup = m_latestSetup.load(std::memory_order_relaxed);
			chunk->eventCount = 0;
			chunk->stops = played.stops();
			size_t index = 0;
			for (const auto& [channel, sequencer] : sequencers)
				for (const graph::ProcessEvent& event : sequencer->input())
					if (input(event) && index++ >= sent && chunk->eventCount < chunk->events.size())
						chunk->events[chunk->eventCount++] = {channel, event.offset - offset,
							played.at(static_cast<int64_t>(block.position + event.offset)), event,
							played.lapAt(static_cast<int64_t>(block.position + event.offset))};
			sent += chunk->eventCount;
			m_inFlight.fetch_add(1, std::memory_order_relaxed);
			m_filled->tryPush(chunk); // there is room for every chunk
		} while (sent < count);
		return true;
	}

	void Recorder::run()
	{
		std::unique_lock lock(m_mutex);
		while (!m_stopping)
		{
			// ponytail: polls, since the audio thread can't wake it; a semaphore if 2 ms ever matters.
			if (drain(true))
				continue;
			applySetups(m_latestSetup.load(std::memory_order_acquire)); // for the chunks to come
			if (!m_filled)
				m_wake.wait(lock); // no device: prepare and the destructor wake it
			else if (m_recording || m_midiRecording || m_armed.load(std::memory_order_relaxed) ||
				m_midiArmed.load(std::memory_order_relaxed))
				m_wake.wait_for(lock, g_idle);
			else
				m_wake.wait_for(lock, g_standby); // setup wakes it
		}
	}

	bool Recorder::drain(bool mayBegin)
	{
		if (m_opening || !m_filled)
			return false; // not prepared, or the chunks behind the one that starts a recording wait for its files
		bool handled = false;
		for (Chunk* const* next = m_filled->peek(); next != nullptr; next = m_filled->peek())
		{
			Chunk* chunk = *next;
			if (!mayBegin && !chunk->midi && chunk->position >= 0 && !m_recording)
				break; // starts a recording: the disk thread lists the folder and creates the files
			m_filled->tryPop(chunk);
			applySetups(chunk->setup);
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
			m_inFlight.fetch_sub(1, std::memory_order_release);
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
				m_cycled = m_cycled || startsLap(takeEnd, chunk.position);
				m_takes.push_back({chunk.position, m_written, 0});
			}
		}
		write(chunk.samples.data(), chunk.frames);
		m_takes.back().length += chunk.frames;
		m_deviceEnd = chunk.deviceFrame + chunk.frames;
	}

	Recorder::Unlocked::Unlocked(Recorder& recorder) : m_recorder(recorder)
	{
		m_recorder.m_opening = true;
		m_recorder.m_mutex.unlock();
	}

	Recorder::Unlocked::~Unlocked()
	{
		m_recorder.m_mutex.lock();
		m_recorder.m_opening = false;
		m_recorder.m_wake.notify_all();
	}

	void Recorder::begin(const Chunk& chunk)
	{
		m_current = m_next;
		m_recording = true;
		m_files.clear();
		m_takes.clear();
		m_cycled = false;
		m_written = 0;
		// Audio pre-record: the frames that came in just before, if none is missing in between.
		const size_t ports = m_historyPorts.size();
		const size_t capacity = ports > 0 ? m_history.size() / ports : 0;
		const size_t before = capacity > 0 && m_historyEnd == chunk.deviceFrame ? m_historyFrames : 0;
		if (m_current.format.broadcast)
		{
			// The project start time plus the first frame's position, pre-recorded input included, not before midnight.
			BroadcastInfo& broadcast = *m_current.format.broadcast;
			const auto first = static_cast<time::Samples>(broadcast.timeReference) + chunk.position -
				static_cast<time::Samples>(before);
			broadcast.timeReference = static_cast<uint64_t>(std::max<time::Samples>(first, 0));
		}
		{
			// The folder and the files without the lock: the folder can hold thousands of recordings, and
			// retrospective() shouldn't wait for them. Meanwhile no chunk is handled and no setup applied (m_opening),
			// prepare and the destructor wait, and nothing else uses m_current, m_files or the pre-record history.
			const Unlocked unlocked(*this);
			// Without a folder it records no files, rather than into the working folder: the recording fails once.
			bool hasFolder = !m_current.tracks.empty();
			if (hasFolder && m_current.folder.empty())
			{
				fail("there is no folder to record into: save the project first");
				hasFolder = false;
			}
			else if (hasFolder)
			{
				std::error_code error;
				fs::create_directories(m_current.folder, error);
				hasFolder = !error && fs::is_directory(m_current.folder, error);
				if (!hasFolder)
					fail("can't create the record folder " + m_current.folder.u8string() + ": " +
						(error ? error.message() : std::string("a file of that name is there")));
			}
			// One listing for all tracks.
			m_taken = hasFolder ? takenNames(m_current.folder) : std::set<std::string>{};
			const std::vector<RecordTrack> none;
			for (const RecordTrack& track : hasFolder ? m_current.tracks : none)
			{
				try
				{
					const std::string path = createFreeFile(m_current.folder,
						fs::u8path(fileName(track.name) + extensionOf(m_current.format.format)), m_taken)
												 .u8string();
					WriteOptions options = m_current.format;
					options.channels = static_cast<int>(track.ports.size());
					if (const std::optional<BroadcastInfo> broadcast = options.broadcast)
						options.broadcast =
							broadcastNow(broadcast->description.empty() ? track.name : broadcast->description,
								broadcast->originator, broadcast->originatorReference, broadcast->timeReference);
					if (options.text && options.text->description.empty())
						options.text->description = track.name;
					m_files.push_back({track, path, m_createWriter(path, options), options});
				}
				catch (const std::exception& error)
				{
					fail("can't record track " + track.name + ": " + error.what());
				}
			}
		}

		// The history holds the armed tracks' ports only: in pieces of all channels, as write takes them.
		const auto channels = static_cast<size_t>(m_channels);
		std::vector<float> piece(std::min(before, g_historyPiece) * channels, 0.0f);
		for (size_t done = 0; done < before;)
		{
			const size_t frames = std::min(before - done, g_historyPiece);
			for (size_t i = 0; i < frames; ++i)
			{
				const size_t index = (m_historyStart + done + i) % capacity;
				for (size_t k = 0; k < ports; ++k)
					piece[(i * channels) + static_cast<size_t>(m_historyPorts[k])] = m_history[(index * ports) + k];
			}
			write(piece.data(), static_cast<uint32_t>(frames));
			done += frames;
		}
		m_historyFrames = 0;
		m_takes.push_back({chunk.position, m_written, 0});
	}

	void Recorder::write(const float* samples, uint32_t frames)
	{
		const auto channels = static_cast<size_t>(m_channels);
		for (size_t index = 0, count = m_files.size(); index < count; ++index) // not the files that a full one adds
		{
			File& file = m_files[index];
			if (file.end >= 0)
				continue;
			const std::vector<int>& ports = file.track.ports;
			m_trackSamples.assign(static_cast<size_t>(frames) * ports.size(), 0.0f);
			for (size_t c = 0; c < ports.size(); ++c)
				if (ports[c] >= 0 && static_cast<size_t>(ports[c]) < channels)
					for (size_t i = 0; i < frames; ++i)
						m_trackSamples[(i * ports.size()) + c] =
							samples[(i * channels) + static_cast<size_t>(ports[c])];
			try
			{
				file.writer->write(m_trackSamples.data(), frames);
			}
			catch (const AudioFileFull&)
			{
				// A Wave file without RF64 or an AIFF file at 4 GB goes on in the next file (Split Files, p. 1615).
				file.end = m_written;
				goOn(index, frames);
			}
			catch (const std::exception& error)
			{
				// The file ends there, and what it holds still lands (#580); the other tracks record on.
				fail("can't write " + file.path + ": " + error.what());
				file.end = m_written;
			}
		}
		m_written += frames;
	}

	void Recorder::goOn(size_t index, uint32_t frames)
	{
		// Once in 4 GB, so it creates the file with the lock held.
		File& full = m_files[index];
		File next{full.track, {}, {}, full.options, m_written};
		if (next.options.broadcast)
			next.options.broadcast->timeReference += static_cast<uint64_t>(m_written - full.start);
		try
		{
			full.writer->close();
			next.path = createFreeFile(
				m_current.folder, fs::u8path(fileName(full.track.name) + extensionOf(m_current.format.format)), m_taken)
							.u8string();
			next.writer = m_createWriter(next.path, next.options);
			next.writer->write(m_trackSamples.data(), frames);
		}
		catch (const std::exception& error)
		{
			fail("can't go on recording track " + full.track.name + " in a new file: " + error.what());
			// The empty or header-only file stays out of the record folder: closed first, as Windows removes no
			// open file (#2096).
			next.writer.reset();
			std::error_code ignored;
			if (!next.path.empty())
				fs::remove(fs::u8path(next.path), ignored);
			return;
		}
		m_files.push_back(std::move(next)); // full is no longer valid
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
			// The parts of the takes in this file.
			Json takes = Json::array();
			const time::Samples fileEnd = file.end < 0 ? m_written : file.end;
			for (const Take& take : m_takes)
			{
				const time::Samples from = std::max(take.offset, file.start);
				const time::Samples to = std::min(take.offset + take.length, fileEnd);
				if (to <= from)
					continue;
				takes.push_back({{"position", take.position + (from - take.offset)}, {"offset", from - file.start},
					{"length", to - from}});
				if (from > take.offset) // the track's previous file holds the take's start (#2102)
					takes.back()["continues"] = true;
			}
			tracks.push_back({{"track", file.track.track}, {"path", file.path}, {"takes", std::move(takes)}});
		}
		m_files.clear();
		Json data = {{"project", m_current.project},
			{"mode", g_audioRecordModes.at(static_cast<size_t>(m_current.mode))}, {"cycle", m_cycled}};
		if (!m_pendingMidi.empty())
		{
			data["cycle"] = m_cycled || m_pendingMidiCycled;
			m_pendingMidiCycled = false;
			for (Json& track : m_pendingMidi)
				tracks.push_back(std::move(track));
			m_pendingMidi = Json::array();
			data["midiMode"] = g_midiRecordModes.at(static_cast<size_t>(m_pendingMidiSetup.midiMode));
			data["cycleMode"] = g_midiCycleRecordModes.at(static_cast<size_t>(m_pendingMidiSetup.midiCycleMode));
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
				keepRetrospective(chunk.events[i], chunk);
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
			m_midiCycled = m_midiCycled || startsLap(take.position + take.length, chunk.position);
			// A take that got no frames is left out of recording.add, and nothing was recorded in it: the next one
			// takes its place, so the takes' indices stay those recording.add gets.
			if (take.length == 0)
				m_midiTakes.back() = {chunk.position, frame, 0};
			else
				m_midiTakes.push_back({chunk.position, frame, 0});
			for (auto& [channel, builder] : m_midiBuilders)
				builder.setTake(m_midiTakes.size() - 1);
		}
		Take& take = m_midiTakes.back();
		take.length = std::max(take.length, frame + chunk.frames - take.offset);
		for (size_t i = 0; i < chunk.eventCount; ++i)
		{
			const MidiInput& input = chunk.events[i];
			const auto builder = m_midiBuilders.find(input.channel);
			// A track that only monitors records only chord events, no part: its input stays retrospective too.
			const bool chordsOnly = std::any_of(m_midiCurrent.midiTracks.begin(), m_midiCurrent.midiTracks.end(),
				[&input](const MidiRecordTrack& track) { return track.channel == input.channel && track.chordsOnly; });
			if (builder == m_midiBuilders.end() || chordsOnly)
				keepRetrospective(input, chunk);
			if (chordsOnly)
				if (const auto kept = m_retrospective.find(input.channel); kept != m_retrospective.end())
					kept->second.recorded = kept->second.events.size();
			if (builder != m_midiBuilders.end() && m_midiCurrent.midiFilter.records(input.event))
				builder->second.add(chunk.position + input.offset, input.event);
		}
	}

	void Recorder::beginMidi(const Chunk& chunk)
	{
		m_midiCurrent = m_next;
		m_midiRecording = true;
		m_midiTakes.assign(1, {chunk.position, static_cast<time::Samples>(chunk.deviceFrame), 0});
		m_midiCycled = false;
		m_midiBuilders.clear();
		for (const MidiRecordTrack& track : m_midiCurrent.midiTracks)
			m_midiBuilders[track.channel] = MidiTakeBuilder();
		// The catch range: input that came just before recording started belongs to its start, unless it came before
		// or during an earlier recording of the track, whether that one took it or not. All that was kept so far is
		// before this recording: the next one doesn't take it again.
		for (const MidiRecordTrack& track : m_midiCurrent.midiTracks)
		{
			const auto found = m_retrospective.find(track.channel);
			if (found == m_retrospective.end())
				continue;
			Retrospective& buffer = found->second;
			const uint64_t start = m_frameBase + chunk.deviceFrame;
			if (m_midiCurrent.catchFrames > 0)
				for (auto kept = buffer.events.begin() + static_cast<std::ptrdiff_t>(buffer.recorded);
					kept != buffer.events.end(); ++kept)
					if (kept->deviceFrame < start &&
						kept->deviceFrame + static_cast<uint64_t>(m_midiCurrent.catchFrames) >= start &&
						m_midiCurrent.midiFilter.records(kept->event))
						m_midiBuilders[track.channel].add(chunk.position, kept->event);
			buffer.recorded = buffer.events.size();
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
			if (builder.empty() || takes.empty())
				continue;
			tracks.push_back({{"track", track.track}, {"takes", takes}, {"notes", builder.notes()},
				{"controllers", builder.controllers()}});
			if (track.chordsOnly)
				tracks.back()["chordsOnly"] = true;
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
			m_pendingMidiCycled = m_pendingMidiCycled || m_midiCycled;
			return;
		}
		if (m_notify)
			m_notify("recording.finished",
				{{"project", m_midiCurrent.project},
					{"midiMode", g_midiRecordModes.at(static_cast<size_t>(m_midiCurrent.midiMode))},
					{"cycleMode", g_midiCycleRecordModes.at(static_cast<size_t>(m_midiCurrent.midiCycleMode))},
					{"snapToBars", m_midiCurrent.snapToBars}, {"cycle", m_midiCycled}, {"tracks", std::move(tracks)}});
	}

	bool Recorder::startsLap(time::Samples end, time::Samples start) const
	{
		return m_next.cycleLeft >= 0 && start == m_next.cycleLeft && end == m_next.cycleRight;
	}

	void Recorder::keepRetrospective(const MidiInput& input, const Chunk& chunk)
	{
		const uint64_t deviceFrame = chunk.deviceFrame;
		// With midi.retrospectiveEvents 0 it keeps only the catch range's input, which retrospective() doesn't return.
		if (m_next.retrospective == 0 && m_next.catchFrames == 0)
			return;
		Retrospective& buffer = m_retrospective[input.channel];
		const uint64_t frame = m_frameBase + deviceFrame + input.offset;
		if (buffer.inserted)
		{
			// Input after an insert leaves out what was inserted.
			const auto after = std::find_if(buffer.events.begin(), buffer.events.end(),
				[&buffer](const RetrospectiveEvent& kept) { return kept.deviceFrame > *buffer.inserted; });
			const auto count = static_cast<size_t>(after - buffer.events.begin());
			buffer.events.erase(buffer.events.begin(), after);
			buffer.recorded -= std::min(buffer.recorded, count);
			buffer.stoppedBefore = static_cast<size_t>(
				std::count_if(buffer.events.begin(), buffer.events.end(), [this](const RetrospectiveEvent& kept)
					{ return kept.position < 0 && kept.deviceFrame < m_frameBase; }));
			buffer.inserted.reset();
		}
		// Input after the buffer was inserted starts it afresh, and so does input in stop mode while it holds input in
		// stop mode, after a pause since the last input, from playback or not (reference manual p. 354).
		const auto stopped = [&buffer]
		{
			return std::any_of(buffer.events.begin(), buffer.events.end(),
				[](const RetrospectiveEvent& kept) { return kept.position < 0; });
		};
		if (input.played < 0 && !buffer.events.empty() &&
			frame > buffer.events.back().deviceFrame + static_cast<uint64_t>(m_next.pauseFrames) && stopped())
		{
			buffer.events.clear();
			buffer.recorded = 0;
			buffer.stoppedBefore = 0;
		}
		else if (input.played < 0 && buffer.stoppedBefore > 0)
		{
			// On the next device, input in stop mode leaves out the earlier devices' input in stop mode: the insert
			// places it by its frames, which leave out the time the old device ran. Input during playback stays, as
			// the insert places it where it played.
			const auto earlier = [this](const RetrospectiveEvent& kept)
			{ return kept.position < 0 && kept.deviceFrame < m_frameBase; };
			const auto recorded = buffer.events.begin() + static_cast<std::ptrdiff_t>(buffer.recorded);
			buffer.recorded -= static_cast<size_t>(std::count_if(buffer.events.begin(), recorded, earlier));
			buffer.events.erase(
				std::remove_if(buffer.events.begin(), buffer.events.end(), earlier), buffer.events.end());
			buffer.stoppedBefore = 0;
		}
		PlayedLap lap = input.lap;
		// Passes without input in between, e.g. one played just to listen: the insert ends the input before where its
		// own pass stopped, or at that input if it is no longer known (#2157).
		const auto before = std::find_if(buffer.events.rbegin(), buffer.events.rend(),
			[](const RetrospectiveEvent& kept) { return kept.position >= 0; });
		if (input.played >= 0 && before != buffer.events.rend() && lap.pass > before->lap.pass + 1)
		{
			const PassStop& stop = chunk.stops[before->lap.pass % chunk.stops.size()];
			const bool known = stop.pass == before->lap.pass;
			lap.stopped = known ? stop.position : -1;
			lap.stoppedLap = known ? stop.lap : before->lap.lap;
			lap.stoppedRight = known ? stop.right : -1;
		}
		buffer.events.push_back({frame, input.played, input.event, lap});
		const auto full = [&]
		{
			return m_next.retrospective > 0
				? buffer.events.size() > m_next.retrospective
				: buffer.events.front().deviceFrame + static_cast<uint64_t>(m_next.catchFrames) < frame;
		};
		while (full())
		{
			if (buffer.events.front().position < 0 && buffer.events.front().deviceFrame < m_frameBase &&
				buffer.stoppedBefore > 0)
				--buffer.stoppedBefore;
			buffer.events.pop_front();
			if (buffer.recorded > 0)
				--buffer.recorded;
		}
	}

	void Recorder::keepHistory(const Chunk& chunk)
	{
		const size_t ports = m_historyPorts.size();
		const size_t capacity = ports > 0 ? m_history.size() / ports : 0;
		if (capacity == 0 || chunk.frames == 0)
			return;
		if (chunk.deviceFrame != m_historyEnd)
			m_historyFrames = 0; // frames are missing in between
		const auto channels = static_cast<size_t>(m_channels);
		for (size_t i = 0; i < chunk.frames; ++i)
		{
			const size_t index = (m_historyStart + m_historyFrames) % capacity;
			for (size_t k = 0; k < ports; ++k)
				m_history[(index * ports) + k] = chunk.samples[(i * channels) + static_cast<size_t>(m_historyPorts[k])];
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
