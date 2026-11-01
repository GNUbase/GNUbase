// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"
#include "engine/schema.h"

#include <filesystem>
#include <string>
#include <vector>

// Self-contained projects: the project folder with its media folder, copying media into it, and backups of a project
// into a folder of its own that opens on another machine. Everything here does file I/O, so it runs on the command
// thread or on a job's thread, never on an audio thread. Paths are UTF-8 on every platform.
namespace gnubase::engine
{
	class JobContext;
	class Jobs;

	// The folder where a project keeps its audio files: "Audio" in the folder of its project file.
	std::filesystem::path mediaFolderFor(const std::filesystem::path& projectFile);

	// Whether file is somewhere inside folder. Compares the absolute paths as they are written, without following
	// links.
	bool isInside(const std::string& file, const std::filesystem::path& folder);

	// Creates an empty file in folder, which must exist, and returns its path: name, or if a file of that name is
	// there, "<stem>-2<extension>", "<stem>-3<extension>" and so on. Creating fails if the file is there, even if
	// another job or program creates it at the same moment, so two writers never get the same file. Throws
	// std::runtime_error.
	std::filesystem::path createFreeFile(const std::filesystem::path& folder, const std::filesystem::path& name);

	// A media file and its copy.
	struct MediaCopy
	{
		ObjectId media = 0;
		std::string from; // the media file's path
		std::string to;   // the copy's path; empty if from couldn't be read
	};

	// Copies media files into folder, which it creates if needed, each under its own name or a free one (see
	// createFreeFile), in chunks, reporting progress to the job and stopping when it is cancelled. Sets each copy's
	// to, or leaves it empty for a file that can't be read. If it doesn't complete, it removes the copies it made.
	// Throws JobCancelled or std::runtime_error.
	void copyMedia(std::vector<MediaCopy>& files, const std::filesystem::path& folder, JobContext& job);

	// Copies media files into the media folder as a job, then points the media at their copies with one
	// command, a transaction named action, so that it is one undo step. Media whose path changed while the job
	// copied, or that were removed, keep their path, and their copies are removed. The job's result is
	// {copied: [{media, from, to}], missing: [{media, path}], changed: [media]}. Returns the job.
	Json startMediaCopy(
		Jobs& jobs, std::string kind, std::vector<MediaCopy> files, std::filesystem::path folder, std::string action);

	// Removes files, ignoring errors, e.g. copies that a job made and doesn't need.
	void removeFiles(const std::vector<std::string>& files);

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
	//
	// Minimized files start their ranges on the resampling grid of their sample rate in the project's, so that
	// they play sample for sample as the originals do. They keep every sample: files whose samples GNUbase can't
	// write unchanged (32-bit integer) and files whose metadata would no longer fit (Broadcast Wave, whose time
	// reference counts from the first sample) are copied whole. Other metadata, such as tags or cue points, isn't
	// kept in minimized files.
	Json backupProject(Project project, const BackupOptions& options, JobContext& job);
}
