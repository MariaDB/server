/* Copyright (c) 2016, 2021, MariaDB Corporation.

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

my_bool my_may_have_atomic_write= IF_WIN(1,0);

#ifdef __linux__

my_bool has_shannon_atomic_write, has_fusion_io_atomic_write,
        has_sfx_atomic_write;
my_bool has_sfx_card;

#include <sys/ioctl.h>

/* Linux seems to allow up to 15 partitions per block device.
Partition number 0 is the whole block device. */
# define SAME_DEV(fs_dev, blk_dev) \
  (fs_dev == blk_dev) || ((fs_dev & ~15U) == blk_dev)

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
    if (stat(path, &stat_buff) < 0)
    {
      printf("%s(): stat %s failed.\n", __func__, path);
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
      if (stat(path, &stat_buff) < 0)
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
      fprintf(stderr, "Unable to determine if atomic writes are supported:"
              " open(\"%s\"): %m\n", dev->dev_name);
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
   This is called only at first open of a file.  In this case it doesn't
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
    if (SAME_DEV(stat_buff.st_dev, dev->st_dev))
      return shannon_dev_has_atomic_write(dev, page_size);
 }
 return 0;
}


/***********************************************************************
  ScaleFlux
************************************************************************/

#define SFX_GET_ATOMIC_SIZE          _IOR('N', 0x243, int)
#define SFX_MAX_DEVICES              (32)
#define SFX_UNKNOWN_ATOMIC_WRITE_YET (-2)
#define SFX_MAX_ATOMIC_SIZE          (256 * 1024)

#define SFX_GET_SPACE_RATIO          _IO('N', 0x244)
#define SFX_UNKNOWN_PUNCH_HOLE_YET   (-3)

/**
  Threshold for logical_space / physical_space
  No less than the threshold means we can disable hole punching
*/
#define SFX_DISABLE_PUNCH_HOLE_RATIO (2)

struct sfx_dev
{
  char dev_name[32];
  dev_t st_dev;
  int atomic_write;
  int disable_punch_hole;
};

static struct sfx_dev sfx_devices[SFX_MAX_DEVICES + 1];

/**
   Check if the system has a ScaleFlux card
   If card exists, record device numbers to allow us to later check if
   a given file is on this device
   Variables for atomic_write and disable_punch_hole will be initialized
   @return TRUE   Card exists
*/

static my_bool test_if_sfx_card_exists()
{
  uint sfx_found_devices = 0;
  uint dev_num;

  for (dev_num = 0; dev_num < SFX_MAX_DEVICES; dev_num++)
  {
    struct stat stat_buff;

    sprintf(sfx_devices[sfx_found_devices].dev_name, "/dev/sfdv%dn1",
            dev_num);
    if (stat(sfx_devices[sfx_found_devices].dev_name, &stat_buff) < 0)
      break;

    sfx_devices[sfx_found_devices].st_dev= stat_buff.st_rdev;
    /*
      The atomic size will be checked on first access. This is needed
      as a normal user can't open the /dev/sfdvXn1 file
    */
    sfx_devices[sfx_found_devices].atomic_write= SFX_UNKNOWN_ATOMIC_WRITE_YET;
    sfx_devices[sfx_found_devices].disable_punch_hole=
      SFX_UNKNOWN_PUNCH_HOLE_YET;
    if (++sfx_found_devices == SFX_MAX_DEVICES)
      goto end;
  }
end:
  sfx_devices[sfx_found_devices].st_dev= 0;
  has_sfx_card = (sfx_found_devices > 0);

  return sfx_found_devices > 0;
}

static my_bool sfx_dev_has_atomic_write(struct sfx_dev *dev,
                                            int page_size)
{
  int result= -1, max_atomic_size= SFX_MAX_ATOMIC_SIZE;

  if (dev->atomic_write == SFX_UNKNOWN_ATOMIC_WRITE_YET)
  {
    int fd= open(dev->dev_name, 0);
    if (fd < 0)
      fprintf(stderr, "Unable to determine if atomic writes are supported:"
              " open(\"%s\"): %m\n", dev->dev_name);
    else
    {
      result= ioctl(fd, SFX_GET_ATOMIC_SIZE, &max_atomic_size);
      close(fd);
    }
    dev->atomic_write= result == 0 && page_size <= max_atomic_size;
  }

  return dev->atomic_write;
}

/**
   Check if a file is on a ScaleFlux device and that it supports atomic_write
   @param[in] file              OS file handle
   @param[in] page_size         page size
   @return TRUE                 Atomic write supported

   @notes
   This is called only at first open of a file.  In this case it doesn't
   matter so much that we loop over all cards.
   We update the atomic size on first access.
*/

static my_bool sfx_has_atomic_write(File file, int page_size)
{
  struct sfx_dev *dev;
  struct stat stat_buff;

  if (fstat(file, &stat_buff) == 0)
    for (dev= sfx_devices; dev->st_dev; dev++)
      if (SAME_DEV(stat_buff.st_dev, dev->st_dev))
        return sfx_dev_has_atomic_write(dev, page_size);
  return 0;
}

static my_bool sfx_dev_could_disable_punch_hole(struct sfx_dev *dev, File file)
{
  int result = 0;

  if (dev->disable_punch_hole == SFX_UNKNOWN_PUNCH_HOLE_YET)
  {
    int fd= open(dev->dev_name, 0);
    if (fd < 0)
    {
      fprintf(stderr, "Unable to determine if thin provisioning is used:"
              " open(\"%s\"): %m\n", dev->dev_name);
      dev->disable_punch_hole= 0;                      /* Don't try again */
      return FALSE;
    }

    /*
      Ratio left-shifts 8 (multiplies 256) inside the ioctl;
      will also add 1 to guarantee a round-up integer.
    */
    result= ioctl(fd, SFX_GET_SPACE_RATIO);
    result+= 1;
    dev->disable_punch_hole= (result >= (((double)SFX_DISABLE_PUNCH_HOLE_RATIO) * 256));
  }

  return dev->disable_punch_hole;
}

/**
   Check if a file is on a ScaleFlux device and whether it is possible to
   disable hole punch.
   @param[in] file              OS file handle
   @return TRUE                 Could disable hole punch

   @notes
   This is called only at first open of a file. In this case it's doesn't
   matter so much that we loop over all cards
*/

static my_bool sfx_could_disable_punch_hole(File file)
{
  struct sfx_dev *dev;
  struct stat stat_buff;

  if (fstat(file, &stat_buff) == 0)
    for (dev = sfx_devices; dev->st_dev; dev++)
      if (SAME_DEV(stat_buff.st_dev, dev->st_dev))
        return sfx_dev_could_disable_punch_hole(dev, file);
  return 0;
}

/***********************************************************************
  Generic atomic write code
************************************************************************/

/**
  Initialize the atomic write subsystem.
  Checks if we have any devices that supports atomic write
*/

void my_init_atomic_write(void)
{
  has_shannon_atomic_write= test_if_shannon_card_exists();
  has_fusion_io_atomic_write= test_if_fusion_io_card_exists();
  has_sfx_atomic_write= test_if_sfx_card_exists();

  my_may_have_atomic_write= has_shannon_atomic_write ||
    has_fusion_io_atomic_write || has_sfx_atomic_write;

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

  if (has_sfx_atomic_write &&
      sfx_has_atomic_write(handle, page_size))
    return 1;

  return 0;
}


/**
  Check if a file resides on thinly provisioned storage.

  @return FALSE   File cannot disable hole punch
          TRUE    File could disable hole punch
*/

my_bool my_test_if_thinly_provisioned(File handle)
{
  if (has_sfx_card && sfx_could_disable_punch_hole(handle))
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
