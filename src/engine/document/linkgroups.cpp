// SPDX-License-Identifier: GPL-3.0-or-later
// Link groups and VCA faders (#110, reference manual pp. 457-461, 515-519). Like every command that changes the
// project, each is one undo step and changes nothing if it fails (ProjectModel::addEdit).
#include "engine/document/linkgroups.h"

#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <algorithm>
#include <iterator>
#include <map>
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

		// After a group changed: removes it if no channel is left, or announces the change. Returns the next group.
		std::map<ObjectId, LinkGroup>::iterator keepOrRemove(Project& project,
			std::map<ObjectId, LinkGroup>::iterator group, const LinkGroup& before, CommandContext& context)
		{
			if (group->second.channels.empty())
			{
				context.emit("linkGroup.removed", {{"group", group->first}});
				return project.linkGroups.erase(group);
			}
			if (!(group->second == before))
				context.emit("linkGroup.changed", {{"group", toJson(group->second)}});
			return std::next(group);
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

			// A channel is in one group at most, a VCA fader controls one group at most (p. 515). A group that loses
			// its last channel goes.
			for (auto other = project.linkGroups.begin(); other != project.linkGroups.end();)
			{
				LinkGroup& changed = other->second;
				if (changed.id == group.id)
				{
					++other;
					continue;
				}
				const LinkGroup before = changed;
				changed.channels.erase(std::remove_if(changed.channels.begin(), changed.channels.end(),
										   [&group](ObjectId channel)
										   {
											   return std::find(group.channels.begin(), group.channels.end(),
														  channel) != group.channels.end();
										   }),
					changed.channels.end());
				if (group.vca != 0 && changed.vca == group.vca)
				{
					changed.vca = 0;
					changed.volume = true;
				}
				other = keepOrRemove(project, other, before, context);
			}
			project.linkGroups[group.id] = group;
			for (const auto& [id, other] : project.linkGroups)
				require(other.vca == 0 || !controlsItself(project, other.vca),
					"VCA fader " + idText(other.vca) + " would control itself");
		}

		// Link Group N, N one more than the number of groups, or the next number no group's name has.
		std::string defaultName(const Project& project)
		{
			const auto taken = [&project](const std::string& name)
			{
				return std::any_of(project.linkGroups.begin(), project.linkGroups.end(),
					[&name](const auto& entry) { return entry.second.name == name; });
			};
			for (size_t number = project.linkGroups.size() + 1;; ++number)
				if (const std::string name = "Link Group " + std::to_string(number); !taken(name))
					return name;
		}

		Json addGroup(Project& project, CommandContext& context)
		{
			LinkGroup group;
			group.id = project.newId();
			group.name = defaultName(project);
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

	void removeFromLinkGroups(Project& project, const std::vector<ObjectId>& gone, CommandContext& context)
	{
		const auto isGone = [&gone](ObjectId id) { return std::find(gone.begin(), gone.end(), id) != gone.end(); };
		for (auto group = project.linkGroups.begin(); group != project.linkGroups.end();)
		{
			LinkGroup& changed = group->second;
			const LinkGroup before = changed;
			changed.channels.erase(
				std::remove_if(changed.channels.begin(), changed.channels.end(), isGone), changed.channels.end());
			if (changed.vca != 0 && isGone(changed.vca))
			{
				changed.vca = 0;
				changed.volume = true;
			}
			group = keepOrRemove(project, group, before, context);
		}
	}

	void dropLinkGroupLoops(Project& project)
	{
		for (auto& [id, group] : project.linkGroups)
			if (group.vca != 0 && controlsItself(project, group.vca))
			{
				group.vca = 0;
				group.volume = true;
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
			"were in, and a group left without channels goes (linkGroup.removed). What a change to one of them passes "
			"on to the others (channel.set, unless linked is false): "
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
				"Links channels in a new link group (reference manual pp. 457-459), named Link Group N, with a number "
				"no other group's name has, unless name is given; by default it links the volume and the sends, or "
				"with a VCA fader the sends. " +
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
