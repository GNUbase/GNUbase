// SPDX-License-Identifier: GPL-3.0-or-later
// Track versions (#165, reference manual pp. 230-237). Like every command that changes the project, each is one undo
// step and changes nothing if it fails (ProjectModel::addEdit).
#include "engine/document/trackversions.h"

#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <algorithm>
#include <iterator>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		std::string idText(ObjectId id)
		{
			return std::to_string(id);
		}

		// The tracks of the parameters' tracks, each once, in their order; every one an audio, instrument or MIDI
		// track. Gives each the first version as a version of its own.
		std::vector<ObjectId> tracksOf(Project& project, const Json& parameters)
		{
			std::vector<ObjectId> tracks;
			for (const Json& item : parameters.at("tracks"))
			{
				const auto id = item.get<ObjectId>();
				const auto found = project.trackById.find(id);
				require(found != project.trackById.end(), "no track " + idText(id));
				require(hasVersions(found->second.type), "track " + idText(id) + " has no versions");
				if (std::find(tracks.begin(), tracks.end(), id) == tracks.end())
					tracks.push_back(id);
				if (found->second.versions.empty())
					found->second.versions.emplace_back();
			}
			require(!tracks.empty(), "tracks must not be empty");
			return tracks;
		}

		TrackVersion* versionOf(Track& track, int id)
		{
			const auto found = std::find_if(track.versions.begin(), track.versions.end(),
				[id](const TrackVersion& version) { return version.id == id; });
			return found != track.versions.end() ? &*found : nullptr;
		}

		TrackVersion& active(Track& track)
		{
			return *versionOf(track, track.activeVersion);
		}

		// An ID that no version of any track has: versions made together get one.
		int newVersionId(const Project& project)
		{
			int highest = 1; // every track's first version
			for (const auto& [id, track] : project.trackById)
				for (const TrackVersion& version : track.versions)
					highest = std::max(highest, version.id);
			return highest + 1;
		}

		// vN, N one more than the number of the track's versions, or the next number none of their names has.
		std::string defaultName(const Track& track)
		{
			const auto taken = [&track](const std::string& name)
			{
				return std::any_of(track.versions.begin(), track.versions.end(),
					[&name](const TrackVersion& version) { return version.name == name; });
			};
			for (size_t number = track.versions.size() + 1;; ++number)
				if (const std::string name = "v" + std::to_string(number); !taken(name))
					return name;
		}

		// Moves the track's events and parts out of the project into its active version, which holds them while
		// another one is active. Its MIDI parts are no longer shared copies of parts that play.
		void stash(Project& project, Track& track, CommandContext& context)
		{
			TrackVersion& version = active(track);
			for (auto event = project.audioEvents.begin(); event != project.audioEvents.end();)
				if (event->second.track == track.id)
				{
					context.emit("audioEvent.removed", {{"event", event->first}});
					version.audioEvents.push_back(std::move(event->second));
					event = project.audioEvents.erase(event);
				}
				else
					++event;
			for (auto part = project.audioParts.begin(); part != project.audioParts.end();)
				if (part->second.track == track.id)
				{
					context.emit("audioPart.removed", {{"part", part->first}});
					version.audioParts.push_back(std::move(part->second));
					part = project.audioParts.erase(part);
				}
				else
					++part;
			for (auto part = project.midiParts.begin(); part != project.midiParts.end();)
				if (part->second.track == track.id)
				{
					context.emit("midiPart.removed", {{"part", part->first}});
					part->second.shared = 0;
					version.midiParts.push_back(std::move(part->second));
					part = project.midiParts.erase(part);
				}
				else
					++part;
		}

		// Makes a version of the track active: its events and parts go into the project. The active one's must be
		// stashed or gone.
		void unstash(Project& project, Track& track, int id, CommandContext& context)
		{
			track.activeVersion = id;
			TrackVersion& version = active(track);
			for (AudioPart& part : version.audioParts)
			{
				context.emit("audioPart.added", {{"part", toJson(part)}});
				project.audioParts[part.id] = std::move(part);
			}
			for (AudioEvent& event : version.audioEvents)
			{
				context.emit("audioEvent.added", {{"event", toJson(event)}});
				project.audioEvents[event.id] = std::move(event);
			}
			for (MidiPart& part : version.midiParts)
			{
				context.emit("midiPart.added", {{"part", toJson(part)}});
				const ObjectId partId = part.id;
				project.midiParts[partId] = std::move(part);
			}
			version.audioEvents.clear();
			version.audioParts.clear();
			version.midiParts.clear();
		}

		void changed(const Project& project, ObjectId track, CommandContext& context)
		{
			context.emit("track.changed", {{"track", toJson(project.trackById.at(track))}});
		}

		Json versionsResult(const Project& project, const std::vector<ObjectId>& tracks)
		{
			Json result = Json::array();
			for (const ObjectId id : tracks)
				result.push_back(toJson(project.trackById.at(id)));
			return {{"tracks", std::move(result)}};
		}

		// New Version and Duplicate Version: a new version, active, with one ID on every track.
		Json addVersions(Project& project, CommandContext& context, bool duplicate)
		{
			const std::vector<ObjectId> tracks = tracksOf(project, context.parameters());
			const int id = newVersionId(project);
			for (const ObjectId trackId : tracks)
			{
				Track& track = project.trackById.at(trackId);
				TrackVersion version;
				version.id = id;
				version.name = context.parameters().value("name", defaultName(track));
				require(!version.name.empty(), "name must not be empty");
				stash(project, track, context);
				if (duplicate)
				{
					TrackVersion copy = copyVersion(project, active(track), trackId);
					version.audioEvents = std::move(copy.audioEvents);
					version.audioParts = std::move(copy.audioParts);
					version.midiParts = std::move(copy.midiParts);
				}
				track.versions.push_back(std::move(version));
				unstash(project, track, id, context);
				changed(project, trackId, context);
			}
			return versionsResult(project, tracks);
		}

		Json renameVersions(Project& project, CommandContext& context)
		{
			const std::string name = context.parameters().at("name").get<std::string>();
			require(!name.empty(), "name must not be empty");
			const std::vector<ObjectId> tracks = tracksOf(project, context.parameters());
			for (const ObjectId id : tracks)
			{
				active(project.trackById.at(id)).name = name;
				changed(project, id, context);
			}
			return versionsResult(project, tracks);
		}

		// Delete Version: the active version goes with its events and parts, and the one before it becomes active (the
		// next one for the first). Tracks with only one version keep it.
		Json removeVersions(Project& project, CommandContext& context)
		{
			std::vector<ObjectId> removed;
			for (const ObjectId id : tracksOf(project, context.parameters()))
			{
				Track& track = project.trackById.at(id);
				if (track.versions.size() < 2)
					continue;
				stash(project, track, context); // into the version that goes
				const auto place = std::find_if(track.versions.begin(), track.versions.end(),
					[&track](const TrackVersion& version) { return version.id == track.activeVersion; });
				const auto next = track.versions.erase(place);
				const int nextId = (next == track.versions.begin() ? *next : *std::prev(next)).id;
				unstash(project, track, nextId, context);
				if (track.versions.size() == 1 && track.versions.front() == TrackVersion())
					track.versions.clear(); // back to the first version only
				removed.push_back(id);
				changed(project, id, context);
			}
			require(!removed.empty(), "no track has a version to delete: each has only one");
			return versionsResult(project, removed);
		}

		// Activates the version with an ID on those of the tracks that have one.
		Json activateVersions(Project& project, CommandContext& context)
		{
			const int id = context.parameters().at("version").get<int>();
			std::vector<ObjectId> activated;
			for (const ObjectId trackId : tracksOf(project, context.parameters()))
			{
				Track& track = project.trackById.at(trackId);
				if (versionOf(track, id) == nullptr)
					continue;
				activated.push_back(trackId);
				if (track.activeVersion == id)
					continue;
				stash(project, track, context);
				unstash(project, track, id, context);
				changed(project, trackId, context);
			}
			require(!activated.empty(), "none of the tracks has a version with ID " + std::to_string(id));
			return versionsResult(project, activated);
		}

		Json assignCommonId(Project& project, CommandContext& context)
		{
			const std::vector<ObjectId> tracks = tracksOf(project, context.parameters());
			const int id = newVersionId(project);
			for (const ObjectId trackId : tracks)
			{
				Track& track = project.trackById.at(trackId);
				active(track).id = id;
				track.activeVersion = id;
				changed(project, trackId, context);
			}
			return versionsResult(project, tracks);
		}
	}

	TrackVersion copyVersion(Project& project, const TrackVersion& version, ObjectId track)
	{
		TrackVersion copy;
		copy.id = version.id;
		copy.name = version.name;
		std::map<ObjectId, ObjectId> parts;
		for (AudioPart part : version.audioParts)
		{
			part.track = track;
			parts[part.id] = project.newId();
			part.id = parts.at(part.id);
			copy.audioParts.push_back(std::move(part));
		}
		// New ids in stack order keep it, with every copy ordered by its id.
		std::vector<AudioEvent> events = version.audioEvents;
		std::stable_sort(events.begin(), events.end(),
			[](const AudioEvent& a, const AudioEvent& b) { return frontOf(a) < frontOf(b); });
		std::map<ObjectId, ObjectId> ids;
		for (const AudioEvent& event : events)
			ids[event.id] = project.newId();
		for (AudioEvent& event : events)
		{
			event.id = ids.at(event.id);
			event.track = track;
			event.front = 0;
			const auto from = ids.find(event.crossfade.from);
			event.crossfade.from = from == ids.end() ? 0 : from->second;
			const auto part = parts.find(event.part);
			event.part = part == parts.end() ? 0 : part->second;
		}
		copy.audioEvents = std::move(events);
		for (MidiPart part : version.midiParts)
		{
			part.id = project.newId();
			part.track = track;
			if (part.shared == 0)
				renumber(project, part);
			copy.midiParts.push_back(std::move(part));
		}
		return copy;
	}

	void registerTrackVersionCommands(ProjectModel& model, Engine& engine)
	{
		const Json idSchema = {{"type", "integer"}};
		const Json tracksSchema = {{"type", "array"}, {"items", idSchema}, {"minItems", 1}};
		const Json nameSchema = {{"type", "string"}, {"minLength", 1}};
		const auto schema = [](Json properties, std::vector<std::string> required)
		{
			return Json{{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)},
				{"additionalProperties", false}};
		};
		const Json result = schema({{"tracks", {{"type", "array"}, {"items", {{"type", "object"}}}}}}, {"tracks"});
		const std::string tracks =
			"tracks: audio, instrument and MIDI tracks; each has its first version, v1 with ID 1, until it gets more "
			"(versions and activeVersion in project.get). ";
		const std::string returns = " Returns the tracks with their versions; emits track.changed for each, and the "
									"events of the events and parts that come and go.";

		model.addEdit(engine,
			{"trackVersion.add", 1,
				"New Version (reference manual p. 232): gives each of the tracks a new, empty version and activates "
				"it; the events and parts of the version that was active stay with it. The new versions share a new "
				"ID; each is named vN (N one more than the track's number of versions, or the next number none of its "
				"versions' names has) unless name is given. " +
					tracks + returns,
				schema({{"tracks", tracksSchema}, {"name", nameSchema}}, {"tracks"}), result, false},
			"New Version",
			[](Project& project, CommandContext& context) { return addVersions(project, context, false); });
		model.addEdit(engine,
			{"trackVersion.duplicate", 1,
				"Duplicate Version (p. 234): gives each of the tracks a new version with a copy of the active one's "
				"events and parts, named and with an ID as trackVersion.add gives them, and activates it. " +
					tracks + returns,
				schema({{"tracks", tracksSchema}, {"name", nameSchema}}, {"tracks"}), result, false},
			"Duplicate Version",
			[](Project& project, CommandContext& context) { return addVersions(project, context, true); });
		model.addEdit(engine,
			{"trackVersion.set", 1,
				"Rename Version (p. 236): names the active version of each of the tracks. " + tracks + returns,
				schema({{"tracks", tracksSchema}, {"name", nameSchema}}, {"tracks", "name"}), result, false},
			"Rename Version", renameVersions);
		model.addEdit(engine,
			{"trackVersion.remove", 1,
				"Delete Version (p. 235): deletes the active version of each of the tracks that have more than one, "
				"with its events and parts, and activates the version before it (the next one for the first). Fails "
				"if every track has only one version. " +
					tracks +
					" Returns the tracks that lost a version, with their versions; emits track.changed for "
					"each, and the events of the events and parts that come and go.",
				schema({{"tracks", tracksSchema}}, {"tracks"}), result, false},
			"Delete Version", removeVersions);
		model.addEdit(engine,
			{"trackVersion.activate", 1,
				"Activates the version with the ID version on each of the tracks that has one (pp. 233-234): its "
				"events and parts play and show, while those of the version that was active stay with that one. Fails "
				"if none of the tracks has such a version. " +
					tracks +
					" Returns the tracks with such a version, with their versions; emits track.changed for each that "
					"changed, and the events of the events and parts that come and go.",
				schema({{"tracks", tracksSchema}, {"version", {{"type", "integer"}, {"minimum", 1}}}},
					{"tracks", "version"}),
				result, false},
			"Activate Version", activateVersions);
		model.addEdit(engine,
			{"trackVersion.assignCommonId", 1,
				"Assign Common Version ID (p. 233): gives the active versions of the tracks a new ID they share, so "
				"that trackVersion.activate activates them together. " +
					tracks + returns,
				schema({{"tracks", tracksSchema}}, {"tracks"}), result, false},
			"Assign Common Version ID", assignCommonId);
	}
}
