// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/automation.h"
#include "engine/audio/sequencer.h"
#include "engine/document/stripparameters.h"
#include "engine/processors/builtin.h"
#include "engine/processors/builtinplugin.h"
#include "engine/processors/midieffects.h"

#include "engine/graph/graph.h"
#include "engine/midi/midivalues.h"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

namespace gnubase::engine
{
	namespace
	{
		// How strongly a curve of 1 or -1 bends.
		constexpr double g_curveBend = 6.0;
	}

	double automationBend(double x, double curve)
	{
		const double k = g_curveBend * curve;
		if (std::abs(k) < 1e-9)
			return x;
		return (1.0 - std::exp(-k * x)) / (1.0 - std::exp(-k));
	}

	namespace
	{
		// A stereo channel's balance: the side the pan moves away from is turned down.
		double balance(int channel, double pan)
		{
			if (channel == 0)
				return pan > 0.0 ? 1.0 - pan : 1.0;
			return pan < 0.0 ? 1.0 + pan : 1.0;
		}

		// Applies the fader's gain and panner to frames begin to end.
		void applyGains(const graph::ProcessContext& context, const Panning& panning, uint32_t begin, uint32_t end,
			double gain, double pan, double panLeft, double panRight) noexcept
		{
			float* const* audio = context.audio;
			if (panning.panner == Panner::Mono)
			{
				const auto [left, right] = panGains(panning.law, panning.bypass ? 0.0 : pan);
				const auto toLeft = static_cast<float>(gain * left);
				const auto toRight = static_cast<float>(gain * right);
				for (uint32_t i = begin; i < end; ++i)
				{
					audio[1][i] = audio[0][i] * toRight;
					audio[0][i] *= toLeft;
				}
				return;
			}
			const bool stereo = panning.panner == Panner::Stereo && !panning.bypass;
			if (stereo && panning.stereo == StereoPanner::Combined)
			{
				const auto [leftToLeft, leftToRight] = panGains(panning.law, panLeft);
				const auto [rightToLeft, rightToRight] = panGains(panning.law, panRight);
				const auto ll = static_cast<float>(gain * leftToLeft);
				const auto rl = static_cast<float>(gain * rightToLeft);
				const auto lr = static_cast<float>(gain * leftToRight);
				const auto rr = static_cast<float>(gain * rightToRight);
				for (uint32_t i = begin; i < end; ++i)
				{
					const float left = audio[0][i];
					const float right = audio[1][i];
					audio[0][i] = (left * ll) + (right * rl);
					audio[1][i] = (left * lr) + (right * rr);
				}
				return;
			}
			if (gain == 1.0 && (pan == 0.0 || !stereo))
				return;
			for (int c = 0; c < context.channels; ++c)
			{
				const auto factor = static_cast<float>(stereo ? gain * balance(c, pan) : gain);
				for (uint32_t i = begin; i < end; ++i)
					audio[c][i] *= factor;
			}
		}

		using Points = std::vector<RenderAutomationPoint>;
		using Kind = PlayedLane::Kind;

		// Where a block's automation events go: see AutomationPlayer::play.
		struct Outputs
		{
			graph::EventBuffer& source;
			graph::EventBuffer& fader;
			graph::EventBuffer& plugins;
			uint32_t declick = 0;                // how many frames mute glides
			uint8_t key = 0;                     // a plugin lane's: its slot's index in the program that plays
			graph::EventBuffer* again = nullptr; // see AutomationPlayer::play

			graph::EventBuffer& of(const PlayedLane& lane) const noexcept
			{
				if (lane.kind == Kind::Fader || lane.kind == Kind::Strip)
					return fader;
				return lane.kind == Kind::Plugin ? plugins : source;
			}

			// Adds an event to the lane's events, and with sentAgain (a value set again, not a lane's new one) one for
			// source also to again.
			void add(const PlayedLane& lane, const graph::ProcessEvent& event, bool sentAgain) const noexcept
			{
				graph::EventBuffer& events = of(lane);
				if (events.add(event) && sentAgain && again != nullptr && &events == &source)
					again->add(event);
			}
		};

		// A point on the bent curve between two points.
		double bent(const RenderAutomationPoint& from, const RenderAutomationPoint& to, int64_t position)
		{
			const double x =
				static_cast<double>(position - from.position) / static_cast<double>(to.position - from.position);
			return from.value + ((to.value - from.value) * automationBend(x, from.curve));
		}

		// The value between two points, at a position from the first one up to the second one. Curves are played as
		// straight pieces between the points of the curve every g_automationInterval frames from its start, so the
		// value is the same whatever the block size and wherever playback jumps to.
		double between(const RenderAutomationPoint& from, const RenderAutomationPoint& to, int64_t position)
		{
			if (position >= to.position)
				return to.value;
			switch (from.shape)
			{
			case AutomationShape::Step:
				return from.value;
			case AutomationShape::Linear:
				return from.value +
					((to.value - from.value) *
						(static_cast<double>(position - from.position) /
							static_cast<double>(to.position - from.position)));
			case AutomationShape::Curve:
			{
				const int64_t start =
					from.position + (((position - from.position) / g_automationInterval) * g_automationInterval);
				const int64_t end = std::min<int64_t>(start + g_automationInterval, to.position);
				const double a = bent(from, to, start);
				const double b = end == to.position ? to.value : bent(from, to, end);
				return a + ((b - a) * (static_cast<double>(position - start) / static_cast<double>(end - start)));
			}
			}
			return from.value;
		}

		// The first point after position.
		size_t after(const Points& points, int64_t position)
		{
			return static_cast<size_t>(
				std::upper_bound(points.begin(), points.end(), position,
					[](int64_t at, const RenderAutomationPoint& point) { return at < point.position; }) -
				points.begin());
		}

		// Whether a position lies in a gap of a lane that keeps its gap points, a MIDI lane without a static value of
		// the channel (see channelAutomation()), which sends nothing there. points aren't empty.
		bool inGap(const Points& points, int64_t position)
		{
			const size_t next = after(points, position);
			return points[next == 0 ? 0 : next - 1].gap;
		}

		uint32_t midiValue(double value)
		{
			return static_cast<uint32_t>(std::llround(std::clamp(value, 0.0, 1.0) * 4294967295.0));
		}

		// A MIDI lane's value at MIDI 1.0's resolution: 7 bits, 14 for pitch bend.
		uint32_t midi1Value(const PlayedLane& lane, double value)
		{
			const uint32_t full = midiValue(value);
			return lane.target.type == AutomationTargetType::PitchBend ? midi::pitchBendToMidi1(full)
																	   : midi::controllerToMidi1(full);
		}

		// Whether the events have room for a step of a ramp: ramps leave half of them to the source's own events, the
		// notes. The steps they skip only delay the value; the end of a ramp always arrives.
		bool roomForRamp(const graph::EventBuffer& events) noexcept
		{
			return events.size() < events.capacity() / 2;
		}

		// Adds an event that sets the lane's parameter to value at offset, gliding there over ramp frames if the
		// parameter glides; again: it sets the value again (see AutomationPlayer::play).
		void emit(const PlayedLane& lane, uint32_t offset, double value, uint32_t ramp, const Outputs& out,
			bool again = false) noexcept
		{
			using Type = graph::ProcessEvent::Type;
			value = std::clamp(value, lane.minimum, lane.maximum);
			graph::ProcessEvent event;
			event.offset = offset;
			if (lane.kind != Kind::Midi && lane.kind != Kind::Program)
			{
				event.type = Type::Parameter;
				event.parameter = lane.parameter;
				event.parameterValue = value;
				event.ramp = lane.target.type == AutomationTargetType::Mute ? std::max(ramp, out.declick) : ramp;
				event.key = out.key;
				out.add(lane, event, again);
				return;
			}
			if (lane.kind == Kind::Program)
			{
				event.type = Type::Program;
				event.channel = lane.target.channel;
				event.key = static_cast<uint8_t>(lane.parameter);
				event.value = value < 0.0 ? g_noBank : static_cast<uint32_t>(value);
				out.add(lane, event, again);
				return;
			}
			event.channel = lane.target.channel;
			event.value = midiValue(value);
			if (lane.target.type == AutomationTargetType::PitchBend)
			{
				event.type = Type::PitchBend;
			}
			else if (lane.target.type == AutomationTargetType::ChannelPressure)
			{
				event.type = Type::ChannelPressure;
			}
			else
			{
				event.type = Type::Controller;
				event.key = lane.target.number;
			}
			out.add(lane, event, again);
		}

		// The value a lane sends where it arrives at points at a position (a step, the end of a ramp or a jump between
		// points at one position), if there are points there and they send one: the value from there on, or a
		// terminator's that a gap follows; nothing where a gap starts or where the value doesn't change.
		std::optional<double> arriving(const Points& points, int64_t at, bool glides)
		{
			const size_t next = after(points, at);
			if (next == 0 || points[next - 1].position != at)
				return std::nullopt;
			const size_t first = static_cast<size_t>(
				std::lower_bound(points.begin(), points.end(), at,
					[](const RenderAutomationPoint& point, int64_t position) { return point.position < position; }) -
				points.begin());
			size_t last = next;
			while (last > first && points[last - 1].gap)
				--last;
			const bool fromRamp = first > 0 && points[first - 1].shape != AutomationShape::Step;
			// Nothing was sent in a gap, also in virgin territory before the first point, whose gap point is
			// points[0].
			const bool fromGap = points[first == 0 ? 0 : first - 1].gap;
			const double approached = first == 0 || fromRamp ? points[first].value : points[first - 1].value;
			if (last > first && (points[last - 1].value != approached || (fromRamp && !glides) || fromGap))
				return points[last - 1].value;
			return std::nullopt;
		}

		// Plays a lane's curve over one transport segment. With jump, playback didn't arrive here from the frame
		// before, so the value at the segment's start is set first; a lane without points glides there over smoothing
		// frames. That value is set again (AutomationPlayer::play's sentAgain) unless it is new there: renewed, a
		// static value set in this block, or a point's that the lane arrives at there.
		void playLane(const PlayedLane& lane, const graph::TransportSegment& segment, bool jump, const Outputs& out,
			uint32_t smoothing, bool renewed) noexcept
		{
			const Points& points = lane.points;
			const int64_t begin = segment.position;
			const int64_t end = begin + segment.frames;
			const auto offset = [&](int64_t at) { return segment.offset + static_cast<uint32_t>(at - begin); };
			if (points.empty())
			{
				if (jump)
					emit(lane, segment.offset, lane.staticValue, smoothing, out, !renewed);
				return;
			}
			// The fader and the pre gain glide along ramps; the others get the value at each g_automationInterval
			// frame.
			const bool glides = lane.glides();
			if (jump && !inGap(points, begin))
				emit(lane, segment.offset, automationValue(points, begin), 0, out,
					!renewed && !arriving(points, begin, glides));

			int64_t at = begin;
			for (;;)
			{
				const size_t next = after(points, at);
				if (at < end && (at > begin || !jump))
					if (const std::optional<double> value = arriving(points, at, glides))
						emit(lane, offset(at), *value, 0, out);
				if (at >= end || next == points.size())
					break; // after the last point the value stays
				if (next == 0)
				{
					at = std::min(end, points[0].position); // before the first point the value stays too
					continue;
				}

				const RenderAutomationPoint& from = points[next - 1];
				const RenderAutomationPoint& to = points[next];
				const int64_t until = std::min(end, to.position);
				if (from.shape != AutomationShape::Step)
				{
					// Only what glides follows a straight ramp exactly; everything else goes in pieces.
					const bool pieces = !glides || from.shape == AutomationShape::Curve;
					for (int64_t piece = at; piece < until;)
					{
						int64_t pieceEnd = until;
						if (pieces)
						{
							const int64_t grid = from.position +
								((((piece - from.position) / g_automationInterval) + 1) * g_automationInterval);
							pieceEnd = std::min(pieceEnd, grid);
						}
						if (glides)
						{
							emit(lane, offset(piece), between(from, to, pieceEnd),
								static_cast<uint32_t>(pieceEnd - piece), out);
						}
						else if (piece > from.position && (piece - from.position) % g_automationInterval == 0 &&
							(piece > begin || !jump) && roomForRamp(out.of(lane)))
						{
							// MIDI controllers only when their MIDI 1.0 value changes, so that ramps don't flood the
							// events: compared with the step before, which had the same MIDI 1.0 value as the last
							// one sent.
							const double value = between(from, to, piece);
							if (lane.kind != Kind::Midi ||
								midi1Value(lane, value) !=
									midi1Value(lane, between(from, to, piece - g_automationInterval)))
								emit(lane, offset(piece), value, 0, out);
						}
						piece = pieceEnd;
					}
				}
				at = until;
			}
		}
	}

	std::unique_ptr<ChannelAutomation> channelAutomation(const RenderChannel& channel, double maxVolume)
	{
		auto automation = std::make_unique<ChannelAutomation>();
		automation->instrument = channel.instrument;
		std::vector<PlayedLane>& lanes = automation->lanes;
		// The mute button, while no mute automation plays (RenderChannel::muted): the gaps of a mute lane have it too.
		const PlayedLane mute{{AutomationTargetType::Mute, 0, 0, 0, 0}, Kind::Fader, ChannelFader::Mute,
			channel.muted ? 1.0 : 0.0, 0.0, 1.0, {}};
		if (channel.format != ChannelFormat::None)
		{
			lanes.push_back({{AutomationTargetType::Volume, 0, 0, 0, 0}, Kind::Fader, ChannelFader::Volume,
				channel.volume, 0.0, volumeGain(maxVolume), {}});
			lanes.push_back(
				{{AutomationTargetType::Pan, 0, 0, 0, 0}, Kind::Fader, ChannelFader::Pan, channel.pan, -1.0, 1.0, {}});
			lanes.push_back({{AutomationTargetType::PanLeft, 0, 0, 0, 0}, Kind::Fader, ChannelFader::PanLeft,
				channel.panLeft, -1.0, 1.0, {}});
			lanes.push_back({{AutomationTargetType::PanRight, 0, 0, 0, 0}, Kind::Fader, ChannelFader::PanRight,
				channel.panRight, -1.0, 1.0, {}});
			lanes.push_back(mute);
		}
		else if (channel.type == TrackType::Midi)
		{
			lanes.push_back(mute);
		}
		// A silent MIDI track sends its instrument nothing: the snapshot leaves a soloed-out or disabled one's notes
		// and controllers out, a muted one's play into its strip's mute, and the player leaves its MIDI volume and pan,
		// program and MIDI automation out, so it doesn't change what other tracks play there. Its mute lane plays, so
		// its mute still ends the notes that sound.
		automation->silent = channel.type == TrackType::Midi && channel.silent;
		automation->quiet = channel.type == TrackType::Midi && channel.quiet;
		for (const auto& [number, value] :
			{std::pair{uint8_t{7}, channel.midiVolume}, std::pair{uint8_t{10}, channel.midiPan}})
			if (value)
				lanes.push_back({{AutomationTargetType::Controller, 0, number, channel.midiChannel, 0}, Kind::Midi, 0,
					static_cast<double>(midi::controllerFromMidi1(static_cast<uint8_t>(*value))) / 4294967295.0, 0.0,
					1.0, {}});
		if (const processors::BuiltinInfo* info = processors::findBuiltin(channel.instrument))
			for (const processors::ParameterInfo& parameter : info->parameters)
			{
				const auto set = channel.instrumentParameters.find(parameter.id);
				const double value = set != channel.instrumentParameters.end() ? set->second : parameter.defaultValue;
				lanes.push_back({{AutomationTargetType::Instrument, parameter.id, 0, 0, 0}, Kind::Instrument,
					parameter.id, value, parameter.minimum, parameter.maximum, {}});
			}
		// The parameters of the MIDI effects in its MIDI insert and send slots, for its sequencer.
		if (channel.type == TrackType::Midi || channel.type == TrackType::Instrument)
			for (uint8_t number = 0; number < 2 * midi::g_effectSlots; ++number)
			{
				const midi::EffectSlot& slot = (number < midi::g_effectSlots ? channel.midiInserts : channel.midiSends)
												   .slots[number % midi::g_effectSlots];
				if (const processors::BuiltinInfo* info = processors::findMidiEffect(slot.effect))
					for (const processors::ParameterInfo& parameter : info->parameters)
						lanes.push_back({{AutomationTargetType::MidiEffect, parameter.id, number, 0, 0},
							Kind::MidiEffect, midi::g_effectEvents + (uint32_t{number} * 256) + parameter.id,
							parameter.id < slot.parameters.size() ? slot.parameters[parameter.id]
																  : parameter.defaultValue,
							parameter.minimum, parameter.maximum, {}});
			}
		if (channel.format != ChannelFormat::None)
			for (const processors::ParameterInfo& parameter : stripParameters())
			{
				// The pre gain glides as a linear gain, like the fader's volume.
				const auto unit = [&parameter](double value)
				{ return parameter.id == StripParameter::PreGain ? dsp::dbToGain(value) : value; };
				lanes.push_back({{AutomationTargetType::Strip, parameter.id, 0, 0, 0}, Kind::Strip,
					g_stripEvents + parameter.id, unit(stripValue(channel.sections, parameter.id)),
					unit(parameter.minimum), unit(parameter.maximum), {}});
			}
		for (const RenderSlot& slot : channel.slots)
		{
			// Other plugins report their parameters once they run (#283).
			const processors::BuiltinInfo* info = processors::findBuiltinPlugin(slot.plugin);
			if (info == nullptr)
				continue;
			// The values set apply on top of the plugin's state (Slot::parameters).
			for (const processors::ParameterInfo& parameter : info->parameters)
				lanes.push_back({{AutomationTargetType::Plugin, parameter.id, 0, 0, slot.id}, Kind::Plugin,
					parameter.id, processors::BuiltinPlugin::staticValue(parameter, slot.parameters, slot.state.get()),
					parameter.minimum, parameter.maximum, {}});
		}
		const bool midi = channel.type == TrackType::Midi || channel.type == TrackType::Instrument;
		// Lanes of one target play side by side, each its own points: lanes of one controller on several MIDI
		// channels, which a MIDI track set to a channel plays on its own (rendersnapshot.cpp), as it plays the parts'
		// events of several channels. The first takes the static value; a lane added below has none.
		const size_t statics = lanes.size();
		std::vector<bool> taken(statics, false);
		for (const RenderAutomationLane& lane : channel.automation)
		{
			const auto end = lanes.begin() + static_cast<std::ptrdiff_t>(statics);
			const auto match = std::find_if(
				lanes.begin(), end, [&lane](const PlayedLane& played) { return played.target == lane.target; });
			const bool hasStatic = match != end && !taken[static_cast<size_t>(match - lanes.begin())];
			PlayedLane* played = nullptr;
			if (hasStatic)
			{
				taken[static_cast<size_t>(match - lanes.begin())] = true;
				played = &*match;
			}
			else if (midi &&
				(lane.target.type == AutomationTargetType::Controller ||
					lane.target.type == AutomationTargetType::PitchBend ||
					lane.target.type == AutomationTargetType::ChannelPressure))
				played = &lanes.emplace_back(PlayedLane{lane.target, Kind::Midi, 0, 0.0, 0.0, 1.0, {}});
			// Anything else, e.g. a parameter of an instrument the channel no longer plays, waits.
			if (played == nullptr)
				continue;
			played->points = lane.points;
			// Gaps play the static value, as ordinary steps; a mute lane's have the mute button's (see
			// RenderAutomationPoint::gap). MIDI lanes without a static value of the channel keep their gap points and
			// send nothing in their gaps, like a lane that doesn't read.
			if (hasStatic)
				for (RenderAutomationPoint& point : played->points)
					if (point.gap)
					{
						if (lane.target.type != AutomationTargetType::Mute)
							point.value = played->staticValue;
						point.gap = false;
					}
		}
		// The program first, so the instrument has the patch before the controllers. No automation lane targets it.
		if (channel.program)
			lanes.insert(lanes.begin(),
				PlayedLane{{AutomationTargetType::Controller, 0, 0, channel.midiChannel, 0}, Kind::Program,
					static_cast<uint32_t>(*channel.program), channel.bank ? static_cast<double>(*channel.bank) : -1.0,
					-1.0, static_cast<double>(g_maxBank), {}});
		return automation;
	}

	bool PlayedLane::operator==(const PlayedLane& other) const
	{
		return std::tie(target, kind, parameter, staticValue, minimum, maximum, points) ==
			std::tie(other.target, other.kind, other.parameter, other.staticValue, other.minimum, other.maximum,
				other.points);
	}

	bool PlayedLane::glides() const
	{
		return kind == Kind::Fader || (kind == Kind::Strip && parameter == g_stripEvents + StripParameter::PreGain);
	}

	double automationValue(const std::vector<RenderAutomationPoint>& points, int64_t position)
	{
		if (points.empty())
			return 0.0;
		const size_t next = after(points, position);
		if (next == 0)
			return points.front().value;
		if (next == points.size())
			return points.back().value;
		return between(points[next - 1], points[next], position);
	}

	AutomationPlayer::AutomationPlayer() : m_automation(std::make_unique<const ChannelAutomation>())
	{
	}

	void AutomationPlayer::setAutomation(std::unique_ptr<ChannelAutomation> automation)
	{
		automation->revision = ++m_revision;
		const ChannelAutomation& old = m_automation.latest();
		const auto revisioned = [](const PlayedLane& lane)
		{ return lane.kind == Kind::Program || (lane.kind == Kind::Midi && lane.points.empty()); };
		for (PlayedLane& lane : automation->lanes)
			if (revisioned(lane))
			{
				// The same as before, or as when the track was last heard: a value changed and changed back while it
				// was silent doesn't go out again.
				const auto same = [&lane](const std::vector<PlayedLane>& lanes) -> const PlayedLane*
				{
					const auto found = std::find(lanes.begin(), lanes.end(), lane);
					return found != lanes.end() ? &*found : nullptr;
				};
				const PlayedLane* kept = same(old.lanes);
				if (kept == nullptr)
					kept = same(m_heardLanes);
				lane.revision = kept != nullptr ? kept->revision : automation->revision;
			}
		if (!automation->silent)
		{
			m_heardLanes.clear();
			std::copy_if(
				automation->lanes.begin(), automation->lanes.end(), std::back_inserter(m_heardLanes), revisioned);
		}
		m_automation.publish(std::move(automation));
	}

	void AutomationPlayer::play(const graph::TransportBlock& transport, const std::vector<const void*>& processors,
		const std::vector<ObjectId>& slots, graph::EventBuffer& source, graph::EventBuffer& fader,
		graph::EventBuffer& plugins, uint32_t smoothing, graph::EventBuffer* sentAgain) noexcept
	{
		const ChannelAutomation& automation = m_automation.acquire();
		if (sentAgain != nullptr)
			sentAgain->clear();
		// The first values of a new channel don't glide, not even mute's.
		const uint32_t declick = m_played == 0 ? 0 : smoothing;
		// Where a lane's events go: a plugin lane's are keyed by its slot's index in the program that plays, and go
		// nowhere when that program doesn't have its slot (yet, or any more).
		const auto outputs = [&](const PlayedLane& lane) noexcept -> std::optional<Outputs>
		{
			Outputs out{source, fader, plugins, declick, 0, sentAgain};
			if (lane.kind != Kind::Plugin)
				return out;
			const auto slot = std::find(slots.begin(), slots.end(), lane.target.slot);
			if (slot == slots.end())
				return std::nullopt;
			out.key = static_cast<uint8_t>(slot - slots.begin());
			return out;
		};
		// New automation, or a new source processor (e.g. another instrument) or plugin that hasn't had the values
		// yet: the automation may have reached the audio thread before the graph with the new processor.
		const size_t count = std::min(processors.size(), m_processors.size());
		const bool newProcessors = count != m_processorCount ||
			!std::equal(
				processors.begin(), processors.begin() + static_cast<std::ptrdiff_t>(count), m_processors.begin());
		bool changed = automation.revision != m_played || newProcessors;
		// The program and the MIDI controllers without points go out again only when they changed since the track was
		// last heard, for a new processor, a start, a locate or a jump, and the controllers after a new program: other
		// new automation, e.g. a new MIDI volume, or the track being heard again would override a program change or
		// controller a part has played. A silent MIDI track plays none of them.
		const uint64_t heard = m_heard;
		const bool silent = automation.silent;
		const auto midi = [](const PlayedLane& lane) { return lane.kind == Kind::Midi || lane.kind == Kind::Program; };
		const bool programChanged = newProcessors ||
			std::any_of(automation.lanes.begin(), automation.lanes.end(),
				[heard](const PlayedLane& lane) { return lane.kind == Kind::Program && lane.revision > heard; });
		const auto again = [&](const PlayedLane& lane)
		{
			if (lane.kind == Kind::Program)
				return programChanged;
			if (lane.kind == Kind::Midi && lane.points.empty())
				return programChanged || lane.revision > heard;
			return changed;
		};
		const auto hear = [&](bool moved)
		{
			if (!silent)
			{
				m_heard = automation.revision;
				m_missed = HeardAgain::No;
			}
			else if (moved || newProcessors)
			{
				m_heard = 0;
				if (moved)
					m_missed = automation.quiet ? HeardAgain::Quiet : std::max(m_missed, HeardAgain::Muted);
			}
		};
		// Heard again after playback moved while it was silent: its parts are chased after what it sends here.
		m_heardAgain->store(silent ? HeardAgain::No : m_missed, std::memory_order_relaxed);
		// New static values glide, unless they are the first ones.
		const bool revised = automation.revision != m_played && m_played != 0;
		const auto glide = [revised, smoothing](const PlayedLane& lane)
		{ return revised && lane.smooth ? smoothing : 0; };
		// A program or MIDI controller without points whose value is new in this block (PlayedLane::revision) isn't
		// set again: a MIDI track's mute sends what it kept before it, as before a lane's new value. Nor are the MIDI
		// controllers that a new program brings along, which go out after it.
		const bool newProgram =
			std::any_of(automation.lanes.begin(), automation.lanes.end(), [played = m_played](const PlayedLane& lane)
				{ return lane.kind == Kind::Program && lane.revision > played; });
		const auto renewed = [played = m_played, newProgram, &midi](const PlayedLane& lane)
		{ return lane.revision > played || (newProgram && midi(lane)); };
		m_played = automation.revision;
		std::copy_n(processors.begin(), count, m_processors.begin());
		m_processorCount = count;

		if (!transport.playing)
		{
			const bool moved = m_next >= 0 || transport.position != m_stoppedAt;
			if (changed || moved)
				for (const PlayedLane& lane : automation.lanes)
				{
					if ((silent && midi(lane)) || !(moved || again(lane)))
						continue;
					const std::optional<Outputs> out = outputs(lane);
					if (out && (lane.points.empty() || !inGap(lane.points, transport.position)))
						emit(lane, 0,
							lane.points.empty() ? lane.staticValue : automationValue(lane.points, transport.position),
							glide(lane), *out, !renewed(lane));
				}
			hear(moved);
			m_stoppedAt = transport.position;
			m_next = -1;
			return;
		}
		m_stoppedAt = -1;
		bool first = true;
		for (size_t s = 0; s < transport.segmentCount; ++s)
		{
			const graph::TransportSegment& segment = transport.segments[s];
			const bool moved = segment.position != m_next;
			for (const PlayedLane& lane : automation.lanes)
				if (const std::optional<Outputs> out = outputs(lane); out && !(silent && midi(lane)))
					playLane(lane, segment, moved || (first && again(lane)), *out, first && changed ? glide(lane) : 0,
						first && renewed(lane));
			first = false;
			hear(moved);
			m_next = segment.position + segment.frames;
		}
	}

	ChannelFader::ChannelFader()
	{
		m_volume.set(1.0, 0);
		m_panLeft.set(-1.0, 0);
		m_panRight.set(1.0, 0);
		m_audible.set(1.0, 0);
	}

	void ChannelFader::process(
		const graph::ProcessContext& context, const Panning& panning, PreFaderTap* muted) noexcept
	{
		uint32_t position = 0;
		for (const graph::ProcessEvent& event : context.eventsIn)
		{
			const uint32_t offset = std::min(event.offset, context.frames);
			if (offset > position)
			{
				render(context, panning, muted, position, offset);
				position = offset;
			}
			if (event.type != graph::ProcessEvent::Type::Parameter)
				continue;
			if (event.parameter == Mute)
			{
				m_audible.set(1.0 - std::clamp(event.parameterValue, 0.0, 1.0), event.ramp);
				continue;
			}
			Ramp* const ramps[] = {&m_volume, &m_pan, &m_panLeft, &m_panRight};
			if (event.parameter < std::size(ramps))
				ramps[event.parameter]->set(
					event.parameter == Volume ? event.parameterValue : std::clamp(event.parameterValue, -1.0, 1.0),
					event.ramp);
		}
		if (position < context.frames)
			render(context, panning, muted, position, context.frames);
	}

	void ChannelFader::render(const graph::ProcessContext& context, const Panning& panning, PreFaderTap* muted,
		uint32_t begin, uint32_t end) noexcept
	{
		// While nothing glides, one set of gains serves every frame; otherwise each frame gets its own.
		const bool gliding = m_volume.length != 0 || m_pan.length != 0 || m_panLeft.length != 0 ||
			m_panRight.length != 0 || m_audible.length != 0;
		for (uint32_t i = begin; i < end;)
		{
			const uint32_t until = gliding ? i + 1 : end;
			const double volume = gliding ? m_volume.next() : m_volume.value;
			const double audible = gliding ? m_audible.next() : m_audible.value;
			const double pan = gliding ? m_pan.next() : m_pan.value;
			const double panLeft = gliding ? m_panLeft.next() : m_panLeft.value;
			const double panRight = gliding ? m_panRight.next() : m_panRight.value;
			applyGains(context, panning, i, until, volume * audible, pan, panLeft, panRight);
			if (muted != nullptr && audible != 1.0)
				for (int c = 0; c < std::min(context.channels, muted->channels()); ++c)
					for (uint32_t frame = i; frame < until; ++frame)
						muted->channel(c)[frame] *= static_cast<float>(audible);
			i = until;
		}
	}

	bool Monitoring::active(bool playing, bool recording) const noexcept
	{
		if (monitor)
			return true;
		if (!recordEnable)
			return false;
		switch (mode)
		{
		case AutoMonitoring::Manual:
			return false;
		case AutoMonitoring::WhileRecordEnabled:
			return true;
		case AutoMonitoring::WhileRecording:
			return recording;
		case AutoMonitoring::TapeMachine:
			return !playing || recording;
		}
		return false;
	}

	void ChannelStrip::glide(const graph::ProcessContext& context, int channels, float& gain, float target) noexcept
	{
		const float from = gain < 0.0f ? target : gain;
		gain = target;
		const float step = (target - from) / static_cast<float>(context.frames);
		if (from != 1.0f || target != 1.0f)
			for (int c = 0; c < channels; ++c)
				for (uint32_t i = 0; i < context.frames; ++i)
					context.audio[c][i] *= from + (step * static_cast<float>(i + 1));
	}

	ChannelStrip::ChannelStrip(std::shared_ptr<ChannelState> state, std::shared_ptr<graph::Processor> source,
		Panning panning, std::optional<Monitoring> monitoring, bool silent, StripPart part, bool lookAhead,
		std::vector<const void*> slots, std::vector<ObjectId> slotIds)
		: m_state(std::move(state)), m_source(std::move(source)), m_panning(panning), m_monitoring(monitoring),
		  m_silent(silent), m_part(part), m_lookAhead(lookAhead), m_processors(std::move(slots)),
		  m_slots(std::move(slotIds)), m_sourceEvents(graph::g_maxEventsPerBlock),
		  m_faderEvents(graph::g_maxEventsPerBlock), m_faderOut(graph::g_maxEventsPerBlock)
	{
		m_processors.insert(m_processors.begin(), m_source.get());
	}

	void ChannelStrip::prepare(double sampleRate, uint32_t maxFrames)
	{
		if (m_source)
			m_source->prepareFor(sampleRate, maxFrames);
		m_state->sections.prepare(sampleRate, maxFrames);
		m_smoothing = static_cast<uint32_t>(std::llround(sampleRate * g_faderSmoothing));
		m_sampleRate = sampleRate;
		if (m_tap)
			m_tap->prepare(maxFrames);
		if (m_render)
			m_render->prepare(maxFrames);
	}

	void PreFaderTap::prepare(uint32_t maxFrames)
	{
		m_maxFrames = maxFrames;
		m_samples.assign(static_cast<size_t>(m_channels) * maxFrames, 0.0f);
	}

	SendStrip::SendStrip(std::shared_ptr<ChannelState> state, int number, float gain, std::shared_ptr<PreFaderTap> tap)
		: m_state(std::move(state)), m_number(static_cast<size_t>(number)), m_gain(gain), m_tap(std::move(tap))
	{
	}

	void SendStrip::process(const graph::ProcessContext& context) noexcept
	{
		if (m_tap)
			for (int c = 0; c < std::min(context.channels, m_tap->channels()); ++c)
				std::memcpy(context.audio[c], m_tap->channel(c), context.frames * sizeof(float));
		if (context.frames > 0)
			ChannelStrip::glide(context, context.channels, m_state->sendGains[m_number], m_gain);
	}

	uint32_t ChannelStrip::latency() const
	{
		const uint32_t source = m_source ? m_source->latency() : 0;
		return source + (m_part == StripPart::Source ? 0 : m_state->sections.latency(m_lookAhead));
	}

	void ChannelStrip::process(const graph::ProcessContext& context) noexcept
	{
		if (m_part == StripPart::Fader)
		{
			// The source node played the automation in this block already.
			fade(context, m_state->faderEvents);
			return;
		}
		m_sourceEvents.clear();
		m_faderEvents.clear();
		for (const graph::ProcessEvent& event : context.eventsIn)
			m_sourceEvents.add(event);
		m_sourceEvents.nextSource();
		m_state->pluginEvents.clear();
		m_state->automation.play(context.transport, m_processors, m_slots, m_sourceEvents, m_faderEvents,
			m_state->pluginEvents, m_smoothing, m_sentAgain.capacity() > 0 ? &m_sentAgain : nullptr);

		if (m_monitoring && context.frames > 0)
		{
			const graph::TransportBlock& transport = context.transport;
			glide(context, context.channels, m_state->monitorGain,
				m_monitoring->active(transport.playing, transport.recording) ? 1.0f : 0.0f);
		}

		if (m_source)
			m_source->process({context.audio, context.channels, context.frames, m_sourceEvents, context.eventsOut,
				context.transport});
		else
			for (const graph::ProcessEvent& event : m_sourceEvents)
				context.eventsOut.add(event);
		if (context.channels == 0)
			muteMidi(context.eventsOut, context.transport);
		render(RenderPoint::Source, context);
		m_state->sections.processPre(m_faderEvents, context.audio, context.channels, context.frames);

		if (m_part == StripPart::Source)
		{
			m_state->faderEvents.clear();
			for (const graph::ProcessEvent& event : m_faderEvents)
				m_state->faderEvents.add(event);
			return;
		}
		fade(context, m_faderEvents);
	}

	int DroppedMidi::kindOf(const graph::ProcessEvent& event) noexcept
	{
		using Type = graph::ProcessEvent::Type;
		const auto channel = static_cast<int>((event.channel & 15U) * g_perChannel);
		switch (event.type)
		{
		case Type::Controller:
			return channel + (event.key & 127);
		case Type::Program:
			return channel + 128;
		case Type::PitchBend:
			return channel + 129;
		case Type::ChannelPressure:
			return channel + 130;
		case Type::NoteOn:
		case Type::NoteOff:
		case Type::PolyPressure: // its note was ended
		case Type::SysEx:
		case Type::Parameter:
		case Type::NoteExpression:
			return -1;
		}
		return -1;
	}

	void DroppedMidi::drop(const graph::ProcessEvent& event) noexcept
	{
		const int kind = kindOf(event);
		if (kind < 0)
			return;
		const auto index = static_cast<uint16_t>(kind);
		if (m_waiting[index])
			unlink(index);
		m_previous[index] = m_last;
		m_next[index] = g_none;
		(m_last != g_none ? m_next[m_last] : m_first) = index;
		m_last = index;
		m_waiting[index] = true;
		m_values[index] = event.value;
		m_keys[index] = event.key;
	}

	void DroppedMidi::forget(const graph::ProcessEvent& event) noexcept
	{
		const int kind = kindOf(event);
		if (kind < 0 || !m_waiting[static_cast<size_t>(kind)])
			return;
		const auto index = static_cast<uint16_t>(kind);
		unlink(index);
		m_waiting[index] = false;
	}

	void DroppedMidi::unlink(uint16_t kind) noexcept
	{
		const uint16_t previous = m_previous[kind];
		const uint16_t next = m_next[kind];
		(previous != g_none ? m_next[previous] : m_first) = next;
		(next != g_none ? m_previous[next] : m_last) = previous;
	}

	void DroppedMidi::clear() noexcept
	{
		for (uint16_t kind = m_first; kind != g_none; kind = m_next[kind])
			m_waiting[kind] = false;
		m_first = g_none;
		m_last = g_none;
	}

	void ChannelStrip::muteMidi(graph::EventBuffer& out, const graph::TransportBlock& transport) noexcept
	{
		using Type = graph::ProcessEvent::Type;
		ChannelState& state = *m_state;
		// Another project's track on this channel: what the old one's mute kept isn't this one's, and the pedals that
		// hold the old one's notes go up at the block's start, before this one's events, as a mute releases them: the
		// next project may play the same instrument (project.revert), which would keep them down.
		bool release = false;
		if (state.midiProject != m_project)
		{
			state.midiProject = m_project;
			state.midiDropped.clear();
			for (const std::array<uint32_t, 3>& values : state.midiPedals)
				release = release ||
					std::any_of(values.begin(), values.end(),
						[](uint32_t value) { return midi::controllerToMidi1(value) != 0; });
			if (!release)
				state.midiPedals = {};
		}
		const auto isMute = [](const graph::ProcessEvent& event) noexcept
		{ return event.type == Type::Parameter && event.parameter == ChannelFader::Mute; };
		const auto note = [](const graph::ProcessEvent& event) noexcept
		{ return (size_t{event.channel & 15U} * 128) + (event.key & 127U); };
		// A pedal's index in ChannelState::midiPedals.
		const auto pedal = [](uint8_t controller) noexcept -> size_t
		{
			if (controller == 64)
				return 0;
			return controller == 66 ? 1 : 2;
		};
		// Keeps track of the notes that sound and the pedals' values, for an event that passes.
		const auto track = [&state, &note, &pedal](const graph::ProcessEvent& event) noexcept
		{
			if (event.type == Type::NoteOn || event.type == Type::NoteOff)
				state.midiHeld[note(event)] = event.type == Type::NoteOn;
			else if (event.type == Type::Controller && graph::holdsNotes(event.key))
				state.midiPedals[event.channel & 15U][pedal(event.key)] = event.value;
		};
		// Whether an event passes: a note-off of a note that sounds always does. A muted one's value waits for
		// unmuting; one that passes replaces such a value of its kind, unless it goes out before it (replaces false).
		const auto pass = [&state, &note, &track](const graph::ProcessEvent& event, bool replaces = true) noexcept
		{
			if (event.type == Type::NoteOff && state.midiHeld[note(event)])
			{
				state.midiHeld[note(event)] = false;
				return true;
			}
			if (state.midiMuted)
			{
				state.midiDropped.drop(event);
				return false;
			}
			track(event);
			if (replaces && state.midiDropped.waiting())
				state.midiDropped.forget(event);
			return true;
		};
		// The track's own chase, for its strip alone (chaseFrom): a mute that keeps it keeps the values for unmuting;
		// otherwise the instrument chased them, which makes values dropped before stale.
		const graph::EventBuffer* chased = m_chased && !m_chased->empty() ? m_chased.get() : nullptr;
		// Heard again after playback moved while it was silent: the sequencer that chases the track chases the kinds
		// its channel sends again, the track's latest included, and the track's own chase is in chased. Of those kinds
		// what the mute kept is forgotten before anything that waits goes out, so that only the chased value reaches
		// the instrument; also a value that the track's MIDI input sent into the mute, as at a jump.
		if (state.midiDropped.waiting() && state.automation.heardNow() != HeardAgain::No)
		{
			if (m_chaser)
				for (const graph::ProcessEvent& kind : m_channelKinds)
					if (m_chaser->chasesKindOf(kind))
						state.midiDropped.forget(kind);
			if (chased != nullptr)
				for (const graph::ProcessEvent& event : *chased)
					state.midiDropped.forget(event);
			// A track that isn't routed to an instrument (no chaseFrom) chases into its own events, at the block's
			// start, where its sequencer chases them after the channel's values: what it sets there, the chase or the
			// track's events, is later than what the mute kept. The values that the automation sent again there are
			// earlier.
			if (!m_chased)
			{
				std::bitset<DroppedMidi::g_kinds> again;
				for (const graph::ProcessEvent& event : m_sentAgain)
					if (const int kind = DroppedMidi::kindOf(event); event.offset == 0 && kind >= 0)
						again[static_cast<size_t>(kind)] = true;
				for (const graph::ProcessEvent& event : out)
					if (const int kind = DroppedMidi::kindOf(event);
						event.offset == 0 && kind >= 0 && !again[static_cast<size_t>(kind)])
						state.midiDropped.forget(event);
			}
		}
		const auto take = [this, &state, &track](const graph::ProcessEvent& event) noexcept
		{
			if (state.midiMuted && m_keepChase)
			{
				state.midiDropped.drop(event);
				return;
			}
			track(event);
			if (state.midiDropped.waiting())
				state.midiDropped.forget(event);
		};
		// The block's jumps up to offset (a start, a locate or a cycle jump, as the sequencer finds them): what the
		// mute dropped before one is stale after it, where the chase sets what is in effect.
		size_t segment = 0;
		const auto jumpsUntil = [&state, &transport, &segment](uint32_t offset) noexcept
		{
			if (!transport.playing)
			{
				if (segment > 0)
					return;
				segment = 1;
				if (state.midiNext >= 0)
					state.midiCursor = state.midiNext; // where playback stopped
				state.midiNext = -1;
				if (state.midiCursor >= 0 && transport.position != state.midiCursor) // a locate while stopped
					state.midiDropped.clear();
				state.midiCursor = transport.position;
				return;
			}
			for (; segment < transport.segmentCount && transport.segments[segment].offset <= offset; ++segment)
			{
				const graph::TransportSegment& at = transport.segments[segment];
				if (at.position != state.midiNext)
					state.midiDropped.clear();
				state.midiNext = at.position + at.frames;
			}
		};
		if (!release && !state.midiMuted && !state.midiDropped.waiting() && chased == nullptr &&
			std::none_of(m_faderEvents.begin(), m_faderEvents.end(), isMute))
		{
			jumpsUntil(UINT32_MAX);
			for (const graph::ProcessEvent& event : out)
				pass(event);
			return;
		}
		// The block's events while they are muted, in the buffer of the fader's output, which a channel without audio
		// doesn't use. m_sourceEvents still has what the source received, the channel's values among them.
		graph::EventBuffer& events = m_faderOut;
		events.clear();
		for (const graph::ProcessEvent& event : out)
			events.add(event);
		out.clear();
		// After a project change's pedal releases, the block's last values that no longer fit wait as if a mute had
		// dropped them, after the block's other events, as the latest of their kinds.
		std::bitset<graph::g_maxEventsPerBlock> deferred;
		if (release)
		{
			for (size_t channel = 0; channel < state.midiPedals.size(); ++channel)
				for (const uint8_t controller : {uint8_t{64}, uint8_t{66}, uint8_t{69}})
					if (midi::controllerToMidi1(state.midiPedals[channel][pedal(controller)]) != 0)
					{
						graph::ProcessEvent up;
						up.type = Type::Controller;
						up.channel = static_cast<uint8_t>(channel);
						up.key = controller;
						out.add(up);
					}
			state.midiPedals = {};
			size_t over = out.size() + events.size() > out.capacity() ? out.size() + events.size() - out.capacity() : 0;
			for (size_t i = events.size(); i > 0 && over > 0; --i)
				if (!graph::endsNotes(events[i - 1]))
				{
					deferred[i - 1] = true;
					--over;
				}
		}
		// Before a mute's count note-offs and pedal releases: in a full buffer they take the places of the last
		// values that passed (EventBuffer::addEnd), which then wait for unmuting as if the mute had dropped them,
		// kept oldest first so that the latest of a kind wins and the kinds keep their order.
		const auto makeRoom = [&state, &out](size_t count) noexcept
		{
			const size_t room = out.capacity() - out.size();
			if (count <= room)
				return;
			size_t displaced = count - room;
			size_t first = out.size();
			for (; first > 0 && displaced > 0; --first)
				if (!graph::endsNotes(out[first - 1]))
					--displaced;
			for (size_t i = first; i < out.size(); ++i)
				if (!graph::endsNotes(out[i]))
					state.midiDropped.drop(out[i]);
		};
		// The events that end notes go first in a full buffer, so that no note hangs.
		const auto put = [&out](const graph::ProcessEvent& event) noexcept
		{ return graph::endsNotes(event) ? out.addEnd(event) : out.add(event); };
		size_t next = 0;  // the events from here on haven't passed yet
		size_t ahead = 0; // but this many of them, which went out early
		// Sends what the mute dropped at offset, as far as it fits with room left for the block's events still to
		// come; the rest follows in the next block.
		const auto resend = [&state, &out, &next, &ahead, &track, &events](uint32_t offset) noexcept
		{
			state.midiDropped.send(offset,
				[&out, &next, &ahead, &track, &events](const graph::ProcessEvent& event) noexcept
				{
					if (out.size() + (events.size() - next - ahead) >= out.capacity() || !out.add(event))
						return false;
					track(event);
					return true;
				});
		};
		// What waits goes out at resendAt, after the values that the automation sent again at that sample (a start, a
		// locate, a jump, new automation, being heard again): the channel's program, MIDI volume, pan and MIDI
		// automation, over which the values the track's events left win, as where a mute lane unmutes. Before the
		// track's own events at that sample and a MIDI lane's new value there, which are later.
		int64_t resendAt = -1;
		const auto flush = [&resendAt, &resend](uint32_t offset) noexcept
		{
			if (resendAt >= 0 && offset > resendAt)
			{
				resend(static_cast<uint32_t>(resendAt));
				resendAt = -1;
			}
		};
		// Whether event is one that the automation sent again (m_sentAgain), which the source passes on at its sample:
		// found among those at that sample that weren't found yet, also where the source left one out (a full buffer,
		// a filter) or put its own between them.
		std::bitset<graph::g_maxEventsPerBlock> found;
		size_t unfound = 0; // m_sentAgain's first one at or after the sample that wasn't found yet
		const auto sentAgain = [this, &found, &unfound](const graph::ProcessEvent& event) noexcept
		{
			while (unfound < m_sentAgain.size() && (m_sentAgain[unfound].offset < event.offset || found[unfound]))
				++unfound;
			for (size_t i = unfound; i < m_sentAgain.size() && m_sentAgain[i].offset == event.offset; ++i)
			{
				const graph::ProcessEvent& again = m_sentAgain[i];
				if (!found[i] && again.type == event.type && again.channel == event.channel && again.key == event.key &&
					again.value == event.value && again.parameter == event.parameter)
				{
					found[i] = true;
					return true;
				}
			}
			return false;
		};
		// A soloed-out or disabled track (m_silent) sends nothing: what its mute dropped waits until it is heard again.
		jumpsUntil(0);
		if (!state.midiMuted && !m_silent)
			resendAt = 0;
		const graph::ProcessEvent* mute = m_faderEvents.begin();
		// Mutes or unmutes at the mute events up to offset. A mute ends the notes that sound and then releases the
		// pedals that hold notes at its sample; unmuting sends the latest values of what the mute dropped.
		const auto muteUntil = [&](uint32_t offset) noexcept
		{
			for (; mute != m_faderEvents.end() && mute->offset <= offset; ++mute)
			{
				if (!isMute(*mute))
					continue;
				flush(mute->offset);
				jumpsUntil(mute->offset);
				const bool muted = mute->parameterValue >= 0.5;
				if (muted && !state.midiMuted)
				{
					resendAt = -1; // unmuted at this sample: what waits stays
					size_t ends = static_cast<size_t>(std::count(state.midiHeld.begin(), state.midiHeld.end(), true));
					for (const std::array<uint32_t, 3>& values : state.midiPedals)
						ends += static_cast<size_t>(std::count_if(values.begin(), values.end(),
							[](uint32_t value) { return midi::controllerToMidi1(value) != 0; }));
					makeRoom(ends);
					for (size_t n = 0; n < g_midiNotes; ++n)
						if (state.midiHeld[n])
						{
							graph::ProcessEvent off;
							off.offset = mute->offset;
							off.type = Type::NoteOff;
							off.channel = static_cast<uint8_t>(n / 128);
							off.key = static_cast<uint8_t>(n % 128);
							out.addEnd(off);
							state.midiHeld[n] = false;
						}
					// A pedal it releases goes back to its value when it unmutes, unless the track changes it
					// meanwhile.
					for (size_t channel = 0; channel < state.midiPedals.size(); ++channel)
						for (const uint8_t controller : {uint8_t{64}, uint8_t{66}, uint8_t{69}})
							if (const uint32_t value = state.midiPedals[channel][pedal(controller)];
								midi::controllerToMidi1(value) != 0)
							{
								graph::ProcessEvent up;
								up.offset = mute->offset;
								up.type = Type::Controller;
								up.channel = static_cast<uint8_t>(channel);
								up.key = controller;
								up.value = 0;
								out.addEnd(up);
								up.value = value;
								state.midiDropped.drop(up);
							}
					state.midiPedals = {};
				}
				const bool unmutes = !muted && state.midiMuted;
				state.midiMuted = muted;
				if (unmutes && !m_silent)
					resendAt = mute->offset;
			}
			flush(offset);
			jumpsUntil(offset);
		};
		// The chase comes at a jump, after the mute events at its sample and before the events there.
		size_t chasedNext = 0;
		const auto chaseUntil = [&](uint32_t offset) noexcept
		{
			for (; chased != nullptr && chasedNext < chased->size() && (*chased)[chasedNext].offset <= offset;
				++chasedNext)
			{
				muteUntil((*chased)[chasedNext].offset);
				take((*chased)[chasedNext]);
			}
		};
		std::bitset<graph::g_maxEventsPerBlock> early; // the events that went out before the ones ahead of them
		for (; next < events.size(); ++next)
		{
			if (early[next])
			{
				--ahead;
				continue;
			}
			if (deferred[next])
				continue;
			const graph::ProcessEvent& event = events[next];
			chaseUntil(event.offset);
			muteUntil(event.offset);
			// The note-offs and the values sent again at the sample go before what waits, which is later, also those
			// that the source put after events it received there, from the track's MIDI input.
			const bool before = sentAgain(event);
			if (!before && event.type != Type::NoteOff)
			{
				if (resendAt == static_cast<int64_t>(event.offset))
					for (size_t later = next + 1; later < events.size() && events[later].offset == event.offset;
						++later)
						if (!deferred[later] && sentAgain(events[later]))
						{
							early[later] = true;
							++ahead;
							if (pass(events[later], false))
								put(events[later]);
						}
				flush(UINT32_MAX);
			}
			if (pass(event, !before || resendAt < 0))
				put(event);
		}
		chaseUntil(UINT32_MAX);
		muteUntil(UINT32_MAX);
		flush(UINT32_MAX);
		for (size_t i = 0; i < events.size(); ++i)
			if (deferred[i])
				state.midiDropped.drop(events[i]);
	}

	void ChannelStrip::render(RenderPoint point, const graph::ProcessContext& context) noexcept
	{
		if (m_render && m_renderPoint == point)
			for (int c = 0; c < std::min(context.channels, m_render->channels()); ++c)
				std::memcpy(m_render->channel(c), context.audio[c], context.frames * sizeof(float));
	}

	void ChannelStrip::fade(const graph::ProcessContext& context, const graph::EventBuffer& events) noexcept
	{
		if (context.channels == 0)
			return;
		render(RenderPoint::Inserts, context);
		m_state->sections.processPost(events, m_lookAhead, context.audio, context.channels, context.frames);
		render(RenderPoint::Strip, context);
		if (m_tap)
			for (int c = 0; c < std::min(context.channels, m_tap->channels()); ++c)
				std::memcpy(m_tap->channel(c), context.audio[c], context.frames * sizeof(float));
		m_faderOut.clear();
		m_state->fader->process(
			{context.audio, context.channels, context.frames, events, m_faderOut, context.transport}, m_panning,
			m_tapFollowsMute ? m_tap.get() : nullptr);
		// A mono channel's panner makes its output stereo, as graph::describe gives its node.
		const ChannelFormat wider = pannerOutput(m_panning.panner);
		const int outputs = wider == ChannelFormat::None ? context.channels : channelCount(wider);
		if (context.frames > 0)
			glide(context, outputs, m_state->outputGain, m_silent ? 0.0f : 1.0f);
		m_state->meter.process(context.audio, outputs, context.frames, m_sampleRate);
	}

	SlotStrip::SlotStrip(std::shared_ptr<ChannelState> state, std::shared_ptr<graph::Processor> plugin, uint8_t slot)
		: m_state(std::move(state)), m_plugin(std::move(plugin)), m_slot(slot), m_events(graph::g_maxEventsPerBlock)
	{
	}

	void SlotStrip::process(const graph::ProcessContext& context) noexcept
	{
		m_events.clear();
		for (const graph::ProcessEvent& event : context.eventsIn)
			m_events.add(event);
		m_events.nextSource();
		for (const graph::ProcessEvent& event : m_state->pluginEvents)
			if (event.key == m_slot)
				m_events.add(event);
		m_plugin->process(
			{context.audio, context.channels, context.frames, m_events, context.eventsOut, context.transport});
	}
}
