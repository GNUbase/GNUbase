// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine/audio/sampleloader.h"

#include "platform/log.h"

#include <algorithm>
#include <exception>
#include <utility>

namespace gnubase::engine
{
	SampleLoader::SampleLoader(std::function<void()> loaded, Read read)
		: m_loaded(std::move(loaded)), m_read(std::move(read))
	{
	}

	SampleLoader::~SampleLoader()
	{
		{
			const std::scoped_lock lock(m_mutex);
			m_stopping = true;
		}
		m_wake.notify_all();
		if (m_thread.joinable())
			m_thread.join();
	}

	void SampleLoader::request(const std::string& path)
	{
		{
			const std::scoped_lock lock(m_mutex);
			if (m_samples.count(path) != 0 || std::find(m_queue.begin(), m_queue.end(), path) != m_queue.end())
				return;
			m_queue.push_back(path);
			if (!m_thread.joinable())
				m_thread = std::thread([this] { run(); });
		}
		m_wake.notify_all();
	}

	std::shared_ptr<const processors::Sample> SampleLoader::sample(const std::string& path) const
	{
		const std::scoped_lock lock(m_mutex);
		const auto found = m_samples.find(path);
		return found != m_samples.end() ? found->second : nullptr;
	}

	void SampleLoader::keepOnly(const std::set<std::string>& paths)
	{
		const std::scoped_lock lock(m_mutex);
		for (auto entry = m_samples.begin(); entry != m_samples.end();)
			entry = paths.count(entry->first) == 0 ? m_samples.erase(entry) : std::next(entry);
		// The files waiting too, but the one being read, which can't be stopped.
		const auto unwanted = [&paths](const std::string& path) { return paths.count(path) == 0; };
		const auto first = m_queue.begin() + (m_reading ? 1 : 0);
		m_queue.erase(std::remove_if(first, m_queue.end(), unwanted), m_queue.end());
	}

	bool SampleLoader::waitUntilRead(std::chrono::milliseconds timeout)
	{
		std::unique_lock lock(m_mutex);
		return m_done.wait_for(lock, timeout, [this] { return m_queue.empty(); });
	}

	void SampleLoader::run()
	{
		std::unique_lock lock(m_mutex);
		while (true)
		{
			m_wake.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
			if (m_stopping)
				return;
			// The file stays in the queue while it is read, so that it isn't asked for twice and waitUntilRead waits.
			const std::string path = m_queue.front();
			m_reading = true;
			lock.unlock();
			std::shared_ptr<const processors::Sample> sample;
			try
			{
				sample = m_read(path);
			}
			catch (const std::exception& error)
			{
				// Plays nothing, as a missing file does; an exception must not end the thread and the program.
				logging::warning("Reading the sample {} failed: {}", path, error.what());
			}
			lock.lock();
			m_samples[path] = sample ? std::move(sample) : std::make_shared<const processors::Sample>();
			lock.unlock();
			try
			{
				if (m_loaded)
					m_loaded();
			}
			catch (const std::exception& error)
			{
				// The samplers get the sample with the next update instead.
				logging::warning("Announcing the sample {} failed: {}", path, error.what());
			}
			lock.lock();
			m_reading = false;
			m_queue.pop_front();
			m_done.notify_all();
		}
	}
}
