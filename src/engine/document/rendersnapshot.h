// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gnubase::engine
{
	struct RenderBus
	{
		ObjectId id = 0;
		BusDirection direction = BusDirection::Output;
		ChannelFormat format = ChannelFormat::Stereo;
		std::vector<std::string> ports{}; // as in Bus
		int parent = -1;                  // child busses: an index into RenderSnapshot::busses
		std::vector<int> parentChannels{};
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
		Fade fadeIn; // fade lengths in the media file's sample frames
		Fade fadeOut;
		Fade clipFadeIn; // at the media file's start and end
		Fade clipFadeOut;
		Crossfade crossfade; // only one that applies (crossfadeApplies)
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
		ControllerType type = ControllerType::Control;
		uint8_t number = 0;
		uint32_t value = 0;
		uint8_t channel = 0;
		std::vector<uint8_t> data; // SysEx
	};

	struct RenderAutomationPoint
	{
		time::Samples position = 0;
		double value = 0.0; // in the target's unit; volume as a linear gain
		AutomationShape shape = AutomationShape::Linear;
		double curve = 0.0;

		bool operator==(const RenderAutomationPoint& other) const
		{
			return position == other.position && value == other.value && shape == other.shape && curve == other.curve;
		}
	};

	// A lane that plays: read-enabled and with points.
	struct RenderAutomationLane
	{
		AutomationTarget target;
		std::vector<RenderAutomationPoint> points; // sorted by position; points at one position in the order added
	};

	struct RenderChannel
	{
		ObjectId id = 0;
		ObjectId track = 0;
		TrackType type = TrackType::Audio;
		ChannelFormat format = ChannelFormat::Stereo;
		int input = -1; // index into RenderSnapshot::busses, or -1
		RenderTarget output;
		std::string instrument;       // instrument tracks: a built-in instrument's id, or empty
		double volume = 1.0;          // the fader's static value, a linear gain
		double pan = 0.0;             // the static pan, -1 to 1
		Panner panner = Panner::None; // by its format and its destination's
		StereoPanner stereoPanner = StereoPanner::Balance;
		double panLeft = -1.0; // the combined panner's static positions
		double panRight = 1.0;
		bool panBypass = false;
		std::optional<int> midiVolume; // MIDI channels: CC 7 and CC 10 to send, 0 to 127
		std::optional<int> midiPan;
		AutoFades autoFades;                          // audio tracks: their own settings or the project's
		std::vector<RenderAudioEvent> audioEvents;    // sorted by start
		std::vector<RenderNote> notes;                // sorted by start, then pitch
		std::vector<RenderController> controllers;    // sorted by position
		std::vector<RenderAutomationLane> automation; // in the order of the lanes' ids
	};

	// A channel volume in dB as a linear gain; the fader's bottom, g_minChannelVolume, is silence.
	double volumeGain(double decibels);

	// What the audio thread needs from the project, compiled by the command thread after every change and handed
	// over with rt::SnapshotPublisher. Objects refer to each other by index and every position is in the project's
	// samples, so the audio thread never looks anything up. Immutable once published.
	struct RenderSnapshot
	{
		uint64_t revision = 0; // counts the snapshots compiled for an engine
		double sampleRate = 48000.0;
		time::TempoMap tempo{120.0}; // for processors that follow the tempo, e.g. a synced delay
		PanLaw panLaw = PanLaw::Minus3;
		double maxVolume = 6.0; // dB
		std::vector<RenderBus> busses;
		std::vector<RenderMedia> media;
		std::vector<RenderChannel> channels; // in track order
	};

	std::unique_ptr<const RenderSnapshot> compile(const Project& project, uint64_t revision);
}
