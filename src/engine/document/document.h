// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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

	// The most audio channels a bus or channel can have: 9.1.6 surround.
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
		int channels = 2;
	};

	enum class TrackType : uint8_t
	{
		Audio,
		Instrument,
		Midi,
		Folder, // contains other tracks; has a channel only if its group channel is on
		Group,  // sums the channels routed to it; holds no events
		Effect, // receives sends; holds no events
	};

	// The mixer channel of a track. MIDI tracks have a MIDI channel: no audio channels, and their output is an
	// instrument track's channel.
	struct Channel
	{
		ObjectId id = 0;
		ObjectId track = 0;
		int channels = 2;    // audio channels: 1 mono, 2 stereo, more for surround; 0 for MIDI
		ObjectId input = 0;  // audio tracks: an input bus, or 0
		ObjectId output = 0; // an output bus or a group channel; MIDI: an instrument channel; or 0
	};

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
	// where an event has no fade of its own; auto crossfades where an event starts on top of another or ends above
	// one that goes on, unless the two have a crossfade of their own.
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

	// Note and controller values have the resolution of MIDI 2.0 (#19): MIDI 1.0 values fit without loss.
	struct Note
	{
		ObjectId id = 0;
		time::Ticks position = 0; // from the start of the part
		time::Ticks length = 0;
		uint8_t pitch = 60;    // 0-127
		uint16_t velocity = 0; // 0-65535
		uint8_t channel = 0;   // 0-15
	};

	struct Controller
	{
		ObjectId id = 0;
		time::Ticks position = 0; // from the start of the part
		uint8_t number = 0;       // 0-127
		uint32_t value = 0;       // 0-4294967295
		uint8_t channel = 0;      // 0-15
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

	// A named position on the timeline.
	struct Marker
	{
		ObjectId id = 0;
		time::Ticks position = 0;
		std::string name;
		std::string color; // "#rrggbb", or empty for the default color
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

		ObjectId newId() { return nextId++; }

		// Every track id from top to bottom, folders before the tracks they contain.
		std::vector<ObjectId> trackOrder() const;
	};

	// An empty project with a stereo input and a stereo output bus.
	Project newProject();

	// Equality compares every member; the undo history uses it to find what a command changed.
	inline bool operator==(const Bus& a, const Bus& b)
	{
		return std::tie(a.id, a.name, a.direction, a.channels) == std::tie(b.id, b.name, b.direction, b.channels);
	}

	inline bool operator==(const Channel& a, const Channel& b)
	{
		return std::tie(a.id, a.track, a.channels, a.input, a.output) ==
			std::tie(b.id, b.track, b.channels, b.input, b.output);
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

	inline bool operator==(const Note& a, const Note& b)
	{
		return std::tie(a.id, a.position, a.length, a.pitch, a.velocity, a.channel) ==
			std::tie(b.id, b.position, b.length, b.pitch, b.velocity, b.channel);
	}

	inline bool operator==(const Controller& a, const Controller& b)
	{
		return std::tie(a.id, a.position, a.number, a.value, a.channel) ==
			std::tie(b.id, b.position, b.number, b.value, b.channel);
	}

	// A MIDI part's own properties, without its notes and controllers.
	inline bool sameProperties(const MidiPart& a, const MidiPart& b)
	{
		return std::tie(a.id, a.track, a.name, a.position, a.length) ==
			std::tie(b.id, b.track, b.name, b.position, b.length);
	}

	inline bool operator==(const Marker& a, const Marker& b)
	{
		return std::tie(a.id, a.position, a.name, a.color) == std::tie(b.id, b.position, b.name, b.color);
	}

	inline bool operator==(const Track& a, const Track& b)
	{
		return std::tie(a.id, a.type, a.name, a.color, a.timeBase, a.parent, a.channel, a.children, a.autoFades) ==
			std::tie(b.id, b.type, b.name, b.color, b.timeBase, b.parent, b.channel, b.children, b.autoFades);
	}
}
