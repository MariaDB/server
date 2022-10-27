/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2011-2016 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#include "file-impl.hpp"

#include <sys/types.h>
#include <sys/stat.h>

#ifdef WIN32
# ifdef min
#  undef min
# endif  // min
# ifdef max
#  undef max
# endif  // max
#else  // WIN32
# include <fcntl.h>
# include <sys/mman.h>
# include <unistd.h>
#endif  // WIN32

#include <algorithm>
#include <limits>

/* Must be the same value as GRN_OPEN_CREATE_MODE */
#ifdef WIN32
# define GRN_IO_FILE_CREATE_MODE (GENERIC_READ | GENERIC_WRITE)
#else /* WIN32 */
# define GRN_IO_FILE_CREATE_MODE 0640
#endif /* WIN32 */

namespace grn {
namespace dat {

#ifdef WIN32

FileImpl::FileImpl()
    : ptr_(NULL),
      size_(0),
      file_(INVALID_HANDLE_VALUE),
      map_(INVALID_HANDLE_VALUE),
      addr_(NULL) {}

FileImpl::~FileImpl() {
  if (addr_ != NULL) {
    ::UnmapViewOfFile(addr_);
  }

  if (map_ != INVALID_HANDLE_VALUE) {
    ::CloseHandle(map_);
  }

  if (file_ != INVALID_HANDLE_VALUE) {
    ::CloseHandle(file_);
  }
}

#else  // WIN32

FileImpl::FileImpl()
    : ptr_(NULL),
      size_(0),
      fd_(-1),
      addr_(MAP_FAILED),
      length_(0) {}

FileImpl::~FileImpl() {
  if (addr_ != MAP_FAILED) {
    ::munmap(addr_, length_);
  }

  if (fd_ != -1) {
    ::close(fd_);
  }
}

#endif  // WIN32

void FileImpl::create(const char *path, UInt64 size) {
  GRN_DAT_THROW_IF(PARAM_ERROR, size == 0);
  GRN_DAT_THROW_IF(PARAM_ERROR,
      size > static_cast<UInt64>(std::numeric_limits< ::size_t>::max()));

  FileImpl new_impl;
  new_impl.create_(path, size);
  new_impl.swap(this);
}

void FileImpl::open(const char *path) {
  GRN_DAT_THROW_IF(PARAM_ERROR, path == NULL);
  GRN_DAT_THROW_IF(PARAM_ERROR, path[0] == '\0');

  FileImpl new_impl;
  new_impl.open_(path);
  new_impl.swap(this);
}

void FileImpl::close() {
  FileImpl new_impl;
  new_impl.swap(this);
}

#ifdef WIN32

void FileImpl::swap(FileImpl *rhs) {
  std::swap(ptr_, rhs->ptr_);
  std::swap(size_, rhs->size_);
  std::swap(file_, rhs->file_);
  std::swap(map_, rhs->map_);
  std::swap(addr_, rhs->addr_);
}

void FileImpl::flush() {
  if (!addr_) {
    return;
  }

  BOOL succeeded = ::FlushViewOfFile(addr_, static_cast<SIZE_T>(size_));
  GRN_DAT_THROW_IF(IO_ERROR, !succeeded);

  SYSTEMTIME system_time;
  GetSystemTime(&system_time);
  FILETIME file_time;
  succeeded = SystemTimeToFileTime(&system_time, &file_time);
  GRN_DAT_THROW_IF(IO_ERROR, !succeeded);

  succeeded = SetFileTime(file_, NULL, NULL, &file_time);
  GRN_DAT_THROW_IF(IO_ERROR, !succeeded);
}

void FileImpl::create_(const char *path, UInt64 size) {
  if ((path != NULL) && (path[0] != '\0')) {
    file_ = ::CreateFileA(path, GRN_IO_FILE_CREATE_MODE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    GRN_DAT_THROW_IF(IO_ERROR, file_ == INVALID_HANDLE_VALUE);

    const LONG size_low = static_cast<LONG>(size & 0xFFFFFFFFU);
    LONG size_high = static_cast<LONG>(size >> 32);
    const DWORD file_pos = ::SetFilePointer(file_, size_low, &size_high,
                                            FILE_BEGIN);
    GRN_DAT_THROW_IF(IO_ERROR, (file_pos == INVALID_SET_FILE_POINTER) &&
                               (::GetLastError() != 0));
    GRN_DAT_THROW_IF(IO_ERROR, ::SetEndOfFile(file_) == 0);

    map_ = ::CreateFileMapping(file_, NULL, PAGE_READWRITE, 0, 0, NULL);
    GRN_DAT_THROW_IF(IO_ERROR, map_ == INVALID_HANDLE_VALUE);
  } else {
    const DWORD size_low = static_cast<DWORD>(size & 0xFFFFFFFFU);
    const DWORD size_high = static_cast<DWORD>(size >> 32);

    map_ = ::CreateFileMapping(file_, NULL, PAGE_READWRITE,
                               size_high, size_low, NULL);
    GRN_DAT_THROW_IF(IO_ERROR, map_ == INVALID_HANDLE_VALUE);
  }

  addr_ = ::MapViewOfFile(map_, FILE_MAP_WRITE, 0, 0, 0);
  GRN_DAT_THROW_IF(IO_ERROR, addr_ == NULL);

  ptr_ = addr_;
  size_ = static_cast< ::size_t>(size);
}

void FileImpl::open_(const char *path) {
#ifdef _MSC_VER
  struct __stat64 st;
  GRN_DAT_THROW_IF(IO_ERROR, ::_stat64(path, &st) == -1);
#else  // _MSC_VER
  struct _stat st;
  GRN_DAT_THROW_IF(IO_ERROR, ::_stat(path, &st) == -1);
#endif  // _MSC_VER
  GRN_DAT_THROW_IF(IO_ERROR, st.st_size == 0);
  GRN_DAT_THROW_IF(IO_ERROR,
      static_cast<UInt64>(st.st_size) > std::numeric_limits< ::size_t>::max());

  file_ = ::CreateFileA(path, GRN_IO_FILE_CREATE_MODE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  GRN_DAT_THROW_IF(IO_ERROR, file_ == NULL);

  map_ = ::CreateFileMapping(file_, NULL, PAGE_READWRITE, 0, 0, NULL);
  GRN_DAT_THROW_IF(IO_ERROR, map_ == NULL);

  addr_ = ::MapViewOfFile(map_, FILE_MAP_WRITE, 0, 0, 0);
  GRN_DAT_THROW_IF(IO_ERROR, addr_ == NULL);

  ptr_ = addr_;
  size_ = static_cast< ::size_t>(st.st_size);
}

#else  // WIN32

void FileImpl::swap(FileImpl *rhs) {
  std::swap(ptr_, rhs->ptr_);
  std::swap(size_, rhs->size_);
  std::swap(fd_, rhs->fd_);
  std::swap(addr_, rhs->addr_);
  std::swap(length_, rhs->length_);
}

void FileImpl::flush() {
  if (!addr_) {
    return;
  }

  int result = ::msync(addr_, length_, MS_SYNC);
  GRN_DAT_THROW_IF(IO_ERROR, result != 0);
}

void FileImpl::create_(const char *path, UInt64 size) {
  GRN_DAT_THROW_IF(PARAM_ERROR,
      size > static_cast<UInt64>(std::numeric_limits< ::off_t>::max()));

  if ((path != NULL) && (path[0] != '\0')) {
    fd_ = ::open(path, O_RDWR | O_CREAT | O_TRUNC, GRN_IO_FILE_CREATE_MODE);
    GRN_DAT_THROW_IF(IO_ERROR, fd_ == -1);

    const ::off_t file_size = static_cast< ::off_t>(size);
    GRN_DAT_THROW_IF(IO_ERROR, ::ftruncate(fd_, file_size) == -1);
  }

#ifdef MAP_ANONYMOUS
  const int flags = (fd_ == -1) ? (MAP_PRIVATE | MAP_ANONYMOUS) : MAP_SHARED;
#else  // MAP_ANONYMOUS
  const int flags = (fd_ == -1) ? (MAP_PRIVATE | MAP_ANON) : MAP_SHARED;
#endif  // MAP_ANONYMOUS

  length_ = static_cast< ::size_t>(size);
#ifdef USE_MAP_HUGETLB
  addr_ = ::mmap(NULL, length_, PROT_READ | PROT_WRITE,
                 flags | MAP_HUGETLB, fd_, 0);
#endif  // USE_MAP_HUGETLB
  if (addr_ == MAP_FAILED) {
    addr_ = ::mmap(NULL, length_, PROT_READ | PROT_WRITE, flags, fd_, 0);
    GRN_DAT_THROW_IF(IO_ERROR, addr_ == MAP_FAILED);
  }

  ptr_ = addr_;
  size_ = length_;
}

void FileImpl::open_(const char *path) {
  struct stat st;
  GRN_DAT_THROW_IF(IO_ERROR, ::stat(path, &st) == -1);
  GRN_DAT_THROW_IF(IO_ERROR, (st.st_mode & S_IFMT) != S_IFREG);
  GRN_DAT_THROW_IF(IO_ERROR, st.st_size == 0);
  GRN_DAT_THROW_IF(IO_ERROR,
      static_cast<UInt64>(st.st_size) > std::numeric_limits< ::size_t>::max());

  fd_ = ::open(path, O_RDWR);
  GRN_DAT_THROW_IF(IO_ERROR, fd_ == -1);

  length_ = static_cast<std::size_t>(st.st_size);
  addr_ = ::mmap(NULL, length_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
  GRN_DAT_THROW_IF(IO_ERROR, addr_ == MAP_FAILED);

  ptr_ = addr_;
  size_ = length_;
}

#endif  // WIN32

}  // namespace dat
}  // namespace grn
