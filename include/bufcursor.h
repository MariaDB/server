/**

 SPDX-License-Identifier: GPL-2.0
 */

#pragma once

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** A cursor for writing to a C string buffer.
 *
 * This cursor is write-only and monotonic. It does not resize. Use it for
 * writing sequential strings
 */
typedef struct bufcursor
{
  char *pos;
  char *end;
} bufcursor;

bufcursor bcurs_new(char *start, size_t capacity);

size_t bcurs_spare_capacity(const bufcursor *curs);

size_t bcurs_ensure_spare_cap(const bufcursor *curs, size_t len);

size_t bcurs_write(bufcursor *curs, const char *format, ...);

char *bcurs_write_str(bufcursor *curs, const char *s);

char *bcurs_write_bytes(bufcursor *curs, const char *s, size_t len);

void bcurs_write_char(bufcursor *curs, char c);

void bcurs_terminate(bufcursor *curs);

/** Get the pointer to the start of this cursor's writeable buffer.
 *
 * NOTE: you should call `bcurs_ensure_spare_cap` before writing to this
 * pointer
 */
inline char *bcurs_ptr(bufcursor *curs) { return curs->pos; }

/** Seek to a relative position, usually after manually writing to it
 */
inline void bcurs_seek(bufcursor *curs, ssize_t change) { curs->pos+= change; }


#ifdef __cplusplus
}
#endif
