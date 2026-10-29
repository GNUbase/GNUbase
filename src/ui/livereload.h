// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

// Live reload: the UI's RML and RCSS files can be edited while GNUbase runs, and windows show the changes right away.
// Debug builds of the app check for changed files four times a second (GNUBASE_LIVE_RELOAD).
namespace gnubase::ui
{
	class FileInterface;
	class Shell;

	// If a file that RmlUi read has changed since (FileInterface::changedFiles), loads every window's documents again
	// from their files: style sheets and templates are read afresh, documents that were shown are shown again, and
	// data models stay bound. Documents that weren't loaded from a file are left as they are. Elements made in code
	// are lost with the old document, so documents should get their data through data models. Returns how many
	// documents were loaded again.
	size_t reloadChangedDocuments(FileInterface& files, Shell& shell);
}
