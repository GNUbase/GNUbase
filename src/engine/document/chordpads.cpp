// SPDX-License-Identifier: GPL-3.0-or-later
// The chord pads and the chord assistant (#174, reference manual pp. 1346-1391): pads with chords, the root key, the
// basic player's voicings, playing and recording pads, step input, presets, the List and Circle of Fifths assistants
// and the pads' remote keys. chordPads.set, chordPads.assign, chordPads.modify, chordPads.setPad, chordPads.move,
// chordPads.fromChordTrack, chordPads.loadPreset, chordPads.step and chordPads.toTrack change the project, so each is
// one undo step and changes nothing if it fails (ProjectModel::add), except that a step position move alone is
// navigation, no undo step (ProjectChange::stepPositionBefore); chordPads.get and chordPads.assistant are
// queries. chordPads.trigger plays notes, so it is a control command (ProjectModel::addControl): no undo step, but not
// read-only. The render snapshot carries the remote keys to the audio thread (chordPadKeys).
#include "engine/document/chordpads.h"

#include "engine/document/projectmodel.h"
#include "engine/document/serialize.h"
#include "engine/engine.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
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

		int pitchClass(int pitch)
		{
			return ((pitch % 12) + 12) % 12;
		}

		constexpr std::array<const char*, 7> g_numerals = {"I", "II", "III", "IV", "V", "VI", "VII"};
		constexpr int g_maxPads = 32;
		constexpr std::string_view g_presetsKey = "chordPads.presets";
		// The chord modifiers' remote keys (reference manual p. 1387), in ChordPads::modifierKeys' order.
		constexpr std::array<const char*, 6> g_modifierNames = {
			"previousVoicing", "nextVoicing", "fewerTensions", "moreTensions", "transposeDown", "transposeUp"};

		Json modifierKeysJson(const ChordPads& pads)
		{
			Json keys = Json::object();
			for (size_t i = 0; i < g_modifierNames.size(); ++i)
				keys[g_modifierNames.at(i)] = pads.modifierKeys.at(i);
			return keys;
		}

		// What chordPads.listen listens for, and what chordPads.remoteInput saw of the remote keys (#1089).
		struct RemoteState
		{
			std::set<int> held;       // the pads whose remote keys are down
			int last = -1;            // the pad triggered last, which the chord modifiers change when none is held
			std::string listen;       // "", "assign" or "learn"
			int pad = -1;             // assign: the pad that gets the chord
			std::string target;       // learn: "range" or a modifier's name
			std::set<int> down;       // the notes down while listening
			std::vector<int> pitches; // assign: the notes played; learn range: the keys pressed

			Json listening() const
			{
				if (listen.empty())
					return nullptr;
				return listen == "assign" ? Json{{"assign", pad}} : Json{{"learn", target}};
			}
		};

		// The user's own chord pads presets (chordPads.savePreset), by name.
		Json userPresets(CommandContext& context)
		{
			const Json presets = context.call("settings.get", {{"key", std::string(g_presetsKey)}}).at("value");
			return presets.is_object() ? presets : Json::object();
		}

		// The steps of a preset's scale (major, minor, dorian, ...), the major scale's for another name.
		std::vector<int> scaleOf(const std::string& scale)
		{
			const std::vector<std::string>& names = chordpads::presetNames();
			const std::optional<midi::ScaleType> type = midi::scaleTypeFromName(scale);
			if (std::find(names.begin(), names.end(), scale) == names.end() || !type.has_value())
				return {0, 2, 4, 5, 7, 9, 11};
			return midi::scaleSteps(*type);
		}

		bool inScale(const std::vector<int>& steps, int key, int note)
		{
			return std::find(steps.begin(), steps.end(), pitchClass(note - key)) != steps.end();
		}

		midi::ChordSymbol chordOf(const std::string& text)
		{
			return midi::parseChord(text).value_or(midi::ChordSymbol());
		}

		midi::ChordSymbol chordFrom(const std::string& text)
		{
			const std::optional<midi::ChordSymbol> chord = midi::parseChord(text);
			require(chord.has_value() && chord->root >= 0, "unknown chord " + text);
			return chord.value_or(midi::ChordSymbol());
		}

		std::vector<int> classesOf(const midi::ChordSymbol& chord)
		{
			std::vector<int> classes;
			for (const int interval : midi::chordIntervals(chord))
				classes.push_back(pitchClass(chord.root + interval));
			return classes;
		}

		// The distance of two roots on the circle of fifths, 0 to 6.
		int fifthsDistance(int a, int b)
		{
			const int distance = std::abs(pitchClass(a * 7) - pitchClass(b * 7));
			return std::min(distance, 12 - distance);
		}

		void sortTensions(std::vector<std::string>& tensions)
		{
			const std::vector<std::string>& order = midi::chordTensions();
			std::sort(tensions.begin(), tensions.end(), [&order](const std::string& a, const std::string& b)
				{ return std::find(order.begin(), order.end(), a) < std::find(order.begin(), order.end(), b); });
		}

		ChordPad& padAt(ChordPads& pads, const Json& index)
		{
			const int pad = index.get<int>();
			require(pad >= 0 && pad < static_cast<int>(pads.pads.size()), "no chord pad " + std::to_string(pad));
			return pads.pads[static_cast<size_t>(pad)];
		}

		// The last bar step input writes a chord at: the one the longest project ends in, at the project's tempo, so
		// that the step position, stepBars of any size and the chord events step input writes stay far from
		// overflowing (#1048), also at sample rates far below 8 kHz (longestProjectTicks, #1174).
		int64_t lastStepBar(const Project& project)
		{
			return project.signatures.toBarsBeats(longestProjectTicks(project)).bar;
		}

		// The last step position: the end of the last bar, where a step at that bar leaves it.
		time::Ticks stepEnd(const Project& project)
		{
			return project.signatures.barStart(lastStepBar(project) + 1);
		}

		const ChordPlayer& playerOf(const ChordPads& pads, int index = -1)
		{
			const int player = index >= 0 ? index : pads.player;
			return pads.players.at(
				static_cast<size_t>(std::clamp(player, 0, static_cast<int>(pads.players.size()) - 1)));
		}

		// The players that play: the selected one and those with a track (reference manual p. 1377).
		std::vector<int> playingPlayers(const ChordPads& pads)
		{
			std::vector<int> playing;
			for (int i = 0; i < static_cast<int>(pads.players.size()); ++i)
				if (i == pads.player || pads.players[static_cast<size_t>(i)].track != 0)
					playing.push_back(i);
			return playing;
		}

		// The selected player's voicing of a chord (reference manual pp. 1376-1377): the basic player's moved by its
		// octave offset, the piano and guitar players' in their voicing style; with adaptive, the one nearest to
		// previous.
		std::vector<int> playerVoicing(const ChordPads& pads, const midi::ChordSymbol& chord, bool adaptive = false,
			const std::vector<int>& previous = {}, int index = -1)
		{
			const ChordPlayer& player = playerOf(pads, index);
			if (player.library == "basic")
				return midi::voiceChord(chord, {player.octaveOffset, adaptive}, previous);
			midi::VoicingStyle style;
			style.library = player.library;
			style.style = player.style;
			return midi::voiceChord(chord, {0, adaptive}, previous, style);
		}

		Json playersJson(const ChordPads& pads)
		{
			Json players = Json::array();
			for (const ChordPlayer& player : pads.players)
				players.push_back({{"name", player.name}, {"library", player.library}, {"style", player.style},
					{"octaveOffset", player.octaveOffset}, {"track", player.track}});
			return players;
		}

		std::vector<ChordPlayer> playersFromJson(const Json& json)
		{
			std::vector<ChordPlayer> players;
			const std::vector<std::string>& libraries = midi::voicingLibraries();
			for (const Json& item : json.is_array() ? json : Json::array())
			{
				if (!item.is_object() || players.size() == 8)
					continue;
				ChordPlayer player;
				player.library = item.value("library", std::string("basic"));
				if (std::find(libraries.begin(), libraries.end(), player.library) == libraries.end())
					player.library = "basic";
				player.name = item.value("name", player.name);
				player.style = item.value("style", std::string());
				player.octaveOffset = std::clamp(item.value("octaveOffset", 0), -3, 3);
				player.track = item.value("track", ObjectId{0});
				players.push_back(player);
			}
			return players;
		}

		// A player's track: none (0), or a MIDI or instrument track that no other player has.
		ObjectId playerTrack(const Project& project, ObjectId track, int player)
		{
			if (track == 0)
				return 0;
			const auto found = project.trackById.find(track);
			require(found != project.trackById.end() &&
					(found->second.type == TrackType::Midi || found->second.type == TrackType::Instrument),
				"a chord pads player plays a MIDI or instrument track");
			const std::vector<ChordPlayer>& players = project.chordPads.players;
			for (size_t i = 0; i < players.size(); ++i)
				require(static_cast<int>(i) == player || players[i].track != track,
					"another chord pads player plays track " + std::to_string(track));
			return track;
		}

		// The inversion of a pad's basic voicing that its current voicing is, for a pad with adaptive voicing whose
		// voicing changes by hand.
		int inversionOf(const ChordPads& pads, const midi::ChordSymbol& chord, const std::vector<int>& voicing)
		{
			const std::vector<int> basic = playerVoicing(pads, chord);
			const auto upper = static_cast<int>(basic.size()) - 1;
			for (int steps = -2 * upper; steps <= 2 * upper; ++steps)
				if (chordpads::inverted(basic, steps) == voicing)
					return steps;
			return 0;
		}

		const std::vector<std::string>& overlapNames()
		{
			static const std::vector<std::string> names = {"holdFirst", "legato", "stopFirst"};
			return names;
		}

		midi::PadOverlap overlapOf(const std::string& name)
		{
			if (name == "legato")
				return midi::PadOverlap::Legato;
			return name == "stopFirst" ? midi::PadOverlap::StopFirst : midi::PadOverlap::HoldFirst;
		}
	}

	namespace chordpads
	{
		const std::vector<std::string>& presetNames()
		{
			static const std::vector<std::string> names = {
				"major", "minor", "dorian", "phrygian", "lydian", "mixolydian", "locrian"};
			return names;
		}

		std::vector<midi::ChordSymbol> presetChords(const std::string& preset, int root)
		{
			const std::vector<std::string>& names = presetNames();
			const std::optional<midi::ScaleType> type = midi::scaleTypeFromName(preset);
			if (std::find(names.begin(), names.end(), preset) == names.end() || !type.has_value())
				return {};
			const std::vector<int> steps = midi::scaleSteps(*type);
			std::vector<midi::ChordSymbol> chords;
			for (size_t degree = 0; degree < steps.size(); ++degree)
			{
				const int third = pitchClass(steps[(degree + 2) % steps.size()] - steps[degree]);
				const int fifth = pitchClass(steps[(degree + 4) % steps.size()] - steps[degree]);
				midi::ChordSymbol chord;
				chord.root = pitchClass(root + steps[degree]);
				if (third == 4)
					chord.type = fifth == 8 ? "aug" : "maj";
				else
					chord.type = fifth == 6 ? "dim" : "min";
				chords.push_back(chord);
			}
			return chords;
		}

		std::string nashvilleName(const midi::ChordSymbol& chord, int key, const std::string& scale)
		{
			std::string roman = degreeName(chord, key, scale);
			if (roman.empty())
				return {};
			std::string accidental;
			if (roman.front() == 'b' || roman.front() == '#')
			{
				accidental = roman.substr(0, 1);
				roman.erase(0, 1);
			}
			std::string numeral;
			while (!roman.empty() && std::string("IViv").find(roman.front()) != std::string::npos)
			{
				numeral += static_cast<char>(std::toupper(static_cast<unsigned char>(roman.front())));
				roman.erase(0, 1);
			}
			const auto degree = std::find(g_numerals.begin(), g_numerals.end(), numeral) - g_numerals.begin() + 1;
			return accidental + std::to_string(degree) + (chord.type == "min" ? "m" : "") + roman;
		}

		std::string degreeName(const midi::ChordSymbol& chord, int key, const std::string& scale)
		{
			if (chord.root < 0)
				return {};
			const std::vector<int> steps = scaleOf(scale);
			const int interval = pitchClass(chord.root - key);
			// The degree of a pitch class in the scale, or -1.
			const auto degreeOf = [&steps](int pitch)
			{
				for (size_t i = 0; i < steps.size(); ++i)
					if (steps.at(i) == pitchClass(pitch))
						return static_cast<int>(i);
				return -1;
			};
			std::string accidental;
			int degree = degreeOf(interval);
			if (degree < 0)
			{
				accidental = "b";
				degree = degreeOf(interval + 1);
			}
			if (degree < 0)
			{
				accidental = "#";
				degree = degreeOf(interval - 1);
			}
			std::string numeral = g_numerals.at(static_cast<size_t>(degree));
			if (chord.type == "min" || chord.type == "dim")
				std::transform(numeral.begin(), numeral.end(), numeral.begin(),
					[](char letter) { return static_cast<char>(letter - 'A' + 'a'); });
			if (chord.type == "dim")
				numeral += "\xC2\xB0"; // the degree sign
			else if (chord.type == "aug")
				numeral += "+";
			return accidental + numeral;
		}

		midi::ChordSymbol withTensions(const midi::ChordSymbol& chord, int steps, int key)
		{
			midi::ChordSymbol result = chord;
			if (chord.root < 0)
				return result;
			std::vector<std::string> ladder = {"7"};
			const bool dominant = pitchClass(chord.root - key) == 7 ||
				std::find(chord.tensions.begin(), chord.tensions.end(), "7") != chord.tensions.end();
			if (chord.type == "maj")
				ladder = {dominant ? "7" : "maj7", "9", "13"};
			else if (chord.type == "min")
				ladder = {"7", "9", "11"};
			for (; steps > 0; --steps)
				for (const std::string& tension : ladder)
					if (std::find(result.tensions.begin(), result.tensions.end(), tension) == result.tensions.end())
					{
						result.tensions.push_back(tension);
						break;
					}
			sortTensions(result.tensions);
			for (; steps < 0 && !result.tensions.empty(); ++steps)
				result.tensions.pop_back();
			return result;
		}

		midi::ChordSymbol transposed(const midi::ChordSymbol& chord, int semitones)
		{
			midi::ChordSymbol result = chord;
			if (result.root >= 0)
				result.root = pitchClass(result.root + semitones);
			if (result.bass >= 0)
				result.bass = pitchClass(result.bass + semitones);
			return result;
		}

		std::vector<int> inverted(std::vector<int> voicing, int steps)
		{
			if (voicing.size() < 2)
				return voicing;
			const auto upper = voicing.begin() + 1;
			for (; steps > 0; --steps)
			{
				std::rotate(upper, upper + 1, voicing.end());
				voicing.back() += 12;
			}
			for (; steps < 0; ++steps)
			{
				std::rotate(upper, voicing.end() - 1, voicing.end());
				*upper -= 12;
			}
			return voicing;
		}

		std::vector<Suggestion> suggestions(const midi::ChordSymbol& reference, int key, const std::string& scale,
			const std::string& algorithm, int commonNotes, const std::string& cadenceType)
		{
			const std::vector<int> steps = scaleOf(scale);
			// The harmonic function of a root's scale degree: tonic I, iii, vi; subdominant ii, IV; dominant V, vii.
			const auto function = [&steps, key](int root)
			{
				const auto degree = std::find(steps.begin(), steps.end(), pitchClass(root - key)) - steps.begin();
				static constexpr std::array<const char*, 7> functions = {
					"tonic", "subdominant", "tonic", "subdominant", "dominant", "tonic", "dominant"};
				return degree < 7 ? std::string(functions.at(static_cast<size_t>(degree))) : std::string();
			};
			const std::vector<int> referenceClasses = classesOf(reference);
			// How strongly a root step follows: a fifth down, a fifth up, the relative chords, a second, a third.
			const auto rank = [](int step)
			{
				static const std::map<int, int> ranks = {
					{5, 0}, {7, 1}, {9, 2}, {3, 2}, {2, 3}, {10, 3}, {8, 4}, {4, 4}};
				const auto found = ranks.find(step);
				return found == ranks.end() ? 5 : found->second;
			};
			struct Candidate
			{
				std::tuple<int, int, int, int, int> order;
				Suggestion suggestion;
			};
			std::vector<Candidate> candidates;
			const std::array<std::pair<const char*, const char*>, 6> types = {
				{{"maj", ""}, {"min", ""}, {"dim", ""}, {"maj", "7"}, {"maj", "maj7"}, {"min", "7"}}};
			for (int root = 0; root < 12; ++root)
				for (size_t type = 0; type < types.size(); ++type)
				{
					midi::ChordSymbol chord;
					chord.root = root;
					chord.type = types.at(type).first;
					if (*types.at(type).second != '\0')
						chord.tensions = {types.at(type).second};
					if (chord == reference)
						continue;
					const std::vector<int> classes = classesOf(chord);
					const auto outside = static_cast<int>(std::count_if(classes.begin(), classes.end(),
						[&steps, key](int note) { return !inScale(steps, key, note); }));
					const auto shared = static_cast<int>(std::count_if(classes.begin(), classes.end(),
						[&referenceClasses](int note)
						{
							return std::find(referenceClasses.begin(), referenceClasses.end(), note) !=
								referenceClasses.end();
						}));
					if (algorithm == "commonNotes" && shared != commonNotes)
						continue;
					if (cadenceType != "all" && function(root) != cadenceType)
						continue;
					const int level = std::min(outside, 2);
					const int step = reference.root < 0 ? 0 : pitchClass(root - reference.root);
					const int distance =
						reference.root < 0 ? fifthsDistance(root, key) : fifthsDistance(root, reference.root);
					const int strength = algorithm == "commonNotes" ? 0 : rank(step);
					candidates.push_back(
						{{level, strength, distance, static_cast<int>(type), root}, {std::move(chord), level}});
				}
			std::stable_sort(candidates.begin(), candidates.end(),
				[](const Candidate& a, const Candidate& b) { return a.order < b.order; });
			std::vector<Suggestion> result;
			result.reserve(std::min<size_t>(candidates.size(), 24));
			for (size_t i = 0; i < candidates.size() && (algorithm == "proximity" || i < 24); ++i)
				result.push_back(candidates[i].suggestion);
			return result;
		}

		std::vector<Suggestion> proximity(const midi::ChordSymbol& reference, int key, const std::string& scale)
		{
			std::vector<Suggestion> chords = suggestions(reference, key, scale, "proximity", 0);
			const std::vector<int> referenceClasses = classesOf(reference);
			struct Candidate
			{
				std::tuple<int, int, int> order;
				Suggestion suggestion;
			};
			std::vector<Candidate> candidates;
			candidates.reserve(chords.size());
			for (Suggestion& suggestion : chords)
			{
				const std::vector<int> classes = classesOf(suggestion.chord);
				const auto foreign = static_cast<int>(std::count_if(classes.begin(), classes.end(),
					[&referenceClasses](int note)
					{
						return std::find(referenceClasses.begin(), referenceClasses.end(), note) ==
							referenceClasses.end();
					}));
				suggestion.distance = foreign + (suggestion.chord.tensions.empty() ? 0 : 1);
				const int fifths = reference.root < 0 ? 0 : fifthsDistance(suggestion.chord.root, reference.root);
				candidates.push_back({{suggestion.distance, suggestion.level, fifths}, suggestion});
			}
			std::stable_sort(candidates.begin(), candidates.end(),
				[](const Candidate& a, const Candidate& b) { return a.order < b.order; });
			std::vector<Suggestion> result;
			result.reserve(candidates.size());
			for (size_t i = 0; i < candidates.size() && i < 24; ++i)
				result.push_back(candidates[i].suggestion);
			return result;
		}
	}

	std::vector<std::vector<int>> chordPadVoicings(const ChordPads& pads, int player)
	{
		const auto basic = [&pads, player](const ChordPad& pad)
		{ return chordpads::inverted(playerVoicing(pads, chordOf(pad.chord), false, {}, player), pad.voicing); };
		const bool referenced = pads.reference >= 0 && pads.reference < static_cast<int>(pads.pads.size()) &&
			!pads.pads[static_cast<size_t>(pads.reference)].chord.empty();
		const std::vector<int> reference =
			referenced ? basic(pads.pads[static_cast<size_t>(pads.reference)]) : std::vector<int>();
		std::vector<std::vector<int>> voicings;
		voicings.reserve(pads.pads.size());
		for (size_t i = 0; i < pads.pads.size(); ++i)
		{
			const ChordPad& pad = pads.pads[i];
			if (pad.chord.empty())
				voicings.emplace_back();
			else if (referenced && pad.adaptive && static_cast<int>(i) != pads.reference)
				voicings.push_back(playerVoicing(pads, chordOf(pad.chord), true, reference, player));
			else
				voicings.push_back(basic(pad));
		}
		return voicings;
	}

	ChordPadKeys chordPadKeys(const ChordPads& pads)
	{
		ChordPadKeys keys;
		keys.active = pads.remote;
		keys.start = static_cast<uint8_t>(std::clamp(pads.remoteStart, 0, 127));
		keys.end = static_cast<uint8_t>(std::clamp(pads.remoteEnd, 0, 127));
		keys.latch = pads.latch;
		keys.filter = pads.filterNotes;
		keys.overlap = overlapOf(pads.overlaps);
		keys.output = pads.outputMode;
		keys.modifiers = pads.modifiers;
		for (size_t i = 0; i < keys.modifierKeys.size(); ++i)
			keys.modifierKeys.at(i) = static_cast<uint8_t>(std::clamp(pads.modifierKeys.at(i), 0, 127));
		keys.portHash = chordPadPortHash(pads.remotePort);
		keys.selected =
			static_cast<uint8_t>(std::clamp(pads.player, 0, static_cast<int>(ChordPadKeys::g_maxPlayers) - 1));
		keys.players = 0;
		for (const int player : playingPlayers(pads))
		{
			if (player >= static_cast<int>(ChordPadKeys::g_maxPlayers))
				continue;
			keys.players = static_cast<uint8_t>(keys.players | (1U << static_cast<unsigned>(player)));
			const std::vector<std::vector<int>> voicings = chordPadVoicings(pads, player);
			for (int key = std::max(pads.remoteStart, 0); key <= std::min(pads.remoteEnd, 127); ++key)
			{
				const auto pad = static_cast<size_t>(key - pads.remoteStart);
				if (pad >= voicings.size())
					break;
				const std::vector<int>& voicing = voicings[pad];
				const size_t count = std::min(voicing.size(), ChordPadKeys::g_maxPitches);
				auto& pitches = keys.pitches.at(static_cast<size_t>(player)).at(static_cast<size_t>(key));
				for (size_t i = 0; i < count; ++i)
					pitches.at(i) = static_cast<uint8_t>(std::clamp(voicing[i], 0, 127));
				keys.counts.at(static_cast<size_t>(player)).at(static_cast<size_t>(key)) = static_cast<uint8_t>(count);
			}
		}
		return keys;
	}

	Json chordPadsJson(const Project& project)
	{
		const ChordPads& pads = project.chordPads;
		const std::vector<std::vector<int>> voicings = chordPadVoicings(pads);
		Json list = Json::array();
		for (size_t i = 0; i < pads.pads.size(); ++i)
		{
			const ChordPad& pad = pads.pads[i];
			const int key = pads.remoteStart + static_cast<int>(i);
			list.push_back({{"chord", pad.chord},
				{"degree",
					pad.chord.empty() ? std::string()
									  : chordpads::degreeName(chordOf(pad.chord), pads.rootKey, pads.scale)},
				{"nashville",
					pad.chord.empty() ? std::string()
									  : chordpads::nashvilleName(chordOf(pad.chord), pads.rootKey, pads.scale)},
				{"pitches", voicings[i]}, {"voicing", pad.voicing}, {"adaptive", pad.adaptive}, {"locked", pad.locked},
				{"key", key <= pads.remoteEnd && key <= 127 ? Json(key) : Json(nullptr)}});
		}
		return {{"pads", list}, {"rootKey", pads.rootKey}, {"reference", pads.reference},
			{"octaveOffset", playerOf(pads).octaveOffset}, {"players", playersJson(pads)}, {"player", pads.player},
			{"velocity", pads.velocity}, {"remote", pads.remote}, {"remoteStart", pads.remoteStart},
			{"remoteEnd", pads.remoteEnd}, {"preset", pads.preset}, {"scale", pads.scale},
			{"stepInput", pads.stepInput}, {"moveInsert", pads.moveInsert}, {"stepPosition", pads.stepPosition},
			{"outputMode", pads.outputMode}, {"latch", pads.latch}, {"overlaps", pads.overlaps},
			{"filterNotes", pads.filterNotes}, {"modifiers", pads.modifiers}, {"modifierKeys", modifierKeysJson(pads)},
			{"remotePort", pads.remotePort}, {"snapPlayback", pads.snapPlayback}};
	}

	void chordPadsFromJson(Project& project, const Json& json)
	{
		if (!json.is_object())
			return;
		ChordPads& pads = project.chordPads;
		// A file without pads keeps the default ones: a project has at least one (chordPads.set padCount).
		if (json.contains("pads") && json.at("pads").is_array() && !json.at("pads").empty())
		{
			pads.pads.clear();
			for (const Json& item : json.at("pads"))
			{
				if (pads.pads.size() == g_maxPads)
					break;
				ChordPad pad;
				const midi::ChordSymbol chord = chordOf(item.value("chord", std::string()));
				pad.chord = chord.root < 0 ? std::string() : midi::chordName(chord);
				pad.voicing = std::clamp(item.value("voicing", 0), -12, 12);
				pad.adaptive = item.value("adaptive", true);
				pad.locked = item.value("locked", false);
				pads.pads.push_back(pad);
			}
		}
		pads.rootKey = pitchClass(json.value("rootKey", 0));
		pads.reference = json.value("reference", -1);
		if (pads.reference < -1 || pads.reference >= static_cast<int>(pads.pads.size()))
			pads.reference = -1;
		// A file from before the players has the basic player's octave offset only.
		pads.players = playersFromJson(json.value("players", Json::array()));
		if (pads.players.empty())
			pads.players = {ChordPlayer()};
		if (!json.contains("players"))
			pads.players.front().octaveOffset = std::clamp(json.value("octaveOffset", 0), -3, 3);
		pads.player = std::clamp(json.value("player", 0), 0, static_cast<int>(pads.players.size()) - 1);
		pads.velocity = std::clamp(json.value("velocity", 100), 1, 127);
		pads.remote = json.value("remote", false);
		pads.remoteStart = std::clamp(json.value("remoteStart", 36), 0, 127);
		pads.remoteEnd = std::clamp(json.value("remoteEnd", 47), pads.remoteStart, 127);
		pads.preset = json.value("preset", std::string("major"));
		const std::vector<std::string>& scales = chordpads::presetNames();
		pads.scale = json.value("scale", pads.preset);
		if (std::find(scales.begin(), scales.end(), pads.scale) == scales.end())
			pads.scale = "major";
		pads.stepInput = json.value("stepInput", false);
		pads.moveInsert = json.value("moveInsert", false);
		pads.stepPosition = std::clamp(json.value("stepPosition", time::Ticks{0}), time::Ticks{0}, stepEnd(project));
		pads.outputMode = json.value("outputMode", true);
		pads.latch = json.value("latch", false);
		pads.overlaps = json.value("overlaps", std::string("holdFirst"));
		const std::vector<std::string>& overlaps = overlapNames();
		if (std::find(overlaps.begin(), overlaps.end(), pads.overlaps) == overlaps.end())
			pads.overlaps = "holdFirst";
		pads.filterNotes = json.value("filterNotes", false);
		pads.modifiers = json.value("modifiers", false);
		const Json keys = json.value("modifierKeys", Json::object());
		for (size_t i = 0; i < g_modifierNames.size(); ++i)
			if (keys.is_object() && keys.contains(g_modifierNames.at(i)) && keys.at(g_modifierNames.at(i)).is_number())
				pads.modifierKeys.at(i) = std::clamp(keys.at(g_modifierNames.at(i)).get<int>(), 0, 127);
		pads.remotePort = json.value("remotePort", std::string());
		pads.snapPlayback = json.value("snapPlayback", false);
	}

	void ProjectModel::registerChordPadCommands(Engine& engine)
	{
		const Json integerSchema = {{"type", "integer"}};
		const Json booleanSchema = {{"type", "boolean"}};
		const Json stringSchema = {{"type", "string"}};
		const Json padsSchema = {{"type", "array"}, {"items", integerSchema}};
		const Json padsResult = {{"type", "object"},
			{"properties",
				{{"pads", {{"type", "array"}}}, {"rootKey", integerSchema}, {"reference", integerSchema},
					{"octaveOffset", integerSchema}, {"players", {{"type", "array"}}}, {"player", integerSchema},
					{"velocity", integerSchema}, {"remote", booleanSchema}, {"remoteStart", integerSchema},
					{"remoteEnd", integerSchema}, {"preset", stringSchema}, {"scale", stringSchema},
					{"stepInput", booleanSchema}, {"moveInsert", booleanSchema}, {"stepPosition", integerSchema},
					{"outputMode", booleanSchema}, {"latch", booleanSchema}, {"overlaps", stringSchema},
					{"filterNotes", booleanSchema}, {"modifiers", booleanSchema},
					{"modifierKeys", {{"type", "object"}}}, {"remotePort", stringSchema},
					{"snapPlayback", booleanSchema}}},
			{"required",
				{"pads", "rootKey", "reference", "octaveOffset", "players", "player", "velocity", "remote",
					"remoteStart", "remoteEnd", "preset", "scale", "stepInput", "moveInsert", "stepPosition",
					"outputMode", "latch", "overlaps", "filterNotes", "modifiers", "modifierKeys", "remotePort",
					"snapPlayback"}}};
		// Each command that changes the pads announces them.
		auto remote = std::make_shared<RemoteState>();
		const auto changed = [](Project& project, CommandContext& context)
		{
			Json result = chordPadsJson(project);
			context.emit("chordPads.changed", result);
			return result;
		};

		add(engine,
			{"chordPads.get", 1,
				"The chord pads (reference manual pp. 1346-1391): pads, each with its chord (\"\" while unassigned), "
				"degree (its scale degree as a roman numeral, in the loaded preset's scale on the root key), nashville "
				"(the same in the Nashville Number System, such as 2m), pitches "
				"(the voicing it plays, from the lowest up), voicing (the inversion of its basic voicing), adaptive "
				"(adaptive voicing), locked and key (the remote key that triggers it, or null); rootKey (0 C to 11 "
				"B); reference (the adaptive voicing reference pad, or -1); octaveOffset (the selected basic "
				"player's); players (each "
				"with its name, library, style and octaveOffset) and player (the selected one's index); "
				"velocity (of pads triggered with the mouse); remote, remoteStart and remoteEnd (the pads' remote "
				"keys); preset (the last loaded) and scale (its scale); stepInput, moveInsert and stepPosition "
				"(ticks); outputMode, latch, "
				"overlaps, filterNotes, modifiers, modifierKeys and remotePort (as chordPads.set takes them); "
				"listening, what chordPads.listen listens for ({assign: pad} or {learn: target}), or null.",
				objectSchema({}), padsResult, true},
			[remote](Project& project, CommandContext&)
			{
				Json result = chordPadsJson(project);
				result["listening"] = remote->listening();
				return result;
			});
		add(engine,
			{"chordPads.set", 1,
				"Changes the chord pads' settings: rootKey (0 C to 11 B; with transposePads the assigned unlocked pads "
				"move with it, the nearer way, reference manual p. 1365, without only their degrees change, p. 1364); "
				"padCount (1 to 32, the number of pads; pads beyond it go); player (the selected player, "
				"chordPads.addPlayer); octaveOffset (-3 to 3, the selected basic "
				"player's Octave Offset, p. 1377); velocity (1 to 127, Velocity on Mouse Click); remote (Pad Remote "
				"Control: notes from MIDI inputs on the remote keys trigger the pads instead of playing, p. 1386), "
				"remoteStart and remoteEnd (Pads Remote Range Start and End, MIDI notes: the first key triggers the "
				"first pad and so on); stepInput, moveInsert and stepPosition (ticks, from 0 to the end of the bar "
				"the longest project, 2^48 samples, ends in at the project's tempo) for chordPads.step (p. 1369); "
				"stepBars moves the step position by bars, to a bar's start, at most that bar's (after "
				"stepPosition), as Left and Right Arrow move the step input cursor. Moving the step position is no "
				"undo step of its own; with another change it goes back on undo. outputMode (Chord Pad Output Mode, "
				"p. 1347, on by default): on, the pads play into every MIDI and instrument track that monitors or is "
				"record-enabled; off, only into those whose midiInput is chordPads (channel.set). latch (Latch "
				"Chords, p. 1386): a pad's chord plays until the pad is triggered again. overlaps (p. 1379), what a "
				"chord played over another does with the other's notes: holdFirst (the default: they go on until "
				"their own off; shared notes sound once), legato (they end but for the shared ones, which go on) or "
				"stopFirst (they all end). filterNotes (Filter Notes From MIDI Thru): the remote keys of pads without "
				"a chord play nothing. modifiers activates the chord modifiers' remote keys (p. 1387), modifierKeys "
				"(MIDI notes by name: previousVoicing, nextVoicing, fewerTensions, moreTensions, transposeDown, "
				"transposeUp; C2 to F2 by default): such a key changes the pads whose remote keys are down, or else "
				"the "
				"one triggered last, and plays nothing. remotePort: the MIDI input whose notes trigger the pads, its "
				"midi.portSettings key such as dummy/in/Keys (p. 1386); empty (the default) for all MIDI inputs. "
				"snapPlayback (Snap Playback to Musical Grid, p. 1349): while the transport plays, a triggered pad's "
				"chord starts at the next position of the quantize grid (quantize.set).",
				objectSchema({{"rootKey", integerSchema}, {"transposePads", booleanSchema}, {"padCount", integerSchema},
					{"octaveOffset", integerSchema}, {"player", integerSchema}, {"velocity", integerSchema},
					{"remote", booleanSchema}, {"remoteStart", integerSchema}, {"remoteEnd", integerSchema},
					{"stepInput", booleanSchema}, {"moveInsert", booleanSchema}, {"stepPosition", integerSchema},
					{"stepBars", integerSchema}, {"outputMode", booleanSchema}, {"latch", booleanSchema},
					{"overlaps", {{"type", "string"}, {"enum", overlapNames()}}}, {"filterNotes", booleanSchema},
					{"modifiers", booleanSchema},
					{"modifierKeys",
						{{"type", "object"}, {"additionalProperties", false},
							{"properties",
								{{"previousVoicing", integerSchema}, {"nextVoicing", integerSchema},
									{"fewerTensions", integerSchema}, {"moreTensions", integerSchema},
									{"transposeDown", integerSchema}, {"transposeUp", integerSchema}}}}},
					{"remotePort", stringSchema}, {"snapPlayback", booleanSchema}}),
				padsResult, false},
			[changed](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				ChordPads& pads = project.chordPads;
				if (parameters.contains("rootKey"))
				{
					const int key = parameters.at("rootKey").get<int>();
					require(key >= 0 && key < 12, "rootKey must be 0 to 11");
					if (parameters.value("transposePads", false))
					{
						int step = pitchClass(key - pads.rootKey);
						step = step > 6 ? step - 12 : step;
						for (ChordPad& pad : pads.pads)
							if (!pad.chord.empty() && !pad.locked)
								pad.chord = midi::chordName(chordpads::transposed(chordOf(pad.chord), step));
					}
					pads.rootKey = key;
				}
				if (parameters.contains("padCount"))
				{
					const int count = parameters.at("padCount").get<int>();
					require(count >= 1 && count <= g_maxPads, "padCount must be 1 to 32");
					pads.pads.resize(static_cast<size_t>(count));
					if (pads.reference >= count)
						pads.reference = -1;
				}
				if (parameters.contains("player"))
				{
					pads.player = parameters.at("player").get<int>();
					require(pads.player >= 0 && pads.player < static_cast<int>(pads.players.size()),
						"no chord pads player " + std::to_string(pads.player));
				}
				if (parameters.contains("octaveOffset"))
				{
					ChordPlayer& player = pads.players.at(static_cast<size_t>(pads.player));
					require(player.library == "basic", "the octave offset is a basic player's");
					player.octaveOffset = parameters.at("octaveOffset").get<int>();
					require(player.octaveOffset >= -3 && player.octaveOffset <= 3, "octaveOffset must be -3 to 3");
				}
				pads.velocity = parameters.value("velocity", pads.velocity);
				require(pads.velocity >= 1 && pads.velocity <= 127, "velocity must be 1 to 127");
				pads.remote = parameters.value("remote", pads.remote);
				pads.remoteStart = parameters.value("remoteStart", pads.remoteStart);
				pads.remoteEnd = parameters.value("remoteEnd", pads.remoteEnd);
				require(pads.remoteStart >= 0 && pads.remoteEnd <= 127 && pads.remoteStart <= pads.remoteEnd,
					"the remote keys must be MIDI notes, remoteStart up to remoteEnd");
				pads.stepInput = parameters.value("stepInput", pads.stepInput);
				pads.moveInsert = parameters.value("moveInsert", pads.moveInsert);
				pads.outputMode = parameters.value("outputMode", pads.outputMode);
				pads.latch = parameters.value("latch", pads.latch);
				pads.overlaps = parameters.value("overlaps", pads.overlaps);
				pads.filterNotes = parameters.value("filterNotes", pads.filterNotes);
				pads.modifiers = parameters.value("modifiers", pads.modifiers);
				if (parameters.contains("modifierKeys"))
					for (size_t i = 0; i < g_modifierNames.size(); ++i)
						if (const Json& keys = parameters.at("modifierKeys"); keys.contains(g_modifierNames.at(i)))
						{
							const int key = keys.at(g_modifierNames.at(i)).get<int>();
							require(key >= 0 && key <= 127, "modifierKeys must be MIDI notes, 0 to 127");
							pads.modifierKeys.at(i) = key;
						}
				pads.remotePort = parameters.value("remotePort", pads.remotePort);
				pads.snapPlayback = parameters.value("snapPlayback", pads.snapPlayback);
				if (parameters.contains("stepPosition"))
				{
					const time::Ticks end = stepEnd(project);
					pads.stepPosition = parameters.at("stepPosition").get<time::Ticks>();
					require(pads.stepPosition >= 0 && pads.stepPosition <= end,
						"stepPosition must be 0 to " + std::to_string(end) + " ticks");
				}
				if (parameters.contains("stepBars"))
				{
					// An unsigned number past int64_t's range is as far on as any; the bars are clamped before the
					// sum, which then can't overflow, and after it, to the bars step input writes at.
					const Json& given = parameters.at("stepBars");
					constexpr int64_t most = std::numeric_limits<int64_t>::max();
					const int64_t requested =
						given.is_number_unsigned() && given.get<uint64_t>() > static_cast<uint64_t>(most)
						? most
						: given.get<int64_t>();
					const time::SignatureMap& signatures = project.signatures;
					const int64_t lastBar = lastStepBar(project);
					const int64_t bars = std::clamp<int64_t>(requested, -lastBar - 1, lastBar + 1);
					pads.stepPosition = signatures.barStart(
						std::clamp<int64_t>(signatures.toBarsBeats(pads.stepPosition).bar + bars, 0, lastBar));
				}
				return changed(project, context);
			});
		add(engine,
			{"chordPads.addPlayer", 1,
				"Add Player (reference manual pp. 1376-1377): a basic, piano or guitar player (library) with a name "
				"(default after its library) and, for piano and guitar, a voicing style (style, default the "
				"library's first: chord.setTrack lists them), which becomes the selected player whose voicings the "
				"pads play. At most 8.",
				objectSchema({{"library", {{"type", "string"}, {"enum", midi::voicingLibraries()}}},
								 {"name", stringSchema}, {"style", stringSchema}, {"track", integerSchema}},
					{"library"}),
				padsResult, false},
			[changed](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				ChordPads& pads = project.chordPads;
				require(pads.players.size() < 8, "the chord pads have 8 players already");
				ChordPlayer player;
				player.library = parameters.at("library").get<std::string>();
				std::string named = player.library + " Player";
				named.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(named.front())));
				player.name = parameters.value("name", named);
				player.style = parameters.value("style", std::string());
				const std::vector<std::string>& styles = midi::voicingStyles(player.library);
				require(player.style.empty() || std::find(styles.begin(), styles.end(), player.style) != styles.end(),
					"no " + player.library + " voicing style " + player.style);
				player.track = playerTrack(project, parameters.value("track", ObjectId{0}), -1);
				pads.players.push_back(player);
				pads.player = static_cast<int>(pads.players.size()) - 1;
				return changed(project, context);
			});
		add(engine,
			{"chordPads.setPlayer", 1,
				"Changes a chord pads player (player, default the selected one; reference manual p. 1377): name "
				"(Rename Selected Player), style (Chord Voicing Style, piano and guitar players: their library's "
				"styles), octaveOffset (-3 to 3, basic players), track (Using Different Players on Multiple Tracks, "
				"p. 1377: a MIDI or instrument track that gets this player's voicings while the other tracks get the "
				"selected player's; 0 for none; one player a track).",
				objectSchema({{"player", integerSchema}, {"name", stringSchema}, {"style", stringSchema},
					{"octaveOffset", integerSchema}, {"track", integerSchema}}),
				padsResult, false},
			[changed](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				ChordPads& pads = project.chordPads;
				const int index = parameters.value("player", pads.player);
				require(index >= 0 && index < static_cast<int>(pads.players.size()),
					"no chord pads player " + std::to_string(index));
				ChordPlayer& player = pads.players.at(static_cast<size_t>(index));
				player.name = parameters.value("name", player.name);
				require(!player.name.empty(), "a player needs a name");
				if (parameters.contains("style"))
				{
					player.style = parameters.at("style").get<std::string>();
					const std::vector<std::string>& styles = midi::voicingStyles(player.library);
					require(player.library != "basic" &&
							std::find(styles.begin(), styles.end(), player.style) != styles.end(),
						"no " + player.library + " voicing style " + player.style);
				}
				if (parameters.contains("octaveOffset"))
				{
					require(player.library == "basic", "the octave offset is a basic player's");
					player.octaveOffset = parameters.at("octaveOffset").get<int>();
					require(player.octaveOffset >= -3 && player.octaveOffset <= 3, "octaveOffset must be -3 to 3");
				}
				if (parameters.contains("track"))
					player.track = playerTrack(project, parameters.at("track").get<ObjectId>(), index);
				return changed(project, context);
			});
		add(engine,
			{"chordPads.removePlayer", 1,
				"Delete Selected Player (reference manual p. 1377): removes a chord pads player (player, default the "
				"selected one), but not the last one. The one before it is selected.",
				objectSchema({{"player", integerSchema}}), padsResult, false},
			[changed](Project& project, CommandContext& context)
			{
				ChordPads& pads = project.chordPads;
				const int index = context.parameters().value("player", pads.player);
				require(index >= 0 && index < static_cast<int>(pads.players.size()),
					"no chord pads player " + std::to_string(index));
				require(pads.players.size() > 1, "the chord pads keep one player");
				pads.players.erase(pads.players.begin() + index);
				if (pads.player >= index && pads.player > 0)
					--pads.player;
				return changed(project, context);
			});
		add(engine,
			{"chordPads.assign", 1,
				"Assigns chords to chord pads (reference manual pp. 1356-1362): each of assignments has a pad and a "
				"chord as the chord editor's text input takes it (such as Cmaj7, Dm7(9) or C/E; \"\" removes the "
				"assignment), or pitches (MIDI notes, Assign from MIDI Input: the chord they make). The pad's voicing "
				"goes back to its basic voicing with adaptive voicing. Locked pads are refused.",
				objectSchema(
					{{"assignments",
						{{"type", "array"},
							{"items",
								objectSchema({{"pad", integerSchema}, {"chord", stringSchema}, {"pitches", padsSchema}},
									{"pad"})}}}},
					{"assignments"}),
				padsResult, false},
			[changed](Project& project, CommandContext& context)
			{
				for (const Json& item : context.parameters().at("assignments"))
				{
					ChordPad& pad = padAt(project.chordPads, item.at("pad"));
					require(!pad.locked, "chord pad " + std::to_string(item.at("pad").get<int>()) + " is locked");
					require(item.contains("chord") != item.contains("pitches"), "give a chord or pitches");
					if (item.contains("pitches"))
					{
						const std::optional<midi::ChordSymbol> chord =
							midi::recognizeChordSymbol(item.at("pitches").get<std::vector<int>>(), true, true);
						require(chord.has_value(), "the pitches make no chord");
						pad.chord = midi::chordName(chord.value_or(midi::ChordSymbol()));
					}
					else
					{
						const auto text = item.at("chord").get<std::string>();
						pad.chord = text.empty() ? std::string() : midi::chordName(chordFrom(text));
					}
					pad.voicing = 0;
					pad.adaptive = true;
				}
				return changed(project, context);
			});
		add(engine,
			{"chordPads.modify", 1,
				"The pad modifiers (reference manual pp. 1351-1352) on the assigned, unlocked pads of pads: transpose "
				"moves their chords by semitones, tensions adds (more) or takes away (fewer) as many tensions, voicing "
				"steps through their voicings (inversions), which turns their adaptive voicing off.",
				objectSchema({{"pads", padsSchema}, {"transpose", integerSchema}, {"tensions", integerSchema},
								 {"voicing", integerSchema}},
					{"pads"}),
				padsResult, false},
			[changed](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				ChordPads& pads = project.chordPads;
				const std::vector<std::vector<int>> voicings = chordPadVoicings(pads);
				for (const Json& index : parameters.at("pads"))
				{
					ChordPad& pad = padAt(pads, index);
					if (pad.chord.empty() || pad.locked)
						continue;
					midi::ChordSymbol chord = chordOf(pad.chord);
					chord = chordpads::transposed(chord, parameters.value("transpose", 0));
					chord = chordpads::withTensions(chord, parameters.value("tensions", 0), pads.rootKey);
					if (parameters.contains("voicing"))
					{
						if (pad.adaptive)
							pad.voicing = inversionOf(pads, chord, voicings[index.get<size_t>()]);
						pad.voicing = std::clamp(pad.voicing + parameters.at("voicing").get<int>(), -12, 12);
						pad.adaptive = false;
					}
					pad.chord = midi::chordName(chord);
				}
				return changed(project, context);
			});
		add(engine,
			{"chordPads.setPad", 1,
				"Changes a chord pad's state (reference manual p. 1351): locked (Lock: its chord and voicing can't "
				"change; locking turns adaptive voicing off), adaptive (Adaptive Voicing: it takes the voicing nearest "
				"to the reference pad's) and reference (Adaptive Voicing Reference: true makes it the reference, "
				"false leaves no pad the reference).",
				objectSchema({{"pad", integerSchema}, {"locked", booleanSchema}, {"adaptive", booleanSchema},
								 {"reference", booleanSchema}},
					{"pad"}),
				padsResult, false},
			[changed](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				ChordPads& pads = project.chordPads;
				ChordPad& pad = padAt(pads, parameters.at("pad"));
				const int index = parameters.at("pad").get<int>();
				if (parameters.contains("locked"))
				{
					pad.locked = parameters.at("locked").get<bool>();
					if (pad.locked)
						pad.adaptive = false;
				}
				if (parameters.contains("adaptive"))
				{
					require(!pad.locked || !parameters.at("adaptive").get<bool>(), "the pad is locked");
					pad.adaptive = parameters.at("adaptive").get<bool>();
				}
				if (parameters.contains("reference") && parameters.at("reference").get<bool>())
					pads.reference = index;
				else if (parameters.contains("reference") && pads.reference == index)
					pads.reference = -1;
				return changed(project, context);
			});
		add(engine,
			{"chordPads.move", 1,
				"Swaps the chord assignments of two pads with their settings, or with copy copies from's to to "
				"(reference manual p. 1361). The adaptive voicing reference stays on its pad. A locked to is refused.",
				objectSchema({{"from", integerSchema}, {"to", integerSchema}, {"copy", booleanSchema}}, {"from", "to"}),
				padsResult, false},
			[changed](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				ChordPads& pads = project.chordPads;
				ChordPad& from = padAt(pads, parameters.at("from"));
				ChordPad& to = padAt(pads, parameters.at("to"));
				require(!to.locked, "chord pad " + std::to_string(parameters.at("to").get<int>()) + " is locked");
				if (parameters.value("copy", false))
				{
					to = from;
				}
				else
				{
					require(
						!from.locked, "chord pad " + std::to_string(parameters.at("from").get<int>()) + " is locked");
					std::swap(from, to);
				}
				return changed(project, context);
			});
		add(engine,
			{"chordPads.fromChordTrack", 1,
				"Assign from Chord Track (reference manual p. 1361): the chord track's chords, in their order and each "
				"once, onto the pads from the first; the other unlocked pads become unassigned. Undefined chords are "
				"left out.",
				objectSchema({}), padsResult, false},
			[changed](Project& project, CommandContext& context)
			{
				std::vector<std::string> chords;
				for (const ChordEvent& event : project.chords.events)
					if (event.text != "X" && std::find(chords.begin(), chords.end(), event.text) == chords.end())
						chords.push_back(event.text);
				require(!chords.empty(), "the chord track has no chords");
				size_t next = 0;
				for (ChordPad& pad : project.chordPads.pads)
				{
					if (pad.locked)
						continue;
					pad = ChordPad();
					if (next < chords.size())
						pad.chord = chords[next++];
				}
				return changed(project, context);
			});
		add(engine,
			{"chordPads.loadPreset", 1,
				"Loads a chord pads preset (reference manual pp. 1370-1371). The scale-based ones, major, minor, "
				"dorian, "
				"phrygian, lydian, mixolydian and locrian, are the triads on the seven degrees of the scale on root (0 "
				"C to 11 B, default the root key), which becomes the root key, onto the pads from the first. The "
				"user's own (chordPads.savePreset) bring back their chords pad by pad with their voicings, their root "
				"key and scale, and their player. load: chordsAndPlayers (the default), chords (the chords; the other "
				"unlocked pads become unassigned) or players (the player: for the scale-based presets the basic "
				"player with no octave offset). chordPads.presets lists them.",
				objectSchema({{"preset", stringSchema}, {"root", integerSchema},
								 {"load", {{"type", "string"}, {"enum", {"chordsAndPlayers", "chords", "players"}}}}},
					{"preset"}),
				padsResult, false},
			[changed](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				ChordPads& pads = project.chordPads;
				const auto preset = parameters.at("preset").get<std::string>();
				const int root = parameters.value("root", pads.rootKey);
				require(root >= 0 && root < 12, "root must be 0 to 11");
				const std::string load = parameters.value("load", std::string("chordsAndPlayers"));
				const std::vector<midi::ChordSymbol> chords = chordpads::presetChords(preset, root);
				const Json mine = chords.empty() ? userPresets(context).value(preset, Json()) : Json();
				require(!chords.empty() || mine.is_object(), "no chord pads preset " + preset);
				if (load != "players")
				{
					const Json saved = mine.is_object() ? mine.value("pads", Json::array()) : Json::array();
					size_t next = 0;
					for (size_t i = 0; i < pads.pads.size(); ++i)
					{
						ChordPad& pad = pads.pads[i];
						if (pad.locked)
							continue;
						pad = ChordPad();
						if (!chords.empty() && next < chords.size())
						{
							pad.chord = midi::chordName(chords[next++]);
						}
						else if (i < saved.size() && saved[i].is_object())
						{
							const midi::ChordSymbol chord = chordOf(saved[i].value("chord", std::string()));
							pad.chord = chord.root < 0 ? std::string() : midi::chordName(chord);
							pad.voicing = std::clamp(saved[i].value("voicing", 0), -12, 12);
							pad.adaptive = saved[i].value("adaptive", true);
						}
					}
					pads.rootKey = chords.empty() ? pitchClass(mine.value("rootKey", 0)) : root;
					pads.preset = preset;
					const std::vector<std::string>& scales = chordpads::presetNames();
					pads.scale = chords.empty() ? mine.value("scale", std::string("major")) : preset;
					if (std::find(scales.begin(), scales.end(), pads.scale) == scales.end())
						pads.scale = "major";
				}
				if (load != "chords")
				{
					pads.players = chords.empty() ? playersFromJson(mine.value("players", Json::array()))
												  : std::vector<ChordPlayer>{ChordPlayer()};
					if (pads.players.empty()) // a preset from before the players
					{
						pads.players = {ChordPlayer()};
						pads.players.front().octaveOffset = std::clamp(mine.value("octaveOffset", 0), -3, 3);
					}
					pads.player = std::clamp(
						chords.empty() ? mine.value("player", 0) : 0, 0, static_cast<int>(pads.players.size()) - 1);
				}
				return changed(project, context);
			});
		add(engine,
			{"chordPads.presets", 1,
				"The chord pads presets that chordPads.loadPreset loads, as Previous and Next Preset step through them "
				"(reference manual p. 1348): the scale-based ones, then the user's own by name.",
				objectSchema({}),
				objectSchema({{"presets", {{"type", "array"}, {"items", stringSchema}}}}, {"presets"}), true},
			[](Project&, CommandContext& context)
			{
				Json names = chordpads::presetNames();
				const Json mine = userPresets(context);
				for (const auto& [name, preset] : mine.items())
					names.push_back(name);
				return Json{{"presets", names}};
			});
		addControl(engine,
			{"chordPads.savePreset", 1,
				"Save Preset (reference manual p. 1371): the chord pads as a preset of the user's own, kept in the "
				"user "
				"setting chordPads.presets: each pad's chord, voicing and adaptive voicing, the root key, the scale "
				"and the player (the octave offset). A preset of the name is replaced; the scale-based ones can't be. "
				"No change to the project.",
				objectSchema({{"name", stringSchema}}, {"name"}), objectSchema({}), false},
			[this, &engine](CommandContext& context)
			{
				const auto name = context.parameters().at("name").get<std::string>();
				require(!name.empty(), "the preset needs a name");
				const std::vector<std::string>& builtIn = chordpads::presetNames();
				require(std::find(builtIn.begin(), builtIn.end(), name) == builtIn.end(),
					name + " is a built-in chord pads preset");
				const ChordPads& pads = m_project.chordPads;
				Json saved = Json::array();
				for (const ChordPad& pad : pads.pads)
					saved.push_back({{"chord", pad.chord}, {"voicing", pad.voicing}, {"adaptive", pad.adaptive}});
				const Json preset = {{"pads", saved}, {"rootKey", pads.rootKey}, {"scale", pads.scale},
					{"players", playersJson(pads)}, {"player", pads.player}};
				engine.modifySetting(context, g_presetsKey,
					[&name, &preset](const Json& value)
					{
						Json presets = value.is_object() ? value : Json::object();
						presets[name] = preset;
						return presets;
					});
				return Json::object();
			});
		addControl(engine,
			{"chordPads.deletePreset", 1, "Deletes a chord pads preset of the user's own (chordPads.savePreset).",
				objectSchema({{"name", stringSchema}}, {"name"}), objectSchema({}), false},
			[&engine](CommandContext& context)
			{
				const auto name = context.parameters().at("name").get<std::string>();
				require(userPresets(context).contains(name), "no chord pads preset " + name + " of the user's own");
				engine.modifySetting(context, g_presetsKey,
					[&name](const Json& value)
					{
						Json presets = value.is_object() ? value : Json::object();
						presets.erase(name);
						return presets;
					});
				return Json::object();
			});
		add(engine,
			{"chordPads.step", 1,
				"Step input from the chord pads (reference manual p. 1369): the pad's chord becomes a chord event at "
				"the start of the bar of the step input position (ticks, chordPads.set stepPosition) on the chord "
				"track, which is added if there is none, and the position moves to the next bar. With moveInsert the "
				"chord events from there on move a bar later first; without, a chord event already there gets the "
				"pad's chord. Needs stepInput, and a bar not after the one the longest project ends in.",
				objectSchema({{"pad", integerSchema}}, {"pad"}), padsResult, false},
			[this, changed](Project& project, CommandContext& context)
			{
				ChordPads& pads = project.chordPads;
				require(pads.stepInput, "step input is off");
				const ChordPad& pad = padAt(pads, context.parameters().at("pad"));
				require(!pad.chord.empty(), "the chord pad has no chord");
				const std::string chord = pad.chord;
				const auto bar = project.signatures.toBarsBeats(pads.stepPosition).bar;
				const time::Ticks at = project.signatures.barStart(bar);
				require(bar <= lastStepBar(project), "the step input position is past the longest project's end");
				const time::Ticks next = project.signatures.barStart(bar + 1);
				if (std::none_of(project.trackById.begin(), project.trackById.end(),
						[](const auto& entry) { return entry.second.type == TrackType::Chord; }))
					runEdit("track.add", project, {{"type", "chord"}}, context);
				time::TimeBase base = time::TimeBase::Musical;
				for (const auto& [id, track] : project.trackById)
					if (track.type == TrackType::Chord)
						base = track.timeBase;
				const auto inBase = [&project, base](time::Ticks tick)
				{
					return time::rebase({time::TimeBase::Musical, tick}, base, project.tempo, project.sampleRate).value;
				};
				// The chord events keep their positions in the chord track's time base, where chord.add refuses a
				// second event: a bar's start in ticks doesn't always come back from the samples of a linear track
				// (#1048).
				const int64_t start = inBase(at);
				if (pads.moveInsert)
				{
					// Events past the last step position stay where they are, as far as a chord event can be, so that
					// moving them a bar on can't overflow (#1174).
					const int64_t last = inBase(stepEnd(project));
					require(std::none_of(project.chords.events.begin(), project.chords.events.end(),
								[start, last](const ChordEvent& event)
								{ return event.position.value >= start && event.position.value > last; }),
						"a chord event to move is past the longest project's end");
					// An event's position in ticks, and for one on a bar's start exactly that bar's, so that it moves
					// onto the next bar's start in samples too, whatever the tempo there.
					const auto ticksOf = [&project, &inBase](const time::Position& position)
					{
						const time::Ticks ticks = time::toTicks(position, project.tempo, project.sampleRate);
						const int64_t own = project.signatures.toBarsBeats(ticks).bar;
						for (int64_t candidate = own; candidate <= own + 1; ++candidate)
						{
							const time::Ticks barStart = project.signatures.barStart(candidate);
							if (inBase(barStart) == position.value)
								return barStart;
						}
						return ticks;
					};
					Json moved = Json::array();
					for (const ChordEvent& event : project.chords.events)
						if (event.position.value >= start)
							moved.push_back(
								{{"event", event.id}, {"position", inBase(ticksOf(event.position) + next - at)}});
					if (!moved.empty())
						runEdit("chord.set", project, {{"events", moved}}, context);
				}
				const auto there = std::find_if(project.chords.events.begin(), project.chords.events.end(),
					[start](const ChordEvent& event) { return event.position.value == start; });
				if (there != project.chords.events.end())
					runEdit("chord.set", project, {{"events", {{{"event", there->id}, {"chord", chord}}}}}, context);
				else
					runEdit("chord.add", project, {{"position", start}, {"chord", chord}}, context);
				project.chordPads.stepPosition = next;
				return changed(project, context);
			});
		add(engine,
			{"chordPads.toTrack", 1,
				"A chord pad dragged onto a track (reference manual p. 1370): onto the chord track, its chord as a "
				"chord event at position (ticks, default the cursor); onto a MIDI or instrument track, a MIDI part a "
				"bar long there with its voicing.",
				objectSchema(
					{{"pad", integerSchema}, {"track", integerSchema}, {"position", integerSchema}}, {"pad", "track"}),
				{{"type", "object"}}, false},
			[this](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const auto index = parameters.at("pad").get<int>();
				const ChordPad& pad = padAt(project.chordPads, parameters.at("pad"));
				require(!pad.chord.empty(), "the chord pad has no chord");
				const auto id = parameters.at("track").get<ObjectId>();
				const auto found = project.trackById.find(id);
				require(found != project.trackById.end(), "no track " + std::to_string(id));
				const Track& track = found->second;
				const time::Ticks at = parameters.contains("position")
					? parameters.at("position").get<time::Ticks>()
					: time::toTicks(
						  {time::TimeBase::Linear, context.call("transport.get").at("position").get<int64_t>()},
						  project.tempo, project.sampleRate);
				require(at >= 0, "position must not be before the project start");
				const auto inBase = [&project, &track](time::Ticks tick)
				{
					return time::rebase(
						{time::TimeBase::Musical, tick}, track.timeBase, project.tempo, project.sampleRate)
						.value;
				};
				if (track.type == TrackType::Chord)
					return runEdit("chord.add", project, {{"position", inBase(at)}, {"chord", pad.chord}}, context);
				require(track.type == TrackType::Midi || track.type == TrackType::Instrument,
					"chord pads go onto the chord track or a MIDI or instrument track");
				const std::vector<int> pitches = chordPadVoicings(project.chordPads).at(static_cast<size_t>(index));
				const time::Ticks end = project.signatures.barStart(project.signatures.toBarsBeats(at).bar + 1);
				const time::Ticks length =
					std::max<time::Ticks>(end - at, project.signatures.barStart(1) - project.signatures.barStart(0));
				const Json added = runEdit("midiPart.add", project,
					{{"track", id}, {"position", inBase(at)}, {"length", inBase(at + length) - inBase(at)}}, context);
				const auto part = added.at("part").at("id").get<ObjectId>();
				Json notes = Json::array();
				for (const int pitch : pitches)
					notes.push_back({{"position", 0}, {"length", length}, {"pitch", pitch}, {"velocity", 0.75}});
				runEdit("note.add", project, {{"part", part}, {"notes", notes}}, context);
				return Json{{"part", toJson(project.midiParts.at(part))}};
			});

		// While the transport records, a triggered pad's chord becomes a chord event on the record-enabled chord track
		// (reference manual p. 1368), where the cursor is.
		const auto record = [this](CommandContext& context, int pad)
		{
			const ChordPads& pads = m_project.chordPads;
			if (!m_project.chords.recordEnable || pad < 0 || pad >= static_cast<int>(pads.pads.size()) ||
				pads.pads[static_cast<size_t>(pad)].chord.empty() ||
				std::none_of(
					m_project.trackById.begin(), m_project.trackById.end(),
					[](const auto& entry) { return entry.second.type == TrackType::Chord; }))
				return;
			const Json transport = context.call("transport.get");
			if (transport.value("recording", false))
				context.call("chordPads.recordChord", {{"pad", pad}, {"position", transport.at("position")}});
		};
		add(engine,
			{"chordPads.recordChord", 1,
				"Records a chord pad's chord as a chord event on the chord track at position (samples on the "
				"transport), as pads triggered while the transport records do on a record-enabled chord track "
				"(reference manual p. 1368); a chord event there gets the pad's chord.",
				objectSchema({{"pad", integerSchema}, {"position", integerSchema}}, {"pad", "position"}),
				{{"type", "object"}}, false},
			[this](Project& project, CommandContext& context)
			{
				const ChordPad& pad = padAt(project.chordPads, context.parameters().at("pad"));
				require(!pad.chord.empty(), "the chord pad has no chord");
				const auto position = context.parameters().at("position").get<int64_t>();
				require(position >= 0 && position <= time::g_maxPosition, "position must be a transport position");
				const auto track = std::find_if(project.trackById.begin(), project.trackById.end(),
					[](const auto& entry) { return entry.second.type == TrackType::Chord; });
				require(track != project.trackById.end(), "the project has no chord track");
				const int64_t at = time::rebase(
					{time::TimeBase::Linear, position}, track->second.timeBase, project.tempo, project.sampleRate)
									   .value;
				const std::string chord = pad.chord;
				const auto there = std::find_if(project.chords.events.begin(), project.chords.events.end(),
					[at](const ChordEvent& event) { return event.position.value == at; });
				if (there != project.chords.events.end())
					return runEdit(
						"chord.set", project, {{"events", {{{"event", there->id}, {"chord", chord}}}}}, context);
				return runEdit("chord.add", project, {{"position", at}, {"chord", chord}}, context);
			});

		// The chords that sound, by what triggered them: the pads by their index, an audition after them.
		// By player (#1089).
		auto voices = std::make_shared<std::array<midi::PadVoices, ChordPadKeys::g_maxPlayers>>();
		addControl(engine,
			{"chordPads.trigger", 1,
				"Triggers a chord pad, or auditions a chord (an assistant's suggestion), as a click does (reference "
				"manual p. 1366): on (the default) plays its voicing, off ends it; with latch (chordPads.set) an off "
				"ends nothing and an on ends the pad's chord if it sounds. A chord played over another treats the "
				"other's notes as overlaps says (chordPads.set). The notes play as the chord pads' (midi.keyboard "
				"chordPads), the highest voice on MIDI channel 1, the next on channel 2 and so on: MIDI and "
				"instrument tracks whose MIDI input is the chord pads, and with outputMode those set to all MIDI "
				"inputs, play them while they monitor or are record-enabled, and record them. velocity: 1 to 127, "
				"default the pads' velocity. Returns the pitches the pad plays now, or that ended, and on, whether it "
				"sounds now. Emits chordPads.triggered with the pad (-1 for a chord), the chord, on and the pitches, "
				"also for the pads that the overlap ended.",
				needsOne(objectSchema({{"pad", integerSchema}, {"chord", stringSchema}, {"on", booleanSchema},
					{"velocity", integerSchema}})),
				objectSchema({{"pitches", padsSchema}, {"on", booleanSchema}}, {"pitches", "on"}), false},
			[this, voices, record, remote](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const ChordPads& pads = m_project.chordPads;
				require(parameters.contains("pad") != parameters.contains("chord"), "give a pad or a chord");
				bool on = parameters.value("on", true);
				const int velocity = parameters.value("velocity", pads.velocity);
				require(velocity >= 1 && velocity <= 127, "velocity must be 1 to 127");
				int index = -1;
				std::string chord;
				size_t trigger = g_maxPads; // an audition's
				std::optional<midi::ChordSymbol> audition;
				if (parameters.contains("pad"))
				{
					index = parameters.at("pad").get<int>();
					require(index >= 0 && index < static_cast<int>(pads.pads.size()),
						"no chord pad " + std::to_string(index));
					chord = pads.pads[static_cast<size_t>(index)].chord;
					trigger = static_cast<size_t>(index);
				}
				else
				{
					audition = chordFrom(parameters.at("chord").get<std::string>());
					chord = midi::chordName(*audition);
				}
				const auto holds = [&voices](size_t pad)
				{
					return std::any_of(voices->begin(), voices->end(),
						[pad](const midi::PadVoices& player) { return player.holds(pad); });
				};
				if (pads.latch && index >= 0)
				{
					if (!on)
						return Json{{"pitches", Json::array()}, {"on", holds(trigger)}};
					on = !holds(trigger); // the next trigger of a latched pad ends its chord
				}
				std::array<bool, g_maxPads> held{};
				for (size_t pad = 0; pad < held.size(); ++pad)
					held.at(pad) = holds(pad);
				// Each player that plays voices the chord its way, into the tracks it plays (p. 1377); the selected
				// player's pitches are the result. An off ends what the on played, whatever changed since.
				std::vector<int> pitches;
				std::vector<int> ended;
				std::vector<int> players = playingPlayers(pads);
				if (!on)
					for (int player = 0; player < static_cast<int>(voices->size()); ++player)
						if (std::find(players.begin(), players.end(), player) == players.end())
							players.push_back(player);
				for (const int player : players)
				{
					if (player >= static_cast<int>(voices->size()))
						continue;
					midi::PadVoices& playing = voices->at(static_cast<size_t>(player));
					std::vector<int> voicing;
					if (on && audition)
						voicing = playerVoicing(pads, *audition, false, {}, player);
					else if (on)
						voicing = chordPadVoicings(pads, player).at(trigger);
					Json messages = Json::array();
					const auto emit = [&messages, &ended, player, &pads](
										  bool start, uint8_t pitch, uint8_t level, uint8_t channel)
					{
						messages.push_back({{"bytes", {(start ? 0x90 : 0x80) | channel, pitch, level}}});
						if (!start && player == pads.player)
							ended.push_back(pitch);
					};
					if (on && !voicing.empty())
					{
						std::vector<uint8_t> notes;
						notes.reserve(voicing.size());
						for (const int pitch : voicing)
							notes.push_back(static_cast<uint8_t>(std::clamp(pitch, 0, 127)));
						playing.on(trigger, notes.data(), notes.size(), static_cast<uint8_t>(velocity),
							overlapOf(pads.overlaps), emit);
					}
					else
					{
						playing.off(trigger, emit);
					}
					if (player == pads.player)
						pitches = voicing;
					if (!messages.empty())
						context.call(
							"midi.keyboard", {{"messages", messages}, {"chordPads", true}, {"player", player}});
				}
				if (!on || pitches.empty())
				{
					std::sort(ended.begin(), ended.end());
					pitches = ended;
					on = false;
				}
				for (size_t pad = 0; pad < held.size(); ++pad)
					if (held.at(pad) && !holds(pad) && static_cast<int>(pad) != index)
						context.emit("chordPads.triggered",
							{{"pad", pad}, {"chord", pad < pads.pads.size() ? pads.pads[pad].chord : std::string()},
								{"on", false}, {"pitches", Json::array()}});
				context.emit(
					"chordPads.triggered", {{"pad", index}, {"chord", chord}, {"on", on}, {"pitches", pitches}});
				if (on && index >= 0)
				{
					remote->last = index;
					record(context, index);
				}
				return Json{{"pitches", pitches}, {"on", on}};
			});
		addControl(engine,
			{"chordPads.listen", 1,
				"Listens to the next notes from the MIDI inputs, which play as they are meanwhile: assign (Assign from "
				"MIDI Input, reference manual p. 1360), a pad that gets the chord of the notes played once they are "
				"all released; learn (MIDI Learn, p. 1388): range, the next two keys make the pads' remote range, or "
				"a chord modifier's name (chordPads.set modifierKeys), the next key becomes its key. Without either "
				"it stops listening. Emits chordPads.listening with listening ({assign: pad}, {learn: target} or "
				"null), also once it is done; chordPads.get has it too.",
				objectSchema({{"assign", integerSchema},
					{"learn",
						{{"type", "string"},
							{"enum",
								{"range", "previousVoicing", "nextVoicing", "fewerTensions", "moreTensions",
									"transposeDown", "transposeUp"}}}}}),
				objectSchema({{"listening", {{"type", {"object", "null"}}}}}, {"listening"}), false},
			[this, remote](CommandContext& context)
			{
				const Json& parameters = context.parameters();
				require(!(parameters.contains("assign") && parameters.contains("learn")), "give assign or learn");
				RemoteState& state = *remote;
				if (parameters.contains("assign"))
				{
					const int pad = parameters.at("assign").get<int>();
					require(pad >= 0 && pad < static_cast<int>(m_project.chordPads.pads.size()),
						"no chord pad " + std::to_string(pad));
					require(!m_project.chordPads.pads[static_cast<size_t>(pad)].locked,
						"chord pad " + std::to_string(pad) + " is locked");
				}
				state.listen.clear();
				if (parameters.contains("assign"))
					state.listen = "assign";
				else if (parameters.contains("learn"))
					state.listen = "learn";
				state.pad = parameters.value("assign", -1);
				state.target = parameters.value("learn", std::string());
				state.down.clear();
				state.pitches.clear();
				context.call("midi.padListen", {{"on", !state.listen.empty()}});
				const Json result = {{"listening", state.listening()}};
				context.emit("chordPads.listening", result);
				return result;
			});
		addControl(engine,
			{"chordPads.remoteInput", 1,
				"What the chord pads' remote keys did, as the audio engine reports it from the MIDI inputs (#1089): "
				"each of events has a kind and a key (a MIDI note) and velocity (0 for a note-off): triggered (the key "
				"triggered its pad, which chordPads.triggered announces with remote true, and while the transport "
				"records a record-enabled chord track records its chord), released (the key ended its pad's chord), "
				"modifier (a chord modifier's key, chordPads.set modifierKeys: it changes the pads whose keys are "
				"down, else the one triggered last) and note (a note while chordPads.listen listens).",
				objectSchema(
					{{"events",
						{{"type", "array"},
							{"items",
								objectSchema(
									{{"kind",
										 {{"type", "string"}, {"enum", {"triggered", "released", "modifier", "note"}}}},
										{"key", integerSchema}, {"velocity", integerSchema}},
									{"kind", "key"})}}}},
					{"events"}),
				objectSchema({}), false},
			[this, remote, record](CommandContext& context)
			{
				RemoteState& state = *remote;
				for (const Json& event : context.parameters().at("events"))
				{
					const ChordPads& pads = m_project.chordPads;
					const auto kind = event.at("kind").get<std::string>();
					const int key = event.at("key").get<int>();
					const int velocity = event.value("velocity", 0);
					const int pad = key - pads.remoteStart;
					const bool isPad = pad >= 0 && pad < static_cast<int>(pads.pads.size()) && key <= pads.remoteEnd;
					if ((kind == "triggered" || kind == "released") && isPad)
					{
						const bool on = kind == "triggered";
						if (on)
						{
							state.held.insert(pad);
							state.last = pad;
						}
						else
						{
							state.held.erase(pad);
						}
						const auto index = static_cast<size_t>(pad);
						context.emit("chordPads.triggered",
							{{"pad", pad}, {"chord", pads.pads[index].chord}, {"on", on},
								{"pitches", on ? Json(chordPadVoicings(pads).at(index)) : Json::array()},
								{"remote", true}});
						if (on)
							record(context, pad);
					}
					else if (kind == "modifier")
					{
						size_t which = 0;
						while (which < pads.modifierKeys.size() && pads.modifierKeys.at(which) != key)
							++which;
						std::vector<int> targets(state.held.begin(), state.held.end());
						if (targets.empty() && state.last >= 0 && state.last < static_cast<int>(pads.pads.size()))
							targets.push_back(state.last);
						if (which == pads.modifierKeys.size() || targets.empty())
							continue;
						const int step = which % 2 == 0 ? -1 : 1;
						static constexpr std::array<const char*, 3> changes = {"voicing", "tensions", "transpose"};
						const char* change = changes.at(which / 2);
						context.call("chordPads.modify", {{"pads", targets}, {change, step}});
					}
					else if (kind == "note" && !state.listen.empty())
					{
						if (velocity > 0)
						{
							state.down.insert(key);
							if (std::find(state.pitches.begin(), state.pitches.end(), key) == state.pitches.end())
								state.pitches.push_back(key);
						}
						else
						{
							state.down.erase(key);
						}
						bool done = false;
						if (state.listen == "assign" && state.down.empty() && !state.pitches.empty())
						{
							context.call("chordPads.assign",
								{{"assignments", {{{"pad", state.pad}, {"pitches", state.pitches}}}}});
							done = true;
						}
						else if (state.listen == "learn" && state.target == "range" && state.pitches.size() == 2)
						{
							const auto [low, high] = std::minmax(state.pitches.front(), state.pitches.back());
							context.call("chordPads.set", {{"remoteStart", low}, {"remoteEnd", high}});
							done = true;
						}
						else if (state.listen == "learn" && state.target != "range" && velocity > 0)
						{
							context.call("chordPads.set", {{"modifierKeys", {{state.target, key}}}});
							done = true;
						}
						if (done)
							context.call("chordPads.listen");
					}
				}
				return Json::object();
			});
		add(engine,
			{"chordPads.assistant", 1,
				"The chord assistant (reference manual pp. 1372-1375). kind list (the List assistant): suggestions for "
				"the chord after chord (the reference, such as the selected pad's), at most 24, each with its chord, "
				"its degree in the root key and its level: 0 common (green), 1 adventurous (orange), 2 far (red), "
				"by how many of its notes are outside the scale of the loaded preset (chordPads.get preset) on the "
				"root key; algorithm cadence (the default: the common ones first, and of the same level those that "
				"follow most strongly first) or commonNotes (those that share commonNotes notes, default 2, with "
				"it). kind circle (the Circle of Fifths assistant): outer, the twelve major chords from the "
				"root key's in fifths (minor: from its relative major's), and inner, their relative minor chords, "
				"each with its degree in the key, empty for chords outside it. cadenceType (with cadence, p. 1373): "
				"all (the default), or the chords of one harmonic function, by the scale degree of their root: tonic "
				"(I, iii, vi), subdominant (ii, IV) or dominant (V, vii). kind proximity (the Proximity assistant, "
				"p. 1374): the same chords around the reference, at most 24, nearest first, each with its distance: "
				"how many of its notes the reference doesn't have, one more for a seventh.",
				objectSchema(
					{{"kind", {{"type", "string"}, {"enum", {"list", "circle", "proximity"}}}}, {"chord", stringSchema},
						{"algorithm", {{"type", "string"}, {"enum", {"cadence", "commonNotes"}}}},
						{"cadenceType", {{"type", "string"}, {"enum", {"all", "tonic", "subdominant", "dominant"}}}},
						{"commonNotes", integerSchema}, {"minor", booleanSchema}},
					{"kind"}),
				{{"type", "object"}}, true},
			[](Project& project, CommandContext& context)
			{
				const Json& parameters = context.parameters();
				const int key = project.chordPads.rootKey;
				// A chord's degree in the key, empty for a chord with notes outside the key's scale.
				const auto entry = [key](const midi::ChordSymbol& chord, const std::string& scale)
				{
					const std::vector<int> steps = scaleOf(scale);
					const std::vector<int> classes = classesOf(chord);
					const bool inKey = std::all_of(
						classes.begin(), classes.end(), [&steps, key](int note) { return inScale(steps, key, note); });
					return Json{{"chord", midi::chordName(chord)},
						{"degree", inKey ? chordpads::degreeName(chord, key, scale) : std::string()}};
				};
				if (parameters.at("kind") == "circle")
				{
					const bool minor = parameters.value("minor", false);
					const std::string scale = minor ? "minor" : "major";
					Json outer = Json::array();
					Json inner = Json::array();
					for (int i = 0; i < 12; ++i)
					{
						midi::ChordSymbol major;
						major.root = pitchClass(key + (minor ? 3 : 0) + (7 * i));
						midi::ChordSymbol relative;
						relative.root = pitchClass(major.root + 9);
						relative.type = "min";
						outer.push_back(entry(major, scale));
						inner.push_back(entry(relative, scale));
					}
					return Json{{"outer", outer}, {"inner", inner}};
				}
				const midi::ChordSymbol reference = chordFrom(parameters.value("chord", std::string()));
				Json suggestions = Json::array();
				const std::string& scale = project.chordPads.scale;
				const std::string algorithm = parameters.value("algorithm", std::string("cadence"));
				const bool near = parameters.at("kind") == "proximity";
				for (const chordpads::Suggestion& suggestion : near
						? chordpads::proximity(reference, key, scale)
						: chordpads::suggestions(reference, key, scale, algorithm, parameters.value("commonNotes", 2),
							  algorithm == "cadence" ? parameters.value("cadenceType", std::string("all")) : "all"))
				{
					Json item = entry(suggestion.chord, scale);
					item["degree"] = chordpads::degreeName(suggestion.chord, key, scale);
					item["level"] = suggestion.level;
					if (near)
						item["distance"] = suggestion.distance;
					suggestions.push_back(item);
				}
				return Json{{"suggestions", suggestions}};
			});
	}
}
