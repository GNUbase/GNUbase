// SPDX-License-Identifier: GPL-3.0-or-later
// Render in place and freeze (#116, docs/features/render-in-place.md): track.renderInPlace, track.freeze and
// track.unfreeze. Both render with mixdown.export, from a point inside the channel's strip, before they change the
// project, so a render is bit-identical to an export of the same range; then they change it in one undo step.
#include "engine/document/channelformat.h"
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/rendersnapshot.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/media/backup.h"
#include "engine/time/tempomap.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>
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

		// Where what plays on a channel starts and ends, in the project's samples: its events and notes, and on an
		// instrument channel the notes of the MIDI channels that play it.
		struct Span
		{
			std::optional<time::Samples> start;
			time::Samples end = 0;
		};

		Span spanOf(const RenderSnapshot& snapshot, ObjectId channel)
		{
			Span span;
			const auto add = [&span](time::Samples start, time::Samples end)
			{
				span.start = std::min(span.start.value_or(start), start);
				span.end = std::max(span.end, end);
			};
			for (const RenderChannel& candidate : snapshot.channels)
			{
				const bool plays = candidate.output.kind == RenderTarget::Kind::Channel &&
					candidate.type == TrackType::Midi &&
					snapshot.channels[static_cast<size_t>(candidate.output.index)].id == channel;
				if (candidate.id != channel && !plays)
					continue;
				for (const RenderAudioEvent& event : candidate.audioEvents)
				{
					const double ratio =
						snapshot.sampleRate / snapshot.media[static_cast<size_t>(event.media)].sampleRate;
					add(event.start,
						event.start +
							static_cast<time::Samples>(std::llround(static_cast<double>(event.length) * ratio)));
				}
				for (const RenderNote& note : candidate.notes)
					add(note.start, note.end);
			}
			return span;
		}

		const Track& renderable(const Project& project, ObjectId id)
		{
			const auto track = project.trackById.find(id);
			require(track != project.trackById.end(), "there is no track " + std::to_string(id));
			require(track->second.type == TrackType::Audio || track->second.type == TrackType::Instrument,
				"only audio and instrument tracks render; track " + std::to_string(id) + " is neither");
			return track->second;
		}

		std::vector<ObjectId> trackList(const Json& parameters)
		{
			auto ids = parameters.at("tracks").get<std::vector<ObjectId>>();
			require(!ids.empty(), "tracks must not be empty");
			std::vector<ObjectId> sorted = ids;
			std::sort(sorted.begin(), sorted.end());
			require(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end(), "tracks names a track twice");
			return ids;
		}

		// A file name from a track's name.
		std::string fileNameOf(const Track& track)
		{
			return track.name.empty() ? "Track " + std::to_string(track.id) : track.name;
		}

		// Renders a channel's [start, end) from point into a 32-bit float WAV file in folder; returns the file.
		Json renderChannel(CommandContext& context, const fs::path& folder, const std::string& name, ObjectId channel,
			time::Samples start, time::Samples end, const char* point, double tail, const std::string& sampleFormat)
		{
			const Json result = context.call("mixdown.export",
				{{"folder", folder.u8string()}, {"name", name}, {"channels", {channel}}, {"start", start}, {"end", end},
					{"point", point}, {"tail", tail}, {"sampleFormat", sampleFormat}});
			return result.at("files").at(0);
		}
	}

	void ProjectModel::registerRenderCommands(Engine& engine)
	{
		const Json integer = {{"type", "integer"}};
		const Json ids = {{"type", "array"}, {"items", integer}};
		const Json tail = {{"type", "number"}, {"minimum", 0}, {"maximum", 60}};
		// Freeze files go into the Freeze folder in the project's folder (reference manual p. 220), or for a project
		// without a file into the folder its recordings go to.
		const auto freezeFolder = [this]
		{
			return (m_file.empty() ? unsavedMediaFolder(m_settingsDirectory)
								   : fs::absolute(fs::u8path(m_file)).parent_path()) /
				"Freeze";
		};

		addEdit(engine,
			{"track.freeze", 1,
				"Freezes audio and instrument tracks (reference manual pp. 219-220): renders what each plays after its "
				"plugin slots, before the strip's other sections (the instrument, the events, the pre section and the "
				"inserts), offline from the project's start to the end of its last event or note, with up to tail "
				"seconds more (0 to 60, default 30) that end after the last sample a 24-bit file doesn't hold as "
				"silence, into a 32-bit float WAV file named after the track in the Freeze folder of the project's "
				"folder (for a project without a file, of the folder its recordings go to). The track then plays the "
				"file from the project's start instead of its events, parts, instrument, pre section and slots, which "
				"stay as they are; the strip's other sections, the fader, pan, mute and sends still play, with their "
				"automation. Renders as mixdown.export does, and is refused when it is, for a frozen or disabled "
				"track and for one with nothing to play. One undo step. Returns the tracks.",
				{{"type", "object"}, {"additionalProperties", false}, {"properties", {{"tracks", ids}, {"tail", tail}}},
					{"required", {"tracks"}}},
				{{"type", "object"}, {"properties", {{"tracks", {{"type", "array"}}}}}, {"required", {"tracks"}}},
				false},
			"Freeze",
			[this, freezeFolder](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const std::vector<ObjectId> tracks = trackList(parameters);
				const double seconds = parameters.value("tail", 30.0);
				const RenderSnapshot& snapshot = m_snapshots.latest();
				std::vector<Span> spans;
				for (const ObjectId id : tracks)
				{
					const Track& track = renderable(project, id);
					require(!track.frozen, track.name + " is frozen already");
					require(track.enabled, track.name + " is disabled");
					spans.push_back(spanOf(snapshot, track.channel));
					require(spans.back().start.has_value(), track.name + " has nothing to freeze");
				}
				std::vector<std::string> written;
				std::vector<Json> files;
				try
				{
					for (size_t i = 0; i < tracks.size(); ++i)
					{
						const Track& track = project.trackById.at(tracks[i]);
						files.push_back(renderChannel(context, freezeFolder(), fileNameOf(track), track.channel, 0,
							spans[i].end, "inserts", seconds, "float32"));
						written.push_back(files.back().at("path").get<std::string>());
					}
				}
				catch (...)
				{
					removeFiles(written);
					throw;
				}
				Json result = Json::array();
				for (size_t i = 0; i < tracks.size(); ++i)
				{
					Track& track = project.trackById.at(tracks[i]);
					track.frozen = Freeze{files[i].at("path").get<std::string>(), project.sampleRate,
						channelCount(project.channels.at(track.channel).format),
						files[i].at("frames").get<time::Samples>()};
					result.push_back(toJson(track));
					context.emit("track.changed", {{"track", result.back()}});
				}
				return Json{{"tracks", std::move(result)}};
			});

		addEdit(engine,
			{"track.unfreeze", 1,
				"Unfreezes frozen tracks: they play their events, instrument and plugin slots again. Their freeze "
				"files stay, so the step can be undone. One undo step. Returns the tracks.",
				{{"type", "object"}, {"additionalProperties", false}, {"properties", {{"tracks", ids}}},
					{"required", {"tracks"}}},
				{{"type", "object"}, {"properties", {{"tracks", {{"type", "array"}}}}}, {"required", {"tracks"}}},
				false},
			"Unfreeze",
			[](Project& project, CommandContext& context)
			{
				const std::vector<ObjectId> tracks = trackList(context.parameters());
				for (const ObjectId id : tracks)
				{
					const Track& track = renderable(project, id);
					require(track.frozen.has_value(), track.name + " isn't frozen");
				}
				Json result = Json::array();
				for (const ObjectId id : tracks)
				{
					Track& track = project.trackById.at(id);
					track.frozen.reset();
					result.push_back(toJson(track));
					context.emit("track.changed", {{"track", result.back()}});
				}
				return Json{{"tracks", std::move(result)}};
			});

		const Json processing = {{"type", "string"}, {"enum", {"dry", "channelSettings", "completeSignalPath"}}};
		addEdit(engine,
			{"track.renderInPlace", 1,
				"Renders audio and instrument tracks in place (reference manual pp. 1471-1477): what each plays from "
				"start to end, in project samples (a range selection; default: from the start of its first event or "
				"note to the end of its last), with up to tail seconds more (0 to 60, default 0) that end after the "
				"last sample a 24-bit file doesn't hold as silence, into a WAV file (sampleFormat as in "
				"mixdown.export, default float32) in the project's media folder (for a project without a file, the "
				"folder its recordings go to), named name or after the track, and puts it as one event at start on a "
				"new audio track below the track, named after it with (Rendered), which plays through the same "
				"output. processing: dry renders what plays on the channel, before its pre section and plugin slots, "
				"and the new track gets the channel's slots, strip, fader, pan and sends; channelSettings (the "
				"default) renders after the slots and the strip, before the fader, and the new track gets the fader, "
				"pan and sends; completeSignalPath renders after the fader and panner, in the output's format, and "
				"the new track gets no settings. sourceTracks: keep (the default), mute, disable or remove the "
				"tracks rendered. Renders as mixdown.export does, and is refused when it is. One undo step. Returns "
				"the new tracks, media files and events, in the order of tracks.",
				{{"type", "object"}, {"additionalProperties", false},
					{"properties",
						{{"tracks", ids}, {"start", {{"type", "integer"}, {"minimum", 0}}},
							{"end", {{"type", "integer"}, {"minimum", 0}}}, {"processing", processing}, {"tail", tail},
							{"sampleFormat",
								{{"type", "string"}, {"enum", {"int16", "int24", "int32", "float32", "float64"}}}},
							{"sourceTracks", {{"type", "string"}, {"enum", {"keep", "mute", "disable", "remove"}}}},
							{"name", {{"type", "string"}, {"minLength", 1}}}}},
					{"required", {"tracks"}}},
				{{"type", "object"}, {"properties", {{"tracks", ids}, {"media", ids}, {"events", ids}}},
					{"required", {"tracks", "media", "events"}}},
				false},
			"Render in place",
			[this](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const std::vector<ObjectId> tracks = trackList(parameters);
				require(parameters.contains("start") == parameters.contains("end"), "give both start and end, or none");
				const std::string mode = parameters.value("processing", std::string("channelSettings"));
				const char* point = "output";
				if (mode == "dry")
					point = "source";
				else if (mode == "channelSettings")
					point = "strip";
				const RenderSnapshot& snapshot = m_snapshots.latest();
				std::vector<Span> spans;
				for (const ObjectId id : tracks)
				{
					const Track& track = renderable(project, id);
					// A freeze file is already through the pre section and the slots that dry copies onto the new
					// track.
					require(mode != "dry" || !track.frozen, track.name + " is frozen: unfreeze it to render it dry");
					Span span = spanOf(snapshot, track.channel);
					if (parameters.contains("start"))
						span = {parameters.at("start").get<time::Samples>(), parameters.at("end").get<time::Samples>()};
					require(span.start.has_value(), track.name + " has nothing to render");
					require(span.end > *span.start, "the range is empty: end must be after start");
					spans.push_back(span);
				}
				const fs::path folder =
					m_file.empty() ? unsavedMediaFolder(m_settingsDirectory) : mediaFolderFor(fs::u8path(m_file));
				const auto run = [this, &project, &context](const char* command, const Json& commandParameters)
				{
					CommandContext inner(commandParameters, &context);
					return m_edits.at(command).handler(project, inner);
				};

				std::vector<std::string> written;
				Json added = Json::array();
				Json media = Json::array();
				Json events = Json::array();
				try
				{
					std::vector<Json> files;
					for (size_t i = 0; i < tracks.size(); ++i)
					{
						const Track& track = project.trackById.at(tracks[i]);
						files.push_back(renderChannel(context, folder, parameters.value("name", fileNameOf(track)),
							track.channel, *spans[i].start, spans[i].end, point, parameters.value("tail", 0.0),
							parameters.value("sampleFormat", std::string("float32"))));
						written.push_back(files.back().at("path").get<std::string>());
					}
					for (size_t i = 0; i < tracks.size(); ++i)
					{
						const Track& source = project.trackById.at(tracks[i]);
						const Channel from = project.channels.at(source.channel);
						const std::vector<ObjectId>& siblings =
							source.parent == 0 ? project.tracks : project.trackById.at(source.parent).children;
						const Json mediaFile = run("media.add", {{"path", written[i]}}).at("media");
						Json trackParameters = {{"type", "audio"}, {"name", source.name + " (Rendered)"},
							{"timeBase", timeBaseName(source.timeBase)}, {"parent", source.parent},
							{"index",
								std::distance(
									siblings.begin(), std::find(siblings.begin(), siblings.end(), source.id)) +
									1}};
						if (mode == "completeSignalPath")
							trackParameters["channels"] = mediaFile.at("channels");
						else
							trackParameters["format"] = formatName(from.format);
						if (from.output != 0)
							trackParameters["output"] = from.output;
						const auto track = run("track.add", trackParameters).at("track").at("id").get<ObjectId>();
						const auto clip =
							run("clip.add", {{"media", mediaFile.at("id")}}).at("clip").at("id").get<ObjectId>();
						const time::Position at = time::rebase({time::TimeBase::Linear, *spans[i].start},
							project.trackById.at(track).timeBase, project.tempo, project.sampleRate);
						events.push_back(
							run("audioEvent.add", {{"track", track}, {"clip", clip}, {"position", at.value}})
								.at("event")
								.at("id"));
						added.push_back(track);
						media.push_back(mediaFile.at("id"));

						// The settings that weren't rendered.
						Channel& channel = project.channels.at(project.trackById.at(track).channel);
						if (mode != "completeSignalPath")
						{
							channel.sends = from.sends;
							channel.volume = from.volume;
							channel.pan = from.pan;
							channel.panner = from.panner;
							channel.panLeft = from.panLeft;
							channel.panRight = from.panRight;
							channel.panBypass = from.panBypass;
						}
						std::vector<ObjectId> slots;
						if (mode == "dry")
						{
							channel.sections = from.sections;
							for (const ObjectId id : from.slots)
							{
								Slot slot = project.slots.at(id);
								slot.id = project.newId();
								slot.channel = channel.id;
								channel.slots.push_back(slot.id);
								slots.push_back(slot.id);
								project.slots[slot.id] = std::move(slot);
							}
						}
						context.emit("channel.changed", {{"channel", toJson(channel)}});
						for (const ObjectId slot : slots)
							context.emit("plugin.added", {{"slot", describeSlot(project, slot)}});
					}
				}
				catch (...)
				{
					removeFiles(written);
					throw;
				}

				const std::string sources = parameters.value("sourceTracks", std::string("keep"));
				for (const ObjectId id : tracks)
				{
					if (sources == "mute")
						run("channel.set", {{"channel", project.trackById.at(id).channel}, {"mute", true}});
					else if (sources == "disable")
						run("track.set", {{"track", id}, {"enabled", false}});
					else if (sources == "remove")
						run("track.remove", {{"track", id}});
				}
				return Json{{"tracks", std::move(added)}, {"media", std::move(media)}, {"events", std::move(events)}};
			});
	}
}
