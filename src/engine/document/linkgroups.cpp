// SPDX-License-Identifier: GPL-3.0-or-later
// Link groups and VCA faders (#110, reference manual pp. 457-461, 515-519). Like every command that changes the
// project, each is one undo step and changes nothing if it fails (ProjectModel::addEdit).
#include "engine/document/linkgroups.h"

#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <algorithm>
#include <set>
#include <string>
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

		bool isVca(const Project& project, ObjectId channel)
		{
			const auto found = project.channels.find(channel);
			return found != project.channels.end() && project.trackById.at(found->second.track).type == TrackType::Vca;
		}

		// Whether a VCA fader controls itself through the groups it is in (nested VCA faders, p. 518).
		bool controlsItself(const Project& project, ObjectId vca)
		{
			std::set<ObjectId> seen;
			for (const LinkGroup* group = linkGroupOf(project, vca); group != nullptr && group->vca != 0;
				group = linkGroupOf(project, group->vca))
			{
				if (group->vca == vca)
					return true;
				if (!seen.insert(group->vca).second)
					return false;
			}
			return false;
		}

		// Changes a group by what the parameters have, takes its channels and its VCA fader from other groups, and
		// checks the result. A group with a VCA fader doesn't link the volume (p. 517): giving it one turns volume off
		// unless the parameters say otherwise, taking it away turns it back on.
		void change(Project& project, LinkGroup& group, const Json& parameters, CommandContext& context)
		{
			const ObjectId vcaBefore = group.vca;
			if (parameters.contains("name"))
				group.name = parameters.at("name").get<std::string>();
			if (parameters.contains("channels"))
			{
				group.channels.clear();
				for (const ObjectId channel : parameters.at("channels").get<std::vector<ObjectId>>())
				{
					require(project.channels.count(channel) != 0, "no channel " + idText(channel));
					require(std::find(group.channels.begin(), group.channels.end(), channel) == group.channels.end(),
						"channels lists channel " + idText(channel) + " twice");
					group.channels.push_back(channel);
				}
				require(!group.channels.empty(), "a link group needs a channel");
			}
			if (parameters.contains("vca"))
			{
				group.vca = parameters.at("vca").get<ObjectId>();
				require(group.vca == 0 || isVca(project, group.vca), "vca must be the channel of a VCA track");
			}
			group.pan = parameters.value("pan", group.pan);
			group.mute = parameters.value("mute", group.mute);
			group.record = parameters.value("record", group.record);
			group.eq = parameters.value("eq", group.eq);
			group.dynamics = parameters.value("dynamics", group.dynamics);
			group.sends = parameters.value("sends", group.sends);
			if (group.vca != vcaBefore)
				group.volume = group.vca == 0;
			group.volume = parameters.value("volume", group.volume);
			require(!group.volume || group.vca == 0, "a link group with a VCA fader doesn't link the volume");
			require(std::find(group.channels.begin(), group.channels.end(), group.vca) == group.channels.end(),
				"a VCA fader can't control a link group it is in");

			// A channel is in one group at most, a VCA fader controls one group at most (p. 515).
			for (auto& [id, other] : project.linkGroups)
			{
				if (id == group.id)
					continue;
				const LinkGroup before = other;
				other.channels.erase(std::remove_if(other.channels.begin(), other.channels.end(),
										 [&group](ObjectId channel)
										 {
											 return std::find(group.channels.begin(), group.channels.end(), channel) !=
												 group.channels.end();
										 }),
					other.channels.end());
				if (group.vca != 0 && other.vca == group.vca)
				{
					other.vca = 0;
					other.volume = true;
				}
				if (!(other == before))
					context.emit("linkGroup.changed", {{"group", toJson(other)}});
			}
			project.linkGroups[group.id] = group;
			for (const auto& [id, other] : project.linkGroups)
				require(other.vca == 0 || !controlsItself(project, other.vca),
					"VCA fader " + idText(other.vca) + " would control itself");
		}

		Json addGroup(Project& project, CommandContext& context)
		{
			LinkGroup group;
			group.id = project.newId();
			group.name = "Link Group " + std::to_string(project.linkGroups.size() + 1);
			change(project, group, context.parameters(), context);
			Json result = {{"group", toJson(project.linkGroups.at(group.id))}};
			context.emit("linkGroup.added", result);
			return result;
		}

		LinkGroup& findGroup(Project& project, const Json& parameters)
		{
			const auto id = parameters.at("group").get<ObjectId>();
			const auto found = project.linkGroups.find(id);
			require(found != project.linkGroups.end(), "no link group " + idText(id));
			return found->second;
		}

		Json setGroup(Project& project, CommandContext& context)
		{
			LinkGroup group = findGroup(project, context.parameters());
			change(project, group, context.parameters(), context);
			Json result = {{"group", toJson(project.linkGroups.at(group.id))}};
			context.emit("linkGroup.changed", result);
			return result;
		}

		Json removeGroup(Project& project, CommandContext& context)
		{
			const ObjectId id = findGroup(project, context.parameters()).id;
			project.linkGroups.erase(id);
			context.emit("linkGroup.removed", {{"group", id}});
			return Json::object();
		}
	}

	void registerLinkGroupCommands(ProjectModel& model, Engine& engine)
	{
		const Json idSchema = {{"type", "integer"}};
		const Json flagSchema = {{"type", "boolean"}};
		const Json properties = {{"name", {{"type", "string"}}}, {"channels", {{"type", "array"}, {"items", idSchema}}},
			{"volume", flagSchema}, {"pan", flagSchema}, {"mute", flagSchema}, {"record", flagSchema},
			{"eq", flagSchema}, {"dynamics", flagSchema}, {"sends", flagSchema}, {"vca", idSchema}};
		const Json groupResult = {
			{"type", "object"}, {"properties", {{"group", {{"type", "object"}}}}}, {"required", {"group"}}};
		const std::string settings =
			"channels: the linked channels; a channel is in one link group at most, so they leave the groups they "
			"were in. What a change to one of them passes on to the others (channel.set, unless linked is false): "
			"volume (the fader, by as much as it moved), pan (likewise), mute (the mute, solo and listen buttons), "
			"record (record enable and monitor), eq (the equalizer's changes), dynamics (the strip's: gate, compressor "
			"and limiter), sends (the sends' changes, on by default; a channel that can't take one, such as a send to "
			"itself, keeps its own). vca: the channel of a VCA track that controls the group, or 0; it "
			"leaves the group it controlled. The VCA fader's volume adds to each channel's, its automation to theirs, "
			"and its buttons pass on to them; VCA faders can be in groups that other VCA faders control, but never "
			"control themselves. A group with a VCA fader doesn't link the volume: giving it one turns volume off, "
			"taking it away turns it on, unless volume is given.";

		const auto schema = [&properties, &idSchema](bool add)
		{
			Json result = properties;
			if (!add)
				result["group"] = idSchema;
			return Json{{"type", "object"}, {"properties", result},
				{"required", add ? std::vector<std::string>{"channels"} : std::vector<std::string>{"group"}},
				{"additionalProperties", false}};
		};

		model.addEdit(engine,
			{"linkGroup.add", 1,
				"Links channels in a new link group (reference manual pp. 457-459), named Link Group N unless name is "
				"given; by default it links the volume and the sends, or with a VCA fader the sends. " +
					settings + " Emits linkGroup.added.",
				schema(true), groupResult, false},
			"Link channels", addGroup);
		model.addEdit(engine,
			{"linkGroup.set", 1,
				"Changes a link group: what isn't given stays. " + settings + " Emits linkGroup.changed.",
				schema(false), groupResult, false},
			"Change link group", setGroup);
		model.addEdit(engine,
			{"linkGroup.remove", 1,
				"Unlinks a link group's channels (p. 459). A VCA fader that controlled it stays. Emits "
				"linkGroup.removed.",
				{{"type", "object"}, {"properties", {{"group", idSchema}}}, {"required", {"group"}},
					{"additionalProperties", false}},
				Json::object(), false},
			"Unlink channels", removeGroup);
	}
}
