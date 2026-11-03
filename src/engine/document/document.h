// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/channelformat.h"
#include "engine/document/snap.h"
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
		Folder, // contains other tracks; has a channel only if its group channel is on
		Group,  // sums the channels routed to it; holds no events
		Effect, // receives sends; holds no events
		Marker, // holds markers; has no channel and stays at the top level
		Ruler,  // shows a ruler in a time format of its own; has no channel
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
		// The values set for the instrument's parameters (parameter.set), by parameter id; the others are at their
		// defaults. A new instrument starts with none.
		// The initializer lets Channel{...} leave it out without GCC's and Clang's -Wmissing-field-initializers, like
		// the members above with default values.
		// NOLINTNEXTLINE(readability-redundant-member-init): see above
		std::map<uint32_t, double> instrumentParameters = {};
		// Audio, MIDI and instrument tracks: the monitor and record enable buttons
		// (docs/features/latency-and-monitoring.md, docs/features/midi-recording.md).
		bool monitor = false;
		bool recordEnable = false;
		// MIDI and instrument tracks: where their MIDI input comes from, "all" (all MIDI inputs) or "none".
		std::string midiInput = "all";
		// The mute and solo buttons (docs/features/track-list.md): a muted channel is silent, unless mute automation
		// plays (docs/features/automation.md); while any channel is soloed, only soloed ones, where they play through
		// and what plays into them are heard.
		bool mute = false;
		bool solo = false;
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

	// What audio events play: for now, a whole media file. Its fades apply to every event that plays it, at the
	// file's start and end.
	struct Clip
	{
		ObjectId id = 0;
		std::string name;
		ObjectId media = 0;
		Fade fadeIn;
		Fade fadeOut;
		ObjectId folder = 0; // the pool folder it is in; 0: the pool's Audio folder itself
	};

	// A folder of the pool that clips are organized in (#93, reference manual p. 757). It only organizes the pool:
	// the files stay where they are.
	struct PoolFolder
	{
		ObjectId id = 0;
		std::string name;
		ObjectId parent = 0; // 0: in the pool's Audio folder
	};

	// Plays a section of a clip on an audio track.
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
		// The audio part it is in, on the same track, or 0 for none. It keeps its position on the track: it plays
		// only where the part is, and moves, splits and goes with the part.
		ObjectId part = 0;
	};

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
		std::map<ObjectId, Note> notes;
		std::map<ObjectId, Controller> controllers;
	};

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
	};

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
		Controller,      // a MIDI control change: number and channel; 0 to 1
		PitchBend,       // channel; 0 to 1, 0.5 is no bend
		ChannelPressure, // channel; 0 to 1
	};

	struct AutomationTarget
	{
		AutomationTargetType type = AutomationTargetType::Volume;
		uint32_t parameter = 0; // Instrument and Plugin: the parameter's id
		uint8_t number = 0;     // Controller: 0-127
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
	};

	// The automation of one parameter on a track. Points at the same position make a jump; before the first point the
	// curve has its value, after the last point the last one's. read: the curve plays; without it, or without points,
	// the parameter keeps its static value (write modes come with #122).
	// NOLINTNEXTLINE(bugprone-exception-escape): MSVC's std::map allocates when moved; only out of memory throws
	struct AutomationLane
	{
		ObjectId id = 0;
		ObjectId track = 0;
		AutomationTarget target;
		bool read = true;
		std::map<ObjectId, AutomationPoint> points;
	};

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
	// channels of audio, instrument, MIDI, group and effect tracks; folders' group channels are group channels.
	inline constexpr std::array<const char*, 7> g_mixerChannelTypes = {
		"input", "audio", "instrument", "midi", "group", "effect", "output"};

	// What a mixer hides (#106, docs/features/mixer-window.md): channels and busses by id, and channel types.
	struct MixerView
	{
		std::vector<ObjectId> hidden;
		std::vector<std::string> hiddenTypes; // from g_mixerChannelTypes
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

	// NOLINTNEXTLINE(bugprone-exception-escape): MSVC's std::map allocates when moved; only out of memory throws
	struct Project
	{
		std::string name = "Untitled";
		double sampleRate = 48000.0;
		time::TempoMap tempo{120.0};
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

		// The quantize settings that apply when none are given, and whether recording quantizes (#132). Settings,
		// not part of undo.
		midi::QuantizeSettings quantize;
		bool autoQuantize = false;
		// Snap and grid (#87): settings, not part of undo.
		SnapSettings snap;
		// The color last given to events or parts, for editing.autoTrackColor's lastApplied: not part of undo or the
		// file.
		std::string lastAppliedColor;
		// What each mixer hides (#106), by mixer: lower, window1 to window4. Settings, not part of undo.
		std::map<std::string, MixerView> mixers;

		ObjectId newId() { return nextId++; }

		// Every track id from top to bottom, folders before the tracks they contain.
		std::vector<ObjectId> trackOrder() const;
	};

	// An empty project with a stereo input and a stereo output bus.
	Project newProject();

	// The main mix, or 0 if the project has no output bus.
	ObjectId mainMix(const Project& project);

	// Makes the first top-level output bus the main mix if no bus is.
	void keepMainMix(Project& project);

	// Gives a part's notes and controllers new ids.
	void renumber(Project& project, MidiPart& part);

	// What is wrong with a bus of the project, or empty: its number of ports, a child bus's parent and channels.
	std::string busProblem(const Project& project, const Bus& bus);

	// Whether a marker is long enough: a cycle marker ends at least one sample after its position.
	bool hasMinimumLength(const Project& project, const Marker& marker);

	// Moves the end of a cycle marker that isn't long enough, e.g. after a new time base or sample rate, to one sample
	// after its position. Returns whether it moved it.
	bool keepMinimumLength(const Project& project, Marker& marker);

	// Whether folder is the pool's Audio folder (0) or a pool folder whose parents lead there without passing
	// excluded: false for a folder that isn't there, one in a loop, and excluded or a folder inside it.
	bool poolFolderOutside(const Project& project, ObjectId folder, ObjectId excluded);

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
				   a.panRight, a.panBypass, a.midiVolume, a.midiPan, a.instrumentParameters, a.monitor, a.recordEnable,
				   a.midiInput, a.mute, a.solo, a.slots, a.notes, a.sections) ==
			std::tie(b.id, b.track, b.format, b.input, b.output, b.instrument, b.volume, b.pan, b.panner, b.panLeft,
				b.panRight, b.panBypass, b.midiVolume, b.midiPan, b.instrumentParameters, b.monitor, b.recordEnable,
				b.midiInput, b.mute, b.solo, b.slots, b.notes, b.sections);
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

	inline bool operator==(const Clip& a, const Clip& b)
	{
		return std::tie(a.id, a.name, a.media, a.fadeIn, a.fadeOut, a.folder) ==
			std::tie(b.id, b.name, b.media, b.fadeIn, b.fadeOut, b.folder);
	}

	inline bool operator==(const PoolFolder& a, const PoolFolder& b)
	{
		return std::tie(a.id, a.name, a.parent) == std::tie(b.id, b.name, b.parent);
	}

	inline bool operator==(const AudioEvent& a, const AudioEvent& b)
	{
		return std::tie(a.id, a.track, a.position, a.clip, a.offset, a.length, a.volume, a.mute, a.lock, a.fadeIn,
				   a.fadeOut, a.crossfade, a.snapPoint, a.color, a.part) ==
			std::tie(b.id, b.track, b.position, b.clip, b.offset, b.length, b.volume, b.mute, b.lock, b.fadeIn,
				b.fadeOut, b.crossfade, b.snapPoint, b.color, b.part);
	}

	inline bool operator==(const AudioPart& a, const AudioPart& b)
	{
		return std::tie(a.id, a.track, a.name, a.position, a.length, a.mute, a.lock, a.color) ==
			std::tie(b.id, b.track, b.name, b.position, b.length, b.mute, b.lock, b.color);
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
		return std::tie(a.id, a.position, a.length, a.pitch, a.velocity, a.channel, a.releaseVelocity, a.origin) ==
			std::tie(b.id, b.position, b.length, b.pitch, b.velocity, b.channel, b.releaseVelocity, b.origin);
	}

	inline bool operator==(const Controller& a, const Controller& b)
	{
		return std::tie(a.id, a.position, a.type, a.number, a.value, a.channel, a.data) ==
			std::tie(b.id, b.position, b.type, b.number, b.value, b.channel, b.data);
	}

	// A MIDI part's own properties, without its notes and controllers.
	inline bool sameProperties(const MidiPart& a, const MidiPart& b)
	{
		return std::tie(a.id, a.track, a.name, a.position, a.length, a.mute, a.lock, a.shared, a.color) ==
			std::tie(b.id, b.track, b.name, b.position, b.length, b.mute, b.lock, b.shared, b.color);
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

	inline bool operator==(const AutomationTarget& a, const AutomationTarget& b)
	{
		return std::tie(a.type, a.parameter, a.slot, a.number, a.channel) ==
			std::tie(b.type, b.parameter, b.slot, b.number, b.channel);
	}

	inline bool operator==(const AutomationPoint& a, const AutomationPoint& b)
	{
		return std::tie(a.id, a.position, a.value, a.shape, a.curve) ==
			std::tie(b.id, b.position, b.value, b.shape, b.curve);
	}

	// An automation lane's own properties, without its points.
	inline bool sameProperties(const AutomationLane& a, const AutomationLane& b)
	{
		return std::tie(a.id, a.track, a.target, a.read) == std::tie(b.id, b.track, b.target, b.read);
	}

	inline bool operator==(const Track& a, const Track& b)
	{
		return std::tie(a.id, a.type, a.name, a.color, a.timeBase, a.parent, a.channel, a.children, a.autoFades,
				   a.timeFormat, a.enabled, a.picture) ==
			std::tie(b.id, b.type, b.name, b.color, b.timeBase, b.parent, b.channel, b.children, b.autoFades,
				b.timeFormat, b.enabled, b.picture);
	}
	// Where an audio event plays, in the project's samples (fadeedits.cpp).
	struct EventSpan
	{
		time::Samples start = 0;
		time::Samples end = 0;
		double ratio = 1.0; // project samples per clip frame
	};

	EventSpan eventSpan(const Project& project, const AudioEvent& event);

	// Whether an event's crossfade plays: the event it comes from is on the same track, neither is muted, and this
	// one starts inside it and ends after it. Otherwise the crossfade rests, and both play their own fades at those
	// ends. Playback (through the render snapshot) and the DAWproject export decide by this.
	bool crossfadeApplies(const Project& project, const AudioEvent& to);
}
