#pragma once

#include <cstdint>
#include <cstring>
#include "my_dbug.h"
#include "m_string.h"
#include "my_global.h"
#include "m_ctype.h"

namespace traits
{
template<typename Key>
struct Open_address_hash_key_trait;

template<typename Value>
struct Open_address_hash_value_trait;
}

template <typename Key, typename Value,
          typename Key_trait=traits::Open_address_hash_key_trait<Key>,
          typename Value_trait=traits::Open_address_hash_value_trait<Value> >
class Open_address_hash
{
  static const Key *get_key(const Value &elem)
  { return Key_trait::get_key(elem); }
  static bool is_empty(const Value &el) { return Value_trait::is_empty(el); }
  static bool is_equal(const Value &lhs, const Value &rhs)
  { return Value_trait::is_equal(lhs, rhs); }
  static constexpr Value EMPTY= Value_trait::EMPTY;
public:
  using Hash_value_type= typename Key_trait::Hash_value_type;

  void init()
  {
    first.set(EMPTY, true);
    second= EMPTY;
  }
  Open_address_hash()
  {
    init();
  }

  ~Open_address_hash()
  {
    if (!first.mark())
    {
      DBUG_ASSERT(hash_array);
      free(hash_array);
    }
  }

private:
  Hash_value_type to_index(const Hash_value_type &hash_value) const
  {
    return hash_value & ((1UL << capacity_power) - 1);
  }

  Hash_value_type hash_from_value(const Value &value) const
  {
    return Key_trait::get_hash_value(get_key(value));
  }

  template <typename ElemSuitsFunc, typename GetElemFunc>
  bool insert_into_bucket(const Key& key,
                          const ElemSuitsFunc &elem_suits,
                          const GetElemFunc &get_elem)
  {
    auto hash_val= to_index(Key_trait::get_hash_value(&key));

    while (!is_empty(hash_array[hash_val]))
    {
      if (elem_suits(hash_array[hash_val]))
        return false;
      hash_val= to_index(hash_val + 1);
    }

    Value &&value= get_elem();
    hash_array[hash_val]= get_elem();
    return !is_empty(value);
  }

  bool insert_into_bucket(const Value &value)
  {
    return insert_into_bucket(*get_key(value),
              [&value](const Value &rhs){ return is_equal(rhs, value); },
              [&value](){ return value; }
    );
  }

  uint rehash_subsequence(uint i)
  {
    for (uint j= to_index(i + 1); !is_empty(hash_array[j]); j= to_index(j + 1))
    {
      auto temp_el= hash_array[j];
      if (to_index(hash_from_value(temp_el)) == j)
        continue;
      hash_array[j]= EMPTY;
      insert_into_bucket(temp_el);
    }

    return i;
  }

  bool erase_from_bucket(const Value &value)
  {
    for (auto key= to_index(Key_trait::get_hash_value(get_key(value)));
         !is_empty(hash_array[key]); key= to_index(key + 1))
    {
      if (is_equal(hash_array[key], value))
      {
        hash_array[key]= EMPTY;
        rehash_subsequence(key);
        return true;
      }
    }

    return false;
  }

  bool grow(const uint new_capacity_power)
  {
    DBUG_ASSERT(new_capacity_power > capacity_power);
    size_t past_capacity= 1UL << capacity_power;
    size_t capacity= 1UL << new_capacity_power;
    capacity_power= new_capacity_power;
    hash_array= (Value *) realloc(hash_array, capacity * sizeof(Value));
    if (!hash_array)
      return false;
    bzero(hash_array + past_capacity,
          (capacity - past_capacity) * sizeof(Value*));

    for (size_t i= 0; i < capacity; i++)
    {
      if (hash_array[i] && i != to_index(hash_from_value(hash_array[i])))
      {
        auto temp_el= hash_array[i];
        hash_array[i]= EMPTY;
        insert_into_bucket(temp_el);
      }
    }
    return true;
  }

  void shrink(const uint new_capacity_power)
  {
    DBUG_ASSERT(new_capacity_power < capacity_power);
    size_t past_capacity= 1UL << capacity_power;
    size_t capacity= 1UL << new_capacity_power;
    capacity_power= new_capacity_power;

    for (size_t i= capacity; i < past_capacity; i++)
    {
      if (hash_array[i])
      {
        auto temp_el= hash_array[i];
        insert_into_bucket(temp_el);
      }
    }

    hash_array= (Value *) realloc(hash_array, capacity * sizeof(Value));
  }


  bool init_hash_array()
  {
    Value _first= first.ptr();
    Value _second= second;

    capacity_power= CAPACITY_POWER_INITIAL;
    hash_array= (Value*)calloc(1UL << capacity_power, sizeof (Value*));
    _size= 0;

    if (!insert_into_bucket(_first))
      return false;
    _size++;
    if (!insert_into_bucket(_second))
      return false;
    _size++;

    return true;
  }

public:
  Value find(const Value &elem) const
  {
    return find(*get_key(elem),
                [&elem](const Value &rhs) { return is_equal(rhs, elem); });
  }

  template <typename Func>
  Value find(const Key &key, const Func &elem_suits) const
  {
    if (likely(first.mark()))
    {
      if (!is_empty(first.ptr()) && elem_suits(first.ptr()))
        return first.ptr();
      if (!is_empty(second) && elem_suits(second))
        return second;

      return EMPTY;
    }

    for (auto idx= to_index(Key_trait::get_hash_value(&key));
         !is_empty(hash_array[idx]); idx= to_index(idx + 1))
    {
      if (elem_suits(hash_array[idx]))
        return hash_array[idx];
    }

    return EMPTY;
  }

  bool erase(const Value &value)
  {
    if (first.mark())
    {
      if (!is_empty(first.ptr()) && is_equal(first.ptr(), value))
      {
        first.set_ptr(second);
        second= EMPTY;
        return true;
      }
      else if (!is_empty(second) && is_equal(second, value))
      {
        second= EMPTY;
        return true;
      }
      return false;
    }

    const size_t capacity= 1UL << capacity_power;
    if (unlikely(capacity > 7 && (_size - 1) * LOW_LOAD_FACTOR < capacity))
      shrink(capacity_power - 1);

    if (!erase_from_bucket(value))
      return false;
    _size--;
    if (!_size)
      init();
    return true;
  }

  template <typename ElemSuitsFunc, typename GetElemFunc>
  bool insert(const Key &key,
              const ElemSuitsFunc &elem_suits,
              const GetElemFunc &get_elem)
  {
    if (first.mark())
    {
      if (is_empty(first.ptr()))
      {
        first.set_ptr(get_elem());
        return true;
      }

      if (elem_suits(first.ptr()))
        return true;

      if (is_empty(second))
      {
        second= get_elem();
        return true;
      }

      if (elem_suits(second))
        return true;

      first.set_mark(false);
      if (!init_hash_array())
        return false;

    }

    if (unlikely(_size == TABLE_SIZE_MAX))
      return false;

    bool res= true;
    const size_t capacity= 1UL << capacity_power;
    if (unlikely(((ulonglong)_size + 1) * MAX_LOAD_FACTOR > capacity))
      res= grow(capacity_power + 1);

    res= res && insert_into_bucket(key, elem_suits, get_elem);
    if (res)
      _size++;
    return res;
  };

  bool insert(const Value &value)
  {
    return insert(*get_key(value),
                  [&value](const Value &rhs){ return is_equal(rhs, value); },
                  [&value](){ return value; });
  }

  bool clear()
  {
    if (first.mark())
    {
      first.set_ptr(EMPTY);
      second= EMPTY;
      return true;
    }
    if (!hash_array)
      return false;

    free(hash_array);
    capacity_power= CAPACITY_POWER_INITIAL;

    first.set_mark(true);
    first.set_ptr(EMPTY);
    second= EMPTY;

    return true;
  }

  size_t size() const
  { 
    if (first.mark())
    {
      size_t ret_size= 0;
      if (!is_empty(first.ptr()))
        ret_size++;
      if (!is_empty(second))
        ret_size++;
      return ret_size;
    }
    else
    {
      return _size; 
    }
  }
  size_t buffer_size() const { return first.mark() ? 0 :
                                      1UL << capacity_power; }

  Open_address_hash &operator=(const Open_address_hash&)
  {
    // Do nothing. Copy operator is called by set_query_tables_list used only for backup.
    return *this;
  }
private:
  static constexpr uint CAPACITY_POWER_INITIAL= 3;
  static constexpr ulong MAX_LOAD_FACTOR= 2;
  static constexpr ulong LOW_LOAD_FACTOR= 10;
  static constexpr size_t SIZE_BITS= SIZEOF_VOIDP >= 8 ? 58 : 32;
  static constexpr size_t TABLE_SIZE_MAX= 1L << SIZE_BITS;

  class markable_reference
  {
  public:
    static constexpr uint MARK_SHIFT = 63;
    static constexpr uintptr_t MARK_MASK = 1UL << MARK_SHIFT;

    void set(Value ptr, bool mark)
    {
      uintptr_t mark_bit = static_cast<uintptr_t>(mark) << MARK_SHIFT;
      p = reinterpret_cast<uintptr_t>(ptr) | mark_bit;
    }

    void set_ptr(Value ptr)
    {
      p = reinterpret_cast<uintptr_t>(ptr) | (p & MARK_MASK);
    }

    Value ptr() const { return reinterpret_cast<Value>(p & ~MARK_MASK); }

    void set_mark(bool mark)
    {
      p = (p & ~MARK_MASK) | (static_cast<uintptr_t>(mark) << MARK_SHIFT);
    }

    bool mark() const
    {
#if SIZEOF_VOIDP >= 8
      return p & MARK_MASK;
#else
      return false; // 32-bit support: inlining is always disabled.
#endif
    }

  private:
    uintptr_t p;
  };

  union
  {
    struct
    {
      markable_reference first;
      Value second;
    };
    struct
    {
      Value *hash_array;
      uint capacity_power: 6;
      size_t _size: SIZE_BITS;
    };
  };
};

namespace traits
{

template<typename Key>
struct Open_address_hash_key_trait
{
  public:
  using Hash_value_type= ulong;

  static Hash_value_type get_hash_value(const Key *key)
  {
    ulong nr1= 1, nr2= 4;
    my_ci_hash_sort(&my_charset_bin, (uchar*) key, sizeof (Key), &nr1, &nr2);
    return (Hash_value_type) nr1;
  }
  /**
   Function returning key based on value, needed to be able to rehash the table
   on expansion. Value should be able to return Key from itself.
   The provided instantiation implements "set", i.e. Key matches Value
  */
  static Key *get_key(Key *value) { return value; }
};

template<typename Value>
struct Open_address_hash_value_trait
{
  static_assert(sizeof (Value) <= sizeof (uintptr_t),
                "The plain-type Value can only be specified for elements of a bucket size. "
                "You may have wanted to specify Value=Your_value_type*.");
  static bool is_equal(const Value &lhs, const Value &rhs)
  { return lhs == rhs; }
  /** The following two methods have to be specialized for non-scalar Value */
  static bool is_empty(const Value el)
  { return el == 0; }
};

template<typename T>
struct Open_address_hash_value_trait<T*>
{
  static bool is_equal(const T *lhs, const T *rhs)
  { return lhs == rhs; }
  static bool is_empty(const T* el) { return el == nullptr; }
  static constexpr T *EMPTY= NULL;
};

}
