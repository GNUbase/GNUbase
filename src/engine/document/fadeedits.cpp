// SPDX-License-Identifier: GPL-3.0-or-later
// Fades and crossfades (#138): audioEvent.setFades, clip.setFades, crossfade.set, crossfade.remove, autoFades.set,
// and the Fades menu's range.fades, event.crossfade and event.removeFades.
// Like every command that changes the project, each is one undo step and changes nothing if it fails
// (ProjectModel::add). Playback applies them (makePlaylist in src/engine/audio/playback.cpp).
#include "engine/audio/fades.h"
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/document/warp.h"
#include "engine/engine.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
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
			if (!shape)
				throw CommandError("unknown fade shape " + value.dump());
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
			const EventSpan out = eventSpan(project, from);
			const EventSpan in = eventSpan(project, to);
			require(out.start < in.start && in.start < out.end && out.end <= in.end,
				"event " + idText(to.id) + " must start inside event " + idText(from.id) + " and end after it");

			// A crossfade that is there already keeps what the parameters don't change.
			Crossfade crossfade = to.crossfade.from == from.id ? to.crossfade : Crossfade();
			crossfade.from = from.id;
			if (parameters.contains("mode"))
			{
				const auto mode = crossfadeModeFromName(parameters.at("mode").get<std::string>());
				if (!mode)
					throw CommandError("unknown crossfade mode " + parameters.at("mode").dump());
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

		AudioEvent& audioEvent(Project& project, ObjectId id)
		{
			const auto found = project.audioEvents.find(id);
			require(found != project.audioEvents.end(), "no audio event " + idText(id));
			return found->second;
		}

		void changed(const AudioEvent& event, CommandContext& context)
		{
			context.emit("audioEvent.changed", {{"event", toJson(event)}});
		}

		// The Fades menu's range functions (reference manual pp. 384-385, 388): fade in to the range's start or
		// from its end, fade out from its end or start, or both to fit it.
		Json fadesToRange(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const auto start = parameters.at("start").get<time::Samples>();
			const auto end = parameters.at("end").get<time::Samples>();
			require(start >= 0 && end > start, "the range is empty");
			const std::string kind = parameters.at("fade").get<std::string>();
			const bool fadeIn = kind == "in" || kind == "adjust";
			const bool fadeOut = kind == "out" || kind == "adjust";
			std::vector<ObjectId> tracks;
			if (parameters.contains("tracks"))
				tracks = parameters.at("tracks").get<std::vector<ObjectId>>();
			Json events = Json::array();
			for (auto& [id, event] : project.audioEvents)
			{
				if (!tracks.empty() && std::find(tracks.begin(), tracks.end(), event.track) == tracks.end())
					continue;
				const EventSpan span = eventSpan(project, event);
				const bool coversStart = start <= span.start;
				const bool coversEnd = end >= span.end;
				if (end <= span.start || start >= span.end || (coversStart && coversEnd))
					continue;
				const auto frames = [ratio = span.ratio, length = event.length](time::Samples samples)
				{ return std::clamp<time::Samples>(std::llround(static_cast<double>(samples) / ratio), 0, length); };
				Fade in = event.fadeIn;
				Fade out = event.fadeOut;
				// Adjust sets only the fades at the edges the range crosses, both when it is inside the event.
				const bool inSet = fadeIn && (coversStart || kind == "in" || !coversEnd);
				const bool outSet = fadeOut && (coversEnd || kind == "out" || !coversStart);
				if (!inSet && !outSet)
					continue;
				if (inSet)
					in.length = frames(coversStart ? end - span.start : start - span.start);
				if (outSet)
					out.length = frames(coversEnd ? span.end - start : span.end - end);
				// A fade that no longer fits next to the one set gets shorter.
				if (in.length + out.length > event.length)
				{
					if (inSet)
						out.length = event.length - in.length;
					else
						in.length = event.length - out.length;
				}
				event.fadeIn = in;
				event.fadeOut = out;
				changed(event, context);
				events.push_back(id);
			}
			require(!events.empty(), "no audio event on the range's tracks crosses the range");
			return {{"events", events}};
		}

		// Crossfade (reference manual p. 388): each event given with the events next to it on its track that it
		// overlaps, as crossfade.set does it with the default settings; crossfades there already stay.
		Json crossfadeEvents(Project& project, CommandContext& context)
		{
			std::vector<ObjectId> given = context.parameters().at("events").get<std::vector<ObjectId>>();
			for (const ObjectId id : given)
				audioEvent(project, id);
			std::sort(given.begin(), given.end());
			const auto isGiven = [&given](ObjectId id) { return std::binary_search(given.begin(), given.end(), id); };
			// Several events crossfade with each other, one event with its neighbours.
			const bool single = given.size() == 1;
			// Each track's events by start.
			std::map<ObjectId, std::vector<std::pair<EventSpan, AudioEvent*>>> tracks;
			for (auto& [id, event] : project.audioEvents)
				tracks[event.track].emplace_back(eventSpan(project, event), &event);
			std::vector<ObjectId> events;
			for (auto& [track, row] : tracks)
			{
				std::sort(
					row.begin(), row.end(), [](const auto& a, const auto& b) { return a.first.start < b.first.start; });
				for (size_t i = 1; i < row.size(); ++i)
				{
					const EventSpan& out = row[i - 1].first;
					const EventSpan& in = row[i].first;
					const AudioEvent& from = *row[i - 1].second;
					AudioEvent& to = *row[i].second;
					const bool chosen =
						single ? isGiven(from.id) || isGiven(to.id) : isGiven(from.id) && isGiven(to.id);
					if (!chosen || out.start >= in.start || in.start >= out.end || out.end > in.end)
						continue;
					if (to.crossfade.from != from.id)
					{
						for (auto& [id, other] : project.audioEvents)
							if (other.crossfade.from == from.id)
							{
								other.crossfade = Crossfade();
								changed(other, context);
							}
						to.crossfade = Crossfade();
						to.crossfade.from = from.id;
						changed(to, context);
					}
					events.push_back(to.id);
				}
			}
			require(!events.empty(), "the events don't overlap an event next to them on their track");
			std::sort(events.begin(), events.end());
			return {{"events", events}};
		}

		// Remove Fade In, Remove Fade Out and Remove Fades (reference manual pp. 385, 395).
		Json removeEventFades(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const bool fadeIn = parameters.value("fadeIn", true);
			const bool fadeOut = parameters.value("fadeOut", true);
			std::vector<ObjectId> ids = parameters.at("events").get<std::vector<ObjectId>>();
			std::sort(ids.begin(), ids.end());
			ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
			for (const ObjectId id : ids)
			{
				AudioEvent& event = audioEvent(project, id);
				if (fadeIn)
				{
					event.fadeIn.length = 0;
					event.crossfade = Crossfade();
				}
				if (fadeOut)
				{
					event.fadeOut.length = 0;
					for (auto& [otherId, other] : project.audioEvents)
						if (other.crossfade.from == id)
						{
							other.crossfade = Crossfade();
							changed(other, context);
						}
				}
				changed(event, context);
			}
			return {{"events", ids}};
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
				project.autoFades = settings.value_or(project.autoFades); // useProject is for tracks only
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

	EventSpan eventSpan(const Project& project, const AudioEvent& event)
	{
		EventSpan result;
		result.ratio = project.sampleRate / project.media.at(project.clips.at(event.clip).media).sampleRate;
		result.start = time::toSamples(event.position, project.tempo, project.sampleRate);
		result.end = result.start + std::llround(static_cast<double>(event.length) * result.ratio);
		if (warped(project.clips.at(event.clip)))
			result.end = result.start +
				std::llround(warpedOffset(project, event, static_cast<double>(event.offset + event.length)));
		return result;
	}

	bool crossfadeApplies(const Project& project, const AudioEvent& to)
	{
		const auto from = project.audioEvents.find(to.crossfade.from);
		if (to.crossfade.from == 0 || from == project.audioEvents.end())
			return false;
		const AudioEvent& out = from->second;
		if (out.track != to.track || out.mute || to.mute)
			return false;
		const EventSpan a = eventSpan(project, out);
		const EventSpan b = eventSpan(project, to);
		return a.start < b.start && b.start < a.end && a.end <= b.end;
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
				"crossfade replaces the fades at the ends it joins while it plays.",
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
				"event only. Changing a crossfade keeps what isn't given. After an edit that moves the events apart, "
				"the crossfade rests and both play their own fades until they overlap like that again. Returns to.",
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
				"an event has no fade of its own at that end (they never stack), crossfades where an event starts on "
				"top of another one or ends above one that goes on, unless they crossfade already; there an event's "
				"own fade is the crossfade's, mirrored by the event behind. length is 1 to 500 ms; shapes as for "
				"audioEvent.setFades; crossfadeMode equalGain or equalPower, with crossfadeShape the fade in as for "
				"crossfade.set. A track uses the project's settings until it gets its own; useProject: true goes back "
				"to them. Returns the settings that apply.",
				object({{"track", id}, {"useProject", boolean}, {"fadeIn", boolean}, {"fadeOut", boolean},
						   {"crossfades", boolean}, {"length", number}, {"fadeInShape", shapes},
						   {"fadeOutShape", shapes}, {"crossfadeMode", modes}, {"crossfadeShape", shapes}},
					{}),
				result("autoFades"), false},
			setAutoFades);

		const Json ids = {{"type", "array"}, {"items", id}};
		const Json eventsResult = object({{"events", ids}}, {"events"});
		addEdit(engine,
			{"range.fades", 1,
				"The Fades menu's range functions on the audio events on tracks (ids, default every track) that cross "
				"the range from start to end (project samples). fade in (Fade In to Range Start): fades in from the "
				"event's start to the range's start, or to its end where the range covers the event's start. out "
				"(Fade Out from Range End): fades out from the range's end to the event's end, or from its start where "
				"the range covers the event's end. adjust (Adjust Fades to Range): the fades at the edges of the event "
				"that the range covers, both when the range is inside it. Shapes stay; a fade that no longer fits next "
				"to the one set gets shorter. Returns the events changed.",
				object({{"start", integer}, {"end", integer}, {"tracks", ids},
						   {"fade", {{"type", "string"}, {"enum", {"in", "out", "adjust"}}}}},
					{"start", "end", "fade"}),
				eventsResult, false},
			"Fades to Range", fadesToRange);
		addEdit(engine,
			{"event.crossfade", 1,
				"Crossfades audio events with the overlapping ones next to them on their tracks, with the default "
				"settings of crossfade.set: several events with each other where one starts inside the one before it "
				"and ends after it, one event with the events on either side of it like that. Crossfades there already "
				"stay as they are. Returns the events that fade in.",
				object({{"events", ids}}, {"events"}), eventsResult, false},
			"Crossfade", crossfadeEvents);
		addEdit(engine,
			{"event.removeFades", 1,
				"Removes the fade in (fadeIn, default true) and the fade out (fadeOut, default true) of audio events, "
				"with the crossfades at those ends. Returns the events.",
				object({{"events", ids}, {"fadeIn", boolean}, {"fadeOut", boolean}}, {"events"}), eventsResult, false},
			"Remove Fades", removeEventFades);
	}
}
