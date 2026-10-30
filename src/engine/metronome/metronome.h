// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/graph/processor.h"
#include "engine/metronome/clicks.h"
#include "engine/metronome/metronomesettings.h"
#include "platform/rt/snapshotpublisher.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace gnubase::engine::metronome
{
	// The longest sample file a click plays; longer files are cut.
	inline constexpr double g_maxSampleSeconds = 10.0;
	inline constexpr double g_beepSeconds = 0.05;
	inline constexpr double g_midiNoteSeconds = 0.05; // how long a MIDI click's note lasts at most

	using Sound = std::vector<float>; // mono, at the device's rate, with its level applied

	// What the metronome plays, compiled on the command thread from the project's settings, tempo and signatures and
	// the device's sample rate. Immutable once published.
	struct ClickConfig
	{
		bool clickWhilePlaying = false;
		double sampleRate = 48000.0; // the project's: transport positions are in its samples
		time::TempoMap tempo{120.0};
		time::SignatureMap signatures;
		PatternTable patterns;

		bool audio = false;
		std::array<std::shared_ptr<const Sound>, g_accents> sounds; // never null

		bool midi = false;
		uint8_t channel = 0; // 0-15
		std::array<uint8_t, g_accents> notes{};
		std::array<uint16_t, g_accents> velocities{};
		uint32_t noteLength = 1; // frames
	};

	// The click sounds, made on the command thread. Sample files are decoded once and kept while they don't change.
	class SoundLibrary
	{
	public:
		// The beep of an accent at a sample rate, gain applied.
		static std::shared_ptr<const Sound> beep(const Beep& beep, double gain, double sampleRate);

		// A sample file's sound, mixed to mono and converted to the sample rate, gain applied. Throws std::exception
		// if the file can't be read.
		std::shared_ptr<const Sound> sample(const std::string& path, double gain, double sampleRate);

		// Opens a file the way sample() does, without keeping it. Throws std::exception if it can't be read.
		static void check(const std::string& path);

		// What tells whether a file changed: its time and size, or nothing if it isn't there.
		struct Stamp
		{
			std::filesystem::file_time_type modified;
			uintmax_t size = 0;
			bool exists = false;

			bool operator==(const Stamp& other) const
			{
				return exists == other.exists && modified == other.modified && size == other.size;
			}
		};
		static Stamp stamp(const std::string& path);

	private:
		struct Decoded
		{
			Stamp stamp;
			std::shared_ptr<const Sound> sound; // at sampleRate, gain 1
		};
		std::map<std::pair<std::string, double>, Decoded> m_decoded; // by path and sample rate
	};

	// The settings compiled for the audio thread. Sample files that can't be read play the accent's beep.
	std::unique_ptr<const ClickConfig> compile(const MetronomeSettings& settings, const time::TempoMap& tempo,
		const time::SignatureMap& signatures, double projectRate, double deviceRate, SoundLibrary& sounds);

	// The metronome's node: on a mono output, the audio click, and as events, the MIDI click. It clicks while the
	// transport plays if the settings say so, and while the transport counts in. A click sounds on, or its note
	// lasts, until it ends or the next click of the same voice comes; up to g_voices audio clicks sound at once, and a
	// further one takes the voice of the one that started first. The output is the same for any block size.
	class Metronome : public graph::Processor
	{
	public:
		static constexpr size_t g_voices = 8;

		Metronome();

		// Command thread: what the metronome plays from the next block on.
		void setConfig(std::unique_ptr<const ClickConfig> config);
		const ClickConfig& config() const { return m_config.latest(); }

		void process(const graph::ProcessContext& context) noexcept override;

	private:
		struct Voice
		{
			int accent = 0;    // 1-4, 0: free
			int64_t start = 0; // the frame it started at, counted like m_frame
		};

		static size_t length(const ClickConfig& config, int accent) noexcept; // of the accent's sound

		// At frame offset of the block: renders the voices up to there, then starts a click.
		void click(
			const ClickConfig& config, const graph::ProcessContext& context, uint32_t offset, int accent) noexcept;
		void render(const ClickConfig& config, const graph::ProcessContext& context, uint32_t until) noexcept;
		void endNote(graph::EventBuffer& out, uint32_t offset) noexcept;

		rt::SnapshotPublisher<ClickConfig> m_config;
		int64_t m_frame = 0;     // frames processed before this block
		uint32_t m_rendered = 0; // frames of this block the voices are rendered to
		std::array<Voice, g_voices> m_voices{};
		bool m_noteOn = false;
		uint8_t m_noteChannel = 0;
		uint8_t m_noteKey = 0;
		int64_t m_noteEnd = 0; // the frame the note ends at, counted like m_frame
	};
}
