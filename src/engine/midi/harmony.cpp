// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/midi/harmony.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <functional>
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
			// A sus after the seventh, as in C7sus4, the common spelling of Csus47.
			if (chord.type == "maj" && startsWith(text, at, "sus"))
			{
				chord.type = startsWith(text, at, "sus2") ? "sus2" : "sus4";
				at += startsWith(text, at, "sus2") || startsWith(text, at, "sus4") ? 4 : 3;
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
				// A sus chord's seventh comes before the sus: C7sus4.
				const bool sus = chord.type == "sus2" || chord.type == "sus4";
				name.insert(sus ? name.size() - 4 : name.size(), first);
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
		// The scale by the chord's family, then, if that doesn't hold all of its notes, the family's scale that holds
		// the most of them, the first of those.
		const bool dominant = with("7") && chord.type != "min" && chord.type != "dim";
		std::vector<ScaleType> candidates;
		if (dominant)
			candidates = {ScaleType::Mixolydian, ScaleType::LydianDominant, ScaleType::MixolydianFlat6,
				ScaleType::PhrygianDominant, ScaleType::HalfWholeDiminished, ScaleType::Altered};
		else if (chord.type == "min")
			candidates = {ScaleType::Dorian, ScaleType::Minor, ScaleType::Phrygian, ScaleType::MelodicMinor,
				ScaleType::HarmonicMinor};
		else if (chord.type == "dim")
			candidates = {ScaleType::Locrian, ScaleType::WholeHalfDiminished, ScaleType::HalfWholeDiminished};
		else if (chord.type == "aug")
			candidates = {ScaleType::WholeTone, ScaleType::Altered};
		else
			candidates = {ScaleType::Major, ScaleType::Lydian, ScaleType::Mixolydian, ScaleType::LydianDominant,
				ScaleType::HarmonicMinor};
		const std::vector<int> intervals = chordIntervals(chord);
		const auto held = [&intervals](ScaleType candidate)
		{
			const std::vector<int> steps = scaleSteps(candidate);
			return std::count_if(intervals.begin(), intervals.end(),
				[&steps](int interval) { return std::find(steps.begin(), steps.end(), interval) != steps.end(); });
		};
		ScaleType type = ScaleType::Major;
		if (chord.type == "min" && with("maj7"))
			type = ScaleType::MelodicMinor;
		else if (chord.type == "min" && (with("6") || with("7") || with("13")))
			type = ScaleType::Dorian;
		else if (chord.type == "min")
			type = ScaleType::Minor;
		else if (chord.type == "dim" && with("6"))
			type = ScaleType::WholeHalfDiminished;
		else if (chord.type == "dim")
			type = ScaleType::Locrian;
		else if (chord.type == "aug")
			type = ScaleType::WholeTone;
		else if (with("#11") && chord.type == "maj")
			type = ScaleType::Lydian;
		else if (with("7"))
			type = ScaleType::Mixolydian;
		if (held(type) < static_cast<std::ptrdiff_t>(intervals.size()))
			for (const ScaleType candidate : candidates)
				if (held(candidate) > held(type))
					type = candidate;
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

	const std::vector<std::string>& voicingLibraries()
	{
		static const std::vector<std::string> names = {"basic", "piano", "guitar"};
		return names;
	}

	const std::vector<std::string>& voicingStyles(const std::string& library)
	{
		static const std::vector<std::string> none;
		static const std::vector<std::string> piano = {
			"triads", "triadsMaj9", "triadsMaj9Min9", "fourNote", "fourNoteOpen", "fiveNote"};
		static const std::vector<std::string> guitar = {"triads", "fourNote", "threeString", "modernJazz"};
		if (library == "piano")
			return piano;
		return library == "guitar" ? guitar : none;
	}

	namespace
	{
		// The intervals from the root of a piano style's upper notes.
		std::vector<int> pianoNotes(const ChordSymbol& chord, const std::string& style)
		{
			std::vector<int> notes = chordIntervals(chord);
			const auto add = [&notes](int interval)
			{
				if (std::find(notes.begin(), notes.end(), interval) == notes.end())
					notes.push_back(interval);
			};
			const auto without = [&notes](int interval)
			{ notes.erase(std::remove(notes.begin(), notes.end(), interval), notes.end()); };
			const size_t size = notes.size();
			if ((style == "triadsMaj9" || style == "triadsMaj9Min9") && size == 3)
			{
				add(style == "triadsMaj9Min9" && chord.type == "dim" ? 1 : 2);
				without(0);
			}
			else if (style == "fourNote" && size >= 3)
			{
				without(0);
				for (const int extra : {2, 7, 9})
					if (notes.size() < 4)
						add(extra);
				notes.resize(std::min<size_t>(notes.size(), 4));
			}
			else if (style == "fourNoteOpen" && size >= 3)
			{
				without(0);
				without(7);
				for (const int extra : {2, 9, 7})
					if (notes.size() < 4)
						add(extra);
				notes.resize(std::min<size_t>(notes.size(), 4));
			}
			else if (style == "fiveNote" && size >= 4)
			{
				add(2);
				notes.resize(std::min<size_t>(notes.size(), 5));
			}
			return notes;
		}

		int voicingDistance(const std::vector<int>& from, const std::vector<int>& to)
		{
			int sum = 0;
			for (const int pitch : from)
			{
				int nearest = 128;
				for (const int other : to)
					nearest = std::min(nearest, std::abs(pitch - other));
				sum += nearest;
			}
			return sum;
		}

		// The inversion of upper (close position, from the lowest up) within [low, high] nearest to previous, or
		// without previous the one nearest to upper itself; upper if none fits.
		std::vector<int> nearestInversion(
			const std::vector<int>& upper, const std::vector<int>& previous, int low, int high)
		{
			std::vector<int> best = upper;
			int bestCost = std::numeric_limits<int>::max();
			for (const int shift : {-24, -12, 0, 12})
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
					if (candidate.front() < low || candidate.back() > high)
						continue;
					const int cost = previous.empty()
						? std::abs(candidate.front() - upper.front())
						: voicingDistance(candidate, previous) + voicingDistance(previous, candidate);
					if (cost < bestCost)
					{
						bestCost = cost;
						best = candidate;
					}
				}
			}
			return best;
		}

		std::vector<int> pianoVoicing(const ChordSymbol& chord, const VoicingSettings& settings,
			const std::vector<int>& previous, const VoicingStyle& style)
		{
			const int offset = 12 * settings.octaveOffset;
			std::vector<int> upper;
			for (const int interval : pianoNotes(chord, style.style))
				upper.push_back(60 + offset + pitchClass(chord.root + interval));
			std::sort(upper.begin(), upper.end());
			std::vector<int> before;
			if (settings.adaptive)
				for (const int pitch : previous)
					if (pitch >= style.lowest + offset)
						before.push_back(pitch);
			upper = nearestInversion(upper, before, style.lowest + offset, style.highest + offset);
			std::vector<int> voicing;
			const int bass = pitchClass(chord.bass >= 0 ? chord.bass : chord.root);
			const int root = style.lowestRoot + offset + pitchClass(bass - style.lowestRoot);
			if (style.addRoot)
				voicing.push_back(root);
			if (style.addRoot && style.duplicateRoot)
				voicing.push_back(root + 12);
			if (style.fattenUp && !upper.empty())
				upper.push_back(upper.front() + 12);
			voicing.insert(voicing.end(), upper.begin(), upper.end());
			std::sort(voicing.begin(), voicing.end());
			voicing.erase(std::unique(voicing.begin(), voicing.end()), voicing.end());
			return voicing;
		}

		// A guitar in standard tuning, from the low E.
		constexpr std::array<int, 6> g_strings = {40, 45, 50, 55, 59, 64};

		std::vector<int> guitarVoicing(const ChordSymbol& chord, const VoicingSettings& settings,
			const std::vector<int>& previous, const VoicingStyle& style)
		{
			const std::vector<std::string>& styles = voicingStyles("guitar");
			const std::string name =
				std::find(styles.begin(), styles.end(), style.style) == styles.end() ? styles.front() : style.style;
			const std::vector<int> intervals = chordIntervals(chord);
			std::set<int> required; // pitch classes
			std::set<int> allowed;
			const auto add = [&chord](std::set<int>& to, int interval)
			{ to.insert(pitchClass(chord.root + interval)); };
			const bool jazz = name == "modernJazz";
			const bool three = name == "threeString";
			if (jazz)
			{
				for (const int interval : intervals)
				{
					add(allowed, interval);
					if (interval != 0 && interval != 7)
						add(required, interval);
				}
			}
			else
			{
				// The triad, with fourNote its sixth or seventh.
				std::vector<int> notes(intervals.begin(), intervals.begin() + std::min<size_t>(3, intervals.size()));
				if (name == "fourNote")
					for (const int interval : intervals)
						if ((interval == 9 || interval == 10 || interval == 11) && notes.size() < 4 &&
							std::find(notes.begin(), notes.end(), interval) == notes.end())
							notes.push_back(interval);
				for (const int interval : notes)
				{
					add(allowed, interval);
					add(required, interval);
				}
			}
			const int bass = pitchClass(chord.bass >= 0 ? chord.bass : chord.root);
			if (!jazz && !three)
			{
				allowed.insert(bass);
				required.insert(bass);
			}
			const size_t fewest = three ? 3 : 4;
			const bool adaptive = settings.adaptive && !previous.empty();

			std::vector<int> best;
			std::tuple<int, int, int> bestScore{std::numeric_limits<int>::max(), 0, 0};
			std::vector<int> pitches;
			for (int window = 0; window <= 9; ++window)
				for (size_t low = 0; low + fewest <= g_strings.size(); ++low)
				{
					const size_t high = three ? low + 3 : g_strings.size();
					// Every string from low to high plays an allowed class at a fret of the window, or open.
					const std::function<void(size_t)> search = [&](size_t string)
					{
						if (string == high)
						{
							std::set<int> classes;
							for (const int pitch : pitches)
								classes.insert(pitchClass(pitch));
							if (!std::includes(classes.begin(), classes.end(), required.begin(), required.end()) ||
								(!jazz && !three && pitchClass(pitches.front()) != bass))
								return;
							std::vector<int> sorted = pitches;
							std::sort(sorted.begin(), sorted.end());
							const std::tuple<int, int, int> score{adaptive
									? voicingDistance(sorted, previous) + voicingDistance(previous, sorted)
									: window,
								-static_cast<int>(sorted.size()), window};
							if (score < bestScore)
							{
								bestScore = score;
								best = sorted;
							}
							return;
						}
						for (int fret = 0; fret <= window + 3; ++fret)
						{
							const int pitch = g_strings.at(string) + fret;
							if ((fret != 0 && fret < std::max(window, 1)) || allowed.count(pitchClass(pitch)) == 0)
								continue;
							pitches.push_back(pitch);
							search(string + 1);
							pitches.pop_back();
						}
					};
					search(low);
				}
			for (int& pitch : best)
				pitch += 12 * settings.octaveOffset;
			return best;
		}
	}

	std::vector<int> voiceChord(const ChordSymbol& chord, const VoicingSettings& settings,
		const std::vector<int>& previous, const VoicingStyle& style)
	{
		if (chordIntervals(chord).empty())
			return {};
		std::vector<int> voicing;
		if (style.library == "piano")
			voicing = pianoVoicing(chord, settings, previous, style);
		else if (style.library == "guitar")
			voicing = guitarVoicing(chord, settings, previous, style);
		else
			return voiceChord(chord, settings, previous);
		for (int& pitch : voicing)
			pitch = clampPitch(pitch);
		return voicing;
	}

	const std::vector<std::string>& followModeNames()
	{
		static const std::vector<std::string> names = {
			"off", "chordsAndScales", "chords", "scales", "rootNotes", "voicings", "singleVoice"};
		return names;
	}

	const std::vector<std::string>& voiceNames()
	{
		static const std::vector<std::string> names = {"soprano", "alto", "tenor", "bass"};
		return names;
	}

	int followPitch(int pitch, const std::string& mode, const Harmony& from, const Harmony& to, int single)
	{
		if (mode == "off" || from.chord.root < 0 || to.chord.root < 0)
			return pitch;
		if (mode == "singleVoice" && !to.voicing.empty())
		{
			const int count = static_cast<int>(to.voicing.size());
			const int index = single >= 3 ? 0 : std::max(0, count - 1 - single);
			return to.voicing[static_cast<size_t>(index)];
		}
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
