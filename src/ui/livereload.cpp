// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/livereload.h"

#include "platform/log.h"
#include "ui/rmlui.h"
#include "ui/shell.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/StringUtilities.h>

#include <filesystem>
#include <vector>

namespace gnubase::ui
{
	size_t reloadChangedDocuments(FileInterface& files, Shell& shell)
	{
		const std::vector<std::filesystem::path> changed = files.changedFiles();
		if (changed.empty())
			return 0;
		for (const std::filesystem::path& file : changed)
			logging::info("{} changed; loading the documents again", file.u8string());
		Rml::Factory::ClearStyleSheetCache();
		Rml::Factory::ClearTemplateCache();

		size_t reloaded = 0;
		for (Window* window : shell.windows())
		{
			Rml::Context& context = window->context();
			std::vector<Rml::ElementDocument*> documents;
			documents.reserve(static_cast<size_t>(context.GetNumDocuments()));
			for (int i = 0; i < context.GetNumDocuments(); ++i)
				documents.push_back(context.GetDocument(i));
			for (Rml::ElementDocument* document : documents)
			{
				// RmlUi writes a Windows drive's colon as | in source URLs.
				const Rml::String& source = document->GetSourceURL();
				if (!files.hasRead(std::filesystem::u8path(Rml::StringUtilities::Replace(source, '|', ':'))))
					continue; // not from a file
				Rml::ElementDocument* loaded = context.LoadDocument(source);
				if (loaded == nullptr)
				{
					logging::warning("{} couldn't be loaded again; the window keeps the old document", source);
					continue;
				}
				if (document->IsVisible())
					loaded->Show();
				document->Close();
				++reloaded;
			}
			window->requestRender();
		}
		return reloaded;
	}
}
