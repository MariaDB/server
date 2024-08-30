#pragma once
#include <queue>
#include <vector>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include "trx0sys.h"

class ThreadPool {
public:
	typedef std::function<void(unsigned)> job_t;

	ThreadPool() { m_stop = false; m_stopped = true; }
	ThreadPool (ThreadPool &&other) = delete;
	ThreadPool &  operator= (ThreadPool &&other) = delete;
	ThreadPool(const ThreadPool &) = delete;
	ThreadPool & operator= (const ThreadPool &) = delete;

	bool start(size_t threads_count);
	void stop();
	void push(job_t &&j);
	size_t threads_count() const { return m_threads.size(); }
private:
	void thread_func(unsigned thread_num);
	std::mutex m_mutex;
	std::condition_variable m_cv;
	std::queue<job_t> m_jobs;
	std::atomic<bool> m_stop;
	std::atomic<bool> m_stopped;
	std::vector<std::thread> m_threads;
};

class TasksGroup {
public:
	TasksGroup(ThreadPool &thread_pool) : m_thread_pool(thread_pool) {
		m_tasks_count = 0;
		m_tasks_result = 1;
	}
	void push_task(ThreadPool::job_t &&j) {
		++m_tasks_count;
		m_thread_pool.push(std::forward<ThreadPool::job_t>(j));
	}
	void finish_task(int res) {
		--m_tasks_count;
		m_tasks_result.fetch_and(res);
	}
	int get_result() const { return m_tasks_result; }
	bool is_finished() const {
		return !m_tasks_count;
	}
	bool wait_for_finish() {
		while (!is_finished())
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
		return get_result();
	}
private:
	ThreadPool &m_thread_pool;
	std::atomic<size_t> m_tasks_count;
	std::atomic<int> m_tasks_result;
};
