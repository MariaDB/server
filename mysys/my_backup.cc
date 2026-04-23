#include "my_global.h"
#include "my_backup.h"

#ifdef __APPLE__
# include <sys/attr.h>
# include <sys/clonefile.h>
# include <copyfile.h>
#elif defined __linux__
# include <sys/sendfile.h>
#elif !defined _WIN32
# include <sys/mman.h>
#endif

namespace
{
#if !(defined __APPLE__ || defined _WIN32)

using copying_step= ssize_t(int,int,size_t,off_t*);
template<copying_step step>
static ssize_t copy(int in_fd, int out_fd, off_t c) noexcept
{
  ssize_t ret;
  for (off_t offset{0};;)
  {
    off_t count= c;
    if (count > INT_MAX >> 20 << 20)
      count = INT_MAX >> 20 << 20;
    ret= step(in_fd, out_fd, size_t(count), &offset);
    if (ret < 0)
      break;
    c-= ret;
    if (!c)
      return 0;
    if (!ret)
      return -1;
  }
  return ret;
}
# if defined __linux__ || defined __FreeBSD__
/* Copy between files in a single (type of) file system */
static inline ssize_t
copy_step(int in_fd, int out_fd, size_t count, off_t *offset) noexcept
{
  return copy_file_range(in_fd, offset, out_fd, nullptr, count, 0);
}
#  define cfr(src,dst,size) copy<copy_step>(src, dst, size)
# endif
# ifdef __linux__
/* Copy a file to a stream or to a regular file. */
static inline ssize_t
send_step(int in_fd, int out_fd, size_t count, off_t *offset) noexcept
{
  return sendfile(out_fd, in_fd, offset, count);
}
# else
/** Copy a file using a memory mapping.
@param in_fd   source file
@param out_fd  destination
@param count   number of bytes to copy
@return error code
@retval 0  on success
@retval 1  if a memory mapping failed */
static ssize_t mmap_copy(int in_fd, int out_fd, off_t count)
{
#if SIZEOF_SIZE_T < 8
  if (count != ssize_t(count))
    return 1;
#endif
  void *p= mmap(nullptr, count, PROT_READ, MAP_SHARED, in_fd, 0);
  if (p == MAP_FAILED)
    return 1;
  ssize_t ret;
  size_t c= size_t(count);
  for (const char *b= static_cast<const char*>(p);; b+= ret)
  {
    ret= write(out_fd, b, std::min(c, size_t(INT_MAX >> 20 << 20)));
    if (ret < 0)
      break;
    c-= ret;
    if (!c)
    {
      ret= 0;
      break;
    }
    if (!ret)
    {
      ret= -1;
      break;
    }
  }
  munmap(p, count);
  return ret;
}

static ssize_t pread_write(int in_fd, int out_fd, off_t count) noexcept
{
  constexpr size_t READ_WRITE_SIZE= 65536;
  char *b= static_cast<char*>(aligned_malloc(READ_WRITE_SIZE, 4096));
  if (!b)
    return -1;
  ssize_t ret;
  for (off_t o= 0;; o+= ret)
  {
    ret= pread(in_fd, b, ssize_t(std::min(count, off_t{READ_WRITE_SIZE})), o);
    if (ret > 0)
      ret= write(out_fd, b, ret);
    if (ret < 0)
      break;
    count-= ret;
    if (!count)
    {
      ret= 0;
      break;
    }
    if (!ret)
    {
      ret= -1;
      break;
    }
  }
  aligned_free(b);
  return ret;
}
# endif
#endif
}

namespace backup
{

#ifndef _WIN32
/** Copy a file.
@param src  source file descriptor
@param dst  target to append src to
@return error code (negative)
@retval 0   on success */
int copy_file(int src, int dst) noexcept
{
#ifdef __APPLE__
  return fcopyfile(src, dst, nullptr, COPYFILE_ALL | COPYFILE_CLONE);
#else
  return copy_file(src, dst, lseek(src, 0, SEEK_END));
#endif
}

/** Copy a file.
@param src  source file descriptor
@param dst  target to append src to
@param size amount of data to be copied
@return error code (negative)
@retval 0   on success */
int copy_file(int src, int dst, off_t size) noexcept
{
#ifdef __APPLE__
  return fcopyfile(src, dst, nullptr, COPYFILE_ALL | COPYFILE_CLONE);
#else
  ssize_t ret;
# ifdef cfr
  if (!(ret= cfr(src, dst, size)))
    return int(ret);
#  ifdef __linux__
  if (errno == EOPNOTSUPP)
#  endif
# endif
# ifdef __linux__ // starting with Linux 2.6.33, we can rely on sendfile(2)
    ret= copy<send_step>(src, dst, size);
# else
  if ((ret= mmap_copy(src, dst, size)) == 1)
    ret= pread_write(src, dst, size);
# endif
  DBUG_ASSERT(ret <= 0);
  return int(ret);
#endif
}
#endif
}