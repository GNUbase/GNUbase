// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/automation.h"
#include "engine/processors/builtin.h"

#include "engine/graph/graph.h"
#include "engine/midi/midivalues.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
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
			uint32_t declick = 0; // how many frames mute glides
			uint8_t key = 0;      // a plugin lane's: its slot's index in the program that plays

			graph::EventBuffer& of(const PlayedLane& lane) const noexcept
			{
				if (lane.kind == Kind::Fader)
					return fader;
				return lane.kind == Kind::Plugin ? plugins : source;
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
		// parameter glides.
		void emit(const PlayedLane& lane, uint32_t offset, double value, uint32_t ramp, const Outputs& out) noexcept
		{
			using Type = graph::ProcessEvent::Type;
			value = std::clamp(value, lane.minimum, lane.maximum);
			graph::ProcessEvent event;
			event.offset = offset;
			if (lane.kind != Kind::Midi)
			{
				event.type = Type::Parameter;
				event.parameter = lane.parameter;
				event.parameterValue = value;
				event.ramp = lane.target.type == AutomationTargetType::Mute ? std::max(ramp, out.declick) : ramp;
				event.key = out.key;
				out.of(lane).add(event);
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
			out.source.add(event);
		}

		// Plays a lane's curve over one transport segment. With jump, playback didn't arrive here from the frame
		// before, so the value at the segment's start is set first; a lane without points glides there over smoothing
		// frames.
		void playLane(const PlayedLane& lane, const graph::TransportSegment& segment, bool jump, const Outputs& out,
			uint32_t smoothing) noexcept
		{
			const Points& points = lane.points;
			const int64_t begin = segment.position;
			const int64_t end = begin + segment.frames;
			const auto offset = [&](int64_t at) { return segment.offset + static_cast<uint32_t>(at - begin); };
			if (points.empty())
			{
				if (jump)
					emit(lane, segment.offset, lane.staticValue, smoothing, out);
				return;
			}
			if (jump && !inGap(points, begin))
				emit(lane, segment.offset, automationValue(points, begin), 0, out);

			// The fader glides along ramps; instruments, plugins and MIDI get the value at each g_automationInterval
			// frame.
			const bool glides = lane.kind == Kind::Fader;
			int64_t at = begin;
			for (;;)
			{
				// Arriving at points: a step, the end of a ramp or a jump between points at one position.
				const size_t next = after(points, at);
				if (at < end && next > 0 && points[next - 1].position == at && (at > begin || !jump))
				{
					const size_t first =
						static_cast<size_t>(std::lower_bound(points.begin(), points.end(), at,
												[](const RenderAutomationPoint& point, int64_t position)
												{ return point.position < position; }) -
							points.begin());
					// The value from here on, or a terminator's that a gap follows; nothing where a gap starts.
					size_t last = next;
					while (last > first && points[last - 1].gap)
						--last;
					const bool fromRamp = first > 0 && points[first - 1].shape != AutomationShape::Step;
					const bool fromGap = first > 0 && points[first - 1].gap; // nothing was sent there
					const double approached = first == 0 || fromRamp ? points[first].value : points[first - 1].value;
					if (last > first && (points[last - 1].value != approached || (fromRamp && !glides) || fromGap))
						emit(lane, offset(at), points[last - 1].value, 0, out);
				}
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
					// Only the fader follows a straight ramp exactly; everything else goes in pieces.
					const bool pieces = lane.kind != Kind::Fader || from.shape == AutomationShape::Curve;
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
			lanes.push_back(
				{{AutomationTargetType::Mute, 0, 0, 0, 0}, Kind::Fader, ChannelFader::Mute, 0.0, 0.0, 1.0, {}});
		}
		for (const auto& [number, value] :
			{std::pair{uint8_t{7}, channel.midiVolume}, std::pair{uint8_t{10}, channel.midiPan}})
			if (value)
				lanes.push_back({{AutomationTargetType::Controller, 0, number, 0, 0}, Kind::Midi, 0,
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
		for (const RenderSlot& slot : channel.slots)
		{
			// Other plugins report their parameters once they run (#283).
			if (const processors::BuiltinInfo* info = processors::findBuiltinPlugin(slot.plugin))
				for (const processors::ParameterInfo& parameter : info->parameters)
				{
					const auto set = slot.parameters.find(parameter.id);
					const double value = set != slot.parameters.end() ? set->second : parameter.defaultValue;
					lanes.push_back({{AutomationTargetType::Plugin, parameter.id, 0, 0, slot.id}, Kind::Plugin,
						parameter.id, value, parameter.minimum, parameter.maximum, {}});
				}
		}
		const bool midi = channel.type == TrackType::Midi || channel.type == TrackType::Instrument;
		for (const RenderAutomationLane& lane : channel.automation)
		{
			const auto match = std::find_if(
				lanes.begin(), lanes.end(), [&lane](const PlayedLane& played) { return played.target == lane.target; });
			const bool hasStatic = match != lanes.end(); // a lane added below has none
			PlayedLane* played = nullptr;
			if (hasStatic)
				played = &*match;
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
		return automation;
	}

	bool PlayedLane::operator==(const PlayedLane& other) const
	{
		return std::tie(target, kind, parameter, staticValue, minimum, maximum, points) ==
			std::tie(other.target, other.kind, other.parameter, other.staticValue, other.minimum, other.maximum,
				other.points);
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
		m_automation.publish(std::move(automation));
	}

	void AutomationPlayer::play(const graph::TransportBlock& transport, const std::vector<const void*>& processors,
		const std::vector<ObjectId>& slots, graph::EventBuffer& source, graph::EventBuffer& fader,
		graph::EventBuffer& plugins, uint32_t smoothing) noexcept
	{
		const ChannelAutomation& automation = m_automation.acquire();
		// Where a lane's events go: a plugin lane's are keyed by its slot's index in the program that plays, and go
		// nowhere when that program doesn't have its slot (yet, or any more).
		const auto outputs = [&](const PlayedLane& lane) noexcept -> std::optional<Outputs>
		{
			Outputs out{source, fader, plugins, smoothing};
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
		bool changed = automation.revision != m_played || count != m_processorCount ||
			!std::equal(
				processors.begin(), processors.begin() + static_cast<std::ptrdiff_t>(count), m_processors.begin());
		// New static values glide, unless they are the first ones.
		const bool revised = automation.revision != m_played && m_played != 0;
		const auto glide = [revised, smoothing](const PlayedLane& lane)
		{ return revised && lane.smooth ? smoothing : 0; };
		m_played = automation.revision;
		std::copy_n(processors.begin(), count, m_processors.begin());
		m_processorCount = count;

		if (!transport.playing)
		{
			if (changed || m_next >= 0 || transport.position != m_stoppedAt)
				for (const PlayedLane& lane : automation.lanes)
				{
					const std::optional<Outputs> out = outputs(lane);
					if (out && (lane.points.empty() || !inGap(lane.points, transport.position)))
						emit(lane, 0,
							lane.points.empty() ? lane.staticValue : automationValue(lane.points, transport.position),
							glide(lane), *out);
				}
			m_stoppedAt = transport.position;
			m_next = -1;
			return;
		}
		m_stoppedAt = -1;
		for (size_t s = 0; s < transport.segmentCount; ++s)
		{
			const graph::TransportSegment& segment = transport.segments[s];
			const bool jump = changed || segment.position != m_next;
			for (const PlayedLane& lane : automation.lanes)
				if (const std::optional<Outputs> out = outputs(lane))
					playLane(lane, segment, jump, *out, changed ? glide(lane) : 0);
			changed = false;
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

	void ChannelFader::Ramp::set(double newValue, uint32_t ramp) noexcept
	{
		if (ramp == 0)
		{
			value = target = newValue;
			length = 0;
			return;
		}
		start = value;
		target = newValue;
		length = ramp;
		done = 0;
	}

	double ChannelFader::Ramp::next() noexcept
	{
		if (length == 0)
			return value;
		const double current = value;
		++done;
		if (done >= length)
		{
			value = target;
			length = 0;
		}
		else
		{
			value = start + ((target - start) * (static_cast<double>(done) / static_cast<double>(length)));
		}
		return current;
	}

	void ChannelFader::process(const graph::ProcessContext& context, const Panning& panning) noexcept
	{
		uint32_t position = 0;
		for (const graph::ProcessEvent& event : context.eventsIn)
		{
			const uint32_t offset = std::min(event.offset, context.frames);
			if (offset > position)
			{
				render(context, panning, position, offset);
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
			render(context, panning, position, context.frames);
	}

	void ChannelFader::render(
		const graph::ProcessContext& context, const Panning& panning, uint32_t begin, uint32_t end) noexcept
	{
		// While nothing glides, one set of gains serves every frame; otherwise each frame gets its own.
		const bool gliding = m_volume.length != 0 || m_pan.length != 0 || m_panLeft.length != 0 ||
			m_panRight.length != 0 || m_audible.length != 0;
		for (uint32_t i = begin; i < end;)
		{
			const uint32_t until = gliding ? i + 1 : end;
			const double gain = gliding ? m_volume.next() * m_audible.next() : m_volume.value * m_audible.value;
			const double pan = gliding ? m_pan.next() : m_pan.value;
			const double panLeft = gliding ? m_panLeft.next() : m_panLeft.value;
			const double panRight = gliding ? m_panRight.next() : m_panRight.value;
			applyGains(context, panning, i, until, gain, pan, panLeft, panRight);
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
		Panning panning, std::optional<Monitoring> monitoring, bool silent, StripPart part,
		const ChannelSections& sections, std::vector<const void*> slots, std::vector<ObjectId> slotIds)
		: m_state(std::move(state)), m_source(std::move(source)), m_panning(panning), m_monitoring(monitoring),
		  m_silent(silent), m_part(part), m_sections(sections), m_processors(std::move(slots)),
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
	}

	uint32_t ChannelStrip::latency() const
	{
		const uint32_t source = m_source ? m_source->latency() : 0;
		return source + (m_part == StripPart::Source ? 0 : m_state->sections.latency(m_sections));
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
			m_state->pluginEvents, m_smoothing);

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
		m_state->sections.processPre(m_sections, context.audio, context.channels, context.frames);

		if (m_part == StripPart::Source)
		{
			m_state->faderEvents.clear();
			for (const graph::ProcessEvent& event : m_faderEvents)
				m_state->faderEvents.add(event);
			return;
		}
		fade(context, m_faderEvents);
	}

	void ChannelStrip::fade(const graph::ProcessContext& context, const graph::EventBuffer& events) noexcept
	{
		if (context.channels == 0)
			return;
		m_state->sections.processPost(m_sections, context.audio, context.channels, context.frames);
		m_faderOut.clear();
		m_state->fader->process(
			{context.audio, context.channels, context.frames, events, m_faderOut, context.transport}, m_panning);
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
