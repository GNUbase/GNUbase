// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/trackvisibility.h"

#include "engine/document/mixerviews.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		constexpr std::array<const char*, 5> g_syncs = {"", "window1", "window2", "window3", "window4"};

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		// The ids that are tracks of the project.
		std::vector<ObjectId> existing(const Project& project, std::vector<ObjectId> ids)
		{
			ids.erase(std::remove_if(
						  ids.begin(), ids.end(), [&project](ObjectId id) { return project.trackById.count(id) == 0; }),
				ids.end());
			return ids;
		}

		// Track ids from the parameters, sorted and once each; throws for an id that is no track.
		std::vector<ObjectId> tracksFrom(const Project& project, const Json& json)
		{
			auto ids = json.get<std::vector<ObjectId>>();
			for (const ObjectId id : ids)
				require(project.trackById.count(id) != 0, "no track " + std::to_string(id) + " in the project");
			std::sort(ids.begin(), ids.end());
			ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
			return ids;
		}

		// The ids with the tracks inside the folders among them, nested too, sorted and once each: a hidden folder
		// hides them all in the track list, and the mixers that follow hide their channels with it (#906).
		std::vector<ObjectId> withInside(const Project& project, std::vector<ObjectId> ids)
		{
			const std::set<ObjectId> given(ids.begin(), ids.end());
			std::vector<ObjectId> pending(given.begin(), given.end());
			std::set<ObjectId> all = given;
			while (!pending.empty())
			{
				const ObjectId id = pending.back();
				pending.pop_back();
				const auto track = project.trackById.find(id);
				if (track == project.trackById.end())
					continue;
				for (const ObjectId child : track->second.children)
					if (all.insert(child).second)
						pending.push_back(child);
			}
			return {all.begin(), all.end()};
		}

		const VisibilityConfiguration* active(const TrackVisibility& visibility)
		{
			for (const VisibilityConfiguration& configuration : visibility.configurations)
				if (configuration.name == visibility.configuration)
					return &configuration;
			return nullptr;
		}

		// As trackVisibility.get returns it: without the tracks that are gone, and whether the tracks hidden differ
		// from the active configuration's.
		Json visibilityJson(const Project& project)
		{
			TrackVisibility visibility = project.trackVisibility;
			visibility.hidden = existing(project, visibility.hidden);
			for (VisibilityConfiguration& configuration : visibility.configurations)
				configuration.hidden = existing(project, configuration.hidden);
			const VisibilityConfiguration* configuration = active(visibility);
			Json json = toJson(visibility);
			json["modified"] = configuration != nullptr && configuration->hidden != visibility.hidden;
			return json;
		}

		// Gives the mixers that follow the track visibility their state: a track's channel is hidden there while
		// the track is. Busses keep theirs. Returns the mixers whose view changed.
		std::vector<std::string> syncMixerViews(Project& project)
		{
			const std::set<ObjectId> tracks(
				project.trackVisibility.hidden.begin(), project.trackVisibility.hidden.end());
			std::vector<std::string> changed;
			for (const std::string& mixer : {std::string("lower"), project.trackVisibility.sync})
			{
				if (mixer.empty())
					continue;
				// A mixer without a view yet gets one only for channels to hide, so a load adds none to the project.
				const auto view = project.mixers.find(mixer);
				const std::vector<ObjectId> before =
					view != project.mixers.end() ? view->second.hidden : std::vector<ObjectId>();
				std::vector<ObjectId> hidden;
				for (const ObjectId id : before)
					if (project.channels.count(id) == 0)
						hidden.push_back(id);
				for (const auto& [id, channel] : project.channels)
					if (tracks.count(channel.track) != 0)
						hidden.push_back(id);
				std::sort(hidden.begin(), hidden.end());
				if (hidden == before)
					continue;
				project.mixers[mixer].hidden = std::move(hidden);
				changed.push_back(mixer);
			}
			return changed;
		}

		// syncMixerViews with mixer.changed for each view that changed. For silent, the view changes without it:
		// mixer.set, which called followMixer, emits the view itself right after (#906).
		void syncMixers(Project& project, CommandContext& context, const std::string& silent = {})
		{
			for (const std::string& mixer : syncMixerViews(project))
				if (mixer != silent)
					context.emit("mixer.changed", mixerViewJson(project, mixer));
		}
	}

	Json trackLayoutJson(const Project& project)
	{
		TrackLayout layout;
		for (const auto& [track, height] : project.trackLayout.heights)
			if (project.trackById.count(track) != 0)
				layout.heights[track] = height;
		layout.folded = existing(project, project.trackLayout.folded);
		return toJson(layout);
	}

	void followMixer(Project& project, const std::string& mixer, CommandContext& context)
	{
		TrackVisibility& visibility = project.trackVisibility;
		if (mixer != "lower" && mixer != visibility.sync)
			return;
		const std::vector<ObjectId>& channels = project.mixers[mixer].hidden;
		const auto shows = [&channels](ObjectId channel)
		{ return std::find(channels.begin(), channels.end(), channel) == channels.end(); };
		std::set<ObjectId> hidden(visibility.hidden.begin(), visibility.hidden.end());
		// A hidden track whose channel shows again shows with the tracks inside it: those of a folder whose group
		// channel shows again, though their channels are still hidden in the mixer, as the folder hid them (#1366).
		std::vector<ObjectId> shown;
		for (const auto& [id, channel] : project.channels)
			if (shows(id) && hidden.count(channel.track) != 0)
				shown.push_back(channel.track);
		shown = withInside(project, std::move(shown));
		for (const auto& [id, channel] : project.channels)
			if (!shows(id) && !std::binary_search(shown.begin(), shown.end(), channel.track))
				hidden.insert(channel.track);
			else
				hidden.erase(channel.track);
		for (const ObjectId id : shown)
			hidden.erase(id);
		std::vector<ObjectId> tracks = withInside(project, {hidden.begin(), hidden.end()});
		if (tracks != visibility.hidden)
		{
			visibility.hidden = std::move(tracks);
			context.emit("trackVisibility.changed", visibilityJson(project));
		}
		// Also without a change of the tracks: a channel of a track inside a folder that is hidden still shows in
		// the mixer the change came from, which the tracks hide again (#906). mixer.set emits the view itself.
		syncMixers(project, context, mixer);
	}

	void sanitizeTrackVisibility(Project& project)
	{
		TrackVisibility& visibility = project.trackVisibility;
		// An unknown sync would make syncMixers a mixer view of its own, which the project would then save (#906).
		if (std::find(g_syncs.begin(), g_syncs.end(), visibility.sync) == g_syncs.end())
			visibility.sync.clear();
		std::vector<VisibilityConfiguration> configurations;
		std::set<std::string> names;
		for (VisibilityConfiguration& configuration : visibility.configurations)
			if (!configuration.name.empty() && names.insert(configuration.name).second)
			{
				configuration.hidden = withInside(project, existing(project, std::move(configuration.hidden)));
				configurations.push_back(std::move(configuration));
			}
		visibility.configurations = std::move(configurations);
		if (active(visibility) == nullptr)
			visibility.configuration.clear();
		visibility.hidden = withInside(project, existing(project, std::move(visibility.hidden)));
		// The mixers that follow hide the channels of the tracks that joined from their folders, as after
		// trackVisibility.set (#1366). The project's load announces the whole project, its mixer views too.
		syncMixerViews(project);
	}

	void registerTrackVisibilityCommands(ProjectModel& model, Engine& engine)
	{
		const Json idsSchema = {{"type", "array"}, {"items", {{"type", "integer"}}}};
		const Json syncSchema = {
			{"type", "string"}, {"enum", std::vector<std::string>(g_syncs.begin(), g_syncs.end())}};
		const Json configurationsSchema = {{"type", "array"},
			{"items",
				{{"type", "object"}, {"properties", {{"name", {{"type", "string"}}}, {"hidden", idsSchema}}},
					{"required", {"name", "hidden"}}, {"additionalProperties", false}}}};
		const Json resultSchema = {{"type", "object"},
			{"properties",
				{{"hidden", idsSchema}, {"sync", syncSchema}, {"configurations", configurationsSchema},
					{"configuration", {{"type", "string"}}}, {"modified", {{"type", "boolean"}}}}},
			{"required", {"hidden", "sync", "configurations", "configuration", "modified"}}};
		const std::string text =
			"hidden: the tracks (by id) the project window's track list and event display don't show; the tracks "
			"inside a folder in it hide with it, nested ones too, and join the list. sync: the mixer window (window1 "
			"to window4) whose channel visibility follows the tracks', and back (Sync Visibility of Project and "
			"Mixer), or empty; the lower zone's mixer always follows, so a hidden track's channel is hidden in it, "
			"and a channel hidden there hides its track. "
			"configurations: the track visibility configurations, each a name and its hidden tracks; configuration: "
			"the active one's name, or empty; modified: whether the hidden tracks differ from the active "
			"configuration's. Tracks that are gone are left out.";

		engine.registerCommand(
			{"trackVisibility.get", 1, "What the project window hides (reference manual pp. 58, 80-82). " + text,
				{{"type", "object"}, {"additionalProperties", false}}, resultSchema, true},
			[&model](CommandContext&) { return visibilityJson(model.project()); });

		model.addEdit(engine,
			{"trackVisibility.set", 1,
				"Changes what the project window hides; what isn't given stays. " + text +
					" configurations replaces the list; names are unique and not empty. Without the active one in it, "
					"none is active. configuration makes one active and, without hidden, hides its tracks, with the "
					"tracks that joined its folders since, which become its own. A setting, not an undo step, saved "
					"with the project. Emits trackVisibility.changed, and mixer.changed for the mixers that follow.",
				{{"type", "object"},
					{"properties",
						{{"hidden", idsSchema}, {"sync", syncSchema}, {"configurations", configurationsSchema},
							{"configuration", {{"type", "string"}}}}},
					{"additionalProperties", false}},
				resultSchema, false},
			"Change track visibility",
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				TrackVisibility& visibility = project.trackVisibility;
				if (parameters.contains("configurations"))
				{
					std::vector<VisibilityConfiguration> configurations;
					std::set<std::string> names;
					for (const Json& item : parameters.at("configurations"))
					{
						const auto name = item.at("name").get<std::string>();
						require(!name.empty(), "a configuration needs a name");
						require(names.insert(name).second, "two configurations are named " + name);
						configurations.push_back({name, withInside(project, tracksFrom(project, item.at("hidden")))});
					}
					visibility.configurations = std::move(configurations);
					if (active(visibility) == nullptr)
						visibility.configuration.clear();
				}
				if (parameters.contains("configuration"))
				{
					visibility.configuration = parameters.at("configuration").get<std::string>();
					const auto configuration = std::find_if(visibility.configurations.begin(),
						visibility.configurations.end(), [&visibility](const VisibilityConfiguration& item)
						{ return item.name == visibility.configuration; });
					require(visibility.configuration.empty() || configuration != visibility.configurations.end(),
						"no configuration " + visibility.configuration);
					// Applied, the tracks that joined its folders since are the configuration's too, so that it doesn't
					// show as modified right after (#1366).
					if (configuration != visibility.configurations.end() && !parameters.contains("hidden"))
					{
						configuration->hidden = withInside(project, existing(project, configuration->hidden));
						visibility.hidden = configuration->hidden;
					}
				}
				if (parameters.contains("hidden"))
					visibility.hidden = withInside(project, tracksFrom(project, parameters.at("hidden")));
				if (parameters.contains("sync"))
					visibility.sync = parameters.at("sync").get<std::string>();
				syncMixers(project, context);
				Json result = visibilityJson(project);
				context.emit("trackVisibility.changed", result);
				return result;
			});
	}

	void registerTrackLayoutCommands(ProjectModel& model, Engine& engine)
	{
		const Json heightsSchema = {{"type", "array"},
			{"items",
				{{"type", "object"},
					{"properties",
						{{"track", {{"type", "integer"}}},
							{"height",
								{{"type", "number"}, {"minimum", g_minTrackHeight}, {"maximum", g_maxTrackHeight}}}}},
					{"required", {"track", "height"}}, {"additionalProperties", false}}}};
		const Json foldedSchema = {{"type", "array"}, {"items", {{"type", "integer"}}}};
		const Json resultSchema = {{"type", "object"},
			{"properties", {{"heights", heightsSchema}, {"folded", foldedSchema}}},
			{"required", {"heights", "folded"}}};
		const std::string text =
			"heights: the tracks' heights in the track list and event display, in dp before vertical zoom (20 to 400), "
			"by track id, sorted; tracks without one are 40 dp high. folded: the folded folder tracks, sorted, whose "
			"tracks the track list and event display don't show. Tracks that are gone are left out.";

		engine.registerCommand(
			{"trackLayout.get", 1,
				"The track list's track heights and folded folders (reference manual pp. 217, 225). " + text,
				{{"type", "object"}, {"additionalProperties", false}}, resultSchema, true},
			[&model](CommandContext&) { return trackLayoutJson(model.project()); });

		model.addEdit(engine,
			{"trackLayout.set", 1,
				"Changes the track list's track heights and folded folders; what isn't given stays. " + text +
					" heights sets the given tracks' heights, the others keep theirs; folded replaces the folded "
					"folders. Settings, not an undo step, saved with the project. Emits trackLayout.changed, as undo "
					"and redo do when a track with a height or a fold state comes back or goes. Values the project has "
					"already change nothing: no event, and the project doesn't count as modified.",
				{{"type", "object"}, {"properties", {{"heights", heightsSchema}, {"folded", foldedSchema}}},
					{"additionalProperties", false}},
				resultSchema, false},
			"Change track layout",
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				TrackLayout& layout = project.trackLayout;
				const TrackLayout before = layout;
				if (parameters.contains("heights"))
					for (const Json& item : parameters.at("heights"))
					{
						const auto track = item.at("track").get<ObjectId>();
						require(project.trackById.count(track) != 0,
							"no track " + std::to_string(track) + " in the project");
						layout.heights[track] = item.at("height").get<double>();
					}
				if (parameters.contains("folded"))
				{
					layout.folded = tracksFrom(project, parameters.at("folded"));
					for (const ObjectId id : layout.folded)
						require(project.trackById.at(id).type == TrackType::Folder,
							"track " + std::to_string(id) + " is not a folder");
				}
				Json result = trackLayoutJson(project);
				// The layout the project has already is no change, which would count the project as modified (#1131).
				if (layout.heights == before.heights && layout.folded == before.folded)
				{
					context.markUnchanged();
					return result;
				}
				context.emit("trackLayout.changed", result);
				return result;
			});
	}
}
