// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui/themes.h"

#include "platform/files.h"
#include "platform/log.h"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/Plugin.h>
#include <RmlUi/Core/StyleSheetContainer.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <stdexcept>
#include <utility>

namespace gnubase::ui
{
	class Themes::Loader final : public Rml::Plugin
	{
	public:
		explicit Loader(std::vector<std::string> styleSheets) : m_styleSheets(std::move(styleSheets)) {}

		int GetEventClasses() override { return EVT_DOCUMENT; }

		// The shared style sheets and the themes first, then the document's own style.
		void OnDocumentLoad(Rml::ElementDocument* document) override
		{
			auto combined = Rml::MakeShared<Rml::StyleSheetContainer>();
			for (const std::string& file : m_styleSheets)
				if (const Rml::SharedPtr<Rml::StyleSheetContainer> sheet = Rml::Factory::InstanceStyleSheetFile(file))
					combined->MergeStyleSheetContainer(*sheet);
			if (const Rml::StyleSheetContainer* own = document->GetStyleSheetContainer())
				combined->MergeStyleSheetContainer(*own);
			document->SetStyleSheetContainer(std::move(combined));
		}

	private:
		std::vector<std::string> m_styleSheets; // as RmlUi names files
	};

	namespace
	{
		// The files in the folder with the extension, sorted; none if there is no such folder.
		std::vector<std::filesystem::path> filesIn(const std::filesystem::path& folder, const char* extension)
		{
			std::vector<std::filesystem::path> files;
			std::error_code error;
			for (const auto& entry : std::filesystem::directory_iterator(folder, error))
				if (entry.is_regular_file() && entry.path().extension() == extension)
					files.push_back(entry.path());
			std::sort(files.begin(), files.end());
			return files;
		}

		// The text without comments and whitespace at its ends.
		std::string withoutComments(const std::string& text)
		{
			std::string result;
			for (size_t at = 0; at < text.size();)
			{
				if (text.compare(at, 2, "/*") == 0)
				{
					const size_t end = text.find("*/", at + 2);
					at = end == std::string::npos ? text.size() : end + 2;
					continue;
				}
				result += text[at++];
			}
			const auto space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
			const auto first = std::find_if_not(result.begin(), result.end(), space);
			const auto last = std::find_if_not(result.rbegin(), result.rend(), space).base();
			return first < last ? std::string(first, last) : std::string();
		}

		// Checks that every rule of the theme's style sheet is inside its media query, so that it applies only while
		// the theme is active: the sheet is one "@media (theme: <name>) { ... }" block.
		void checkTheme(const std::string& name, const std::filesystem::path& file)
		{
			const std::optional<std::string> text = files::readFile(file);
			if (!text)
				throw std::runtime_error("cannot read the theme style sheet " + file.u8string());
			const std::string query = "@media (theme: " + name + ")";
			const std::string body = withoutComments(*text);
			const size_t open = body.find_first_not_of(" \t\r\n", query.size());
			size_t close = std::string::npos; // where the media query's block ends
			if (body.rfind(query, 0) == 0 && open != std::string::npos && body[open] == '{')
			{
				int depth = 0;
				for (size_t at = open; at < body.size() && close == std::string::npos; ++at)
				{
					if (body[at] == '{')
						++depth;
					else if (body[at] == '}' && --depth == 0)
						close = at;
				}
			}
			if (body.empty() || close != body.size() - 1)
				throw std::runtime_error(
					"the theme style sheet " + file.u8string() + " must have all its rules inside " + query + " { }");
		}
	}

	Themes::Themes(const std::filesystem::path& resources)
	{
		std::vector<std::string> styleSheets;
		for (const std::filesystem::path& file : filesIn(resources / "styles", ".rcss"))
			styleSheets.push_back(file.generic_u8string());

		std::error_code error;
		std::vector<std::filesystem::path> folders;
		for (const auto& entry : std::filesystem::directory_iterator(resources / "themes", error))
			if (entry.is_directory() && std::filesystem::is_regular_file(entry.path() / "theme.rcss"))
				folders.push_back(entry.path());
		std::sort(folders.begin(), folders.end());
		if (folders.empty())
			throw std::runtime_error("no themes in " + (resources / "themes").u8string());
		for (const std::filesystem::path& folder : folders)
		{
			const std::string name = folder.filename().u8string();
			checkTheme(name, folder / "theme.rcss");
			m_names.push_back(name);
			styleSheets.push_back((folder / "theme.rcss").generic_u8string());
		}

		for (const std::string& file : styleSheets)
			if (!Rml::Factory::InstanceStyleSheetFile(file))
				throw std::runtime_error("RmlUi cannot read the style sheet " + file);
		m_loader = std::make_unique<Loader>(std::move(styleSheets));
		Rml::RegisterPlugin(m_loader.get());
		logging::info("Themes: {} from {}", m_names.size(), resources.u8string());
	}

	Themes::~Themes()
	{
		Rml::UnregisterPlugin(m_loader.get());
	}

	bool Themes::has(std::string_view name) const
	{
		return std::find(m_names.begin(), m_names.end(), name) != m_names.end();
	}
}
