// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/midi/quantizesettings.h"
#include "engine/schema.h"
#include "engine/time/signaturemap.h"
#include "engine/time/tempomap.h"
#include "engine/time/timecode.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Snap and grid (#87, reference manual pp. 105-109): where editing in the project window puts positions. The project
// keeps the settings; snap() is the one place that snaps, for the snap.position command and for the event display,
// which snaps while the mouse drags. docs/features/snap.md describes it.
namespace gnubase::engine
{
	class Engine;
	class ProjectModel;
	struct AudioEvent;
	struct Project;

	// What positions snap to (reference manual pp. 105-106).
	enum class SnapType : uint8_t
	{
		Grid,         // the nearest grid line
		GridRelative, // moves in grid steps, keeping the distance to the grid
		Events,       // the starts, ends and snap points of other events, and markers, when near
		Shuffle,      // events trade places with their neighbours (event.shuffle); positions don't snap
		Cursor,       // the project cursor, when near
		GridCursor,
		EventsCursor,
		EventsGridCursor,
	};

	// The grid (reference manual pp. 107-108): musical, linear, in frames or in samples.
	enum class GridType : uint8_t
	{
		Bar,
		Beat,        // the signature's denominator
		Quantize,    // the project's quantize grid, with tuplets and swing
		AdaptToZoom, // a note value that the view chooses (SnapGrid::zoomStep)
		Ms1,
		Ms10,
		Ms100,
		Ms1000,
		Subframe, // a frame has 80
		QuarterFrame,
		HalfFrame,
		Frame,
		TwoFrames,
		Second,
		Samples1,
		Samples10,
		Samples100,
		Samples44100,
	};

	// The time format the grid follows: the project window's ruler (reference manual pp. 105, 107-109). The names
	// are those of g_rulerTimeFormats.
	enum class GridFormat : uint8_t
	{
		BarsBeats,  // bar, beat, quantize, adaptToZoom
		Seconds,    // the milliseconds
		Timecode,   // the frames
		Samples,    // the samples
		UserFrames, // the frames, at the user's frame rate
	};

	inline constexpr size_t g_gridFormatCount = 5;

	// Whether a grid type is one of the format's.
	bool hasGrid(GridFormat format, GridType grid);
	// The format's grid types, in the order of the grid type menu: those hasGrid accepts.
	std::vector<GridType> gridTypes(GridFormat format);

	// Settings, not undo steps; saved with the project.
	struct SnapSettings
	{
		bool on = false;
		SnapType type = SnapType::Grid;
		GridFormat format = GridFormat::BarsBeats;
		// The grid type of each format, by GridFormat.
		std::array<GridType, g_gridFormatCount> grids = {
			GridType::Bar, GridType::Ms100, GridType::Frame, GridType::Samples100, GridType::Frame};
		bool zeroCrossing = false; // splitting and resizing audio events cut where the audio crosses zero
		// The grids entries of a newer build that this one can't read, by format name: a format it doesn't know, or
		// a grid type it doesn't know for one it does. Saving writes them back, until this build chooses a grid type
		// for that format. In the order of the names, as JSON objects have them.
		std::vector<std::pair<std::string, std::string>> newerGrids;

		// The grid in use: the format's.
		GridType grid() const { return grids.at(static_cast<size_t>(format)); }
		// Chooses the grid type of its format; one that isn't the format's in use switches to its format (a frame
		// grid to timecode).
		void setGrid(GridType grid);
		// Chooses the grid type of a format, which must be the format's.
		void setGrid(GridFormat of, GridType grid);
	};

	inline bool operator==(const SnapSettings& a, const SnapSettings& b)
	{
		return a.on == b.on && a.type == b.type && a.format == b.format && a.grids == b.grids &&
			a.zeroCrossing == b.zeroCrossing && a.newerGrids == b.newerGrids;
	}

	const char* snapTypeName(SnapType type);
	const char* gridTypeName(GridType grid);
	const char* gridFormatName(GridFormat format);
	std::optional<SnapType> snapTypeFromName(const std::string& name);
	std::optional<GridType> gridTypeFromName(const std::string& name);
	std::optional<GridFormat> gridFormatFromName(const std::string& name);
	// The settings as snap.get has them; saving, with a newer build's grids entries (SnapSettings::newerGrids).
	Json toJson(const SnapSettings& settings, bool saving = false);
	// Changes what json names: grids, then format, then grid, which is the format's (without a format, as before
	// #413, it chooses its format). Throws std::invalid_argument for unknown names and for a grid type that isn't its
	// format's. Loading a project, it leaves those out instead, so that a newer build's projects load, keeps a newer
	// build's grids entries in newerGrids, and reads grid only when grids has no entry for the format (before #413).
	SnapSettings snapSettingsFrom(const Json& json, SnapSettings settings, bool loading = false);

	// What the grid is made of.
	struct SnapGrid
	{
		time::TempoMap tempo{120.0};
		time::SignatureMap signatures;
		double sampleRate = 48000.0;
		midi::QuantizeSettings quantize;
		time::Ticks zoomStep = 0; // AdaptToZoom: ticks from each bar's start; 0 or a bar or more: bars
		time::FrameRate frameRate = time::g_fps25;
	};

	// The grid line nearest to position (project samples): ticks for the musical grids, samples for the others.
	time::Position nearestGridLine(GridType grid, const SnapGrid& at, time::Samples position);

	// A grid step near position: ticks for the musical grids, samples for the others; for nudging.
	time::Position gridStep(GridType grid, const SnapGrid& at, time::Samples position);

	// What a position snaps to, in project samples.
	struct SnapTarget
	{
		time::Samples position = 0;         // where the point would go
		std::optional<time::Samples> from;  // where it was: grid relative keeps its distance to the grid
		std::vector<time::Samples> magnets; // Events: other events' edges and snap points, markers
		time::Samples cursor = 0;           // the project cursor
		time::Samples range = 0;            // magnets and the cursor pull from this near
	};

	// Where the point goes: the nearest magnet or the cursor within range, if the type has them, otherwise the grid
	// line if it has a grid; ticks when a musical grid line put it there, otherwise samples. Off, shuffle, and the
	// magnetic types without anything near leave it where it is.
	time::Position snap(const SnapSettings& settings, const SnapGrid& grid, const SnapTarget& target);

	// The project's grid, with its tempo, signatures, quantize settings and the frame rate of format: the project's
	// (project setup, #78), or the user's for user frames.
	SnapGrid snapGrid(const Project& project, GridFormat format);

	// The position nearest to at (project samples) where an audio event's audio crosses zero, within 50 ms and the
	// clip (with inside, strictly inside the event, for splitting it), or at if there is none or the file can't be
	// read. Reads the file: not on audio threads.
	time::Samples zeroCrossing(const Project& project, const AudioEvent& event, time::Samples at, bool inside = false);

	// snap.get, snap.set and snap.position.
	void registerSnapCommands(ProjectModel& model, Engine& engine);
}
