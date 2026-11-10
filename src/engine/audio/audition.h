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
	// Auditions one audio track (#470, reference manual pp. 252-253), or one media file on its own or with a clip's
	// fades, as the sample editor plays its clip (#730, #860, p. 630): plays its events, or the file, from a
	// position, as the play tool does while the mouse is pressed, or scrubs them forwards and backwards at the speed
	// new positions come in, as the scrub tool does. Either goes straight to the main mix's device ports, past the
	// track's channel (fader, pan, mute, effects) and without the transport. docs/features/audition.md describes it.
	class Audition
	{
	public:
		// What is auditioned: an audio track, or a media file, with the clip whose fades it plays it with (0 for
		// what isn't).
		struct Subject
		{
			ObjectId track = 0;
			ObjectId media = 0;
			ObjectId clip = 0;

			bool operator==(const Subject& other) const
			{
				return track == other.track && media == other.media && clip == other.clip;
			}
			bool operator!=(const Subject& other) const { return !(*this == other); }
		};

		Audition();

		// Command thread: plays the subject from position on, streamed from its clips, up to end (exclusive, a
		// position of source's playlist), then nothing until stop. channels: the main mix's; maxFrames: the device's
		// block size.
		void play(const Subject& subject, const Playback::Track& source, std::map<ObjectId, std::string> mediaPaths,
			double sampleRate, time::Samples position, time::Samples end, int channels, uint32_t maxFrames);

		// Command thread: plays the subject from where the audio thread got to in the last scrub of it (where that
		// one starts if it hasn't played yet), to position, forwards or backwards, in seconds (default: the time since
		// that scrub, from 0.01 to 0.1 seconds), so speed and pitch follow how fast the positions come. The first
		// scrub of a subject only sets where the next one starts.
		void scrub(const Subject& subject, const Playback::Track& source,
			const std::map<ObjectId, std::string>& mediaPaths, double sampleRate, time::Samples position,
			std::optional<double> seconds, int channels);

		// Command thread: ends playing or scrubbing, and closes the files that played.
		void stop();

		// Command thread: the disk streams open for playing.
		size_t voices() const { return m_playback.voices(); }

		// Command thread: what is auditioned (both 0 for nothing), and whether it scrubs.
		const Subject& subject() const { return m_subject; }
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
			// Playing: the track's player from start to end, and its output, channels blocks of maxFrames frames.
			std::shared_ptr<TrackPlayer> player;
			time::Samples start = 0;
			time::Samples end = 0;
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
		Subject m_subject;
		bool m_scrubbing = false;
		// The last scrub: from and to, in frames frames (0 for none: to is where the next one starts), and the serial
		// of its state.
		time::Samples m_scrubFrom = 0;
		time::Samples m_scrubTo = 0;
		size_t m_scrubFrames = 0;
		uint64_t m_scrubSerial = 0;
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
		// Written by the audio thread after each block: the state's serial and the position it plays next, or while it
		// scrubs, the frames of the scrub it played.
		rt::SeqLockPair<uint64_t, time::Samples> m_played;
	};
}
