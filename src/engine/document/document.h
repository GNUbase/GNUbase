// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

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
		int channels = 2;       // audio channels: 1 mono, 2 stereo, more for surround; 0 for MIDI
		ObjectId input = 0;     // audio tracks: an input bus, or 0
		ObjectId output = 0;    // an output bus or a group channel; MIDI: an instrument channel; or 0
		std::string instrument; // instrument tracks: the built-in instrument that plays, or empty (plugins: M05)
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

	// What audio events play: for now, a whole media file.
	struct Clip
	{
		ObjectId id = 0;
		std::string name;
		ObjectId media = 0;
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
	// the original back. A note whose timing no longer is the quantized one was changed by other means, such as
	// moving it by hand: its original no longer counts.
	struct QuantizeOrigin
	{
		NoteTiming original;
		NoteTiming quantized;
	};

	// Note and controller values have the resolution of MIDI 2.0 (#19): MIDI 1.0 values fit without loss.
	struct Note
	{
		ObjectId id = 0;
		time::Ticks position = 0; // from the start of the part
		time::Ticks length = 0;
		uint8_t pitch = 60;                   // 0-127
		uint16_t velocity = 0;                // 0-65535
		uint8_t channel = 0;                  // 0-15
		std::optional<QuantizeOrigin> origin; // set while the note is quantized
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
		ObjectId parent = 0;            // a folder track, or 0 at the top level
		ObjectId channel = 0;           // 0 for folders without a group channel
		std::vector<ObjectId> children; // folders: the tracks inside, from top to bottom
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

	// Equality compares every member; the undo history uses it to find what a command changed.
	inline bool operator==(const Bus& a, const Bus& b)
	{
		return std::tie(a.id, a.name, a.direction, a.channels) == std::tie(b.id, b.name, b.direction, b.channels);
	}

	inline bool operator==(const Channel& a, const Channel& b)
	{
		return std::tie(a.id, a.track, a.channels, a.input, a.output, a.instrument) ==
			std::tie(b.id, b.track, b.channels, b.input, b.output, b.instrument);
	}

	inline bool operator==(const MediaFile& a, const MediaFile& b)
	{
		return std::tie(a.id, a.path, a.sampleRate, a.channels, a.length) ==
			std::tie(b.id, b.path, b.sampleRate, b.channels, b.length);
	}

	inline bool operator==(const Clip& a, const Clip& b)
	{
		return std::tie(a.id, a.name, a.media) == std::tie(b.id, b.name, b.media);
	}

	inline bool operator==(const AudioEvent& a, const AudioEvent& b)
	{
		return std::tie(a.id, a.track, a.position, a.clip, a.offset, a.length, a.volume, a.mute) ==
			std::tie(b.id, b.track, b.position, b.clip, b.offset, b.length, b.volume, b.mute);
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
		return std::tie(a.id, a.position, a.length, a.pitch, a.velocity, a.channel, a.origin) ==
			std::tie(b.id, b.position, b.length, b.pitch, b.velocity, b.channel, b.origin);
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
		return std::tie(a.id, a.position, a.name, a.color) == std::tie(b.id, b.position, b.name, b.color);
	}

	inline bool operator==(const Track& a, const Track& b)
	{
		return std::tie(a.id, a.type, a.name, a.color, a.timeBase, a.parent, a.channel, a.children) ==
			std::tie(b.id, b.type, b.name, b.color, b.timeBase, b.parent, b.channel, b.children);
	}
}
