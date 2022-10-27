/*
  Copyright(C) 2015-2017 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#pragma once

#ifdef WIN32
# ifdef __cplusplus
#  define grn_memcpy(dest, src, n) ::memcpy_s((dest), (n), (src), (n))
# else /* __cplusplus */
#  define grn_memcpy(dest, src, n) memcpy_s((dest), (n), (src), (n))
# endif /* __cplusplus */
#else /* WIN32 */
# ifdef __cplusplus
#  define grn_memcpy(dest, src, n) std::memcpy((dest), (src), (n))
# else /* __cplusplus */
#  define grn_memcpy(dest, src, n) memcpy((dest), (src), (n))
# endif /* __cplusplus */
#endif /* WIN32 */

#ifdef WIN32
# define grn_memmove(dest, src, n) memmove_s((dest), (n), (src), (n))
#else /* WIN32 */
# define grn_memmove(dest, src, n) memmove((dest), (src), (n))
#endif /* WIN32 */

#define GRN_ENV_BUFFER_SIZE 1024

#ifdef WIN32
# define grn_getenv(name, dest, dest_size) do {                         \
    char *dest_ = (dest);                                               \
    size_t dest_size_ = (dest_size);                                    \
    if (dest_size_ > 0) {                                               \
      DWORD env_size;                                                   \
      env_size = GetEnvironmentVariableA((name), dest_, dest_size_);    \
      if (env_size == 0 || env_size > dest_size_) {                     \
        dest_[0] = '\0';                                                \
      }                                                                 \
    }                                                                   \
  } while (0)
#else /* WIN32 */
# define grn_getenv(name, dest, dest_size) do {         \
    const char *env_value = getenv((name));             \
    char *dest_ = (dest);                               \
    size_t dest_size_ = (dest_size);                    \
    if (dest_size_ > 0) {                               \
      if (env_value) {                                  \
        strncpy(dest_, env_value, dest_size_ - 1);      \
      } else {                                          \
        dest_[0] = '\0';                                \
      }                                                 \
    }                                                   \
  } while (0)
#endif /* WIN32 */

#ifdef WIN32
# define grn_fopen(name, mode) _fsopen((name), (mode), _SH_DENYNO)
#else /* WIN32 */
# define grn_fopen(name, mode) fopen((name), (mode))
#endif /* WIN32 */

#ifdef WIN32
# define grn_strdup_raw(string) _strdup((string))
#else /* WIN32 */
# define grn_strdup_raw(string) strdup((string))
#endif /* WIN32 */

#ifdef WIN32
# define grn_unlink(filename) _unlink((filename))
#else /* WIN32 */
# define grn_unlink(filename) unlink((filename))
#endif /* WIN32 */

#ifdef WIN32
# define grn_strncat(dest, dest_size, src, n)   \
  strncat_s((dest), (dest_size), (src), (n))
#else /* WIN32 */
# define grn_strncat(dest, dest_size, src, n)   \
  strncat((dest), (src), (n))
#endif /* WIN32 */

#ifdef WIN32
# define grn_strcpy(dest, dest_size, src)       \
  strcpy_s((dest), (dest_size), (src))
#else /* WIN32 */
# define grn_strcpy(dest, dest_size, src)       \
  strcpy((dest), (src))
#endif /* WIN32 */

#ifdef WIN32
# define grn_strncpy(dest, dest_size, src, n)   \
  strncpy_s((dest), (dest_size), (src), (n))
#else /* WIN32 */
# define grn_strncpy(dest, dest_size, src, n)   \
  strncpy((dest), (src), (n))
#endif /* WIN32 */

#ifdef WIN32
# define grn_strcat(dest, dest_size, src)       \
  strcat_s((dest), (dest_size), (src))
#else /* WIN32 */
# define grn_strcat(dest, dest_size, src)       \
  strcat((dest), (src))
#endif /* WIN32 */

#ifdef WIN32
# define grn_strcasecmp(string1, string2)       \
  _stricmp((string1), (string2))
#else /* WIN32 */
# define grn_strcasecmp(string1, string2)       \
  strcasecmp((string1), (string2))
#endif /* WIN32 */

#ifdef WIN32
# define grn_strncasecmp(string1, string2, n)   \
  _strnicmp((string1), (string2), (n))
#else /* WIN32 */
# define grn_strncasecmp(string1, string2, n)   \
  strncasecmp((string1), (string2), (n))
#endif /* WIN32 */

#ifdef WIN32
# define grn_snprintf(dest, dest_size, n, ...) do {             \
    _snprintf_s((dest), (dest_size), (n) - 1, __VA_ARGS__);     \
  } while (GRN_FALSE)
#else /* WIN32 */
# define grn_snprintf(dest, dest_size, n, ...)  \
  snprintf((dest), (n), __VA_ARGS__)
#endif /* WIN32 */

#ifdef WIN32
# define grn_vsnprintf(dest, dest_size, format, args) do {      \
    vsnprintf((dest), (dest_size), (format), (args));           \
    (dest)[(dest_size) - 1] = '\0';                             \
  } while (GRN_FALSE)
#else /* WIN32 */
# define grn_vsnprintf(dest, dest_size, format, args)           \
  vsnprintf((dest), (dest_size), (format), (args))
#endif /* WIN32 */

#ifdef WIN32
# define grn_write(fd, buf, count) _write((fd), (buf), (count))
#else /* WIN32 */
# define grn_write(fd, buf, count) write((fd), (buf), (count))
#endif /* WIN32 */

#ifdef WIN32
# define grn_read(fd, buf, count) _read((fd), (buf), (count))
#else /* WIN32 */
# define grn_read(fd, buf, count) read((fd), (buf), (count))
#endif /* WIN32 */

#ifdef WIN32
# define GRN_OPEN_CREATE_MODE (_S_IREAD | _S_IWRITE)
# define GRN_OPEN_FLAG_BINARY _O_BINARY
# define grn_open(fd, pathname, flags)                                  \
  _sopen_s(&(fd), (pathname), (flags), _SH_DENYNO, GRN_OPEN_CREATE_MODE)
#else /* WIN32 */
# define GRN_OPEN_CREATE_MODE (S_IRUSR | S_IWUSR | S_IRGRP)
# define GRN_OPEN_FLAG_BINARY 0
# define grn_open(fd, pathname, flags)                          \
  (fd) = open((pathname), (flags), GRN_OPEN_CREATE_MODE)
#endif /* WIN32 */

#ifdef WIN32
# define grn_close(fd) _close((fd))
#else /* WIN32 */
# define grn_close(fd) close((fd))
#endif /* WIN32 */

#ifdef WIN32
# define grn_fileno(stream) _fileno((stream))
#else /* WIN32 */
# define grn_fileno(stream) fileno((stream))
#endif /* WIN32 */

#ifdef WIN32
# define grn_isatty(stream) _isatty((stream))
#else /* WIN32 */
# define grn_isatty(stream) isatty((stream))
#endif /* WIN32 */

#ifdef WIN32
# define grn_getpid() _getpid()
#else /* WIN32 */
# define grn_getpid() getpid()
#endif /* WIN32 */
