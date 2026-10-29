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
#include <memory>
#include <numeric>
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

		// Folded to lower case, so names that differ only in case count as the same on every file system.
		std::string folded(const fs::path& name)
		{
			std::string text = name.u8string();
			std::transform(text.begin(), text.end(), text.begin(),
				[](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
			return text;
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
				if (taken.count(folded(candidate)) == 0 && createExclusively(folder / candidate))
				{
					taken.insert(folded(candidate));
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
			for (const auto& [id, event] : project.audioEvents)
			{
				if (project.clips.at(event.clip).media != media)
					continue;
				if (event.offset >= frames)
					return {};
				const time::Samples start = std::max<time::Samples>(0, event.offset - handle);
				const time::Samples end = event.offset + event.length + handle;
				ranges.push_back({start - (start % step), std::min(frames, (end + step - 1) / step * step), 0});
			}
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
		// metadata that refers to its frames. 32-bit integers don't fit into a float, and a Broadcast Wave time
		// reference counts from the first frame, which minimizing moves; such files are copied whole.
		bool canMinimize(const AudioFileInfo& info)
		{
			return info.sampleFormat != SampleFormat::Int32 && !info.broadcast;
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
			for (const auto& [id, event] : project.audioEvents)
				usedClips.insert(event.clip);
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
			return std::any_of(project.audioEvents.begin(), project.audioEvents.end(),
				[&](const auto& entry) { return project.clips.at(entry.second.clip).media == media; });
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
				fs::remove(m_audio, ignored); // only if it's empty
				if (m_createdFolder)
					fs::remove(m_folder, ignored);
			}

			Cleanup(const Cleanup&) = delete;
			Cleanup& operator=(const Cleanup&) = delete;

			void add(fs::path file) { m_files.push_back(std::move(file)); }
			void done() { m_done = true; }

		private:
			fs::path m_folder;
			fs::path m_audio;
			bool m_createdFolder = false;
			std::vector<fs::path> m_files;
			bool m_done = false;
		};
	}

	fs::path mediaFolderFor(const fs::path& projectFile)
	{
		return fs::absolute(projectFile).parent_path() / "Audio";
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

	std::string fileNameProblem(const std::string& name)
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
		return safe.empty() ? "Untitled" : safe;
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

		std::uintmax_t done = 0;
		std::string detail;
		const auto advance = [&](size_t bytes)
		{
			done += bytes;
			job.checkCancelled();
			job.progress(static_cast<double>(done) / static_cast<double>(total), detail);
		};

		std::set<std::string> taken;
		Json missing = Json::array();
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
				for (auto& entry : project.audioEvents)
				{
					AudioEvent& event = entry.second;
					if (project.clips.at(event.clip).media != media.id)
						continue;
					const time::Samples offset = event.offset;
					const auto range = std::find_if(ranges.cbegin(), ranges.cend(),
						[offset](const Range& r) { return offset >= r.start && offset < r.end; });
					if (range != ranges.cend())
						event.offset = event.offset - range->start + range->target;
				}
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
