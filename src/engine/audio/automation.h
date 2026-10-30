// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/document/rendersnapshot.h"
#include "engine/graph/processor.h"
#include "platform/rt/snapshotpublisher.h"

#include <cstdint>
#include <memory>
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
			Midi,       // MIDI controller events for the channel's source
		};

		AutomationTarget target;
		Kind kind = Kind::Fader;
		uint32_t parameter = 0;   // Fader and Instrument: the parameter id
		double staticValue = 0.0; // the value without points
		double minimum = 0.0;     // values are kept in this range
		double maximum = 1.0;
		std::vector<RenderAutomationPoint> points; // sorted by position; none: the static value
		// A fader lane whose static value changed while it has no points: it glides there over g_faderSmoothing.
		// Not compared: it only says how the new value arrives.
		bool smooth = false;

		bool operator==(const PlayedLane& other) const;
	};

	// What a channel's automation plays, compiled by the command thread: a lane for the fader's volume and pan and
	// for each of the instrument's parameters, with or without points, so that they return to their static values
	// when their automation goes; and a lane for each MIDI controller with points.
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

	// What a channel's automation plays: the fader's and the instrument's parameters with their static values or
	// curves, a MIDI channel's volume and pan as controllers 7 and 10 on MIDI channel 1 when it has them, and the
	// MIDI controllers that have curves. maxVolume is the project's, in dB.
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

		// Audio thread: adds the block's events for the fader to fader and the others to source, for the source
		// processor processor (only compared, to send every value again to a new one). New automation's smooth
		// lanes glide to their values over smoothing frames.
		void play(const graph::TransportBlock& transport, const void* processor, graph::EventBuffer& source,
			graph::EventBuffer& fader, uint32_t smoothing = 0) noexcept;

	private:
		rt::SnapshotPublisher<ChannelAutomation> m_automation;
		uint64_t m_revision = 0; // command thread: the last revision given

		// Audio thread.
		uint64_t m_played = 0;             // the revision played last
		const void* m_processor = nullptr; // the source processor played for last
		int64_t m_next = -1;               // where playback goes on without a jump; -1 while stopped
		int64_t m_stoppedAt = -1;          // the cursor while stopped
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
	};

	// What a channel keeps across graph rebuilds: its automation and its fader.
	struct ChannelState
	{
		AutomationPlayer automation;
		std::shared_ptr<ChannelFader> fader = std::make_shared<ChannelFader>();
	};

	// The processor of a channel node: the channel's source (a track player, a sequencer or an instrument, or
	// nothing for a group or effect channel) with the automation events added to its input, then the fader and its
	// panner. A new strip is made for every graph, around the source and the state that the channel keeps.
	class ChannelStrip : public graph::Processor
	{
	public:
		ChannelStrip(
			std::shared_ptr<ChannelState> state, std::shared_ptr<graph::Processor> source, Panning panning = {});

		void process(const graph::ProcessContext& context) noexcept override;
		uint32_t latency() const override { return m_source ? m_source->latency() : 0; }

	protected:
		void prepare(double sampleRate, uint32_t maxFrames) override;

	private:
		std::shared_ptr<ChannelState> m_state;
		std::shared_ptr<graph::Processor> m_source;
		Panning m_panning;
		uint32_t m_smoothing = 0; // g_faderSmoothing in frames
		graph::EventBuffer m_sourceEvents;
		graph::EventBuffer m_faderEvents;
		graph::EventBuffer m_faderOut;
	};
}
