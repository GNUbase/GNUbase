// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/audio/fades.h"
#include "engine/document/document.h"
#include "engine/dsp/warp.h"
#include "engine/midi/padvoices.h"

#include <array>
#include <cstddef>
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

	// A clip of the project: its media file (an id of RenderSnapshot::media) and its fades, for auditioning it on its
	// own (audition.start). Not read on the audio thread.
	struct RenderClip
	{
		ObjectId id = 0;
		ObjectId media = 0;
		Fade fadeIn;
		Fade fadeOut;
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
		int64_t rank = 0;                   // of events at the same front, the highest plays (AudioEvent::rank)
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
		// Which of tuning (1), pressure (2) and brightness (4) the note has a point of at its start (#176): MPE's
		// initial value of the others comes before it (addInitialExpressions).
		uint8_t startsWith = 0;
	};

	// A point of a note's per-note expression (#176), with the note's played pitch and channel.
	struct RenderExpression
	{
		time::Samples position = 0;
		uint8_t pitch = 60;
		uint8_t channel = 0;
		plugin::NoteExpression type = plugin::NoteExpression::Volume;
		double value = 0.0;
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

	// A chord or scale of the chord track from start (samples) on, until the next: its pitch classes, bit n for
	// pitch class n (C is 0).
	struct LiveHarmony
	{
		time::Samples start = 0;
		uint16_t classes = 0;
	};

	// What a note played into a track with a drum map plays (#882, reference manual pp. 1239-1240): through the I-notes
	// to the sound's pitch, then the sound's O-note on its channel, unless it is muted.
	struct LiveDrumNote
	{
		uint8_t key = 0;
		int8_t channel = -1; // 0-15, or -1 for the note's own (Any)
		bool mute = false;
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
		uint8_t midiChannel = 0;      // MIDI channels: the MIDI channel the above go out on, 0 to 15 (Any: 0)
		int8_t inputChannel = -1;     // MIDI channels: the one the events played into it go out on, -1 (Any) for theirs
		midi::Modifiers modifiers;    // MIDI and instrument channels: notes has them applied, live notes get them
		midi::EffectRack midiInserts; // MIDI and instrument channels: the effects their sequencer runs
		midi::EffectRack midiSends;
		// MIDI and instrument channels: where each MIDI send's effect plays, an instrument channel's index, or -1 for
		// the track's own output.
		std::array<int, midi::g_effectSlots> midiSendOutputs{-1, -1, -1, -1};
		// MIDI and instrument channels whose MIDI sends play before modifiers that change notes: the notes of their
		// parts without the track's MIDI modifiers, sorted as notes, and their expression points, sorted as
		// expressions; empty otherwise.
		std::vector<RenderNote> preNotes;
		std::vector<RenderExpression> preExpressions;
		// MIDI and instrument channels: the rules of their input transformer's modules that are on, the track's or
		// the project's, in order; empty for none.
		std::vector<midi::LogicalRules> inputTransformer;
		// MIDI and instrument channels: Live Input (#933, reference manual p. 1338), the chord track's chords or
		// scales that the notes played into the track move to, the nearest note of the one playing at the transport
		// position; empty for none.
		std::vector<LiveHarmony> liveInput;
		// MIDI and instrument channels: the track's articulation map (#175), whose remote triggers select the sound
		// slot of the notes played into the track; without slots for none.
		ArticulationMap articulations;
		// MIDI and instrument channels with a drum map: what each note played into the track plays, by its pitch (128
		// of them); empty without one.
		std::vector<LiveDrumNote> drumInput;
		bool monitor = false; // the monitor and record enable buttons
		bool recordEnable = false;
		// MIDI and instrument tracks: they play what comes from all MIDI inputs (MIDI thru), since they monitor or
		// are record-enabled and their MIDI input is all MIDI inputs.
		bool midiThru = false;
		// MIDI and instrument tracks whose MIDI input is the chord pads, and that monitor or are record-enabled: they
		// play the chord pads (#1089).
		bool padThru = false;
		int padPlayer = -1; // the chord pads' player whose track it is, or -1 for the selected one
		// Muted, silenced by another channel's solo, or on a disabled track: outputs silence. A quiet MIDI channel
		// has no notes or controllers, and a disabled track no events or parts.
		bool silent = false;
		// Silenced by another channel's solo or on a disabled track: silent but for the mute button. Its strips output
		// silence (ChannelStrip), which stays on through every switch between the mute button and mute automation.
		bool quiet = false;
		// Its mute button mutes it (no mute automation plays): it is the static value of the channel's mute lane
		// (channelAutomation), so the fader mutes it, and pre-fader sends follow it with the user setting
		// audio.mutePreSends.
		bool muted = false;
		bool listen = false; // also plays on the monitoring bus, after its fader (graph::NodeKind::ListenBus)
		AutoFades autoFades; // audio tracks: their own settings or the project's
		std::vector<RenderAudioEvent> audioEvents;    // sorted by start
		std::vector<RenderNote> notes;                // sorted by start, key switches first, then pitch
		std::vector<RenderController> controllers;    // sorted by position
		std::vector<RenderExpression> expressions;    // the notes' points that lie within them, sorted by position
		std::vector<RenderPart> midiParts;            // sorted by start
		std::vector<RenderAutomationLane> automation; // in the order of the lanes' ids
		std::vector<RenderSlot> slots;                // from top to bottom
		ChannelSections sections;                     // the strip's sections before the fader
		std::vector<RenderSend> sends;                // those with a destination, by number
		// The media files a sampler (#180) or a drum machine (#182) plays, by slot: the sampler's one, the drum
		// machine's pads; empty for none.
		std::vector<std::string> samples;
	};

	// A channel volume in dB as a linear gain; the fader's bottom, g_minChannelVolume, is silence.
	double volumeGain(double decibels);

	// The chord pads' remote keys (#174, reference manual p. 1386): per MIDI note, the pitches that a MIDI input's note
	// there plays instead while the pad remote control is active; keys with none play themselves.
	struct ChordPadKeys
	{
		static constexpr size_t g_maxPitches = 8;
		static constexpr size_t g_maxPlayers = 8; // the chord pads' players (#1089)
		bool active = false;
		uint8_t start = 36; // the remote range, which Filter Notes keeps from playing itself
		uint8_t end = 47;
		bool latch = false;
		bool filter = false;
		midi::PadOverlap overlap = midi::PadOverlap::HoldFirst;
		bool output = true; // Chord Pad Output Mode: the pads play into the tracks of all MIDI inputs too
		// The chord modifiers' remote keys (p. 1387), while they are active: previous and next voicing, fewer and more
		// tensions, transpose down and up. They play nothing; the command thread applies them (chordPads.remoteInput).
		bool modifiers = false;
		std::array<uint8_t, 6> modifierKeys{};
		// The MIDI input that the remote keys come from (p. 1386), chordPadPortHash of its midi.portSettings key such
		// as "dummy/in/Keys"; 0 for all inputs that belong to all MIDI inputs.
		uint64_t portHash = 0;
		// Snap Playback to Musical Grid (p. 1349): the grid in ticks, the quantize grid's; 0 while it's off.
		int64_t snapTicks = 0;
		// The pattern player mode (pp. 1379-1380): the pattern the pads' chords play while it's on.
		midi::PadPatternSteps pattern;
		// The sections player mode (pp. 1381-1385, 1389): its remote keys (255 for none), as ChordPads has them.
		bool sections = false;
		std::array<uint8_t, 5> sectionKeys{};
		std::array<uint8_t, 5> subsectionKeys{};
		std::array<uint8_t, 5> subsectionSections{};
		std::array<int16_t, 5> subsectionOffsets{};
		uint8_t sectionPlay = 0; // 0 sections, 1 chordPads, 2 combination
		bool latchPads = false;
		bool spread = false;
		std::array<bool, 5> mutedSections{};
		std::array<uint8_t, 3> playerKeys{}; // previous, next, mute player
		// What moves all pads' voicings, tensions and transposition (p. 1387): 0 nothing, 1 channel pressure, 2 pitch
		// bend, 3 a controller of controlNumbers.
		std::array<uint8_t, 3> controlSources{};
		std::array<uint8_t, 3> controlNumbers{};
		// Per player and key, as the player voices the pad's chord. The selected player and those with a track play
		// (players, a bit per player).
		std::array<std::array<uint8_t, 128>, g_maxPlayers> counts{};
		std::array<std::array<std::array<uint8_t, g_maxPitches>, 128>, g_maxPlayers> pitches{};
		uint8_t players = 1;
		uint8_t selected = 0;
	};

	// A MIDI port's key (backend/in/id) as ChordPadKeys::portHash has it: FNV-1a, 0 for none.
	inline uint64_t chordPadPortHash(const std::string& key)
	{
		if (key.empty())
			return 0;
		uint64_t hash = 14695981039346656037ULL;
		for (const char letter : key)
			hash = (hash ^ static_cast<unsigned char>(letter)) * 1099511628211ULL;
		return hash == 0 ? 1 : hash;
	}

	// What the audio thread needs from the project, compiled by the command thread after every change and handed
	// over with rt::SnapshotPublisher. Objects refer to each other by index and every position is in the project's
	// samples, so the audio thread never looks anything up. Immutable once published.
	struct RenderSnapshot
	{
		uint64_t revision = 0; // counts the snapshots compiled for an engine
		uint64_t opened = 0; // the open project (ProjectModel::openedRevision): another one when a project replaces it
		double sampleRate = 48000.0;
		time::TempoMap tempo{120.0}; // for processors that follow the tempo, e.g. a synced delay
		time::SignatureMap signatures;
		metronome::MetronomeSettings metronome;
		PanLaw panLaw = PanLaw::Minus3;
		double maxVolume = 6.0;               // dB
		std::string recordBitDepth = "int24"; // the project's, for audio.get; not read on the audio thread
		std::vector<RenderBus> busses;
		std::vector<RenderMedia> media;
		std::vector<RenderClip> clips;
		std::vector<RenderChannel> channels; // in track order
		// What arranger mode plays (#167); shared with the cue thread.
		std::shared_ptr<const ArrangerPlan> arranger = std::make_shared<const ArrangerPlan>();
		ChordPadKeys chordPadKeys;
	};

	// previous is the snapshot published before: its arranger plan stays if the new one plays the same, since the
	// transport finds its place again in every new plan. opened identifies the project (RenderSnapshot::opened).
	std::unique_ptr<const RenderSnapshot> compile(
		const Project& project, uint64_t revision, const RenderSnapshot* previous = nullptr, uint64_t opened = 0);

	// The arranger plan of a project: its arranger events and the active chain's steps in samples (arranger.cpp).
	ArrangerPlan arrangerPlan(const Project& project);
}
