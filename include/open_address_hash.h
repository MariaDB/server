#pragma once

#include <cstdint>
#include <cstring>
#include "my_dbug.h"
#include "m_string.h"
#include "my_global.h"
#include "m_ctype.h"

#include "hash.h"

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

  Open_address_hash()
  {
    first.set_mark(true);
    first.set_ptr(EMPTY);
    second= EMPTY;
  }

  ~Open_address_hash()
  {
    if (!first.mark())
    {
      my_hash_free(&hash);
    }
  }

private:

  Hash_value_type hash_from_value(const Value &value) const
  {
    return Key_trait::get_hash_value(get_key(value));
  }

  inline bool insert_into_bucket(const Value &value)
  {
    return !my_hash_insert(&hash, (uchar*)value);
  };


  bool init_hash_array()
  {
    Value _first= first.ptr();
    Value _second= second;

    my_hash_init3(PSI_NOT_INSTRUMENTED, &hash, 0, &my_charset_bin, 16, 0, 0, 
                  Key_trait::get_key_compat,
                  Key_trait::hash_function_compat, NULL, 
                  (int (*)(const uchar*, const uchar*))
                  Key_trait::is_equal,
                  0);

    if (!insert_into_bucket(_first))
      return false;
    if (!insert_into_bucket(_second))
      return false;

    return true;
  }

public:
  Value find(const Value &elem) const
  {
    return find(*Key_trait::get_key(elem),
                [&elem](const Value &rhs) { return is_equal(rhs, elem); });
  }

  template <typename Func>
  Value find(const Key &key, const Func &elem_suits) const
  {
    if (first.mark())
    {
      if (first.ptr() && 0 == Key_trait::is_equal((Key*)&key, (Key*)get_key(first.ptr()))
                      && elem_suits(first.ptr()))
        return first.ptr();
      if (!is_empty(second) && 0 == Key_trait::is_equal((Key*)&key, (Key*)get_key(second))
                            && elem_suits(second))
        return second;

      return EMPTY;
    }

    HASH_SEARCH_STATE state;
    for (auto res= my_hash_first(&hash, (uchar*)&key, sizeof key, &state);
         res;
         res= my_hash_next(&hash, (uchar*)&key, sizeof key, &state))
    {
      if (elem_suits((Value)res))
        return (Value)res;
    }

    return EMPTY;
  };

  bool erase(const Value &value)
  {
    if (first.mark())
    {
      if (!is_empty(first.ptr()) && is_equal(first.ptr(), value))
      {
        first.set_ptr(EMPTY);
        return true;
      }
      else if (second && is_equal(second, value))
      {
        second= EMPTY;
        return true;
      }
      return false;
    }

    return !my_hash_delete(&hash, (uchar*)value);;
  }

  bool insert(const Value &value)
  {
    if (first.mark())
    {
      if (is_empty(first.ptr()))
      {
        if (is_equal(second, value))
          return false;
        first.set_ptr(value);
        return true;
      }
      else if (is_empty(second))
      {
        if (is_equal(first.ptr(), value))
          return false;
        second= value;
        return true;
      }
      else
      {
        first.set_mark(false);
        if (!init_hash_array())
          return false;
      }
    }

    if (unlikely(hash.blength == TABLE_SIZE_MAX))
      return false;

    return insert_into_bucket(value);
  };

  bool clear()
  {
    if (first.mark())
    {
      first.set_ptr(EMPTY);
      second= EMPTY;
      return true;
    }

    my_hash_free(&hash);

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
    return hash.records;
  }
  size_t buffer_size() const
  {
    return first.mark() ? 0 : hash.blength;
  }

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
    HASH hash;
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
  static inline Hash_value_type hash_function_compat(CHARSET_INFO *ci,
                                              const uchar *key, size_t len)
  {
    ulong nr1= 1, nr2= 4;
    my_ci_hash_sort(&my_charset_bin, (uchar*) key, sizeof (Key), &nr1, &nr2);
    return (Hash_value_type) nr1;
  };
  /**
   Function returning key based on value, needed to be able to rehash the table
   on expansion. Value should be able to return Key from itself.
   The provided instantiation implements "set", i.e. Key matches Value
  */
  static Key *get_key(Key *value) { return value; }

  const uchar* get_key_compat(const uchar* _val, 
                              size_t* size, char first)
  {
    *size= sizeof(Key);
    return _val;
  }
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
