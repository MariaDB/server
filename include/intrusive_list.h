/*
   Copyright (c) 2019, 2020, MariaDB

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

#pragma once

#include <cstddef>
#include <iterator>

namespace intrusive
{

// Derive your class from this struct to insert to a linked list.
template <class Tag= void> struct list_node
{
  list_node(list_node *next= NULL, list_node *prev= NULL)
      : next(next), prev(prev)
  {
  }

  list_node *next;
  list_node *prev;
};

// Modelled after std::list<T>
template <class T, class Tag= void> class list
{
public:
  typedef list_node<Tag> ListNode;
  class Iterator;

  // All containers in C++ should define these types to implement generic
  // container interface.
  typedef T value_type;
  typedef std::size_t size_type;
  typedef std::ptrdiff_t difference_type;
  typedef value_type &reference;
  typedef const value_type &const_reference;
  typedef T *pointer;
  typedef const T *const_pointer;
  typedef Iterator iterator;
  typedef Iterator const_iterator; /* FIXME */
  typedef std::reverse_iterator<iterator> reverse_iterator;
  typedef std::reverse_iterator<const iterator> const_reverse_iterator;

  class Iterator
  {
  public:
    // All iterators in C++ should define these types to implement generic
    // iterator interface.
    typedef std::bidirectional_iterator_tag iterator_category;
    typedef T value_type;
    typedef std::ptrdiff_t difference_type;
    typedef T *pointer;
    typedef T &reference;

    Iterator(ListNode *node) : node_(node) {}

    Iterator &operator++()
    {
      node_= node_->next;
      return *this;
    }
    Iterator operator++(int)
    {
      Iterator tmp(*this);
      operator++();
      return tmp;
    }

    Iterator &operator--()
    {
      node_= node_->prev;
      return *this;
    }
    Iterator operator--(int)
    {
      Iterator tmp(*this);
      operator--();
      return tmp;
    }

    reference operator*() { return *static_cast<pointer>(node_); }
    pointer operator->() { return static_cast<pointer>(node_); }

    bool operator==(const Iterator &rhs) { return node_ == rhs.node_; }
    bool operator!=(const Iterator &rhs) { return !(*this == rhs); }

  private:
    ListNode *node_;

    friend class list;
  };

  list() : sentinel_(&sentinel_, &sentinel_), size_(0) {}

  reference front() { return *begin(); }
  reference back() { return *--end(); }
  const_reference front() const { return *begin(); }
  const_reference back() const { return *--end(); }

  iterator begin() { return iterator(sentinel_.next); }
  const_iterator begin() const
  {
    return iterator(const_cast<ListNode *>(sentinel_.next));
  }
  iterator end() { return iterator(&sentinel_); }
  const_iterator end() const
  {
    return iterator(const_cast<ListNode *>(&sentinel_));
  }

  reverse_iterator rbegin() { return reverse_iterator(end()); }
  const_reverse_iterator rbegin() const { return reverse_iterator(end()); }
  reverse_iterator rend() { return reverse_iterator(begin()); }
  const_reverse_iterator rend() const { return reverse_iterator(begin()); }

  bool empty() const { return size_ == 0; }
  size_type size() const { return size_; }

  void clear()
  {
    sentinel_.next= &sentinel_;
    sentinel_.prev= &sentinel_;
    size_= 0;
  }

  iterator insert(iterator pos, reference value)
  {
    ListNode *curr= pos.node_;
    ListNode *prev= pos.node_->prev;

    prev->next= &value;
    curr->prev= &value;

    static_cast<ListNode &>(value).prev= prev;
    static_cast<ListNode &>(value).next= curr;

    ++size_;
    return iterator(&value);
  }

  iterator erase(iterator pos)
  {
    ListNode *prev= pos.node_->prev;
    ListNode *next= pos.node_->next;

    prev->next= next;
    next->prev= prev;

    // This is not required for list functioning. But maybe it'll prevent bugs
    // and ease debugging.
    ListNode *curr= pos.node_;
    curr->prev= NULL;
    curr->next= NULL;

    --size_;
    return next;
  }

  void push_back(reference value) { insert(end(), value); }
  void pop_back() { erase(end()); }

  void push_front(reference value) { insert(begin(), value); }
  void pop_front() { erase(begin()); }

  // STL version is O(n) but this is O(1) because an element can't be inserted
  // several times in the same intrusive list.
  void remove(reference value) { erase(iterator(&value)); }

private:
  ListNode sentinel_;
  size_type size_;
};

} // namespace intrusive
