/* Copyright(C) 2019, 20222, MariaDB Corporation.

This program is free software; you can redistribute itand /or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111 - 1301 USA*/

#pragma once
#include <my_global.h>
#include <my_pthread.h>
#include <vector>
#include <stack>
#include <assert.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
/* Suppress TSAN warnings, that we believe are not critical. */
#if defined(__has_feature)
#define TPOOL_HAS_FEATURE(...) __has_feature(__VA_ARGS__)
#else
#define TPOOL_HAS_FEATURE(...) 0
#endif

#if TPOOL_HAS_FEATURE(address_sanitizer)
#define TPOOL_SUPPRESS_TSAN  __attribute__((no_sanitize("thread"),noinline))
#elif defined(__GNUC__) && defined (__SANITIZE_THREAD__)
#define TPOOL_SUPPRESS_TSAN  __attribute__((no_sanitize_thread,noinline))
#else
#define TPOOL_SUPPRESS_TSAN
#endif


namespace tpool
{

/**
  Generic "pointer" cache of a fixed size
  with fast put/get operations.

  Compared to STL containers,e.g stack or queue
  is faster/does not do allocations.

  However, get() operation will wait if there is no free items.

  We assume that put() will only put back the elements that
  were retrieved previously with get().
*/
template<typename T, bool timed=false> class cache
{
  /** Protects updates of m_pos and m_cache members */
  std::mutex m_mtx;

  /**
    Notify waiting threads about "cache full" or "cache not empty" conditions
    @see get() and wait()
  */
  std::condition_variable m_cv;

  /** Cached items vector.Does not change after construction */
  std::vector<T> m_base;

  /**
   Pointers to cached items. Protected by m_mtx. Does not grow after
   construction. Elements in position [0,m_pos-1] are "borrowed",
   elements in position [m_pos,capacity()-1] are "free"
  */
  std::vector<T*> m_cache;

  /** Number of threads waiting for "cache full" condition (s. wait())
  Protected by m_mtx */
  int m_waiters;

  /** Current cache size. Protected by m_mtx*/
  size_t m_pos;

  /**
   Total time spent waiting on entries in cache, inside get()
   Only valid if timed template parameter is true.
  */
  std::chrono::duration<double> m_wait_time;

private:

  inline size_t capacity()
  {
    return m_base.size();
  }

  /**
  @return true if cache is full (no items are borrowed)
  */
  bool is_full()
  {
    return m_pos == 0;
  }

  /**
  @return true if cache is empty (all items are borrowed)
  */
  bool is_empty()
  {
    return m_pos == capacity();
  }

  /**
   Wait on condition. Instrumented (wait time is recorded),
   if timed template parameter is true.
  */
  void condition_wait(std::unique_lock<std::mutex>& lock)
  {
    if (timed)
    {
      auto start= std::chrono::high_resolution_clock::now();
      m_cv.wait(lock);
      m_wait_time+= std::chrono::high_resolution_clock::now() - start;
    }
    else
      m_cv.wait(lock);
  }
public:
  /**
  Constructor
  @param size - maximum number of items in cache
  */
  cache(size_t size) :m_mtx(), m_cv(), m_base(size), m_cache(size),
    m_waiters(), m_pos(0), m_wait_time()
  {
    for(size_t i= 0 ; i < size; i++)
      m_cache[i]= &m_base[i];
  }

  /**
   Retrieve an item from cache. Waits for free item, if cache is
   currently empty.
   @return borrowed item
  */
  T* get()
  {
    std::unique_lock<std::mutex> lock(m_mtx);
    while (is_empty())
      condition_wait(lock);
    assert(m_pos < capacity());
    //  return last element
    T *t= m_cache[m_pos++];
    return t;
  }

  std::mutex &mutex() { return m_mtx; }

  /**
   Put back an element to cache.
   @param ele element to put back
  */
  void put(T *ele)
  {
    std::unique_lock<std::mutex> lock(m_mtx);
    assert(!is_full());
    const bool was_empty= is_empty();
    // put element to the logical end of the array
    m_cache[--m_pos] = ele;

    if (was_empty || (is_full() && m_waiters))
      m_cv.notify_all();
  }

  /** Check if pointer represents cached element */
  bool contains(T* ele)
  {
    // No locking required, m_base does not change after construction.
    return ele >= &m_base[0] && ele <= &m_base[capacity() - 1];
  }

  /** Wait until cache is full
  @param lock */
  void wait(std::unique_lock<std::mutex> &lk)
  {
    m_waiters++;
    while (!is_full())
      m_cv.wait(lk);
    m_waiters--;
  }

  /* Wait until cache is full.*/
  void wait()
  {
    std::unique_lock<std::mutex> lock(m_mtx);
    wait(lock);
  }

  /**
   @return approximate number of "borrowed" items.
   A "dirty" read, not used in any critical functionality.
  */
  TPOOL_SUPPRESS_TSAN size_t pos()
  {
    return m_pos;
  }

  TPOOL_SUPPRESS_TSAN std::chrono::duration<double> wait_time()
  {
    return m_wait_time;
  }

  void resize(size_t count)
  {
    assert(is_full());
    m_base.resize(count);
    m_cache.resize(count);
    for (size_t i = 0; i < count; i++)
      m_cache[i] = &m_base[i];
  }
};


/**
  Circular, fixed size queue
  used for the task queue.

  Compared to STL queue, this one is
  faster, and does not do memory allocations
*/
template <typename T> class circular_queue
{

public:
  circular_queue(size_t N = 16)
    : m_capacity(N + 1), m_buffer(m_capacity), m_head(), m_tail()
  {
  }
  bool empty() { return m_head == m_tail; }
  bool full() { return (m_head + 1) % m_capacity == m_tail; }
  void clear() { m_head = m_tail = 0; }
  void resize(size_t new_size)
  {
    auto current_size = size();
    if (new_size <= current_size)
      return;
    size_t new_capacity = new_size - 1;
    std::vector<T> new_buffer(new_capacity);
    /* Figure out faster way to copy*/
    size_t i = 0;
    while (!empty())
    {
      T& ele = front();
      pop();
      new_buffer[i++] = ele;
    }
    m_buffer = new_buffer;
    m_capacity = new_capacity;
    m_tail = 0;
    m_head = current_size;
  }
  void push(T ele)
  {
    if (full())
    {
      assert(size() == m_capacity - 1);
      resize(size() + 1024);
    }
    m_buffer[m_head] = ele;
    m_head = (m_head + 1) % m_capacity;
  }
  void push_front(T ele)
  {
    if (full())
    {
      resize(size() + 1024);
    }
    if (m_tail == 0)
      m_tail = m_capacity - 1;
    else
      m_tail--;
    m_buffer[m_tail] = ele;
  }
  T& front()
  {
    assert(!empty());
    return m_buffer[m_tail];
  }
  void pop()
  {
    assert(!empty());
    m_tail = (m_tail + 1) % m_capacity;
  }
  size_t size()
  {
    if (m_head < m_tail)
    {
      return m_capacity - m_tail + m_head;
    }
    else
    {
      return m_head - m_tail;
    }
  }

  /*Iterator over elements in queue.*/
  class iterator
  {
    size_t m_pos;
    circular_queue<T>* m_queue;
  public:
    explicit iterator(size_t pos , circular_queue<T>* q) : m_pos(pos), m_queue(q) {}
    iterator& operator++()
    {
      m_pos= (m_pos + 1) % m_queue->m_capacity;
      return *this;
    }
    iterator operator++(int)
    {
      iterator retval= *this;
      ++*this;
      return retval;
    }
    bool operator==(iterator other) const { return m_pos == other.m_pos; }
    bool operator!=(iterator other) const { return !(*this == other); }
    T& operator*() const { return m_queue->m_buffer[m_pos]; }
  };

  iterator begin() { return iterator(m_tail, this); }
  iterator end() { return iterator(m_head, this); }
private:
  size_t m_capacity;
  std::vector<T> m_buffer;
  size_t m_head;
  size_t m_tail;
};

/* Doubly linked list. Intrusive,
   requires element to have m_next and m_prev pointers.
*/
template<typename T> class doubly_linked_list
{
public:
  T* m_first;
  T* m_last;
  size_t m_count;
  doubly_linked_list():m_first(),m_last(),m_count()
  {}
  void check()
  {
    assert(!m_first || !m_first->m_prev);
    assert(!m_last || !m_last->m_next);
    assert((!m_first && !m_last && m_count == 0)
     || (m_first != 0 && m_last != 0 && m_count > 0));
    T* current = m_first;
    for(size_t i=1; i< m_count;i++)
    {
      current = current->m_next;
    }
    assert(current == m_last);
    current = m_last;
    for (size_t i = 1; i < m_count; i++)
    {
      current = current->m_prev;
    }
    assert(current == m_first);
  }
  T* front()
  {
    return m_first;
  }
  size_t size()
  {
    return m_count;
  }
  void push_back(T* ele)
  {
    ele->m_prev = m_last;
    if (m_last)
      m_last->m_next = ele;

    ele->m_next = 0;
    m_last = ele;
    if (!m_first)
      m_first = m_last;

    m_count++;
  }
  T* back()
  {
    return m_last;
  }
  bool empty()
  {
    return m_count == 0;
  }
  void pop_back()
  {
    m_last = m_last->m_prev;
    if (m_last)
      m_last->m_next = 0;
    else
      m_first = 0;
    m_count--;
  }
  bool contains(T* ele)
  {
    if (!ele)
      return false;
    T* current = m_first;
    while(current)
    {
      if(current == ele)
        return true;
      current = current->m_next;
    }
    return false;
  }

  void erase(T* ele)
  {
    assert(contains(ele));

    if (ele == m_first)
    {
      m_first = ele->m_next;
      if (m_first)
        m_first->m_prev = 0;
      else
        m_last = 0;
    }
    else if (ele == m_last)
    {
      assert(ele->m_prev);
      m_last = ele->m_prev;
      m_last->m_next = 0;
    }
    else
    {
      assert(ele->m_next);
      assert(ele->m_prev);
      ele->m_next->m_prev = ele->m_prev;
      ele->m_prev->m_next = ele->m_next;
    }
    m_count--;
  }
};

}
