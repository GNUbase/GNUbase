// SPDX-License-Identifier: GPL-3.0-or-later
// The arranger track (#167, reference manual pp. 164, 404-413): arranger events and arranger chains. arranger.add,
// arranger.set, arranger.remove, arranger.addChain, arranger.setChain and arranger.removeChain change the project, so
// each is one undo step and changes nothing if it fails (ProjectModel::add). arranger.setMode changes arranger mode,
// the jump mode and the active chain, settings saved with the project but not undo steps. arranger.get is a query.
// arranger.flatten is in eventedits.cpp with the range edits it is made of; arranger.goToStep and arranger.trigger,
// which move the transport, are the audio engine's. The plan the transport plays is compiled here (arrangerPlan).
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/document/rendersnapshot.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <algorithm>
#include <cstdint>
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

		const Track* arrangerTrack(const Project& project)
		{
			for (const auto& [id, track] : project.trackById)
				if (track.type == TrackType::Arranger)
					return &track;
			return nullptr;
		}

		const Track& requireTrack(const Project& project)
		{
			const Track* track = arrangerTrack(project);
			require(track != nullptr, "the project has no arranger track");
			return *track;
		}

		time::Samples samples(const Project& project, const time::Position& position)
		{
			return time::toSamples(position, project.tempo, project.sampleRate);
		}

		// Names in alphabetical order (reference manual p. 405): A to Z, then AA, AB and so on; the first one free.
		std::string nextName(const Project& project)
		{
			std::set<std::string> taken;
			for (const ArrangerEvent& event : project.arranger.events)
				taken.insert(event.name);
			for (int64_t n = 0;; ++n)
			{
				std::string name;
				for (int64_t k = n; k >= 0; k = (k / 26) - 1)
					name.insert(name.begin(), static_cast<char>('A' + (k % 26)));
				if (taken.count(name) == 0)
					return name;
			}
		}

		ArrangerEvent& findEvent(Project& project, ObjectId id)
		{
			for (ArrangerEvent& event : project.arranger.events)
				if (event.id == id)
					return event;
			throw CommandError("no arranger event " + std::to_string(id));
		}

		ArrangerChain& findChain(Project& project, const Json& parameters)
		{
			requireTrack(project);
			const ArrangerChain* active = activeArrangerChain(project);
			const ObjectId id = parameters.contains("chain") ? parameters.at("chain").get<ObjectId>()
				: active != nullptr                          ? active->id
															 : 0;
			for (ArrangerChain& chain : project.arranger.chains)
				if (chain.id == id)
					return chain;
			throw CommandError("no arranger chain " + std::to_string(id));
		}

		Json eventJson(const Project& project, const ArrangerEvent& event)
		{
			return {{"id", event.id}, {"name", event.name}, {"position", event.position.value},
				{"end", event.end.value}, {"start", samples(project, event.position)},
				{"endSamples", samples(project, event.end)}};
		}

		Json chainJson(const ArrangerChain& chain)
		{
			Json steps = Json::array();
			for (const ArrangerStep& step : chain.steps)
				steps.push_back({{"event", step.event}, {"repeats", step.repeats},
					{"mode", g_arrangerRepeatModes.at(static_cast<size_t>(step.mode))}});
			return {{"id", chain.id}, {"name", chain.name}, {"steps", steps}};
		}

		void announce(const Project& project, CommandContext& context)
		{
			context.emit("arranger.changed", arrangerJson(project));
		}

		// Moves and renames an event from the parameters: position (in the track's time base) moves it, keeping its
		// length unless end is given.
		void change(const Project& project, ArrangerEvent& event, const Json& parameters)
		{
			if (parameters.contains("position"))
			{
				const int64_t position = parameters.at("position").get<int64_t>();
				require(position >= 0, "position must not be before the project start");
				event.end.value += position - event.position.value;
				event.position.value = position;
			}
			if (parameters.contains("end"))
				event.end.value = parameters.at("end").get<int64_t>();
			if (parameters.contains("name"))
				event.name = parameters.at("name").get<std::string>();
			require(samples(project, event.end) > samples(project, event.position),
				"an arranger event ends at least one sample after its position");
		}

		std::vector<ArrangerStep> stepsFrom(const Project& project, const Json& json)
		{
			std::vector<ArrangerStep> steps;
			for (const Json& item : json)
			{
				ArrangerStep step;
				step.event = item.at("event").get<ObjectId>();
				require(std::any_of(project.arranger.events.begin(), project.arranger.events.end(),
							[&step](const ArrangerEvent& event) { return event.id == step.event; }),
					"no arranger event " + std::to_string(step.event));
				step.repeats = item.value("repeats", 1);
				require(step.repeats >= 1 && step.repeats <= 999, "repeats must be 1 to 999");
				const std::string mode = item.value("mode", std::string("normal"));
				const auto found = std::find(g_arrangerRepeatModes.begin(), g_arrangerRepeatModes.end(), mode);
				require(found != g_arrangerRepeatModes.end(), "unknown repeat mode " + mode);
				step.mode = static_cast<ArrangerRepeat>(found - g_arrangerRepeatModes.begin());
				steps.push_back(step);
			}
			return steps;
		}

		// "Chain N" with the first number from the number of chains on that no chain has.
		std::string chainName(const Project& project)
		{
			for (size_t n = project.arranger.chains.size() + 1;; ++n)
			{
				const std::string name = "Chain " + std::to_string(n);
				if (std::none_of(project.arranger.chains.begin(), project.arranger.chains.end(),
						[&name](const ArrangerChain& chain) { return chain.name == name; }))
					return name;
			}
		}

		// Where a pending jump may happen in a section (ArrangerSection::jumps), for the jump mode.
		std::vector<time::Samples> jumpPoints(const Project& project, time::Samples start, time::Samples end)
		{
			std::vector<time::Samples> points;
			const ArrangerJump jump = project.arrangerJump;
			if (jump == ArrangerJump::None)
				return points;
			if (jump != ArrangerJump::End)
			{
				const time::TempoMap& tempo = project.tempo;
				const time::SignatureMap& signatures = project.signatures;
				const double rate = project.sampleRate;
				const time::Ticks from = tempo.toTicks(start, rate);
				int64_t bar = signatures.toBarsBeats(from).bar;
				if (tempo.toSamples(signatures.barStart(bar), rate) < start)
					++bar; // the first bar line at or after the start
				const int64_t every = jump == ArrangerJump::FourBars ? 4 : jump == ArrangerJump::TwoBars ? 2 : 1;
				// ponytail: one point per beat or bar of the section; a grid per section is enough for any song.
				for (int64_t b = jump == ArrangerJump::OneBeat ? bar - 1 : bar;; ++b)
				{
					const time::Ticks barStart = signatures.barStart(b);
					if (tempo.toSamples(barStart, rate) >= end)
						break;
					if (jump == ArrangerJump::OneBeat)
					{
						const time::TimeSignature signature = signatures.signatureAt(barStart);
						const time::Ticks beat = time::g_ticksPerQuarter * 4 / signature.denominator;
						for (int k = 0; k < signature.numerator; ++k)
						{
							const time::Samples at = tempo.toSamples(barStart + (k * beat), rate);
							if (at > start && at < end)
								points.push_back(at);
						}
					}
					else if ((b - bar) % every == 0)
					{
						const time::Samples at = tempo.toSamples(barStart, rate);
						if (at > start)
							points.push_back(at);
					}
				}
			}
			points.push_back(end);
			return points;
		}
	}

	ArrangerPlan arrangerPlan(const Project& project)
	{
		ArrangerPlan plan;
		plan.active = project.arrangerMode;
		plan.jump = project.arrangerJump;
		for (const ArrangerEvent& event : project.arranger.events)
		{
			const time::Samples start = samples(project, event.position);
			const time::Samples end = std::max(start + 1, samples(project, event.end));
			plan.sections.push_back({start, end, jumpPoints(project, start, end), event.id});
		}
		if (const ArrangerChain* chain = activeArrangerChain(project))
			for (const ArrangerStep& step : chain->steps)
			{
				const auto found = std::find_if(project.arranger.events.begin(), project.arranger.events.end(),
					[&step](const ArrangerEvent& event) { return event.id == step.event; });
				if (found != project.arranger.events.end())
					plan.steps.push_back({static_cast<int32_t>(found - project.arranger.events.begin()),
						static_cast<int32_t>(step.repeats), step.mode});
			}
		return plan;
	}

	void ProjectModel::registerArrangerCommands(Engine& engine)
	{
		const Json idSchema = {{"type", "integer"}};
		const Json integerSchema = {{"type", "integer"}};
		const Json stringSchema = {{"type", "string"}};
		const Json booleanSchema = {{"type", "boolean"}};
		const Json eventResult = {
			{"type", "object"}, {"properties", {{"event", {{"type", "object"}}}}}, {"required", {"event"}}};
		const Json chainResult = {
			{"type", "object"}, {"properties", {{"chain", {{"type", "object"}}}}}, {"required", {"chain"}}};
		const Json stepSchema = {{"type", "object"},
			{"properties",
				{{"event", idSchema}, {"repeats", integerSchema},
					{"mode", {{"type", "string"}, {"enum", g_arrangerRepeatModes}}}}},
			{"required", {"event"}}, {"additionalProperties", false}};
		const Json arrangerResult = {{"type", "object"},
			{"properties",
				{{"track", integerSchema}, {"events", {{"type", "array"}}}, {"chains", {{"type", "array"}}},
					{"chain", integerSchema}, {"mode", booleanSchema}, {"jumpMode", stringSchema}}},
			{"required", {"track", "events", "chains", "chain", "mode", "jumpMode"}}};

		add(engine,
			{"arranger.add", 1,
				"Adds an arranger event to the arranger track, which is added if the project has none. position and "
				"end are in the track's time base (ticks or samples) and default to the left and right locators; the "
				"event ends at least one sample after its position. Without a name it gets the first free one in "
				"alphabetical order: A, B, ..., Z, AA and so on.",
				objectSchema({{"position", integerSchema}, {"end", integerSchema}, {"name", stringSchema}}),
				eventResult, false},
			[this](Project& project, CommandContext& context)
			{
				if (arrangerTrack(project) == nullptr)
					runEdit("track.add", project, {{"type", "arranger"}}, context);
				const Track& track = requireTrack(project);
				const Json& parameters = context.parameters();
				ArrangerEvent event;
				event.id = project.newId();
				event.name = parameters.value("name", nextName(project));
				if (!parameters.contains("position") || !parameters.contains("end"))
				{
					const Json transport = context.call("transport.get");
					const auto at = [&](const char* key)
					{
						return time::rebase({time::TimeBase::Linear, transport.at(key).get<time::Samples>()},
							track.timeBase, project.tempo, project.sampleRate);
					};
					event.position = at("left");
					event.end = at("right");
				}
				event.position.base = track.timeBase;
				event.end.base = track.timeBase;
				if (parameters.contains("position"))
					event.position.value = parameters.at("position").get<int64_t>();
				if (parameters.contains("end"))
					event.end.value = parameters.at("end").get<int64_t>();
				require(event.position.value >= 0, "position must not be before the project start");
				require(samples(project, event.end) > samples(project, event.position),
					"an arranger event ends at least one sample after its position");
				project.arranger.events.push_back(event);
				announce(project, context);
				return Json{{"event", eventJson(project, event)}};
			});
		add(engine,
			{"arranger.set", 1,
				"Changes an arranger event: moves it (position, in the track's time base; it keeps its length unless "
				"end is given), changes its end or renames it.",
				objectSchema(
					{{"event", idSchema}, {"position", integerSchema}, {"end", integerSchema}, {"name", stringSchema}},
					{"event"}),
				eventResult, false},
			[](Project& project, CommandContext& context)
			{
				ArrangerEvent& event = findEvent(project, context.parameters().at("event").get<ObjectId>());
				change(project, event, context.parameters());
				announce(project, context);
				return Json{{"event", eventJson(project, event)}};
			});
		add(engine,
			{"arranger.remove", 1, "Removes arranger events, and their steps from every arranger chain.",
				objectSchema({{"events", {{"type", "array"}, {"items", idSchema}}}}, {"events"}), Json::object(),
				false},
			[](Project& project, CommandContext& context)
			{
				const auto ids = context.parameters().at("events").get<std::set<ObjectId>>();
				for (const ObjectId id : ids)
					findEvent(project, id);
				std::vector<ArrangerEvent>& events = project.arranger.events;
				events.erase(std::remove_if(events.begin(), events.end(),
								 [&ids](const ArrangerEvent& event) { return ids.count(event.id) != 0; }),
					events.end());
				for (ArrangerChain& chain : project.arranger.chains)
					chain.steps.erase(std::remove_if(chain.steps.begin(), chain.steps.end(),
										  [&ids](const ArrangerStep& step) { return ids.count(step.event) != 0; }),
						chain.steps.end());
				announce(project, context);
				return Json::object();
			});
		add(engine,
			{"arranger.addChain", 1,
				"Adds an arranger chain, empty or with duplicate a copy of the active chain's steps (Create New Chain "
				"and Duplicate Current Chain), and makes it the active one. Without a name it is \"Chain N\".",
				objectSchema({{"name", stringSchema}, {"duplicate", booleanSchema}}), chainResult, false},
			[](Project& project, CommandContext& context)
			{
				requireTrack(project);
				const Json& parameters = context.parameters();
				ArrangerChain chain;
				chain.id = project.newId();
				chain.name = parameters.value("name", chainName(project));
				if (parameters.value("duplicate", false))
					if (const ArrangerChain* active = activeArrangerChain(project))
						chain.steps = active->steps;
				project.arranger.chains.push_back(chain);
				project.arrangerChain = chain.id;
				announce(project, context);
				return Json{{"chain", chainJson(chain)}};
			});
		add(engine,
			{"arranger.setChain", 1,
				"Changes an arranger chain (default: the active one): renames it, or gives it new steps, each an "
				"arranger event played repeats times (1 to 999, default 1) in a repeat mode: normal (then the next "
				"step), repeatForever (until another step or event is chosen) or pauseAfterRepeats (then playback "
				"stops at the next step). Appending, moving and copying steps give the whole list; removeSteps "
				"removes the steps at those indices (0 for the first; Remove Touched and Remove Selected).",
				objectSchema(
					{{"chain", idSchema}, {"name", stringSchema}, {"steps", {{"type", "array"}, {"items", stepSchema}}},
						{"removeSteps", {{"type", "array"}, {"items", integerSchema}}}}),
				chainResult, false},
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				ArrangerChain& chain = findChain(project, parameters);
				if (parameters.contains("name"))
					chain.name = parameters.at("name").get<std::string>();
				if (parameters.contains("steps"))
					chain.steps = stepsFrom(project, parameters.at("steps"));
				if (parameters.contains("removeSteps")) // after the new steps, if any
				{
					const auto indices = parameters.at("removeSteps").get<std::set<int64_t>>();
					for (auto index = indices.rbegin(); index != indices.rend(); ++index)
					{
						require(*index >= 0 && static_cast<size_t>(*index) < chain.steps.size(),
							"the chain has no step " + std::to_string(*index));
						chain.steps.erase(chain.steps.begin() + *index);
					}
				}
				announce(project, context);
				return Json{{"chain", chainJson(chain)}};
			});
		add(engine,
			{"arranger.removeChain", 1,
				"Removes an arranger chain (default: the active one), unless it is the only one. The first chain is "
				"active if the active one goes.",
				objectSchema({{"chain", idSchema}}), Json::object(), false},
			[](Project& project, CommandContext& context)
			{
				const ObjectId id = findChain(project, context.parameters()).id;
				require(project.arranger.chains.size() > 1, "the arranger track keeps its last chain");
				std::vector<ArrangerChain>& chains = project.arranger.chains;
				chains.erase(std::remove_if(chains.begin(), chains.end(),
								 [id](const ArrangerChain& chain) { return chain.id == id; }),
					chains.end());
				if (project.arrangerChain == id)
					project.arrangerChain = 0;
				announce(project, context);
				return Json::object();
			});
		add(engine,
			{"arranger.get", 1,
				"The arranger: the arranger track (0 for none), its events in timeline order (positions in the "
				"track's time base, start and endSamples in samples), the chains with their steps, the active chain, "
				"whether arranger mode is on and the jump mode.",
				objectSchema({}), arrangerResult, true},
			[](Project& project, CommandContext&) { return arrangerJson(project); });

		// Settings saved with the project, not undo steps: a change publishes the plan the transport plays.
		CommandInfo setInfo{"arranger.setMode", 1,
			"Changes the arranger's playback settings: mode (Activate Arranger Mode: playback follows the active chain "
			"instead of the cycle), chain (the active chain, Select Active Chain) and jumpMode (Jump Mode: when a step "
			"chosen with arranger.goToStep or an event triggered with arranger.trigger takes over while playing: none "
			"at "
			"once, 4bars or 2bars every 4 or 2 bars from the event's start, 1bar at the next bar line, 1beat at the "
			"next "
			"beat, end at the end of the event playing). Saved with the project, not undo steps.",
			objectSchema({{"mode", booleanSchema}, {"chain", idSchema},
				{"jumpMode", {{"type", "string"}, {"enum", g_arrangerJumpModes}}}}),
			arrangerResult, false};
		Handler set = [](Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const ObjectId chain =
				parameters.contains("chain") ? findChain(project, parameters).id : project.arrangerChain;
			const std::string name =
				parameters.value("jumpMode", g_arrangerJumpModes.at(static_cast<size_t>(project.arrangerJump)));
			const auto found = std::find(g_arrangerJumpModes.begin(), g_arrangerJumpModes.end(), name);
			require(found != g_arrangerJumpModes.end(), "unknown jump mode " + name);
			project.arrangerChain = chain;
			project.arrangerMode = parameters.value("mode", project.arrangerMode);
			project.arrangerJump = static_cast<ArrangerJump>(found - g_arrangerJumpModes.begin());
			Json result = arrangerJson(project);
			context.emit("arranger.changed", result);
			return result;
		};
		m_edits[setInfo.name] = {setInfo, set};
		engine.registerCommand(std::move(setInfo),
			[this, set = std::move(set)](CommandContext& context)
			{
				Json result = set(m_project, context);
				publish();
				return result;
			});
	}
}
