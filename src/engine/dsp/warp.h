// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/audiofile/audiofile.h"
#include "engine/dsp/timestretch.h"

#include <memory>
#include <vector>

// Real-time warping of audio (#144, docs/features/warp.md): a file played through a time stretcher whose ratio follows
// a warp map, for musical mode and free warp.
namespace gnubase::engine::dsp
{
	// A point of a warp map: the source frame (of the file) plays at the target frame (of the warped audio, at the
	// file's sample rate). A map has two points at least, both increasing; it is linear between its points and goes
	// on with the slope of its last segment after it.
	struct WarpPoint
	{
		double source = 0.0;
		double target = 0.0;

		bool operator==(const WarpPoint& other) const { return source == other.source && target == other.target; }
	};

	// The target of a source frame, and the source frame at a target: linear between points, the end segments' slopes
	// beyond them.
	double warpTarget(const std::vector<WarpPoint>& map, double source);
	double warpSource(const std::vector<WarpPoint>& map, double target);

	// Reads a file warped by a map whose first target is 0: frame t of this reader is the file's audio at
	// warpSource(map, t), stretched in real-time mode by the slope of the map there (clamped to the real-time
	// range). It is as long as the map's last target. A disk stream reads it like any file, on its disk thread:
	// seeking resets the stretcher and allocates nothing else.
	class WarpReader : public AudioFileReader
	{
	public:
		// Throws std::invalid_argument for a map with fewer than two points or a file the stretcher can't take.
		WarpReader(std::unique_ptr<AudioFileReader> source, std::vector<WarpPoint> map, StretchPreset preset);
		~WarpReader() override;
		WarpReader(const WarpReader&) = delete;
		WarpReader& operator=(const WarpReader&) = delete;

		size_t read(float* buffer, size_t frames) override;

	protected:
		bool seekTo(time::Samples frame) override;

	private:
		// Starts over at a frame of this reader: the stretcher reset, its start pad to feed and its latency to drop.
		void restart(time::Samples frame);
		// Feeds the stretcher the next input frames, up to the next point of the map.
		void feed();

		std::unique_ptr<AudioFileReader> m_source;
		std::vector<WarpPoint> m_map;
		std::unique_ptr<TimeStretcher> m_stretcher;
		std::vector<float> m_interleaved;
		std::vector<std::vector<float>> m_planar;
		std::vector<float*> m_pointers;
		time::Samples m_input = 0;     // the next source frame to feed; before 0, silence
		time::Samples m_sourceAt = -1; // where the source reader is
		time::Samples m_position = 0;  // the next frame read() returns
		size_t m_drop = 0;             // output frames of latency still to drop
		bool m_final = false;          // the source's end was fed
	};
}
