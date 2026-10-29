// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/channelformat.h"
#include "engine/midi/quantizesettings.h"
#include "engine/time/signaturemap.h"
#include "engine/time/tempomap.h"

#include <cstdint>
#include <map>
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

	// A connection to the audio hardware, e.g. "Stereo Out".
	struct Bus
	{
		ObjectId id = 0;
		std::string name;
		BusDirection direction = BusDirection::Output;
		ChannelFormat format = ChannelFormat::Stereo;
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
	};

	// A project has at most this many marker tracks.
	inline constexpr int g_maxMarkerTracks = 10;

	// The mixer channel of a track. MIDI tracks have a MIDI channel: no audio channels, and their output is an
	// instrument track's channel.
	struct Channel
	{
		ObjectId id = 0;
		ObjectId track = 0;
		ChannelFormat format = ChannelFormat::Stereo; // the speaker arrangement; None for MIDI
		ObjectId input = 0;                           // audio tracks: an input bus, or 0
		ObjectId output = 0;    // an output bus or a group channel; MIDI: an instrument channel; or 0
		std::string instrument; // instrument tracks: the built-in instrument that plays, or empty (plugins: M05)
		double volume = 0.0;    // the fader, in dB from g_minChannelVolume to g_maxChannelVolume
		double pan = 0.0;       // stereo channels' balance: -1 left, 0 center, 1 right
	};

	inline constexpr double g_minChannelVolume = -144.0;
	inline constexpr double g_maxChannelVolume = 6.0;

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
		Fade fadeIn;         // from the event's start
		Fade fadeOut;        // up to the event's end
		Crossfade crossfade; // from the event before; replaces the fade in, and from's fade out
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
	};

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
		Pan,             // the channel's balance, -1 to 1
		Instrument,      // a parameter of the channel's built-in instrument, in its range
		Controller,      // a MIDI control change: number and channel; 0 to 1
		PitchBend,       // channel; 0 to 1, 0.5 is no bend
		ChannelPressure, // channel; 0 to 1
	};

	struct AutomationTarget
	{
		AutomationTargetType type = AutomationTargetType::Volume;
		uint32_t parameter = 0; // Instrument: the parameter's id
		uint8_t number = 0;     // Controller: 0-127
		uint8_t channel = 0;    // the MIDI types: 0-15
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

	// NOLINTNEXTLINE(bugprone-exception-escape): MSVC's std::map allocates when moved; only out of memory throws
	struct Project
	{
		std::string name = "Untitled";
		double sampleRate = 48000.0;
		time::TempoMap tempo{120.0};
		time::SignatureMap signatures;
		AutoFades autoFades; // for audio tracks without settings of their own
		ObjectId nextId = 1;

		std::vector<Bus> busses;      // in the order they are shown
		std::vector<ObjectId> tracks; // top-level tracks, from top to bottom
		std::map<ObjectId, Track> trackById;
		std::map<ObjectId, Channel> channels;
		std::map<ObjectId, MediaFile> media;
		std::map<ObjectId, Clip> clips;
		std::map<ObjectId, AudioEvent> audioEvents;
		std::map<ObjectId, MidiPart> midiParts;
		std::map<ObjectId, Marker> markers;
		ObjectId activeMarkerTrack = 0; // the marker track that marker commands use by default, or 0
		std::map<ObjectId, AutomationLane> automation;

		// The quantize settings that apply when none are given, and whether recording quantizes (#132). Settings,
		// not part of undo.
		midi::QuantizeSettings quantize;
		bool autoQuantize = false;

		ObjectId newId() { return nextId++; }

		// Every track id from top to bottom, folders before the tracks they contain.
		std::vector<ObjectId> trackOrder() const;
	};

	// An empty project with a stereo input and a stereo output bus.
	Project newProject();

	// Whether a marker is long enough: a cycle marker ends at least one sample after its position.
	bool hasMinimumLength(const Project& project, const Marker& marker);

	// Moves the end of a cycle marker that isn't long enough, e.g. after a new time base or sample rate, to one sample
	// after its position. Returns whether it moved it.
	bool keepMinimumLength(const Project& project, Marker& marker);

	// Equality compares every member; the undo history uses it to find what a command changed.
	inline bool operator==(const Bus& a, const Bus& b)
	{
		return std::tie(a.id, a.name, a.direction, a.format) == std::tie(b.id, b.name, b.direction, b.format);
	}

	inline bool operator==(const Channel& a, const Channel& b)
	{
		return std::tie(a.id, a.track, a.format, a.input, a.output, a.instrument, a.volume, a.pan) ==
			std::tie(b.id, b.track, b.format, b.input, b.output, b.instrument, b.volume, b.pan);
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
		return std::tie(a.id, a.name, a.media, a.fadeIn, a.fadeOut) ==
			std::tie(b.id, b.name, b.media, b.fadeIn, b.fadeOut);
	}

	inline bool operator==(const AudioEvent& a, const AudioEvent& b)
	{
		return std::tie(a.id, a.track, a.position, a.clip, a.offset, a.length, a.volume, a.mute, a.fadeIn, a.fadeOut,
				   a.crossfade) ==
			std::tie(b.id, b.track, b.position, b.clip, b.offset, b.length, b.volume, b.mute, b.fadeIn, b.fadeOut,
				b.crossfade);
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
		return std::tie(a.id, a.track, a.name, a.position, a.length) ==
			std::tie(b.id, b.track, b.name, b.position, b.length);
	}

	inline bool operator==(const Marker& a, const Marker& b)
	{
		return std::tie(a.id, a.track, a.type, a.number, a.position, a.end, a.name, a.color) ==
			std::tie(b.id, b.track, b.type, b.number, b.position, b.end, b.name, b.color);
	}

	inline bool operator==(const AutomationTarget& a, const AutomationTarget& b)
	{
		return std::tie(a.type, a.parameter, a.number, a.channel) == std::tie(b.type, b.parameter, b.number, b.channel);
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
		return std::tie(a.id, a.type, a.name, a.color, a.timeBase, a.parent, a.channel, a.children, a.autoFades) ==
			std::tie(b.id, b.type, b.name, b.color, b.timeBase, b.parent, b.channel, b.children, b.autoFades);
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
