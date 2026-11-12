// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/audio/arranger.h"
#include "engine/document/channelformat.h"
#include "engine/document/snap.h"
#include "engine/metronome/metronomesettings.h"
#include "engine/midi/effects.h"
#include "engine/midi/logical.h"
#include "engine/midi/modifiers.h"
#include "engine/midi/quantizesettings.h"
#include "engine/time/signaturemap.h"
#include "engine/time/tempomap.h"
#include "engine/time/timecode.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

// The project document: a tree of objects with stable ids. Only the engine's command thread reads and changes it,
// through the commands in projectmodel.cpp; the audio thread gets a compiled RenderSnapshot instead.
namespace gnubase::engine
{
	// Identifies an object in a project. Ids are never reused within a project; 0 means none.
	using ObjectId = uint64_t;

	// The most audio channels a media file can have: 9.1.6 surround. Busses and channels have a ChannelFormat.
	inline constexpr int g_maxAudioChannels = 16;

	enum class BusDirection : uint8_t
	{
		Input,
		Output,
	};

	// A connection to the audio hardware, e.g. "Stereo Out" (docs/features/busses.md).
	struct Bus
	{
		ObjectId id = 0;
		std::string name;
		BusDirection direction = BusDirection::Output;
		ChannelFormat format = ChannelFormat::Stereo;
		// The device port each channel uses, by name; empty for none. No ports at all: the bus takes consecutive
		// device ports in bus order. Child busses have none: they use their parent's.
		std::vector<std::string> ports;
		ObjectId parent = 0;             // child busses: a top-level bus of the same direction
		std::vector<int> parentChannels; // child busses: the parent's channel each of its channels is
		bool mainMix = false;            // the default output; exactly one top-level output bus has it
	};

	enum class TrackType : uint8_t
	{
		Audio,
		Instrument,
		Midi,
		Folder,    // contains other tracks; has a channel only if its group channel is on
		Group,     // sums the channels routed to it; holds no events
		Effect,    // receives sends; holds no events
		Marker,    // holds markers; has no channel and stays at the top level
		Ruler,     // shows a ruler in a time format of its own; has no channel
		Tempo,     // shows and edits the project's tempo track; has no channel, stays at the top level, at most one
		Signature, // shows and edits the project's time signatures; has no channel, stays at the top level, at most one
		Vca,       // a VCA fader (docs/features/link-groups.md): a channel without audio that controls a link group
		Arranger,  // holds the arranger events (#167); has no channel, stays at the top level, at most one
		Transpose, // holds the transpose events (#168); has no channel, stays at the top level, at most one
		Chord,     // holds the chord and scale events (#173); has no channel, stays at the top level, at most one
	};

	// The time formats of ruler tracks, as the event display's ruler names them.
	inline constexpr std::array<const char*, 5> g_rulerTimeFormats = {
		"barsBeats", "seconds", "timecode", "samples", "userFrames"};

	// A project has at most this many marker tracks.
	inline constexpr int g_maxMarkerTracks = 10;

	enum class StereoPanner : uint8_t
	{
		Balance,  // turns down the side panned away from
		Combined, // pans the left and the right channel each on its own, linked or apart
	};

	// The channel strip's sections before the fader (#107, docs/features/channel-strip.md): the pre section's gain,
	// phase and cut filters, the four-band equalizer and the strip's dynamics. All of them start off, so a new
	// channel passes its audio unchanged.
	struct CutFilter
	{
		bool on = false;
		double frequency = 20.0; // Hz: a low cut from 20 to 20000, a high cut from 50 to 20000
		int slope = 12;          // dB per octave: 6, 12, 24, 36 or 48
	};

	enum class EqBandType : uint8_t
	{
		Bell,
		LowShelf,  // band 1
		HighShelf, // band 4
		LowCut,    // band 1, 12 dB per octave
		HighCut,   // band 4, 12 dB per octave
	};

	struct EqBand
	{
		bool on = false;
		EqBandType type = EqBandType::Bell;
		double frequency = 1000.0; // Hz, 20 to 20000
		double gain = 0.0;         // dB, -24 to 24
		double q = 1.0;            // 0.1 to 12
	};

	struct StripGate
	{
		bool on = false;
		double threshold = -40.0; // dB, -80 to 0
		double range = -80.0;     // dB, -80 to 0: the gain while closed
		double attack = 0.1;      // ms, 0.01 to 100
		double release = 100.0;   // ms, 1 to 4000
	};

	struct StripCompressor
	{
		bool on = false;
		double threshold = -20.0; // dB, -60 to 0
		double ratio = 4.0;       // 1 to 20
		double attack = 10.0;     // ms, 0.1 to 100
		double release = 100.0;   // ms, 10 to 2000
		double makeup = 0.0;      // dB, 0 to 24
	};

	struct StripLimiter
	{
		bool on = false;
		double threshold = 0.0; // dB, -30 to 0: no sample leaves above it
		double release = 50.0;  // ms, 1 to 1000
	};

	struct ChannelSections
	{
		double gain = 0.0;  // the pre gain, dB, -24 to 24
		bool phase = false; // inverts the polarity
		CutFilter lowCut{false, 20.0, 12};
		CutFilter highCut{false, 20000.0, 12};
		bool eqBypass = false;
		std::array<EqBand, 4> eq = {
			{{false, EqBandType::LowShelf, 100.0, 0.0, 0.71}, {false, EqBandType::Bell, 500.0, 0.0, 1.0},
				{false, EqBandType::Bell, 2000.0, 0.0, 1.0}, {false, EqBandType::HighShelf, 8000.0, 0.0, 0.71}}};
		StripGate gate;
		StripCompressor compressor;
		StripLimiter limiter;
	};

	inline bool operator==(const CutFilter& a, const CutFilter& b)
	{
		return std::tie(a.on, a.frequency, a.slope) == std::tie(b.on, b.frequency, b.slope);
	}

	inline bool operator==(const EqBand& a, const EqBand& b)
	{
		return std::tie(a.on, a.type, a.frequency, a.gain, a.q) == std::tie(b.on, b.type, b.frequency, b.gain, b.q);
	}

	inline bool operator==(const StripGate& a, const StripGate& b)
	{
		return std::tie(a.on, a.threshold, a.range, a.attack, a.release) ==
			std::tie(b.on, b.threshold, b.range, b.attack, b.release);
	}

	inline bool operator==(const StripCompressor& a, const StripCompressor& b)
	{
		return std::tie(a.on, a.threshold, a.ratio, a.attack, a.release, a.makeup) ==
			std::tie(b.on, b.threshold, b.ratio, b.attack, b.release, b.makeup);
	}

	inline bool operator==(const StripLimiter& a, const StripLimiter& b)
	{
		return std::tie(a.on, a.threshold, a.release) == std::tie(b.on, b.threshold, b.release);
	}

	inline bool operator==(const ChannelSections& a, const ChannelSections& b)
	{
		return std::tie(a.gain, a.phase, a.lowCut, a.highCut, a.eqBypass, a.eq, a.gate, a.compressor, a.limiter) ==
			std::tie(b.gain, b.phase, b.lowCut, b.highCut, b.eqBypass, b.eq, b.gate, b.compressor, b.limiter);
	}

	// One of an audio channel's sends (reference manual pp. 552-557): a share of its audio, before or after its fader,
	// for a group or effect channel.
	struct Send
	{
		ObjectId destination = 0; // a group or effect channel, or 0
		bool on = false;
		double level = 0.0;    // dB, from g_minChannelVolume (silence) to the project's maximum volume
		bool preFader = false; // before the fader, so it doesn't follow the fader or the mute button
	};

	inline bool operator==(const Send& a, const Send& b)
	{
		return std::tie(a.destination, a.on, a.level, a.preFader) == std::tie(b.destination, b.on, b.level, b.preFader);
	}

	// An audio channel has this many sends (reference manual p. 542).
	inline constexpr size_t g_maxSends = 8;

	// The mixer channel of a track. MIDI tracks have a MIDI channel: no audio channels, and their output is an
	// instrument track's channel.
	// NOLINTNEXTLINE(bugprone-exception-escape): MSVC's std::map allocates when moved; only out of memory throws
	struct Channel
	{
		ObjectId id = 0;
		ObjectId track = 0;
		ChannelFormat format = ChannelFormat::Stereo; // the speaker arrangement; None for MIDI
		ObjectId input = 0;                           // audio tracks: an input bus, or 0
		ObjectId output = 0;    // an output bus or a group channel; MIDI: an instrument channel; or 0
		std::string instrument; // instrument tracks: the built-in instrument that plays, or empty (plugins: M05)
		double volume = 0.0;    // the fader, in dB from g_minChannelVolume to g_maxChannelVolume
		double pan = 0.0;       // mono channels' position, stereo channels' balance: -1 left, 0 center, 1 right
		StereoPanner panner = StereoPanner::Balance; // stereo channels
		double panLeft = -1.0; // the combined panner's positions of the left and the right channel, -1 to 1
		double panRight = 1.0;
		bool panBypass = false; // mono channels play at the center, stereo ones hard left and right
		// MIDI channels: the MIDI volume (CC 7) and pan (CC 10, 64 is the center) sent to the instrument, 0 to 127;
		// none sends nothing, which leaves the instrument's own.
		std::optional<int> midiVolume = std::nullopt;
		std::optional<int> midiPan = std::nullopt;
		// MIDI channels: the program (0 to 127) and bank (0 to 16383) sent to the instrument, none: nothing, and the
		// installed MIDI device (user setting midi.devices) whose patches choose them, by name, or empty
		// (docs/features/midi-devices.md).
		std::optional<int> program = std::nullopt;
		std::optional<int> bank = std::nullopt;
		// NOLINTNEXTLINE(readability-redundant-member-init): see instrumentParameters
		std::string device = {};
		// MIDI and instrument tracks: the MIDI modifiers, which change how the track's notes play
		// (docs/features/midi-modifiers.md).
		// NOLINTNEXTLINE(readability-redundant-member-init): see instrumentParameters
		midi::Modifiers modifiers = {};
		// MIDI and instrument tracks: the MIDI inserts and sends, the built-in MIDI effects that play the track's
		// events (docs/features/midi-effects.md). NOLINTNEXTLINE(readability-redundant-member-init): see
		// instrumentParameters
		midi::EffectRack midiInserts = {};
		// NOLINTNEXTLINE(readability-redundant-member-init): see instrumentParameters
		midi::EffectRack midiSends = {};
		// MIDI and instrument tracks: the input transformer, which changes what their MIDI input plays
		// (docs/features/midi-rules.md). NOLINTNEXTLINE(readability-redundant-member-init): see
		// instrumentParameters
		midi::InputTransformer inputTransformer = {};
		// The values set for the instrument's parameters (parameter.set), by parameter id; the others are at their
		// defaults. A new instrument starts with none.
		// The initializer lets Channel{...} leave it out without GCC's and Clang's -Wmissing-field-initializers, like
		// the members above with default values.
		// NOLINTNEXTLINE(readability-redundant-member-init): see above
		std::map<uint32_t, double> instrumentParameters = {};
		// Sampler tracks, instrument tracks that play the built-in sampler (#180): the pool clip whose media file the
		// sampler plays, or 0 for none (docs/features/sampler-track.md).
		ObjectId sample = 0;
		// Audio, MIDI and instrument tracks: the monitor and record enable buttons
		// (docs/features/latency-and-monitoring.md, docs/features/midi-recording.md).
		bool monitor = false;
		bool recordEnable = false;
		// MIDI and instrument tracks: where their MIDI input comes from, "all" (all MIDI inputs) or "none".
		std::string midiInput = "all";
		// The mute and solo buttons (docs/features/solo-mute-listen.md): a muted channel is silent, unless mute
		// automation plays (docs/features/automation.md); while any channel is soloed, only soloed ones, where they
		// play through and what plays into them are heard, and channels with solo defeat, with where they play through.
		// A listening channel also plays on the monitoring bus.
		bool mute = false;
		bool solo = false;
		bool soloDefeat = false;
		bool listen = false;
		// Audio channels: the plugin slots, from top to bottom; the audio passes through them before the fader.
		// NOLINTNEXTLINE(readability-redundant-member-init): see instrumentParameters
		std::vector<ObjectId> slots = {};
		// The notepad (reference manual p. 168): the user's notes on the channel, in the inspector and the mixer.
		// NOLINTNEXTLINE(readability-redundant-member-init): see instrumentParameters
		std::string notes = {};
		// Audio channels: the strip's sections before the fader. Pre runs before the plugin slots, the equalizer and
		// the dynamics after them.
		// NOLINTNEXTLINE(readability-redundant-member-init): see instrumentParameters
		ChannelSections sections = {};
		// Audio channels: the sends, in the order of their numbers 1 to 8.
		// NOLINTNEXTLINE(readability-redundant-member-init): see instrumentParameters
		std::array<Send, g_maxSends> sends = {};
	};

	// A channel has at most this many plugin slots (reference manual p. 544).
	inline constexpr size_t g_maxSlots = 16;

	// A plugin in one of a channel's slots (#282). The plugin itself lives in the engine, a built-in one in the
	// engine's process and any other in a plugin host process; the document keeps what makes it again.
	// NOLINTNEXTLINE(bugprone-exception-escape): MSVC's std::map allocates when moved; only out of memory throws
	struct Slot
	{
		ObjectId id = 0;
		ObjectId channel = 0;
		std::string plugin;  // "<format>:<id>", e.g. "builtin:eq"; one this build doesn't know stays and passes audio
		std::string name;    // the plugin's name when it was inserted, for plugins this build doesn't know
		bool bypass = false; // the plugin keeps processing, but the audio passes it unchanged
		bool active = true;  // off: the plugin stays in its slot, deactivated, and the audio passes it unchanged
		// The values set for its parameters (parameter.set), by parameter id, on top of its state.
		// NOLINTNEXTLINE(readability-redundant-member-init): see Channel::instrumentParameters
		std::map<uint32_t, double> parameters = {};
		// The plugin's own state from a file, loaded before the parameters; none: its defaults. Shared, since copies
		// of the project (undo, render snapshots) don't change it.
		// NOLINTNEXTLINE(readability-redundant-member-init): see Channel::instrumentParameters
		std::shared_ptr<const std::vector<uint8_t>> state = {};
	};

	// The fader's range: its bottom is silence, its top the project's maximum volume, at most this.
	inline constexpr double g_minChannelVolume = -144.0;
	inline constexpr double g_maxChannelVolume = 12.0;

	// An audio file on disk.
	struct MediaFile
	{
		ObjectId id = 0;
		std::string path;
		double sampleRate = 48000.0;
		int channels = 2;
		time::Samples length = 0; // in the file's sample frames
	};

	// The curve of a fade in, from silence at 0 to full volume at 1 (src/engine/audio/fades.h has the formulas). A
	// fade out plays it backwards.
	enum class FadeShape : uint8_t
	{
		Linear,
		Exponential, // starts slowly, rises fast: about -15 dB half way
		Logarithmic, // rises fast, then slowly: the mirror image of exponential, about -1.7 dB half way
		SCurve,      // half a cosine: slow at both ends, -6 dB half way
		EqualPower,  // a quarter sine, -3 dB half way: a fade out and a fade in keep the power constant
	};

	// A fade in or out. Its length is in the clip's sample frames; 0 means none.
	struct Fade
	{
		time::Samples length = 0;
		FadeShape shape = FadeShape::Linear;
	};

	enum class CrossfadeMode : uint8_t
	{
		EqualGain,  // the fade out mirrors the fade in, so their gains add up to 1
		EqualPower, // both are quarter sines, so their power adds up to 1
		Custom,     // the fade in and the fade out have shapes of their own
	};

	// A crossfade from the event before (from) into this one, where the two overlap. Both play there: from fades out
	// over outLength up to its end, this event fades in over length from its start. A length of 0 is the whole
	// overlap, so equal lengths make the crossfade symmetric.
	struct Crossfade
	{
		ObjectId from = 0; // 0: no crossfade
		CrossfadeMode mode = CrossfadeMode::EqualGain;
		FadeShape shape = FadeShape::Linear;    // the fade in
		FadeShape outShape = FadeShape::Linear; // the fade out; for equal gain and equal power, derived from shape
		time::Samples length = 0;               // in this event's clip frames
		time::Samples outLength = 0;            // in from's clip frames
	};

	inline constexpr double g_minAutoFadeLength = 1.0; // milliseconds
	inline constexpr double g_maxAutoFadeLength = 500.0;

	// Short fades and crossfades applied automatically to the events of audio tracks, against clicks. Auto fades go
	// where an event has no fade of its own at that end, never on top of one; auto crossfades where an event starts on
	// top of another or ends above one that goes on, unless the two have a crossfade of their own. Where the event in
	// front has a fade of its own at that end, its fade is the auto crossfade's, and the one behind fades with the
	// mirror image over the same stretch.
	struct AutoFades
	{
		bool fadeIn = false;
		bool fadeOut = false;
		bool crossfades = false;
		double length = 10.0; // milliseconds, g_minAutoFadeLength to g_maxAutoFadeLength
		FadeShape fadeInShape = FadeShape::Linear;
		FadeShape fadeOutShape = FadeShape::Linear;
		CrossfadeMode crossfadeMode = CrossfadeMode::EqualGain;
		FadeShape crossfadeShape = FadeShape::Linear; // the fade in, as in Crossfade
	};

	// A named section of a clip that marks something in its audio (#140, reference manual pp. 624-626): its start and
	// end in the clip's sample frames, start before end.
	struct ClipRegion
	{
		ObjectId id = 0;
		std::string name;
		time::Samples start = 0;
		time::Samples end = 0;
	};

	// A position in a clip where a sound starts (#143, reference manual pp. 636-641): its clip frame, the peak level
	// of the sound there in dBFS and its intensity, how sharply its level rises (0 to 1, 1 for the sharpest of the
	// clip's detected ones), as clip.detectHitpoints finds them; whether it is locked, so that the filters keep it,
	// as hitpoints inserted or moved by hand are; and whether it is disabled, so that it counts for nothing.
	struct Hitpoint
	{
		time::Samples position = 0;
		double peak = 0.0;
		double intensity = 0.0;
		bool locked = false;
		bool disabled = false;
	};

	inline constexpr double g_minHitpointThreshold = -60.0; // dBFS

	// The Hitpoints section's filters (p. 638): the hitpoints that count are those that aren't disabled and are
	// locked, or have a peak of threshold dBFS or more and an intensity of intensity (0 to 1) or more, and start
	// minimumLength clip frames or more after the one before that counts.
	struct HitpointFilter
	{
		double threshold = g_minHitpointThreshold; // g_minHitpointThreshold to 0
		double intensity = 0.0;
		time::Samples minimumLength = 0;
	};

	// A beat of a clip's definition grid that isn't where the clip's tempo puts it (#144, Auto Adjust and Manual
	// Adjust, reference manual pp. 651-654): the clip frame it is at. Beats count in quarter notes from the grid start.
	struct BeatEdit
	{
		int64_t beat = 0;
		time::Samples frame = 0;
	};

	// A free warp marker (#144, reference manual pp. 654-656): the audio at the clip frame position plays where the
	// clip frame target would without it. The audio between markers, and between them and the clip's ends, is
	// stretched to fit.
	struct WarpMarker
	{
		time::Samples position = 0;
		time::Samples target = 0;
	};

	// How a clip's audio is warped in real time (#144, docs/features/warp.md). Its definition is its tempo (BPM of
	// quarter notes; 0 until one is set or musical mode is turned on), beats per bar, the clip frame of its first beat
	// and the beats edited from there. In musical mode its beats follow the project's tempo. Its warp markers move
	// sections of its audio in time. preset is the time stretch algorithm (dsp::stretchPresetNames()).
	// The warp markers a clip had before warp quantizing (#145) first changed them, and what it made of them.
	// While the clip still has those, quantizing starts from the original ones and resetting puts them back.
	struct WarpQuantizeOrigin
	{
		std::vector<WarpMarker> original;
		std::vector<WarpMarker> quantized;
	};

	struct ClipWarp
	{
		bool musical = false;
		double tempo = 0.0;
		int beatsPerBar = 4;
		time::Samples gridStart = 0;
		std::vector<BeatEdit> beats;     // by beat, frames increasing
		std::vector<WarpMarker> markers; // by position, targets increasing
		std::string preset = "quality";
		std::optional<WarpQuantizeOrigin> quantizeOrigin;
	};

	// What audio events play: for now, a whole media file. Its fades apply to every event that plays it, at the
	// file's start and end.
	struct Clip
	{
		ObjectId id = 0;
		std::string name;
		ObjectId media = 0;
		Fade fadeIn;
		Fade fadeOut;
		ObjectId folder = 0;             // the pool folder it is in; 0: the pool's Audio folder itself
		std::vector<ClipRegion> regions; // by start, then id
		std::vector<Hitpoint> hitpoints; // by position, one at most at each
		HitpointFilter hitpointFilter;
		ClipWarp warp;
	};

	// Which of a clip's hitpoints count, as its filters leave them (hitpoints.cpp): one for each of them.
	std::vector<bool> activeHitpoints(const Clip& clip);

	// A folder of the pool that clips are organized in (#93, reference manual p. 757). It only organizes the pool:
	// the files stay where they are.
	struct PoolFolder
	{
		ObjectId id = 0;
		std::string name;
		ObjectId parent = 0; // 0: in the pool's Audio folder
	};

	// A point of an audio event's volume curve (#139): where in the clip it is (its sample frames, so the curve moves
	// with the event and stays with the audio when the event is resized or split), the volume there in dB
	// (g_minEventVolume to g_maxEventVolume), and how the curve bends on its way to the next point (-1 to 1, as
	// AutomationShape::Curve bends).
	struct VolumePoint
	{
		time::Samples position = 0;
		double volume = 0.0;
		double curve = 0.0;
	};

	// Plays a section of a clip on an audio track.
	struct EventQuantizeOrigin
	{
		time::Position original;
		time::Position quantized;
	};

	struct AudioEvent
	{
		ObjectId id = 0;
		ObjectId track = 0;
		time::Position position; // in the track's time base
		ObjectId clip = 0;
		time::Samples offset = 0; // where in the clip playback starts, in the clip's sample frames
		time::Samples length = 0; // in the clip's sample frames
		double volume = 0.0;      // dB, from g_minEventVolume to g_maxEventVolume
		bool mute = false;
		bool lock = false;   // event.* commands don't move, resize, split, glue or remove it
		Fade fadeIn;         // from the event's start
		Fade fadeOut;        // up to the event's end
		Crossfade crossfade; // from the event before; replaces the fade in, and from's fade out
		// Where in the clip (its sample frames) the event snaps (reference manual p. 105); none, or outside the
		// event: its start.
		std::optional<time::Samples> snapPoint;
		std::string color; // "#rrggbb", or empty to follow the track's color
		// Its volume curve by position, on top of volume: level with the first point before it and with the last one
		// after it; without points, 0 dB. Points at the same position make a jump.
		std::vector<VolumePoint> volumeCurve;
		// The audio part it is in, on the same track, or 0 for none. It keeps its position on the track: it plays
		// only where the part is, and moves, splits and goes with the part.
		ObjectId part = 0;
		int lane = 0; // the track's lane it is on, from 0: cycle recordings put their takes on lanes (#98)
		// Where it is in the stack of its track's events (#99): where events overlap, the one whose frontOf is highest
		// plays. 0 orders it by its id, so an event added later is in front; event.toFront gives it a new id here.
		ObjectId front = 0;
		// Where it was before quantizing (#145) first moved it, and where quantizing put it. While it is there,
		// quantizing starts from the original position and resetting puts it back; moved by other means, it is
		// where it is.
		std::optional<EventQuantizeOrigin> quantizeOrigin;
	};

	inline ObjectId frontOf(const AudioEvent& event)
	{
		return event.front != 0 ? event.front : event.id;
	}

	// A container of audio events on an audio track (#471, reference manual p. 250). Its events are the audio events
	// whose part it is; they play only within it.
	struct AudioPart
	{
		ObjectId id = 0;
		ObjectId track = 0;
		std::string name;
		time::Position position; // in the track's time base
		int64_t length = 0;      // in the unit of the position's time base: ticks or samples
		bool mute = false;       // its events don't play
		bool lock = false;       // event.* commands don't move, resize, split, glue or remove it
		std::string color;       // "#rrggbb", or empty to follow the track's color
		int lane = 0;            // the track's lane it is on, from 0
	};

	inline constexpr double g_minEventVolume = -144.0;
	inline constexpr double g_maxEventVolume = 6.0;

	// What quantizing changes in a note.
	struct NoteTiming
	{
		time::Ticks position = 0;
		time::Ticks length = 0;
		uint16_t velocity = 0;
	};

	// A quantized note's timing before it was first quantized, and what quantizing made of it (#132). Resetting puts
	// the original back. A note whose position no longer is the quantized one was moved by other means, such as by
	// hand: its original no longer counts. A velocity or length changed by other means stays (midi/quantize.h).
	struct QuantizeOrigin
	{
		NoteTiming original;
		NoteTiming quantized;
	};

	// A note's release velocity unless it has one of its own: MIDI 1.0's 64, the center.
	inline constexpr uint16_t g_defaultReleaseVelocity = 0x8000;

	// Note and controller values have the resolution of MIDI 2.0 (#19): MIDI 1.0 values fit without loss.
	struct Note
	{
		ObjectId id = 0;
		time::Ticks position = 0; // from the start of the part
		time::Ticks length = 0;
		uint8_t pitch = 60;                                  // 0-127
		uint16_t velocity = 0;                               // 0-65535
		uint8_t channel = 0;                                 // 0-15
		uint16_t releaseVelocity = g_defaultReleaseVelocity; // 0-65535, the note-off velocity
		std::optional<QuantizeOrigin> origin;                // set while the note is quantized
		double probability = 1.0;                            // 0-1, the chance that it plays (#128)
		double velocityVariance = 0.0; // -1 to 1, how far playback varies the velocity, up or down (#128)
		// The attributes of the track's articulation map that the note has, by name (#175)
		std::vector<std::string> articulations;
	};

	// The MIDI events besides notes, all held as controller events.
	enum class ControllerType : uint8_t
	{
		Control,         // a control change: number is the controller
		PitchBend,       // value: midi::g_pitchBendCenter is no bend
		ChannelPressure, // value
		PolyPressure,    // number is the pitch; value
		Program,         // number is the program; value is the bank, or g_noBank
		SysEx,           // data is the whole message, from 0xF0 to 0xF7
		// A direction of the track's articulation map (#175): articulation is its name; value 1 makes it a reset
		// event, which ends the directions of its group. Plays nothing itself.
		Articulation,
	};

	inline constexpr uint32_t g_noBank = 0xFFFFFFFFU;
	inline constexpr uint32_t g_maxBank = 16383; // MIDI's bank select: 14 bits

	struct Controller
	{
		ObjectId id = 0;
		time::Ticks position = 0; // from the start of the part
		ControllerType type = ControllerType::Control;
		uint8_t number = 0;        // 0-127
		uint32_t value = 0;        // 0-4294967295
		uint8_t channel = 0;       // 0-15
		std::vector<uint8_t> data; // SysEx
		std::string articulation;  // Articulation
	};

	// Holds notes and controllers on an instrument or MIDI track. Notes and controllers outside it are not played.
	// NOLINTNEXTLINE(bugprone-exception-escape): MSVC's std::map allocates when moved; only out of memory throws
	struct MidiPart
	{
		ObjectId id = 0;
		ObjectId track = 0;
		std::string name;
		time::Position position; // in the track's time base
		int64_t length = 0;      // in the unit of the position's time base: ticks or samples
		bool mute = false;       // doesn't play
		bool lock = false;       // event.* commands don't move, resize, split, glue or remove it
		// Shared copies (docs/features/parts-and-events.md) have the same id here and the same notes and controllers,
		// with the same ids: a change to one goes to the others. 0: not shared.
		ObjectId shared = 0;
		std::string color; // "#rrggbb", or empty to follow the track's color
		int lane = 0;      // the track's lane it is on, from 0: cycle recordings put their takes on lanes (#98)
		std::map<ObjectId, Note> notes;
		std::map<ObjectId, Controller> controllers;
		// Added to the track's MIDI modifiers' transpose and velocity shift as it plays (reference manual p. 1107),
		// -127 to 127: the info line's Transpose and Velocity.
		int transpose = 0;
		int velocity = 0;
		// Global transposition (#168, reference manual pp. 414-421): the part's root key (-1 for none, 0 C to 11 B),
		// which the project root key transposes to, and Global Transpose Independent, which keeps the part out of the
		// project root key's and the transpose track's transposition.
		int rootKey = -1;
		bool independent = false;
		// A pattern event (#181): the pattern it plays from its start, over and over, instead of its notes; 0 for an
		// ordinary part.
		ObjectId pattern = 0;
		// Where in its pattern a pattern event starts, in ticks: how far the pattern went before it, after a split or
		// a start sized later (earlier: below 0), so that it continues the pattern.
		time::Ticks patternOffset = 0;
	};

	// A frozen track's rendered audio (#116, reference manual pp. 219-220): a file of the project's media folder's
	// Freeze folder that plays from the project's start instead of the track's events, instrument and plugin slots.
	struct Freeze
	{
		std::string path;
		double sampleRate = 48000.0;
		int channels = 2;
		time::Samples length = 0; // in the file's sample frames
	};

	inline bool operator==(const Freeze& a, const Freeze& b)
	{
		return std::tie(a.path, a.sampleRate, a.channels, a.length) ==
			std::tie(b.path, b.sampleRate, b.channels, b.length);
	}

	// A track version (#165, reference manual pp. 230-237): one of the sets of events and parts an audio, instrument
	// or MIDI track can switch between. The active version's events and parts are the project's; another version
	// holds its own here, as they were when it was last active, so playback, editing and the event display only ever
	// see the active one.
	struct TrackVersion
	{
		int id = 1;              // the version ID: versions made together on several tracks share it
		std::string name = "v1"; // the version name
		std::vector<AudioEvent> audioEvents;
		std::vector<AudioPart> audioParts;
		std::vector<MidiPart> midiParts;
	};

	bool operator==(const TrackVersion& a, const TrackVersion& b);

	struct Track
	{
		ObjectId id = 0;
		TrackType type = TrackType::Audio;
		std::string name;
		std::string color; // "#rrggbb", or empty for the default color
		time::TimeBase timeBase = time::TimeBase::Musical;
		ObjectId parent = 0;                // a folder track, or 0 at the top level
		ObjectId channel = 0;               // 0 for folders without a group channel
		std::vector<ObjectId> children;     // folders: the tracks inside, from top to bottom
		std::optional<AutoFades> autoFades; // audio tracks: settings of their own instead of the project's
		std::string timeFormat;             // ruler tracks: one of g_rulerTimeFormats
		bool enabled = true; // audio, instrument and MIDI tracks: a disabled one plays nothing and is silent
		// An image file that the mixer shows on the track's channel (#106), or empty. The initializer lets Track{...}
		// leave it out, as for Channel::instrumentParameters.
		// NOLINTNEXTLINE(readability-redundant-member-init): see above
		std::string picture = {};
		// Ruler tracks (reference manual p. 159): where their ruler counts from while timeOffsetOn, in the track's time
		// base (ticks or samples); while it is off, the ruler follows the project setup's start time and bar offset.
		int64_t timeOffset = 0;
		bool timeOffsetOn = false;
		// Audio and instrument tracks: what plays while the track is frozen, or none. Initialized like picture.
		// NOLINTNEXTLINE(readability-redundant-member-init): see above
		std::optional<Freeze> frozen = {};
		ObjectId drumMap = 0;         // MIDI and instrument tracks: the drum map their notes play through, or 0 (#129)
		ObjectId articulationMap = 0; // MIDI and instrument tracks: their articulation map, or 0 (#175)
		// Audio, instrument and MIDI tracks: their versions in the order they were made; none for a track that has
		// only its first one, v1 with ID 1 (versionsOf). Initialized like picture.
		// NOLINTNEXTLINE(readability-redundant-member-init): see above
		std::vector<TrackVersion> versions = {};
		int activeVersion = 1; // the ID of the active version
	};

	// Whether tracks of a type have versions: audio, instrument and MIDI tracks.
	inline bool hasVersions(TrackType type)
	{
		return type == TrackType::Audio || type == TrackType::Instrument || type == TrackType::Midi;
	}

	// A track's versions, with its first one if it has none of its own yet.
	std::vector<TrackVersion> versionsOf(const Track& track);

	// Calls visit with every audio event of the project, those that inactive track versions hold too: their clips are
	// in use. Project may be const.
	template <typename P, typename Visit> void forEveryAudioEvent(P& project, Visit visit)
	{
		for (auto& entry : project.audioEvents)
			visit(entry.second);
		for (auto& entry : project.trackById)
			for (auto& version : entry.second.versions)
				for (auto& event : version.audioEvents)
					visit(event);
	}

	// Calls visit with the pool clip of every sampler track (#180): their clips are in use too. Project may be const.
	template <typename P, typename Visit> void forEverySamplerClip(P& project, Visit visit)
	{
		for (auto& entry : project.channels)
			if (entry.second.sample != 0)
				visit(entry.second.sample);
	}

	// Whether tracks of a type can be disabled (reference manual p. 219): audio, instrument and MIDI tracks.
	inline bool canDisable(TrackType type)
	{
		return type == TrackType::Audio || type == TrackType::Instrument || type == TrackType::Midi;
	}

	enum class MarkerType : uint8_t
	{
		Position, // a position on the timeline
		Cycle,    // a range, e.g. a section of a song, that can become the locators
	};

	// A marker on a marker track. Position and cycle markers are numbered separately on each track, from 1; key
	// commands use the numbers.
	struct Marker
	{
		ObjectId id = 0;
		ObjectId track = 0;
		MarkerType type = MarkerType::Position;
		int number = 1;
		time::Position position; // in the track's time base; a cycle marker's start
		time::Position end;      // cycle markers: at least one sample after the position; position markers: the same
		std::string name;
		std::string color; // "#rrggbb", or empty for the default color
	};

	// An arranger event (#167, reference manual p. 404): a section of the project on the arranger track, in its time
	// base, at least one sample long.
	struct ArrangerEvent
	{
		ObjectId id = 0;
		std::string name;
		time::Position position;
		time::Position end;
	};

	// A step of an arranger chain: an arranger event, played repeats times.
	struct ArrangerStep
	{
		ObjectId event = 0;
		int repeats = 1;
		ArrangerRepeat mode = ArrangerRepeat::Normal;
	};

	// An arranger chain: the order in which arranger mode plays the arranger events.
	struct ArrangerChain
	{
		ObjectId id = 0;
		std::string name;
		std::vector<ArrangerStep> steps;
	};

	// The arranger track's events and chains (#167). A project with an arranger track has at least one chain.
	struct Arranger
	{
		std::vector<ArrangerEvent> events; // in the order they were added
		std::vector<ArrangerChain> chains; // in the order they were added
	};

	// A transpose event (#168, reference manual p. 418): from its position, in the transpose track's time base, to the
	// next one, what follows global transpose plays value semitones (-24 to 24) higher.
	struct TransposeEvent
	{
		ObjectId id = 0;
		time::Position position;
		int value = 0;
	};

	// The transpose track's events and controls (reference manual pp. 165, 418-419).
	struct TransposeTrack
	{
		std::vector<TransposeEvent> events; // in timeline order
		bool mute = false;                  // Mute Transpose Events: the events don't play
		bool keepInOctave = false;          // Keep Transpose in Octave Range: never more than 7 semitones either way
		bool lock = false;                  // the events can't be added, changed or removed
	};

	// A chord track event (#173, reference manual pp. 1320, 1330), in the chord track's time base: a chord event's text
	// is its chord's name (midi::chordName, "X" for an undefined chord), a scale event's its scale's (midi::scaleName,
	// such as "D dorian"). Each plays from its position to the next of its kind.
	struct ChordEvent
	{
		ObjectId id = 0;
		time::Position position;
		std::string text;
		// A chord event made from an audio event (reference manual pp. 1327, 1343-1344): the audio event, and the
		// chords detected there, the most likely first; none for other events.
		ObjectId audioEvent = 0;
		std::vector<std::string> detected;
	};

	// The harmony a track's notes were played in when it began to follow the chord track (reference manual p. 1341):
	// chords by position in ticks, each until the next.
	struct ChordSource
	{
		time::Ticks position = 0;
		std::string chord;
	};

	// How a track follows the chord track (reference manual p. 1340): a mode of midi::followModeNames(), and the
	// harmony its notes are mapped from.
	struct ChordFollow
	{
		std::string mode = "off";
		std::vector<ChordSource> source;
		// This Track's Voicings (p. 1338): "chordTrack" (the chord track's voicings) or a library, "basic", "piano"
		// or "guitar"; the voice of Single Voice, 0 soprano to 3 bass; and the track whose voicings a Single Voice
		// track takes as its master, 0 for its own.
		std::string voicings = "chordTrack";
		int voice = 0;
		ObjectId master = 0;
		// Live Input (p. 1338): "off", "chords" or "scales", what the notes played into the track move to.
		std::string liveInput = "off";
	};

	// The chord track's voicing library (reference manual pp. 1333-1335), as midi::VoicingStyle has it.
	struct ChordVoicing
	{
		std::string library = "basic";
		std::string style;
		bool addRoot = true;
		bool duplicateRoot = false;
		bool fattenUp = false;
		int lowestRoot = 36;
		int lowest = 52;
		int highest = 84;
	};

	// The chord track's events and settings (reference manual pp. 1319-1345).
	struct ChordTrack
	{
		std::vector<ChordEvent> events;         // chord events, in timeline order
		std::vector<ChordEvent> scales;         // scale events, in timeline order
		int octaveOffset = 0;                   // the basic voicings' Octave Offset from C3, -3 to 3
		bool adaptiveVoicings = true;           // voicings that move the voices the least
		bool automaticScales = true;            // each chord event's scale is its chord's, not the scale events'
		std::map<ObjectId, ChordFollow> follow; // by MIDI or instrument track
		ChordVoicing voicing;                   // the voicing library; basic uses octaveOffset only
		time::Ticks shiftChords = 0;            // Shift Chords: where the chord events take effect, from them
		bool mute = false;                      // Mute Chord Track: no track follows it and nothing auditions
		bool showScales = false;                // Show Scales: the scale lane shows
		bool recordEnable = false;              // recorded MIDI notes become chord events
		ObjectId audition = 0;                  // Select Track for Auditioning: a MIDI or instrument track, or 0
		bool acousticFeedback = false;          // the chord events play on the audition track
	};

	// A chord pad (#174, reference manual pp. 1352-1353): its chord's name (midi::chordName), empty while unassigned;
	// its voicing, the inversion of its basic voicing it plays; adaptive voicing; and whether it is locked.
	struct ChordPad
	{
		std::string chord;
		int voicing = 0; // -12 to 12
		bool adaptive = true;
		bool locked = false;
	};

	// The chord pads (#174, reference manual pp. 1346-1391), saved with the project. A new project has twelve pads with
	// the major preset on C.
	struct ChordPads
	{
		std::vector<ChordPad> pads = {{"C", 0, true, false}, {"Dm", 0, true, false}, {"Em", 0, true, false},
			{"F", 0, true, false}, {"G", 0, true, false}, {"Am", 0, true, false}, {"Bdim", 0, true, false},
			{"", 0, true, false}, {"", 0, true, false}, {"", 0, true, false}, {"", 0, true, false},
			{"", 0, true, false}};
		int rootKey = 0;              // 0 C to 11 B
		int reference = -1;           // the adaptive voicing reference pad, or -1
		int octaveOffset = 0;         // the basic player's Octave Offset, -3 to 3
		int velocity = 100;           // of pads triggered with the mouse
		bool remote = false;          // notes from MIDI inputs on the remote keys trigger the pads
		int remoteStart = 36;         // C1: the first pad's remote key
		int remoteEnd = 47;           // B1: the last remote key
		std::string preset = "major"; // the preset loaded last
		bool stepInput = false;       // triggered pads become chord events (chordPads.step)
		bool moveInsert = false;
		time::Ticks stepPosition = 0;
	};

	// What an automation lane changes.
	enum class AutomationTargetType : uint8_t
	{
		Volume,          // the channel's fader, in dB
		Pan,             // the channel's pan, -1 to 1
		PanLeft,         // the combined panner's position of the left channel, -1 to 1
		PanRight,        // and of the right channel
		Mute,            // the channel's mute button: 0 off, 1 on; a step curve
		Instrument,      // a parameter of the channel's built-in instrument, in its range
		Plugin,          // a parameter of the plugin in one of the channel's slots, in its range: slot and parameter
		Strip,           // a parameter of the channel strip's sections (StripParameter), in its range
		Controller,      // a MIDI control change: number and channel; 0 to 1
		PitchBend,       // channel; 0 to 1, 0.5 is no bend
		ChannelPressure, // channel; 0 to 1
		MidiEffect,      // a parameter of the MIDI effect in one of the channel's MIDI slots, in its range: number and
						 // parameter
	};

	struct AutomationTarget
	{
		AutomationTargetType type = AutomationTargetType::Volume;
		uint32_t parameter = 0; // Instrument, Plugin, Strip and MidiEffect: the parameter's id
		uint8_t number = 0;     // Controller: 0-127; MidiEffect: the slot, 0-3 the MIDI inserts, 4-7 the MIDI sends
		uint8_t channel = 0;    // the MIDI types: 0-15
		ObjectId slot = 0;      // Plugin: the slot
	};

	// The shape of the curve from an automation point to the next one.
	enum class AutomationShape : uint8_t
	{
		Step,   // keeps the point's value until the next point
		Linear, // a straight ramp to the next point
		Curve,  // a bent ramp: see AutomationPoint::curve
	};

	struct AutomationPoint
	{
		ObjectId id = 0;
		time::Position position; // in the track's time base, like the track's events
		double value = 0.0;      // in the target's unit
		AutomationShape shape = AutomationShape::Linear;
		double curve = 0.0; // Curve: -1 to 1; above 0 the value moves fast at first, below 0 slowly at first
		// Ends its curve (reference manual p. 881): from here to the next point is a gap, where the parameter has its
		// static value.
		bool terminator = false;
	};

	// The automation of one parameter on a track. Points at the same position make a jump; before the first point the
	// curve has its value, after the last point the last one's, unless the project uses virgin territory
	// (Project::virginTerritory). read: the curve plays; without it, or without points, the parameter keeps its static
	// value (write modes come with #122).
	// NOLINTNEXTLINE(bugprone-exception-escape): MSVC's std::map allocates when moved; only out of memory throws
	struct AutomationLane
	{
		ObjectId id = 0;
		ObjectId track = 0;
		AutomationTarget target;
		bool read = true;
		std::map<ObjectId, AutomationPoint> points;
	};

	// Linked channels (reference manual pp. 457-461, docs/features/link-groups.md): a change to one of them goes to the
	// others, for the settings the group links. A channel is in one link group at most. A VCA fader that controls the
	// group (pp. 515-519) adds its volume to theirs and passes its mute, solo, listen, monitor and record buttons on.
	struct LinkGroup
	{
		ObjectId id = 0;
		std::string name;
		// NOLINTNEXTLINE(readability-redundant-member-init): see Channel::instrumentParameters
		std::vector<ObjectId> channels = {}; // in the order they were linked
		bool volume = true;                  // relative changes of the fader; never with a VCA fader
		bool pan = false;                    // relative changes of the pan
		bool mute = false;                   // the mute, solo and listen buttons
		bool record = false;                 // the record enable and monitor buttons
		bool eq = false;                     // the equalizer's changes (channel.set eq)
		bool dynamics = false;               // the strip's changes: gate, compressor, limiter (channel.set strip)
		bool sends = true;                   // the sends' changes (channel.set sends)
		ObjectId vca = 0;                    // the channel of the VCA track that controls the group, or 0
	};

	inline bool operator==(const LinkGroup& a, const LinkGroup& b)
	{
		return std::tie(
				   a.id, a.name, a.channels, a.volume, a.pan, a.mute, a.record, a.eq, a.dynamics, a.sends, a.vca) ==
			std::tie(b.id, b.name, b.channels, b.volume, b.pan, b.mute, b.record, b.eq, b.dynamics, b.sends, b.vca);
	}

	// A drum sound of a drum map (#129, reference manual pp. 1235-1240, docs/features/drum-editor.md): the sound of
	// the pitch that is its index in the map.
	struct DrumSound
	{
		std::string name;
		midi::NoteValue snap; // the drum editor's grid and length for the sound
		bool mute = false;    // doesn't play
		uint8_t iNote = 0;    // the note played in that becomes this pitch
		uint8_t oNote = 0;    // the note that plays
		int channel = -1;     // 0-15, the MIDI channel it plays on; -1: Any, the note's
		std::string output;   // the MIDI output it plays on; empty: the track's (Default)
	};

	inline bool operator==(const DrumSound& a, const DrumSound& b)
	{
		return std::tie(a.name, a.snap, a.mute, a.iNote, a.oNote, a.channel, a.output) ==
			std::tie(b.name, b.snap, b.mute, b.iNote, b.oNote, b.channel, b.output);
	}

	inline constexpr int g_drumSounds = 128;

	// Settings for each of the 128 pitches that change how the notes of the tracks using the map play and come in,
	// and how the drum editor shows them.
	struct DrumMap
	{
		ObjectId id = 0;
		std::string name;
		std::array<DrumSound, g_drumSounds> sounds;
		std::array<uint8_t, g_drumSounds> order{}; // the drum sound list's order, from the top: each pitch once
	};

	inline bool operator==(const DrumMap& a, const DrumMap& b)
	{
		return std::tie(a.id, a.name, a.sounds, a.order) == std::tie(b.id, b.name, b.sounds, b.order);
	}

	// Articulation maps (#175, reference manual pp. 1262-1292, docs/features/articulation-maps.md).
	enum class ArticulationType : uint8_t
	{
		Attribute, // valid for single notes: notes have them
		Direction, // valid from its position to the next direction of its group: articulation controller events
	};

	struct Articulation
	{
		std::string name; // unique in its map
		ArticulationType type = ArticulationType::Direction;
		int group = 0; // its mutual exclusion group: an index into the map's groups
	};

	inline bool operator==(const Articulation& a, const Articulation& b)
	{
		return std::tie(a.name, a.type, a.group) == std::tie(b.name, b.type, b.group);
	}

	// What an output mapping sends: a note (a key switch), a control change or a program change.
	enum class OutputStatus : uint8_t
	{
		Note,
		Controller,
		Program,
	};

	struct OutputEvent
	{
		OutputStatus status = OutputStatus::Note;
		uint8_t data1 = 0; // the pitch, the controller number or the program
		uint8_t data2 = 0; // the velocity or the controller value
		bool off = false;  // sent when the slot ends instead of when it starts
	};

	inline bool operator==(const OutputEvent& a, const OutputEvent& b)
	{
		return std::tie(a.status, a.data1, a.data2, a.off) == std::tie(b.status, b.data1, b.data2, b.off);
	}

	// A sound slot: the sound that notes with its articulations play, the output events that switch the instrument
	// to it and the MIDI modifiers it applies to the notes.
	struct SoundSlot
	{
		std::string name;
		std::string color;                      // "#rrggbb", or empty
		std::vector<std::string> articulations; // by name, at most one of each group
		int trigger = -1;                       // the remote trigger: a key or a program, 0-127; -1 for none
		std::vector<OutputEvent> outputs;
		int channel = -1;   // 0-15, the channel the notes play on; -1: their own
		int length = 100;   // percent of the notes' length
		int velocity = 100; // percent of the notes' velocity
		int transpose = 0;  // semitones
	};

	inline bool operator==(const SoundSlot& a, const SoundSlot& b)
	{
		return std::tie(a.name, a.color, a.articulations, a.trigger, a.outputs, a.channel, a.length, a.velocity,
				   a.transpose) ==
			std::tie(
				b.name, b.color, b.articulations, b.trigger, b.outputs, b.channel, b.length, b.velocity, b.transpose);
	}

	struct ArticulationMap
	{
		ObjectId id = 0;
		std::string name;
		std::vector<std::string> groups; // the mutual exclusion groups, the first the most important; at least one
		std::vector<Articulation> articulations;
		std::vector<SoundSlot> slots; // the first is the default slot
		bool programTriggers = false; // remote triggers are program changes instead of key switches
		bool latch = true;            // a key switch's slot stays after its key's release
	};

	inline bool operator==(const ArticulationMap& a, const ArticulationMap& b)
	{
		return std::tie(a.id, a.name, a.groups, a.articulations, a.slots, a.programTriggers, a.latch) ==
			std::tie(b.id, b.name, b.groups, b.articulations, b.slots, b.programTriggers, b.latch);
	}

	// Patterns of the pattern editor (#181, reference manual pp. 895-937, docs/features/pattern-editor.md).
	enum class PatternMode : uint8_t
	{
		Drum, // a lane per drum sound
		Mono, // melodic: a lane per pitch, one step at a time
		Poly, // melodic: chords
	};

	enum class PlayDirection : uint8_t
	{
		Forward,
		Backward,
		Alternating, // forward, then backward, and so on
	};

	// A step of a step lane, with its parameters (the parameter lane, p. 929).
	struct PatternStep
	{
		int index = 0;                 // its position on the lane, from 0
		int length = 1;                // in steps: more than 1 ties the steps after it to it
		int velocity = 100;            // 1-127
		int repeats = 1;               // 1-10: it plays as that many sub-steps
		double offset = 0.0;           // -0.5 to 0.5 steps: micro-timing
		double probability = 1.0;      // 0-1: the chance that it plays, each time the lane plays
		double velocityVariance = 0.0; // 0-1: how far playback varies the velocity, up or down
		double gate = 1.0;             // 0.01-1: how much of its length the note lasts
	};

	inline bool operator==(const PatternStep& a, const PatternStep& b)
	{
		return std::tie(
				   a.index, a.length, a.velocity, a.repeats, a.offset, a.probability, a.velocityVariance, a.gate) ==
			std::tie(b.index, b.length, b.velocity, b.repeats, b.offset, b.probability, b.velocityVariance, b.gate);
	}

	// A row of the step display: the steps of one pitch.
	struct PatternLane
	{
		int pitch = 36;
		std::string name; // drum mode: the lane's name
		bool mute = false;
		bool solo = false;
		int length = 0;                                   // steps, 1-128; 0: the pattern's
		std::optional<midi::NoteValue> resolution;        // none: the pattern's
		PlayDirection direction = PlayDirection::Forward; // forward: the pattern's
		double swing = 0.0;                               // -1 to 1: added to the pattern's
		double offset = 0.0;                              // -1 to 1 steps: the whole lane earlier or later
		std::vector<PatternStep> steps;                   // by index, at most one starting at each
	};

	inline bool operator==(const PatternLane& a, const PatternLane& b)
	{
		return std::tie(
				   a.pitch, a.name, a.mute, a.solo, a.length, a.resolution, a.direction, a.swing, a.offset, a.steps) ==
			std::tie(b.pitch, b.name, b.mute, b.solo, b.length, b.resolution, b.direction, b.swing, b.offset, b.steps);
	}

	// A pattern of a track, which its pattern events (MidiPart::pattern) play over and over.
	struct Pattern
	{
		ObjectId id = 0;
		ObjectId track = 0; // the track whose pattern management lists it
		std::string name;
		PatternMode mode = PatternMode::Drum;
		int length = 16;            // Number of Steps, 1-128
		midi::NoteValue resolution; // Step Resolution, up to 1/64
		PlayDirection direction = PlayDirection::Forward;
		double swing = 0.0; // -1 to 1: how far every second step moves, in half steps
		std::vector<PatternLane> lanes;
	};

	inline bool operator==(const Pattern& a, const Pattern& b)
	{
		return std::tie(a.id, a.track, a.name, a.mode, a.length, a.resolution, a.direction, a.swing, a.lanes) ==
			std::tie(b.id, b.track, b.name, b.mode, b.length, b.resolution, b.direction, b.swing, b.lanes);
	}

	// A color of the project's color set (reference manual pp. 115-118), which colors are picked from.
	struct ProjectColor
	{
		std::string name;
		std::string color; // "#rrggbb"
	};

	// The color set of new projects.
	std::vector<ProjectColor> factoryColors();

	// Whether text is a color as "#rrggbb".
	bool isColor(const std::string& text);

	// The channel types a mixer's filter shows or hides (reference manual p. 454): input and output busses, and the
	// channels of audio, instrument, MIDI, group, effect and VCA tracks; folders' group channels are group channels.
	inline constexpr std::array<const char*, 8> g_mixerChannelTypes = {
		"input", "audio", "instrument", "midi", "group", "effect", "vca", "output"};

	// What a mixer hides (#106, docs/features/mixer-window.md): channels and busses by id, and channel types.
	struct MixerView
	{
		std::vector<ObjectId> hidden;
		std::vector<std::string> hiddenTypes; // from g_mixerChannelTypes
	};

	// A track visibility configuration (#166, reference manual p. 58): a name for a set of hidden tracks.
	struct VisibilityConfiguration
	{
		std::string name;
		std::vector<ObjectId> hidden;
	};

	// What the project window's track list hides (#166, docs/features/track-visibility.md, reference manual pp. 80-82).
	struct TrackVisibility
	{
		std::vector<ObjectId> hidden; // tracks, sorted
		// The mixer window whose channel visibility follows the tracks' (window1 to window4), or empty; the lower
		// zone's mixer always does.
		std::string sync;
		std::vector<VisibilityConfiguration> configurations;
		std::string configuration; // the active configuration's name, or empty
	};

	// The track list's track heights and folded folders (#491, docs/features/track-list.md, reference manual pp. 217,
	// 225), saved with the project.
	struct TrackLayout
	{
		std::map<ObjectId, double> heights; // in dp, before vertical zoom; tracks without one have the default
		std::vector<ObjectId> folded;       // folder tracks, sorted
	};

	// A workspace (#163, docs/features/workspaces.md, reference manual pp. 1576-1578): a name for a window layout, the
	// user setting ui.layout as JSON text, with the windows that are open and where they are.
	struct Workspace
	{
		std::string name;
		std::string layout;
	};

	// The project's workspaces and the workspace the project was last shown in.
	struct ProjectWorkspaces
	{
		std::vector<Workspace> workspaces; // the project workspaces, in the user's order
		std::string activeType;            // global or project; empty for none
		std::string active;                // the active workspace's name
	};

	// The record file types and bit depths of the project setup (reference manual pp. 129-130), as the engine API
	// names them. Recording (#96) writes its files in them.
	inline constexpr std::array<const char*, 4> g_recordFileTypes = {"wav", "broadcastWav", "aiff", "flac"};
	inline constexpr std::array<const char*, 5> g_recordBitDepths = {"int16", "int24", "int32", "float32", "float64"};

	// The project setup's settings that only clients and recording read (#78, docs/features/project-setup.md); the
	// sample rate, pan law and maximum volume are the project's own.
	struct ProjectSetup
	{
		std::string recordFileType = "wav";   // one of g_recordFileTypes
		std::string recordBitDepth = "int24"; // one of g_recordBitDepths
		time::FrameRate frameRate = time::g_fps25;
		double startTime = 0.0;               // the time shown at the project start, in seconds
		double length = 600.0;                // in seconds
		int barOffset = 0;                    // added to the bar numbers shown
		std::string timeFormat = "barsBeats"; // the primary time format: one of g_rulerTimeFormats
	};

	// The sample rates a project can have, in Hz: from the lowest common one to the highest that export writes. Far
	// outside them the conversions between ticks and samples leave a time::Ticks or a time::Samples.
	inline constexpr double g_lowestSampleRate = 8000.0;
	inline constexpr double g_highestSampleRate = 768000.0;

	// NOLINTNEXTLINE(bugprone-exception-escape): MSVC's std::map allocates when moved; only out of memory throws
	struct Project
	{
		std::string name = "Untitled";
		double sampleRate = 48000.0;
		// The tempo the project plays: the tempo track's tempo events while it is active (tempo track mode), the fixed
		// tempo otherwise (#155, reference manual p. 1446).
		time::TempoMap tempo{120.0};
		bool tempoTrackActive = false;
		// The tempo of the mode not in use: the tempo track's events in fixed tempo mode, the fixed tempo (one point)
		// in tempo track mode. Switching modes swaps it with tempo; tempoTrack() and fixedTempo() read either.
		time::TempoMap otherTempo{120.0};
		time::SignatureMap signatures;
		AutoFades autoFades; // for audio tracks without settings of their own
		PanLaw panLaw = PanLaw::Minus3;
		double maxVolume = 6.0;                             // the top of the faders, in dB: 6 or 12
		std::vector<ProjectColor> colors = factoryColors(); // at least one
		ProjectSetup setup;
		ObjectId nextId = 1;

		std::vector<Bus> busses;      // in the order they are shown
		std::vector<ObjectId> tracks; // top-level tracks, from top to bottom
		std::map<ObjectId, Track> trackById;
		std::map<ObjectId, Channel> channels;
		std::map<ObjectId, Slot> slots;
		std::map<ObjectId, MediaFile> media;
		std::map<ObjectId, Clip> clips;
		std::map<ObjectId, PoolFolder> poolFolders;
		std::map<ObjectId, AudioEvent> audioEvents;
		std::map<ObjectId, AudioPart> audioParts;
		std::map<ObjectId, MidiPart> midiParts;
		std::map<ObjectId, Marker> markers;
		ObjectId activeMarkerTrack = 0; // the marker track that marker commands use by default, or 0
		std::map<ObjectId, AutomationLane> automation;
		std::map<ObjectId, LinkGroup> linkGroups;
		Arranger arranger;
		// Arranger mode, its jump mode and the active chain (0: the first one): settings saved with the project, not
		// part of undo.
		bool arrangerMode = false;
		ArrangerJump arrangerJump = ArrangerJump::None;
		ObjectId arrangerChain = 0;
		int rootKey = -1;                     // the Project Root Key (#168): -1 for none, 0 C to 11 B
		TransposeTrack transpose;             // the transpose track's events and controls; reset when the track goes
		ChordTrack chords;                    // the chord track's (#173); reset when the track goes
		ChordPads chordPads;                  // the chord pads (#174)
		std::map<ObjectId, DrumMap> drumMaps; // a new project has the GM Map (#129)
		std::map<ObjectId, ArticulationMap> articulationMaps; // (#175)
		std::map<ObjectId, Pattern> patterns;                 // the pattern editor's (#181)
		// The project input transformer's modules, which the MIDI and instrument tracks whose input transformer is in
		// project mode share (#178).
		midi::InputTransformerModules inputTransformer;
		// Virgin territory instead of initial values (reference manual p. 881): automation lanes have gaps before their
		// first and after their last point too, where their parameters have their static values. A setting saved with
		// the project, not part of undo.
		bool virginTerritory = false;

		// The quantize settings that apply when none are given, and whether recording quantizes (#132). Settings,
		// not part of undo.
		midi::QuantizeSettings quantize;
		bool autoQuantize = false;
		// The preset last chosen with quantize.set, saved or renamed while the quantize settings are its own, which
		// quantize.get reports among presets with equal settings (#770); empty once the settings change. A setting,
		// not part of undo.
		std::string quantizePreset;
		// Snap and grid (#87): settings, not part of undo.
		SnapSettings snap;
		// The color last given to events or parts, for editing.autoTrackColor's lastApplied: not part of undo or the
		// file.
		std::string lastAppliedColor;
		// What each mixer hides (#106), by mixer: lower, window1 to window4. Settings, not part of undo.
		std::map<std::string, MixerView> mixers;
		// What the track list hides (#166). A setting, not part of undo.
		TrackVisibility trackVisibility;
		// The track heights and folded folders (#491). Settings, not part of undo.
		TrackLayout trackLayout;
		// The project workspaces and the active workspace (#163). Settings, not part of undo.
		ProjectWorkspaces workspaces;

		// The metronome and count-in (#101). Settings, not part of undo.
		metronome::MetronomeSettings metronome;

		ObjectId newId() { return nextId++; }

		// Every track id from top to bottom, folders before the tracks they contain.
		std::vector<ObjectId> trackOrder() const;

		// The tempo track's tempo events, in either tempo mode.
		const time::TempoMap& tempoTrack() const { return tempoTrackActive ? tempo : otherTempo; }
		time::TempoMap& tempoTrack() { return tempoTrackActive ? tempo : otherTempo; }
		// The tempo of fixed tempo mode, in either tempo mode.
		double fixedTempo() const { return (tempoTrackActive ? otherTempo : tempo).points().front().bpm; }
	};

	// An empty project with a stereo input and a stereo output bus.
	Project newProject();

	// The main mix, or 0 if the project has no output bus.
	ObjectId mainMix(const Project& project);

	// How many ticks from its start the longest project lasts at its tempo: g_maxPosition samples long. At 999 BPM and
	// the lowest sample rate that is about 10^18 ticks, which still fits a time::Ticks. Edits that compute positions,
	// such as the rule editors', stop there.
	time::Ticks longestProjectTicks(const Project& project);

	// Makes the first top-level output bus the main mix if no bus is.
	void keepMainMix(Project& project);

	// The link group a channel is in, or none.
	const LinkGroup* linkGroupOf(const Project& project, ObjectId channel);

	// The link group a VCA fader's channel controls, or none.
	const LinkGroup* controlledBy(const Project& project, ObjectId vca);

	// The VCA faders that control a channel, nearest first: the one of its link group, the one of that VCA fader's
	// group, and so on (nested VCA faders, reference manual p. 518). Stops before a VCA fader it has met, so a loop
	// ends it.
	std::vector<ObjectId> vcaChain(const Project& project, ObjectId channel);

	// Gives a part's notes and controllers new ids.
	void renumber(Project& project, MidiPart& part);

	// Takes a part out of its shared copies before its contents change for its own sake: its events get new ids
	// (renumber), so that it no longer shares them.
	void detach(Project& project, MidiPart& part);

	// What is wrong with a bus of the project, or empty: its number of ports, a child bus's parent and channels.
	std::string busProblem(const Project& project, const Bus& bus);

	// Whether a marker is long enough: a cycle marker ends at least one sample after its position.
	bool hasMinimumLength(const Project& project, const Marker& marker);

	// Moves the end of a cycle marker that isn't long enough, e.g. after a new time base or sample rate, to one sample
	// after its position. Returns whether it moved it.
	bool keepMinimumLength(const Project& project, Marker& marker);

	// Moves the end of an arranger event that isn't at least one sample long to one sample after its position, as
	// keepMinimumLength does for cycle markers. Returns whether it moved it.
	bool keepMinimumLength(const Project& project, ArrangerEvent& event);

	// The project's active arranger chain: arrangerChain, or the first chain if that is gone; none without chains.
	const ArrangerChain* activeArrangerChain(const Project& project);

	// Root key names (#168): "" for none (-1), then "C", "C#" and so on to "B" (0 to 11).
	const std::vector<std::string>& rootKeyNames();
	std::string rootKeyName(int key);
	std::optional<int> rootKeyFromName(const std::string& name);

	// The step from a part's root key to the project root key, the nearer way (-5 to 6 semitones): 0 for an
	// Independent part and unless both have a root key. project.get gives it as each part's rootKeyTranspose.
	int rootKeyTranspose(const Project& project, const MidiPart& part);

	// The semitones that global transposition adds to a part's notes at a position (project samples): nothing for an
	// Independent part; otherwise the step from the part's root key to the project root key, the nearer way (-5 to
	// 6; nothing unless both have one), plus the value of the transpose event playing there unless the transpose track
	// is muted, kept within 7 semitones either way with Keep Transpose in Octave Range. The part's own transpose adds
	// to that.
	int globalTranspose(const Project& project, const MidiPart& part, time::Samples at);

	// Whether folder is the pool's Audio folder (0) or a pool folder whose parents lead there without passing
	// excluded: false for a folder that isn't there, one in a loop, and excluded or a folder inside it.
	bool poolFolderOutside(const Project& project, ObjectId folder, ObjectId excluded);

	// Whether a channel can be a send's destination: a group or effect channel, or a folder's group channel.
	bool isSendDestination(const Project& project, ObjectId channel);

	// The MIDI insert (number 0 to 3) or send (4 to 7) slot of a channel, as AutomationTarget::number counts them.
	inline const midi::EffectSlot& midiEffectSlot(const Channel& channel, uint8_t number)
	{
		const midi::EffectRack& rack = number < midi::g_effectSlots ? channel.midiInserts : channel.midiSends;
		return rack.slots[number % midi::g_effectSlots];
	}

	// Whether a MIDI send from a channel to another one (midi::EffectSlot::output) would come back to it: whether
	// channel is to or is reached from it through the outputs of MIDI sends.
	bool midiSendLoops(const Project& project, ObjectId channel, ObjectId to);

	// Whether audio from a channel to another one, through an output or a send, would come back to it: whether
	// channel is to or is reached from it through outputs and the destinations of sends, on or off. Routing never
	// closes a loop, so the processing graph has no feedback.
	bool routingLoops(const Project& project, ObjectId channel, ObjectId to);

	// Equality compares every member; the undo history uses it to find what a command changed.
	inline bool operator==(const ProjectSetup& a, const ProjectSetup& b)
	{
		return std::tie(
				   a.recordFileType, a.recordBitDepth, a.frameRate, a.startTime, a.length, a.barOffset, a.timeFormat) ==
			std::tie(b.recordFileType, b.recordBitDepth, b.frameRate, b.startTime, b.length, b.barOffset, b.timeFormat);
	}

	inline bool operator==(const Bus& a, const Bus& b)
	{
		return std::tie(a.id, a.name, a.direction, a.format, a.ports, a.parent, a.parentChannels, a.mainMix) ==
			std::tie(b.id, b.name, b.direction, b.format, b.ports, b.parent, b.parentChannels, b.mainMix);
	}

	inline bool operator==(const Channel& a, const Channel& b)
	{
		return std::tie(a.id, a.track, a.format, a.input, a.output, a.instrument, a.volume, a.pan, a.panner, a.panLeft,
				   a.panRight, a.panBypass, a.midiVolume, a.midiPan, a.program, a.bank, a.device, a.modifiers,
				   a.midiInserts, a.midiSends, a.inputTransformer, a.instrumentParameters, a.sample, a.monitor,
				   a.recordEnable, a.midiInput, a.mute, a.solo, a.soloDefeat, a.listen, a.slots, a.notes, a.sections,
				   a.sends) ==
			std::tie(b.id, b.track, b.format, b.input, b.output, b.instrument, b.volume, b.pan, b.panner, b.panLeft,
				b.panRight, b.panBypass, b.midiVolume, b.midiPan, b.program, b.bank, b.device, b.modifiers,
				b.midiInserts, b.midiSends, b.inputTransformer, b.instrumentParameters, b.sample, b.monitor,
				b.recordEnable, b.midiInput, b.mute, b.solo, b.soloDefeat, b.listen, b.slots, b.notes, b.sections,
				b.sends);
	}

	inline bool operator==(const Slot& a, const Slot& b)
	{
		const bool sameState = a.state == b.state || (a.state && b.state && *a.state == *b.state);
		return sameState &&
			std::tie(a.id, a.channel, a.plugin, a.name, a.bypass, a.active, a.parameters) ==
			std::tie(b.id, b.channel, b.plugin, b.name, b.bypass, b.active, b.parameters);
	}

	inline bool operator==(const MediaFile& a, const MediaFile& b)
	{
		return std::tie(a.id, a.path, a.sampleRate, a.channels, a.length) ==
			std::tie(b.id, b.path, b.sampleRate, b.channels, b.length);
	}

	inline bool operator==(const Fade& a, const Fade& b)
	{
		return std::tie(a.length, a.shape) == std::tie(b.length, b.shape);
	}

	inline bool operator==(const Crossfade& a, const Crossfade& b)
	{
		return std::tie(a.from, a.mode, a.shape, a.outShape, a.length, a.outLength) ==
			std::tie(b.from, b.mode, b.shape, b.outShape, b.length, b.outLength);
	}

	inline bool operator==(const AutoFades& a, const AutoFades& b)
	{
		return std::tie(a.fadeIn, a.fadeOut, a.crossfades, a.length, a.fadeInShape, a.fadeOutShape, a.crossfadeMode,
				   a.crossfadeShape) ==
			std::tie(b.fadeIn, b.fadeOut, b.crossfades, b.length, b.fadeInShape, b.fadeOutShape, b.crossfadeMode,
				b.crossfadeShape);
	}

	inline bool operator==(const ClipRegion& a, const ClipRegion& b)
	{
		return std::tie(a.id, a.name, a.start, a.end) == std::tie(b.id, b.name, b.start, b.end);
	}

	inline bool operator==(const Hitpoint& a, const Hitpoint& b)
	{
		return std::tie(a.position, a.peak, a.intensity, a.locked, a.disabled) ==
			std::tie(b.position, b.peak, b.intensity, b.locked, b.disabled);
	}

	inline bool operator==(const HitpointFilter& a, const HitpointFilter& b)
	{
		return std::tie(a.threshold, a.intensity, a.minimumLength) ==
			std::tie(b.threshold, b.intensity, b.minimumLength);
	}

	inline bool operator==(const BeatEdit& a, const BeatEdit& b)
	{
		return a.beat == b.beat && a.frame == b.frame;
	}

	inline bool operator==(const WarpMarker& a, const WarpMarker& b)
	{
		return a.position == b.position && a.target == b.target;
	}

	inline bool operator==(const WarpQuantizeOrigin& a, const WarpQuantizeOrigin& b)
	{
		return a.original == b.original && a.quantized == b.quantized;
	}

	inline bool operator==(const ClipWarp& a, const ClipWarp& b)
	{
		return std::tie(
				   a.musical, a.tempo, a.beatsPerBar, a.gridStart, a.beats, a.markers, a.preset, a.quantizeOrigin) ==
			std::tie(b.musical, b.tempo, b.beatsPerBar, b.gridStart, b.beats, b.markers, b.preset, b.quantizeOrigin);
	}

	inline bool operator==(const Clip& a, const Clip& b)
	{
		return std::tie(a.id, a.name, a.media, a.fadeIn, a.fadeOut, a.folder, a.regions, a.hitpoints, a.hitpointFilter,
				   a.warp) ==
			std::tie(
				b.id, b.name, b.media, b.fadeIn, b.fadeOut, b.folder, b.regions, b.hitpoints, b.hitpointFilter, b.warp);
	}

	inline bool operator==(const PoolFolder& a, const PoolFolder& b)
	{
		return std::tie(a.id, a.name, a.parent) == std::tie(b.id, b.name, b.parent);
	}

	inline bool operator==(const VolumePoint& a, const VolumePoint& b)
	{
		return std::tie(a.position, a.volume, a.curve) == std::tie(b.position, b.volume, b.curve);
	}

	inline bool operator==(const EventQuantizeOrigin& a, const EventQuantizeOrigin& b)
	{
		return a.original == b.original && a.quantized == b.quantized;
	}

	inline bool operator==(const AudioEvent& a, const AudioEvent& b)
	{
		return std::tie(a.id, a.track, a.position, a.clip, a.offset, a.length, a.volume, a.mute, a.lock, a.fadeIn,
				   a.fadeOut, a.crossfade, a.snapPoint, a.color, a.part, a.lane, a.volumeCurve, a.front,
				   a.quantizeOrigin) ==
			std::tie(b.id, b.track, b.position, b.clip, b.offset, b.length, b.volume, b.mute, b.lock, b.fadeIn,
				b.fadeOut, b.crossfade, b.snapPoint, b.color, b.part, b.lane, b.volumeCurve, b.front, b.quantizeOrigin);
	}

	inline bool operator==(const AudioPart& a, const AudioPart& b)
	{
		return std::tie(a.id, a.track, a.name, a.position, a.length, a.mute, a.lock, a.color, a.lane) ==
			std::tie(b.id, b.track, b.name, b.position, b.length, b.mute, b.lock, b.color, b.lane);
	}

	inline bool operator==(const NoteTiming& a, const NoteTiming& b)
	{
		return std::tie(a.position, a.length, a.velocity) == std::tie(b.position, b.length, b.velocity);
	}

	inline bool operator==(const QuantizeOrigin& a, const QuantizeOrigin& b)
	{
		return a.original == b.original && a.quantized == b.quantized;
	}

	inline bool operator==(const Note& a, const Note& b)
	{
		return std::tie(a.id, a.position, a.length, a.pitch, a.velocity, a.channel, a.releaseVelocity, a.origin,
				   a.probability, a.velocityVariance, a.articulations) ==
			std::tie(b.id, b.position, b.length, b.pitch, b.velocity, b.channel, b.releaseVelocity, b.origin,
				b.probability, b.velocityVariance, b.articulations);
	}

	inline bool operator==(const Controller& a, const Controller& b)
	{
		return std::tie(a.id, a.position, a.type, a.number, a.value, a.channel, a.data, a.articulation) ==
			std::tie(b.id, b.position, b.type, b.number, b.value, b.channel, b.data, b.articulation);
	}

	// A MIDI part's own properties, without its notes and controllers.
	inline bool sameProperties(const MidiPart& a, const MidiPart& b)
	{
		return std::tie(a.id, a.track, a.name, a.position, a.length, a.mute, a.lock, a.shared, a.color, a.lane,
				   a.transpose, a.velocity, a.rootKey, a.independent, a.pattern, a.patternOffset) ==
			std::tie(b.id, b.track, b.name, b.position, b.length, b.mute, b.lock, b.shared, b.color, b.lane,
				b.transpose, b.velocity, b.rootKey, b.independent, b.pattern, b.patternOffset);
	}

	inline bool operator==(const ProjectColor& a, const ProjectColor& b)
	{
		return std::tie(a.name, a.color) == std::tie(b.name, b.color);
	}

	inline bool operator==(const Marker& a, const Marker& b)
	{
		return std::tie(a.id, a.track, a.type, a.number, a.position, a.end, a.name, a.color) ==
			std::tie(b.id, b.track, b.type, b.number, b.position, b.end, b.name, b.color);
	}

	inline bool operator==(const ArrangerEvent& a, const ArrangerEvent& b)
	{
		return std::tie(a.id, a.name, a.position, a.end) == std::tie(b.id, b.name, b.position, b.end);
	}

	inline bool operator==(const ArrangerStep& a, const ArrangerStep& b)
	{
		return std::tie(a.event, a.repeats, a.mode) == std::tie(b.event, b.repeats, b.mode);
	}

	inline bool operator==(const ArrangerChain& a, const ArrangerChain& b)
	{
		return std::tie(a.id, a.name, a.steps) == std::tie(b.id, b.name, b.steps);
	}

	inline bool operator==(const Arranger& a, const Arranger& b)
	{
		return std::tie(a.events, a.chains) == std::tie(b.events, b.chains);
	}

	inline bool operator==(const TransposeEvent& a, const TransposeEvent& b)
	{
		return std::tie(a.id, a.position, a.value) == std::tie(b.id, b.position, b.value);
	}

	inline bool operator==(const TransposeTrack& a, const TransposeTrack& b)
	{
		return std::tie(a.events, a.mute, a.keepInOctave, a.lock) == std::tie(b.events, b.mute, b.keepInOctave, b.lock);
	}

	inline bool operator==(const ChordEvent& a, const ChordEvent& b)
	{
		return std::tie(a.id, a.position, a.text, a.audioEvent, a.detected) ==
			std::tie(b.id, b.position, b.text, b.audioEvent, b.detected);
	}

	inline bool operator==(const ChordSource& a, const ChordSource& b)
	{
		return std::tie(a.position, a.chord) == std::tie(b.position, b.chord);
	}

	inline bool operator==(const ChordFollow& a, const ChordFollow& b)
	{
		return std::tie(a.mode, a.source, a.voicings, a.voice, a.master, a.liveInput) ==
			std::tie(b.mode, b.source, b.voicings, b.voice, b.master, b.liveInput);
	}

	inline bool operator==(const ChordVoicing& a, const ChordVoicing& b)
	{
		return std::tie(
				   a.library, a.style, a.addRoot, a.duplicateRoot, a.fattenUp, a.lowestRoot, a.lowest, a.highest) ==
			std::tie(b.library, b.style, b.addRoot, b.duplicateRoot, b.fattenUp, b.lowestRoot, b.lowest, b.highest);
	}

	inline bool operator==(const ChordTrack& a, const ChordTrack& b)
	{
		return std::tie(a.events, a.scales, a.octaveOffset, a.adaptiveVoicings, a.automaticScales, a.follow, a.voicing,
				   a.shiftChords, a.mute, a.showScales, a.recordEnable, a.audition, a.acousticFeedback) ==
			std::tie(b.events, b.scales, b.octaveOffset, b.adaptiveVoicings, b.automaticScales, b.follow, b.voicing,
				b.shiftChords, b.mute, b.showScales, b.recordEnable, b.audition, b.acousticFeedback);
	}

	inline bool operator==(const ChordPad& a, const ChordPad& b)
	{
		return std::tie(a.chord, a.voicing, a.adaptive, a.locked) == std::tie(b.chord, b.voicing, b.adaptive, b.locked);
	}

	inline bool operator==(const ChordPads& a, const ChordPads& b)
	{
		return std::tie(a.pads, a.rootKey, a.reference, a.octaveOffset, a.velocity, a.remote, a.remoteStart,
				   a.remoteEnd, a.preset, a.stepInput, a.moveInsert, a.stepPosition) ==
			std::tie(b.pads, b.rootKey, b.reference, b.octaveOffset, b.velocity, b.remote, b.remoteStart, b.remoteEnd,
				b.preset, b.stepInput, b.moveInsert, b.stepPosition);
	}

	inline bool operator==(const AutomationTarget& a, const AutomationTarget& b)
	{
		return std::tie(a.type, a.parameter, a.slot, a.number, a.channel) ==
			std::tie(b.type, b.parameter, b.slot, b.number, b.channel);
	}

	inline bool operator==(const AutomationPoint& a, const AutomationPoint& b)
	{
		return std::tie(a.id, a.position, a.value, a.shape, a.curve, a.terminator) ==
			std::tie(b.id, b.position, b.value, b.shape, b.curve, b.terminator);
	}

	// An automation lane's own properties, without its points.
	inline bool sameProperties(const AutomationLane& a, const AutomationLane& b)
	{
		return std::tie(a.id, a.track, a.target, a.read) == std::tie(b.id, b.track, b.target, b.read);
	}

	inline bool operator==(const Track& a, const Track& b)
	{
		return std::tie(a.id, a.type, a.name, a.color, a.timeBase, a.parent, a.channel, a.children, a.autoFades,
				   a.timeFormat, a.enabled, a.picture, a.timeOffset, a.timeOffsetOn, a.frozen, a.drumMap,
				   a.articulationMap, a.versions, a.activeVersion) ==
			std::tie(b.id, b.type, b.name, b.color, b.timeBase, b.parent, b.channel, b.children, b.autoFades,
				b.timeFormat, b.enabled, b.picture, b.timeOffset, b.timeOffsetOn, b.frozen, b.drumMap,
				b.articulationMap, b.versions, b.activeVersion);
	}
	// Where an audio event plays, in the project's samples (fadeedits.cpp).
	struct EventSpan
	{
		time::Samples start = 0;
		time::Samples end = 0;
		double ratio = 1.0; // project samples per clip frame
	};

	EventSpan eventSpan(const Project& project, const AudioEvent& event);

	// The sample frames an event's fades may take together: its length, or on a warped event the frames at the
	// clip's sample rate that it plays for, which is where its own fades play (playback.cpp, warpEvent).
	time::Samples fadeRoom(const Project& project, const AudioEvent& event);

	// How many project samples after an audio event's start the clip frames frames after its offset play, and the
	// other way round: through the clip's warp when it plays warped (warp.h), else at the ratio of the sample rates.
	double eventSamples(const Project& project, const AudioEvent& event, double frames);
	double eventFrames(const Project& project, const AudioEvent& event, double samples);

	// Whether an event's crossfade plays: the event it comes from is on the same track, neither is muted, and this
	// one starts inside it and ends after it. Otherwise the crossfade rests, and both play their own fades at those
	// ends. Playback (through the render snapshot) and the DAWproject export decide by this.
	bool crossfadeApplies(const Project& project, const AudioEvent& to);
}
