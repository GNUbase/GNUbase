// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <memory>
#include <utility>
#include <vector>

namespace gnubase::rt
{
	// Hands immutable snapshots from one writer thread to one reader thread, e.g. render snapshots from the engine's
	// command thread to the audio thread. Publishing is an atomic pointer swap. The reader never waits, allocates or
	// frees; the writer frees an old snapshot once the reader has moved on to a newer one.
	//
	// The reader marks the snapshot it uses, then checks that it is still the newest; the writer frees only snapshots
	// that are no longer the newest and not marked. All accesses are sequentially consistent, so if the writer sees no
	// mark on a snapshot it retired, the reader's check sees the newer one and the reader never touches the old one.
	template <typename T> class SnapshotPublisher
	{
	public:
		explicit SnapshotPublisher(std::unique_ptr<const T> initial)
			: m_current(std::move(initial)), m_latest(m_current.get())
		{
		}

		// The reader must be done with its snapshot.
		~SnapshotPublisher() = default;

		SnapshotPublisher(const SnapshotPublisher&) = delete;
		SnapshotPublisher& operator=(const SnapshotPublisher&) = delete;

		// Writer: makes snapshot the newest one and frees old ones the reader no longer uses.
		void publish(std::unique_ptr<const T> snapshot)
		{
			m_latest.store(snapshot.get());
			m_retired.push_back(std::move(m_current));
			m_current = std::move(snapshot);
			collect();
		}

		// Writer: frees old snapshots the reader no longer uses. publish does this too.
		void collect()
		{
			const T* inUse = m_inUse.load();
			m_retired.erase(std::remove_if(m_retired.begin(), m_retired.end(),
								[inUse](const std::unique_ptr<const T>& retired) { return retired.get() != inUse; }),
				m_retired.end());
		}

		// Writer: the newest snapshot.
		const T& latest() const { return *m_current; }

		// Reader: the newest snapshot. It stays valid until the next acquire or release. Real-time safe.
		const T& acquire()
		{
			const T* snapshot = m_latest.load();
			for (;;)
			{
				m_inUse.store(snapshot);
				const T* newest = m_latest.load();
				if (newest == snapshot)
					return *snapshot;
				snapshot = newest;
			}
		}

		// Reader: no longer uses its snapshot, so the writer may free it. Real-time safe.
		void release() { m_inUse.store(nullptr); }

	private:
		// Writer only.
		std::unique_ptr<const T> m_current;
		std::vector<std::unique_ptr<const T>> m_retired;

		std::atomic<const T*> m_latest;
		std::atomic<const T*> m_inUse{nullptr};
	};
}
