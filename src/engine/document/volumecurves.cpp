// SPDX-License-Identifier: GPL-3.0-or-later
// Event volume curves (#139, reference manual pp. 398-402). Like every command that changes the project, each is one
// undo step and changes nothing if it fails (ProjectModel::addEdit).
#include "engine/document/volumecurves.h"

#include "engine/audio/fades.h"
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <algorithm>
#include <cmath>
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

		AudioEvent& audioEvent(Project& project, ObjectId id)
		{
			const auto found = project.audioEvents.find(id);
			require(found != project.audioEvents.end(), "no audio event " + std::to_string(id));
			return found->second;
		}

		void checkVolume(double volume)
		{
			require(volume >= g_minEventVolume && volume <= g_maxEventVolume, "volume must be from -144 to 6 dB");
		}

		void changed(const AudioEvent& event, CommandContext& context)
		{
			context.emit("audioEvent.changed", {{"event", toJson(event)}});
		}

		Json setVolumeCurve(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			AudioEvent& event = audioEvent(project, parameters.at("event").get<ObjectId>());
			const time::Samples length = project.media.at(project.clips.at(event.clip).media).length;
			std::vector<VolumePoint> curve = volumeCurveFromJson(parameters.at("points"));
			for (const VolumePoint& point : curve)
			{
				require(point.position >= 0 && point.position <= length,
					"points must be within the clip (0 to " + std::to_string(length) + " sample frames)");
				checkVolume(point.volume);
				require(point.curve >= -1.0 && point.curve <= 1.0, "curve must be from -1 to 1");
			}
			std::stable_sort(curve.begin(), curve.end(),
				[](const VolumePoint& a, const VolumePoint& b) { return a.position < b.position; });
			event.volumeCurve = std::move(curve);
			Json result = {{"event", toJson(event)}};
			changed(event, context);
			return result;
		}

		// Remove Volume Curve (reference manual p. 401) on events.
		Json removeVolumeCurves(Project& project, CommandContext& context)
		{
			std::vector<ObjectId> ids = context.parameters().at("events").get<std::vector<ObjectId>>();
			std::sort(ids.begin(), ids.end());
			ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
			for (const ObjectId id : ids)
			{
				AudioEvent& event = audioEvent(project, id);
				event.volumeCurve.clear();
				changed(event, context);
			}
			return {{"events", ids}};
		}

		// The volume of the audio events in a range (reference manual pp. 401-402): their curves are level at volume
		// from the range's start to its end, and stay as they were outside it.
		Json setRangeVolume(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const auto start = parameters.at("start").get<time::Samples>();
			const auto end = parameters.at("end").get<time::Samples>();
			require(start >= 0 && end > start, "the range is empty");
			const double volume = parameters.value("volume", 0.0);
			checkVolume(volume);
			std::vector<ObjectId> tracks;
			if (parameters.contains("tracks"))
				tracks = parameters.at("tracks").get<std::vector<ObjectId>>();
			Json events = Json::array();
			for (auto& [id, event] : project.audioEvents)
			{
				if (!tracks.empty() && std::find(tracks.begin(), tracks.end(), event.track) == tracks.end())
					continue;
				const EventSpan span = eventSpan(project, event);
				if (end <= span.start || start >= span.end)
					continue;
				const auto frame = [&span, &event](time::Samples sample)
				{
					const auto into = std::llround(
						static_cast<double>(std::clamp(sample, span.start, span.end) - span.start) / span.ratio);
					return event.offset + std::clamp<time::Samples>(into, 0, event.length);
				};
				const time::Samples from = frame(start);
				const time::Samples to = frame(end);
				// Without a curve, the event plays at 0 dB everywhere: a single point.
				const std::vector<VolumePoint> old =
					event.volumeCurve.empty() ? std::vector<VolumePoint>{{0, 0.0, 0.0}} : event.volumeCurve;
				std::vector<VolumePoint> curve = cutVolumeCurve(old, std::min(old.front().position, from), from);
				curve.push_back({from, volume, 0.0});
				curve.push_back({to, volume, 0.0});
				const std::vector<VolumePoint> rest = cutVolumeCurve(old, to, std::max(old.back().position, to));
				curve.insert(curve.end(), rest.begin(), rest.end());
				// A curve that is 0 dB everywhere is none.
				if (std::all_of(
						curve.begin(), curve.end(), [](const VolumePoint& point) { return point.volume == 0.0; }))
					curve.clear();
				event.volumeCurve = std::move(curve);
				changed(event, context);
				events.push_back(id);
			}
			require(!events.empty(), "no audio event on the range's tracks crosses the range");
			return {{"events", events}};
		}
	}

	void registerVolumeCurveCommands(ProjectModel& model, Engine& engine)
	{
		const Json integer = {{"type", "integer"}};
		const Json number = {{"type", "number"}};
		const Json ids = {{"type", "array"}, {"items", integer}};
		const auto object = [](Json properties, std::vector<std::string> required)
		{
			return Json{{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)},
				{"additionalProperties", false}};
		};
		const Json point = object({{"position", integer}, {"volume", number}, {"curve", number}}, {"position"});
		const Json eventsResult = object({{"events", ids}}, {"events"});

		model.addEdit(engine,
			{"audioEvent.setVolumeCurve", 1,
				"Sets an audio event's volume curve, on top of its volume: points at positions in the event's clip "
				"(sample frames, 0 to the clip's length; the curve moves with the event and stays with the audio when "
				"it is resized or split), each with a volume in dB (-144 to 6, default 0) and a curve (-1 to 1, "
				"default 0) that bends the way to the next point: from point to point the gain goes from one's to the "
				"next's along (1 - e^(-6cx)) / (1 - e^(-6c)), straight for 0, fast at first above 0, slowly at first "
				"below. Before the first point the curve is level with it, after the last one with that; points at "
				"the same position make a jump. No points: no curve. Returns the event, its points by position.",
				object({{"event", integer}, {"points", {{"type", "array"}, {"items", point}}}}, {"event", "points"}),
				object({{"event", {{"type", "object"}}}}, {"event"}), false},
			"Change volume curve", setVolumeCurve);
		model.addEdit(engine,
			{"event.removeVolumeCurve", 1,
				"Remove Volume Curve: audio events play at their volume again, without a curve. Returns the events.",
				object({{"events", ids}}, {"events"}), eventsResult, false},
			"Remove Volume Curve", removeVolumeCurves);
		model.addEdit(engine,
			{"range.volumeCurve", 1,
				"Sets the volume curves of the audio events on tracks (ids, default every track) that cross the range "
				"from start to end (project samples) to volume (dB, -144 to 6, default 0) inside the range, with "
				"jumps at its edges; outside it they stay as they were. With volume 0, Remove Volume Curve on a "
				"range: the original level there. A curve left at 0 dB everywhere is none. Returns the events "
				"changed.",
				object({{"start", integer}, {"end", integer}, {"tracks", ids}, {"volume", number}}, {"start", "end"}),
				eventsResult, false},
			"Change volume curve", setRangeVolume);
	}
}
