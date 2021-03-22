/*****************************************************************************
Copyright (c) 2021 MariaDB Corporation.
This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.
This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA
*****************************************************************************/

#ifndef STRING_VIEW_H
#define STRING_VIEW_H

#include <cassert>
#include <cstddef>

#include <algorithm>
#include <iosfwd>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

#include "my_attribute.h"

class string_view
{
public:
  using traits_type= std::char_traits<char>;
  using value_type= char;
  using pointer= char *;
  using const_pointer= const char *;
  using reference= char &;
  using const_reference= const char &;
  using const_iterator= const char *;
  using iterator= const_iterator;
  using const_reverse_iterator= std::reverse_iterator<const_iterator>;
  using reverse_iterator= const_reverse_iterator;
  using size_type= std::size_t;
  using difference_type= std::ptrdiff_t;

  static constexpr size_type npos= size_type(-1);

  constexpr string_view() noexcept : str_(nullptr), size_(0) {}
  constexpr string_view(const string_view &rhs) noexcept= default;
  constexpr string_view(const char *s, size_type count) : str_(s), size_(count)
  {
  }
  string_view(const char *s) : str_(s), size_(traits_type::length(s)) {}
  // In C++20 it's different.
  template <class It>
  constexpr string_view(It first, It last) : str_(&*first), size_(last - first)
  {
  }
  // Add such ctor because we can't add string_view operator to std::string
  string_view(const std::string &s) noexcept : str_(s.data()), size_(s.size())
  {
  }

  string_view &operator=(const string_view &rhs)
  {
    str_= rhs.str_;
    size_= rhs.size_;
    return *this;
  }

  constexpr const_iterator begin() const noexcept { return str_; }
  constexpr const_iterator cbegin() const noexcept { return str_; }

  constexpr const_iterator end() const noexcept { return str_ + size_; }
  constexpr const_iterator cend() const noexcept { return str_ + size_; }

  const_reverse_iterator rbegin() const noexcept
  {
    return const_reverse_iterator(end());
  }
  const_reverse_iterator crbegin() const noexcept
  {
    return const_reverse_iterator(end());
  }

  const_reverse_iterator rend() const noexcept
  {
    return const_reverse_iterator(begin());
  }
  const_reverse_iterator crend() const noexcept
  {
    return const_reverse_iterator(begin());
  }

  constexpr const_reference operator[](size_type pos) const noexcept
  {
    return str_[pos];
  }

  const_reference at(size_type pos) const
  {
    if (pos >= size())
      throw std::out_of_range("string_view::at()");

    return str_[pos];
  }

  constexpr const_reference front() const noexcept { return operator[](0); }
  constexpr const_reference back() const noexcept
  {
    return operator[](size() - 1);
  }

  constexpr const_pointer data() const noexcept { return str_; }

  constexpr size_type size() const noexcept { return size_; }
  constexpr size_type length() const noexcept { return size_; }

  constexpr size_type max_size() const noexcept
  {
    return std::numeric_limits<size_type>::max();
  }

  constexpr __attribute__((warn_unused_result)) bool empty() const noexcept
  {
    return size() == 0;
  }

  void remove_prefix(size_type n)
  {
    assert(n <= size());
    str_+= n;
    size_-= n;
  }

  void remove_suffix(size_type n)
  {
    assert(n <= size());
    size_-= n;
  }

  void swap(string_view &rhs) noexcept
  {
    std::swap(str_, rhs.str_);
    std::swap(size_, rhs.size_);
  }

  size_type copy(char *dest, size_type count, size_type pos= 0) const
  {
    if (pos > size())
      throw std::out_of_range("string_view::copy()");

    auto rcount= std::min(size() - pos, count);
    traits_type::copy(dest, data() + pos, rcount);
    return rcount;
  }

  string_view substr(size_type pos= 0, size_type count= npos) const
  {
    if (pos > size())
      throw std::out_of_range("string_view::substr()");

    auto rcount= std::min(size() - pos, count);
    return {data() + pos, pos + rcount};
  }

  int compare(string_view v) const noexcept
  {
    auto rlen= std::min(size(), v.size());
    return traits_type::compare(data(), v.data(), rlen);
  }
  int compare(size_type pos1, size_type count1, string_view v) const
  {
    return substr(pos1, count1).compare(v);
  }
  int compare(size_type pos1, size_type count1, string_view v, size_type pos2,
              size_type count2) const
  {
    return substr(pos1, count1).compare(v.substr(pos2, count2));
  }
  int compare(const char *s) const { return compare(string_view(s)); }
  int compare(size_type pos1, size_type count1, const char *s) const
  {
    return substr(pos1, count1).compare(string_view(s));
  }
  int compare(size_type pos1, size_type count1, const char *s,
              size_type count2) const
  {
    return substr(pos1, count1).compare(string_view(s, count2));
  }

  bool starts_with(string_view sv) const noexcept
  {
    return substr(0, sv.size()) == sv;
  }
  constexpr bool starts_with(char c) const noexcept
  {
    return !empty() && traits_type::eq(front(), c);
  }
  bool starts_with(const char *s) const { return starts_with(string_view(s)); }

  bool ends_with(string_view sv) const noexcept
  {
    return size() >= sv.size() && compare(size() - sv.size(), npos, sv) == 0;
  }
  constexpr bool ends_with(char c) const noexcept
  {
    return !empty() && traits_type::eq(back(), c);
  }
  bool ends_with(const char *s) const { return ends_with(string_view(s)); }

  bool contains(string_view sv) const noexcept { return find(sv) != npos; }
  bool contains(char c) const noexcept { return find(c) != npos; }
  bool contains(const char *s) const { return find(s) != npos; }

  size_type find(string_view v, size_type pos= 0) const noexcept
  {
    // TODO: optimize with std::strstr()
    auto it= std::search(begin() + pos, end(), v.begin(), v.end());
    if (it == end())
      return npos;
    return it - begin();
  }
  size_type find(char ch, size_type pos= 0) const noexcept
  {
    return find(string_view(std::addressof(ch), 1));
  }
  size_type find(const char *s, size_type pos, size_type count) const
  {
    return find(string_view(s, count), pos);
  }
  size_type find(const char *s, size_type pos= 0) const
  {
    return find(string_view(s), pos);
  }

  size_type rfind(string_view v, size_type pos= npos) const noexcept
  {
    assert(!v.empty());

    if (empty())
      return npos;

    pos= std::min(size() - 1, pos);

    if (v.size() > pos + 1)
      return npos;

    pos-= v.size();

    for (;; --pos)
    {
      string_view tmp(str_ + pos, v.size());
      if (tmp == v)
        return pos;

      if (pos == 0)
        break;
    }

    return npos;
  }
  size_type rfind(char c, size_type pos= npos) const noexcept
  {
    return rfind(string_view(std::addressof(c), 1), pos);
  }
  size_type rfind(const char *s, size_type pos, size_type count) const
  {
    return rfind(string_view(s, count), pos);
  }
  size_type rfind(const char *s, size_type pos= npos) const
  {
    return rfind(string_view(s), pos);
  }

  size_type find_first_of(string_view v, size_type pos= 0) const noexcept
  {
    // TODO: optimize with a lookup table.
    auto it= std::find_if(begin() + pos, end(),
                          [v](char c) { return v.find(c) != npos; });
    if (it == end())
      return npos;
    return it - begin();
  }
  size_type find_first_of(char c, size_type pos= 0) const noexcept
  {
    return find_first_of(string_view(std::addressof(c), 1), pos);
  }
  size_type find_first_of(const char *s, size_type pos, size_type count) const
  {
    return find_first_of(string_view(s, count), pos);
  }
  size_type find_first_of(const char *s, size_type pos= 0) const
  {
    return find_first_of(string_view(s), pos);
  }

  size_type find_last_of(string_view v, size_type pos= npos) const noexcept
  {
    // TODO: optimize with a lookup table.
    auto it= std::find_if(reverse_iterator(begin() + pos), rend(),
                          [v](char c) { return v.find(c) != npos; });
    if (it == rend())
      return npos;
    return it.base() - begin();
  }
  size_type find_last_of(char c, size_type pos= npos) const noexcept
  {
    return find_last_of(string_view(std::addressof(c), 1), pos);
  }
  size_type find_last_of(const char *s, size_type pos, size_type count) const
  {
    return find_last_of(string_view(s, count), pos);
  }
  size_type find_last_of(const char *s, size_type pos= npos) const
  {
    return find_last_of(string_view(s), pos);
  }

  size_type find_first_not_of(string_view v, size_type pos= 0) const noexcept
  {
    // TODO: optimize with a lookup table.
    auto it= std::find_if(begin() + pos, end(),
                          [v](char c) { return v.find(c) == npos; });
    if (it == end())
      return npos;
    return it - begin();
  }
  size_type find_first_not_of(char c, size_type pos= 0) const noexcept
  {
    return find_first_not_of(string_view(std::addressof(c), 1), pos);
  }
  size_type find_first_not_of(const char *s, size_type pos,
                              size_type count) const
  {
    return find_first_not_of(string_view(s, count), pos);
  }
  size_type find_first_not_of(const char *s, size_type pos= 0) const
  {
    return find_first_not_of(string_view(s), pos);
  }

  size_type find_last_not_of(string_view v, size_type pos= npos) const noexcept
  {
    // TODO: optimize with a lookup table.
    auto it= std::find_if(reverse_iterator(begin() + pos), rend(),
                          [v](char c) { return v.find(c) == npos; });
    if (it == rend())
      return npos;
    return it.base() - begin();
  }
  size_type find_last_not_of(char c, size_type pos= npos) const noexcept
  {
    return find_last_not_of(string_view(std::addressof(c), 1), pos);
  }
  size_type find_last_not_of(const char *s, size_type pos,
                             size_type count) const
  {
    return find_last_not_of(string_view(s, count), pos);
  }
  size_type find_last_not_of(const char *s, size_type pos= npos) const
  {
    return find_last_not_of(string_view(s), pos);
  }

  friend bool operator==(string_view lhs, string_view rhs) noexcept
  {
    return lhs.compare(rhs) == 0;
  }
  friend bool operator!=(string_view lhs, string_view rhs) noexcept
  {
    return lhs.compare(rhs) != 0;
  }
  friend bool operator<(string_view lhs, string_view rhs) noexcept
  {
    return lhs.compare(rhs) < 0;
  }
  friend bool operator<=(string_view lhs, string_view rhs) noexcept
  {
    return lhs.compare(rhs) <= 0;
  }
  friend bool operator>(string_view lhs, string_view rhs) noexcept
  {
    return lhs.compare(rhs) > 0;
  }
  friend bool operator>=(string_view lhs, string_view rhs) noexcept
  {
    return lhs.compare(rhs) >= 0;
  }

private:
  const_pointer str_= nullptr;
  size_type size_= 0;
};

std::basic_ostream<char> &operator<<(std::basic_ostream<char> &os,
                                     string_view v);

namespace std
{

template <> struct hash<string_view>
{
  size_t operator()(string_view v)
  {
    uint32_t hash= 0;

    for (char c : v)
      hash= (hash * 2166136261u) ^ static_cast<uint32_t>(c);

    return static_cast<size_t>(hash);
  }
};

} // namespace std

#endif
