/*
   Copyright (c) 2020, MariaDB

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

#include <type_traits>
#include <utility>

namespace detail
{

template <typename Callable> class scope_exit
{
public:
  template <typename F>
  explicit scope_exit(F &&f) : function_(std::forward<F>(f))
  {
  }

  template <typename F>
  scope_exit(F &&f, bool engaged) : function_(std::forward<F>(f)), engaged_(engaged)
  {
  }

  scope_exit(scope_exit &&rhs)
      : function_(std::move(rhs.function_)), engaged_(rhs.engaged_)
  {
    rhs.release();
  }

  scope_exit(const scope_exit &)= delete;
  scope_exit &operator=(scope_exit &&)= delete;
  scope_exit &operator=(const scope_exit &)= delete;

  void release() { engaged_= false; }
  void engage() { assert(!engaged_); engaged_= true; }

  ~scope_exit()
  {
    if (engaged_)
      function_();
  }

private:
  Callable function_;
  bool engaged_= true;
};

} // end namespace detail

template <typename Callable>
inline
::detail::scope_exit<typename std::decay<Callable>::type>
make_scope_exit(Callable &&f, bool engaged= true)
{
  return ::detail::scope_exit<typename std::decay<Callable>::type>(
      std::forward<Callable>(f), engaged);
}

#define CONCAT_IMPL(x, y) x##y
#define CONCAT(x, y) CONCAT_IMPL(x, y)
#define ANONYMOUS_VARIABLE CONCAT(_anonymous_variable, __LINE__)

#define SCOPE_EXIT auto ANONYMOUS_VARIABLE= make_scope_exit

#define IF_CLASS(C) typename std::enable_if<std::is_class<C>::value>::type
#define IF_NOT_CLASS(C) typename std::enable_if<!std::is_class<C>::value>::type

namespace detail
{

template <typename T>
class Scope_value
{
public:
  // Use SFINAE for passing structs by reference and plain types by value.
  // This ctor is defined only if T is a class or struct:
  template <typename U = T, typename = IF_CLASS(U)>
  Scope_value(T &variable, const T &scope_value)
      : variable_(&variable), saved_value_(variable)
  {
    variable= scope_value;
  }

  // This ctor is defined only if T is NOT a class or struct:
  template <typename U = T, typename = IF_NOT_CLASS(U)>
  Scope_value(T &variable, const T scope_value)
      : variable_(&variable), saved_value_(variable)
  {
    variable= scope_value;
  }

  Scope_value(Scope_value &&rhs)
      : variable_(rhs.variable_), saved_value_(rhs.saved_value_)
  {
    rhs.variable_= NULL;
  }

  Scope_value(const Scope_value &)= delete;
  Scope_value &operator=(const Scope_value &)= delete;
  Scope_value &operator=(Scope_value &&)= delete;

  ~Scope_value()
  {
    if (variable_)
      *variable_= saved_value_;
  }

private:
  T *variable_;
  T saved_value_;
};

} // namespace detail

// Use like this:
// auto _= make_scope_value(var, tmp_value);

template <typename T, typename = IF_CLASS(T)>
inline
::detail::Scope_value<T> make_scope_value(T &variable, const T &scope_value)
{
  return ::detail::Scope_value<T>(variable, scope_value);
}

template <typename T, typename = IF_NOT_CLASS(T)>
inline
::detail::Scope_value<T> make_scope_value(T &variable, T scope_value)
{
  return ::detail::Scope_value<T>(variable, scope_value);
}

/*
  Note: perfect forwarding version can not pass const:

  template <typename T, typename U>
  inline
  detail::Scope_value<T> make_scope_value(T &variable, U &&scope_value)
  {
      return detail::Scope_value<T>(variable, std::forward<U>(scope_value));
  }

  as `const U &&` fails with error `expects an rvalue for 2nd argument`. That
  happens because const U && is treated as rvalue only (this is the exact syntax
  for declaring rvalues).
*/


#define SCOPE_VALUE auto ANONYMOUS_VARIABLE= make_scope_value
#define SCOPE_SET(VAR, MASK) auto ANONYMOUS_VARIABLE= make_scope_value(VAR, VAR | MASK)
#define SCOPE_CLEAR(VAR, MASK) auto ANONYMOUS_VARIABLE= make_scope_value(VAR, VAR & ~MASK)
