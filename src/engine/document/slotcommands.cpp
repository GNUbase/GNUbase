// SPDX-License-Identifier: GPL-3.0-or-later
// Plugin slots (#282): plugin.insert, plugin.remove, plugin.move and plugin.set change the plugins in a channel's
// slots, each as one undo step that changes nothing if it fails (ProjectModel::add); plugin.list and plugin.get read
// them. The engine makes and runs the plugins from the render snapshot.
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"
#include "engine/processors/builtin.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <iterator>
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

		Json objectSchema(Json properties, const std::vector<std::string>& required = {})
		{
			return {{"type", "object"}, {"properties", std::move(properties)}, {"required", required},
				{"additionalProperties", false}};
		}

		// A channel that slots can be on: one with audio.
		Channel& audioChannel(Project& project, ObjectId id)
		{
			const auto found = project.channels.find(id);
			require(found != project.channels.end(), "no channel " + std::to_string(id));
			require(found->second.format != ChannelFormat::None, "a MIDI channel has no plugin slots");
			return found->second;
		}

		Slot& findSlot(Project& project, const Json& parameters)
		{
			const auto id = parameters.at("slot").get<ObjectId>();
			const auto found = project.slots.find(id);
			require(found != project.slots.end(), "no slot " + std::to_string(id));
			return found->second;
		}

		using SlotStatus = std::function<std::string(ObjectId)>;

		Json describe(const Project& project, const Slot& slot, const SlotStatus& status)
		{
			Json json = toJson(slot);
			json.erase("state"); // only files need it
			const std::vector<ObjectId>& slots = project.channels.at(slot.channel).slots;
			json["index"] = std::distance(slots.begin(), std::find(slots.begin(), slots.end(), slot.id));
			json["status"] = status ? status(slot.id) : "noDevice";
			return json;
		}

		void requireIndex(int64_t index, size_t size)
		{
			require(
				index >= 0 && index <= static_cast<int64_t>(size), "index must be from 0 to " + std::to_string(size));
		}

		void requireRoom(const Channel& channel)
		{
			require(channel.slots.size() < g_maxSlots,
				"a channel has at most " + std::to_string(g_maxSlots) + " plugin slots");
		}

		// The built-in plugin a plugin id names, or nullptr. Other formats come with the plugin scan (#66).
		const processors::BuiltinInfo* knownPlugin(const std::string& plugin)
		{
			return processors::findBuiltinPlugin(plugin);
		}

		Json insertPlugin(Project& project, CommandContext& context, const SlotStatus& status)
		{
			const Json& parameters = context.parameters();
			Channel& channel = audioChannel(project, parameters.at("channel").get<ObjectId>());
			const auto plugin = parameters.at("plugin").get<std::string>();
			const processors::BuiltinInfo* info = knownPlugin(plugin);
			require(info != nullptr, "no plugin " + plugin);
			require(!info->instrument, plugin + " is an instrument, not an effect");
			requireRoom(channel);
			const auto index = parameters.value("index", static_cast<int64_t>(channel.slots.size()));
			requireIndex(index, channel.slots.size());

			Slot slot;
			slot.id = project.newId();
			slot.channel = channel.id;
			slot.plugin = plugin;
			slot.name = info->name;
			channel.slots.insert(channel.slots.begin() + index, slot.id);
			const ObjectId id = slot.id;
			project.slots[id] = std::move(slot);
			context.emit("channel.changed", {{"channel", toJson(channel)}});
			Json described = describe(project, project.slots.at(id), status);
			context.emit("plugin.added", {{"slot", described}});
			return {{"slot", std::move(described)}};
		}

		// Removes the automation lanes of a slot's plugin, which go with the plugin when it leaves its channel.
		void removeAutomation(Project& project, CommandContext& context, ObjectId slot)
		{
			for (auto lane = project.automation.begin(); lane != project.automation.end();)
			{
				if (lane->second.target.type != AutomationTargetType::Plugin || lane->second.target.slot != slot)
				{
					++lane;
					continue;
				}
				context.emit("automationLane.removed", {{"lane", lane->first}});
				lane = project.automation.erase(lane);
			}
		}

		Json removePlugin(Project& project, CommandContext& context)
		{
			const Slot& slot = findSlot(project, context.parameters());
			const ObjectId id = slot.id;
			Channel& channel = project.channels.at(slot.channel);
			channel.slots.erase(std::find(channel.slots.begin(), channel.slots.end(), id));
			project.slots.erase(id);
			removeAutomation(project, context, id);
			context.emit("channel.changed", {{"channel", toJson(channel)}});
			context.emit("plugin.removed", {{"slot", id}});
			return Json::object();
		}

		Json movePlugin(Project& project, CommandContext& context, const SlotStatus& status)
		{
			const Json& parameters = context.parameters();
			Slot& slot = findSlot(project, parameters);
			Channel& from = project.channels.at(slot.channel);
			Channel& to = audioChannel(project, parameters.value("channel", slot.channel));
			// The index counts the slots without the moved one.
			const size_t size = to.slots.size() - (to.id == from.id ? 1 : 0);
			if (to.id != from.id)
				requireRoom(to);
			const auto index = parameters.at("index").get<int64_t>();
			requireIndex(index, size);

			from.slots.erase(std::find(from.slots.begin(), from.slots.end(), slot.id));
			to.slots.insert(to.slots.begin() + index, slot.id);
			slot.channel = to.id;
			if (to.id != from.id)
				removeAutomation(project, context, slot.id);
			context.emit("channel.changed", {{"channel", toJson(from)}});
			if (to.id != from.id)
				context.emit("channel.changed", {{"channel", toJson(to)}});
			return {{"slot", describe(project, slot, status)}};
		}

		Json setPlugin(Project& project, CommandContext& context, const SlotStatus& status)
		{
			const Json& parameters = context.parameters();
			Slot& slot = findSlot(project, parameters);
			slot.bypass = parameters.value("bypass", slot.bypass);
			slot.active = parameters.value("active", slot.active);
			Json described = describe(project, slot, status);
			context.emit("plugin.changed", {{"slot", described}});
			return {{"slot", std::move(described)}};
		}

		Json listPlugins(Project& project, CommandContext& context, const SlotStatus& status)
		{
			const Channel& channel = audioChannel(project, context.parameters().at("channel").get<ObjectId>());
			Json slots = Json::array();
			for (const ObjectId id : channel.slots)
				slots.push_back(describe(project, project.slots.at(id), status));
			return {{"slots", std::move(slots)}};
		}

		Json getPlugin(Project& project, CommandContext& context, const SlotStatus& status)
		{
			return {{"slot", describe(project, findSlot(project, context.parameters()), status)}};
		}
	}

	Json ProjectModel::describeSlot(const Project& project, ObjectId slot) const
	{
		const auto found = project.slots.find(slot);
		return found != project.slots.end() ? describe(project, found->second, m_slotStatus) : Json();
	}

	void ProjectModel::registerSlotCommands(Engine& engine)
	{
		const Json idSchema = {{"type", "integer"}};
		const Json indexSchema = {{"type", "integer"}, {"minimum", 0}, {"maximum", g_maxSlots}};
		const Json slotResult = {
			{"type", "object"}, {"properties", {{"slot", {{"type", "object"}}}}}, {"required", {"slot"}}};
		const std::string slotText =
			"A slot has its id, channel, index (0 is the top), plugin, name, bypass, active, the parameters set on its "
			"plugin (see parameter.list) and the status of its plugin in the audio engine: \"loading\" (being made or "
			"activated, while the audio passes it unchanged), \"active\", \"inactive\", \"failed\" (it couldn't be "
			"made or activated), \"unavailable\" (this build has no such plugin) or \"noDevice\" (no audio device is "
			"open). A command reports the status as of the command; plugin.changed announces it when it changes.";
		const auto with = [this](Json (*handler)(Project&, CommandContext&, const SlotStatus&))
		{
			return [this, handler](Project& project, CommandContext& context)
			{ return handler(project, context, m_slotStatus); };
		};

		add(engine,
			{"plugin.insert", 1,
				"Puts a plugin into a slot of a channel with audio (not a MIDI channel), at index or else below the "
				"others; the slot at the index and those below it move down. A channel has at most 16 slots. The "
				"audio passes through the slots from top to bottom before the fader. plugin is a built-in effect, "
				"\"builtin:<id>\" with an id from processor.list that isn't an instrument; plugins of other formats "
				"come with the plugin scan. The plugin starts with its parameters at their defaults, active and not "
				"bypassed. Emits plugin.added with the slot. Returns the slot. " +
					slotText,
				objectSchema({{"channel", idSchema}, {"plugin", {{"type", "string"}}}, {"index", indexSchema}},
					{"channel", "plugin"}),
				slotResult, false},
			with(insertPlugin));
		add(engine,
			{"plugin.remove", 1,
				"Removes a slot and its plugin; the slots below it move up. Emits plugin.removed with the slot's id. "
				"Undo puts the plugin back with its parameters.",
				objectSchema({{"slot", idSchema}}, {"slot"}), {{"type", "object"}}, false},
			removePlugin);
		add(engine,
			{"plugin.move", 1,
				"Moves a slot to index on its channel or on another channel with audio (channel); the index counts the "
				"slots without the moved one. The plugin keeps its parameters. Returns the slot.",
				objectSchema({{"slot", idSchema}, {"channel", idSchema}, {"index", indexSchema}}, {"slot", "index"}),
				slotResult, false},
			with(movePlugin));
		add(engine,
			{"plugin.set", 1,
				"Changes a slot: bypass lets the audio pass the plugin unchanged while the plugin keeps processing, so "
				"switching it doesn't click; the audio is delayed by the plugin's latency, so bypassing never shifts "
				"timing. active false deactivates the plugin, which stays in its slot, and the audio passes it "
				"unchanged. Emits plugin.changed with the slot. Returns the slot.",
				objectSchema({{"slot", idSchema}, {"bypass", {{"type", "boolean"}}}, {"active", {{"type", "boolean"}}}},
					{"slot"}),
				slotResult, false},
			with(setPlugin));
		add(engine,
			{"plugin.list", 1, "The slots of a channel with audio, from top to bottom. " + slotText,
				objectSchema({{"channel", idSchema}}, {"channel"}),
				{{"type", "object"}, {"properties", {{"slots", {{"type", "array"}}}}}, {"required", {"slots"}}}, true},
			with(listPlugins));
		add(engine,
			{"plugin.get", 1, "One slot. " + slotText, objectSchema({{"slot", idSchema}}, {"slot"}), slotResult, true},
			with(getPlugin));
	}
}
