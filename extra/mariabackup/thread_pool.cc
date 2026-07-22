#include "thread_pool.h"
#include "common.h"

bool ThreadPool::start(size_t threads_count) {
	if (!m_stopped)
		return false;
	m_stopped = false;
	for (unsigned i = 0; i < threads_count; ++i)
		m_threads.emplace_back(&ThreadPool::thread_func, this, i);
	return true;
}

void ThreadPool::stop() {
	if (m_stopped)
		return;
	m_stop = true;
	m_cv.notify_all();
	for (auto &t : m_threads)
		t.join();
	m_stopped = true;
};

void ThreadPool::push(ThreadPool::job_t &&j) {
	std::unique_lock<std::mutex> lock(m_mutex);
	m_jobs.push(j);
	lock.unlock();
	m_cv.notify_one();
}

void ThreadPool::thread_func(unsigned thread_num) {
	if (my_thread_init())
		die("Can't init mysql thread");
	std::unique_lock<std::mutex> lock(m_mutex);
	while(true) {
		if (m_stop)
			goto exit;
		while (!m_jobs.empty()) {
			if (m_stop)
				goto exit;
			job_t j = std::move(m_jobs.front());
			m_jobs.pop();
			lock.unlock();
			j(thread_num);
			lock.lock();
		}
		m_cv.wait(lock, [&] { return m_stop || !m_jobs.empty(); });
	}
exit:
	my_thread_end();
}
