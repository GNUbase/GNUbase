// SPDX-License-Identifier: GPL-3.0-or-later
// The pool (#93, docs/features/pool.md): every clip and media file of the project, the folders that organize them,
// and media management. pool.get is a query; the others change the project, so each is one undo step and changes
// nothing if it fails (ProjectModel::add). media.locate and media.conform read and write files on the command thread,
// as media.convert does. Steps that consist of other commands run them through m_edits, so they emit their events.
#include "engine/audiofile/audiofile.h"
#include "engine/audiofile/resampler.h"
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/media/backup.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <map>
#include <set>
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
			for (const auto& [id, event] : project.audioEvents)
				++uses[event.clip];
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
	}

	void ProjectModel::registerPoolCommands(Engine& engine)
	{
		const Json integer = {{"type", "integer"}};
		const Json number = {{"type", "number"}};
		const Json boolean = {{"type", "boolean"}};
		const Json string = {{"type", "string"}};
		const Json ids = {{"type", "array"}, {"items", integer}};
		const auto result = [](std::vector<std::string> names)
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
				"number of audio events that use it (used), its media file's path, sample rate, channel count and "
				"length in sample frames, whether the file is missing and whether it is external, outside the project "
				"folder (every file of a project that has no file yet). The totals of the info line: audioFiles, "
				"usedFiles (media files that an event plays), missingFiles, externalFiles and totalSize, the bytes of "
				"the files on disk.",
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
						{"channels", media.channels}, {"length", media.length}, {"missing", missing(media)},
						{"external", external(media)}});
				}
				Json folders = Json::array();
				for (const auto& [id, folder] : project.poolFolders)
					folders.push_back(toJson(folder));
				int missingFiles = 0;
				int externalFiles = 0;
				std::uintmax_t totalSize = 0;
				for (const auto& [id, media] : project.media)
				{
					std::error_code error;
					const std::uintmax_t size = fs::file_size(fs::u8path(media.path), error);
					if (missing(media))
						++missingFiles;
					else if (!error)
						totalSize += size;
					if (external(media))
						++externalFiles;
				}
				return Json{{"folders", std::move(folders)}, {"clips", std::move(clips)},
					{"audioFiles", project.media.size()}, {"usedFiles", usedMedia.size()},
					{"missingFiles", missingFiles}, {"externalFiles", externalFiles}, {"totalSize", totalSize}};
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
				"Removes the clips that no audio event uses, then the media files that no clip uses, from the pool "
				"(reference manual p. 747), as one undo step. The files stay on disk. Returns the ids removed.",
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

		add(engine,
			{"media.locate", 1,
				"Looks for missing media files in folder and the folders inside it (the Resolve Missing Files "
				"dialog's Browse Folder, reference manual pp. 750-752): a file with the same name, in any letter "
				"case, and the media file's sample rate and channel count. The files found are relinked as one undo "
				"step. media limits it to those media files (default: every missing one). Returns the media files "
				"relinked and the ones still missing.",
				objectSchema({{"folder", string}, {"media", ids}}, {"folder"}),
				objectSchema({{"relinked", ids}, {"missing", ids}}, {"relinked", "missing"}), false},
			[this](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const fs::path folder = fs::u8path(parameters.at("folder").get<std::string>());
				std::error_code error;
				require(fs::is_directory(folder, error), "no folder " + folder.u8string());
				std::vector<ObjectId> wanted;
				if (parameters.contains("media"))
					for (const Json& id : parameters.at("media"))
					{
						const auto mediaId = id.get<ObjectId>();
						if (missing(find(project.media, mediaId, "media file")))
							wanted.push_back(mediaId);
					}
				else
					for (const auto& [id, media] : project.media)
						if (missing(media))
							wanted.push_back(id);

				// The files in the folder by their names in lower case.
				// ponytail: walks the whole folder on the command thread, like media.convert; a job if folders get
				// huge.
				std::multimap<std::string, fs::path> files;
				if (!wanted.empty())
					for (auto entry = fs::recursive_directory_iterator(
							 folder, fs::directory_options::skip_permission_denied, error);
						!error && entry != fs::recursive_directory_iterator(); entry.increment(error))
						if (entry->is_regular_file(error))
							files.emplace(lower(entry->path().filename().u8string()), entry->path());

				Json relink = Json::array();
				Json relinked = Json::array();
				Json stillMissing = Json::array();
				for (const ObjectId id : wanted)
				{
					const MediaFile& media = project.media.at(id);
					const std::string name = fs::u8path(media.path).filename().u8string();
					const auto [first, last] = files.equal_range(lower(name));
					std::vector<fs::path> candidates;
					for (auto file = first; file != last; ++file)
						candidates.push_back(file->second);
					// The same spelling first.
					std::stable_partition(candidates.begin(), candidates.end(),
						[&name](const fs::path& path) { return path.filename().u8string() == name; });
					bool found = false;
					for (const fs::path& candidate : candidates)
					{
						try
						{
							const AudioFileInfo info = AudioFileReader::open(candidate.u8string())->info();
							if (info.sampleRate != media.sampleRate || info.channels != media.channels)
								continue;
						}
						catch (const AudioFileError&)
						{
							continue;
						}
						relink.push_back({{"media", id}, {"path", candidate.u8string()}});
						relinked.push_back(id);
						found = true;
						break;
					}
					if (!found)
						stillMissing.push_back(id);
				}
				if (!relink.empty())
				{
					const Json relinkParameters = {{"files", relink}};
					CommandContext inner(relinkParameters, &context);
					m_edits.at("media.relink").handler(project, inner);
				}
				return Json{{"relinked", std::move(relinked)}, {"missing", std::move(stillMissing)}};
			});

		add(engine,
			{"media.removeMissing", 1,
				"Removes the media files that are missing from the pool with their clips and the audio events that "
				"play them (reference manual p. 752), as one undo step. Returns the ids removed.",
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

		add(engine,
			{"media.conform", 1,
				"Conforms media files to the project's sample rate (Convert Files with New + Replace in Pool, "
				"reference manual pp. 759-760): writes each one at another sample rate into the media folder as a "
				"32-bit float WAV file at the project's sample rate and points the media file at it, as one undo "
				"step. The clip fades, offsets, lengths, fades, crossfades and snap points of what plays it move with "
				"the new sample frames, so events play the same section. The original files stay on disk. media "
				"limits it to those media files (default: every one that isn't missing). The project must have been "
				"saved. Returns the media files conformed.",
				objectSchema({{"media", ids}}), objectSchema({{"media", ids}}, {"media"}), false},
			[this](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				std::vector<ObjectId> wanted;
				if (parameters.contains("media"))
					for (const Json& id : parameters.at("media"))
						wanted.push_back(find(project.media, id.get<ObjectId>(), "media file").id);
				else
					for (const auto& [id, media] : project.media)
						if (!missing(media))
							wanted.push_back(id);
				wanted.erase(std::remove_if(wanted.begin(), wanted.end(), [&project](ObjectId id)
								 { return project.media.at(id).sampleRate == project.sampleRate; }),
					wanted.end());
				if (wanted.empty())
					return Json{{"media", Json::array()}};
				require(
					!m_file.empty(), "save the project first: files are conformed into the folder of its project file");
				const fs::path folder = mediaFolderFor(fs::u8path(m_file));

				std::map<ObjectId, Rescale> rescales;
				std::vector<std::string> written; // removed if conforming fails
				try
				{
					std::error_code error;
					fs::create_directories(folder, error);
					require(!error, "can't create " + folder.u8string() + ": " + error.message());
					for (const ObjectId id : wanted)
					{
						MediaFile& media = project.media.at(id);
						const fs::path name = fs::u8path(fs::u8path(media.path).stem().u8string() + ".wav");
						const std::string path = createFreeFile(folder, name).u8string();
						written.push_back(path);
						const AudioFileInfo info = convertSampleRate(media.path, {path}, project.sampleRate);
						rescales[id] = {info.sampleRate / media.sampleRate, info.frames};
						media.path = path;
						media.sampleRate = info.sampleRate;
						media.length = info.frames;
						context.emit("media.changed", {{"media", toJson(media)}});
					}
				}
				catch (const CommandError&)
				{
					removeFiles(written);
					throw;
				}
				catch (const std::exception& problem)
				{
					removeFiles(written);
					throw CommandError(problem.what());
				}

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
				std::map<ObjectId, const Rescale*> eventRescales;
				for (const auto& [id, event] : project.audioEvents)
					eventRescales[id] = rescaleOf(event.clip);
				for (auto& [id, event] : project.audioEvents)
				{
					const Rescale* rescale = eventRescales.at(id);
					const auto from = eventRescales.find(event.crossfade.from);
					const Rescale* fromRescale = from == eventRescales.end() ? nullptr : from->second;
					if (rescale == nullptr && fromRescale == nullptr)
						continue;
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
					}
					if (fromRescale != nullptr)
						event.crossfade.outLength = (*fromRescale)(event.crossfade.outLength);
					context.emit("audioEvent.changed", {{"event", toJson(event)}});
				}
				Json conformed = Json::array();
				for (const ObjectId id : wanted)
					conformed.push_back(id);
				return Json{{"media", std::move(conformed)}};
			});
	}
}
