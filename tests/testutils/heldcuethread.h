// SPDX-License-Identifier: GPL-3.0-or-later
// Holds the audio engine's cue thread back when it first runs, through Engine::setCueStartHook, until the test
// releases it, so that what the test sends right after audio.start reaches the audio thread before the cue thread reads
// anything, however the threads are scheduled (#1889). Only the cue thread waits: the device runs meanwhile, and
// commands and events go on, but nothing posts transport.update or midi.activity and no stream is cued.
#pragma once

#include <chrono>
#include <memory>

namespace gnubase::engine
{
	class Engine;
}

namespace gnubase::test
{
	class HeldCueThread
	{
	public:
		// Holds the cue thread of the engine's next device start, such as audio.start, once, for hold at most: then the
		// hold lapses and the thread goes on as if released. Later starts aren't held. One HeldCueThread per engine at
		// a time; it must not outlive the engine.
		explicit HeldCueThread(engine::Engine& engine, std::chrono::milliseconds hold = std::chrono::seconds(30));

		// Releases the thread and holds no other.
		~HeldCueThread();
		HeldCueThread(const HeldCueThread&) = delete;
		HeldCueThread& operator=(const HeldCueThread&) = delete;
		HeldCueThread(HeldCueThread&&) = delete;
		HeldCueThread& operator=(HeldCueThread&&) = delete;

		// Waits until the cue thread is held and returns whether it is: false if it didn't get there in time, or if it
		// was released or the hold lapsed already.
		bool waitUntilHeld(std::chrono::milliseconds timeout = std::chrono::seconds(30));

		// Lets the thread go on and holds no other. Returns whether it was held until now: false if it didn't get to
		// the hold yet or the hold lapsed, so that it may have run before what the test did.
		bool release();

	private:
		struct State;

		engine::Engine& m_engine;
		std::shared_ptr<State> m_state; // shared with the hook, which the cue thread keeps while it runs
	};
}
