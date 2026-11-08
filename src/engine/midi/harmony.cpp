// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/midi/harmony.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <set>
#include <tuple>

namespace gnubase::engine::midi
{
	namespace
	{
		constexpr std::array<const char*, 12> g_names = {
			"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

		struct TypeEntry
		{
			const char* name;
			const char* suffix;
			std::vector<int> intervals;
		};

		const std::vector<TypeEntry>& typeEntries()
		{
			static const std::vector<TypeEntry> types = {{"maj", "", {0, 4, 7}}, {"min", "m", {0, 3, 7}},
				{"dim", "dim", {0, 3, 6}}, {"aug", "aug", {0, 4, 8}}, {"sus2", "sus2", {0, 2, 7}},
				{"sus4", "sus4", {0, 5, 7}}};
			return types;
		}

		const TypeEntry* typeEntry(const std::string& name)
		{
			for (const TypeEntry& entry : typeEntries())
				if (name == entry.name)
					return &entry;
			return nullptr;
		}

		// The tensions' intervals, in the order of chordTensions().
		constexpr std::array<int, 10> g_tensionIntervals = {9, 10, 11, 1, 2, 3, 5, 6, 8, 9};

		int pitchClass(int pitch)
		{
			return ((pitch % 12) + 12) % 12;
		}

		bool startsWith(const std::string& text, size_t at, const std::string& prefix)
		{
			return text.compare(at, prefix.size(), prefix) == 0;
		}

		// A note name (C to B with # or b) at text[at], which it moves past; -1 if there is none.
		int parseNote(const std::string& text, size_t& at)
		{
			static const std::string letters = "C D EF G A B";
			if (at >= text.size() || letters.find(text[at]) == std::string::npos || text[at] == ' ')
				return -1;
			int note = static_cast<int>(letters.find(text[at++]));
			if (at < text.size() && (text[at] == '#' || text[at] == 'b'))
				note += text[at++] == '#' ? 1 : -1;
			return pitchClass(note);
		}

		void addTension(std::vector<std::string>& tensions, const std::string& tension)
		{
			if (std::find(tensions.begin(), tensions.end(), tension) == tensions.end())
				tensions.push_back(tension);
		}

		bool hasTension(const ChordSymbol& chord, const std::string& tension)
		{
			return std::find(chord.tensions.begin(), chord.tensions.end(), tension) != chord.tensions.end();
		}

		std::string joined(const std::vector<std::string>& items)
		{
			std::string text;
			for (const std::string& item : items)
				text += (text.empty() ? "" : ",") + item;
			return text;
		}

		// The step from one root to another the nearer way, -5 to 6 semitones.
		int rootStep(int from, int to)
		{
			const int step = pitchClass(to - from);
			return step > 6 ? step - 12 : step;
		}

		// The pitch nearest to pitch whose class is one of classes (relative to root), down on a tie.
		int nearestOf(int pitch, int root, const std::vector<int>& intervals)
		{
			for (int distance = 0; distance < 12; ++distance)
				for (const int candidate : {pitch - distance, pitch + distance})
					if (std::find(intervals.begin(), intervals.end(), pitchClass(candidate - root)) != intervals.end())
						return candidate;
			return pitch;
		}

		// The index of the pitch's class among intervals from root, or -1.
		int indexOf(int pitch, int root, const std::vector<int>& intervals)
		{
			const auto found = std::find(intervals.begin(), intervals.end(), pitchClass(pitch - root));
			return found == intervals.end() ? -1 : static_cast<int>(found - intervals.begin());
		}

		int clampPitch(int pitch)
		{
			return std::clamp(pitch, 0, 127);
		}
	}

	bool operator==(const ChordSymbol& a, const ChordSymbol& b)
	{
		return std::tie(a.root, a.type, a.tensions, a.bass) == std::tie(b.root, b.type, b.tensions, b.bass);
	}

	const std::vector<std::string>& chordTypeNames()
	{
		static const std::vector<std::string> names = {"maj", "min", "dim", "aug", "sus2", "sus4"};
		return names;
	}

	const std::vector<std::string>& chordTensions()
	{
		static const std::vector<std::string> tensions = {"6", "7", "maj7", "b9", "9", "#9", "11", "#11", "b13", "13"};
		return tensions;
	}

	std::optional<ChordSymbol> parseChord(const std::string& input)
	{
		std::string text;
		for (const char c : input)
			if (c != ' ')
				text += c;
		ChordSymbol chord;
		if (text.empty() || text == "X")
			return chord;
		size_t at = 0;
		chord.root = parseNote(text, at);
		if (chord.root < 0)
			return std::nullopt;
		std::vector<std::string> tensions;
		// The type, if any, by its spelling; m7b5 is diminished with the seventh, dim7 with the diminished seventh (6).
		static const std::vector<std::pair<std::string, std::string>> types = {{"m7b5", "dim"}, {"min", "min"},
			{"dim7", "dim"}, {"dim", "dim"}, {"aug", "aug"}, {"+", "aug"}, {"sus2", "sus2"}, {"sus4", "sus4"},
			{"sus", "sus4"}};
		const auto type = std::find_if(
			types.begin(), types.end(), [&](const auto& entry) { return startsWith(text, at, entry.first); });
		if (type != types.end())
		{
			chord.type = type->second;
			if (type->first == "m7b5")
				tensions.emplace_back("7");
			if (type->first == "dim7")
				tensions.emplace_back("6");
			at += type->first.size();
		}
		else if (startsWith(text, at, "maj") &&
			(at + 3 >= text.size() || std::isdigit(static_cast<unsigned char>(text[at + 3])) == 0))
		{
			at += 3; // a major chord; maj7 and the like are tensions
		}
		else if (startsWith(text, at, "m") && !startsWith(text, at, "maj"))
		{
			chord.type = "min";
			at += 1;
		}
		// Tensions, longest first; 9, 11 and 13 bring the seventh unless after add or a sixth or seventh.
		static const std::vector<std::pair<std::string, std::vector<std::string>>> tokens = {{"maj13", {"maj7", "13"}},
			{"maj11", {"maj7", "11"}}, {"maj9", {"maj7", "9"}}, {"maj7", {"maj7"}}, {"b13", {"b13"}}, {"#11", {"#11"}},
			{"b9", {"b9"}}, {"#9", {"#9"}}, {"13", {"13"}}, {"11", {"11"}}, {"9", {"9"}}, {"7", {"7"}}, {"6", {"6"}}};
		bool added = false;
		while (at < text.size() && text[at] != '/')
		{
			if (text[at] == '(' || text[at] == ')' || text[at] == ',')
			{
				++at;
				continue;
			}
			if (startsWith(text, at, "add"))
			{
				added = true;
				at += 3;
				continue;
			}
			const auto token = std::find_if(
				tokens.begin(), tokens.end(), [&](const auto& entry) { return startsWith(text, at, entry.first); });
			if (token == tokens.end())
				return std::nullopt;
			const bool extension = token->first == "9" || token->first == "11" || token->first == "13";
			const bool seventh = std::any_of(tensions.begin(), tensions.end(),
				[](const std::string& tension) { return tension == "6" || tension == "7" || tension == "maj7"; });
			if (extension && !added && !seventh)
				addTension(tensions, "7");
			for (const std::string& tension : token->second)
				addTension(tensions, tension);
			at += token->first.size();
		}
		if (at < text.size())
		{
			++at;
			chord.bass = parseNote(text, at);
			if (chord.bass < 0 || at != text.size())
				return std::nullopt;
			if (chord.bass == chord.root)
				chord.bass = -1;
		}
		for (const std::string& tension : chordTensions())
			if (std::find(tensions.begin(), tensions.end(), tension) != tensions.end())
				chord.tensions.push_back(tension);
		return chord;
	}

	std::string chordName(const ChordSymbol& chord)
	{
		if (chord.root < 0)
			return "X";
		const TypeEntry* type = typeEntry(chord.type);
		std::string name = g_names.at(static_cast<size_t>(pitchClass(chord.root)));
		std::vector<std::string> tensions = chord.tensions;
		if (chord.type == "dim" && hasTension(chord, "7"))
		{
			name += "m7b5";
			tensions.erase(std::find(tensions.begin(), tensions.end(), "7"));
		}
		else if (chord.type == "dim" && hasTension(chord, "6"))
		{
			name += "dim7";
			tensions.erase(std::find(tensions.begin(), tensions.end(), "6"));
		}
		else if (type != nullptr)
		{
			name += type->suffix;
		}
		if (!tensions.empty())
		{
			const std::string& first = tensions.front();
			if (first == "6" || first == "7" || first == "maj7")
			{
				name += first;
				tensions.erase(tensions.begin());
				if (!tensions.empty())
					name += "(" + joined(tensions) + ")";
			}
			else
			{
				name += "add" + (tensions.size() == 1 ? tensions.front() : "(" + joined(tensions) + ")");
			}
		}
		if (chord.bass >= 0 && chord.bass != chord.root)
			name += std::string("/") + g_names.at(static_cast<size_t>(pitchClass(chord.bass)));
		return name;
	}

	std::vector<int> chordIntervals(const ChordSymbol& chord)
	{
		std::vector<int> intervals;
		const TypeEntry* type = typeEntry(chord.type);
		if (chord.root < 0 || type == nullptr)
			return intervals;
		intervals = type->intervals;
		const std::vector<std::string>& names = chordTensions();
		for (const std::string& tension : chord.tensions)
		{
			const auto found = std::find(names.begin(), names.end(), tension);
			if (found == names.end())
				continue;
			const int interval = g_tensionIntervals.at(static_cast<size_t>(found - names.begin()));
			if (std::find(intervals.begin(), intervals.end(), interval) == intervals.end())
				intervals.push_back(interval);
		}
		return intervals;
	}

	std::optional<Scale> parseScale(const std::string& text)
	{
		size_t at = 0;
		const int root = parseNote(text, at);
		const size_t space = text.find(' ', at);
		if (root < 0 || space != at)
			return std::nullopt;
		const std::optional<ScaleType> type = scaleTypeFromName(text.substr(space + 1));
		if (!type.has_value())
			return std::nullopt;
		return Scale{root, *type};
	}

	std::string scaleName(const Scale& scale)
	{
		return g_names.at(static_cast<size_t>(pitchClass(scale.root))) + std::string(" ") + scaleTypeName(scale.type);
	}

	std::optional<Scale> automaticScale(const ChordSymbol& chord)
	{
		if (chord.root < 0)
			return std::nullopt;
		const auto with = [&chord](const char* tension) { return hasTension(chord, tension); };
		ScaleType type = ScaleType::Major;
		if (chord.type == "min" && with("maj7"))
			type = ScaleType::MelodicMinor;
		else if (chord.type == "min" && (with("6") || with("7") || with("13")))
			type = ScaleType::Dorian;
		else if (chord.type == "min")
			type = ScaleType::Minor;
		else if (chord.type == "dim")
			type = ScaleType::Locrian;
		else if (chord.type == "aug")
			type = ScaleType::WholeTone;
		else if (with("#11") && chord.type == "maj")
			type = ScaleType::Lydian;
		else if (with("7"))
			type = ScaleType::Mixolydian;
		return Scale{chord.root, type};
	}

	std::optional<ChordSymbol> recognizeChordSymbol(const std::vector<int>& pitches, bool bass, bool tensions)
	{
		std::set<int> classes;
		for (const int pitch : pitches)
			classes.insert(pitchClass(pitch));
		if (classes.size() < 3)
			return std::nullopt;
		const int lowest = pitchClass(*std::min_element(pitches.begin(), pitches.end()));
		std::optional<ChordSymbol> best;
		std::pair<size_t, int> bestScore{std::numeric_limits<size_t>::max(), 0};
		for (const int root : classes)
			for (const TypeEntry& type : typeEntries())
			{
				std::set<int> rest = classes;
				bool fits = true;
				for (const int interval : type.intervals)
					fits = rest.erase(pitchClass(root + interval)) != 0 && fits;
				if (!fits)
					continue;
				ChordSymbol chord{root, type.name, {}, -1};
				for (size_t i = 0; i < chordTensions().size(); ++i)
				{
					const std::string& tension = chordTensions()[i];
					const int pitch = pitchClass(root + g_tensionIntervals.at(i));
					// A sixth and a 13 are the same note: a 13 with the seventh.
					if (rest.count(pitch) == 0 || (tension == "6" && rest.count(pitchClass(root + 10)) != 0))
						continue;
					rest.erase(pitch);
					chord.tensions.push_back(tension);
				}
				if (!rest.empty())
					continue;
				const std::pair<size_t, int> score{chord.tensions.size(), root == lowest ? 0 : 1};
				if (score < bestScore)
				{
					bestScore = score;
					best = chord;
				}
			}
		if (!best.has_value())
			return std::nullopt;
		if (!tensions)
			best->tensions.clear();
		if (bass && lowest != best->root)
			best->bass = lowest;
		return best;
	}

	std::vector<int> voiceChord(
		const ChordSymbol& chord, const VoicingSettings& settings, const std::vector<int>& previous)
	{
		const std::vector<int> intervals = chordIntervals(chord);
		if (intervals.empty())
			return {};
		const int offset = 12 * settings.octaveOffset;
		std::vector<int> upper;
		upper.reserve(intervals.size());
		for (const int interval : intervals)
			upper.push_back(60 + offset + pitchClass(chord.root) + interval);
		std::sort(upper.begin(), upper.end());
		if (settings.adaptive && previous.size() > 1)
		{
			// The previous voicing's upper notes, without its bass note.
			const std::vector<int> before(previous.begin() + 1, previous.end());
			const auto distance = [](const std::vector<int>& from, const std::vector<int>& to)
			{
				int sum = 0;
				for (const int pitch : from)
				{
					int nearest = std::numeric_limits<int>::max();
					for (const int other : to)
						nearest = std::min(nearest, std::abs(pitch - other));
					sum += nearest;
				}
				return sum;
			};
			std::vector<int> best = upper;
			int bestCost = distance(upper, before) + distance(before, upper);
			for (const int shift : {-12, 0})
			{
				std::vector<int> candidate = upper;
				for (int& pitch : candidate)
					pitch += shift;
				for (size_t inversion = 0; inversion < candidate.size(); ++inversion)
				{
					if (inversion > 0)
					{
						candidate.push_back(candidate.front() + 12);
						candidate.erase(candidate.begin());
					}
					if (candidate.front() < 52 + offset || candidate.back() > 84 + offset)
						continue;
					const int cost = distance(candidate, before) + distance(before, candidate);
					if (cost < bestCost)
					{
						bestCost = cost;
						best = candidate;
					}
				}
			}
			upper = best;
		}
		std::vector<int> voicing;
		voicing.reserve(upper.size() + 1);
		voicing.push_back(48 + offset + pitchClass(chord.bass >= 0 ? chord.bass : chord.root));
		voicing.insert(voicing.end(), upper.begin(), upper.end());
		for (int& pitch : voicing)
			pitch = clampPitch(pitch);
		return voicing;
	}

	const std::vector<std::string>& followModeNames()
	{
		static const std::vector<std::string> names = {
			"off", "chordsAndScales", "chords", "scales", "rootNotes", "voicings"};
		return names;
	}

	int followPitch(int pitch, const std::string& mode, const Harmony& from, const Harmony& to)
	{
		if (mode == "off" || from.chord.root < 0 || to.chord.root < 0)
			return pitch;
		const std::vector<int> toChord = chordIntervals(to.chord);
		if (mode == "voicings" && !from.voicing.empty() && !to.voicing.empty())
		{
			// The voice of the same class nearest to the pitch, and the octaves between them.
			int voice = -1;
			for (size_t i = 0; i < from.voicing.size(); ++i)
				if (pitchClass(from.voicing[i] - pitch) == 0 &&
					(voice < 0 ||
						std::abs(from.voicing[i] - pitch) < std::abs(from.voicing[static_cast<size_t>(voice)] - pitch)))
					voice = static_cast<int>(i);
			if (voice >= 0)
			{
				const size_t target = std::min(static_cast<size_t>(voice), to.voicing.size() - 1);
				return clampPitch(to.voicing[target] + pitch - from.voicing[static_cast<size_t>(voice)]);
			}
		}
		const int moved = pitch + rootStep(from.chord.root, to.chord.root);
		if (mode == "rootNotes")
			return clampPitch(moved);
		const std::vector<int> toScale = scaleSteps(to.scale.type);
		if (mode == "scales")
			return clampPitch(nearestOf(moved, to.scale.root, toScale));
		if (mode == "chordsAndScales")
		{
			const std::vector<int> fromChord = chordIntervals(from.chord);
			const int tone = indexOf(pitch, from.chord.root, fromChord);
			if (tone >= 0 && static_cast<size_t>(tone) < toChord.size())
				return clampPitch(moved + toChord[static_cast<size_t>(tone)] - fromChord[static_cast<size_t>(tone)]);
			// The transpose function's scale correction, the same degree of the new scale, after the same root step
			// as the chord notes: a tritone moves them all up, not some down.
			const int step = rootStep(from.chord.root, to.chord.root);
			const Scale stepped{pitchClass(from.scale.root + step), from.scale.type};
			return transposePitch(pitch, {step, stepped, to.scale, false, 0, 127});
		}
		return clampPitch(nearestOf(moved, to.chord.root, toChord));
	}
}
