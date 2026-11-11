// SPDX-License-Identifier: GPL-3.0-or-later
// The pool (#93, docs/features/pool.md): every clip and media file of the project, the folders that organize them,
// and media management. pool.get is a query; the others change the project, so each is one undo step and changes
// nothing if it fails (ProjectModel::add). media.locate and media.conform look through folders and convert files, so
// they run as jobs (#498), which change the project at the end with one command on the command thread, for the
// project that was open when they started only. Steps that consist of other commands run them through m_edits, so
// they emit their events.
#include "engine/audiofile/audiofile.h"
#include "engine/audiofile/resampler.h"
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/jobs/jobs.h"
#include "engine/media/backup.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		namespace fs = std::filesystem;

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		std::string idText(ObjectId id)
		{
			return std::to_string(id);
		}

		Json objectSchema(Json properties, const std::vector<std::string>& required = {})
		{
			return {{"type", "object"}, {"properties", std::move(properties)}, {"required", required},
				{"additionalProperties", false}};
		}

		template <typename Map> auto& find(Map& objects, ObjectId id, const char* what)
		{
			const auto found = objects.find(id);
			if (found == objects.end())
				throw CommandError(std::string("no ") + what + " " + idText(id));
			return found->second;
		}

		bool missing(const MediaFile& media)
		{
			std::error_code error;
			return !fs::is_regular_file(fs::u8path(media.path), error);
		}

		// The media files the media parameter names, each once, by id; every one without it.
		std::vector<ObjectId> mediaIds(const Project& project, const Json& parameters)
		{
			std::vector<ObjectId> ids;
			if (!parameters.contains("media"))
			{
				for (const auto& [id, media] : project.media)
					ids.push_back(id);
				return ids;
			}
			for (const Json& id : parameters.at("media"))
				ids.push_back(find(project.media, id.get<ObjectId>(), "media file").id);
			std::sort(ids.begin(), ids.end());
			ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
			return ids;
		}

		// Folds ASCII letters to lower case; other characters stay as they are.
		std::string lower(std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(),
				[](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
			return text;
		}

		void requireFolder(const Project& project, ObjectId folder)
		{
			require(folder == 0 || project.poolFolders.count(folder) != 0, "no pool folder " + idText(folder));
		}

		std::string folderName(const Json& parameters, const std::string& fallback)
		{
			std::string name = parameters.value("name", fallback);
			require(!name.empty(), "name must not be empty");
			return name;
		}

		std::map<ObjectId, int> clipUses(const Project& project)
		{
			std::map<ObjectId, int> uses;
			forEveryAudioEvent(project, [&uses](const AudioEvent& event) { ++uses[event.clip]; });
			forEverySamplerClip(project, [&uses](ObjectId clip) { ++uses[clip]; });
			return uses;
		}

		// Positions in a file's sample frames at the rate a media file had, in frames at the rate it has now.
		struct Rescale
		{
			double ratio = 1.0;
			time::Samples length = 0; // the file's new length

			time::Samples operator()(time::Samples frames) const
			{
				return std::llround(static_cast<double>(frames) * ratio);
			}
		};

		// A fade in and out within length, rescaled.
		void rescaleFades(Fade& in, Fade& out, const Rescale& rescale, time::Samples length)
		{
			in.length = std::min(rescale(in.length), length);
			out.length = std::min(rescale(out.length), length - in.length);
		}

		// After media files were pointed at files at another sample rate: moves the clip fades, and the offsets,
		// lengths, fades, crossfades, snap points and volume curves of the events that play them, with the new sample
		// frames.
		void rescaleUses(Project& project, CommandContext& context, const std::map<ObjectId, Rescale>& rescales)
		{
			const auto rescaleOf = [&project, &rescales](ObjectId clip) -> const Rescale*
			{
				const auto found = rescales.find(project.clips.at(clip).media);
				return found == rescales.end() ? nullptr : &found->second;
			};
			for (auto& [id, clip] : project.clips)
				if (const Rescale* rescale = rescaleOf(id))
				{
					rescaleFades(clip.fadeIn, clip.fadeOut, *rescale, rescale->length);
					context.emit("clip.changed", {{"clip", toJson(clip)}});
				}
			// Crossfades' outLength is in the frames of the event they come from, so read those before changing.
			// Inactive track versions' events play the media too.
			std::map<ObjectId, const Rescale*> eventRescales;
			forEveryAudioEvent(
				project, [&](const AudioEvent& event) { eventRescales[event.id] = rescaleOf(event.clip); });
			forEveryAudioEvent(project,
				[&](AudioEvent& event)
				{
					const Rescale* rescale = eventRescales.at(event.id);
					const auto from = eventRescales.find(event.crossfade.from);
					const Rescale* fromRescale = from == eventRescales.end() ? nullptr : from->second;
					if (rescale == nullptr && fromRescale == nullptr)
						return;
					if (rescale != nullptr)
					{
						const time::Samples offset = std::min((*rescale)(event.offset), rescale->length - 1);
						const time::Samples end = std::min((*rescale)(event.offset + event.length), rescale->length);
						event.offset = offset;
						event.length = std::max<time::Samples>(end - offset, 1);
						rescaleFades(event.fadeIn, event.fadeOut, *rescale, event.length);
						event.crossfade.length = std::min((*rescale)(event.crossfade.length), event.length);
						if (event.snapPoint)
							event.snapPoint = (*rescale)(*event.snapPoint);
						for (VolumePoint& point : event.volumeCurve)
							point.position = std::min((*rescale)(point.position), rescale->length);
					}
					if (fromRescale != nullptr)
						event.crossfade.outLength = (*fromRescale)(event.crossfade.outLength);
					// Only the active versions' events are the project's.
					const auto inProject = project.audioEvents.find(event.id);
					if (inProject != project.audioEvents.end() && &inProject->second == &event)
						context.emit("audioEvent.changed", {{"event", toJson(event)}});
				});
		}

		// Lets a job announce its progress at most every 50 ms, so that walking a folder with many small folders in
		// it doesn't send an event for each.
		class Throttle
		{
		public:
			// Whether it is time to announce again; the first time it is.
			bool due()
			{
				const auto now = std::chrono::steady_clock::now();
				if (m_started && now - m_last < std::chrono::milliseconds(50))
					return false;
				m_started = true;
				m_last = now;
				return true;
			}

		private:
			bool m_started = false;
			std::chrono::steady_clock::time_point m_last;
		};

		// A missing media file that media.locate looks for, as its job needs it.
		struct Sought
		{
			ObjectId id = 0;
			std::string path;
			double sampleRate = 0.0;
			int channels = 0;
			time::Samples length = 0;
		};

		// The share of media.locate's progress that walking the folder takes; checking the files found takes the rest.
		constexpr double g_walkShare = 0.9;

		// The files in folder and the folders inside it with one of names, by their names with ASCII letters in lower
		// case (names are in lower case too), so that a whole drive doesn't fill the memory. Stops between entries when
		// the job is cancelled. Its progress is measured in the folder's own entries, and it says which folder it looks
		// in.
		std::multimap<std::string, fs::path> filesInside(
			const fs::path& folder, const std::set<std::string>& names, JobContext& job)
		{
			constexpr auto options = fs::directory_options::skip_permission_denied;
			std::error_code error;
			size_t entries = 0;
			for (auto entry = fs::directory_iterator(folder, options, error);
				!error && entry != fs::directory_iterator(); entry.increment(error))
				++entries;
			std::multimap<std::string, fs::path> files;
			size_t begun = 0; // of the folder's own entries
			Throttle throttle;
			for (auto entry = fs::recursive_directory_iterator(folder, options, error);
				!error && entry != fs::recursive_directory_iterator(); entry.increment(error))
			{
				job.checkCancelled();
				if (entry.depth() == 0)
					++begun;
				std::error_code ignored; // a file that can't be looked at isn't one to relink
				if (std::string name = lower(entry->path().filename().u8string());
					names.count(name) != 0 && entry->is_regular_file(ignored))
					files.emplace(std::move(name), entry->path());
				if (throttle.due())
				{
					const auto done = static_cast<double>(std::max<size_t>(begun, 1) - 1);
					job.progress(g_walkShare * std::min(1.0, done / static_cast<double>(std::max<size_t>(entries, 1))),
						entry->path().parent_path().u8string());
				}
			}
			return files;
		}

		// media.locate's job: looks for the media files in folder, then relinks the ones found with media.relink in a
		// transaction, one undo step for project, the project open when it started. It skips media files relinked or
		// removed meanwhile, and files found that changed or went since.
		Json locateFiles(const std::vector<Sought>& sought, const fs::path& folder, uint64_t project, JobContext& job)
		{
			std::set<std::string> names;
			for (const Sought& media : sought)
				names.insert(lower(fs::u8path(media.path).filename().u8string()));
			const std::multimap<std::string, fs::path> files =
				sought.empty() ? std::multimap<std::string, fs::path>() : filesInside(folder, names, job);
			Json relink = Json::array();
			Json missing = Json::array();
			std::vector<ObjectId> found;
			for (size_t i = 0; i < sought.size(); ++i)
			{
				job.checkCancelled();
				const Sought& media = sought[i];
				const std::string name = fs::u8path(media.path).filename().u8string();
				job.progress(
					g_walkShare + ((1.0 - g_walkShare) * static_cast<double>(i) / static_cast<double>(sought.size())),
					name);
				const auto [first, last] = files.equal_range(lower(name));
				std::vector<fs::path> candidates;
				for (auto file = first; file != last; ++file)
					candidates.push_back(file->second);
				// The same spelling first.
				std::stable_partition(candidates.begin(), candidates.end(),
					[&name](const fs::path& path) { return path.filename().u8string() == name; });
				const auto fits = std::find_if(candidates.begin(), candidates.end(),
					[&media](const fs::path& candidate)
					{
						try
						{
							// A file of another length would leave events pointing past its end.
							const AudioFileInfo info = AudioFileReader::open(candidate.u8string())->info();
							return info.sampleRate == media.sampleRate && info.channels == media.channels &&
								info.frames == media.length;
						}
						catch (const AudioFileError&)
						{
							return false;
						}
					});
				if (fits == candidates.end())
				{
					missing.push_back(media.id);
					continue;
				}
				relink.push_back(
					{{"media", media.id}, {"path", fits->u8string()}, {"from", media.path}, {"length", media.length}});
				found.push_back(media.id);
			}

			job.checkCancelled();
			std::set<ObjectId> skipped;
			if (!relink.empty() && !projectStillOpen(job, project))
			{
				skipped.insert(found.begin(), found.end());
			}
			else if (!relink.empty())
			{
				const Json command = {
					{"command", "media.relink"}, {"parameters", {{"files", relink}, {"project", project}}}};
				const Json result = job.command("history.transaction",
					{{"description", "Locate missing files"}, {"commands", Json::array({command})}});
				for (const Json& media : result.at("results").at(0).at("skipped"))
					skipped.insert(media.get<ObjectId>());
			}
			Json relinked = Json::array();
			Json changed = Json::array();
			for (const ObjectId id : found)
				(skipped.count(id) != 0 ? changed : relinked).push_back(id);
			return Json{
				{"relinked", std::move(relinked)}, {"missing", std::move(missing)}, {"changed", std::move(changed)}};
		}

		// A media file that media.conform converts, as its job needs it.
		struct Conversion
		{
			ObjectId id = 0;
			std::string path;
			double sampleRate = 0.0;
			time::Samples length = 0;
		};

		// The media files that media.conform converts: the ones the media parameter names, each once, or every one
		// that isn't missing; those at the project's sample rate are left out. A media file given that is missing is
		// refused.
		std::vector<Conversion> conversionsOf(const Project& project, const Json& parameters)
		{
			// Each media file once: a second pass would convert the converted file and lose the first ratio.
			const bool given = parameters.contains("media");
			std::vector<Conversion> files;
			for (const ObjectId id : mediaIds(project, parameters))
			{
				const MediaFile& media = project.media.at(id);
				if (media.sampleRate == project.sampleRate)
					continue;
				if (missing(media))
				{
					require(!given, "media file " + idText(id) + " is missing: " + media.path);
					continue;
				}
				files.push_back({id, media.path, media.sampleRate, media.length});
			}
			return files;
		}

		// media.relinkResampled; openedProject is the open project's ProjectModel::openedRevision.
		Json relinkResampled(Project& project, CommandContext& context, uint64_t openedProject)
		{
			const Json& parameters = context.parameters();
			// Each media file once, whether its entry applies or not: a second one would rescale what the first did.
			std::set<ObjectId> given;
			for (const Json& entry : parameters.at("files"))
			{
				const auto id = entry.at("media").get<ObjectId>();
				require(given.insert(id).second, "media file " + idText(id) + " is given twice");
			}
			// Files conformed for another project, or to another project sample rate than the project's now, don't
			// conform it.
			const bool otherProject =
				parameters.contains("project") && parameters.at("project").get<uint64_t>() != openedProject;
			const bool rateChanged = parameters.contains("projectSampleRate") &&
				parameters.at("projectSampleRate").get<double>() != project.sampleRate;
			std::map<ObjectId, Rescale> rescales;
			Json relinked = Json::array();
			Json skipped = Json::array();
			for (const Json& entry : parameters.at("files"))
			{
				const auto id = entry.at("media").get<ObjectId>();
				const auto path = entry.at("path").get<std::string>();
				const auto found = project.media.find(id);
				if (otherProject || rateChanged)
				{
					skipped.push_back(id);
					continue;
				}
				if (entry.contains("from") &&
					(found == project.media.end() || found->second.path != entry.at("from").get<std::string>()))
				{
					skipped.push_back(id);
					continue;
				}
				MediaFile& media = find(project.media, id, "media file");
				if (entry.contains("fromSampleRate") && media.sampleRate != entry.at("fromSampleRate").get<double>())
				{
					skipped.push_back(id);
					continue;
				}
				AudioFileInfo info;
				try
				{
					info = AudioFileReader::open(path)->info();
				}
				catch (const AudioFileError& error)
				{
					throw CommandError(error.what());
				}
				require(info.channels == media.channels,
					path + " has " + std::to_string(info.channels) + " channels, media file " + idText(id) + " " +
						std::to_string(media.channels));
				require(info.frames > 0, path + " has no sample frames");
				rescales[id] = {info.sampleRate / media.sampleRate, info.frames};
				media.path = path;
				media.sampleRate = info.sampleRate;
				media.length = info.frames;
				context.emit("media.changed", {{"media", toJson(media)}});
				relinked.push_back(id);
			}
			rescaleUses(project, context, rescales);
			return Json{{"relinked", std::move(relinked)}, {"skipped", std::move(skipped)}};
		}

		// Where media.conform's job writes and how it changes the project.
		struct ConformTarget
		{
			fs::path folder;
			double sampleRate = 0.0; // the project's when it started
			uint64_t project = 0;    // the project open when it started (ProjectModel::openedRevision)
			std::string merge;       // the merge key of its undo step, or empty
		};

		// media.conform's job: writes each file at the target's sample rate into its folder, then points the media at
		// the new files with media.relinkResampled in a transaction, one undo step for the target's project that skips
		// media files changed meanwhile, and all of them if the project's sample rate changed. Removes the files it
		// wrote if it fails or is cancelled before, and afterwards the ones of media files skipped. The media folder
		// stays, since other writers may use it too.
		Json conformFiles(const std::vector<Conversion>& files, const ConformTarget& target, JobContext& job)
		{
			const fs::path& folder = target.folder;
			const double sampleRate = target.sampleRate;
			if (files.empty())
				return Json{{"media", Json::array()}, {"changed", Json::array()}};
			std::vector<std::string> written;
			Json result;
			try
			{
				std::error_code error;
				fs::create_directories(folder, error);
				if (error)
					throw std::runtime_error("can't create " + folder.u8string() + ": " + error.message());
				// Progress in sample frames read.
				double total = 0.0;
				for (const Conversion& file : files)
					total += static_cast<double>(std::max<time::Samples>(file.length, 1));
				double done = 0.0;
				Json relink = Json::array();
				std::set<std::string> taken = takenNames(folder);
				for (const Conversion& file : files)
				{
					job.checkCancelled();
					const fs::path source = fs::u8path(file.path);
					const std::string detail = source.filename().u8string();
					const auto frames = static_cast<double>(std::max<time::Samples>(file.length, 1));
					job.progress(done / total, detail);
					const std::string path =
						createFreeFile(folder, fs::u8path(source.stem().u8string() + ".wav"), taken).u8string();
					written.push_back(path);
					convertSampleRate(file.path, {path}, sampleRate,
						[&job, &detail, done, frames, total](double fraction)
						{
							job.checkCancelled();
							job.progress((done + (fraction * frames)) / total, detail);
						});
					done += frames;
					relink.push_back(
						{{"media", file.id}, {"path", path}, {"from", file.path}, {"fromSampleRate", file.sampleRate}});
				}
				job.checkCancelled();
				if (projectStillOpen(job, target.project))
				{
					Json transaction = {{"description", "Conform files"},
						{"commands",
							Json::array({{{"command", "media.relinkResampled"},
								{"parameters",
									{{"files", std::move(relink)}, {"projectSampleRate", sampleRate},
										{"project", target.project}}}}})}};
					if (!target.merge.empty())
						transaction["merge"] = target.merge;
					result = job.command("history.transaction", transaction).at("results").at(0);
				}
				else
				{
					Json ids = Json::array();
					for (const Conversion& file : files)
						ids.push_back(file.id);
					result = Json{{"relinked", Json::array()}, {"skipped", std::move(ids)}};
				}
			}
			catch (...)
			{
				removeFiles(written);
				throw;
			}

			std::set<ObjectId> skipped;
			for (const Json& media : result.at("skipped"))
				skipped.insert(media.get<ObjectId>());
			Json conformed = Json::array();
			Json changed = Json::array();
			std::vector<std::string> unused;
			for (size_t i = 0; i < files.size(); ++i)
			{
				if (skipped.count(files[i].id) == 0)
				{
					conformed.push_back(files[i].id);
					continue;
				}
				changed.push_back(files[i].id);
				unused.push_back(written[i]);
			}
			removeFiles(unused);
			return Json{{"media", std::move(conformed)}, {"changed", std::move(changed)}};
		}
	}

	void ProjectModel::registerPoolCommands(Engine& engine, Jobs& jobs)
	{
		const Json integer = {{"type", "integer"}};
		const Json number = {{"type", "number"}};
		const Json boolean = {{"type", "boolean"}};
		const Json string = {{"type", "string"}};
		const Json ids = {{"type", "array"}, {"items", integer}};
		const auto result = [](const std::vector<std::string>& names)
		{
			Json properties = Json::object();
			for (const std::string& name : names)
				properties[name] = {{"type", "object"}};
			return Json{{"type", "object"}, {"properties", properties}, {"required", names}};
		};

		const Json folderSchema = objectSchema({{"id", integer}, {"name", string}, {"parent", integer}});
		const Json clipSchema = objectSchema({{"id", integer}, {"name", string}, {"folder", integer},
			{"media", integer}, {"used", integer}, {"path", string}, {"sampleRate", number}, {"channels", integer},
			{"length", integer}, {"missing", boolean}, {"external", boolean}});
		add(engine,
			{"pool.get", 1,
				"The pool (reference manual pp. 739-744): its folders, and every clip with the folder it is in, the "
				"number of audio events and sampler tracks that use it (used), its media file's path, sample rate, "
				"channel count and length in sample frames, whether the file is missing and whether it is external, "
				"outside the project folder (every file of a project that has no file yet). The totals of the info "
				"line: audioFiles, usedFiles (media files that an event plays), missingFiles, externalFiles and "
				"totalSize, the bytes of the files on disk.",
				objectSchema({}),
				objectSchema({{"folders", {{"type", "array"}, {"items", folderSchema}}},
								 {"clips", {{"type", "array"}, {"items", clipSchema}}}, {"audioFiles", integer},
								 {"usedFiles", integer}, {"missingFiles", integer}, {"externalFiles", integer},
								 {"totalSize", integer}},
					{"folders", "clips", "audioFiles", "usedFiles", "missingFiles", "externalFiles", "totalSize"}),
				true},
			[this](Project& project, CommandContext&)
			{
				const fs::path projectFolder = m_file.empty() ? fs::path() : fs::u8path(m_file).parent_path();
				const auto external = [&projectFolder](const MediaFile& media)
				{ return projectFolder.empty() || !isInside(media.path, projectFolder); };
				// Each media file is looked at on disk once, for whether it is there and its size, not once per clip
				// and again per file: the panel asks after every edit.
				std::set<ObjectId> missingMedia;
				int externalFiles = 0;
				std::uintmax_t totalSize = 0;
				for (const auto& [id, media] : project.media)
				{
					std::error_code error;
					const fs::directory_entry entry(fs::u8path(media.path), error);
					if (error || !entry.is_regular_file(error))
						missingMedia.insert(id);
					else if (const std::uintmax_t size = entry.file_size(error); !error)
						totalSize += size;
					if (external(media))
						++externalFiles;
				}
				const std::map<ObjectId, int> uses = clipUses(project);
				std::set<ObjectId> usedMedia;
				Json clips = Json::array();
				for (const auto& [id, clip] : project.clips)
				{
					const MediaFile& media = project.media.at(clip.media);
					const auto used = uses.find(id);
					const int count = used == uses.end() ? 0 : used->second;
					if (count > 0)
						usedMedia.insert(clip.media);
					clips.push_back({{"id", id}, {"name", clip.name}, {"folder", clip.folder}, {"media", clip.media},
						{"used", count}, {"path", media.path}, {"sampleRate", media.sampleRate},
						{"channels", media.channels}, {"length", media.length},
						{"missing", missingMedia.count(clip.media) != 0}, {"external", external(media)}});
				}
				Json folders = Json::array();
				for (const auto& [id, folder] : project.poolFolders)
					folders.push_back(toJson(folder));
				return Json{{"folders", std::move(folders)}, {"clips", std::move(clips)},
					{"audioFiles", project.media.size()}, {"usedFiles", usedMedia.size()},
					{"missingFiles", missingMedia.size()}, {"externalFiles", externalFiles}, {"totalSize", totalSize}};
			});

		add(engine,
			{"poolFolder.add", 1,
				"Adds a pool folder (reference manual p. 757), named name (default New Folder), in the folder parent "
				"(default 0, the pool's Audio folder). Folders only organize the pool; files stay where they are.",
				objectSchema({{"name", string}, {"parent", integer}}), result({"folder"}), false},
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				PoolFolder folder;
				folder.name = folderName(parameters, "New Folder");
				folder.parent = parameters.value("parent", ObjectId{0});
				requireFolder(project, folder.parent);
				folder.id = project.newId();
				project.poolFolders[folder.id] = folder;
				Json added = {{"folder", toJson(folder)}};
				context.emit("poolFolder.added", added);
				return added;
			});
		add(engine,
			{"poolFolder.set", 1,
				"Renames a pool folder or moves it into another one (parent; 0 is the pool's Audio folder), but not "
				"into itself or a folder inside it.",
				objectSchema({{"folder", integer}, {"name", string}, {"parent", integer}}, {"folder"}),
				result({"folder"}), false},
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const auto id = parameters.at("folder").get<ObjectId>();
				PoolFolder& folder = find(project.poolFolders, id, "pool folder");
				folder.name = folderName(parameters, folder.name);
				const ObjectId parent = parameters.value("parent", folder.parent);
				requireFolder(project, parent);
				require(
					poolFolderOutside(project, parent, id), "can't move pool folder " + idText(id) + " inside itself");
				folder.parent = parent;
				Json changed = {{"folder", toJson(folder)}};
				context.emit("poolFolder.changed", changed);
				return changed;
			});
		add(engine,
			{"poolFolder.remove", 1, "Removes a pool folder; the clips and folders in it go into its parent folder.",
				objectSchema({{"folder", integer}}, {"folder"}), objectSchema({}), false},
			[](Project& project, CommandContext& context)
			{
				const auto id = context.parameters().at("folder").get<ObjectId>();
				const ObjectId parent = find(project.poolFolders, id, "pool folder").parent;
				for (auto& [otherId, folder] : project.poolFolders)
					if (folder.parent == id)
					{
						folder.parent = parent;
						context.emit("poolFolder.changed", {{"folder", toJson(folder)}});
					}
				for (auto& [clipId, clip] : project.clips)
					if (clip.folder == id)
					{
						clip.folder = parent;
						context.emit("clip.changed", {{"clip", toJson(clip)}});
					}
				project.poolFolders.erase(id);
				context.emit("poolFolder.removed", {{"folder", id}});
				return Json::object();
			});
		add(engine,
			{"clip.set", 1,
				"Renames a clip or moves it into a pool folder (0: the pool's Audio folder). The file keeps its name.",
				objectSchema({{"clip", integer}, {"name", string}, {"folder", integer}}, {"clip"}), result({"clip"}),
				false},
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				Clip& clip = find(project.clips, parameters.at("clip").get<ObjectId>(), "clip");
				clip.name = folderName(parameters, clip.name);
				clip.folder = parameters.value("folder", clip.folder);
				requireFolder(project, clip.folder);
				Json changed = {{"clip", toJson(clip)}};
				context.emit("clip.changed", changed);
				return changed;
			});

		add(engine,
			{"pool.removeUnused", 1,
				"Removes the clips that no audio event or sampler track uses, then the media files that no clip uses, "
				"from the pool (reference manual p. 747), as one undo step. The files stay on disk. Returns the ids "
				"removed.",
				objectSchema({}), objectSchema({{"clips", ids}, {"media", ids}}, {"clips", "media"}), false},
			[](Project& project, CommandContext& context)
			{
				const std::map<ObjectId, int> uses = clipUses(project);
				Json clips = Json::array();
				for (auto clip = project.clips.begin(); clip != project.clips.end();)
				{
					if (uses.count(clip->first) != 0)
					{
						++clip;
						continue;
					}
					clips.push_back(clip->first);
					context.emit("clip.removed", {{"clip", clip->first}});
					clip = project.clips.erase(clip);
				}
				std::set<ObjectId> used;
				for (const auto& [id, clip] : project.clips)
					used.insert(clip.media);
				Json media = Json::array();
				for (auto file = project.media.begin(); file != project.media.end();)
				{
					if (used.count(file->first) != 0)
					{
						++file;
						continue;
					}
					media.push_back(file->first);
					context.emit("media.removed", {{"media", file->first}});
					file = project.media.erase(file);
				}
				return Json{{"clips", std::move(clips)}, {"media", std::move(media)}};
			});

		addControl(engine,
			{"media.locate", 1,
				"Looks for missing media files in folder and the folders inside it (the Resolve Missing Files "
				"dialog's Browse Folder, reference manual pp. 750-752): a file with the same name, the same spelling "
				"first, else with ASCII letters in another case (other letters must match exactly), and the media "
				"file's sample rate, channel count and length. media limits it to those media files (default: every "
				"missing one). It runs as a job, so it can't be part of a history.transaction: this returns at once, "
				"job.progress events say which folder it looks in and then which media file it checks, job.cancel "
				"stops it, and once it is done the files found are relinked as one undo step, which skips media "
				"files relinked or removed meanwhile, files found that changed or went since, and every one if "
				"another project was opened meanwhile. The job's result is {relinked, missing, changed}: the media "
				"files relinked, the ones it didn't find, and the ones it found but that changed meanwhile and keep "
				"what they have now.",
				objectSchema({{"folder", string}, {"media", ids}}, {"folder"}),
				objectSchema({{"job", Jobs::schema()}}, {"job"}), false},
			[this, &jobs](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				fs::path folder = fs::u8path(parameters.at("folder").get<std::string>());
				std::error_code error;
				require(fs::is_directory(folder, error), "no folder " + folder.u8string());
				std::vector<Sought> sought;
				for (const ObjectId id : mediaIds(m_project, parameters))
					if (const MediaFile& media = m_project.media.at(id); missing(media))
						sought.push_back({id, media.path, media.sampleRate, media.channels, media.length});
				return Json{{"job",
					jobs.start("media.locate",
						[sought = std::move(sought), folder = std::move(folder), project = m_openedRevision](
							JobContext& job) { return locateFiles(sought, folder, project, job); })}};
			});

		add(engine,
			{"media.removeMissing", 1,
				"Removes the media files that are missing from the pool with their clips and the audio events that "
				"play them (reference manual p. 752), as one undo step; sampler tracks that play them are left without "
				"a sample. Returns the ids removed.",
				objectSchema({}),
				objectSchema({{"media", ids}, {"clips", ids}, {"events", ids}}, {"media", "clips", "events"}), false},
			[this](Project& project, CommandContext& context)
			{
				Json media = Json::array();
				Json clips = Json::array();
				Json events = Json::array();
				std::vector<ObjectId> gone;
				for (const auto& [id, file] : project.media)
					if (missing(file))
						gone.push_back(id);
				for (const ObjectId mediaId : gone)
				{
					std::vector<ObjectId> clipIds;
					for (const auto& [id, clip] : project.clips)
						if (clip.media == mediaId)
							clipIds.push_back(id);
					for (const ObjectId clipId : clipIds)
					{
						std::vector<ObjectId> eventIds;
						for (const auto& [id, event] : project.audioEvents)
							if (event.clip == clipId)
								eventIds.push_back(id);
						for (const ObjectId eventId : eventIds)
						{
							const Json removeParameters = {{"event", eventId}};
							CommandContext inner(removeParameters, &context);
							m_edits.at("audioEvent.remove").handler(project, inner);
							events.push_back(eventId);
						}
						// Inactive track versions lose theirs too.
						for (auto& [trackId, track] : project.trackById)
							for (TrackVersion& version : track.versions)
								version.audioEvents.erase(
									std::remove_if(version.audioEvents.begin(), version.audioEvents.end(),
										[clipId](const AudioEvent& event) { return event.clip == clipId; }),
									version.audioEvents.end());
						// Sampler tracks play nothing (#180).
						for (auto& [channelId, channel] : project.channels)
							if (channel.sample == clipId)
							{
								channel.sample = 0;
								context.emit("channel.changed", {{"channel", toJson(channel)}});
							}
						project.clips.erase(clipId);
						context.emit("clip.removed", {{"clip", clipId}});
						clips.push_back(clipId);
					}
					project.media.erase(mediaId);
					context.emit("media.removed", {{"media", mediaId}});
					media.push_back(mediaId);
				}
				return Json{{"media", std::move(media)}, {"clips", std::move(clips)}, {"events", std::move(events)}};
			});

		addControl(engine,
			{"media.conform", 1,
				"Conforms media files to the project's sample rate (Convert Files with New + Replace in Pool, "
				"reference manual pp. 759-760): writes each one at another sample rate into the media folder as a "
				"32-bit float WAV file at the project's sample rate and points the media file at it with "
				"media.relinkResampled, as one undo step. The clip fades, offsets, lengths, fades, crossfades, snap "
				"points and volume curves of what plays it move with the new sample frames, so events play the same "
				"section. The original files stay on disk. media limits it to those media files, which must not be "
				"missing (default: every one that isn't missing). The project must have been saved. It runs as a "
				"job, so it can't be part of a history.transaction: this returns at once, job.progress events say "
				"which file it converts, and job.cancel stops it and removes the files it wrote. Media files whose "
				"path or sample rate changed meanwhile, or that were removed, keep what they have, and their new "
				"files are removed; so do all of them if the project's sample rate changed or another project was "
				"opened meanwhile. With merge, the undo step is a history.transaction with that merge key: it joins "
				"the last step if that one has the key and nothing was undone since, as the project setup dialog's "
				"new sample rate does. The job's result is {media, changed}: the media files conformed and the ones "
				"that changed meanwhile.",
				objectSchema({{"media", ids}, {"merge", string}}), objectSchema({{"job", Jobs::schema()}}, {"job"}),
				false},
			[this, &jobs](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				std::vector<Conversion> files = conversionsOf(m_project, parameters);
				ConformTarget target;
				if (!files.empty())
				{
					require(!m_file.empty(),
						"save the project first: files are conformed into the folder of its project file");
					target.folder = mediaFolderFor(fs::u8path(m_file));
				}
				target.sampleRate = m_project.sampleRate;
				target.project = m_openedRevision;
				target.merge = parameters.value("merge", std::string());
				const fs::path folder = target.folder;
				return Json{{"job",
					jobs.start(
						"media.conform", [files = std::move(files), target = std::move(target)](JobContext& job)
						{ return conformFiles(files, target, job); }, folder)}};
			});

		const Json relinkEntry = objectSchema(
			{{"media", integer}, {"path", string}, {"from", string}, {"fromSampleRate", number}}, {"media", "path"});
		addEdit(engine,
			{"media.relinkResampled", 1,
				"Points media files at files of theirs at another sample rate, such as the ones media.conform writes, "
				"as one undo step: each media file takes its new file's sample rate and length, and the clip fades, "
				"offsets, lengths, fades, crossfades, snap points and volume curves of what plays it move with the new "
				"sample frames, so events play the same section. Each new file must be an audio file with the media "
				"file's "
				"channel count and at least one sample frame, and each media file can be given once, whether its entry "
				"applies or not. An entry with from applies only if the media file is still there and still points at "
				"from, one with fromSampleRate only if the media file is still at that sample rate; otherwise it is "
				"skipped. With projectSampleRate, every entry is skipped unless the project is still at that sample "
				"rate, and with project unless that project is still open (project.revision's project). Returns the "
				"media files relinked and the ones skipped.",
				objectSchema({{"files", {{"type", "array"}, {"items", relinkEntry}}}, {"projectSampleRate", number},
								 {"project", integer}},
					{"files"}),
				objectSchema({{"relinked", ids}, {"skipped", ids}}, {"relinked", "skipped"}), false},
			"Conform files", [this](Project& project, CommandContext& context)
			{ return relinkResampled(project, context, m_openedRevision); });
	}
}
