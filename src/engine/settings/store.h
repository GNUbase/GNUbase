// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/schema.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// User settings: what the user chose for GNUbase as a whole, kept per user in versioned JSON files. Project settings
// are not here: they belong to the project and are saved in it (see docs/architecture.md, "Settings").
namespace gnubase::engine::settings
{
	// One setting.
	struct Definition
	{
		std::string key; // "area.name", e.g. "ui.scale"
		std::string description;
		Json schema;       // JSON schema of the value, e.g. {"type": "number", "minimum": 0.5, "maximum": 4}
		Json defaultValue; // must fit the schema
	};

	// Turns the values of a file at version from into those of version from + 1: renames keys, converts values.
	// Throwing fails the file, which then falls back to the defaults.
	struct Migration
	{
		int from = 0;
		std::function<void(Json& values)> apply;
	};

	// A kind of settings file: its name, its settings and how its older versions migrate to the current one.
	struct Format
	{
		std::string name; // the file is <name>.json
		int version = 1;  // raised whenever a migration is needed: a key is renamed, a value's meaning changes
		std::vector<Definition> definitions;
		// From the oldest version still read up to the current one, without gaps: the last one is from version - 1.
		// Files older than the first one are reset.
		std::vector<Migration> migrations;
	};

	enum class LoadStatus : uint8_t
	{
		InMemory, // no file: the settings are not saved
		Missing,  // no file yet: the defaults
		Loaded,   // a file of the current version
		Migrated, // a file of an older version, migrated and saved at the current one
		Reset,    // a file that was unreadable, corrupt, too old or too new: the defaults
	};

	// "inMemory", "missing", "loaded", "migrated" or "reset".
	const char* toString(LoadStatus status);

	// What happened when the file was read.
	struct LoadReport
	{
		LoadStatus status = LoadStatus::InMemory;
		int fileVersion = 0; // the version the file had, 0 if none could be read
		// Everything that was discarded, and why: the whole file or single values, which then have their defaults.
		std::vector<std::string> problems;
		// Where the file was kept as it was, if loading discarded or migrated something: <name>.corrupt.json for a
		// file that couldn't be read, <name>.v<version>.json for a file of another version or with invalid values.
		std::filesystem::path backup;
	};

	// A setting that doesn't exist, a value that doesn't fit its schema, a file that can't be written.
	class SettingsError : public std::runtime_error
	{
	public:
		using std::runtime_error::runtime_error;
	};

	// A setting's key with the C++ type of its value, for typed access: store.get(g_uiScale) is a double.
	template <typename T> struct Key
	{
		std::string_view name;
	};

	// The settings of one file. Reads the file when constructed and writes it after every change, keeping only the
	// values that differ from their defaults, so a default that changes in a later version reaches every user who
	// didn't change it. Belongs to one thread at a time (the engine's command thread), never to an audio thread.
	class Store
	{
	public:
		// Reads <directory>/<format.name>.json; without a directory, the settings are kept in memory only. Throws
		// std::logic_error if the format is inconsistent: repeated keys, defaults that don't fit, gaps in migrations.
		explicit Store(Format format, const std::filesystem::path& directory = {});

		const Format& format() const { return m_format; }
		const std::filesystem::path& file() const { return m_file; } // empty when kept in memory
		const LoadReport& loadReport() const { return m_report; }

		// A setting's value. Throws SettingsError if there is no such setting.
		const Json& value(std::string_view key) const;

		// Every setting's value, by key.
		Json values() const;

		// Changes a setting and writes the file; returns false if it had that value already. Throws SettingsError
		// if the setting doesn't exist, the value doesn't fit its schema or the file can't be written; then nothing
		// changes.
		bool set(std::string_view key, Json value);

		// Puts a setting back to its default; returns false if it had its default already.
		bool reset(std::string_view key);

		// Puts every setting back to its default; returns the keys of those that changed, sorted.
		std::vector<std::string> reset();

		template <typename T> T get(Key<T> key) const { return value(key.name).template get<T>(); }
		template <typename T> bool set(Key<T> key, const T& value) { return set(key.name, Json(value)); }

	private:
		const Definition& definition(std::string_view key) const;
		void load();
		Json readValues(const std::string& text);
		void loadValues(const Json& values);
		void keepBackup(const std::filesystem::path& backup, bool move);
		void save(const std::map<std::string, Json, std::less<>>& values) const;
		std::filesystem::path sibling(const std::string& suffix) const;

		Format m_format;
		std::filesystem::path m_file;
		std::map<std::string, Json, std::less<>> m_values;
		LoadReport m_report;
	};
}
