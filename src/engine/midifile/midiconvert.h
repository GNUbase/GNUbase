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
	// markers. At one tick, meta events come first, then note-offs, then controllers, then note-ons. Program changes
	// with a bank are preceded by bank select (controllers 0 and 32). Every track ends with the last part or the
	// range. Throws std::invalid_argument for a track that isn't a MIDI or instrument track.
	File exportProject(const Project& project, const ExportOptions& options);

	// A track read from a MIDI file. Positions are from the file's start, in project ticks.
	struct ImportedTrack
	{
		std::string name; // empty if the file has none
		std::vector<Note> notes;
		std::vector<Controller> controllers;
	};

	struct Import
	{
		time::TempoMap tempo{120.0};
		time::SignatureMap signatures;
		std::vector<Marker> markers;
		std::vector<ImportedTrack> tracks; // those with notes or controllers, in file order
	};

	// What a MIDI file holds. A type 1 or 2 file gives a track per file track with events, a type 0 file a track
	// per MIDI channel. Notes end at their note-off (the first note-on of a pitch and channel ends first) or, if
	// they have none, at the track's end. Bank select directly before a program change at the same tick becomes
	// the program change's bank. Tempo and time signatures that the project can't hold are adjusted: tempo to
	// 1-999 BPM, signature changes to the next bar start; signatures with a beat other than 1, 2, 4, 8 or 16 are
	// left out. Ids are left at 0.
	Import importFile(const File& file);

	// The part a track's events go into: from the start of the bar of its first event to the end of the bar of its
	// last one, in the given bars.
	struct PartSpan
	{
		time::Ticks position = 0;
		time::Ticks length = 0;
	};

	PartSpan partSpan(const ImportedTrack& track, const time::SignatureMap& bars);
}
