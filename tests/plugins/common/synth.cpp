// SPDX-License-Identifier: GPL-3.0-or-later
// Synth: a polyphonic sine instrument with a linear attack and release. It sends a note end event when a voice has
// faded out or was taken for a new note, so hosts can test voice lifetimes.
#include "testplugin.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace gnubase::testplugins
{
	namespace
	{
		constexpr uint32_t g_volumeId = 1;
		constexpr uint32_t g_releaseId = 2;
		constexpr double g_attackSeconds = 0.002;
		constexpr double g_twoPi = 6.283185307179586;

		struct Voice
		{
			bool active = false;
			bool releasing = false;
			int32_t noteId = -1;
			int16_t port = 0;
			int16_t channel = 0;
			int16_t key = 0;
			double velocity = 0.0;
			double phase = 0.0;
			double increment = 0.0;
			double level = 0.0;
		};

		bool matches(int32_t want, int32_t have)
		{
			return want < 0 || want == have;
		}

		class Synth : public TestPlugin
		{
		public:
			Synth() : TestPlugin({volumeParam(), releaseParam()}) {}

			void resetState() override { m_voices = {}; }

			// A released voice fades out within the longest release.
			double tailSeconds() const override { return releaseParam().maxValue / 1000.0; }

			void process(const ProcessBlock& block) override
			{
				m_block = &block;
				TestPlugin::process(block);
				m_block = nullptr;
			}

		protected:
			void handleEvent(const Event& event) override
			{
				if (event.type == EventType::NoteOn)
				{
					startVoice(event);
				}
				else if (event.type == EventType::NoteOff)
				{
					for (Voice& voice : m_voices)
						if (voice.active && matches(event.noteId, voice.noteId) && matches(event.key, voice.key) &&
							matches(event.channel, voice.channel))
							voice.releasing = true;
				}
				else
				{
					TestPlugin::handleEvent(event);
				}
			}

			void render(const ProcessBlock& block, uint32_t from, uint32_t to) override
			{
				double volumeDb = 0.0;
				double releaseMs = 0.0;
				paramValue(g_volumeId, volumeDb);
				paramValue(g_releaseId, releaseMs);
				const double gain = std::pow(10.0, volumeDb / 20.0);
				const double attackStep = 1.0 / (g_attackSeconds * sampleRate());
				const double releaseStep = 1.0 / (releaseMs / 1000.0 * sampleRate());
				for (uint32_t i = from; i < to; ++i)
				{
					double sample = 0.0;
					for (Voice& voice : m_voices)
					{
						if (!voice.active)
							continue;
						if (voice.releasing)
						{
							voice.level -= releaseStep;
							if (voice.level <= 0.0)
							{
								endVoice(voice, block, i);
								continue;
							}
						}
						else if (voice.level < 1.0)
						{
							voice.level = std::min(1.0, voice.level + attackStep);
						}
						sample += std::sin(voice.phase) * voice.level * voice.velocity;
						voice.phase += voice.increment;
						if (voice.phase >= g_twoPi)
							voice.phase -= g_twoPi;
					}
					for (uint32_t c = 0; c < block.outputChannels; ++c)
						block.outputs[c][i] = static_cast<float>(sample * gain);
				}
			}

		private:
			// Takes a free voice, or the first one when all are busy: that voice ends, at the new note's time.
			void startVoice(const Event& event)
			{
				Voice* voice = m_voices.data();
				for (Voice& candidate : m_voices)
					if (!candidate.active)
					{
						voice = &candidate;
						break;
					}
				if (voice->active && m_block != nullptr)
					endVoice(*voice, *m_block, std::min(event.time, m_block->frames));
				*voice = Voice{};
				voice->active = true;
				voice->noteId = event.noteId;
				voice->port = event.port;
				voice->channel = event.channel;
				voice->key = event.key;
				voice->velocity = event.value;
				voice->increment = g_twoPi * 440.0 * std::pow(2.0, (event.key - 69) / 12.0) / sampleRate();
			}

			static void endVoice(Voice& voice, const ProcessBlock& block, uint32_t time)
			{
				voice.active = false;
				if (block.outEvents == nullptr)
					return;
				Event end;
				end.type = EventType::NoteEnd;
				end.time = time;
				end.noteId = voice.noteId;
				end.port = voice.port;
				end.channel = voice.channel;
				end.key = voice.key;
				block.outEvents->push(end);
			}

			static ParamInfo volumeParam()
			{
				ParamInfo info;
				info.id = g_volumeId;
				info.name = "Volume";
				info.minValue = -60.0;
				info.maxValue = 0.0;
				info.defaultValue = -12.0;
				info.unit = "dB";
				return info;
			}

			static ParamInfo releaseParam()
			{
				ParamInfo info;
				info.id = g_releaseId;
				info.name = "Release";
				info.minValue = 1.0;
				info.maxValue = 2000.0;
				info.defaultValue = 20.0;
				info.unit = "ms";
				return info;
			}

			std::array<Voice, 16> m_voices{};
			const ProcessBlock* m_block = nullptr; // the block being processed, for note ends from handleEvent
		};
	}

	const Descriptor& pluginDescriptor()
	{
		static const Descriptor descriptor{
			"org.gnubase.test.synth", "GNUbase Test Synth", "A polyphonic sine synth with note end events", true};
		return descriptor;
	}

	std::unique_ptr<TestPlugin> createPlugin(Host& /*host*/)
	{
		return std::make_unique<Synth>();
	}
}
