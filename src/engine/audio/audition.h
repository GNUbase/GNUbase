// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/audio/playback.h"
#include "engine/audiofile/audiofile.h"
#include "engine/graph/events.h"
#include "platform/rt/seqlockpair.h"
#include "platform/rt/snapshotpublisher.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace gnubase::engine
{
	// Auditions one audio track (#470, reference manual pp. 252-253): plays its events from a position, as the play
	// tool does while the mouse is pressed, or scrubs them forwards and backwards at the speed new positions come in,
	// as the scrub tool does. Either goes straight to the main mix's device ports, past the track's channel (fader,
	// pan, mute, effects) and without the transport. docs/features/audition.md describes it.
	class Audition
	{
	public:
		Audition();

		// Command thread: plays the track from position on, streamed from its clips, until stop. channels: the main
		// mix's; maxFrames: the device's block size.
		void play(ObjectId track, const Playback::Track& source, std::map<ObjectId, std::string> mediaPaths,
			double sampleRate, time::Samples position, int channels, uint32_t maxFrames);

		// Command thread: plays the track from where the last scrub of it got to, to position, forwards or backwards,
		// in seconds (default: the time since that scrub, from 0.01 to 0.1 seconds), so speed and pitch follow how
		// fast the positions come. The first scrub of a track only sets where the next one starts.
		void scrub(ObjectId track, const Playback::Track& source, const std::map<ObjectId, std::string>& mediaPaths,
			double sampleRate, time::Samples position, std::optional<double> seconds, int channels);

		// Command thread: ends playing or scrubbing.
		void stop();

		// Command thread: the auditioned track, or 0, and whether it scrubs.
		ObjectId track() const { return m_track; }
		bool scrubbing() const { return m_scrubbing; }

		// Any thread but the audio thread: positions the disk streams of the track that plays, ahead of the audio
		// thread while it reads them (reading), else exactly where it plays next (offline rendering, between blocks).
		void cue(time::Samples window, bool reading);
		bool waitUntilBuffered(std::chrono::milliseconds timeout) { return m_playback.waitUntilBuffered(timeout); }

		// Audio thread: adds what it plays in this block to the device outputs, channel by channel to the ports
		// (-1: none).
		void process(
			float* const* outputs, int outputChannels, const std::vector<int>& ports, uint32_t frames) noexcept;

	private:
		// What the audio thread plays.
		struct State
		{
			uint64_t serial = 0; // counts up with each new state
			int channels = 0;
			// Playing: the track's player from start on, and its output, channels blocks of maxFrames frames.
			std::shared_ptr<TrackPlayer> player;
			time::Samples start = 0;
			uint32_t maxFrames = 0;
			std::vector<float> buffer;
			std::vector<float*> audio; // into buffer, per channel
			// Scrubbing: frames of channels samples to play once.
			std::shared_ptr<const std::vector<float>> scrub;
		};

		void publish(std::unique_ptr<State> state);
		// The track's front layer from start on, frames frames of channels samples, read from its clips.
		std::vector<float> read(const Playlist& playlist, const std::map<ObjectId, std::string>& mediaPaths,
			double sampleRate, time::Samples start, time::Samples frames, int channels);

		Playback m_playback; // before m_states: their players use it

		// Command side.
		ObjectId m_track = 0;
		bool m_scrubbing = false;
		time::Samples m_head = 0; // where the next scrub starts
		std::chrono::steady_clock::time_point m_lastScrub;
		std::map<ObjectId, std::unique_ptr<AudioFileReader>> m_readers; // while scrubbing; null where opening failed
		std::map<ObjectId, std::string> m_readerPaths;                  // the media files m_readers were opened from
		uint64_t m_serial = 0;
		std::mutex m_mutex; // guards what cue reads: whether it plays, the serial and start of its state
		bool m_cuePlaying = false;
		uint64_t m_cueSerial = 0;
		time::Samples m_cueStart = 0;

		rt::SnapshotPublisher<State> m_states;
		// Audio thread.
		uint64_t m_playing = 0; // the serial of the state it plays
		time::Samples m_position = 0;
		size_t m_scrubbed = 0; // frames of the scrub played
		graph::EventBuffer m_eventsIn;
		graph::EventBuffer m_eventsOut;
		// Written by the audio thread after each block: the state's serial and the position it plays next.
		rt::SeqLockPair<uint64_t, time::Samples> m_played;
	};
}
