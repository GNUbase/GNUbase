// SPDX-License-Identifier: GPL-3.0-or-later
// Time signature events (#156, reference manual pp. 163, 1468-1470): signature.add, signature.set and signature.remove
// edit the project's signature map, whose first event the transport bar's signature (project.set) also sets. Each
// command is one undo step and changes nothing if it fails (ProjectModel::add).
#include "engine/document/document.h"
#include "engine/document/projectmodel.h"
#include "engine/engine.h"

#include <algorithm>
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

		Json objectSchema(Json properties, const std::vector<std::string>& required = {})
		{
			return {{"type", "object"}, {"properties", std::move(properties)}, {"required", required},
				{"additionalProperties", false}};
		}

		int checkedNumerator(const Json& value)
		{
			const int numerator = value.get<int>();
			require(numerator >= 1 && numerator <= 99, "the signature's numerator must be from 1 to 99");
			return numerator;
		}

		int checkedDenominator(const Json& value)
		{
			const int denominator = value.get<int>();
			require(denominator == 1 || denominator == 2 || denominator == 4 || denominator == 8 || denominator == 16,
				"the signature's denominator must be 1, 2, 4, 8 or 16");
			return denominator;
		}

		int64_t checkedBar(const Json& value)
		{
			const auto bar = value.get<int64_t>();
			require(bar >= 0, "a bar must not be negative");
			return bar;
		}

		std::vector<time::SignatureChange>::iterator findBar(std::vector<time::SignatureChange>& changes, int64_t bar)
		{
			const auto found = std::find_if(changes.begin(), changes.end(),
				[bar](const time::SignatureChange& change) { return change.bar == bar; });
			require(found != changes.end(), "no time signature event at bar " + std::to_string(bar));
			return found;
		}

		Json signaturesJson(const Project& project)
		{
			Json signatures = Json::array();
			for (const time::SignatureChange& change : project.signatures.changes())
				signatures.push_back({{"bar", change.bar}, {"numerator", change.signature.numerator},
					{"denominator", change.signature.denominator}});
			return signatures;
		}

		// The signature map as the given changes, which must make one.
		Json setSignatures(Project& project, std::vector<time::SignatureChange> changes, CommandContext& context)
		{
			std::sort(changes.begin(), changes.end(),
				[](const time::SignatureChange& a, const time::SignatureChange& b) { return a.bar < b.bar; });
			for (size_t i = 1; i < changes.size(); ++i)
				require(changes[i].bar != changes[i - 1].bar,
					"two time signature events at bar " + std::to_string(changes[i].bar));
			project.signatures = time::SignatureMap(changes);
			Json result = {{"signatures", signaturesJson(project)}};
			context.emit("signature.changed", result);
			return result;
		}

		Json addSignature(Project& project, CommandContext& context)
		{
			const Json& parameters = context.parameters();
			const Json& at = parameters.at("bar");
			int64_t bar = 0;
			if (at.is_string())
			{
				require(at.get<std::string>() == "cursor", "bar must be a number or \"cursor\"");
				const auto cursor = context.call("transport.get").at("position").get<time::Samples>();
				bar = project.signatures.toBarsBeats(project.tempo.toTicks(cursor, project.sampleRate)).bar;
			}
			else
			{
				bar = checkedBar(at);
			}
			const time::TimeSignature signature = {
				checkedNumerator(parameters.at("numerator")), checkedDenominator(parameters.at("denominator"))};
			std::vector<time::SignatureChange> changes = project.signatures.changes();
			const auto found = std::find_if(changes.begin(), changes.end(),
				[bar](const time::SignatureChange& change) { return change.bar == bar; });
			if (found != changes.end())
				found->signature = signature;
			else
				changes.push_back({bar, signature});
			Json result = setSignatures(project, std::move(changes), context);
			result["bar"] = bar;
			return result;
		}

		Json setSignature(Project& project, CommandContext& context)
		{
			std::vector<time::SignatureChange> before = project.signatures.changes();
			std::vector<time::SignatureChange> changes = before;
			std::vector<int64_t> changed;
			for (const Json& item : context.parameters().at("events"))
			{
				const auto bar = item.at("bar").get<int64_t>();
				require(std::find(changed.begin(), changed.end(), bar) == changed.end(),
					"time signature event " + std::to_string(bar) + " is changed twice");
				changed.push_back(bar);
				// Bars name the events as they were; the moves all happen at once.
				time::SignatureChange& change = changes.at(static_cast<size_t>(findBar(before, bar) - before.begin()));
				if (item.contains("to"))
				{
					const int64_t to = checkedBar(item.at("to"));
					require(bar != 0 || to == 0, "the first time signature event stays at the first bar");
					require(to != 0 || bar == 0, "only the first time signature event is at the first bar");
					change.bar = to;
				}
				if (item.contains("numerator"))
					change.signature.numerator = checkedNumerator(item.at("numerator"));
				if (item.contains("denominator"))
					change.signature.denominator = checkedDenominator(item.at("denominator"));
			}
			return setSignatures(project, std::move(changes), context);
		}

		Json removeSignature(Project& project, CommandContext& context)
		{
			std::vector<time::SignatureChange> changes = project.signatures.changes();
			for (const Json& item : context.parameters().at("bars"))
			{
				const auto bar = item.get<int64_t>();
				require(bar != 0, "the first time signature event can't be removed");
				changes.erase(findBar(changes, bar));
			}
			return setSignatures(project, std::move(changes), context);
		}
	}

	void ProjectModel::registerSignatureCommands(Engine& engine)
	{
		const Json barSchema = {{"type", "integer"}};
		const Json valueSchema = {{"type", "integer"}};
		const Json signaturesSchema = {{"type", "array"},
			{"items",
				objectSchema({{"bar", barSchema}, {"numerator", valueSchema}, {"denominator", valueSchema}},
					{"bar", "numerator", "denominator"})}};
		const Json result = {{"type", "object"}, {"properties", {{"signatures", signaturesSchema}, {"bar", barSchema}}},
			{"required", {"signatures"}}};

		add(engine,
			{"signature.add", 1,
				"Adds a time signature event at bar (zero-based, or \"cursor\": the bar the project cursor is in) "
				"with a signature of numerator (1 to 99) beats of denominator (1, 2, 4, 8 or 16), or changes the one "
				"there. The bars from there to the next event have that signature; a short first bar is a pickup "
				"bar. Musical positions stay in ticks, so events, parts and markers keep theirs while the bars move "
				"under them. Returns the bar and the project's signatures as project.get and signature.changed "
				"report them.",
				objectSchema({{"bar", {{"type", {"integer", "string"}}}}, {"numerator", valueSchema},
								 {"denominator", valueSchema}},
					{"bar", "numerator", "denominator"}),
				result, false},
			addSignature);
		add(engine,
			{"signature.set", 1,
				"Changes time signature events, each named by its bar: to moves it to another bar (the first stays at "
				"bar 0, and no two events share a bar), numerator and denominator change its signature. All changes "
				"happen at once.",
				objectSchema({{"events",
								 {{"type", "array"},
									 {"items",
										 objectSchema({{"bar", barSchema}, {"to", barSchema},
														  {"numerator", valueSchema}, {"denominator", valueSchema}},
											 {"bar"})}}}},
					{"events"}),
				result, false},
			setSignature);
		add(engine,
			{"signature.remove", 1, "Removes time signature events by their bars. The first one stays.",
				objectSchema({{"bars", {{"type", "array"}, {"items", barSchema}}}}, {"bars"}), result, false},
			removeSignature);
	}
}
