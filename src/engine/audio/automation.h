// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/audio/meters.h"
#include "engine/audio/stripsections.h"
#include "engine/document/rendersnapshot.h"
#include "engine/graph/graph.h"
#include "engine/graph/processor.h"
#include "platform/rt/snapshotpublisher.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// Automation playback (#120): the audio thread turns a channel's automation curves into parameter events for each
// block, at their sample offsets, and the channel's fader and source processor play them.
namespace gnubase::engine
{
	// Ramps are played in steps of this many frames, counted from the point where the ramp starts: the fader glides
	// along curves in straight pieces of this length (and along straight ramps exactly), instrument parameters get
	// their value at each of these frames, and MIDI controllers get theirs where their MIDI 1.0 value changes.
	inline constexpr uint32_t g_automationInterval = 64;

	// From 0 to 1 as x goes from 0 to 1, bent by curve (-1 to 1): how a curve segment of automation ramps (see
	// AutomationShape::Curve). (1 - e^(-kx)) / (1 - e^(-k)) with k = 6 * curve.
	double automationBend(double x, double curve);

	// How long the fader glides to a new static value, e.g. when the volume is set during playback, so it doesn't
	// click: 5 ms.
	inline constexpr double g_faderSmoothing = 0.005;

	// One parameter's automation as the audio thread plays it.
	struct PlayedLane
	{
		enum class Kind : uint8_t
		{
			Fader,      // parameter events for the channel's fader, which glides along ramps
			Instrument, // parameter events for the channel's instrument, which jumps to each value
			Plugin,     // parameter events for the plugin in one of the channel's slots, which jumps to each value
			Midi,       // MIDI controller events for the channel's source
		};

		AutomationTarget target;
		Kind kind = Kind::Fader;
		uint32_t parameter = 0;   // Fader, Instrument and Plugin: the parameter id
		double staticValue = 0.0; // the value without points
		double minimum = 0.0;     // values are kept in this range
		double maximum = 1.0;
		std::vector<RenderAutomationPoint> points; // sorted by position; none: the static value
		// A fader lane whose static value changed while it has no points: it glides there over g_faderSmoothing.
		// Not compared: it only says how the new value arrives.
		bool smooth = false;

		bool operator==(const PlayedLane& other) const;
	};

	// What a channel's automation plays, compiled by the command thread: a lane for the fader's volume, pan and mute
	// (whose static value is off: a channel's mute button makes it silent, see ChannelStrip), for each of the
	// instrument's parameters and for each parameter of the built-in plugins in its slots, with or without points, so
	// that they return to their static values when their automation goes; and a lane for each MIDI controller with
	// points.
	struct ChannelAutomation
	{
		uint64_t revision = 0;  // set by AutomationPlayer::setAutomation
		std::string instrument; // a new instrument gets every value again
		std::vector<PlayedLane> lanes;

		bool sameContent(const ChannelAutomation& other) const
		{
			return instrument == other.instrument && lanes == other.lanes;
		}
	};

	// What a channel's automation plays: the fader's, the instrument's and the slots' built-in plugins' parameters
	// with their static values or curves, a MIDI channel's volume and pan as controllers 7 and 10 on MIDI channel 1
	// when it has them, and the MIDI controllers that have curves. maxVolume is the project's, in dB.
	std::unique_ptr<ChannelAutomation> channelAutomation(const RenderChannel& channel, double maxVolume);

	// The value of a curve at a project position (see AutomationLane): before the first point its value, after the
	// last one its value, and where several points share a position the last one's.
	double automationValue(const std::vector<RenderAutomationPoint>& points, int64_t position);

	// Turns a channel's automation into events, block by block. While stopped, every parameter takes its value at
	// the cursor; while playing, each segment of the transport block plays its part of the curves: steps at their
	// sample, ramps as gliding fader events or as the value every g_automationInterval frames. A start, a locate, a
	// cycle jump, new automation and a new source processor set every value at the frame where they happen, so
	// playback is right from any position on.
	class AutomationPlayer
	{
	public:
		AutomationPlayer();

		// Command thread: what to play from the next block on. Gives it the next revision.
		void setAutomation(std::unique_ptr<ChannelAutomation> automation);

		// Command thread: the automation last set.
		const ChannelAutomation& latest() const { return m_automation.latest(); }

		// Audio thread: adds the block's events for the fader to fader, those for the plugins in the slots to plugins
		// with the slot's index in slots as their key, and the others to source. processors are the source processor
		// and the slots' plugins, top to bottom (only compared, to send every value again to new ones), and slots the
		// slots' ids in the same order: those of the program that plays, which may be older or newer than the
		// automation, so plugin lanes whose slot isn't in it are left out. New automation's smooth lanes glide to their
		// values over smoothing frames, and mute glides over them from the sample where it changes, so it doesn't
		// click.
		void play(const graph::TransportBlock& transport, const std::vector<const void*>& processors,
			const std::vector<ObjectId>& slots, graph::EventBuffer& source, graph::EventBuffer& fader,
			graph::EventBuffer& plugins, uint32_t smoothing = 0) noexcept;

	private:
		rt::SnapshotPublisher<ChannelAutomation> m_automation;
		uint64_t m_revision = 0; // command thread: the last revision given

		// Audio thread.
		uint64_t m_played = 0; // the revision played last
		// The processors played for last: the source and the slots' plugins.
		std::array<const void*, g_maxSlots + 1> m_processors{};
		size_t m_processorCount = 0;
		int64_t m_next = -1;      // where playback goes on without a jump; -1 while stopped
		int64_t m_stoppedAt = -1; // the cursor while stopped
	};

	// How a channel's fader pans, fixed for one graph: the panner its routing gives it (pannerFor), a stereo
	// channel's panner, pan bypass and the project's pan law.
	struct Panning
	{
		Panner panner = Panner::None;
		StereoPanner stereo = StereoPanner::Balance;
		bool bypass = false;
		PanLaw law = PanLaw::Minus3;
	};

	// A channel's fader: a linear gain, then its panner. A mono channel on a stereo destination is placed with the
	// pan law and becomes stereo; a stereo channel has a balance as in the built-in gainPan (the side panned away from
	// is lowered, the center is unity gain) or the combined panner, which places its left and its right channel each
	// with the pan law. Bypassed, mono plays at the center and stereo hard left and right. Every value glides along
	// ramps sample by sample, so automation ramps have no steps.
	class ChannelFader
	{
	public:
		enum Parameter : uint8_t
		{
			Volume,   // a linear gain, 0 to 4
			Pan,      // -1 left, 0 center, 1 right
			PanLeft,  // the combined panner's position of the left channel
			PanRight, // and of the right one
			Mute,     // 1 mutes, 0 doesn't
		};

		ChannelFader();

		// Audio thread. The context has the channel's own channels; a mono panner writes the second one of its
		// stereo output after them.
		void process(const graph::ProcessContext& context, const Panning& panning) noexcept;

	private:
		struct Ramp
		{
			double value = 0.0; // the value of the next frame
			double start = 0.0;
			double target = 0.0;
			uint32_t length = 0; // frames of the ramp, 0 while it doesn't move
			uint32_t done = 0;

			void set(double newValue, uint32_t ramp) noexcept;
			double next() noexcept; // the value for this frame; moves on by one frame
		};

		void render(
			const graph::ProcessContext& context, const Panning& panning, uint32_t begin, uint32_t end) noexcept;

		Ramp m_volume;
		Ramp m_pan;
		Ramp m_panLeft;
		Ramp m_panRight;
		Ramp m_audible; // 1 - mute
	};

	// What a channel keeps across graph rebuilds: its automation, its strip's sections and its fader.
	struct ChannelState
	{
		AutomationPlayer automation;
		StripSections sections;
		std::shared_ptr<ChannelFader> fader = std::make_shared<ChannelFader>();
		float monitorGain = -1.0f; // audio thread: the input's gain after the last block, -1 before the first
		float outputGain = -1.0f;  // and the output's, 0 while the channel is silent (mute, solo)
		// Audio thread: the fader's automation events of the block, from a channel's source node to its fader node
		// when plugin slots are between them; the source node always runs first.
		graph::EventBuffer faderEvents{graph::g_maxEventsPerBlock};
		// Audio thread: the automation events of the block for the plugins in the slots, from the source node to the
		// slot nodes (SlotStrip), keyed by the slot's index in the program that plays.
		graph::EventBuffer pluginEvents{graph::g_maxEventsPerBlock};
		LevelMeter meter; // the channel's output after the fader, the panner and muting
		// Audio thread: each send's gain after the last block (SendStrip), -1 before the first.
		std::array<float, g_maxSends> sendGains = []
		{
			std::array<float, g_maxSends> gains{};
			gains.fill(-1.0f);
			return gains;
		}();
	};

	// A channel's audio before its fader, after the strip's sections, for its pre-fader sends: the channel's strip
	// writes it in every block, and the send nodes, which come after the channel's node, read it. Made for one graph.
	class PreFaderTap
	{
	public:
		explicit PreFaderTap(int channels) : m_channels(channels) {}

		// Command thread: room for blocks of up to maxFrames.
		void prepare(uint32_t maxFrames);

		int channels() const { return m_channels; }
		// Audio thread.
		float* channel(int c) noexcept { return m_samples.data() + (static_cast<size_t>(c) * m_maxFrames); }

	private:
		int m_channels = 0;
		uint32_t m_maxFrames = 0;
		std::vector<float> m_samples;
	};

	// Which part of a channel a strip runs: all of it, or, on a channel with plugin slots, what comes before them in
	// its source node or the fader after them in its channel node (graph::describe).
	enum class StripPart : uint8_t
	{
		Whole,
		Source,
		Fader,
	};

	// The user setting audio.autoMonitoring: when record-enabled tracks monitor their input.
	enum class AutoMonitoring : uint8_t
	{
		Manual,             // never: only the monitor button monitors
		WhileRecordEnabled, // always
		WhileRecording,     // while the transport records
		TapeMachine,        // while it stands or records, not while it only plays
	};

	// An audio track's input monitoring (docs/features/latency-and-monitoring.md).
	struct Monitoring
	{
		bool monitor = false; // the monitor button
		bool recordEnable = false;
		AutoMonitoring mode = AutoMonitoring::TapeMachine;

		// Whether the track hears its input while the transport plays or stands and records or not.
		bool active(bool playing, bool recording) const noexcept;
	};

	// The processor of a channel node: the channel's source (a track player, a sequencer or an instrument, or
	// nothing for a group or effect channel) with the automation events added to its input, the strip's sections
	// (the pre section after the source, the others before the fader), then the fader and its panner. A new strip is
	// made for every graph, around the source and the state that the channel keeps. With monitoring, the input reaches
	// the source only while the track monitors it, gliding in and out over a block. A silent strip (mute, solo, a
	// disabled track) glides its output to silence over a block, and back.
	class ChannelStrip : public graph::Processor
	{
	public:
		// slots: the processors of the channel's slot nodes, top to bottom, none while a plugin isn't ready; only
		// compared, so that new plugins get every automation value. slotIds: the slots' ids in the same order, those
		// of this strip's program, whose SlotStrips have their index in it.
		ChannelStrip(std::shared_ptr<ChannelState> state, std::shared_ptr<graph::Processor> source,
			Panning panning = {}, std::optional<Monitoring> monitoring = std::nullopt, bool silent = false,
			StripPart part = StripPart::Whole, const ChannelSections& sections = {},
			std::vector<const void*> slots = {}, std::vector<ObjectId> slotIds = {});

		void process(const graph::ProcessContext& context) noexcept override;
		uint32_t latency() const override;

		// Command thread, before the graph compiles: where the strip leaves its audio before the fader in every block.
		void tapPreFader(std::shared_ptr<PreFaderTap> tap) { m_tap = std::move(tap); }

		// Scales the block's first channels from gain to target, frame by frame, and keeps target as the gain; a gain
		// below 0 (before the first block) starts at the target.
		static void glide(const graph::ProcessContext& context, int channels, float& gain, float target) noexcept;

	protected:
		void prepare(double sampleRate, uint32_t maxFrames) override;

	private:
		// The fader and its panner with the block's fader events, then muting.
		void fade(const graph::ProcessContext& context, const graph::EventBuffer& events) noexcept;

		std::shared_ptr<ChannelState> m_state;
		std::shared_ptr<graph::Processor> m_source;
		Panning m_panning;
		std::optional<Monitoring> m_monitoring;
		bool m_silent = false;
		StripPart m_part = StripPart::Whole;
		ChannelSections m_sections;
		std::vector<const void*> m_processors; // the source and the slots' plugins
		std::vector<ObjectId> m_slots;         // the slots' ids
		uint32_t m_smoothing = 0;              // g_faderSmoothing in frames
		double m_sampleRate = 48000.0;
		graph::EventBuffer m_sourceEvents;
		graph::EventBuffer m_faderEvents;
		graph::EventBuffer m_faderOut;
		std::shared_ptr<PreFaderTap> m_tap; // or none, without pre-fader sends
	};

	// The processor of a send node (graph::NodeKind::Send): a post-fader send's input is its channel's output, a
	// pre-fader send's the channel's PreFaderTap. It scales it by the send's gain, gliding over a block from the gain
	// of the last block, which the channel's state keeps across graphs.
	class SendStrip : public graph::Processor
	{
	public:
		SendStrip(std::shared_ptr<ChannelState> state, int number, float gain, std::shared_ptr<PreFaderTap> tap);

		void process(const graph::ProcessContext& context) noexcept override;

	private:
		std::shared_ptr<ChannelState> m_state;
		size_t m_number = 0; // the send's index
		float m_gain = 0.0f;
		std::shared_ptr<PreFaderTap> m_tap;
	};

	// The processor of a plugin slot node on a channel (graph::NodeKind::PluginSlot): the slot's plugin, with the
	// automation events that the channel's source node played for it in this block added to its events.
	class SlotStrip : public graph::Processor
	{
	public:
		SlotStrip(std::shared_ptr<ChannelState> state, std::shared_ptr<graph::Processor> plugin, uint8_t slot);

		void process(const graph::ProcessContext& context) noexcept override;
		uint32_t latency() const override { return m_plugin->latency(); }
		uint32_t tail() const override { return m_plugin->tail(); }
		void reset() noexcept override { (*m_plugin).reset(); } // the plugin's reset, not the pointer's

	protected:
		void prepare(double sampleRate, uint32_t maxFrames) override { m_plugin->prepareFor(sampleRate, maxFrames); }

	private:
		std::shared_ptr<ChannelState> m_state;
		std::shared_ptr<graph::Processor> m_plugin;
		uint8_t m_slot = 0; // its index in the slots of its program's source strip
		graph::EventBuffer m_events;
	};
}
