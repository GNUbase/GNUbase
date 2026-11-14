// SPDX-License-Identifier: GPL-3.0-or-later
// The transpose track (#168, reference manual pp. 165, 414-421): transpose events, the track's Mute Transpose Events,
// Keep Transpose in Octave Range and Lock, and Set Root Key for Unassigned Events. transpose.add, transpose.set,
// transpose.remove, transpose.setTrack and transpose.assignRootKey change the project, so each is one undo step and
// changes nothing if it fails (ProjectModel::add); transpose.get is a query. The project root key is project.set's,
// a part's root key and Global Transpose midiPart.set's; globalTranspose (document.cpp) is what playback adds.
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <algorithm>
#include <set>
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

		const Track* transposeTrack(const Project& project)
		{
			for (const auto& [id, track] : project.trackById)
				if (track.type == TrackType::Transpose)
					return &track;
			return nullptr;
		}

		// The transpose track's events, for a change: refused without the track or while it is locked.
		std::vector<TransposeEvent>& editableEvents(Project& project)
		{
			require(transposeTrack(project) != nullptr, "the project has no transpose track");
			require(!project.transpose.lock, "the transpose track is locked");
			return project.transpose.events;
		}

		int valueFrom(const Json& value)
		{
			const int semitones = value.get<int>();
			require(semitones >= -24 && semitones <= 24, "a transpose event's value must be -24 to 24 semitones");
			return semitones;
		}

		// Sorts the events into timeline order and refuses two at the same position; announces them.
		void settle(Project& project, CommandContext& context)
		{
			std::vector<TransposeEvent>& events = project.transpose.events;
			std::stable_sort(events.begin(), events.end(),
				[](const TransposeEvent& a, const TransposeEvent& b) { return a.position.value < b.position.value; });
			for (size_t i = 1; i < events.size(); ++i)
				require(events[i].position.value != events[i - 1].position.value,
					"there is a transpose event at that position already");
			context.emit("transpose.changed", transposeJson(project));
		}

		Json eventJson(const Project& project, ObjectId id)
		{
			const Json transpose = transposeJson(project);
			for (const Json& event : transpose.at("events"))
				if (event.at("id") == id)
					return event;
			return Json::object();
		}
	}

	void ProjectModel::registerTransposeCommands(Engine& engine)
	{
		const Json idSchema = {{"type", "integer"}};
		const Json integerSchema = {{"type", "integer"}};
		const Json booleanSchema = {{"type", "boolean"}};
		const Json eventResult = {
			{"type", "object"}, {"properties", {{"event", {{"type", "object"}}}}}, {"required", {"event"}}};
		const Json transposeResult = {{"type", "object"},
			{"properties",
				{{"track", integerSchema}, {"events", {{"type", "array"}}}, {"mute", booleanSchema},
					{"keepInOctave", booleanSchema}, {"lock", booleanSchema}}},
			{"required", {"track", "events", "mute", "keepInOctave", "lock"}}};
		const Json eventSchema =
			objectSchema({{"event", idSchema}, {"position", integerSchema}, {"value", integerSchema}}, {"event"});

		add(engine,
			{"transpose.add", 1,
				"Adds a transpose event to the transpose track, which is added if the project has none. From its "
				"position (in the track's time base, ticks or samples; default the cursor) to the next event, MIDI "
				"parts that follow global transpose play value semitones (-24 to 24, default 0) higher; before the "
				"first event nothing is transposed. Refused while the track is locked and where an event is.",
				objectSchema({{"position", integerSchema}, {"value", integerSchema}}), eventResult, false},
			[this](Project& project, CommandContext& context)
			{
				if (transposeTrack(project) == nullptr)
					runEdit("track.add", project, {{"type", "transpose"}}, context);
				const time::TimeBase base = transposeTrack(project)->timeBase;
				std::vector<TransposeEvent>& events = editableEvents(project);
				const Json& parameters = context.parameters();
				TransposeEvent event;
				event.id = project.newId();
				if (parameters.contains("position"))
					event.position = {base, parameters.at("position").get<int64_t>()};
				else
					event.position = time::rebase(
						{time::TimeBase::Linear, context.call("transport.get").at("position").get<int64_t>()}, base,
						project.tempo, project.sampleRate);
				require(event.position.value >= 0, "position must not be before the project start");
				event.value = valueFrom(parameters.value("value", Json(0)));
				events.push_back(event);
				settle(project, context);
				return Json{{"event", eventJson(project, event.id)}};
			});
		add(engine,
			{"transpose.set", 1,
				"Changes transpose events: each moves to position (in the transpose track's time base) or gets a "
				"new value (-24 to 24 semitones). Refused while the track is locked.",
				objectSchema({{"events", {{"type", "array"}, {"items", eventSchema}}}}, {"events"}), Json::object(),
				false},
			[](Project& project, CommandContext& context)
			{
				std::vector<TransposeEvent>& events = editableEvents(project);
				for (const Json& item : context.parameters().at("events"))
				{
					const auto id = item.at("event").get<ObjectId>();
					const auto found = std::find_if(
						events.begin(), events.end(), [id](const TransposeEvent& event) { return event.id == id; });
					require(found != events.end(), "no transpose event " + std::to_string(id));
					if (item.contains("position"))
					{
						found->position.value = item.at("position").get<int64_t>();
						require(found->position.value >= 0, "position must not be before the project start");
					}
					if (item.contains("value"))
						found->value = valueFrom(item.at("value"));
				}
				settle(project, context);
				return Json::object();
			});
		add(engine,
			{"transpose.remove", 1, "Removes transpose events. Refused while the transpose track is locked.",
				objectSchema({{"events", {{"type", "array"}, {"items", idSchema}}}}, {"events"}), Json::object(),
				false},
			[](Project& project, CommandContext& context)
			{
				std::vector<TransposeEvent>& events = editableEvents(project);
				const auto ids = context.parameters().at("events").get<std::set<ObjectId>>();
				for (const ObjectId id : ids)
					require(std::any_of(events.begin(), events.end(),
								[id](const TransposeEvent& event) { return event.id == id; }),
						"no transpose event " + std::to_string(id));
				events.erase(std::remove_if(events.begin(), events.end(),
								 [&ids](const TransposeEvent& event) { return ids.count(event.id) != 0; }),
					events.end());
				settle(project, context);
				return Json::object();
			});
		add(engine,
			{"transpose.setTrack", 1,
				"Changes the transpose track's controls: mute (Mute Transpose Events: the events don't play), "
				"keepInOctave (Keep Transpose in Octave Range: what a part that follows plays — the root key's step, "
				"the transpose events and the part's own transpose together — stays within 7 semitones either way, "
				"8 up plays 4 down) and lock (the events can't be added, changed or removed).",
				objectSchema({{"mute", booleanSchema}, {"keepInOctave", booleanSchema}, {"lock", booleanSchema}}),
				transposeResult, false},
			[](Project& project, CommandContext& context)
			{
				require(transposeTrack(project) != nullptr, "the project has no transpose track");
				const Json& parameters = context.parameters();
				TransposeTrack& transpose = project.transpose;
				transpose.mute = parameters.value("mute", transpose.mute);
				transpose.keepInOctave = parameters.value("keepInOctave", transpose.keepInOctave);
				transpose.lock = parameters.value("lock", transpose.lock);
				Json result = transposeJson(project);
				context.emit("transpose.changed", result);
				return result;
			});
		add(engine,
			{"transpose.assignRootKey", 1,
				"Set Root Key for Unassigned Events: every MIDI part without a root key gets the project root key "
				"(project.set rootKey), so that it follows later changes of it. Refused without a project root key.",
				objectSchema({}), Json::object(), false},
			[](Project& project, CommandContext& context)
			{
				require(project.rootKey >= 0, "the project has no root key");
				for (auto& [id, part] : project.midiParts)
					if (part.rootKey < 0)
					{
						part.rootKey = project.rootKey;
						context.emit("midiPart.changed", {{"part", toJson(part)}});
					}
				// The parts stashed in inactive track versions get it too, so that activating such a version shows
				// them with it (#896). They lie inside their track, which is announced as changed.
				for (auto& [id, track] : project.trackById)
				{
					bool changed = false;
					for (TrackVersion& version : track.versions)
						for (MidiPart& part : version.midiParts)
							if (part.rootKey < 0)
							{
								part.rootKey = project.rootKey;
								changed = true;
							}
					if (changed)
						context.emit("track.changed", {{"track", toJson(track)}});
				}
				return Json::object();
			});
		add(engine,
			{"transpose.get", 1,
				"The transpose track: its id (0 for none), its events in timeline order (id, position in the "
				"track's time base, start in samples, value in semitones), mute, keepInOctave and lock.",
				objectSchema({}), transposeResult, true},
			[](Project& project, CommandContext&) { return transposeJson(project); });
	}
}
