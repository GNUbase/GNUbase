// SPDX-License-Identifier: GPL-3.0-or-later
// The sample editor (#140, reference manual pp. 613-635): the audio event it shows and its selection range, kept on the
// command thread outside the project like the MIDI editors' views (editors.cpp), so they are no undo steps and don't
// make the project modified; and the regions of clips (pp. 624-626), which are in the project, so each region command
// is one undo step and changes nothing if it fails (ProjectModel::addEdit).
#include "engine/document/sampleeditor.h"

#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/document/snap.h"
#include "engine/engine.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		struct SampleView
		{
			bool open = false;
			uint64_t project = 0;             // the project it belongs to (ProjectModel::openedRevision)
			ObjectId event = 0;               // kept while the event is gone, so that it comes back with an undo
			time::Samples selectionStart = 0; // clip frames; no selection while start and end are the same
			time::Samples selectionEnd = 0;
		};

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

		const AudioEvent* eventOf(const Project& project, ObjectId id)
		{
			const auto found = project.audioEvents.find(id);
			return found != project.audioEvents.end() ? &found->second : nullptr;
		}

		time::Samples clipLength(const Project& project, const AudioEvent& event)
		{
			return project.media.at(project.clips.at(event.clip).media).length;
		}

		// The view now: closed once another project is open; without a selection while its event is gone.
		SampleView current(const SampleView& stored, const ProjectModel& model)
		{
			SampleView view = stored.project == model.openedRevision() ? stored : SampleView();
			const AudioEvent* event = eventOf(model.project(), view.event);
			if (event == nullptr)
			{
				view.selectionStart = view.selectionEnd = 0;
				return view;
			}
			const time::Samples length = clipLength(model.project(), *event);
			view.selectionStart = std::min(view.selectionStart, length);
			view.selectionEnd = std::clamp(view.selectionEnd, view.selectionStart, length);
			return view;
		}

		Json viewJson(const Project& project, const SampleView& view)
		{
			Json json = {{"open", view.open}, {"event", 0}, {"clip", 0}, {"track", 0}, {"media", 0}, {"length", 0},
				{"offset", 0}, {"eventLength", 0}, {"clipStart", 0}, {"ratio", 1.0},
				{"selectionStart", view.selectionStart}, {"selectionEnd", view.selectionEnd}};
			const AudioEvent* event = eventOf(project, view.event);
			if (event == nullptr)
				return json;
			const EventSpan span = eventSpan(project, *event);
			json["event"] = event->id;
			json["clip"] = event->clip;
			json["track"] = event->track;
			json["media"] = project.clips.at(event->clip).media;
			json["length"] = clipLength(project, *event);
			json["offset"] = event->offset;
			json["eventLength"] = event->length;
			json["clipStart"] = span.start - std::llround(static_cast<double>(event->offset) * span.ratio);
			json["ratio"] = span.ratio;
			return json;
		}

		Clip& clipOf(Project& project, const Json& parameters)
		{
			const auto id = parameters.at("clip").get<ObjectId>();
			const auto found = project.clips.find(id);
			require(found != project.clips.end(), "no clip " + std::to_string(id));
			return found->second;
		}

		ClipRegion& regionOf(Clip& clip, const Json& parameters)
		{
			const auto id = parameters.at("region").get<ObjectId>();
			const auto found = std::find_if(
				clip.regions.begin(), clip.regions.end(), [id](const ClipRegion& region) { return region.id == id; });
			require(found != clip.regions.end(),
				"clip " + std::to_string(clip.id) + " has no region " + std::to_string(id));
			return *found;
		}

		// Checks a region's section against its clip and keeps the regions by start.
		void settle(const Project& project, Clip& clip, const ClipRegion& region)
		{
			const time::Samples length = project.media.at(clip.media).length;
			require(region.start >= 0 && region.start < region.end && region.end <= length,
				"a region must start before it ends, within the clip (0 to " + std::to_string(length) +
					" sample frames)");
			std::sort(clip.regions.begin(), clip.regions.end(), [](const ClipRegion& a, const ClipRegion& b)
				{ return std::tie(a.start, a.id) < std::tie(b.start, b.id); });
		}

		// Region and a number that no region of the clip has in its name, from the count of regions on.
		std::string newRegionName(const Clip& clip)
		{
			for (size_t number = clip.regions.size() + 1;; ++number)
			{
				std::string name = "Region " + std::to_string(number);
				if (std::none_of(clip.regions.begin(), clip.regions.end(),
						[&name](const ClipRegion& region) { return region.name == name; }))
					return name;
			}
		}

		Json regionResult(const Clip& clip, ObjectId region, CommandContext& context)
		{
			context.emit("clip.changed", {{"clip", toJson(clip)}});
			for (const ClipRegion& each : clip.regions)
				if (each.id == region)
					return {{"clip", clip.id}, {"region", toJson(each)}};
			return {{"clip", clip.id}};
		}
	}

	void registerSampleEditorCommands(ProjectModel& model, Engine& engine)
	{
		const Json integer = {{"type", "integer"}};
		const Json number = {{"type", "number"}};
		const Json string = {{"type", "string"}};
		const Json boolean = {{"type", "boolean"}};
		const Json viewSchema = {{"type", "object"},
			{"properties",
				{{"open", boolean}, {"event", integer}, {"clip", integer}, {"track", integer}, {"media", integer},
					{"length", integer}, {"offset", integer}, {"eventLength", integer}, {"clipStart", integer},
					{"ratio", number}, {"selectionStart", integer}, {"selectionEnd", integer}}},
			{"required",
				{"open", "event", "clip", "track", "media", "length", "offset", "eventLength", "clipStart", "ratio",
					"selectionStart", "selectionEnd"}}};
		const std::string viewText =
			"The view: open; event, the audio event it shows, with its clip, track and media file (0 while it shows "
			"none, or its event is gone: an undo brings it back); length, the clip's length in sample frames; offset "
			"and eventLength, the section of the clip the event plays (clip frames); clipStart, the project sample "
			"where the clip's frame 0 would play, and ratio, project samples per clip frame, so frame f plays at "
			"clipStart + f * ratio; selectionStart and selectionEnd, the selection range in clip frames, none while "
			"they are the same. View state: no undo steps, not saved, and a new or loaded project closes it.";
		auto views = std::make_shared<SampleView>();
		const auto store = [views, &model](const SampleView& view, CommandContext& context)
		{
			*views = view;
			views->project = model.openedRevision();
			Json result = viewJson(model.project(), *views);
			context.emit("sampleEditor.changed", result);
			return result;
		};

		engine.registerCommand(
			{"sampleEditor.get", 1, "What the sample editor shows. " + viewText, objectSchema({}), viewSchema, true},
			[views, &model](CommandContext&) { return viewJson(model.project(), current(*views, model)); });

		engine.registerCommand(
			{"sampleEditor.open", 1,
				"Opens the sample editor (reference manual p. 613) with an audio event, such as the one double-clicked "
				"or selected in the project window, or shows that event in it if it is open; the selection range goes. "
				"Emits sampleEditor.changed. " +
					viewText,
				objectSchema({{"event", integer}}, {"event"}), viewSchema, false},
			[views, &model, store](CommandContext& context)
			{
				const auto id = context.parameters().at("event").get<ObjectId>();
				require(eventOf(model.project(), id) != nullptr, "no audio event " + std::to_string(id));
				SampleView view = current(*views, model);
				view.open = true;
				if (view.event != id)
					view.selectionStart = view.selectionEnd = 0;
				view.event = id;
				return store(view, context);
			});

		engine.registerCommand(
			{"sampleEditor.set", 1,
				"Changes the open sample editor's selection range (reference manual pp. 618, 635). select: all (the "
				"whole clip), none, event (the section the event plays) or loop (between the locators, within the "
				"clip; none if they are outside it). Or selectionStart and selectionEnd, in clip frames, from 0 to the "
				"clip's length; one of them alone keeps the other. With zeroCrossing (Snap to Zero Crossing), both "
				"ends move to the nearest frame within 50 ms where the audio crosses zero. Emits "
				"sampleEditor.changed. " +
					viewText,
				objectSchema({{"select", {{"type", "string"}, {"enum", {"all", "none", "event", "loop"}}}},
					{"selectionStart", integer}, {"selectionEnd", integer}, {"zeroCrossing", boolean}}),
				viewSchema, false},
			[views, &model, store](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const Project& project = model.project();
				SampleView view = current(*views, model);
				require(view.open, "the sample editor isn't open");
				const AudioEvent* event = eventOf(project, view.event);
				require(event != nullptr, "the sample editor shows no audio event");
				const time::Samples length = clipLength(project, *event);
				time::Samples start = parameters.value("selectionStart", view.selectionStart);
				time::Samples end = parameters.value("selectionEnd", view.selectionEnd);
				const std::string select = parameters.value("select", std::string());
				if (select == "all")
				{
					std::tie(start, end) = std::make_pair(time::Samples{0}, length);
				}
				else if (select == "none")
				{
					start = end = 0;
				}
				else if (select == "event")
				{
					std::tie(start, end) = std::make_pair(event->offset, event->offset + event->length);
				}
				else if (select == "loop")
				{
					const Json transport = context.call("transport.get", Json::object());
					const EventSpan span = eventSpan(project, *event);
					const auto frame = [&](const char* locator)
					{
						const auto at = transport.at(locator).get<time::Samples>();
						return event->offset +
							std::llround(eventFrames(project, *event, static_cast<double>(at - span.start)));
					};
					start = std::clamp<time::Samples>(frame("left"), 0, length);
					end = std::clamp<time::Samples>(frame("right"), 0, length);
					if (end <= start)
						start = end = 0;
				}
				require(start >= 0 && start <= end && end <= length,
					"the selection range must start before it ends, within the clip (0 to " + std::to_string(length) +
						" sample frames)");
				if (parameters.value("zeroCrossing", false) && start < end)
				{
					const MediaFile& media = project.media.at(project.clips.at(event->clip).media);
					start = zeroCrossingFrame(media, start, 0, length - 1);
					end = std::max(start, zeroCrossingFrame(media, end, 0, length - 1));
				}
				view.selectionStart = start;
				view.selectionEnd = end;
				return store(view, context);
			});

		engine.registerCommand(
			{"sampleEditor.close", 1, "Closes the sample editor. Emits sampleEditor.changed. " + viewText,
				objectSchema({}), viewSchema, false},
			[store](CommandContext& context) { return store(SampleView(), context); });

		const Json regionSchema = objectSchema(
			{{"id", integer}, {"name", string}, {"start", integer}, {"end", integer}}, {"id", "name", "start", "end"});
		const Json regionResultSchema = objectSchema({{"clip", integer}, {"region", regionSchema}}, {"clip", "region"});
		model.addEdit(engine,
			{"clip.addRegion", 1,
				"Adds a region to a clip (Add Region, Event or Range as Region, reference manual pp. 620, 624-625): a "
				"named section of its audio, from start to end in the clip's sample frames, start before end; name "
				"defaults to Region and a number that no other region of the clip has in its name. A clip's regions "
				"are in its regions, by start. Emits clip.changed. Returns the clip's id and the region.",
				objectSchema({{"clip", integer}, {"start", integer}, {"end", integer}, {"name", string}},
					{"clip", "start", "end"}),
				regionResultSchema, false},
			"Add region",
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				Clip& clip = clipOf(project, parameters);
				const ClipRegion region{project.newId(), parameters.value("name", newRegionName(clip)),
					parameters.at("start").get<time::Samples>(), parameters.at("end").get<time::Samples>()};
				clip.regions.push_back(region);
				settle(project, clip, region);
				return regionResult(clip, region.id, context);
			});

		model.addEdit(engine,
			{"clip.setRegion", 1,
				"Renames a clip's region or moves its start or end (clip frames; reference manual p. 625); what isn't "
				"given stays. Emits clip.changed. Returns the clip's id and the region.",
				objectSchema(
					{{"clip", integer}, {"region", integer}, {"name", string}, {"start", integer}, {"end", integer}},
					{"clip", "region"}),
				regionResultSchema, false},
			"Change region",
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				Clip& clip = clipOf(project, parameters);
				ClipRegion& region = regionOf(clip, parameters);
				region.name = parameters.value("name", region.name);
				region.start = parameters.value("start", region.start);
				region.end = parameters.value("end", region.end);
				const ClipRegion changed = region;
				settle(project, clip, changed);
				return regionResult(clip, changed.id, context);
			});

		model.addEdit(engine,
			{"clip.removeRegion", 1,
				"Removes a region from a clip (Remove Region, reference manual p. 626). Emits clip.changed.",
				objectSchema({{"clip", integer}, {"region", integer}}, {"clip", "region"}),
				objectSchema({{"clip", integer}}, {"clip"}), false},
			"Remove region",
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				Clip& clip = clipOf(project, parameters);
				const ObjectId id = regionOf(clip, parameters).id;
				clip.regions.erase(std::remove_if(clip.regions.begin(), clip.regions.end(),
									   [id](const ClipRegion& region) { return region.id == id; }),
					clip.regions.end());
				return regionResult(clip, id, context);
			});
	}
}
