/*****************************************************************************

Copyright (c) 2021, MariaDB Corporation.

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

#pragma once

#if defined __powerpc64__
#elif defined __s390__
#elif defined _MSC_VER && (defined _M_IX86 || defined _M_X64) && !defined(__clang__)
#elif defined __GNUC__ && (defined __i386__ || defined __x86_64__)
# if __GNUC__ >= 8
# elif defined __clang_major__ && __clang_major__ > 6
# else
#  define NO_ELISION
# endif
#else /* Transactional memory has not been implemented for this ISA */
# define NO_ELISION
#endif

#ifdef NO_ELISION
constexpr bool have_transactional_memory= false;
# ifdef UNIV_DEBUG
static inline bool xtest() { return false; }
# endif
# define TRANSACTIONAL_TARGET /* nothing */
# define TRANSACTIONAL_INLINE /* nothing */
#else
# if defined __i386__||defined __x86_64__||defined _M_IX86||defined _M_X64
extern bool have_transactional_memory;
bool transactional_lock_enabled();

#  include <immintrin.h>
#  if defined __GNUC__ && !defined __INTEL_COMPILER
#   define TRANSACTIONAL_TARGET __attribute__((target("rtm"),hot))
#   define TRANSACTIONAL_INLINE __attribute__((target("rtm"),hot,always_inline))
#  else
#   define TRANSACTIONAL_TARGET /* nothing */
#   define TRANSACTIONAL_INLINE /* nothing */
#  endif

TRANSACTIONAL_INLINE static inline bool xbegin()
{
  return have_transactional_memory && _xbegin() == _XBEGIN_STARTED;
}

#  ifdef UNIV_DEBUG
#   ifdef __GNUC__
/** @return whether a memory transaction is active */
bool xtest();
#   else
static inline bool xtest() { return have_transactional_memory && _xtest(); }
#   endif
#  endif

TRANSACTIONAL_INLINE static inline void xabort() { _xabort(0); }

TRANSACTIONAL_INLINE static inline void xend() { _xend(); }
# elif defined __powerpc64__ || defined __s390__
extern bool have_transactional_memory;
bool transactional_lock_enabled();
#   define TRANSACTIONAL_TARGET __attribute__((hot))
#   define TRANSACTIONAL_INLINE __attribute__((hot,always_inline))

/**
  Newer gcc compilers only provide __builtin_{htm}
  functions when the -mhtm CFLAG is actually provided. So
  we've got the option of including it globally, or
  pushing down the inclusion of htmxlintrin.h to one
  file with -mhtm enabled and removing the inline
  optimization.

  Per FIXME in s390x's htmxlintrin.h, the __TM_simple_begin
  isn't always_inline resulting in duplicate definitions if
  it where included more than once.  While xabort and xend
  could be implemented here, we keep the implementation the
  same as ppc64.
 */
TRANSACTIONAL_TARGET bool xbegin();
TRANSACTIONAL_TARGET void xabort();
TRANSACTIONAL_TARGET void xend();
#  ifdef UNIV_DEBUG
bool xtest();
#  endif

# endif
#endif

template<class mutex>
class transactional_lock_guard
{
  mutex &m;

public:
  TRANSACTIONAL_INLINE transactional_lock_guard(mutex &m) : m(m)
  {
#ifndef NO_ELISION
    if (xbegin())
    {
      if (was_elided())
        return;
      xabort();
    }
#endif
    m.lock();
  }
  transactional_lock_guard(const transactional_lock_guard &)= delete;
  TRANSACTIONAL_INLINE ~transactional_lock_guard()
  {
#ifndef NO_ELISION
    if (was_elided()) xend(); else
#endif
    m.unlock();
  }

#ifndef NO_ELISION
  bool was_elided() const noexcept { return !m.is_locked_or_waiting(); }
#else
  bool was_elided() const noexcept { return false; }
#endif
};

template<class mutex>
class transactional_shared_lock_guard
{
  mutex &m;
#ifndef NO_ELISION
  bool elided;
#else
  static constexpr bool elided= false;
#endif

public:
  TRANSACTIONAL_INLINE transactional_shared_lock_guard(mutex &m) : m(m)
  {
#ifndef NO_ELISION
    if (xbegin())
    {
      if (!m.is_write_locked())
      {
        elided= true;
        return;
      }
      xabort();
    }
    elided= false;
#endif
    m.lock_shared();
  }
  transactional_shared_lock_guard(const transactional_shared_lock_guard &)=
    delete;
  TRANSACTIONAL_INLINE ~transactional_shared_lock_guard()
  {
#ifndef NO_ELISION
    if (was_elided()) xend(); else
#endif
    m.unlock_shared();
  }

  bool was_elided() const noexcept { return elided; }
};
