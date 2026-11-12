// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"
#include "engine/midifile/smf.h"
#include "engine/time/signaturemap.h"
#include "engine/time/tempomap.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

// Converts between the project and standard MIDI files (#137). Values go from MIDI 2.0 resolution to MIDI 1.0 and
// back with the MIDI 2.0 rules (engine/midi/midivalues.h), so MIDI 1.0 data survives a round trip unchanged.
namespace gnubase::engine::midifile
{
	struct ExportOptions
	{
		int format = 1;               // 0: every track in one, 1: a tempo track, then one per exported track
		int ticksPerQuarter = 480;    // the file's resolution
		std::vector<ObjectId> tracks; // MIDI and instrument tracks; empty: all of them. They go in track order.
		time::Ticks start = 0;        // the range: what starts in it, moved to the file's start and cut at its end
		time::Ticks end = std::numeric_limits<time::Ticks>::max();
		bool markers = true;
	};

	// The project's MIDI and instrument tracks as a MIDI file: notes and controllers as they play (cut at the end of
	// their parts), track names, the project's name, tempo (ramps in sixteenth-note steps), time signatures and
	// markers (cycle markers at their start). In a type 1 file the tempo track has the active marker track's markers
	// and each other marker track with markers a file track of its own after the others, with its name and nothing
	// but its markers; a type 0 file has every marker. At one tick, meta events come first, then
	// note-offs, then controllers, then note-ons. Program changes with a bank are preceded by bank select (controllers
	// 0 and 32). A note ends where the next note of its pitch and channel in the same file track starts, at least a
	// tick after its own start: a note-off ends the oldest sounding note of its key, so overlapping notes of one key
	// would otherwise come back paired differently. Every track ends with the last part or the range. Throws
	// std::invalid_argument for a track that isn't a MIDI or instrument track.
	File exportProject(const Project& project, const ExportOptions& options);

	// A track read from a MIDI file. Positions are from the file's start, in project ticks.
	struct ImportedTrack
	{
		std::string name; // empty if the file has none
		std::vector<Note> notes;
		std::vector<Controller> controllers;
	};

	// A type 1 file's track with nothing but a name and markers, which a marker track of its own exported.
	struct ImportedMarkerTrack
	{
		std::string name;            // empty if the file has none
		std::vector<Marker> markers; // as in Import
	};

	struct Import
	{
		time::TempoMap tempo{120.0};
		time::SignatureMap signatures;
		std::vector<Marker> markers; // position markers in ticks, without a marker track or number; by position
		std::vector<ImportedMarkerTrack> markerTracks; // in file order
		std::vector<ImportedTrack> tracks;             // those with notes or controllers, in file order
	};

	// How importFile reads a file (#478).
	struct ImportOptions
	{
		bool dissolve = true;        // a type 0 file gives a track per MIDI channel; false: one track
		time::Ticks start = 0;       // where the file's start goes, in project ticks
		time::TempoMap tempo{120.0}; // the project's tempo and signatures, which hold before start
		time::SignatureMap signatures;
		// Where the project's content ends (parts, events, markers, automation): on musical tracks in ticks, on linear
		// tracks in samples at sampleRate, which keep their time under the file's tempo.
		time::Ticks contentEnd = 0;
		time::Samples contentEndSamples = 0;
		double sampleRate = 48000.0;
	};

	// What a MIDI file holds. A type 1 or 2 file gives a track per file track with events, a type 0 file a track
	// per MIDI channel unless options.dissolve is false. Everything is placed from options.start on; the tempo and
	// signatures before it are those of options, and the file's start at 120 BPM in 4/4 unless it sets its own. After
	// the file's end they are those of options again if they change after start or options.contentEnd (or
	// contentEndSamples, with the file's tempo up to its end) is after the file's end; otherwise the file's last ones
	// go on. The markers of a type 1 file's tracks after the first that hold nothing but a name and markers are marker
	// tracks of their own; all other markers are merged. The tracks of a type 2 file are independent sequences: they
	// are laid out one after another, each from the first bar start after the previous one's end, and each starts at
	// 120 BPM in 4/4 unless it sets its own tempo and signature at its start; sequences that would start after 2^53
	// ticks are left out. Notes end at their note-off (the first note-on of a pitch and channel ends first) or, if they
	// have none, at the track's end. Bank select directly before a program change at the same tick becomes the program
	// change's bank. Tempo and time signatures that the project can't hold are adjusted: tempo to 1-999 BPM, signature
	// changes to the next bar start; signatures with a beat other than 1, 2, 4, 8 or 16 are left out. Track and marker
	// names that aren't valid UTF-8 are read as Windows-1252 (Latin-1 with more characters in 0x80-0x9F). Ids are left
	// at 0.
	Import importFile(const File& file, const ImportOptions& options = ImportOptions());

	// The part a track's events go into: from the start of the bar of its first event to the end of the bar of its
	// last one, in the given bars.
	struct PartSpan
	{
		time::Ticks position = 0;
		time::Ticks length = 0;
	};

	PartSpan partSpan(const ImportedTrack& track, const time::SignatureMap& bars);
}
