// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/midi/modifiers.h"

#include "engine/midi/midivalues.h"

#include <algorithm>
#include <cmath>

namespace gnubase::engine::midi
{
	namespace
	{
		constexpr std::array<const char*, 5> g_randomTargets = {"off", "position", "pitch", "velocity", "length"};
		constexpr std::array<const char*, 5> g_rangeTargets = {
			"off", "velocityLimit", "velocityFilter", "noteLimit", "noteFilter"};

		template <typename Target, size_t Count>
		std::optional<Target> fromString(const std::array<const char*, Count>& names, const std::string& name)
		{
			for (size_t i = 0; i < Count; ++i)
				if (name == names[i])
					return static_cast<Target>(i);
			return std::nullopt;
		}

		// SplitMix64's finalizer: spreads every bit of x over the result.
		uint64_t mix(uint64_t x) noexcept
		{
			x += 0x9E3779B97F4A7C15ULL;
			x = (x ^ (x >> 30U)) * 0xBF58476D1CE4E5B9ULL;
			x = (x ^ (x >> 27U)) * 0x94D049BB133111EBULL;
			return x ^ (x >> 31U);
		}

		bool changesVelocity(const Modifiers& modifiers) noexcept
		{
			return modifiers.velocityShift != 0 ||
				modifiers.velocityCompression[0] != modifiers.velocityCompression[1] ||
				std::any_of(modifiers.random.begin(), modifiers.random.end(),
					[](const RandomModifier& random) { return random.target == RandomTarget::Velocity; }) ||
				std::any_of(modifiers.range.begin(), modifiers.range.end(),
					[](const RangeModifier& range) { return range.target == RangeTarget::VelocityLimit; });
		}
	}

	const char* toString(RandomTarget target)
	{
		return g_randomTargets[static_cast<size_t>(target)];
	}

	const char* toString(RangeTarget target)
	{
		return g_rangeTargets[static_cast<size_t>(target)];
	}

	std::optional<RandomTarget> randomTargetFromString(const std::string& name)
	{
		return fromString<RandomTarget>(g_randomTargets, name);
	}

	std::optional<RangeTarget> rangeTargetFromString(const std::string& name)
	{
		return fromString<RangeTarget>(g_rangeTargets, name);
	}

	int64_t randomOffset(const Modifiers& modifiers, RandomTarget target, uint64_t seed) noexcept
	{
		if (modifiers.bypass)
			return 0;
		int64_t offset = 0;
		for (size_t i = 0; i < modifiers.random.size(); ++i)
		{
			const RandomModifier& random = modifiers.random[i];
			if (random.target != target || random.maximum < random.minimum)
				continue;
			const auto span = static_cast<uint64_t>(random.maximum - random.minimum) + 1;
			offset += random.minimum + static_cast<int64_t>(mix(seed + i) % span);
		}
		return offset;
	}

	uint64_t noteSeed(uint64_t part, uint64_t note) noexcept
	{
		return mix(mix(part) ^ note) * 2; // two generators: seed and seed + 1
	}

	bool modifyNote(const Modifiers& modifiers, uint64_t seed, uint8_t& pitch, uint16_t& velocity) noexcept
	{
		if (modifiers.bypass)
			return true;
		int key = pitch + modifiers.transpose + static_cast<int>(randomOffset(modifiers, RandomTarget::Pitch, seed));
		if (key < 0 || key > 127)
			return false;
		const bool changes = changesVelocity(modifiers);
		// ponytail: velocity modifiers work in MIDI 1.0 velocities; 16-bit steps when MIDI 2.0 instruments need them.
		int value = velocityToMidi1(velocity);
		if (changes)
		{
			const auto [numerator, denominator] = modifiers.velocityCompression;
			value = static_cast<int>(std::lround(static_cast<double>(value) * numerator / denominator)) +
				modifiers.velocityShift + static_cast<int>(randomOffset(modifiers, RandomTarget::Velocity, seed));
			value = std::clamp(value, 1, 127);
		}
		for (const RangeModifier& range : modifiers.range)
		{
			switch (range.target)
			{
			case RangeTarget::Off:
				break;
			case RangeTarget::VelocityLimit:
				value = std::clamp(value, std::max(range.minimum, 1), std::max(range.maximum, 1));
				break;
			case RangeTarget::VelocityFilter:
				if (value < range.minimum || value > range.maximum)
					return false;
				break;
			case RangeTarget::NoteLimit:
				while (key < range.minimum)
					key += 12;
				while (key > range.maximum)
					key -= 12;
				key = std::clamp(key, 0, 127); // a range narrower than an octave
				break;
			case RangeTarget::NoteFilter:
				if (key < range.minimum || key > range.maximum)
					return false;
				break;
			}
		}
		pitch = static_cast<uint8_t>(key);
		if (changes)
			velocity = velocityFromMidi1(static_cast<uint8_t>(value));
		return true;
	}

	int64_t modifyLength(const Modifiers& modifiers, uint64_t seed, int64_t length) noexcept
	{
		if (modifiers.bypass)
			return length;
		const auto [numerator, denominator] = modifiers.lengthCompression;
		const int64_t compressed = length * numerator / denominator;
		return std::max<int64_t>(1, compressed + randomOffset(modifiers, RandomTarget::Length, seed));
	}
}
