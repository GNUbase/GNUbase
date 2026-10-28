// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/audio/transport.h"
#include "engine/audiofile/diskstream.h"
#include "engine/document/document.h"
#include "engine/graph/processor.h"
#include "platform/rt/snapshotpublisher.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace gnubase::engine
{
	struct RenderSnapshot;
	struct RenderChannel;

	// A stretch of an audio track's playback: a section of one event's clip.
	struct PlaylistEntry
	{
		time::Samples start = 0; // in the project's samples
		time::Samples end = 0;
		ObjectId media = 0;
		time::Samples frame = 0; // the clip's frame at start, at the project's sample rate
		float gain = 1.0f;
		int channels = 1; // the clip's
	};

	// What an audio track plays, sorted by start and never overlapping. A track plays one event at a time: where
	// events overlap, the one added last (the highest id) is in front and plays. Muted events don't play and hide
	// nothing.
	using Playlist = std::vector<PlaylistEntry>;

	Playlist makePlaylist(const RenderSnapshot& snapshot, const RenderChannel& channel);

	// Plays the clips of audio tracks from disk streams. Each track has a few voices: disk streams of its clips that
	// the command thread positions ahead of time ("cueing") wherever playback enters a playlist entry: the cursor
	// when playback starts or locates, the start of each next entry, and the cycle's left locator. At such a point
	// the audio thread switches to the voice waiting there, so entries and cycle jumps start at their exact sample;
	// within an entry it reads on from the same voice. A voice that isn't there in time leaves silence, counted as a
	// dropout.
	class Playback
	{
	public:
		// Where the transport is, for cueing.
		struct Cue
		{
			time::Samples position = 0;
			bool playing = false; // the audio thread reads the voices meanwhile; else they are cued exactly
			TransportSettings settings;
			time::Samples window = 0; // how far ahead along the playback path to cue
		};

		struct Track
		{
			ObjectId channel = 0;
			std::shared_ptr<const Playlist> playlist;
		};

		// Voices per track, and the disk stream buffer of each.
		explicit Playback(size_t voicesPerTrack = 8, const DiskStreamSettings& streams = {4096, 8});
		~Playback();
		Playback(const Playback&) = delete;
		Playback& operator=(const Playback&) = delete;

		// Command thread: the tracks to play and their clips' files, streamed at the project's sample rate.
		void setTracks(std::vector<Track> tracks, std::map<ObjectId, std::string> mediaPaths, double sampleRate);

		// Any thread but the audio thread: positions voices for the playback path from cue.position on. Call it when
		// playback is about to start, after a locate, and regularly while playing.
		void cue(const Cue& cue);

		// Any thread but the audio thread: waits until every voice's buffer is full, or the timeout passes. Offline
		// rendering waits before each block, so it never drops out and renders the same every time.
		bool waitUntilBuffered(std::chrono::milliseconds timeout);

		// Frames of silence where a voice was not in time or its disk stream was late.
		uint64_t dropouts() const;

		// A voice: one of a track's disk streams, and where it is.
		struct Voice
		{
			ObjectId channel = 0;
			ObjectId media = 0;
			std::shared_ptr<DiskStream> stream;
			std::atomic<time::Samples> cueFrame{0}; // where the last cue sent it, written before cueGeneration
			std::atomic<uint64_t> cueGeneration{0};

			// Written by the audio thread.
			std::atomic<time::Samples> next{-1}; // the frame its next read returns
			std::atomic<bool> started{false};    // read since its last cue
			std::atomic<uint64_t> lastBlock{0};  // the block it was last read in
			uint64_t seenGeneration = 0;
		};

		// Audio thread, around every block.
		void beginBlock() noexcept;
		void endBlock() noexcept;

		// Audio thread, between beginBlock and endBlock: a voice of the channel's that reads media from frame, or
		// one that will within the next frames (lead says after how many), or none.
		Voice* take(
			ObjectId channel, ObjectId media, time::Samples frame, time::Samples frames, time::Samples& lead) noexcept;
		void missed(time::Samples frames) noexcept; // frames played as silence for want of a voice
		uint64_t block() const noexcept { return m_block.load(std::memory_order_relaxed); }

	private:
		struct VoiceTable
		{
			std::vector<std::shared_ptr<Voice>> voices; // sorted by channel
		};

		struct Point
		{
			ObjectId media = 0;
			time::Samples frame = 0;
		};

		// Where the playback path ahead of the cursor enters entries: entry starts and cycle jumps.
		std::vector<Point> points(const Playlist& playlist, const Cue& cue) const;
		// The position frames of playback after the cursor.
		static time::Samples ahead(const Cue& cue, time::Samples frames);
		std::shared_ptr<Voice> open(ObjectId channel, ObjectId media, time::Samples frame);
		void retire(const std::shared_ptr<Voice>& voice);
		static void send(Voice& voice, time::Samples frame);

		size_t m_voicesPerTrack;
		DiskStreamSettings m_streamSettings;

		// Command side, guarded by m_mutex.
		mutable std::mutex m_mutex;
		std::vector<Track> m_tracks;
		std::map<ObjectId, std::string> m_mediaPaths;
		std::set<ObjectId> m_unreadable; // media that failed to open, not retried until the tracks change
		double m_sampleRate = 48000.0;
		std::unique_ptr<DiskStreamer> m_streamer; // started with the first voice
		std::vector<std::shared_ptr<Voice>> m_voices;
		uint64_t m_retiredDropouts = 0;

		rt::SnapshotPublisher<VoiceTable> m_tables;
		std::atomic<uint64_t> m_block{0};
		std::atomic<uint64_t> m_missedFrames{0};
		const VoiceTable* m_table = nullptr; // audio thread, during a block
	};

	// Plays an audio track's playlist into its channel node, on top of the node's input.
	class TrackPlayer : public graph::Processor
	{
	public:
		TrackPlayer(std::shared_ptr<const Playlist> playlist, ObjectId channel, Playback& playback);

		void process(const graph::ProcessContext& context) noexcept override;

	protected:
		void prepare(double sampleRate, uint32_t maxFrames) override;

	private:
		void play(const graph::ProcessContext& context, const PlaylistEntry& entry, time::Samples position,
			time::Samples frames, uint32_t offset) noexcept;

		std::shared_ptr<const Playlist> m_playlist;
		ObjectId m_channel;
		Playback& m_playback;
		std::vector<float> m_buffer; // one block of interleaved frames of the clip with the most channels
		size_t m_bufferFrames = 0;
	};
}
