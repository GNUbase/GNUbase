// SPDX-License-Identifier: GPL-3.0-or-later
// Copying and pasting channel settings (#112, reference manual pp. 461, 499-503): the pre section, the equalizer, the
// strip, the sends and the inserts of one channel onto others.
#include "engine/document/channelsettings.h"

#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		// What channel.copySettings keeps for channel.pasteSettings. Not part of the project or of undo.
		struct ChannelClipboard
		{
			ChannelSections sections;
			std::array<Send, g_maxSends> sends = {};
			std::vector<Slot> slots; // the inserts, from top to bottom
		};

		void require(bool condition, const std::string& message)
		{
			if (!condition)
				throw CommandError(message);
		}

		const Channel& audioChannel(const Project& project, ObjectId id)
		{
			const auto found = project.channels.find(id);
			require(found != project.channels.end(), "no channel " + std::to_string(id));
			require(found->second.format != ChannelFormat::None,
				"channel " + std::to_string(id) + " has no audio: MIDI and VCA channels have no channel settings");
			return found->second;
		}

		ChannelClipboard copySettings(const Project& project, ObjectId id)
		{
			const Channel& channel = audioChannel(project, id);
			ChannelClipboard clipboard{channel.sections, channel.sends, {}};
			for (const ObjectId slot : channel.slots)
				clipboard.slots.push_back(project.slots.at(slot));
			return clipboard;
		}

		// Gives a channel the copied settings: its inserts are copies of the copied ones, and a send keeps its
		// destination only where the channel may send.
		void pasteSettings(const ProjectModel& model, Project& project, CommandContext& context, ObjectId id,
			const ChannelClipboard& clipboard)
		{
			Channel& channel = project.channels.at(id);
			channel.sections = clipboard.sections;
			channel.sends = clipboard.sends;
			for (Send& send : channel.sends)
				if (send.destination != 0 &&
					(project.channels.count(send.destination) == 0 || !isSendDestination(project, send.destination) ||
						routingLoops(project, id, send.destination)))
					send.destination = 0;
			for (const ObjectId slot : channel.slots)
			{
				project.slots.erase(slot);
				for (auto lane = project.automation.begin(); lane != project.automation.end();)
				{
					const bool gone =
						lane->second.target.type == AutomationTargetType::Plugin && lane->second.target.slot == slot;
					if (gone)
						context.emit("automationLane.removed", {{"lane", lane->first}});
					lane = gone ? project.automation.erase(lane) : std::next(lane);
				}
				context.emit("plugin.removed", {{"slot", slot}});
			}
			channel.slots.clear();
			for (Slot slot : clipboard.slots)
			{
				slot.id = project.newId();
				slot.channel = id;
				channel.slots.push_back(slot.id);
				project.slots[slot.id] = std::move(slot);
			}
			context.emit("channel.changed", {{"channel", toJson(channel)}});
			for (const ObjectId slot : channel.slots)
				context.emit("plugin.added", {{"slot", model.describeSlot(project, slot)}});
		}
	}

	void registerChannelSettingsCommands(ProjectModel& model, Engine& engine)
	{
		auto clipboard = std::make_shared<std::optional<ChannelClipboard>>();
		const Json idSchema = {{"type", "integer"}};
		const std::string what = "the pre section, the equalizer, the strip, the sends and the inserts (the plugin "
								 "slots with their plugins' settings)";
		engine.registerCommand(
			{"channel.copySettings", 1,
				"Copies a channel's settings for channel.pasteSettings (Copy Channel Settings, reference manual pp. "
				"461, 503): " +
					what +
					"; not the fader, the pan, the buttons, the routing or the notes. MIDI and VCA channels have none. "
					"Changes nothing in the project.",
				{{"type", "object"}, {"properties", {{"channel", idSchema}}}, {"required", {"channel"}},
					{"additionalProperties", false}},
				Json::object(), false},
			[&model, clipboard](CommandContext& context)
			{
				*clipboard = copySettings(model.project(), context.parameters().at("channel").get<ObjectId>());
				return Json::object();
			});
		model.addEdit(engine,
			{"channel.pasteSettings", 1,
				"Pastes what channel.copySettings copied onto channels, in one undo step (Paste Channel Settings): " +
					what +
					". Their inserts are replaced by copies of the copied ones, the automation of the plugins they "
					"had goes with them. A send keeps its destination only if the channel may send there, so not to "
					"itself and never in a loop. Emits channel.changed, plugin.removed and plugin.added. Returns the "
					"channels.",
				{{"type", "object"},
					{"properties", {{"channels", {{"type", "array"}, {"items", idSchema}, {"minItems", 1}}}}},
					{"required", {"channels"}}, {"additionalProperties", false}},
				{{"type", "object"}, {"properties", {{"channels", {{"type", "array"}, {"items", idSchema}}}}},
					{"required", {"channels"}}},
				false},
			"Paste Channel Settings",
			[&model, clipboard](Project& project, CommandContext& context)
			{
				require(clipboard->has_value(), "nothing copied: channel.copySettings copies a channel's settings");
				const auto channels = context.parameters().at("channels").get<std::vector<ObjectId>>();
				for (const ObjectId id : channels)
					audioChannel(project, id);
				for (const ObjectId id : channels)
					pasteSettings(model, project, context, id, **clipboard);
				return Json{{"channels", channels}};
			});
	}
}
