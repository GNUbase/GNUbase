// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/document/mixerviews.h"

#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		constexpr std::array<const char*, 5> g_mixers = {"lower", "window1", "window2", "window3", "window4"};

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		// The view as the project keeps it, without the channels and busses that are gone.
		Json viewJson(const Project& project, const std::string& mixer)
		{
			const auto found = project.mixers.find(mixer);
			MixerView view = found != project.mixers.end() ? found->second : MixerView();
			view.hidden.erase(std::remove_if(view.hidden.begin(), view.hidden.end(),
								  [&project](ObjectId id) { return mixerChannelType(project, id).empty(); }),
				view.hidden.end());
			Json json = toJson(view);
			json["mixer"] = mixer;
			return json;
		}
	}

	std::string mixerChannelType(const Project& project, ObjectId id)
	{
		for (const Bus& bus : project.busses)
			if (bus.id == id)
				return bus.direction == BusDirection::Input ? "input" : "output";
		const auto channel = project.channels.find(id);
		if (channel == project.channels.end())
			return {};
		switch (project.trackById.at(channel->second.track).type)
		{
		case TrackType::Audio:
			return "audio";
		case TrackType::Instrument:
			return "instrument";
		case TrackType::Midi:
			return "midi";
		case TrackType::Effect:
			return "effect";
		case TrackType::Vca:
			return "vca";
		case TrackType::Folder:
		case TrackType::Group:
		case TrackType::Marker:
		case TrackType::Ruler:
		case TrackType::Tempo:
		case TrackType::Signature:
			break;
		}
		return "group";
	}

	void registerMixerCommands(ProjectModel& model, Engine& engine)
	{
		const Json mixerSchema = {
			{"type", "string"}, {"enum", std::vector<std::string>(g_mixers.begin(), g_mixers.end())}};
		const Json typeSchema = {{"type", "string"},
			{"enum", std::vector<std::string>(g_mixerChannelTypes.begin(), g_mixerChannelTypes.end())}};
		const Json hiddenSchema = {{"type", "array"}, {"items", {{"type", "integer"}}}};
		const Json typesSchema = {{"type", "array"}, {"items", typeSchema}};
		const Json viewSchema = {{"type", "object"},
			{"properties", {{"mixer", mixerSchema}, {"hidden", hiddenSchema}, {"hiddenTypes", typesSchema}}},
			{"required", {"mixer", "hidden", "hiddenTypes"}}};
		const std::string viewText =
			"mixer: lower (the mixer in the project window's lower zone) or window1 to window4 (the mixer windows). "
			"hidden: the channels and busses (by id) it doesn't show. hiddenTypes: the channel types its filter hides: "
			"input, audio, instrument, midi, group (also folders' group channels), effect, vca and output.";

		engine.registerCommand({"mixer.get", 1, "What a mixer hides. " + viewText,
								   {{"type", "object"}, {"properties", {{"mixer", mixerSchema}}},
									   {"required", {"mixer"}}, {"additionalProperties", false}},
								   viewSchema, true},
			[&model](CommandContext& context)
			{ return viewJson(model.project(), context.parameters().at("mixer").get<std::string>()); });

		model.addEdit(engine,
			{"mixer.set", 1,
				"Changes what a mixer hides; what isn't given stays. " + viewText +
					" Each mixer has its own; they are settings, not undo steps, saved with the project. Emits "
					"mixer.changed with the mixer's view.",
				{{"type", "object"},
					{"properties", {{"mixer", mixerSchema}, {"hidden", hiddenSchema}, {"hiddenTypes", typesSchema}}},
					{"required", {"mixer"}}, {"additionalProperties", false}},
				viewSchema, false},
			"Change mixer view",
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const std::string mixer = parameters.at("mixer").get<std::string>();
				MixerView& view = project.mixers[mixer];
				if (parameters.contains("hidden"))
				{
					auto hidden = parameters.at("hidden").get<std::vector<ObjectId>>();
					for (const ObjectId id : hidden)
						require(!mixerChannelType(project, id).empty(),
							"no channel or bus " + std::to_string(id) + " in the project");
					std::sort(hidden.begin(), hidden.end());
					hidden.erase(std::unique(hidden.begin(), hidden.end()), hidden.end());
					view.hidden = std::move(hidden);
				}
				if (parameters.contains("hiddenTypes"))
				{
					// In the filter's order, once each.
					const auto given = parameters.at("hiddenTypes").get<std::vector<std::string>>();
					view.hiddenTypes.clear();
					for (const char* type : g_mixerChannelTypes)
						if (std::find(given.begin(), given.end(), type) != given.end())
							view.hiddenTypes.emplace_back(type);
				}
				Json result = viewJson(project, mixer);
				context.emit("mixer.changed", result);
				return result;
			});
	}
}
