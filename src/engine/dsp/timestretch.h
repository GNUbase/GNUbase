// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/audiofile/audiofile.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Time-stretching and pitch-shifting (#142), a thin wrapper around Rubber Band Library: docs/time-stretch.md explains
// the choice, the presets and how audio tracks will use it. Nothing outside this wrapper includes Rubber Band.
namespace gnubase::engine::dsp
{
	enum class StretchMode : uint8_t
	{
		// Streams: input in blocks, ratio and pitch may change between blocks, output after a latency. The process path
		// is real-time safe.
		RealTime,
		// Studies the whole input first, then stretches it: the best quality and an exact output length, no latency.
		// Ratio and pitch are fixed. Never on audio threads.
		Offline,
	};

	// Algorithm presets. The first two use the high-quality engine, the others the CPU-efficient one tuned for kinds of
	// material.
	enum class StretchPreset : uint8_t
	{
		Quality,   // the best result for most material: mixes, vocals, bass, soft onsets; the most CPU
		Efficient, // the high-quality engine with one short window: a fraction of the CPU and latency
		Drums,     // percussive material: short window, phase resets at every hit, timing kept
		Plucked,   // transients over a stable sound, like plucked strings or piano
		Pads,      // pitched, slow material: smooth, no phase resets, timing less exact
		Vocals,    // slower material with soft transients and a tonal character
		Mix,       // mixed material: resets outside the musical range, stereo kept together
		Solo,      // monophonic instruments and voices
	};

	// The names commands use: "realtime", "offline"; "quality", "efficient", "drums" and so on.
	std::string_view toString(StretchMode mode);
	std::string_view toString(StretchPreset preset);
	std::optional<StretchMode> stretchModeFromString(std::string_view name);
	std::optional<StretchPreset> stretchPresetFromString(std::string_view name);
	std::vector<std::string> stretchModeNames();
	std::vector<std::string> stretchPresetNames();

	struct StretchSettings
	{
		int channels = 2;
		double sampleRate = 48000.0;
		StretchMode mode = StretchMode::Offline;
		StretchPreset preset = StretchPreset::Quality;
		double ratio = 1.0;            // output length / input length: 2 is twice as long, half the tempo
		double semitones = 0.0;        // pitch shift; cents add to it
		double cents = 0.0;            //
		bool preserveFormants = false; // keeps the spectral envelope when shifting the pitch, e.g. for voices
		size_t maxBlockFrames = 4096;  // the most input frames one process call gets
	};

	// The frequency factor of a pitch shift: 2^((semitones + cents / 100) / 12).
	double pitchScale(double semitones, double cents);

	// Limits checked by the constructor; the setters clamp to them. Real-time mode has a narrower range, because it
	// allocates its buffers for the whole range up front.
	inline constexpr double g_minStretchRatio = 0.1;
	inline constexpr double g_maxStretchRatio = 10.0;
	inline constexpr double g_maxPitchSemitones = 48.0; // either way
	inline constexpr double g_minRealtimeRatio = 0.25;
	inline constexpr double g_maxRealtimeRatio = 4.0;
	inline constexpr double g_maxRealtimePitchSemitones = 24.0;

	// One stretcher for a fixed number of channels. Construction allocates everything, so it happens off the audio
	// thread; in real-time mode the setters, process, available and retrieve then neither allocate, lock nor block.
	// The same input and settings always give the same output.
	class TimeStretcher
	{
	public:
		// Throws std::invalid_argument for settings out of range.
		explicit TimeStretcher(const StretchSettings& settings);
		~TimeStretcher();
		TimeStretcher(const TimeStretcher&) = delete;
		TimeStretcher& operator=(const TimeStretcher&) = delete;

		const StretchSettings& settings() const { return m_settings; }

		// In real-time mode these may change between process calls, clamped to the real-time limits. In offline mode
		// only before the first study or process call.
		void setRatio(double ratio) noexcept;
		void setPitch(double semitones, double cents) noexcept;
		void setPreserveFormants(bool preserve) noexcept;

		// Real-time latency. Feed startPad() frames before the first real input frame (silence, or the audio before
		// it) and drop the first latency() output frames: the output is then aligned with the input. Both depend on
		// ratio and pitch, so read them after setting those. Zero in offline mode, which compensates internally.
		size_t startPad() const noexcept;
		size_t latency() const noexcept;

		// Offline mode only: the number of input frames, for an exact output length; then every input frame passes
		// through study, final on the last call, before the first process call.
		void setInputLength(size_t frames);
		void study(const float* const* input, size_t frames, bool final);

		// Input frames needed for more output: what to pass to the next process call, at most maxBlockFrames.
		size_t inputRequired() const noexcept;
		// Takes frames (at most maxBlockFrames) input frames, one pointer per channel; final on the last call.
		void process(const float* const* input, size_t frames, bool final) noexcept;
		// Output frames ready to retrieve.
		size_t available() const noexcept;
		// True once the final input has been processed and all output retrieved.
		bool finished() const noexcept;
		// Copies up to frames output frames into one buffer per channel; returns how many.
		size_t retrieve(float* const* output, size_t frames) noexcept;

		// Starts over as after construction, keeping the settings. Not real-time safe.
		void reset();

	private:
		struct Impl;
		StretchSettings m_settings;
		std::unique_ptr<Impl> m_impl;
	};

	// The length of inputFrames stretched by ratio.
	int64_t stretchedLength(int64_t inputFrames, double ratio);

	// Stretches whole channels (equal lengths) and returns stretchedLength frames per channel, aligned with the
	// input: offline mode compensates internally, real-time mode is padded and trimmed by its latency. Not real-time.
	std::vector<std::vector<float>> stretch(const std::vector<std::vector<float>>& input, StretchSettings settings);

	// Stretches the audio file at input into a 32-bit float WAV file at output and returns what was written; channels
	// and sample rate come from the file. Throws AudioFileError, or std::invalid_argument for invalid settings.
	AudioFileInfo stretchFile(const std::string& input, const std::string& output, StretchSettings settings);
}
