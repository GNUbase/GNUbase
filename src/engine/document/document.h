// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/time/tempomap.h"

#include <cstdint>
#include <map>
#include <string>
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
	};

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

	struct Project
	{
		std::string name = "Untitled";
		double sampleRate = 48000.0;
		time::TempoMap tempo{120.0};
		ObjectId nextId = 1;

		std::vector<Bus> busses;      // in the order they are shown
		std::vector<ObjectId> tracks; // top-level tracks, from top to bottom
		std::map<ObjectId, Track> trackById;
		std::map<ObjectId, Channel> channels;
		std::map<ObjectId, MediaFile> media;
		std::map<ObjectId, Clip> clips;
		std::map<ObjectId, AudioEvent> audioEvents;
		std::map<ObjectId, MidiPart> midiParts;

		ObjectId newId() { return nextId++; }

		// Every track id from top to bottom, folders before the tracks they contain.
		std::vector<ObjectId> trackOrder() const;
	};

	// An empty project with a stereo input and a stereo output bus.
	Project newProject();
}
