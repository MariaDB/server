#ifndef SQL_HSET_INCLUDED
#define SQL_HSET_INCLUDED
/* Copyright (c) 2010, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

#include "my_global.h"
#include "hash.h"


/**
  A type-safe wrapper around mysys HASH.
*/

template <typename T>
class Hash_set
{
public:
  enum { START_SIZE= 8 };
  /**
    Constructs an empty unique hash.
  */
  Hash_set(PSI_memory_key psi_key,
           const uchar *(*K)(const void *, size_t *, my_bool),
           CHARSET_INFO *cs= &my_charset_bin)
  {
    my_hash_init(psi_key, &m_hash, cs, START_SIZE, 0, 0, K, 0, HASH_UNIQUE);
  }

  Hash_set(PSI_memory_key psi_key, CHARSET_INFO *charset, ulong default_array_elements,
           size_t key_offset, size_t key_length, my_hash_get_key get_key,
           void (*free_element)(void*), uint flags)
  {
    my_hash_init(psi_key, &m_hash, charset, default_array_elements, key_offset,
                 key_length, get_key, free_element, flags);
  }

  Hash_set(const Hash_set&) = delete; /* It is not safe to copy hash_sets. */
  /**
    Destroy the hash by freeing the buckets table. Does
    not call destructors for the elements.
  */
  ~Hash_set()
  {
    my_hash_free(&m_hash);
  }
  /**
    Insert a single value into a hash. Does not tell whether
    the value was inserted -- if an identical value existed,
    it is not replaced.

    @retval TRUE  Out of memory.
    @retval FALSE OK. The value either was inserted or existed
                  in the hash.
  */
  bool insert(const T *value)
  {
    return my_hash_insert(&m_hash, reinterpret_cast<const uchar*>(value));
  }
  bool remove(const T *value)
  {
    return my_hash_delete(&m_hash,
                          reinterpret_cast<uchar*>(const_cast<T*>(value)));
  }
  T *find(const void *key, size_t klen) const
  {
    return (T*)my_hash_search(&m_hash, reinterpret_cast<const uchar *>(key), klen);
  }

  T *find(const T *other) const
  {
    DBUG_ASSERT(m_hash.get_key);
    size_t klen;
    const uchar *key= m_hash.get_key(reinterpret_cast<const uchar *>(other),
                                     &klen, false);
    return find(key, klen);
  }
  /** Is this hash set empty? */
  bool is_empty() const { return m_hash.records == 0; }
  /** Returns the number of unique elements. */
  size_t size() const { return static_cast<size_t>(m_hash.records); }
  /** Erases all elements from the container */
  void clear() { my_hash_reset(&m_hash); }
  const T* at(size_t i) const
  {
    return reinterpret_cast<T*>(my_hash_element(const_cast<HASH*>(&m_hash), i));
  }
  /** An iterator over hash elements. Is not insert-stable. */
  class Iterator;
  using value_type= T;
  using iterator= Iterator;
  using const_iterator= const Iterator;

  Iterator begin() const { return Iterator(*this, 0); }
  Iterator end() const { return Iterator(*this, m_hash.records); }

  class Iterator
  {
  public:
    using iterator_category= std::forward_iterator_tag;
    using value_type= T;
    using difference_type= std::ptrdiff_t;
    using pointer= T *;
    using reference= T &;

    Iterator(const Hash_set &hash_set, uint idx=0) :
      m_hash(&hash_set.m_hash), m_idx(idx) {}

    Iterator &operator++()
    {
      DBUG_ASSERT(m_idx < m_hash->records);
      m_idx++;
      return *this;
    }

    T &operator*()
    {
      return *reinterpret_cast<T *>(my_hash_element(m_hash, m_idx));
    }

    T *operator->()
    {
      return reinterpret_cast<T *>(my_hash_element(m_hash, m_idx));
    }

    bool operator==(const typename Hash_set<T>::iterator &rhs)
    {
      return m_idx == rhs.m_idx && m_hash == rhs.m_hash;
    }
    bool operator!=(const typename Hash_set<T>::iterator &rhs)
    {
      return m_idx != rhs.m_idx || m_hash != rhs.m_hash;
    }
  private:
    const HASH *m_hash;
    uint m_idx;
  };
private:
  HASH m_hash;
};

#endif // SQL_HSET_INCLUDED
