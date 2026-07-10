/* Copyright (c) 2024, MariaDB plc

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#ifndef QUEUE_INCLUDED
#define QUEUE_INCLUDED

#include <my_global.h>
#include "queues.h"

/**
  A typesafe wrapper of QUEUE, a priority heap
*/
template<typename Element, typename Param=void>
class Queue
{
public:
  Queue() { m_queue.root= 0; }
  ~Queue() { delete_queue(&m_queue); }
  int init(uint max_elements, bool max_at_top, qsort_cmp2 compare,
           Param *param= 0)
  {
    return init_queue(&m_queue, max_elements, 0, max_at_top,
                      compare, (void *)param, 0, 0);
  }

  size_t elements() const { return m_queue.elements; }
  bool is_inited() const { return is_queue_inited(&m_queue); }
  bool is_full() const { return queue_is_full(&m_queue); }
  bool is_empty() const { return elements() == 0; }
  Element *top() const { return (Element*)queue_top(&m_queue); }

  void push(const Element *element) { queue_insert(&m_queue, (uchar*)element); }
  void safe_push(const Element *element)
  {
    if (is_full()) m_queue.elements--; // remove one of the furthest elements
    queue_insert(&m_queue, (uchar*)element);
  }
  Element *pop() { return (Element *)queue_remove_top(&m_queue); }
  void clear() { queue_remove_all(&m_queue); }
  void propagate_top() { queue_replace_top(&m_queue); }
  void replace_top(const Element *element)
  {
    queue_top(&m_queue)= (uchar*)element;
    propagate_top();
  }
private:
  QUEUE m_queue;
};

#endif
