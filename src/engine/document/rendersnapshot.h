// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/audio/fades.h"
#include "engine/document/document.h"
#include "engine/dsp/warp.h"

#include <cstdint>
#include <map>
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
		std::vector<std::string> ports; // as in Bus
		int parent = -1;                // child busses: an index into RenderSnapshot::busses
		std::vector<int> parentChannels;
		bool mainMix = false; // the default output
	};

	struct RenderMedia
	{
		ObjectId id = 0;
		std::string path;
		double sampleRate = 48000.0;
		int channels = 2;
		time::Samples length = 0;
		// An audio event of a warped clip plays a media file of its own, the event's id: path played through this
		// warp map (dsp::WarpReader) with the preset, from the event's start. Empty for a plain file.
		std::vector<dsp::WarpPoint> warp;
		dsp::StretchPreset preset = dsp::StretchPreset::Quality;
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
		Crossfade crossfade;                // only one that applies (crossfadeApplies)
		std::vector<GainPoint> volumeCurve; // in the media file's sample frames
		ObjectId front = 0;                 // frontOf: where events overlap, the highest plays
		// An event of a warped clip (RenderMedia::warp): where its clip's start and end play, in its media file's
		// frames, for the clip fades. Other events' clips start and end with their media files.
		time::Samples clipStart = 0;
		time::Samples clipEnd = 0;
	};

	struct RenderNote
	{
		time::Samples start = 0;
		time::Samples end = 0;
		uint8_t pitch = 60;
		uint16_t velocity = 0;
		uint8_t channel = 0;
		// An articulation map's key switch (#175): a jump sends the last one of its pitch before it, sounding or not.
		bool keySwitch = false;
	};

	struct RenderController
	{
		time::Samples position = 0;
		ControllerType type = ControllerType::Control;
		uint8_t number = 0;
		uint32_t value = 0;
		uint8_t channel = 0;
		std::vector<uint8_t> data; // SysEx
		int32_t part = -1;         // the index of its MIDI part in the channel's midiParts
	};

	struct RenderPart
	{
		time::Samples start = 0;
		time::Samples end = 0;
	};

	struct RenderAutomationPoint
	{
		time::Samples position = 0;
		double value = 0.0; // in the target's unit; volume as a linear gain
		AutomationShape shape = AutomationShape::Linear;
		double curve = 0.0;
		// A gap from here to the next point (virgin territory, or after a terminator): the lane's static value, which
		// channelAutomation() gives it; a mute lane's has the mute button's value already. A MIDI lane without a static
		// value of the channel sends nothing there.
		bool gap = false;

		bool operator==(const RenderAutomationPoint& other) const
		{
			return position == other.position && value == other.value && shape == other.shape && curve == other.curve &&
				gap == other.gap;
		}
	};

	// A lane that plays: read-enabled and with points, and with gap points where it has gaps.
	struct RenderAutomationLane
	{
		AutomationTarget target;
		std::vector<RenderAutomationPoint> points; // sorted by position; points at one position in the order added
	};

	// A plugin slot of a channel, as the engine makes and runs its plugin.
	// NOLINTNEXTLINE(bugprone-exception-escape): MSVC's std::map allocates when moved; only out of memory throws
	struct RenderSlot
	{
		ObjectId id = 0;
		std::string plugin; // "<format>:<id>"
		bool bypass = false;
		bool active = true;
		std::map<uint32_t, double> parameters;             // the values set on top of its state
		std::shared_ptr<const std::vector<uint8_t>> state; // its state from a file, or none
	};

	// A send with a destination (graph::NodeKind::Send).
	struct RenderSend
	{
		int number = 0;      // 0 to 7, its index in Channel::sends
		int destination = 0; // an index into RenderSnapshot::channels
		float gain = 0.0f;   // linear; 0 while it is off or silenced
		bool preFader = false;
	};

	// NOLINTNEXTLINE(bugprone-exception-escape): MSVC's std::map allocates when moved; only out of memory throws
	struct RenderChannel
	{
		ObjectId id = 0;
		ObjectId track = 0;
		TrackType type = TrackType::Audio;
		ChannelFormat format = ChannelFormat::Stereo;
		int input = -1; // index into RenderSnapshot::busses, or -1
		RenderTarget output;
		std::string instrument;                          // instrument tracks: a built-in instrument's id, or empty
		std::map<uint32_t, double> instrumentParameters; // its parameters' static values that aren't the defaults
		double volume = 1.0;                             // the fader's static value, a linear gain
		double pan = 0.0;                                // the static pan, -1 to 1
		Panner panner = Panner::None;                    // by its format and its destination's
		StereoPanner stereoPanner = StereoPanner::Balance;
		double panLeft = -1.0; // the combined panner's static positions
		double panRight = 1.0;
		bool panBypass = false;
		std::optional<int> midiVolume; // MIDI channels: CC 7 and CC 10 to send, 0 to 127
		std::optional<int> midiPan;
		std::optional<int> program; // MIDI channels: the program change to send, with bank select if bank is set
		std::optional<int> bank;
		midi::Modifiers modifiers;    // MIDI and instrument channels: notes has them applied, live notes get them
		midi::EffectRack midiInserts; // MIDI and instrument channels: the effects their sequencer runs
		midi::EffectRack midiSends;
		// MIDI and instrument channels: the rules of their input transformer's modules that are on, the track's or
		// the project's, in order; empty for none.
		std::vector<midi::LogicalRules> inputTransformer;
		bool monitor = false; // the monitor and record enable buttons
		bool recordEnable = false;
		// MIDI and instrument tracks: they play what comes from all MIDI inputs (MIDI thru), since they monitor or
		// are record-enabled and their MIDI input is all MIDI inputs.
		bool midiThru = false;
		// Muted, silenced by another channel's solo, or on a disabled track: outputs silence. A silent MIDI channel
		// has no notes or controllers, and a disabled track no events or parts.
		bool silent = false;
		// Its mute button mutes it (no mute automation plays): pre-fader sends follow it with the user setting
		// audio.mutePreSends.
		bool muted = false;
		bool listen = false; // also plays on the monitoring bus, after its fader (graph::NodeKind::ListenBus)
		AutoFades autoFades; // audio tracks: their own settings or the project's
		std::vector<RenderAudioEvent> audioEvents;    // sorted by start
		std::vector<RenderNote> notes;                // sorted by start, then pitch
		std::vector<RenderController> controllers;    // sorted by position
		std::vector<RenderPart> midiParts;            // sorted by start
		std::vector<RenderAutomationLane> automation; // in the order of the lanes' ids
		std::vector<RenderSlot> slots;                // from top to bottom
		ChannelSections sections;                     // the strip's sections before the fader
		std::vector<RenderSend> sends;                // those with a destination, by number
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
		time::SignatureMap signatures;
		metronome::MetronomeSettings metronome;
		PanLaw panLaw = PanLaw::Minus3;
		double maxVolume = 6.0;               // dB
		std::string recordBitDepth = "int24"; // the project's, for audio.get; not read on the audio thread
		std::vector<RenderBus> busses;
		std::vector<RenderMedia> media;
		std::vector<RenderChannel> channels; // in track order
		// What arranger mode plays (#167); shared with the cue thread.
		std::shared_ptr<const ArrangerPlan> arranger = std::make_shared<const ArrangerPlan>();
	};

	// previous is the snapshot published before: its arranger plan stays if the new one plays the same, since the
	// transport finds its place again in every new plan.
	std::unique_ptr<const RenderSnapshot> compile(
		const Project& project, uint64_t revision, const RenderSnapshot* previous = nullptr);

	// The arranger plan of a project: its arranger events and the active chain's steps in samples (arranger.cpp).
	ArrangerPlan arrangerPlan(const Project& project);
}
