/*
   Copyright (c) 2016, Facebook, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */
#pragma once

/* C++ standard header files */
#include <chrono>
#include <string>

/* MySQL header files */
#include "./sql_string.h"

/* RocksDB header files */
#include "rocksdb/slice.h"

#ifdef HAVE_JEMALLOC
#include <jemalloc/jemalloc.h>
#endif

namespace myrocks {

/*
  Guess what?
  An interface is a class where all members are public by default.
*/

#ifndef interface
#define interface struct
#endif // interface

/*
  Introduce C-style pseudo-namespaces, a handy way to make code more readble
  when calling into a legacy API, which does not have any namespace defined.
  Since we cannot or don't want to change the API in any way, we can use this
  mechanism to define readability tokens that look like C++ namespaces, but are
  not enforced in any way by the compiler, since the pre-compiler strips them
  out. However, on the calling side, code looks like my_core::thd_ha_data()
  rather than plain a thd_ha_data() call. This technique adds an immediate
  visible cue on what type of API we are calling into.
*/

#ifndef my_core
// C-style pseudo-namespace for MySQL Core API, to be used in decorating calls
// to non-obvious MySQL functions, like the ones that do not start with well
// known prefixes: "my_", "sql_", and "mysql_".
#define my_core
#endif // my_core

/*
  The intent behind a SHIP_ASSERT() macro is to have a mechanism for validating
  invariants in retail builds. Traditionally assertions (such as macros defined
  in <cassert>) are evaluated for performance reasons only in debug builds and
  become NOOP in retail builds when NDEBUG is defined.

  This macro is intended to validate the invariants which are critical for
  making sure that data corruption and data loss won't take place. Proper
  intended usage can be described as "If a particular condition is not true then
  stop everything what's going on and terminate the process because continued
  execution will cause really bad things to happen".

  Use the power of SHIP_ASSERT() wisely.
*/

#ifndef SHIP_ASSERT
#define SHIP_ASSERT(expr)                                                      \
  do {                                                                         \
    if (!(expr)) {                                                             \
      my_safe_printf_stderr("\nShip assert failure: \'%s\'\n", #expr);         \
      abort_with_stack_traces();                                               \
    }                                                                          \
  } while (0)
#endif // SHIP_ASSERT

/*
  Assert a implies b.
  If a is true, then b must be true.
  If a is false, then the value is b does not matter.
*/
#ifndef DBUG_ASSERT_IMP
#define DBUG_ASSERT_IMP(a, b) DBUG_ASSERT(!(a) || (b))
#endif

/*
  Assert a if and only if b.
  a and b must be both true or both false.
*/
#ifndef DBUG_ASSERT_IFF
#define DBUG_ASSERT_IFF(a, b)                                                  \
  DBUG_ASSERT(static_cast<bool>(a) == static_cast<bool>(b))
#endif

/*
  Intent behind this macro is to avoid manually typing the function name every
  time we want to add the debugging statement and use the compiler for this
  work. This avoids typical refactoring problems when one renames a function,
  but the tracing message doesn't get updated.

  We could use __func__ or __FUNCTION__ macros, but __PRETTY_FUNCTION__
  contains the signature of the function as well as its bare name and provides
  therefore more context when interpreting the logs.
*/
#define DBUG_ENTER_FUNC() DBUG_ENTER(__PRETTY_FUNCTION__)

/*
  Error handling pattern used across MySQL abides by the following rules: "All
  functions that can report an error (usually an allocation error), should
  return 0/FALSE/false on success, 1/TRUE/true on failure."

  https://dev.mysql.com/doc/internals/en/additional-suggestions.html has more
  details.

  To increase the comprehension and readability of MyRocks codebase we'll use
  constants similar to ones from C standard (EXIT_SUCCESS and EXIT_FAILURE) to
  make sure that both failure and success paths are clearly identifiable. The
  definitions of FALSE and TRUE come from <my_global.h>.
*/
#define HA_EXIT_SUCCESS FALSE
#define HA_EXIT_FAILURE TRUE

/*
  Generic constant.
*/
const size_t RDB_MAX_HEXDUMP_LEN = 1000;

/*
  Helper function to get an NULL terminated uchar* out of a given MySQL String.
*/

inline uchar *rdb_mysql_str_to_uchar_str(my_core::String *str) {
  DBUG_ASSERT(str != nullptr);
  return reinterpret_cast<uchar *>(str->c_ptr());
}

/*
  Helper function to get plain (not necessary NULL terminated) uchar* out of a
  given STL string.
*/

inline const uchar *rdb_std_str_to_uchar_ptr(const std::string &str) {
  return reinterpret_cast<const uchar *>(str.data());
}

/*
  Helper function to convert seconds to milliseconds.
*/

constexpr int rdb_convert_sec_to_ms(int sec) {
  return std::chrono::milliseconds(std::chrono::seconds(sec)).count();
}

/*
  Helper function to get plain (not necessary NULL terminated) uchar* out of a
  given RocksDB item.
*/

inline const uchar *rdb_slice_to_uchar_ptr(const rocksdb::Slice *item) {
  DBUG_ASSERT(item != nullptr);
  return reinterpret_cast<const uchar *>(item->data());
}

/*
  Call this function in cases when you can't rely on garbage collector and need
  to explicitly purge all unused dirty pages. This should be a relatively rare
  scenario for cases where it has been verified that this intervention has
  noticeable benefits.
*/
inline int purge_all_jemalloc_arenas() {
#ifdef HAVE_JEMALLOC
  unsigned narenas = 0;
  size_t sz = sizeof(unsigned);
  char name[25] = {0};

  // Get the number of arenas first. Please see `jemalloc` documentation for
  // all the various options.
  int result = mallctl("arenas.narenas", &narenas, &sz, nullptr, 0);

  // `mallctl` returns 0 on success and we really want caller to know if all the
  // trickery actually works.
  if (result) {
    return result;
  }

  // Form the command to be passed to `mallctl` and purge all the unused dirty
  // pages.
  snprintf(name, sizeof(name) / sizeof(char), "arena.%d.purge", narenas);
  result = mallctl(name, nullptr, nullptr, nullptr, 0);

  return result;
#else
  return EXIT_SUCCESS;
#endif
}

/*
  Helper functions to parse strings.
*/

const char *rdb_skip_spaces(const struct charset_info_st *const cs,
                            const char *str)
    MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

bool rdb_compare_strings_ic(const char *const str1, const char *const str2)
    MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

const char *rdb_find_in_string(const char *str, const char *pattern,
                               bool *const succeeded)
    MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

const char *rdb_check_next_token(const struct charset_info_st *const cs,
                                 const char *str, const char *const pattern,
                                 bool *const succeeded)
    MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

const char *rdb_parse_id(const struct charset_info_st *const cs,
                         const char *str, std::string *const id)
    MY_ATTRIBUTE((__nonnull__(1, 2), __warn_unused_result__));

const char *rdb_skip_id(const struct charset_info_st *const cs, const char *str)
    MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

/*
  Helper functions to populate strings.
*/

std::string rdb_hexdump(const char *data, const std::size_t data_len,
                        const std::size_t maxsize = 0)
    MY_ATTRIBUTE((__nonnull__));

/*
  Helper function to see if a database exists
 */
bool rdb_database_exists(const std::string &db_name);

} // namespace myrocks
