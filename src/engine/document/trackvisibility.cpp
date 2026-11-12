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

		// Gives the mixers that follow the track visibility, but except, its state: a track's channel is hidden
		// there while the track is. Busses keep theirs.
		void syncMixers(Project& project, CommandContext& context, const std::string& except = {})
		{
			const std::set<ObjectId> tracks(
				project.trackVisibility.hidden.begin(), project.trackVisibility.hidden.end());
			for (const std::string& mixer : {std::string("lower"), project.trackVisibility.sync})
			{
				if (mixer.empty() || mixer == except)
					continue;
				MixerView& view = project.mixers[mixer];
				std::vector<ObjectId> hidden;
				for (const ObjectId id : view.hidden)
					if (project.channels.count(id) == 0)
						hidden.push_back(id);
				for (const auto& [id, channel] : project.channels)
					if (tracks.count(channel.track) != 0)
						hidden.push_back(id);
				std::sort(hidden.begin(), hidden.end());
				if (hidden == view.hidden)
					continue;
				view.hidden = std::move(hidden);
				context.emit("mixer.changed", mixerViewJson(project, mixer));
			}
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
		std::set<ObjectId> hidden(visibility.hidden.begin(), visibility.hidden.end());
		for (const auto& [id, channel] : project.channels)
			if (std::find(channels.begin(), channels.end(), id) != channels.end())
				hidden.insert(channel.track);
			else
				hidden.erase(channel.track);
		std::vector<ObjectId> tracks(hidden.begin(), hidden.end());
		if (tracks == visibility.hidden)
			return;
		visibility.hidden = std::move(tracks);
		context.emit("trackVisibility.changed", visibilityJson(project));
		syncMixers(project, context, mixer);
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
			"hidden: the tracks (by id) the project window's track list and event display don't show (a folder's "
			"tracks hide with it there). sync: the mixer window (window1 to window4) whose channel visibility follows "
			"the tracks', and back (Sync Visibility of Project and Mixer), or empty; the lower zone's mixer always "
			"follows, so a hidden track's channel is hidden in it, and a channel hidden there hides its track. "
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
					"none is active. configuration makes one active and, without hidden, hides its tracks. A setting, "
					"not an undo step, saved with the project. Emits trackVisibility.changed, and mixer.changed for "
					"the mixers that follow.",
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
						configurations.push_back({name, tracksFrom(project, item.at("hidden"))});
					}
					visibility.configurations = std::move(configurations);
					if (active(visibility) == nullptr)
						visibility.configuration.clear();
				}
				if (parameters.contains("configuration"))
				{
					visibility.configuration = parameters.at("configuration").get<std::string>();
					const VisibilityConfiguration* configuration = active(visibility);
					require(visibility.configuration.empty() || configuration != nullptr,
						"no configuration " + visibility.configuration);
					if (configuration != nullptr && !parameters.contains("hidden"))
						visibility.hidden = existing(project, configuration->hidden);
				}
				if (parameters.contains("hidden"))
					visibility.hidden = tracksFrom(project, parameters.at("hidden"));
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
					"and "
					"redo do when a track with a height or a fold state comes back or goes. Values the project has "
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
