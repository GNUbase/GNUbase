// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "engine/dsp/delay.h"
#include "engine/dsp/dither.h"
#include "engine/dsp/dynamics.h"
#include "engine/dsp/equalizer.h"
#include "engine/dsp/reverb.h"
#include "engine/graph/processor.h"
#include "engine/processors/builtin.h"
#include "platform/rt/denormals.h"

#include <cstdint>
#include <type_traits>
#include <utility>

// The mixing effects (#114) as built-in processors. Each one runs a core from engine/dsp, which the built-in plugins
// (builtinplugin.h) reach through these processors and the channel strip will use directly.
namespace gnubase::engine::processors
{
	namespace detail
	{
		template <typename Core, typename = void> struct HasLatency : std::false_type
		{
		};
		template <typename Core> struct HasLatency<Core, std::void_t<decltype(std::declval<const Core&>().latency())>>
			: std::true_type
		{
		};

		template <typename Core, typename = void> struct HasTail : std::false_type
		{
		};
		template <typename Core> struct HasTail<Core, std::void_t<decltype(std::declval<const Core&>().tail())>>
			: std::true_type
		{
		};

		template <typename Core, typename = void> struct HasTempo : std::false_type
		{
		};
		template <typename Core> struct HasTempo<Core, std::void_t<decltype(std::declval<Core&>().setTempo(0.0))>>
			: std::true_type
		{
		};
	}

	// Runs a DSP core as a graph::Processor: parameter events take effect at their sample, a core that follows the
	// tempo gets the transport's, and subnormal numbers are off while it processes.
	template <typename Core> class Effect : public graph::Processor
	{
	public:
		void process(const graph::ProcessContext& context) noexcept override
		{
			const rt::ScopedDenormalsDisabled denormals;
			if constexpr (detail::HasTempo<Core>::value)
				m_core.setTempo(context.transport.tempo);
			splitAtEvents(
				context,
				[this](const graph::ProcessEvent& event)
				{
					if (event.type == graph::ProcessEvent::Type::Parameter)
						m_core.setParameter(event.parameter, event.parameterValue);
				},
				[this, &context](uint32_t begin, uint32_t end)
				{
					if (context.audio != nullptr && context.channels > 0)
						m_core.process(context.audio, context.channels, begin, end);
				});
		}

		uint32_t latency() const override
		{
			if constexpr (detail::HasLatency<Core>::value)
				return m_core.latency();
			else
				return 0;
		}

		uint32_t tail() const override
		{
			if constexpr (detail::HasTail<Core>::value)
				return m_core.tail();
			else
				return latency();
		}

		void reset() noexcept override { m_core.reset(); }

		const Core& core() const { return m_core; }

	protected:
		void prepare(double sampleRate, uint32_t maxFrames) override { m_core.prepare(sampleRate, maxFrames); }

	private:
		Core m_core;
	};

	using Equalizer = Effect<dsp::Equalizer>;
	using Gate = Effect<dsp::Gate>;
	using Compressor = Effect<dsp::Compressor>;
	using Limiter = Effect<dsp::Limiter>;
	using Delay = Effect<dsp::Delay>;
	using Reverb = Effect<dsp::Reverb>;
	using Dither = Effect<dsp::Dither>;
}
