// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/media/backup.h"

#include "engine/audiofile/audiofile.h"
#include "engine/audiofile/file.h"
#include "engine/dawproject/dawproject.h"
#include "engine/jobs/jobs.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		namespace fs = std::filesystem;

		// Characters that file names can't have on at least one platform.
		constexpr std::string_view g_reservedCharacters = "<>:\"/\\|?*";

		constexpr size_t g_copyChunk = size_t{1} << 20;     // bytes
		constexpr size_t g_minimizeChunk = size_t{1} << 15; // sample frames

		fs::path pathOf(const std::string& utf8)
		{
			return fs::u8path(utf8);
		}

		// Whether Windows keeps name for a device: CON, PRN, AUX, NUL, COM0 to COM9 and LPT0 to LPT9, also with a
		// superscript 1, 2 or 3, in any case. Before Windows 11 they are devices with an extension too, and with spaces
		// before it ("nul.mix", "CON .x"), so a project file named so would be one there.
		bool isDeviceName(const std::string& name)
		{
			std::string device = name.substr(0, name.find('.'));
			while (!device.empty() && device.back() == ' ')
				device.pop_back();
			device = foldedName(fs::u8path(device));
			if (device == "con" || device == "prn" || device == "aux" || device == "nul")
				return true;
			if (device.compare(0, 3, "com") != 0 && device.compare(0, 3, "lpt") != 0)
				return false;
			const std::string number = device.substr(3);
			return (number.size() == 1 && number[0] >= '0' && number[0] <= '9') || number == "\xC2\xB9" ||
				number == "\xC2\xB2" || number == "\xC2\xB3";
		}

		fs::path numbered(const fs::path& name, int number)
		{
			if (number == 1)
				return name;
			return fs::u8path(name.stem().u8string() + "-" + std::to_string(number) + name.extension().u8string());
		}

		// Creates the file if it isn't there; false if it is. Throws std::runtime_error.
		bool createExclusively(const fs::path& file)
		{
			if (detail::openFile(file.u8string(), "wbx"))
				return true;
			const std::string problem = detail::lastError();
			std::error_code error;
			if (fs::exists(file, error))
				return false;
			throw std::runtime_error("can't create " + file.u8string() + ": " + problem);
		}

		// createFreeFile, skipping names that differ only in case from the ones in taken, and adding the new one.
		fs::path createFreeFile(const fs::path& folder, const fs::path& name, std::set<std::string>& taken)
		{
			for (int number = 1;; ++number)
			{
				const fs::path candidate = numbered(name, number);
				if (taken.count(foldedName(candidate)) == 0 && createExclusively(folder / candidate))
				{
					taken.insert(foldedName(candidate));
					return folder / candidate;
				}
			}
		}

		// Copies a file in chunks, so that a job can report progress and stop in between. Throws std::runtime_error.
		void copyFile(const fs::path& from, const fs::path& to, const std::function<void(size_t)>& copied)
		{
			std::ifstream input(from, std::ios::binary);
			if (!input)
				throw std::runtime_error("can't read " + from.u8string());
			std::ofstream output(to, std::ios::binary | std::ios::trunc); // made for this copy by createFreeFile
			if (!output)
				throw std::runtime_error("can't create " + to.u8string());
			std::vector<char> buffer(g_copyChunk);
			while (input)
			{
				input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
				const auto count = static_cast<size_t>(input.gcount());
				if (count == 0)
					break;
				if (!output.write(buffer.data(), static_cast<std::streamsize>(count)))
					throw std::runtime_error("can't write " + to.u8string());
				copied(count);
			}
			if (input.bad())
				throw std::runtime_error("can't read " + from.u8string());
			output.close();
			if (!output)
				throw std::runtime_error("can't write " + to.u8string());
		}

		// A range of sample frames in a file, and where it starts in the minimized file.
		struct Range
		{
			time::Samples start = 0;
			time::Samples end = 0;
			time::Samples target = 0;
		};

		// Playback resamples a file at another rate than the project's on a grid that starts at the file's first
		// frame: every step frames of the file fall on a whole frame of the project. A file moved by a multiple of
		// step plays the same samples; moved by anything else, it plays them a different fraction of a frame later.
		time::Samples resamplingStep(double fileRate, double projectRate)
		{
			const auto from = std::llround(fileRate);
			const auto to = std::llround(projectRate);
			if (from <= 0 || to <= 0 || from == to || static_cast<double>(from) != fileRate ||
				static_cast<double>(to) != projectRate)
				return 1;
			return from / std::gcd(from, to);
		}

		// The ranges of a media file that its audio events play, each with handles before and after, merged where
		// they overlap or touch. Every range starts, and every one but the last ends, on a multiple of step, so that
		// the frames keep their place on the resampling grid. Empty if an event starts beyond the end of the file,
		// which can't be mapped.
		std::vector<Range> usedRanges(
			const Project& project, ObjectId media, time::Samples frames, time::Samples handle, time::Samples step)
		{
			std::vector<Range> ranges;
			bool unmapped = false;
			forEveryAudioEvent(project,
				[&](const AudioEvent& event)
				{
					if (unmapped || project.clips.at(event.clip).media != media)
						return;
					if (event.offset >= frames)
					{
						unmapped = true;
						return;
					}
					const time::Samples start = std::max<time::Samples>(0, event.offset - handle);
					const time::Samples end = event.offset + event.length + handle;
					ranges.push_back({start - (start % step), std::min(frames, (end + step - 1) / step * step), 0});
				});
			if (unmapped)
				return {};
			std::sort(ranges.begin(), ranges.end(), [](const Range& a, const Range& b) { return a.start < b.start; });
			std::vector<Range> merged;
			for (const Range& range : ranges)
				if (!merged.empty() && range.start <= merged.back().end)
					merged.back().end = std::max(merged.back().end, range.end);
				else
					merged.push_back(range);
			time::Samples target = 0;
			for (Range& range : merged)
			{
				range.target = target;
				target += range.end - range.start;
			}
			return merged;
		}

		// Whether a minimized file keeps what matters of the source: every sample, read as 32-bit float, and the
		// metadata that refers to its frames. 32-bit integers and 64-bit floats don't fit into a float, and a Broadcast
		// Wave time reference counts from the first frame, which minimizing moves; such files are copied whole.
		bool canMinimize(const AudioFileInfo& info)
		{
			return info.sampleFormat != SampleFormat::Int32 && info.sampleFormat != SampleFormat::Float64 &&
				!info.broadcast;
		}

		// The format of a minimized file: the source's where GNUbase writes it and the samples survive unchanged
		// (16 and 24 bits; 32-bit float WAV), otherwise 32-bit float WAV, which holds every sample as it is read.
		WriteOptions minimizedFormat(const AudioFileInfo& info)
		{
			WriteOptions options;
			options.sampleRate = info.sampleRate;
			options.channels = info.channels;
			const bool integer = info.sampleFormat == SampleFormat::Int16 || info.sampleFormat == SampleFormat::Int24;
			const bool writable =
				info.format == AudioFormat::Wav || info.format == AudioFormat::Aiff || info.format == AudioFormat::Flac;
			if (writable && integer)
			{
				options.format = info.format;
				options.sampleFormat = info.sampleFormat;
			}
			else
			{
				options.format = AudioFormat::Wav;
				options.sampleFormat = SampleFormat::Float32;
			}
			return options;
		}

		fs::path minimizedName(const fs::path& source, const AudioFileInfo& info, const WriteOptions& options)
		{
			if (options.format == info.format && source.has_extension())
				return source.filename();
			return fs::u8path(source.stem().u8string() + ".wav");
		}

		// Writes the ranges of the reader's file one after the other.
		void writeRanges(AudioFileReader& reader, const std::vector<Range>& ranges, const fs::path& target,
			const WriteOptions& options, const std::function<void(size_t)>& written)
		{
			const auto writer = AudioFileWriter::create(target.u8string(), options);
			const auto channels = static_cast<size_t>(options.channels);
			std::vector<float> buffer(g_minimizeChunk * channels);
			for (const Range& range : ranges)
			{
				reader.seek(range.start);
				for (time::Samples position = range.start; position < range.end;)
				{
					const auto wanted =
						static_cast<size_t>(std::min<time::Samples>(range.end - position, g_minimizeChunk));
					const size_t read = reader.read(buffer.data(), wanted);
					if (read < wanted)
						std::fill(buffer.begin() + static_cast<std::ptrdiff_t>(read * channels),
							buffer.begin() + static_cast<std::ptrdiff_t>(wanted * channels), 0.0f);
					writer->write(buffer.data(), wanted);
					position += static_cast<time::Samples>(wanted);
					written(wanted * channels * sizeof(float));
				}
			}
			writer->close();
		}

		void removeUnused(Project& project)
		{
			std::set<ObjectId> usedClips;
			forEveryAudioEvent(project, [&usedClips](const AudioEvent& event) { usedClips.insert(event.clip); });
			std::set<ObjectId> usedMedia;
			for (auto clip = project.clips.begin(); clip != project.clips.end();)
			{
				if (usedClips.count(clip->first) == 0)
				{
					clip = project.clips.erase(clip);
					continue;
				}
				usedMedia.insert(clip->second.media);
				++clip;
			}
			for (auto media = project.media.begin(); media != project.media.end();)
				if (usedMedia.count(media->first) == 0)
					media = project.media.erase(media);
				else
					++media;
		}

		bool hasEvents(const Project& project, ObjectId media)
		{
			bool found = false;
			forEveryAudioEvent(project,
				[&](const AudioEvent& event) { found = found || project.clips.at(event.clip).media == media; });
			return found;
		}

		// A file or folder in the project's Mixdown folder (BackupOptions::mixdownFolder).
		struct Mixdown
		{
			fs::path relative;
			bool folder = false;
			std::uintmax_t bytes = 0;
		};

		// Lists what is in listed, a folder in the Mixdown folder root, into mixdowns, with the folders in it and what
		// they hold, but not skipped, the backup's own folder, nor links to folders. A file or folder that can't be
		// read goes into missing as {path}.
		void listMixdowns(const fs::path& root, const fs::path& listed, const fs::path& skipped,
			std::vector<Mixdown>& mixdowns, Json& missing)
		{
			std::error_code error;
			for (auto entry = fs::directory_iterator(listed, error); !error && entry != fs::directory_iterator();
				entry.increment(error))
			{
				const fs::path& path = entry->path();
				std::error_code ignored;
				if (isInside(path.u8string(), skipped) || fs::equivalent(path, skipped, ignored))
					continue;
				if (entry->is_directory(ignored))
				{
					// A link to a folder isn't followed: one to a folder above it would copy that again and again.
					if (!fs::is_directory(entry->symlink_status(ignored)))
						continue;
					mixdowns.push_back({path.lexically_relative(root), true, 0});
					listMixdowns(root, path, skipped, mixdowns, missing);
				}
				else if (entry->is_regular_file(ignored))
				{
					const std::uintmax_t bytes = entry->file_size(ignored);
					mixdowns.push_back({path.lexically_relative(root), false, ignored ? 0 : bytes});
				}
			}
			if (error)
				missing.push_back({{"path", listed.u8string()}});
		}

		// Removes what a backup wrote if it doesn't complete.
		class Cleanup
		{
		public:
			Cleanup(fs::path folder, bool createdFolder)
				: m_folder(std::move(folder)), m_audio(m_folder / "Audio"), m_createdFolder(createdFolder)
			{
			}

			~Cleanup()
			{
				if (m_done)
					return;
				std::error_code ignored;
				for (const fs::path& file : m_files)
					fs::remove(file, ignored);
				for (auto made = m_folders.rbegin(); made != m_folders.rend(); ++made)
					fs::remove(*made, ignored); // the innermost first
				fs::remove(m_audio, ignored);   // only if it's empty
				fs::remove(m_folder / "Freeze", ignored);
				if (m_createdFolder)
					fs::remove(m_folder, ignored);
			}

			Cleanup(const Cleanup&) = delete;
			Cleanup& operator=(const Cleanup&) = delete;

			void add(fs::path file) { m_files.push_back(std::move(file)); }
			void done() { m_done = true; }

			// Creates the folder and those it is in that don't exist yet, which go again if the backup doesn't
			// complete. Throws std::runtime_error.
			void createFolders(const fs::path& folder)
			{
				std::error_code error;
				if (fs::is_directory(folder, error))
					return;
				if (folder.has_relative_path())
					createFolders(folder.parent_path());
				fs::create_directory(folder, error);
				if (error)
					throw std::runtime_error("can't create " + folder.u8string() + ": " + error.message());
				m_folders.push_back(folder);
			}

		private:
			fs::path m_folder;
			fs::path m_audio;
			bool m_createdFolder = false;
			std::vector<fs::path> m_files;
			std::vector<fs::path> m_folders; // that it made, outermost first
			bool m_done = false;
		};
	}

	fs::path mediaFolderFor(const fs::path& projectFile)
	{
		return fs::absolute(projectFile).parent_path() / "Audio";
	}

	fs::path unsavedMediaFolder(const fs::path& settingsDirectory)
	{
		if (!settingsDirectory.empty())
			return settingsDirectory / "Recordings";
		std::error_code error;
		const fs::path temporary = fs::temp_directory_path(error);
		return error ? fs::path() : temporary / "GNUbase Recordings";
	}

	std::string foldedName(const fs::path& name)
	{
		std::string text = name.u8string();
		std::transform(text.begin(), text.end(), text.begin(),
			[](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
		return text;
	}

	bool isInside(const std::string& file, const fs::path& folder)
	{
		std::error_code error;
		const fs::path absoluteFile = fs::absolute(pathOf(file), error).lexically_normal();
		const fs::path absoluteFolder = fs::absolute(folder, error).lexically_normal();
		if (error)
			return false;
		const fs::path relative = absoluteFile.lexically_relative(absoluteFolder);
		return !relative.empty() && *relative.begin() != fs::path("..") && relative != fs::path(".");
	}

	fs::path createFreeFile(const fs::path& folder, const fs::path& name)
	{
		std::set<std::string> taken;
		return createFreeFile(folder, name, taken);
	}

	void removeFiles(const std::vector<std::string>& files)
	{
		std::error_code ignored;
		for (const std::string& file : files)
			fs::remove(pathOf(file), ignored);
	}

	void copyMedia(std::vector<MediaCopy>& files, const fs::path& folder, JobContext& job)
	{
		// How much there is to do, in bytes; 0 for a file that can't be read.
		std::vector<std::uintmax_t> sizes;
		sizes.reserve(files.size());
		std::vector<bool> readable;
		readable.reserve(files.size());
		std::uintmax_t total = 1;
		for (MediaCopy& file : files)
		{
			file.to.clear();
			const fs::path source = pathOf(file.from);
			std::error_code error;
			std::ifstream input(source, std::ios::binary | std::ios::ate);
			readable.push_back(fs::is_regular_file(source, error) && input.good());
			const std::streamoff size = readable.back() ? static_cast<std::streamoff>(input.tellg()) : 0;
			sizes.push_back(size > 0 ? static_cast<std::uintmax_t>(size) : 0);
			total += sizes.back();
		}

		std::vector<std::string> made;
		try
		{
			std::error_code error;
			fs::create_directories(folder, error);
			if (error)
				throw std::runtime_error("can't create " + folder.u8string() + ": " + error.message());
			std::uintmax_t finished = 0; // bytes of the files done
			for (size_t i = 0; i < files.size(); ++i)
			{
				job.checkCancelled();
				MediaCopy& file = files[i];
				if (readable[i])
				{
					const fs::path source = pathOf(file.from);
					const std::string detail = source.filename().u8string();
					job.progress(static_cast<double>(finished) / static_cast<double>(total), detail);
					const fs::path target = createFreeFile(folder, source.filename());
					made.push_back(target.u8string());
					std::uintmax_t done = finished;
					copyFile(source, target,
						[&](size_t bytes)
						{
							done += bytes;
							job.checkCancelled();
							job.progress(static_cast<double>(done) / static_cast<double>(total), detail);
						});
					file.to = target.u8string();
				}
				finished += sizes[i];
			}
		}
		catch (...)
		{
			removeFiles(made);
			throw;
		}
	}

	Json startMediaCopy(Jobs& jobs, std::string kind, std::vector<MediaCopy> files, std::filesystem::path folder,
		std::string action, uint64_t project)
	{
		return jobs.start(std::move(kind),
			[files = std::move(files), folder = std::move(folder), action = std::move(action), project](
				JobContext& job) mutable
			{
				copyMedia(files, folder, job);
				Json relink = Json::array();
				Json missing = Json::array();
				std::vector<std::string> made;
				for (const MediaCopy& file : files)
				{
					if (file.to.empty())
					{
						missing.push_back({{"media", file.media}, {"path", file.from}});
						continue;
					}
					relink.push_back({{"media", file.media}, {"from", file.from}, {"path", file.to}});
					made.push_back(file.to);
				}

				std::set<ObjectId> skipped;
				try
				{
					job.checkCancelled();
					if (!relink.empty() && !projectStillOpen(job, project))
					{
						for (const Json& entry : relink)
							skipped.insert(entry.at("media").get<ObjectId>());
					}
					else if (!relink.empty())
					{
						const Json command = {
							{"command", "media.relink"}, {"parameters", {{"files", relink}, {"project", project}}}};
						const Json result = job.command(
							"history.transaction", {{"description", action}, {"commands", Json::array({command})}});
						for (const Json& media : result.at("results").at(0).at("skipped"))
							skipped.insert(media.get<ObjectId>());
					}
				}
				catch (...)
				{
					removeFiles(made);
					throw;
				}

				Json copied = Json::array();
				Json changed = Json::array();
				std::vector<std::string> unused;
				for (const MediaCopy& file : files)
				{
					if (file.to.empty())
						continue;
					if (skipped.count(file.media) != 0)
					{
						changed.push_back(file.media);
						unused.push_back(file.to);
						continue;
					}
					copied.push_back({{"media", file.media}, {"from", file.from}, {"to", file.to}});
				}
				removeFiles(unused);
				return Json{
					{"copied", std::move(copied)}, {"missing", std::move(missing)}, {"changed", std::move(changed)}};
			});
	}

	bool projectStillOpen(JobContext& job, uint64_t project)
	{
		return job.command("project.revision", Json::object()).at("project").get<uint64_t>() == project;
	}

	std::string fileNameProblem(const std::string& name)
	{
		std::string problem = pathNameProblem(name);
		if (!problem.empty())
			return problem;
		if (isDeviceName(name))
			return "the name can't be " + name + ", which Windows keeps for a device";
		return {};
	}

	std::string pathNameProblem(const std::string& name)
	{
		if (name.empty())
			return "the name must not be empty";
		if (name == "." || name == "..")
			return "the name can't be " + name;
		for (const char c : name)
			if (g_reservedCharacters.find(c) != std::string_view::npos || static_cast<unsigned char>(c) < 0x20)
				return "the name can't contain " + std::string(1, c) + ", which file names can't have";
		if (name.back() == ' ' || name.back() == '.')
			return "the name can't end with a space or a dot";
		return {};
	}

	std::string safeFileName(const std::string& name)
	{
		std::string safe = name;
		for (char& c : safe)
			if (g_reservedCharacters.find(c) != std::string_view::npos || static_cast<unsigned char>(c) < 0x20)
				c = '_';
		while (!safe.empty() && (safe.back() == ' ' || safe.back() == '.'))
			safe.pop_back();
		if (safe.empty())
			return "Untitled";
		return isDeviceName(safe) ? "_" + safe : safe;
	}

	std::string projectFileStem(const fs::path& file)
	{
		constexpr std::string_view extension = ".dawproject";
		std::string name = file.filename().u8string();
		while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
			name.pop_back();
		// Compared byte by byte: the bytes where the extension would be may begin inside a character, which isn't a
		// path that Windows can convert.
		const auto sameLetter = [](char a, char b)
		{ return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); };
		if (name.size() > extension.size() &&
			std::equal(extension.begin(), extension.end(), name.end() - static_cast<std::ptrdiff_t>(extension.size()),
				sameLetter))
			name.resize(name.size() - extension.size());
		return name.empty() ? "Untitled" : name;
	}

	Json backupProject(Project project, const BackupOptions& options, JobContext& job)
	{
		const fs::path folder = fs::absolute(pathOf(options.folder));
		const fs::path audio = folder / "Audio";
		std::error_code error;
		Cleanup cleanup(folder, !fs::exists(folder, error));
		fs::create_directories(folder, error);
		if (error)
			throw std::runtime_error("can't create " + folder.u8string() + ": " + error.message());

		if (options.removeUnused)
			removeUnused(project);

		// How much there is to do, in bytes read: whole files, or the used ranges as float samples.
		struct Plan
		{
			ObjectId media = 0;
			bool minimize = false;
			std::uintmax_t bytes = 0;
		};
		std::vector<Plan> plans;
		std::uintmax_t total = 1;
		for (const auto& [id, media] : project.media)
		{
			Plan plan{id, options.minimize && hasEvents(project, id), 0};
			if (plan.minimize)
			{
				const auto handle = static_cast<time::Samples>(std::llround(options.handles * media.sampleRate));
				const time::Samples step = resamplingStep(media.sampleRate, project.sampleRate);
				for (const Range& range : usedRanges(project, id, media.length, handle, step))
					plan.bytes += static_cast<std::uintmax_t>(range.end - range.start) *
						static_cast<std::uintmax_t>(media.channels) * sizeof(float);
			}
			else
			{
				std::ifstream file(pathOf(media.path), std::ios::binary | std::ios::ate);
				const std::streamoff size = file ? static_cast<std::streamoff>(file.tellg()) : 0;
				plan.bytes = size > 0 ? static_cast<std::uintmax_t>(size) : 0;
			}
			total += plan.bytes;
			plans.push_back(plan);
		}
		// Frozen tracks' freeze files (docs/features/render-in-place.md) go along whole, into the folder Freeze, each
		// once, also when several tracks play it (a duplicated frozen track).
		std::map<std::string, std::uintmax_t> freezes; // freeze file -> its size
		for (const auto& [id, track] : project.trackById)
			if (track.frozen && freezes.count(track.frozen->path) == 0)
			{
				const std::uintmax_t size = fs::file_size(pathOf(track.frozen->path), error);
				total += freezes[track.frozen->path] = error ? 0 : size;
			}

		// The files and folders in the project's Mixdown folder as they are now, without the backup's own folder.
		// Files and folders that can't be read are reported as missing, as media are, and the backup goes on.
		Json missing = Json::array();
		std::vector<Mixdown> mixdowns;
		const fs::path mixdownFolder = options.mixdownFolder.empty() ? fs::path() : pathOf(options.mixdownFolder);
		if (!mixdownFolder.empty() && fs::is_directory(mixdownFolder, error))
		{
			listMixdowns(mixdownFolder, mixdownFolder, folder, mixdowns, missing);
			// In the order of their names, so that of two names that differ only in case the same one keeps it.
			std::sort(mixdowns.begin(), mixdowns.end(),
				[](const Mixdown& a, const Mixdown& b) { return a.relative < b.relative; });
		}
		for (const Mixdown& mixdown : mixdowns)
			total += mixdown.bytes;

		std::uintmax_t done = 0;
		std::string detail;
		const auto advance = [&](size_t bytes)
		{
			done += bytes;
			job.checkCancelled();
			job.progress(static_cast<double>(done) / static_cast<double>(total), detail);
		};

		std::set<std::string> taken;
		int files = 0;
		for (const Plan& plan : plans)
		{
			job.checkCancelled();
			MediaFile& media = project.media.at(plan.media);
			const fs::path source = pathOf(media.path);
			detail = source.filename().u8string();
			job.progress(static_cast<double>(done) / static_cast<double>(total), detail);

			std::unique_ptr<AudioFileReader> reader;
			std::vector<Range> ranges;
			if (plan.minimize)
			{
				try
				{
					reader = AudioFileReader::open(media.path);
				}
				catch (const AudioFileError&)
				{
					missing.push_back({{"media", media.id}, {"path", media.path}});
					done += plan.bytes;
					continue;
				}
				const AudioFileInfo& info = reader->info();
				const auto handle = static_cast<time::Samples>(std::llround(options.handles * info.sampleRate));
				ranges = usedRanges(
					project, media.id, info.frames, handle, resamplingStep(info.sampleRate, project.sampleRate));
				// Nothing to leave out, events that can't be mapped, or a file that minimizing would change: the file
				// goes along as it is.
				if (ranges.empty() || (ranges.size() == 1 && ranges[0].start == 0 && ranges[0].end == info.frames) ||
					!canMinimize(info))
					reader.reset();
			}
			else
			{
				const std::ifstream readable(source, std::ios::binary);
				if (!readable || fs::is_directory(source, error))
				{
					missing.push_back({{"media", media.id}, {"path", media.path}});
					done += plan.bytes;
					continue;
				}
			}

			fs::create_directories(audio, error);
			if (error)
				throw std::runtime_error("can't create " + audio.u8string() + ": " + error.message());
			if (reader)
			{
				const WriteOptions format = minimizedFormat(reader->info());
				const fs::path target = createFreeFile(audio, minimizedName(source, reader->info(), format), taken);
				cleanup.add(target);
				writeRanges(*reader, ranges, target, format, advance);
				forEveryAudioEvent(project,
					[&](AudioEvent& event)
					{
						if (project.clips.at(event.clip).media != media.id)
							return;
						const time::Samples offset = event.offset;
						const auto range = std::find_if(ranges.cbegin(), ranges.cend(),
							[offset](const Range& r) { return offset >= r.start && offset < r.end; });
						if (range != ranges.cend())
							event.offset = event.offset - range->start + range->target;
					});
				media.length = ranges.back().target + (ranges.back().end - ranges.back().start);
				media.path = target.u8string();
			}
			else
			{
				const fs::path target = createFreeFile(audio, source.filename(), taken);
				cleanup.add(target);
				copyFile(source, target, advance);
				media.path = target.u8string();
			}
			++files;
		}

		std::set<std::string> takenFreezes;
		std::map<std::string, std::string> freezeCopies; // freeze file -> its copy
		for (const auto& [path, bytes] : freezes)
		{
			job.checkCancelled();
			const fs::path source = pathOf(path);
			detail = source.filename().u8string();
			job.progress(static_cast<double>(done) / static_cast<double>(total), detail);
			const std::ifstream readable(source, std::ios::binary);
			if (!readable || fs::is_directory(source, error))
			{
				done += bytes;
				continue;
			}
			const fs::path frozenFolder = folder / "Freeze";
			fs::create_directories(frozenFolder, error);
			if (error)
				throw std::runtime_error("can't create " + frozenFolder.u8string() + ": " + error.message());
			const fs::path target = createFreeFile(frozenFolder, source.filename(), takenFreezes);
			cleanup.add(target);
			copyFile(source, target, advance);
			freezeCopies[path] = target.u8string();
			++files;
		}
		// Each track that plays a file that couldn't be read is reported.
		for (auto& [id, track] : project.trackById)
		{
			if (!track.frozen)
				continue;
			if (const auto copy = freezeCopies.find(track.frozen->path); copy != freezeCopies.end())
				track.frozen->path = copy->second;
			else
				missing.push_back({{"track", id}, {"path", track.frozen->path}});
		}

		// Under free names, as media: names that differ only in case, from a file system that tells them apart, don't
		// overwrite each other on one that doesn't.
		std::map<std::string, std::set<std::string>> takenMixdowns; // folded folder -> folded names in it
		for (const Mixdown& mixdown : mixdowns)
		{
			job.checkCancelled();
			const fs::path target = folder / "Mixdown" / mixdown.relative;
			if (mixdown.folder)
			{
				cleanup.createFolders(target);
				continue;
			}
			const fs::path source = mixdownFolder / mixdown.relative;
			std::ifstream readable(source, std::ios::binary);
			if (!readable)
			{
				missing.push_back({{"path", source.u8string()}});
				done += mixdown.bytes;
				continue;
			}
			readable.close();
			cleanup.createFolders(target.parent_path());
			detail = mixdown.relative.filename().u8string();
			job.progress(static_cast<double>(done) / static_cast<double>(total), detail);
			const fs::path copy = createFreeFile(
				target.parent_path(), target.filename(), takenMixdowns[foldedName(target.parent_path())]);
			cleanup.add(copy);
			copyFile(source, copy, advance);
		}

		job.checkCancelled();
		std::set<std::string> projectFiles;
		const fs::path file = createFreeFile(folder, fs::u8path(options.name + ".dawproject"), projectFiles);
		cleanup.add(file);
		DawProjectSaveOptions save;
		save.embedMedia = false;
		saveDawProject(project, file.u8string(), save);
		job.checkCancelled();
		cleanup.done();
		job.progress(1.0, {});
		return {{"file", file.u8string()}, {"files", files}, {"missing", std::move(missing)}};
	}
}
