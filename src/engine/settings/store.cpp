// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/settings/store.h"

#include "platform/files.h"
#include "platform/log.h"
#include "platform/rt/realtime.h"

#include <set>
#include <system_error>
#include <utility>

namespace gnubase::engine::settings
{
	namespace fs = std::filesystem;

	namespace
	{
		// A file that can't be used at all: why, and the suffix of the backup it is moved to.
		class Unusable : public std::runtime_error
		{
		public:
			Unusable(const std::string& problem, std::string suffix)
				: std::runtime_error(problem), backupSuffix(std::move(suffix))
			{
			}

			std::string backupSuffix;
		};

		void checkFormat(const Format& format)
		{
			const auto fail = [&format](const std::string& problem)
			{ throw std::logic_error("settings format " + format.name + ": " + problem); };
			if (format.name.empty() || format.version < 1)
				fail("needs a name and a version from 1");
			std::set<std::string, std::less<>> keys;
			for (const Definition& definition : format.definitions)
			{
				if (definition.key.empty() || !keys.insert(definition.key).second)
					fail("the key \"" + definition.key + "\" is empty or repeated");
				if (const std::string problem = validate(definition.defaultValue, definition.schema, definition.key);
					!problem.empty())
					fail("the default doesn't fit: " + problem);
			}
			const int oldest = format.version - static_cast<int>(format.migrations.size());
			for (size_t i = 0; i < format.migrations.size(); ++i)
			{
				const Migration& migration = format.migrations[i];
				if (migration.from != oldest + static_cast<int>(i) || !migration.apply)
					fail("the migrations must lead from one version to the next up to version " +
						std::to_string(format.version));
			}
		}
	}

	const char* toString(LoadStatus status)
	{
		switch (status)
		{
		case LoadStatus::InMemory:
			return "inMemory";
		case LoadStatus::Missing:
			return "missing";
		case LoadStatus::Loaded:
			return "loaded";
		case LoadStatus::Migrated:
			return "migrated";
		case LoadStatus::Reset:
			return "reset";
		}
		return "";
	}

	Store::Store(Format format, const fs::path& directory) : m_format(std::move(format))
	{
		checkFormat(m_format);
		for (const Definition& definition : m_format.definitions)
			m_values.emplace(definition.key, definition.defaultValue);
		if (directory.empty())
			return;
		m_file = directory / fs::u8path(m_format.name + ".json");
		load();
	}

	const Definition& Store::definition(std::string_view key) const
	{
		for (const Definition& definition : m_format.definitions)
			if (definition.key == key)
				return definition;
		throw SettingsError("there is no setting " + std::string(key));
	}

	const Json& Store::value(std::string_view key) const
	{
		rt::assertNotRealtime("settings::Store::value");
		return m_values.find(definition(key).key)->second;
	}

	Json Store::values() const
	{
		rt::assertNotRealtime("settings::Store::values");
		Json values = Json::object();
		for (const auto& [key, value] : m_values)
			values[key] = value;
		return values;
	}

	bool Store::set(std::string_view key, Json value)
	{
		rt::assertNotRealtime("settings::Store::set");
		const Definition& setting = definition(key);
		if (const std::string problem = validate(value, setting.schema, setting.key); !problem.empty())
			throw SettingsError(problem);
		if (m_values.find(setting.key)->second == value)
			return false;
		auto values = m_values;
		values[setting.key] = std::move(value);
		save(values);
		m_values = std::move(values);
		return true;
	}

	bool Store::reset(std::string_view key)
	{
		return set(key, definition(key).defaultValue);
	}

	std::vector<std::string> Store::reset()
	{
		rt::assertNotRealtime("settings::Store::reset");
		std::map<std::string, Json, std::less<>> defaults;
		std::vector<std::string> changed;
		for (const Definition& definition : m_format.definitions)
			defaults.emplace(definition.key, definition.defaultValue);
		for (const auto& [key, value] : defaults)
			if (m_values.find(key)->second != value)
				changed.push_back(key);
		if (changed.empty())
			return changed;
		save(defaults);
		m_values = std::move(defaults);
		return changed;
	}

	fs::path Store::sibling(const std::string& suffix) const
	{
		return m_file.parent_path() / fs::u8path(m_format.name + suffix + ".json");
	}

	void Store::save(const std::map<std::string, Json, std::less<>>& values) const
	{
		if (m_file.empty())
			return;
		Json changed = Json::object();
		for (const Definition& definition : m_format.definitions)
		{
			if (const Json& value = values.find(definition.key)->second; value != definition.defaultValue)
				changed[definition.key] = value;
		}
		const Json document = {{"version", m_format.version}, {"values", std::move(changed)}};
		try
		{
			files::writeAtomically(m_file, document.dump(1, '\t') + "\n");
		}
		catch (const std::exception& error)
		{
			throw SettingsError("cannot write the settings: " + std::string(error.what()));
		}
	}

	void Store::keepBackup(const fs::path& backup, bool move)
	{
		std::error_code error;
		fs::remove(backup, error); // the backup of an earlier start, if there is one
		if (move)
			fs::rename(m_file, backup, error);
		else
			fs::copy_file(m_file, backup, fs::copy_options::overwrite_existing, error);
		if (error)
			logging::warning(
				"Cannot keep a backup of {} as {}: {}", m_file.u8string(), backup.u8string(), error.message());
		else
			m_report.backup = backup;
	}

	Json Store::readValues(const std::string& text)
	{
		const Json document = Json::parse(text, nullptr, false);
		if (document.is_discarded())
			throw Unusable("is not valid JSON", ".corrupt");
		const auto version = document.find("version");
		const auto values = document.find("values");
		if (!document.is_object() || version == document.end() || !version->is_number_integer() || *version < 1 ||
			values == document.end() || !values->is_object())
			throw Unusable("is no settings file: it needs a version from 1 and an object of values", ".corrupt");

		const int fileVersion = version->get<int>();
		const std::string backupSuffix = ".v" + std::to_string(fileVersion);
		m_report.fileVersion = fileVersion;
		if (fileVersion > m_format.version)
			throw Unusable("was written by a newer GNUbase: version " + std::to_string(fileVersion) +
					", this one reads up to version " + std::to_string(m_format.version),
				backupSuffix);
		const int oldest = m_format.version - static_cast<int>(m_format.migrations.size());
		if (fileVersion < oldest)
			throw Unusable("is too old: version " + std::to_string(fileVersion) +
					", this GNUbase migrates from version " + std::to_string(oldest),
				backupSuffix);

		Json migrated = *values;
		for (const Migration& migration : m_format.migrations)
		{
			if (migration.from < fileVersion)
				continue;
			const std::string step = "could not be migrated from version " + std::to_string(migration.from);
			try
			{
				migration.apply(migrated);
			}
			catch (const std::exception& failure)
			{
				throw Unusable(step + ": " + failure.what(), backupSuffix);
			}
			if (!migrated.is_object())
				throw Unusable(step, backupSuffix);
		}
		return migrated;
	}

	void Store::load()
	{
		rt::assertNotRealtime("settings::Store::load");
		const std::string name = m_file.u8string();
		std::error_code error;
		if (fs::status(m_file, error).type() == fs::file_type::not_found)
		{
			m_report.status = LoadStatus::Missing;
			return;
		}
		Json values;
		try
		{
			const std::optional<std::string> text = files::readFile(m_file);
			if (!text)
				throw Unusable("cannot read the file", ".corrupt");
			values = readValues(*text);
		}
		catch (const Unusable& unusable)
		{
			m_report.status = LoadStatus::Reset;
			m_report.problems.push_back(name + " " + unusable.what() + "; using the defaults");
			keepBackup(sibling(unusable.backupSuffix), true);
			logging::warning("Settings: {}, kept as {}", m_report.problems.back(), m_report.backup.u8string());
			return;
		}

		loadValues(values);
		const int fileVersion = m_report.fileVersion;
		const bool wasMigrated = fileVersion < m_format.version;
		m_report.status = wasMigrated ? LoadStatus::Migrated : LoadStatus::Loaded;
		if (!wasMigrated && m_report.problems.empty())
			return;
		// Keep the file as it was, then save what was read at the current version.
		keepBackup(sibling(".v" + std::to_string(fileVersion)), false);
		for (const std::string& problem : m_report.problems)
			logging::warning("Settings: {}", problem);
		if (wasMigrated)
			logging::info("Settings: migrated {} from version {} to {}", name, fileVersion, m_format.version);
		try
		{
			save(m_values);
		}
		catch (const SettingsError& failure)
		{
			logging::warning("Settings: {}", failure.what());
		}
	}

	void Store::loadValues(const Json& values)
	{
		const std::string prefix = m_file.u8string() + ": ";
		for (const auto& [key, value] : values.items())
		{
			const auto found = m_values.find(key);
			if (found == m_values.end())
			{
				m_report.problems.push_back(prefix);
				m_report.problems.back().append("there is no setting ").append(key).append("; dropped");
				continue;
			}
			if (const std::string problem = validate(value, definition(key).schema, key); !problem.empty())
			{
				m_report.problems.push_back(prefix);
				m_report.problems.back().append(problem).append("; using the default");
				continue;
			}
			found->second = value;
		}
	}
}
