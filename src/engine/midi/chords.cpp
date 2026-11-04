// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/midi/chords.h"

#include <algorithm>
#include <set>

namespace gnubase::engine::midi
{
	namespace
	{
		constexpr std::array<const char*, 12> g_names = {
			"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

		int pitchClass(int pitch)
		{
			return ((pitch % 12) + 12) % 12;
		}

		std::set<int> classesOf(const std::vector<int>& pitches)
		{
			std::set<int> classes;
			for (const int pitch : pitches)
				classes.insert(pitchClass(pitch));
			return classes;
		}

		std::set<int> classesOf(int root, const ChordType& type)
		{
			std::set<int> classes;
			for (const int interval : type.intervals)
				classes.insert(pitchClass(root + interval));
			return classes;
		}
	}

	const std::vector<ChordType>& chordTypes()
	{
		static const std::vector<ChordType> types = {
			{"major", "", {0, 4, 7}},
			{"minor", "m", {0, 3, 7}},
			{"diminished", "dim", {0, 3, 6}},
			{"augmented", "aug", {0, 4, 8}},
			{"sus2", "sus2", {0, 2, 7}},
			{"sus4", "sus4", {0, 5, 7}},
			{"major7", "maj7", {0, 4, 7, 11}},
			{"minor7", "m7", {0, 3, 7, 10}},
			{"dominant7", "7", {0, 4, 7, 10}},
			{"halfDiminished7", "m7b5", {0, 3, 6, 10}},
			{"diminished7", "dim7", {0, 3, 6, 9}},
			{"minorMajor7", "mMaj7", {0, 3, 7, 11}},
		};
		return types;
	}

	const ChordType* chordType(const std::string& name)
	{
		for (const ChordType& type : chordTypes())
			if (name == type.name)
				return &type;
		return nullptr;
	}

	std::string Chord::name() const
	{
		return std::string(g_names.at(static_cast<size_t>(pitchClass(root)))) + (type != nullptr ? type->suffix : "");
	}

	std::optional<Chord> recognizeChord(const std::vector<int>& pitches)
	{
		if (pitches.empty())
			return std::nullopt;
		const std::set<int> classes = classesOf(pitches);
		// The lowest note's class first, then the others upwards.
		const int bass = pitchClass(*std::min_element(pitches.begin(), pitches.end()));
		std::vector<int> roots = {bass};
		for (const int each : classes)
			if (each != bass)
				roots.push_back(each);
		for (const int root : roots)
			for (const ChordType& type : chordTypes())
				if (classesOf(root, type) == classes)
					return Chord{root, &type};
		return std::nullopt;
	}

	std::vector<int> fitChord(const std::vector<int>& pitches, const ChordType& type)
	{
		if (pitches.empty())
			return {};
		std::vector<size_t> order(pitches.size());
		for (size_t i = 0; i < order.size(); ++i)
			order[i] = i;
		std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return pitches[a] < pitches[b]; });
		const std::optional<Chord> current = recognizeChord(pitches);
		const int root = current ? current->root : pitchClass(pitches[order.front()]);
		const std::set<int> tones = classesOf(root, type);
		std::set<int> taken;
		std::vector<int> fitted = pitches;
		for (const size_t index : order)
		{
			const int pitch = pitches[index];
			int best = -1;
			for (int distance = 0; distance <= 6 && best < 0; ++distance)
				for (const int candidate : {pitch - distance, pitch + distance}) // down first on a tie
				{
					const int pitchClassOf = pitchClass(candidate);
					const bool free = taken.count(pitchClassOf) == 0;
					if (candidate >= 0 && candidate <= 127 && tones.count(pitchClassOf) != 0 && free)
					{
						best = candidate;
						break;
					}
				}
			if (best < 0) // every free tone is out of range here: any tone
				for (int distance = 0; distance <= 6 && best < 0; ++distance)
					for (const int candidate : {pitch - distance, pitch + distance})
						if (candidate >= 0 && candidate <= 127 && tones.count(pitchClass(candidate)) != 0)
						{
							best = candidate;
							break;
						}
			taken.insert(pitchClass(best));
			if (taken.size() == tones.size()) // every tone has a note: the next ones start again
				taken.clear();
			fitted[index] = best;
		}
		return fitted;
	}

	std::optional<Voicing> voicingFromName(const std::string& name)
	{
		if (name == "invertUp")
			return Voicing::InvertUp;
		if (name == "invertDown")
			return Voicing::InvertDown;
		if (name == "drop2")
			return Voicing::Drop2;
		if (name == "drop3")
			return Voicing::Drop3;
		if (name == "drop24")
			return Voicing::Drop24;
		return std::nullopt;
	}

	std::optional<std::vector<int>> revoice(const std::vector<int>& pitches, Voicing voicing)
	{
		if (pitches.size() < 2)
			return std::nullopt;
		std::vector<size_t> order(pitches.size()); // highest first
		for (size_t i = 0; i < order.size(); ++i)
			order[i] = i;
		std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return pitches[a] > pitches[b]; });
		std::vector<int> result = pitches;
		const int highest = pitches[order.front()];
		const int lowest = pitches[order.back()];
		const auto drop = [&](size_t rank) -> bool
		{
			if (rank >= order.size())
				return false;
			result[order[rank]] -= 12;
			return true;
		};
		switch (voicing)
		{
		case Voicing::InvertUp:
			while (result[order.back()] <= highest)
				result[order.back()] += 12;
			break;
		case Voicing::InvertDown:
			while (result[order.front()] >= lowest)
				result[order.front()] -= 12;
			break;
		case Voicing::Drop2:
			if (!drop(1))
				return std::nullopt;
			break;
		case Voicing::Drop3:
			if (!drop(2))
				return std::nullopt;
			break;
		case Voicing::Drop24:
			if (!drop(3) || !drop(1))
				return std::nullopt;
			break;
		}
		for (const int pitch : result)
			if (pitch < 0 || pitch > 127)
				return std::nullopt;
		return result;
	}

	std::string pitchName(int pitch)
	{
		return std::string(g_names.at(static_cast<size_t>(pitchClass(pitch)))) + std::to_string((pitch / 12) - 2);
	}
}
