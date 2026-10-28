// SPDX-License-Identifier: GPL-3.0-or-later
// Fades and crossfades (#138): audioEvent.setFades, clip.setFades, crossfade.set, crossfade.remove and autoFades.set.
// Like every command that changes the project, each is one undo step and changes nothing if it fails
// (ProjectModel::add). Playback applies them (makePlaylist in src/engine/audio/playback.cpp).
#include "engine/audio/fades.h"
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
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

		template <typename Map> auto& find(Map& objects, const Json& parameters, const char* key, const char* what)
		{
			const auto id = parameters.at(key).get<ObjectId>();
			const auto found = objects.find(id);
			require(found != objects.end(), std::string("no ") + what + " " + idText(id));
			return found->second;
		}

		FadeShape shape(const Json& value)
		{
			const auto shape = fadeShapeFromName(value.get<std::string>());
			require(shape.has_value(), "unknown fade shape " + value.dump());
			return *shape;
		}

		// A fade with what the parameter changes; lengths in sample frames.
		Fade fade(const Json& parameters, const char* key, const Fade& current)
		{
			if (!parameters.contains(key))
				return current;
			const Json& item = parameters.at(key);
			Fade result = current;
			if (item.contains("length"))
			{
				result.length = item.at("length").get<time::Samples>();
				require(result.length >= 0, std::string(key) + ".length must not be negative");
			}
			if (item.contains("shape"))
				result.shape = shape(item.at("shape"));
			return result;
		}

		void checkFades(const Fade& in, const Fade& out, time::Samples length, const std::string& what)
		{
			require(in.length + out.length <= length,
				"the fade in and the fade out together must fit into the " + what + " (" + std::to_string(length) +
					" sample frames)");
		}

		Json setEventFades(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			AudioEvent& event = find(project.audioEvents, parameters, "event", "audio event");
			const Fade in = fade(parameters, "fadeIn", event.fadeIn);
			const Fade out = fade(parameters, "fadeOut", event.fadeOut);
			checkFades(in, out, event.length, "event");
			event.fadeIn = in;
			event.fadeOut = out;
			Json result = {{"event", toJson(event)}};
			context.emit("audioEvent.changed", result);
			return result;
		}

		Json setClipFades(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			Clip& clip = find(project.clips, parameters, "clip", "clip");
			const Fade in = fade(parameters, "fadeIn", clip.fadeIn);
			const Fade out = fade(parameters, "fadeOut", clip.fadeOut);
			checkFades(in, out, project.media.at(clip.media).length, "clip");
			clip.fadeIn = in;
			clip.fadeOut = out;
			Json result = {{"clip", toJson(clip)}};
			context.emit("clip.changed", result);
			return result;
		}

		// An event's start and end in the project's samples.
		struct Span
		{
			time::Samples start = 0;
			time::Samples end = 0;
			double ratio = 1.0; // project samples per clip frame
		};

		Span span(const Project& project, const AudioEvent& event)
		{
			Span result;
			result.ratio = project.sampleRate / project.media.at(project.clips.at(event.clip).media).sampleRate;
			result.start = time::toSamples(event.position, project.tempo, project.sampleRate);
			result.end = result.start + std::llround(static_cast<double>(event.length) * result.ratio);
			return result;
		}

		// Clears the crossfades into and out of an event.
		void clearCrossfades(Project& project, ObjectId id, CommandContext& context)
		{
			for (auto& [eventId, event] : project.audioEvents)
				if (event.crossfade.from == id || (eventId == id && event.crossfade.from != 0))
				{
					event.crossfade = Crossfade();
					context.emit("audioEvent.changed", {{"event", toJson(event)}});
				}
		}

		Json setCrossfade(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const AudioEvent& from = find(project.audioEvents, parameters, "from", "audio event");
			AudioEvent& to = find(project.audioEvents, parameters, "to", "audio event");
			require(from.id != to.id, "an event can't crossfade with itself");
			require(from.track == to.track, "crossfades are between events on the same track");
			const Span out = span(project, from);
			const Span in = span(project, to);
			require(out.start < in.start && in.start < out.end && out.end <= in.end,
				"event " + idText(to.id) + " must start inside event " + idText(from.id) + " and end after it");

			// A crossfade that is there already keeps what the parameters don't change.
			Crossfade crossfade = to.crossfade.from == from.id ? to.crossfade : Crossfade();
			crossfade.from = from.id;
			if (parameters.contains("mode"))
			{
				const auto mode = crossfadeModeFromName(parameters.at("mode").get<std::string>());
				require(mode.has_value(), "unknown crossfade mode " + parameters.at("mode").dump());
				crossfade.mode = *mode;
			}
			const bool shaped = parameters.contains("shape") || parameters.contains("outShape");
			switch (crossfade.mode)
			{
			case CrossfadeMode::EqualPower:
				require(!shaped, "equal power crossfades are quarter sines; shapes are for equal gain and custom ones");
				crossfade.shape = FadeShape::EqualPower;
				crossfade.outShape = FadeShape::EqualPower;
				break;
			case CrossfadeMode::EqualGain:
				require(!parameters.contains("outShape"),
					"an equal gain crossfade's fade out mirrors its fade in; outShape is for custom ones");
				if (parameters.contains("shape"))
					crossfade.shape = shape(parameters.at("shape"));
				if (!mirrorShape(crossfade.shape, crossfade.outShape))
				{
					require(!parameters.contains("shape"),
						"an equal gain crossfade is linear, exponential, logarithmic or sCurve");
					crossfade.shape = FadeShape::Linear;
					crossfade.outShape = FadeShape::Linear;
				}
				break;
			case CrossfadeMode::Custom:
				if (parameters.contains("shape"))
					crossfade.shape = shape(parameters.at("shape"));
				if (parameters.contains("outShape"))
					crossfade.outShape = shape(parameters.at("outShape"));
				break;
			}

			// Lengths in each event's clip frames, up to the overlap.
			const time::Samples overlap = out.end - in.start;
			const auto length = [&parameters, overlap](const char* key, time::Samples current, double ratio)
			{
				if (!parameters.contains(key))
					return current;
				const auto value = parameters.at(key).get<time::Samples>();
				const auto most = static_cast<time::Samples>(std::floor(static_cast<double>(overlap) / ratio));
				require(value >= 0 && value <= most,
					std::string(key) + " must be 0 (the whole overlap) to " + std::to_string(most) + " sample frames");
				return value;
			};
			crossfade.length = length("length", crossfade.length, in.ratio);
			crossfade.outLength = length("outLength", crossfade.outLength, out.ratio);

			// An event fades out into one event only.
			for (auto& [id, event] : project.audioEvents)
				if (id != to.id && event.crossfade.from == from.id)
				{
					event.crossfade = Crossfade();
					context.emit("audioEvent.changed", {{"event", toJson(event)}});
				}
			to.crossfade = crossfade;
			Json result = {{"event", toJson(to)}};
			context.emit("audioEvent.changed", result);
			return result;
		}

		Json removeCrossfades(Project& project, CommandContext& context)
		{
			const AudioEvent& event = find(project.audioEvents, context.parameters(), "event", "audio event");
			const bool any = event.crossfade.from != 0 ||
				std::any_of(project.audioEvents.begin(), project.audioEvents.end(),
					[&event](const auto& entry) { return entry.second.crossfade.from == event.id; });
			require(any, "audio event " + idText(event.id) + " has no crossfade");
			clearCrossfades(project, event.id, context);
			return Json::object();
		}

		Json setAutoFades(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			Track* track = nullptr;
			if (parameters.contains("track"))
			{
				track = &find(project.trackById, parameters, "track", "track");
				require(track->type == TrackType::Audio, "only audio tracks have auto fades");
			}
			const bool useProject = parameters.value("useProject", false);
			require(track != nullptr || !parameters.contains("useProject"), "useProject is for tracks");

			std::optional<AutoFades> settings;
			if (!useProject)
			{
				const AutoFades current =
					track != nullptr ? track->autoFades.value_or(project.autoFades) : project.autoFades;
				try
				{
					settings = autoFadesFromJson(parameters, current);
				}
				catch (const std::invalid_argument& error)
				{
					throw CommandError(error.what());
				}
				require(settings->length >= g_minAutoFadeLength && settings->length <= g_maxAutoFadeLength,
					"length must be 1 to 500 ms");
				require(
					settings->crossfadeMode != CrossfadeMode::Custom, "auto crossfades are equal gain or equal power");
				FadeShape mirror = FadeShape::Linear;
				require(settings->crossfadeMode == CrossfadeMode::EqualPower ||
						mirrorShape(settings->crossfadeShape, mirror),
					"an equal gain crossfade is linear, exponential, logarithmic or sCurve");
			}
			else
			{
				require(parameters.size() == 2, "with useProject, a track takes the project's settings as they are");
			}

			if (track == nullptr)
			{
				project.autoFades = *settings;
				context.emit("project.changed",
					{{"name", project.name}, {"sampleRate", project.sampleRate},
						{"autoFades", toJson(project.autoFades)}});
				return {{"autoFades", toJson(project.autoFades)}};
			}
			track->autoFades = settings;
			context.emit("track.changed", {{"track", toJson(*track)}});
			return {{"autoFades", toJson(track->autoFades.value_or(project.autoFades))}};
		}
	}

	void ProjectModel::registerFadeCommands(Engine& engine)
	{
		const Json id = {{"type", "integer"}};
		const Json integer = {{"type", "integer"}};
		const Json number = {{"type", "number"}};
		const Json boolean = {{"type", "boolean"}};
		const Json shapes = {{"type", "string"}, {"enum", fadeShapeNames()}};
		const Json modes = {{"type", "string"}, {"enum", crossfadeModeNames()}};
		const auto object = [](Json properties, std::vector<std::string> required)
		{
			return Json{{"type", "object"}, {"properties", std::move(properties)}, {"required", std::move(required)},
				{"additionalProperties", false}};
		};
		const auto result = [](const char* key)
		{ return Json{{"type", "object"}, {"properties", {{key, {{"type", "object"}}}}}, {"required", {key}}}; };
		const Json fade = object({{"length", integer}, {"shape", shapes}}, {});

		add(engine,
			{"audioEvent.setFades", 1,
				"Changes an audio event's fade in (from its start) or fade out (up to its end): length in the clip's "
				"sample frames (0: none) and shape: linear, exponential (slow, then fast), logarithmic (fast, then "
				"slow), sCurve (half a cosine) or equalPower (a quarter sine). Together they fit into the event. A "
				"crossfade replaces the fades at the ends it joins.",
				object({{"event", id}, {"fadeIn", fade}, {"fadeOut", fade}}, {"event"}), result("event"), false},
			setEventFades);
		add(engine,
			{"clip.setFades", 1,
				"Changes a clip's fade in or fade out, at its media file's start and end: length in sample frames (0: "
				"none) and shape, as for audioEvent.setFades. Every event that plays the clip gets them.",
				object({{"clip", id}, {"fadeIn", fade}, {"fadeOut", fade}}, {"clip"}), result("clip"), false},
			setClipFades);
		add(engine,
			{"crossfade.set", 1,
				"Crossfades from an audio event into the next one on its track, where they overlap: to must start "
				"inside from and end after it. Both play there; from fades out up to its end over outLength (from's "
				"clip frames), to fades in from its start over length (to's clip frames); 0, the default, is the whole "
				"overlap, so the crossfade is symmetric. mode equalGain (default): the fade out mirrors the fade in "
				"(shape linear, exponential, logarithmic or sCurve), so their gains add up to 1; equalPower: quarter "
				"sines whose power adds up to 1; custom: shape and outShape of their own. An event fades out into one "
				"event only. Changing a crossfade keeps what isn't given. Returns to.",
				object({{"from", id}, {"to", id}, {"mode", modes}, {"shape", shapes}, {"outShape", shapes},
						   {"length", integer}, {"outLength", integer}},
					{"from", "to"}),
				result("event"), false},
			setCrossfade);
		add(engine,
			{"crossfade.remove", 1, "Removes the crossfades into and out of an audio event.",
				object({{"event", id}}, {"event"}), Json::object(), false},
			removeCrossfades);
		add(engine,
			{"autoFades.set", 1,
				"Changes the auto fades and crossfades of the project, or of an audio track: fadeIn and fadeOut where "
				"an event has no fade of its own, crossfades where an event starts on top of another one or ends "
				"above one that goes on, unless they crossfade already. length is 1 to 500 ms; shapes as for "
				"audioEvent.setFades; crossfadeMode equalGain or equalPower, with crossfadeShape the fade in as for "
				"crossfade.set. A track uses the project's settings until it gets its own; useProject: true goes back "
				"to them. Returns the settings that apply.",
				object({{"track", id}, {"useProject", boolean}, {"fadeIn", boolean}, {"fadeOut", boolean},
						   {"crossfades", boolean}, {"length", number}, {"fadeInShape", shapes},
						   {"fadeOutShape", shapes}, {"crossfadeMode", modes}, {"crossfadeShape", shapes}},
					{}),
				result("autoFades"), false},
			setAutoFades);
	}
}
