/* Copyright (c) 2016, MariaDB Corporation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include "mysys_priv.h"

my_bool my_may_have_atomic_write= 0;

#ifdef __linux__

my_bool has_shannon_atomic_write= 0, has_fusion_io_atomic_write= 0;

#include <sys/ioctl.h>


/***********************************************************************
  FUSION_IO
************************************************************************/

/** FusionIO atomic write control info */
#define DFS_IOCTL_ATOMIC_WRITE_SET      _IOW(0x95, 2, uint)


/**
   Check if the system has a funsion_io card
   @return TRUE   Card exists
*/

static my_bool test_if_fusion_io_card_exists()
{
  /* Fusion card requires fallocate to exists */
#ifndef HAVE_POSIX_FALLOCATE
  return 0;
#else
  return (access("/dev/fcta", F_OK)) == 0;
#endif
}


/**
   Check if a file is on a Fusion_IO device and that it supports atomic_write
   @param[in] file              OS file handle
   @param[in] page_size         page size
   @return TRUE                 Atomic write supported
*/

static my_bool fusion_io_has_atomic_write(File file, int page_size)
{
  int atomic= 1;
  if (page_size <= 32768 &&
      ioctl(file, DFS_IOCTL_ATOMIC_WRITE_SET, &atomic) != -1)
    return(TRUE);
  return(FALSE);
}


/***********************************************************************
  SHANNON
************************************************************************/

#define SHANNON_IOMAGIC 'x'
#define SHANNON_IOCQATOMIC_SIZE _IO(SHANNON_IOMAGIC, 22)

#define SHANNON_MAX_DEVICES 32
#define SHANNON_NO_ATOMIC_SIZE_YET -2

struct shannon_dev
{
  char dev_name[32];
  dev_t st_dev;
  int atomic_size;
};


static struct shannon_dev shannon_devices[SHANNON_MAX_DEVICES+1];

/**
   Check if the system has a Shannon card
   If card exists, record device numbers to allow us to later check if
   a given file is on this device.
   @return TRUE   Card exists
*/

static my_bool test_if_shannon_card_exists()
{
  uint shannon_found_devices= 0;
  char dev_part;
  uint dev_no;

  if (access("/dev/scta", F_OK) < 0)
    return 0;

  /*
    The Shannon devices are /dev/dfX, where X can be from a-z.
    We have to check all of them as some may be missing if the user
    removed one with the U.2 interface.
  */

  for (dev_part= 'a' ; dev_part < 'z' ; dev_part++)
  {
    char path[32];
    struct stat stat_buff;

    sprintf(path, "/dev/df%c", dev_part);
#ifdef TEST_SHANNON
    if (lstat(path, &stat_buff) < 0)
    {
      printf("%s(): lstat failed.\n", __func__);
      break;
    }
#endif
    shannon_devices[shannon_found_devices].st_dev= stat_buff.st_rdev;
    sprintf(shannon_devices[shannon_found_devices].dev_name, "/dev/sct%c",
            dev_part);

#ifdef TEST_SHANNON
    printf("%s(): i=%d, stat_buff.st_dev=0x%lx, stat_buff.st_rdev=0x%lx, st_rdev=0x%lx, dev_name=%s\n",
           __func__,
           shannon_found_devices,
           (ulong) stat_buff.st_dev,
           (ulong) stat_buff.st_rdev,
           (ulong) shannon_devices[shannon_found_devices].st_dev,
           shannon_devices[shannon_found_devices].dev_name);
#endif

    /*
      The atomic size will be checked on first access. This is needed
      as a normal user can't open the /dev/scta file
    */
    shannon_devices[shannon_found_devices].atomic_size=
      SHANNON_NO_ATOMIC_SIZE_YET;
    if (++shannon_found_devices== SHANNON_MAX_DEVICES)
      goto end;

    for (dev_no= 1 ; dev_no < 9 ; dev_no++)
    {
      sprintf(path, "/dev/df%c%d", dev_part, dev_no);
      if (lstat(path, &stat_buff) < 0)
        break;

      shannon_devices[shannon_found_devices].st_dev= stat_buff.st_rdev;
      sprintf(shannon_devices[shannon_found_devices].dev_name, "/dev/sct%c%d",
              dev_part, dev_no);

#ifdef TEST_SHANNON
      printf("%s(): i=%d, st_dev=0x%lx, st_rdev=0x%lx, dev_name=%s\n",
             __func__,
             shannon_found_devices,
             (ulong) stat_buff.st_dev,
             (ulong) shannon_devices[shannon_found_devices].st_dev,
             shannon_devices[shannon_found_devices].dev_name);
#endif

      /*
        The atomic size will be checked on first access. This is needed
        as a normal user can't open the /dev/scta file
      */
      shannon_devices[shannon_found_devices].atomic_size=
        SHANNON_NO_ATOMIC_SIZE_YET;
      if (++shannon_found_devices == SHANNON_MAX_DEVICES)
        goto end;
    }
  }
end:
  shannon_devices[shannon_found_devices].st_dev= 0;
  return shannon_found_devices > 0;
}


static my_bool shannon_dev_has_atomic_write(struct shannon_dev *dev,
                                            int page_size)
{
#ifdef TEST_SHANNON
  printf("%s: enter: page_size=%d, atomic_size=%d, dev_name=%s\n",
          __func__,
          page_size,
          dev->atomic_size,
          dev->dev_name);
#endif
  if (dev->atomic_size == SHANNON_NO_ATOMIC_SIZE_YET)
  {
    int fd= open(dev->dev_name, 0);
    if (fd < 0)
    {
      perror("open() failed!");
      dev->atomic_size= 0;                      /* Don't try again */
      return FALSE;
    }
     dev->atomic_size= ioctl(fd, SHANNON_IOCQATOMIC_SIZE);
     close(fd);
  }

#ifdef TEST_SHANNON
  printf("%s: exit: page_size=%d, atomic_size=%d, dev_name=%s\n",
          __func__,
          page_size,
          dev->atomic_size,
          dev->dev_name);
#endif
  return (page_size <= dev->atomic_size);
}


/**
   Check if a file is on a Shannon device and that it supports atomic_write
   @param[in] file              OS file handle
   @param[in] page_size         page size
   @return TRUE                 Atomic write supported

   @notes
   This is called only at first open of a file.  In this case it's doesn't
   matter so much that we loop over all cards.
   We update the atomic size on first access.
*/

static my_bool shannon_has_atomic_write(File file, int page_size)
{
  struct shannon_dev *dev;
  struct stat stat_buff;

  if (fstat(file, &stat_buff) < 0)
  {
#ifdef TEST_SHANNON
    printf("%s(): fstat failed\n", __func__);
#endif
    return 0;
  }

#ifdef TEST_SHANNON
  printf("%s(): st_dev=0x%lx, st_rdev=0x%lx\n", __func__,
         (ulong) stat_buff.st_dev, (ulong) stat_buff.st_rdev);
#endif

  for (dev= shannon_devices ; dev->st_dev; dev++)
  {
#ifdef TEST_SHANNON
    printf("%s(): st_rdev=0x%lx\n", __func__, (ulong) dev->st_dev);
#endif
    if (stat_buff.st_dev == dev->st_dev)
      return shannon_dev_has_atomic_write(dev, page_size);
 }
 return 0;
}


/***********************************************************************
  Generic atomic write code
************************************************************************/

/*
  Initalize automic write sub systems.
  Checks if we have any devices that supports atomic write
*/

void my_init_atomic_write(void)
{
  if ((has_shannon_atomic_write=   test_if_shannon_card_exists()) ||
      (has_fusion_io_atomic_write= test_if_fusion_io_card_exists()))
  my_may_have_atomic_write= 1;
#ifdef TEST_SHANNON
  printf("%s(): has_shannon_atomic_write=%d, my_may_have_atomic_write=%d\n",
          __func__,
          has_shannon_atomic_write,
          my_may_have_atomic_write);
#endif
}


/**
  Check if a file supports atomic write

  @return FALSE   No atomic write support
          TRUE    File supports atomic write
*/

my_bool my_test_if_atomic_write(File handle, int page_size)
{
#ifdef TEST_SHANNON
  printf("%s(): has_shannon_atomic_write=%d, my_may_have_atomic_write=%d\n",
          __func__,
          has_shannon_atomic_write,
          my_may_have_atomic_write);
#endif
  if (!my_may_have_atomic_write)
    return 0;
  if (has_shannon_atomic_write &&
      shannon_has_atomic_write(handle, page_size))
    return 1;

  if (has_fusion_io_atomic_write &&
      fusion_io_has_atomic_write(handle, page_size))
    return 1;

  return 0;
}

#ifdef TEST_SHANNON
int main()
{
  int fd, ret;

  my_init_atomic_write();
  fd= open("/u01/1.file", O_RDWR);
  ret= my_test_if_atomic_write(fd, 4096);
  if (ret)
    printf("support atomic_write\n");
  else
    printf("do not support atomic_write\n");
  close(fd);
  return 0;
}
#endif


#else /* __linux__ */

/* Dummy functions to provide the interfaces for other systems */

void my_init_atomic_write(void)
{
}
#endif /* __linux__ */
