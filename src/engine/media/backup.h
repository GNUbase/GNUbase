// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"
#include "engine/schema.h"

#include <filesystem>
#include <string>

// Self-contained projects: the project folder with its media folder, copying media into it, and backups of a project
// into a folder of its own that opens on another machine. Everything here does file I/O, so it runs on the command
// thread or on a job's thread, never on an audio thread. Paths are UTF-8 on every platform.
namespace gnubase::engine
{
	class JobContext;

	// The folder where a project keeps its audio files: "Audio" in the folder of its project file.
	std::filesystem::path mediaFolderFor(const std::filesystem::path& projectFile);

	// Whether file is somewhere inside folder. Compares the absolute paths as they are written, without following
	// links.
	bool isInside(const std::string& file, const std::filesystem::path& folder);

	// Copies a file into folder, which it creates if needed, under its own name or, if a file of that name is
	// there, as "<name>-2", "<name>-3" and so on. Returns the copy's path. Throws std::runtime_error.
	std::string copyIntoFolder(const std::string& file, const std::filesystem::path& folder);

	// Why name can't be the name of a project file on every platform, or an empty string if it can.
	std::string fileNameProblem(const std::string& name);

	// name with the characters that file names can't have replaced, for a default file name.
	std::string safeFileName(const std::string& name);

	struct BackupOptions
	{
		std::string folder;               // an empty or new folder
		std::string name;                 // the project file's name, without the extension
		bool minimize = false;            // keep only the used ranges of audio files, with handles
		double handles = 1.0;             // seconds before and after each used range
		bool removeUnused = false;        // leave out clips that no audio event uses, and media that no clip uses
		bool currentVersionsOnly = false; // no effect until clips have offline processing (#141) or versions (#165)
		bool skipVideo = false;           // no effect until projects have video (#198)
	};

	// Writes <folder>/<name>.dawproject and copies the media into <folder>/Audio, where the project file refers to
	// them relative to itself, so the folder opens anywhere. A media file that can't be read is reported and keeps its
	// path. Reports progress to the job and stops when it is cancelled. If it doesn't complete, it removes what it
	// wrote. Returns {file, files, missing: [{media, path}]}. Throws JobCancelled, DawProjectError, AudioFileError or
	// std::runtime_error.
	Json backupProject(Project project, const BackupOptions& options, JobContext& job);
}
