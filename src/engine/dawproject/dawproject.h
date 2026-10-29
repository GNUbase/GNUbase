// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/document.h"

#include <stdexcept>
#include <string>

// DAWproject (https://github.com/bitwig/dawproject), GNUbase's project format: a ZIP file with project.xml and
// metadata.xml, which other applications read too, the media files, and gnubase.json, GNUbase's extension entry.
namespace gnubase::engine
{
	class DawProjectError : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};

	struct DawProjectSaveOptions
	{
		// Copy the media files into the file; otherwise refer to them where they are: relative to the file if they are
		// in its folder, so that the folder can move, or else by their absolute path.
		bool embedMedia = true;
	};

	// project.xml gets everything DAWproject can describe. gnubase.json gets the whole project and a checksum of
	// project.xml, so GNUbase gets back exactly what it saved: ids, input busses, time bases, MIDI tracks, exact
	// sample positions. Throws DawProjectError or AudioFileError.
	void saveDawProject(const Project& project, const std::string& path, const DawProjectSaveOptions& options);

	// Embedded media files are extracted to mediaFolder. If gnubase.json is there and project.xml is unchanged since
	// GNUbase wrote it, the project is restored from gnubase.json; otherwise, as when another application wrote the
	// file or changed it, it is built from project.xml. Throws DawProjectError.
	Project loadDawProject(const std::string& path, const std::string& mediaFolder);
}
