// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/settings/store.h"

#include "platform/files.h"
#include "platform/log.h"
#include "platform/rt/realtime.h"

#include <optional>
#include <set>
#include <system_error>
#include <utility>

namespace gnubase::engine::settings
{
	namespace fs = std::filesystem;

	namespace
	{
		// A file that can't be used at all: why, and the suffix of the backup it is moved to. A newer GNUbase's file
		// is left as it is instead.
		class Unusable : public std::runtime_error
		{
		public:
			Unusable(const std::string& problem, std::string suffix, bool isNewer = false)
				: std::runtime_error(problem), backupSuffix(std::move(suffix)), newer(isNewer)
			{
			}

			std::string backupSuffix;
			bool newer;
		};

		// A settings file's version and values.
		struct Document
		{
			int version = 0;
			Json values;
		};

		Document parseDocument(const std::string& text)
		{
			Json document = Json::parse(text, nullptr, false);
			if (document.is_discarded())
				throw Unusable("is not valid JSON", ".corrupt");
			const auto version = document.find("version");
			const auto values = document.find("values");
			if (!document.is_object() || version == document.end() || !version->is_number_integer() || *version < 1 ||
				values == document.end() || !values->is_object())
				throw Unusable("is no settings file: it needs a version from 1 and an object of values", ".corrupt");
			return {version->get<int>(), std::move(*values)};
		}

		std::string backupSuffix(int version)
		{
			return ".v" + std::to_string(version);
		}

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
		case LoadStatus::Newer:
			return "newer";
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
		const bool changed = m_values.find(setting.key)->second != value;
		if (!m_file.empty() && !m_report.readOnly)
		{
			// Only this setting changes in the file: what other processes saved meanwhile stays, and so does a value
			// that this GNUbase doesn't accept, unless it is this setting's.
			update(
				[&setting, &value](Json& fileValues)
				{
					if (value == setting.defaultValue)
						fileValues.erase(setting.key);
					else
						fileValues[setting.key] = value;
				});
		}
		m_values[setting.key] = std::move(value);
		return changed;
	}

	Json Store::modify(std::string_view key, const std::function<Json(const Json& value)>& change)
	{
		rt::assertNotRealtime("settings::Store::modify");
		const Definition& setting = definition(key);
		const auto changed = [&setting, &change](const Json& value)
		{
			Json result = change(value);
			if (const std::string problem = validate(result, setting.schema, setting.key); !problem.empty())
				throw SettingsError(problem);
			return result;
		};
		std::optional<Json> result;
		if (!m_file.empty() && !m_report.readOnly)
		{
			update(
				[&setting, &changed, &result](Json& fileValues)
				{
					const auto found = fileValues.find(setting.key);
					const bool fits =
						found != fileValues.end() && validate(*found, setting.schema, setting.key).empty();
					Json value = changed(fits ? *found : setting.defaultValue);
					if (value == setting.defaultValue)
						fileValues.erase(setting.key);
					else
						fileValues[setting.key] = value;
					result = std::move(value);
				});
		}
		if (!result) // kept in memory or read-only, which the file of a newer GNUbase may just have made it
			result = changed(m_values.find(setting.key)->second);
		m_values[setting.key] = *result;
		return *result;
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
		if (!m_file.empty() && !m_report.readOnly)
		{
			update(
				[this](Json& fileValues)
				{
					for (const Definition& definition : m_format.definitions)
						fileValues.erase(definition.key);
				});
		}
		m_values = std::move(defaults);
		return changed;
	}

	fs::path Store::sibling(const std::string& suffix) const
	{
		return m_file.parent_path() / fs::u8path(m_format.name + suffix + ".json");
	}

	fs::path Store::lockFile() const
	{
		return m_file.parent_path() / fs::u8path(m_format.name + ".lock");
	}

	void Store::write(const Json& fileValues) const
	{
		const Json document = {{"version", m_format.version}, {"values", fileValues}};
		try
		{
			files::writeAtomically(m_file, document.dump(1, '\t') + "\n");
		}
		catch (const std::exception& error)
		{
			throw SettingsError("cannot write the settings: " + std::string(error.what()));
		}
	}

	bool Store::update(const std::function<void(Json& fileValues)>& change)
	{
		std::optional<files::FileLock> lock;
		try
		{
			lock.emplace(lockFile(), m_lockTimeout);
		}
		catch (const std::system_error& failure)
		{
			throw SettingsError(std::string(failure.what()) + "; is another GNUbase writing the settings?");
		}
		files::removeStaleTemporaries(m_file);

		// The file as it is now: another process may have changed it since this one read it.
		Json current = Json::object();
		bool rewrite = false; // the file isn't usable as it is, so it is written even if nothing changes
		std::error_code error;
		if (fs::status(m_file, error).type() != fs::file_type::not_found)
		{
			try
			{
				const std::optional<std::string> text = files::readFile(m_file);
				if (!text)
					throw Unusable("cannot read the file", ".corrupt");
				Document document = parseDocument(*text);
				if (document.version > m_format.version)
					throw Unusable(
						"was replaced by a newer GNUbase's settings file", backupSuffix(document.version), true);
				if (document.version < m_format.version)
					throw Unusable("has an older version", backupSuffix(document.version));
				current = std::move(document.values);
			}
			catch (const Unusable& unusable)
			{
				if (unusable.newer)
				{
					makeReadOnly(m_file.u8string() + " " + unusable.what());
					return false;
				}
				if (const std::string failure = keepBackup(sibling(unusable.backupSuffix), false); !failure.empty())
					throw SettingsError(failure + "; the file is left as it is");
				current = m_fileValues;
				rewrite = true;
			}
		}
		Json changed = current;
		change(changed);
		if (rewrite || changed != current)
			write(changed);
		m_fileValues = std::move(changed);
		return true;
	}

	std::string Store::keepBackup(const fs::path& backup, bool move)
	{
		std::error_code error;
		fs::remove(backup, error); // the backup of an earlier start, if there is one
		error.clear();
		if (move)
			fs::rename(m_file, backup, error);
		else
			fs::copy_file(m_file, backup, fs::copy_options::overwrite_existing, error);
		if (error)
			return "cannot keep a backup of " + m_file.u8string() + " as " + backup.u8string() + ": " + error.message();
		m_report.backup = backup;
		return {};
	}

	void Store::makeReadOnly(const std::string& reason)
	{
		if (m_report.readOnly)
			return;
		m_report.readOnly = true;
		m_report.problems.push_back(reason + "; changes apply until GNUbase quits but aren't saved");
		logging::warning("Settings: {}", m_report.problems.back());
	}

	Json Store::readValues(const std::string& text)
	{
		Document document = parseDocument(text);
		const int fileVersion = document.version;
		m_report.fileVersion = fileVersion;
		if (fileVersion > m_format.version)
			throw Unusable("was written by a newer GNUbase: version " + std::to_string(fileVersion) +
					", this one reads up to version " + std::to_string(m_format.version),
				backupSuffix(fileVersion), true);
		const int oldest = m_format.version - static_cast<int>(m_format.migrations.size());
		if (fileVersion < oldest)
			throw Unusable("is too old: version " + std::to_string(fileVersion) +
					", this GNUbase migrates from version " + std::to_string(oldest),
				backupSuffix(fileVersion));

		Json migrated = std::move(document.values);
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
				throw Unusable(step + ": " + failure.what(), backupSuffix(fileVersion));
			}
			if (!migrated.is_object())
				throw Unusable(step, backupSuffix(fileVersion));
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
		// Locked while it is read and, after a migration or for a corrupt file, written or moved, so that no other
		// process writes it meanwhile.
		std::optional<files::FileLock> lock;
		try
		{
			lock.emplace(lockFile(), m_lockTimeout);
			files::removeStaleTemporaries(m_file);
		}
		catch (const std::system_error& failure)
		{
			makeReadOnly(failure.what());
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
			m_report.problems.push_back(name + " " + unusable.what() + "; using the defaults");
			logging::warning("Settings: {}", m_report.problems.back());
			if (unusable.newer)
			{
				// Left where it is, so that the newer GNUbase finds it again, and never overwritten from here.
				m_report.status = LoadStatus::Newer;
				makeReadOnly(name + " belongs to a newer GNUbase");
				return;
			}
			m_report.status = LoadStatus::Reset;
			if (m_report.readOnly)
				return;
			if (const std::string failure = keepBackup(sibling(unusable.backupSuffix), true); !failure.empty())
				makeReadOnly(failure); // a later change would overwrite the file without a copy
			else
				logging::info("Settings: kept {} as {}", name, m_report.backup.u8string());
			return;
		}

		loadValues(values);
		const int fileVersion = m_report.fileVersion;
		const bool wasMigrated = fileVersion < m_format.version;
		m_report.status = wasMigrated ? LoadStatus::Migrated : LoadStatus::Loaded;
		for (const std::string& problem : m_report.problems)
			logging::warning("Settings: {}", problem);
		if (!wasMigrated || m_report.readOnly)
			return;
		// Keep the file as it was, for going back to an older GNUbase, then save it at the current version.
		if (const std::string failure = keepBackup(sibling(backupSuffix(fileVersion)), false); !failure.empty())
		{
			makeReadOnly(failure);
			return;
		}
		logging::info("Settings: migrated {} from version {} to {}", name, fileVersion, m_format.version);
		try
		{
			write(m_fileValues);
		}
		catch (const SettingsError& failure)
		{
			logging::warning("Settings: {}", failure.what());
		}
	}

	void Store::loadValues(const Json& values)
	{
		// What this GNUbase doesn't know or accept stays in the file, for the GNUbase that wrote it.
		const std::string prefix = m_file.u8string() + ": ";
		for (const auto& [key, value] : values.items())
		{
			const auto found = m_values.find(key);
			if (found == m_values.end())
			{
				m_report.problems.push_back(prefix);
				m_report.problems.back().append("there is no setting ").append(key).append("; left in the file");
				m_fileValues[key] = value;
				continue;
			}
			const Definition& setting = definition(key);
			if (const std::string problem = validate(value, setting.schema, key); !problem.empty())
			{
				m_report.problems.push_back(prefix);
				m_report.problems.back().append(problem).append("; using the default, the value is left in the file");
				m_fileValues[key] = value;
				continue;
			}
			found->second = value;
			if (value != setting.defaultValue)
				m_fileValues[key] = value;
		}
	}
}
