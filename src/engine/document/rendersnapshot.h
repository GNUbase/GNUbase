// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace gnubase::engine
{
	struct RenderBus
	{
		ObjectId id = 0;
		BusDirection direction = BusDirection::Output;
		int channels = 2;
	};

	struct RenderMedia
	{
		ObjectId id = 0;
		std::string path;
		double sampleRate = 48000.0;
		int channels = 2;
		time::Samples length = 0;
	};

	// Where a channel's output goes: an index into RenderSnapshot::busses or RenderSnapshot::channels.
	struct RenderTarget
	{
		enum class Kind : uint8_t
		{
			None,
			Bus,
			Channel,
		};

		Kind kind = Kind::None;
		int index = -1;
	};

	struct RenderAudioEvent
	{
		ObjectId id = 0;
		time::Samples start = 0;  // in the project's samples
		int media = -1;           // index into RenderSnapshot::media
		time::Samples offset = 0; // in the media file's sample frames
		time::Samples length = 0; // in the media file's sample frames
		float gain = 1.0f;        // the event volume, linear
		bool muted = false;
	};

	struct RenderNote
	{
		time::Samples start = 0;
		time::Samples end = 0;
		uint8_t pitch = 60;
		uint16_t velocity = 0;
		uint8_t channel = 0;
	};

	struct RenderController
	{
		time::Samples position = 0;
		uint8_t number = 0;
		uint32_t value = 0;
		uint8_t channel = 0;
	};

	struct RenderChannel
	{
		ObjectId id = 0;
		ObjectId track = 0;
		TrackType type = TrackType::Audio;
		int channels = 2;
		int input = -1; // index into RenderSnapshot::busses, or -1
		RenderTarget output;
		std::vector<RenderAudioEvent> audioEvents; // sorted by start
		std::vector<RenderNote> notes;             // sorted by start, then pitch
		std::vector<RenderController> controllers; // sorted by position
	};

	// What the audio thread needs from the project, compiled by the command thread after every change and handed
	// over with rt::SnapshotPublisher. Objects refer to each other by index and every position is in the project's
	// samples, so the audio thread never looks anything up. Immutable once published.
	struct RenderSnapshot
	{
		uint64_t revision = 0; // counts the snapshots compiled for an engine
		double sampleRate = 48000.0;
		time::TempoMap tempo{120.0}; // for processors that follow the tempo, e.g. a synced delay
		std::vector<RenderBus> busses;
		std::vector<RenderMedia> media;
		std::vector<RenderChannel> channels; // in track order
	};

	std::unique_ptr<const RenderSnapshot> compile(const Project& project, uint64_t revision);
}
