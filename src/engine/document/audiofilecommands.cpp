// SPDX-License-Identifier: GPL-3.0-or-later
// Audio file import (#92): audioFile.import. Files are read, converted and split on the command thread, like
// media.convert; copies are made by a job, like media.add's. The import is one undo step that adds media, clips,
// tracks and events through the same commands a client would send, so it emits their events.
#include "engine/audiofile/audiofile.h"
#include "engine/audiofile/resampler.h"
#include "engine/document/channelformat.h"
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/engine.h"
#include "engine/jobs/jobs.h"
#include "engine/media/backup.h"
#include "engine/time/tempomap.h"

#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
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

		AudioFileInfo readInfo(const std::string& path)
		{
			try
			{
				return AudioFileReader::open(path)->info();
			}
			catch (const AudioFileError& error)
			{
				throw CommandError(error.what());
			}
		}

		// What a channel's file is called when a file is split: its speaker, or its number beyond 8 channels.
		std::string channelName(int channels, int channel)
		{
			if (channels > g_maxFormatChannels)
				return std::to_string(channel + 1);
			return speakerName(speakers(defaultFormat(channels)).at(static_cast<size_t>(channel)));
		}

		// What goes between a file's name and its channel's when it is split, by nameFormat (#477).
		std::string separatorOf(const std::string& nameFormat)
		{
			if (nameFormat == "underscore")
				return "_";
			if (nameFormat == "dot")
				return ".";
			if (nameFormat == "space")
				return " ";
			return "-";
		}

		// Writes the files that path becomes in the media folder: a WAV file at the project's sample rate, or with
		// split a mono WAV file per channel, named after path and the channel with separator between them. Returns
		// their paths and adds them to written.
		std::vector<std::string> convertInto(const fs::path& folder, const std::string& path, const AudioFileInfo& info,
			double sampleRate, bool split, const std::string& separator, std::vector<std::string>& written)
		{
			require(!folder.empty(), "there is no folder to convert " + path + " into: save the project first");
			std::error_code error;
			fs::create_directories(folder, error);
			require(!error, "can't create " + folder.u8string() + ": " + error.message());
			const std::string stem = fs::u8path(path).stem().u8string();
			std::vector<std::string> outputs;
			try
			{
				for (int channel = 0; channel < (split ? info.channels : 1); ++channel)
				{
					const std::string name = split ? stem + separator + channelName(info.channels, channel) : stem;
					outputs.push_back(createFreeFile(folder, fs::u8path(name + ".wav")).u8string());
					written.push_back(outputs.back());
				}
				convertSampleRate(path, outputs, sampleRate);
			}
			catch (const std::exception& problem)
			{
				throw CommandError(problem.what());
			}
			return outputs;
		}
	}

	void ProjectModel::registerAudioFileCommands(Engine& engine, Jobs& jobs)
	{
		const Json integer = {{"type", "integer"}};
		const Json boolean = {{"type", "boolean"}};
		const Json ids = {{"type", "array"}, {"items", integer}};
		Json job = Jobs::schema();
		job["type"] = {"object", "null"};

		add(engine,
			{"audioFile.import", 2,
				"Imports audio files (WAV, RF64, Wave64, AIFF, FLAC, Ogg Vorbis or MP3) as audio events at position, "
				"in sample frames from the project start (default 0). Each file goes on an audio track of its own: "
				"the first on track if given (such as the selected track), the others on new audio tracks named "
				"after them, with their channels. With oneTrack they go one after another on one track: track, or a "
				"new one. copy copies files from outside the project folder into its media folder (see "
				"project.location) as a job, returned as job (null without one) with the result media.add's copy job "
				"has; the media are pointed at the copies as an undo step of its own. MP3 and "
				"Ogg Vorbis files are always written into the media folder as 32-bit float WAV files (FLAC files "
				"stay as they are); in a project without a file, into the folder its recordings go to. convert "
				"writes files at another sample rate than the project's into the media folder as 32-bit float WAV "
				"files at the project's sample rate, MP3 and Ogg Vorbis files included. splitChannels writes each "
				"channel of a file with several into the media folder as a mono WAV file, named after the file and "
				"its speaker (numbered beyond 8 channels) as nameFormat says: dash (take-L.wav, the default), "
				"underscore (take_L.wav), dot (take.L.wav) or space (take L.wav); each goes on a new mono track of "
				"its own at position, and track and oneTrack only place the files that aren't split. copy, convert "
				"and splitChannels "
				"need a saved project. clipsOnly adds the media files and clips to the pool without tracks or events "
				"(the pool's Import Medium). One undo step. Returns the new tracks, and per file (per channel when "
				"split) the media files and events, in the order of files.",
				{{"type", "object"}, {"additionalProperties", false},
					{"properties",
						{{"files", {{"type", "array"}, {"items", {{"type", "string"}}}}}, {"track", integer},
							{"position", integer}, {"oneTrack", boolean}, {"copy", boolean}, {"convert", boolean},
							{"splitChannels", boolean},
							{"nameFormat", {{"type", "string"}, {"enum", {"dash", "underscore", "dot", "space"}}}},
							{"clipsOnly", boolean}}},
					{"required", {"files"}}},
				{{"type", "object"}, {"properties", {{"tracks", ids}, {"media", ids}, {"events", ids}, {"job", job}}},
					{"required", {"tracks", "media", "events", "job"}}},
				false},
			[this, &jobs](Project& project, CommandContext& context)
			{ return importAudioFiles(project, context, jobs); });
	}

	Json ProjectModel::importAudioFiles(Project& project, CommandContext& context, Jobs& jobs)
	{
		const Json& parameters = context.parameters();
		const auto files = parameters.at("files").get<std::vector<std::string>>();
		require(!files.empty(), "files must not be empty");
		const bool copy = parameters.value("copy", false);
		const bool convert = parameters.value("convert", false);
		const bool split = parameters.value("splitChannels", false);
		const std::string separator = separatorOf(parameters.value("nameFormat", std::string("dash")));
		const bool oneTrack = parameters.value("oneTrack", false);
		const bool clipsOnly = parameters.value("clipsOnly", false);
		ObjectId target = parameters.value("track", ObjectId{0});
		require(!clipsOnly || (target == 0 && !oneTrack), "clipsOnly adds no events: leave out track and oneTrack");
		if (target != 0)
		{
			const auto track = project.trackById.find(target);
			require(track != project.trackById.end() && track->second.type == TrackType::Audio,
				"track " + std::to_string(target) + " isn't an audio track");
		}
		const auto position = parameters.value("position", time::Samples{0});
		require(position >= 0, "position must not be negative");
		const fs::path mediaFolder = m_file.empty() ? fs::path() : mediaFolderFor(fs::u8path(m_file));
		require(!mediaFolder.empty() || (!copy && !convert && !split),
			"save the project first: imported files are copied and converted into the folder of its project file");
		// Where files are converted and split: the media folder, or for a project without a file (which only converts
		// MP3 and Ogg Vorbis files) the folder its recordings go to.
		const fs::path writeFolder = mediaFolder.empty() ? unsavedMediaFolder(m_settingsDirectory) : mediaFolder;

		const auto run = [this, &project, &context](const char* command, const Json& commandParameters)
		{
			CommandContext inner(commandParameters, &context);
			return m_edits.at(command).handler(project, inner);
		};

		Json tracks = Json::array();
		Json media = Json::array();
		Json events = Json::array();
		std::vector<MediaCopy> copies;
		std::vector<std::string> written; // the files converted or split, removed if the import fails
		try
		{
			time::Samples next = position; // in the project's sample frames
			for (const std::string& path : files)
			{
				const AudioFileInfo info = readInfo(path);
				const bool converts = convert && info.sampleRate != project.sampleRate;
				const bool splits = split && info.channels > 1;
				// Compressed files other than FLAC always become WAV files, whatever the options say.
				const bool compressed = info.format == AudioFormat::Mp3 || info.format == AudioFormat::OggVorbis;
				std::vector<std::string> paths{path};
				if (converts || splits || compressed)
					paths = convertInto(writeFolder, path, info, converts ? project.sampleRate : info.sampleRate,
						splits, separator, written);

				time::Samples length = 0;
				for (const std::string& file : paths)
				{
					const Json added = run("media.add", {{"path", file}}).at("media");
					const auto mediaId = added.at("id").get<ObjectId>();
					media.push_back(mediaId);
					if (copy && file == path && !isInside(file, mediaFolder.parent_path()))
						copies.push_back({mediaId, file, {}});
					Json clipParameters = {{"media", mediaId}};
					if (!splits)
						clipParameters["name"] = fs::u8path(path).stem().u8string(); // not a converted copy's name
					const Json clip = run("clip.add", clipParameters).at("clip");
					if (clipsOnly)
						continue;

					ObjectId track = splits ? 0 : target; // every channel of a split file on a track of its own
					if (track == 0)
					{
						Json trackParameters = {{"type", "audio"}, {"name", clip.at("name")}};
						if (const int channels = added.at("channels").get<int>(); channels <= g_maxFormatChannels)
							trackParameters["channels"] = channels;
						track = run("track.add", trackParameters).at("track").at("id").get<ObjectId>();
						tracks.push_back(track);
						if (oneTrack && !splits)
							target = track;
					}
					else if (!oneTrack)
					{
						target = 0; // only the first file goes on the given track
					}
					const time::Position at = time::rebase({time::TimeBase::Linear, splits ? position : next},
						project.trackById.at(track).timeBase, project.tempo, project.sampleRate);
					events.push_back(
						run("audioEvent.add", {{"track", track}, {"clip", clip.at("id")}, {"position", at.value}})
							.at("event")
							.at("id"));
					length = std::llround(static_cast<double>(added.at("length").get<time::Samples>()) *
						project.sampleRate / added.at("sampleRate").get<double>());
				}
				if (oneTrack && !splits)
					next += length;
			}
		}
		catch (...)
		{
			removeFiles(written);
			throw;
		}

		Json job = nullptr;
		if (!copies.empty())
			job = startMediaCopy(
				jobs, "audioFile.import", std::move(copies), mediaFolder, "Copy imported files into the project");
		return {{"tracks", std::move(tracks)}, {"media", std::move(media)}, {"events", std::move(events)},
			{"job", std::move(job)}};
	}
}
