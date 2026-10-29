// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/audio/transport.h"
#include "engine/audiofile/diskstream.h"
#include "engine/document/document.h"
#include "engine/graph/processor.h"
#include "platform/rt/snapshotpublisher.h"

#include <array>
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

	// A fade on a stretch of the timeline, in the project's samples: a fade in is silent before start and at full
	// volume from end on, a fade out at full volume before start and silent from end on.
	struct GainRamp
	{
		time::Samples start = 0;
		time::Samples end = 0;
		FadeShape shape = FadeShape::Linear;
		bool out = false;

		// The gain at a position (fades.h): a fade in at start + n has fadeGain(shape, n / (end - start)), a fade
		// out at end - n has fadeGain(shape, n / (end - start)). So a fade in and a fade out over the same range,
		// with mirrored shapes, add up to 1 at every sample.
		double gainAt(time::Samples position) const noexcept;
	};

	// The most fades on an entry: its event's fade in and out, its clip's, and those of the auto crossfades it plays
	// behind the front in: one where the event in front starts, one where it ends.
	inline constexpr size_t g_maxRamps = 6;

	// A stretch of an audio track's playback: a section of one event's clip.
	struct PlaylistEntry
	{
		time::Samples start = 0; // in the project's samples
		time::Samples end = 0;
		ObjectId media = 0;
		time::Samples frame = 0; // the clip's frame at start, at the project's sample rate
		float gain = 1.0f;
		int channels = 1; // the clip's
		ObjectId event = 0;
		std::array<GainRamp, g_maxRamps> ramps{};
		size_t rampCount = 0;

		// The gain at a position: the event volume times every fade.
		float gainAt(time::Samples position) const noexcept;
		// Whether a fade changes the gain anywhere from position to position + frames.
		bool fades(time::Samples position, time::Samples frames) const noexcept;
	};

	// Entries sorted by start that never overlap.
	using PlaylistLayer = std::vector<PlaylistEntry>;

	// What an audio track plays. The first layer is the front: where events overlap, the one added last (the highest
	// id) is in front and plays. Muted events don't play and hide nothing. The layers after it hold what plays along
	// with the front where events crossfade: the event that fades out under the one that fades in, or the one that
	// fades in under the end of the one in front.
	struct Playlist
	{
		std::vector<PlaylistLayer> layers;
	};

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

		// Voices per track, and the disk stream buffer of each. Where events crossfade, two entries play at once.
		explicit Playback(size_t voicesPerTrack = 16, const DiskStreamSettings& streams = {4096, 8});
		virtual ~Playback();
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
		//
		// Its state says who has it. A cue sets it to waiting (with the number of the cue, so no two cues look the
		// same), the audio thread to the block it last read the voice in, and a cue that repositions or retires the
		// voice first to cueing, which the audio thread leaves alone. Both change it only by compare-and-swap from
		// the state they decided on: a cue never pulls a voice away that the audio thread has started reading since,
		// and the audio thread never reads a voice that a cue has started to reposition.
		struct Voice
		{
			ObjectId channel = 0;
			ObjectId media = 0;
			std::shared_ptr<DiskStream> stream;
			std::atomic<uint64_t> state{0};
			std::atomic<time::Samples> cueFrame{0}; // where the last cue sent it, written before state
			std::atomic<time::Samples> next{-1};    // written by the audio thread: the frame its next read returns
			uint64_t cues = 0;                      // command side
		};

		// Audio thread, around every block.
		void beginBlock() noexcept;
		void endBlock() noexcept;

		// Audio thread, between beginBlock and endBlock: a voice of the channel's that reads media from frame, or
		// else the one that will soonest within the next frames (lead says after how many), or none.
		Voice* take(
			ObjectId channel, ObjectId media, time::Samples frame, time::Samples frames, time::Samples& lead) noexcept;
		void missed(time::Samples frames) noexcept; // frames played as silence for want of a voice
		uint64_t block() const noexcept { return m_block.load(std::memory_order_relaxed); }

	protected:
		// cue() calls this after reading where every voice is and before acting on it. The audio thread goes on
		// meanwhile; tests override it to play blocks at that moment.
		virtual void voicesRead() {}

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

		// Where the playback path ahead of the cursor enters entries in any layer: entry starts and cycle jumps.
		std::vector<Point> points(const Playlist& playlist, const Cue& cue) const;
		// The position frames of playback after the cursor.
		static time::Samples ahead(const Cue& cue, time::Samples frames);
		std::shared_ptr<Voice> open(ObjectId channel, ObjectId media, time::Samples frame);
		void retire(const std::shared_ptr<Voice>& voice);
		// Takes the voice from the audio thread to reposition or retire it, unless its state is no longer seen.
		static bool claim(Voice& voice, uint64_t seen);
		// Positions a claimed or new voice at frame, waiting there for the audio thread.
		static void place(Voice& voice, time::Samples frame);
		// Audio thread: takes the voice to read from next on, unless its state is no longer seen.
		bool startReading(Voice& voice, uint64_t seen, time::Samples next) const noexcept;

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
		// Plays an entry into its layer's frames.
		void play(const PlaylistEntry& entry, size_t layer, time::Samples position, time::Samples frames,
			uint32_t offset) noexcept;

		std::shared_ptr<const Playlist> m_playlist;
		ObjectId m_channel;
		Playback& m_playback;
		size_t m_bufferFrames = 0;
		size_t m_clipChannels = 1;   // of the clip with the most channels
		std::vector<float> m_buffer; // one block of interleaved frames of a clip
		std::vector<float> m_gains;  // one block of gains, where an entry fades
		std::vector<size_t> m_next;  // per layer, the next entry to play in a segment
		// Per layer, one block of what it plays, with the gains applied: frames of m_clipChannels samples, and the
		// number of the clip's channels in each frame (0 where the layer plays nothing).
		std::vector<float> m_layerAudio;
		std::vector<uint8_t> m_layerChannels;
		std::vector<uint8_t> m_layerUsed; // per layer, whether it plays in this block
	};
}
