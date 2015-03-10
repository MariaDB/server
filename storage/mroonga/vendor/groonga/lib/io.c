/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2009-2015 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "grn.h"

#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

#include "grn_ctx.h"
#include "grn_io.h"
#include "grn_plugin.h"
#include "grn_hash.h"
#include "grn_ctx_impl.h"

#define GRN_IO_IDSTR "GROONGA:IO:00001"

#define GRN_IO_VERSION_DEFAULT 0

#define GRN_IO_FILE_SIZE_V1 1073741824UL

#ifdef WIN32
# define GRN_IO_FILE_SIZE_V0  134217728L
#else /* WIN32 */
# define GRN_IO_FILE_SIZE_V0  GRN_IO_FILE_SIZE_V1
#endif /* WIN32 */

#ifndef O_BINARY
# ifdef _O_BINARY
#  define O_BINARY _O_BINARY
# else
#  define O_BINARY 0
# endif
#endif

typedef struct _grn_io_fileinfo {
#ifdef WIN32
  HANDLE fh;
  HANDLE fmo;
  grn_critical_section cs;
#else /* WIN32 */
  int fd;
  dev_t dev;
  ino_t inode;
#endif /* WIN32 */
} fileinfo;

#define IO_HEADER_SIZE 64

static uint32_t grn_io_version_default = GRN_IO_VERSION_DEFAULT;

inline static grn_rc grn_open(grn_ctx *ctx, fileinfo *fi, const char *path, int flags);
inline static void grn_fileinfo_init(fileinfo *fis, int nfis);
inline static int grn_opened(fileinfo *fi);
inline static grn_rc grn_close(grn_ctx *ctx, fileinfo *fi);
#ifdef WIN32
inline static void * grn_mmap(grn_ctx *ctx, grn_io *io,
                              HANDLE *fmo, fileinfo *fi,
                              off_t offset, size_t length);
inline static int grn_munmap(grn_ctx *ctx, grn_io *io,
                             HANDLE *fmo, fileinfo *fi,
                             void *start, size_t length);
# define GRN_MMAP(ctx,io,fmo,fi,offset,length)\
  (grn_mmap((ctx), (io), (fmo), (fi), (offset), (length)))
# define GRN_MUNMAP(ctx,io,fmo,fi,start,length)\
  (grn_munmap((ctx), (io), (fmo), (fi), (start), (length)))
#else /* WIN32 */
inline static void * grn_mmap(grn_ctx *ctx, grn_io *io, fileinfo *fi,
                              off_t offset, size_t length);
inline static int grn_munmap(grn_ctx *ctx, grn_io *io, fileinfo *fi,
                             void *start, size_t length);
# define GRN_MUNMAP(ctx,io,fmo,fi,start,length) \
  (grn_munmap((ctx), (io), (fi), (start), (length)))
# ifdef USE_FAIL_MALLOC
inline static void * grn_fail_mmap(grn_ctx *ctx, grn_io *io, fileinfo *fi,
                                   off_t offset, size_t length,
                                   const char* file, int line, const char *func);
#  define GRN_MMAP(ctx,io,fmo,fi,offset,length)\
  (grn_fail_mmap((ctx), (io), (fi), (offset), (length),\
                 __FILE__, __LINE__, __FUNCTION__))
# else /* USE_FAIL_MALLOC */
#  define GRN_MMAP(ctx,io,fmo,fi,offset,length)\
  (grn_mmap((ctx), (io), (fi), (offset), (length)))
# endif /* USE_FAIL_MALLOC */
#endif  /* WIN32 */
inline static int grn_msync(grn_ctx *ctx, void *start, size_t length);
inline static grn_rc grn_pread(grn_ctx *ctx, fileinfo *fi, void *buf, size_t count, off_t offset);
inline static grn_rc grn_pwrite(grn_ctx *ctx, fileinfo *fi, void *buf, size_t count, off_t offset);

grn_rc
grn_io_init(void)
{
  const char *version_env;

  version_env = getenv("GRN_IO_VERSION");
  if (version_env) {
    grn_io_version_default = atoi(version_env);
  }

  return GRN_SUCCESS;
}

grn_rc
grn_io_fin(void)
{
  return GRN_SUCCESS;
}

static inline uint32_t
grn_io_compute_base(uint32_t header_size)
{
  uint32_t total_header_size;
  total_header_size = IO_HEADER_SIZE + header_size;
  return (total_header_size + grn_pagesize - 1) & ~(grn_pagesize - 1);
}

static inline uint32_t
grn_io_compute_base_segment(uint32_t base, uint32_t segment_size)
{
  return (base + segment_size - 1) / segment_size;
}

static uint32_t
grn_io_compute_max_n_files(uint32_t segment_size, uint32_t max_segment,
                           unsigned int base_segument, unsigned long file_size)
{
  uint64_t last_segment_end;
  last_segment_end = ((uint64_t)segment_size) * (max_segment + base_segument);
  return (uint32_t)((last_segment_end + file_size - 1) / file_size);
}

static inline unsigned long
grn_io_compute_file_size(uint32_t version)
{
  if (version == 0) {
    return GRN_IO_FILE_SIZE_V0;
  } else {
    return GRN_IO_FILE_SIZE_V1;
  }
}

static inline uint32_t
grn_io_max_segment(grn_io *io)
{
  if (io->header->segment_tail) {
    return io->header->segment_tail;
  } else {
    return io->header->max_segment;
  }
}

static uint32_t
grn_io_max_n_files(grn_io *io)
{
  unsigned long file_size;

  file_size = grn_io_compute_file_size(io->header->version);
  return grn_io_compute_max_n_files(io->header->segment_size,
                                    grn_io_max_segment(io),
                                    io->base_seg,
                                    file_size);
}

grn_io *
grn_io_create_tmp(uint32_t header_size, uint32_t segment_size,
                  uint32_t max_segment, grn_io_mode mode, uint32_t flags)
{
  grn_io *io;
  uint32_t b;
  struct _grn_io_header *header;
  b = grn_io_compute_base(header_size);
  header = (struct _grn_io_header *)GRN_MMAP(&grn_gctx, NULL, NULL, NULL, 0, b);
  if (header) {
    header->version = grn_io_version_default;
    header->header_size = header_size;
    header->segment_size = segment_size;
    header->max_segment = max_segment;
    header->n_arrays = 0;
    header->flags = flags;
    header->lock = 0;
    memcpy(header->idstr, GRN_IO_IDSTR, 16);
    if ((io = GRN_GMALLOCN(grn_io, 1))) {
      grn_io_mapinfo *maps = NULL;
      if ((maps = GRN_GCALLOC(sizeof(grn_io_mapinfo) * max_segment))) {
        io->header = header;
        io->user_header = (((byte *) header) + IO_HEADER_SIZE);
        io->maps = maps;
        io->base = b;
        io->base_seg = 0;
        io->mode = mode;
        io->header->curr_size = b;
        io->fis = NULL;
        io->ainfo = NULL;
        io->max_map_seg = 0;
        io->nmaps = 0;
        io->count = 0;
        io->flags = GRN_IO_TEMPORARY;
        io->lock = &header->lock;
        io->path[0] = '\0';
        return io;
      }
      GRN_GFREE(io);
    }
    GRN_MUNMAP(&grn_gctx, NULL, NULL, NULL, header, b);
  }
  return NULL;
}

static void
grn_io_register(grn_io *io)
{
  if (io->fis && (io->flags & (GRN_IO_EXPIRE_GTICK|GRN_IO_EXPIRE_SEGMENT))) {
    grn_bool succeeded = GRN_FALSE;
    CRITICAL_SECTION_ENTER(grn_glock);
    if (grn_gctx.impl && grn_gctx.impl->ios &&
        grn_hash_add(&grn_gctx, grn_gctx.impl->ios, io->path, strlen(io->path),
                     (void **)&io, NULL)) {
      succeeded = GRN_TRUE;
    }
    CRITICAL_SECTION_LEAVE(grn_glock);
    if (!succeeded) {
      GRN_LOG(&grn_gctx, GRN_LOG_WARNING,
              "grn_io_register(%s) failed", io->path);
    }
  }
}

static void
grn_io_unregister(grn_io *io)
{
  if (io->fis && (io->flags & (GRN_IO_EXPIRE_GTICK|GRN_IO_EXPIRE_SEGMENT))) {
    grn_bool succeeded = GRN_FALSE;
    CRITICAL_SECTION_ENTER(grn_glock);
    if (grn_gctx.impl && grn_gctx.impl->ios) {
      grn_hash_delete(&grn_gctx, grn_gctx.impl->ios,
                      io->path, strlen(io->path), NULL);
      succeeded = GRN_TRUE;
    }
    CRITICAL_SECTION_LEAVE(grn_glock);
    if (!succeeded) {
      GRN_LOG(&grn_gctx, GRN_LOG_WARNING,
              "grn_io_unregister(%s) failed", io->path);
    }
  }
}

grn_io *
grn_io_create(grn_ctx *ctx, const char *path, uint32_t header_size, uint32_t segment_size,
              uint32_t max_segment, grn_io_mode mode, uint32_t flags)
{
  grn_io *io;
  fileinfo *fis;
  uint32_t b, max_nfiles;
  uint32_t bs;
  struct _grn_io_header *header;
  uint32_t version = grn_io_version_default;
  unsigned long file_size;

  if (!path) {
    return grn_io_create_tmp(header_size, segment_size, max_segment, mode, flags);
  }
  if (!*path || (strlen(path) > PATH_MAX - 4)) { return NULL; }
  b = grn_io_compute_base(header_size);
  bs = grn_io_compute_base_segment(b, segment_size);
  file_size = grn_io_compute_file_size(version);
  max_nfiles = grn_io_compute_max_n_files(segment_size, max_segment,
                                          bs, file_size);
  if ((fis = GRN_GMALLOCN(fileinfo, max_nfiles))) {
    grn_fileinfo_init(fis, max_nfiles);
    if (!grn_open(ctx, fis, path, O_RDWR|O_CREAT|O_EXCL)) {
      header = (struct _grn_io_header *)GRN_MMAP(&grn_gctx, NULL,
                                                 &fis->fmo, fis, 0, b);
      if (header) {
        header->version = version;
        header->header_size = header_size;
        header->segment_size = segment_size;
        header->max_segment = max_segment;
        header->n_arrays = 0;
        header->flags = flags;
        header->lock = 0;
        memcpy(header->idstr, GRN_IO_IDSTR, 16);
        grn_msync(ctx, header, b);
        if ((io = GRN_GMALLOCN(grn_io, 1))) {
          grn_io_mapinfo *maps = NULL;
          if ((maps = GRN_GCALLOC(sizeof(grn_io_mapinfo) * max_segment))) {
            strncpy(io->path, path, PATH_MAX);
            io->header = header;
            io->user_header = (((byte *) header) + IO_HEADER_SIZE);
            io->maps = maps;
            io->base = b;
            io->base_seg = bs;
            io->mode = mode;
            io->header->curr_size = b;
            io->fis = fis;
            io->ainfo = NULL;
            io->max_map_seg = 0;
            io->nmaps = 0;
            io->count = 0;
            io->flags = flags;
            io->lock = &header->lock;
            grn_io_register(io);
            return io;
          }
          GRN_GFREE(io);
        }
        GRN_MUNMAP(&grn_gctx, NULL, &fis->fmo, fis, header, b);
      }
      grn_close(ctx, fis);
    }
    GRN_GFREE(fis);
  }
  return NULL;
}

static grn_rc
array_init_(grn_io *io, int n_arrays, size_t hsize, size_t msize)
{
  int i;
  uint32_t ws;
  byte *hp, *mp;
  grn_io_array_spec *array_specs = (grn_io_array_spec *)io->user_header;
  hp = io->user_header;
  if (!(mp = GRN_GCALLOC(msize))) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  io->ainfo = (grn_io_array_info *)mp;
  hp += sizeof(grn_io_array_spec) * n_arrays;
  mp += sizeof(grn_io_array_info) * n_arrays;
  for (ws = 0; (1 << ws) < io->header->segment_size; ws++);
  for (i = 0; i < n_arrays; i++) {
    uint32_t we = ws - array_specs[i].w_of_element;
    io->ainfo[i].w_of_elm_in_a_segment = we;
    io->ainfo[i].elm_mask_in_a_segment = (1 << we) - 1;
    io->ainfo[i].max_n_segments = array_specs[i].max_n_segments;
    io->ainfo[i].element_size = 1 << array_specs[i].w_of_element;
    io->ainfo[i].segments = (uint32_t *)hp;
    io->ainfo[i].addrs = (void **)mp;
    hp += sizeof(uint32_t) * array_specs[i].max_n_segments;
    mp += sizeof(void *) * array_specs[i].max_n_segments;
  }
  io->user_header += hsize;
  return GRN_SUCCESS;
}

static grn_rc
array_init(grn_io *io, int n_arrays)
{
  if (n_arrays) {
    int i;
    grn_io_array_spec *array_specs = (grn_io_array_spec *)io->user_header;
    size_t hsize = sizeof(grn_io_array_spec) * n_arrays;
    size_t msize = sizeof(grn_io_array_info) * n_arrays;
    for (i = 0; i < n_arrays; i++) {
      hsize += sizeof(uint32_t) * array_specs[i].max_n_segments;
      msize += sizeof(void *) * array_specs[i].max_n_segments;
    }
    return array_init_(io, n_arrays, hsize, msize);
  }
  return GRN_SUCCESS;
}

grn_io *
grn_io_create_with_array(grn_ctx *ctx, const char *path,
                         uint32_t header_size, uint32_t segment_size,
                         grn_io_mode mode, int n_arrays, grn_io_array_spec *array_specs)
{
  if (n_arrays) {
    int i;
    grn_io *io;
    byte *hp;
    uint32_t nsegs = 0;
    size_t hsize = sizeof(grn_io_array_spec) * n_arrays;
    size_t msize = sizeof(grn_io_array_info) * n_arrays;
    for (i = 0; i < n_arrays; i++) {
      nsegs += array_specs[i].max_n_segments;
      hsize += sizeof(uint32_t) * array_specs[i].max_n_segments;
      msize += sizeof(void *) * array_specs[i].max_n_segments;
    }
    if ((io = grn_io_create(ctx, path, header_size + hsize,
                            segment_size, nsegs, mode, GRN_IO_EXPIRE_GTICK))) {
      hp = io->user_header;
      memcpy(hp, array_specs, sizeof(grn_io_array_spec) * n_arrays);
      io->header->n_arrays = n_arrays;
      io->header->segment_tail = 1;
      if (!array_init_(io, n_arrays, hsize, msize)) {
        return io;
      }
      ERR(GRN_NO_MEMORY_AVAILABLE, "grn_io_create_with_array failed");
      grn_io_close(ctx, io);
    }
  }
  return NULL;
}

inline static uint32_t
segment_alloc(grn_io *io)
{
  uint32_t n, s;
  grn_io_array_info *ai;
  if (io->header->segment_tail) {
    if (io->header->segment_tail > io->header->max_segment) {
      s = 0;
    } else {
      s = io->header->segment_tail++;
    }
  } else {
    char *used = GRN_GCALLOC(io->header->max_segment + 1);
    if (!used) { return 0; }
    for (n = io->header->n_arrays, ai = io->ainfo; n; n--, ai++) {
      for (s = 0; s < ai->max_n_segments; s++) {
        used[ai->segments[s]] = 1;
      }
    }
    for (s = 1; ; s++) {
      if (s > io->header->max_segment) {
        io->header->segment_tail = s;
        s = 0;
        break;
      }
      if (!used[s]) {
        io->header->segment_tail = s + 1;
        break;
      }
    }
    GRN_GFREE(used);
  }
  return s;
}

void
grn_io_segment_alloc(grn_ctx *ctx, grn_io *io, grn_io_array_info *ai, uint32_t lseg, int *flags, void **p)
{
  uint32_t *sp = &ai->segments[lseg];
  if (!*sp) {
    if ((*flags & GRN_TABLE_ADD)) {
      if ((*sp = segment_alloc(io))) {
        *flags |= GRN_TABLE_ADDED;
      }
    }
  }
  if (*sp) {
    uint32_t pseg = *sp - 1;
    GRN_IO_SEG_REF(io, pseg, *p);
    if (*p) { GRN_IO_SEG_UNREF(io, pseg); };
  }
}

void *
grn_io_array_at(grn_ctx *ctx, grn_io *io, uint32_t array, off_t offset, int *flags)
{
  void *res;
  GRN_IO_ARRAY_AT(io,array,offset,flags,res);
  return res;
}

uint32_t
grn_io_detect_type(grn_ctx *ctx, const char *path)
{
  struct _grn_io_header h;
  uint32_t res = 0;
  int fd = GRN_OPEN(path, O_RDWR | O_BINARY);
  if (fd != -1) {
    struct stat s;
    if (fstat(fd, &s) != -1 && s.st_size >= sizeof(struct _grn_io_header)) {
      if (read(fd, &h, sizeof(struct _grn_io_header)) == sizeof(struct _grn_io_header)) {
        if (!memcmp(h.idstr, GRN_IO_IDSTR, 16)) {
          res = h.type;
        } else {
          ERR(GRN_INCOMPATIBLE_FILE_FORMAT, "incompatible file format");
        }
      } else {
        SERR(path);
      }
    } else {
      ERR(GRN_INVALID_FORMAT, "grn_io_detect_type failed");
    }
    GRN_CLOSE(fd);
  } else {
    SERR(path);
  }
  return res;
}

grn_io *
grn_io_open(grn_ctx *ctx, const char *path, grn_io_mode mode)
{
  grn_io *io;
  struct stat s;
  fileinfo fi;
  uint32_t flags = 0;
  uint32_t b;
  uint32_t header_size = 0, segment_size = 0, max_segment = 0, bs;
  if (!path || !*path || (strlen(path) > PATH_MAX - 4)) { return NULL; }
  {
    struct _grn_io_header h;
    int fd = GRN_OPEN(path, O_RDWR | O_BINARY);
    if (fd == -1) { SERR(path); return NULL; }
    if (fstat(fd, &s) != -1 && s.st_size >= sizeof(struct _grn_io_header)) {
      if (read(fd, &h, sizeof(struct _grn_io_header)) == sizeof(struct _grn_io_header)) {
        if (!memcmp(h.idstr, GRN_IO_IDSTR, 16)) {
          header_size = h.header_size;
          segment_size = h.segment_size;
          max_segment = h.max_segment;
          flags = h.flags;
        } else {
          ERR(GRN_INCOMPATIBLE_FILE_FORMAT, "incompatible file format");
        }
      }
    }
    GRN_CLOSE(fd);
    if (!segment_size) { return NULL; }
  }
  b = grn_io_compute_base(header_size);
  bs = grn_io_compute_base_segment(b, segment_size);
  grn_fileinfo_init(&fi, 1);
  if (!grn_open(ctx, &fi, path, O_RDWR)) {
    struct _grn_io_header *header;
    header = GRN_MMAP(&grn_gctx, NULL, &(fi.fmo), &fi, 0, b);
    if (header) {
      unsigned long file_size;
      unsigned int max_nfiles;
      fileinfo *fis;

      file_size = grn_io_compute_file_size(header->version);
      max_nfiles = grn_io_compute_max_n_files(segment_size, max_segment,
                                              bs, file_size);
      fis = GRN_GMALLOCN(fileinfo, max_nfiles);
      if (!fis) {
        GRN_MUNMAP(&grn_gctx, NULL, &(fi.fmo), &fi, header, b);
        grn_close(ctx, &fi);
        return NULL;
      }
      grn_fileinfo_init(fis, max_nfiles);
      memcpy(fis, &fi, sizeof(fileinfo));
      if ((io = GRN_GMALLOC(sizeof(grn_io)))) {
        grn_io_mapinfo *maps = NULL;
        if ((maps = GRN_GCALLOC(sizeof(grn_io_mapinfo) * max_segment))) {
          strncpy(io->path, path, PATH_MAX);
          io->header = header;
          io->user_header = (((byte *) header) + IO_HEADER_SIZE);
          {
            io->maps = maps;
            io->base = b;
            io->base_seg = bs;
            io->mode = mode;
            io->fis = fis;
            io->ainfo = NULL;
            io->max_map_seg = 0;
            io->nmaps = 0;
            io->count = 0;
            io->flags = header->flags;
            io->lock = &header->lock;
            if (!array_init(io, io->header->n_arrays)) {
              grn_io_register(io);
              return io;
            }
          }
          if (io->maps) { GRN_GFREE(io->maps); }
        }
        GRN_GFREE(io);
      }
      GRN_GFREE(fis);
      GRN_MUNMAP(&grn_gctx, NULL, &(fi.fmo), &fi, header, b);
    }
    grn_close(ctx, &fi);
  }
  return NULL;
}

grn_rc
grn_io_close(grn_ctx *ctx, grn_io *io)
{
  uint32_t max_nfiles;

  max_nfiles = grn_io_max_n_files(io);
  grn_io_unregister(io);
  if (io->ainfo) { GRN_GFREE(io->ainfo); }
  if (io->maps) {
    int i;
    uint32_t max_segment;
    uint32_t segment_size;
    unsigned long file_size;
    uint32_t segments_per_file;

    max_segment = grn_io_max_segment(io);
    segment_size = io->header->segment_size;
    file_size = grn_io_compute_file_size(io->header->version);
    segments_per_file = file_size / segment_size;
    for (i = 0; i < max_segment; i++) {
      grn_io_mapinfo *mi;
      mi = &(io->maps[i]);
      if (mi->map) {
        fileinfo *fi = NULL;
        /* if (atomic_read(mi->nref)) { return STILL_IN_USE ; } */
        if (io->fis) {
          uint32_t bseg = i + io->base_seg;
          uint32_t fno = bseg / segments_per_file;
          fi = &io->fis[fno];
        }
        GRN_MUNMAP(&grn_gctx, io, &mi->fmo, fi, mi->map, segment_size);
      }
    }
    GRN_GFREE(io->maps);
  }
  GRN_MUNMAP(&grn_gctx, io, (io->fis ? &io->fis->fmo : NULL),
             io->fis, io->header, io->base);
  if (io->fis) {
    int i;
    for (i = 0; i < max_nfiles; i++) {
      fileinfo *fi = &(io->fis[i]);
      grn_close(ctx, fi);
    }
    GRN_GFREE(io->fis);
  }
  GRN_GFREE(io);
  return GRN_SUCCESS;
}

uint32_t
grn_io_base_seg(grn_io *io)
{
  return io->base_seg;
}

const char *
grn_io_path(grn_io *io)
{
  return io->path;
}

void *
grn_io_header(grn_io *io)
{
  return io->user_header;
}

grn_rc
grn_io_set_type(grn_io *io, uint32_t type)
{
  if (!io || !io->header) {
    return GRN_INVALID_ARGUMENT;
  }
  io->header->type = type;
  return GRN_SUCCESS;
}

uint32_t
grn_io_get_type(grn_io *io)
{
  if (!io || !io->header) { return GRN_VOID; }
  return io->header->type;
}

inline static void
gen_pathname(const char *path, char *buffer, int fno)
{
  size_t len = strlen(path);
  memcpy(buffer, path, len);
  if (fno) {
    buffer[len] = '.';
    grn_itoh(fno, buffer + len + 1, 3);
    buffer[len + 4] = '\0';
  } else {
    buffer[len] = '\0';
  }
}

grn_rc
grn_io_size(grn_ctx *ctx, grn_io *io, uint64_t *size)
{
  int fno;
  struct stat s;
  uint64_t tsize = 0;
  char buffer[PATH_MAX];
  uint32_t nfiles;

  if (io->header->curr_size) {
    unsigned long file_size;
    file_size = grn_io_compute_file_size(io->header->version);
    nfiles = (uint32_t) ((io->header->curr_size + file_size - 1) / file_size);
  } else {
    nfiles = grn_io_max_n_files(io);
  }
  for (fno = 0; fno < nfiles; fno++) {
    gen_pathname(io->path, buffer, fno);
    if (stat(buffer, &s)) {
      SERR(buffer);
    } else {
      tsize += s.st_size;
    }
  }
  *size = tsize;
  return GRN_SUCCESS;
}

grn_rc
grn_io_remove(grn_ctx *ctx, const char *path)
{
  struct stat s;
  if (stat(path, &s)) {
    SERR("stat");
    return ctx->rc;
  } else if (unlink(path)) {
    SERR(path);
    return ctx->rc;
  } else {
    int fno;
    char buffer[PATH_MAX];
    for (fno = 1; ; fno++) {
      gen_pathname(path, buffer, fno);
      if (!stat(buffer, &s)) {
        if (unlink(buffer)) { SERR(buffer); }
      } else {
        break;
      }
    }
    return GRN_SUCCESS;
  }
}

grn_rc
grn_io_rename(grn_ctx *ctx, const char *old_name, const char *new_name)
{
  struct stat s;
  if (stat(old_name, &s)) {
    SERR("stat");
    return ctx->rc;
  } else if (rename(old_name, new_name)) {
    SERR(old_name);
    return ctx->rc;
  } else {
    int fno;
    char old_buffer[PATH_MAX];
    char new_buffer[PATH_MAX];
    for (fno = 1; ; fno++) {
      gen_pathname(old_name, old_buffer, fno);
      if (!stat(old_buffer, &s)) {
        gen_pathname(new_name, new_buffer, fno);
        if (rename(old_buffer, new_buffer)) { SERR(old_buffer); }
      } else {
        SERR("stat");
        return ctx->rc;
      }
    }
    return GRN_SUCCESS;
  }
}

typedef struct {
  grn_io_ja_ehead head;
  char body[256];
} ja_element;

grn_rc
grn_io_read_ja(grn_io *io, grn_ctx *ctx, grn_io_ja_einfo *einfo, uint32_t epos, uint32_t key,
               uint32_t segment, uint32_t offset, void **value, uint32_t *value_len)
{
  uint32_t rest = 0, size = *value_len + sizeof(grn_io_ja_ehead);
  uint32_t segment_size = io->header->segment_size;
  unsigned long file_size = grn_io_compute_file_size(io->header->version);
  uint32_t segments_per_file = file_size / segment_size;
  uint32_t bseg = segment + io->base_seg;
  int fno = bseg / segments_per_file;
  fileinfo *fi = &io->fis[fno];
  off_t base = fno ? 0 : io->base - (uint64_t)segment_size * io->base_seg;
  off_t pos = (uint64_t)segment_size * (bseg % segments_per_file) + offset + base;
  ja_element *v = GRN_MALLOC(size);
  if (!v) {
    *value = NULL;
    *value_len = 0;
    return GRN_NO_MEMORY_AVAILABLE;
  }
  if (pos + size > file_size) {
    rest = pos + size - file_size;
    size = file_size - pos;
  }
  if (!grn_opened(fi)) {
    char path[PATH_MAX];
    gen_pathname(io->path, path, fno);
    if (grn_open(ctx, fi, path, O_RDWR|O_CREAT)) {
      *value = NULL;
      *value_len = 0;
      GRN_FREE(v);
      return ctx->rc;
    }
  }
  if (grn_pread(ctx, fi, v, size, pos)) {
    *value = NULL;
    *value_len = 0;
    GRN_FREE(v);
    return ctx->rc;
  }
  if (einfo->pos != epos) {
    GRN_LOG(ctx, GRN_LOG_WARNING, "einfo pos changed %x => %x", einfo->pos, epos);
    *value = NULL;
    *value_len = 0;
    GRN_FREE(v);
    return GRN_FILE_CORRUPT;
  }
  if (einfo->size != *value_len) {
    GRN_LOG(ctx, GRN_LOG_WARNING, "einfo size changed %d => %d", einfo->size, *value_len);
    *value = NULL;
    *value_len = 0;
    GRN_FREE(v);
    return GRN_FILE_CORRUPT;
  }
  if (v->head.key != key) {
    GRN_LOG(ctx, GRN_LOG_ERROR, "ehead key unmatch %x => %x", key, v->head.key);
    *value = NULL;
    *value_len = 0;
    GRN_FREE(v);
    return GRN_INVALID_FORMAT;
  }
  if (v->head.size != *value_len) {
    GRN_LOG(ctx, GRN_LOG_ERROR, "ehead size unmatch %d => %d", *value_len, v->head.size);
    *value = NULL;
    *value_len = 0;
    GRN_FREE(v);
    return GRN_INVALID_FORMAT;
  }
  if (rest) {
    byte *vr = (byte *)v + size;
    do {
      fi = &io->fis[++fno];
      if (!grn_opened(fi)) {
        char path[PATH_MAX];
        gen_pathname(io->path, path, fno);
        if (grn_open(ctx, fi, path, O_RDWR|O_CREAT)) {
          *value = NULL;
          *value_len = 0;
          GRN_FREE(v);
          return ctx->rc;
        }
      }
      size = rest > file_size ? file_size : rest;
      if (grn_pread(ctx, fi, vr, size, 0)) {
        *value = NULL;
        *value_len = 0;
        GRN_FREE(v);
        return ctx->rc;
      }
      vr += size;
      rest -= size;
    } while (rest);
  }
  *value = v->body;
  return GRN_SUCCESS;
}

grn_rc
grn_io_write_ja(grn_io *io, grn_ctx *ctx, uint32_t key,
                uint32_t segment, uint32_t offset, void *value, uint32_t value_len)
{
  grn_rc rc;
  uint32_t rest = 0, size = value_len + sizeof(grn_io_ja_ehead);
  uint32_t segment_size = io->header->segment_size;
  unsigned long file_size = grn_io_compute_file_size(io->header->version);
  uint32_t segments_per_file = file_size / segment_size;
  uint32_t bseg = segment + io->base_seg;
  int fno = bseg / segments_per_file;
  fileinfo *fi = &io->fis[fno];
  off_t base = fno ? 0 : io->base - (uint64_t)segment_size * io->base_seg;
  off_t pos = (uint64_t)segment_size * (bseg % segments_per_file) + offset + base;
  if (pos + size > file_size) {
    rest = pos + size - file_size;
    size = file_size - pos;
  }
  if (!grn_opened(fi)) {
    char path[PATH_MAX];
    gen_pathname(io->path, path, fno);
    if ((rc = grn_open(ctx, fi, path, O_RDWR|O_CREAT))) { return rc; }
  }
  if (value_len <= 256) {
    ja_element je;
    je.head.size = value_len;
    je.head.key = key;
    memcpy(je.body, value, value_len);
    rc = grn_pwrite(ctx, fi, &je, size, pos);
  } else {
    grn_io_ja_ehead eh;
    eh.size = value_len;
    eh.key =  key;
    if ((rc = grn_pwrite(ctx, fi, &eh, sizeof(grn_io_ja_ehead), pos))) { return rc; }
    pos += sizeof(grn_io_ja_ehead);
    rc = grn_pwrite(ctx, fi, value, size - sizeof(grn_io_ja_ehead), pos);
  }
  if (rc) { return rc; }
  if (rest) {
    byte *vr = (byte *)value + size - sizeof(grn_io_ja_ehead);
    do {
      fi = &io->fis[++fno];
      if (!grn_opened(fi)) {
        char path[PATH_MAX];
        gen_pathname(io->path, path, fno);
        if ((rc = grn_open(ctx, fi, path, O_RDWR|O_CREAT))) { return rc; }
      }
      size = rest > file_size ? file_size : rest;
      if ((rc = grn_pwrite(ctx, fi, vr, size, 0))) { return rc; }
      vr += size;
      rest -= size;
    } while (rest);
  }
  return rc;
}

grn_rc
grn_io_write_ja_ehead(grn_io *io, grn_ctx *ctx, uint32_t key,
                      uint32_t segment, uint32_t offset, uint32_t value_len)
{
  grn_rc rc;
  uint32_t segment_size = io->header->segment_size;
  unsigned long file_size = grn_io_compute_file_size(io->header->version);
  uint32_t segments_per_file = file_size / segment_size;
  uint32_t bseg = segment + io->base_seg;
  int fno = bseg / segments_per_file;
  fileinfo *fi = &io->fis[fno];
  off_t base = fno ? 0 : io->base - (uint64_t)segment_size + io->base_seg;
  off_t pos = (uint64_t)segment_size * (bseg % segments_per_file) + offset + base;
  if (!grn_opened(fi)) {
    char path[PATH_MAX];
    gen_pathname(io->path, path, fno);
    if ((rc = grn_open(ctx, fi, path, O_RDWR|O_CREAT))) { return rc; }
  }
  {
    grn_io_ja_ehead eh;
    eh.size = value_len;
    eh.key =  key;
    return grn_pwrite(ctx, fi, &eh, sizeof(grn_io_ja_ehead), pos);
  }
}

void *
grn_io_win_map(grn_io *io, grn_ctx *ctx, grn_io_win *iw, uint32_t segment,
               uint32_t offset, uint32_t size, grn_io_rw_mode mode)
{
  uint32_t nseg, segment_size = io->header->segment_size;
  if (offset >= segment_size) {
    segment += offset / segment_size;
    offset = offset % segment_size;
  }
  nseg = (offset + size + segment_size - 1) / segment_size;
  if (!size || !ctx || segment + nseg > io->header->max_segment) { return NULL; }
  iw->ctx = ctx;
  iw->diff = 0;
  iw->io = io;
  iw->mode = mode;
  iw->tiny_p = 0;
  iw->segment = segment;
  iw->offset = offset;
  iw->nseg = nseg;
  iw->size = size;
  if (nseg == 1) {
    byte *addr = NULL;
    GRN_IO_SEG_REF(io, segment, addr);
    if (!addr) { return NULL; }
    iw->cached = 1;
    iw->addr = addr + offset;
  } else {
    if (!(iw->addr = GRN_MALLOC(size))) { return NULL; }
    iw->cached = 0;
    switch (mode) {
    case grn_io_rdonly:
    case grn_io_rdwr:
      {
        byte *p, *q = NULL;
        uint32_t s, r;
        for (p = iw->addr, r = size; r; p += s, r -= s, segment++, offset = 0) {
          GRN_IO_SEG_REF(io, segment, q);
          if (!q) {
            GRN_FREE(iw->addr);
            return NULL;
          }
          s = (offset + r > segment_size) ? segment_size - offset : r;
          memcpy(p, q + offset, s);
          GRN_IO_SEG_UNREF(io, segment);
        }
      }
      break;
    case grn_io_wronly:
      break;
    default :
      return NULL;
    }
  }
  return iw->addr;
}

grn_rc
grn_io_win_unmap(grn_io_win *iw)
{
  if (!iw || !iw->io ||!iw->ctx) { return GRN_INVALID_ARGUMENT; }
  if (iw->cached) {
    if (!iw->tiny_p) { GRN_IO_SEG_UNREF(iw->io, iw->segment); }
    return GRN_SUCCESS;
  }
  {
    grn_io *io = iw->io;
    grn_ctx *ctx = iw->ctx;
    switch (iw->mode) {
    case grn_io_rdonly:
      if (!iw->addr) { return GRN_INVALID_ARGUMENT; }
      GRN_FREE(iw->addr);
      return GRN_SUCCESS;
    case grn_io_rdwr:
    case grn_io_wronly:
      {
        byte *p, *q = NULL;
        uint32_t segment_size = io->header->segment_size;
        uint32_t s, r, offset = iw->offset, segment = iw->segment;
        for (p = iw->addr, r = iw->size; r; p += s, r -= s, segment++, offset = 0) {
          GRN_IO_SEG_REF(io, segment, q);
          if (!q) { return GRN_NO_MEMORY_AVAILABLE; }
          s = (offset + r > segment_size) ? segment_size - offset : r;
          memcpy(q + offset, p, s);
          GRN_IO_SEG_UNREF(io, segment);
        }
      }
      GRN_FREE(iw->addr);
      return GRN_SUCCESS;
    default :
      return GRN_INVALID_ARGUMENT;
    }
  }
}

#define DO_MAP(io,fmo,fi,pos,size,segno,res) do {\
  if (((res) = GRN_MMAP(&grn_gctx, (io), (fmo), (fi), (pos), (size)))) {\
    uint32_t nmaps;\
    if (io->max_map_seg < segno) { io->max_map_seg = segno; }\
    GRN_ATOMIC_ADD_EX(&io->nmaps, 1, nmaps);\
    {\
      uint64_t tail = io->base + (uint64_t)(size) * ((segno) + 1);\
      if (tail > io->header->curr_size) { io->header->curr_size = tail; }\
    }\
  }\
} while (0)

#define SEG_MAP(io,segno,info) do {\
  uint32_t segment_size = io->header->segment_size;\
  if ((io->flags & GRN_IO_TEMPORARY)) {\
    DO_MAP(io, &info->fmo, NULL, 0, segment_size, segno, info->map);\
  } else {\
    unsigned long file_size = grn_io_compute_file_size(io->header->version);\
    uint32_t segments_per_file = file_size / segment_size;\
    uint32_t bseg = segno + io->base_seg;\
    uint32_t fno = bseg / segments_per_file;\
    off_t base = fno ? 0 : io->base - (uint64_t)segment_size * io->base_seg;\
    off_t pos = (uint64_t)segment_size * (bseg % segments_per_file) + base;\
    fileinfo *fi = &io->fis[fno];\
    if (!grn_opened(fi)) {\
      char path[PATH_MAX];\
      gen_pathname(io->path, path, fno);\
      if (!grn_open(ctx, fi, path, O_RDWR|O_CREAT)) {  \
        DO_MAP(io, &info->fmo, fi, pos, segment_size, segno, info->map);\
      }\
    } else {\
      DO_MAP(io, &info->fmo, fi, pos, segment_size, segno, info->map);\
    }\
  }\
} while (0)

void
grn_io_seg_map_(grn_ctx *ctx, grn_io *io, uint32_t segno, grn_io_mapinfo *info)
{
  SEG_MAP(io, segno, info);
}

grn_rc
grn_io_seg_expire(grn_ctx *ctx, grn_io *io, uint32_t segno, uint32_t nretry)
{
  uint32_t retry, *pnref;
  grn_io_mapinfo *info;
  if (!io->maps || segno >= io->header->max_segment) { return GRN_INVALID_ARGUMENT; }
  info = &io->maps[segno];
  if (!info->map) { return GRN_INVALID_ARGUMENT; }
  pnref = &info->nref;
  for (retry = 0;; retry++) {
    uint32_t nref;
    GRN_ATOMIC_ADD_EX(pnref, 1, nref);
    if (nref) {
      GRN_ATOMIC_ADD_EX(pnref, -1, nref);
      if (retry >= GRN_IO_MAX_RETRY) {
        GRN_LOG(ctx, GRN_LOG_CRIT, "deadlock detected! in grn_io_seg_expire(%p, %u, %u)", io, segno, nref);
        return GRN_RESOURCE_DEADLOCK_AVOIDED;
      }
    } else {
      GRN_ATOMIC_ADD_EX(pnref, GRN_IO_MAX_REF, nref);
      if (nref > 1) {
        GRN_ATOMIC_ADD_EX(pnref, -(GRN_IO_MAX_REF + 1), nref);
        GRN_FUTEX_WAKE(pnref);
        if (retry >= GRN_IO_MAX_RETRY) {
          GRN_LOG(ctx, GRN_LOG_CRIT, "deadlock detected!! in grn_io_seg_expire(%p, %u, %u)", io,
 segno, nref);
          return GRN_RESOURCE_DEADLOCK_AVOIDED;
        }
      } else {
        uint32_t nmaps;
        fileinfo *fi = &(io->fis[segno]);
        GRN_MUNMAP(&grn_gctx, io, &info->fmo, fi,
                   info->map, io->header->segment_size);
        info->map = NULL;
        GRN_ATOMIC_ADD_EX(pnref, -(GRN_IO_MAX_REF + 1), nref);
        GRN_ATOMIC_ADD_EX(&io->nmaps, -1, nmaps);
        GRN_FUTEX_WAKE(pnref);
        return GRN_SUCCESS;
      }
    }
    if (retry >= nretry) { return GRN_RESOURCE_DEADLOCK_AVOIDED; }
    GRN_FUTEX_WAIT(pnref);
  }
}

uint32_t
grn_io_expire(grn_ctx *ctx, grn_io *io, int count_thresh, uint32_t limit)
{
  uint32_t m, n = 0, ln = io->nmaps;
  switch ((io->flags & (GRN_IO_EXPIRE_GTICK|GRN_IO_EXPIRE_SEGMENT))) {
  case GRN_IO_EXPIRE_GTICK :
    {
      uint32_t nref, nmaps, *pnref = &io->nref;
      GRN_ATOMIC_ADD_EX(pnref, 1, nref);
      if (!nref && grn_gtick - io->count > count_thresh) {
        {
          uint32_t i = io->header->n_arrays;
          grn_io_array_spec *array_specs = (grn_io_array_spec *)io->user_header;
          while (i--) {
            memset(io->ainfo[i].addrs, 0, sizeof(void *) * array_specs[i].max_n_segments);
          }
        }
        {
          uint32_t fno;
          for (fno = 0; fno < io->max_map_seg; fno++) {
            grn_io_mapinfo *info = &(io->maps[fno]);
            if (info->map) {
              fileinfo *fi = &(io->fis[fno]);
              GRN_MUNMAP(&grn_gctx, io, &info->fmo, fi,
                         info->map, io->header->segment_size);
              info->map = NULL;
              info->nref = 0;
              info->count = grn_gtick;
              GRN_ATOMIC_ADD_EX(&io->nmaps, -1, nmaps);
              n++;
            }
          }
        }
      }
      GRN_ATOMIC_ADD_EX(pnref, -1, nref);
    }
    break;
  case GRN_IO_EXPIRE_SEGMENT :
    for (m = io->max_map_seg; n < limit && m; m--) {
      if (!grn_io_seg_expire(ctx, io, m, 0)) { n++; }
    }
    break;
  case (GRN_IO_EXPIRE_GTICK|GRN_IO_EXPIRE_SEGMENT) :
    {
      grn_io_mapinfo *info = io->maps;
      for (m = io->max_map_seg; n < limit && m; info++, m--) {
        if (info->map && (grn_gtick - info->count) > count_thresh) {
          uint32_t nmaps, nref, *pnref = &info->nref;
          GRN_ATOMIC_ADD_EX(pnref, 1, nref);
          if (!nref && info->map && (grn_gtick - info->count) > count_thresh) {
            GRN_MUNMAP(&grn_gctx, io, &info->fmo, NULL,
                       info->map, io->header->segment_size);
            GRN_ATOMIC_ADD_EX(&io->nmaps, -1, nmaps);
            info->map = NULL;
            info->count = grn_gtick;
            n++;
          }
          GRN_ATOMIC_ADD_EX(pnref, -1, nref);
        }
      }
    }
    break;
  }
  if (n) {
    GRN_LOG(ctx, GRN_LOG_INFO, "<%p:%x> expired i=%p max=%d (%d/%d)",
            ctx, grn_gtick, io, io->max_map_seg, n, ln);
  }
  return n;
}

static uint32_t
grn_expire_(grn_ctx *ctx, int count_thresh, uint32_t limit)
{
  uint32_t n = 0;
  grn_io *io;
  GRN_HASH_EACH(ctx, grn_gctx.impl->ios, id, NULL, NULL, (void **)&io, {
    grn_plugin_close(ctx, id);
    n += grn_io_expire(ctx, io, count_thresh, limit);
    if (n >= limit) { break; }
  });
  return n;
}

uint32_t
grn_expire(grn_ctx *ctx, int count_thresh, uint32_t limit)
{
  grn_ctx *c;
  uint32_t n = 0;
  CRITICAL_SECTION_ENTER(grn_glock);
  if (grn_gtick) {
    for (c = grn_gctx.next;; c = ctx->next) {
      if (c == &grn_gctx) {
        CRITICAL_SECTION_LEAVE(grn_glock);
        n = grn_expire_(ctx, count_thresh, limit);
        CRITICAL_SECTION_ENTER(grn_glock);
        break;
      }
      if ((c->seqno & 1) && (c->seqno == c->seqno2)) { break; }
    }
  }
  grn_gtick++;
  for (c = grn_gctx.next; c != &grn_gctx; c = ctx->next) { c->seqno2 = c->seqno; }
  CRITICAL_SECTION_LEAVE(grn_glock);
  return n;
}

void *
grn_io_anon_map(grn_ctx *ctx, grn_io_mapinfo *mi, size_t length)
{
  return (mi->map = GRN_MMAP(ctx, NULL, &mi->fmo, NULL, 0, length));
}

void
grn_io_anon_unmap(grn_ctx *ctx, grn_io_mapinfo *mi, size_t length)
{
  GRN_MUNMAP(ctx, NULL, &mi->fmo, NULL, mi->map, length);
}

grn_rc
grn_io_lock(grn_ctx *ctx, grn_io *io, int timeout)
{
  static int _ncalls = 0, _ncolls = 0;
  uint32_t count, count_log_border = 1000;
  _ncalls++;
  if (!io) { return GRN_INVALID_ARGUMENT; }
  for (count = 0;; count++) {
    uint32_t lock;
    GRN_ATOMIC_ADD_EX(io->lock, 1, lock);
    if (lock) {
      GRN_ATOMIC_ADD_EX(io->lock, -1, lock);
      if (count == count_log_border) {
        GRN_LOG(ctx, GRN_LOG_NOTICE,
                "io(%s) collisions(%d/%d): lock failed %d times",
                io->path, _ncolls, _ncalls, count_log_border);
      }
      if (!timeout || (timeout > 0 && timeout == count)) {
        GRN_LOG(ctx, GRN_LOG_WARNING,
                "[DB Locked] time out(%d): io(%s) collisions(%d/%d)",
                timeout, io->path, _ncolls, _ncalls);
        break;
      }
      if (!(++_ncolls % 1000000) && (_ncolls > _ncalls)) {
        if (_ncolls < 0 || _ncalls < 0) {
          _ncolls = 0; _ncalls = 0;
        } else {
          GRN_LOG(ctx, GRN_LOG_NOTICE,
                  "io(%s) collisions(%d/%d)", io->path, _ncolls, _ncalls);
        }
      }
      grn_nanosleep(GRN_LOCK_WAIT_TIME_NANOSECOND);
      continue;
    }
    return GRN_SUCCESS;
  }
  ERR(GRN_RESOURCE_DEADLOCK_AVOIDED, "grn_io_lock failed");
  return ctx->rc;
}

void
grn_io_unlock(grn_io *io)
{
  if (io) {
    uint32_t lock;
    GRN_ATOMIC_ADD_EX(io->lock, -1, lock);
  }
}

void
grn_io_clear_lock(grn_io *io)
{
  if (io) { *io->lock = 0; }
}

uint32_t
grn_io_is_locked(grn_io *io)
{
  return io ? *io->lock : 0;
}

/** mmap abstraction **/

static size_t mmap_size = 0;

#ifdef WIN32

inline static grn_rc
grn_open_v1(grn_ctx *ctx, fileinfo *fi, const char *path, int flags)
{
  CRITICAL_SECTION_INIT(fi->cs);
  return GRN_SUCCESS;
}

inline static void *
grn_mmap_v1(grn_ctx *ctx, HANDLE *fmo, fileinfo *fi, off_t offset, size_t length)
{
  void *res;
  if (!fi) {
    if (fmo) {
      *fmo = NULL;
    }
    /* TODO: Try to support VirtualAlloc() as anonymous mmap in POSIX.
     * If VirtualAlloc() provides better performance rather than malloc(),
     * we'll use it.
     */
    return GRN_GCALLOC(length);
  }
  /* CRITICAL_SECTION_ENTER(fi->cs); */
  /* try to create fmo */
  *fmo = CreateFileMapping(fi->fh, NULL, PAGE_READWRITE, 0, offset + length, NULL);
  if (!*fmo) { return NULL; }
  res = MapViewOfFile(*fmo, FILE_MAP_WRITE, 0, (DWORD)offset, (SIZE_T)length);
  if (!res) {
    SERR("MapViewOfFile");
    GRN_LOG(ctx, GRN_LOG_ERROR,
            "MapViewOfFile(%lu,%" GRN_FMT_SIZE ") failed <%" GRN_FMT_SIZE ">",
            (DWORD)offset, length, mmap_size);
    return NULL;
  }
  /* CRITICAL_SECTION_LEAVE(fi->cs); */
  mmap_size += length;
  return res;
}

inline static int
grn_munmap_v1(grn_ctx *ctx, HANDLE *fmo, fileinfo *fi,
              void *start, size_t length)
{
  int r = 0;

  if (!fi) {
    GRN_GFREE(start);
    return r;
  }

  if (!fmo) {
    GRN_GFREE(start);
    return r;
  }

  if (*fmo) {
    if (UnmapViewOfFile(start)) {
      mmap_size -= length;
    } else {
      SERR("UnmapViewOfFile");
      GRN_LOG(ctx, GRN_LOG_ERROR,
              "UnmapViewOfFile(%p,%" GRN_FMT_SIZE ") failed <%" GRN_FMT_SIZE ">",
              start, length, mmap_size);
      r = -1;
    }
    if (!CloseHandle(*fmo)) {
      SERR("CloseHandle");
      GRN_LOG(ctx, GRN_LOG_ERROR,
              "CloseHandle(%p,%" GRN_FMT_SIZE ") failed <%" GRN_FMT_SIZE ">",
              start, length, mmap_size);
    }
    *fmo = NULL;
  } else {
    GRN_GFREE(start);
  }

  return r;
}

inline static grn_rc
grn_open_v0(grn_ctx *ctx, fileinfo *fi, const char *path, int flags)
{
  /* signature may be wrong.. */
  fi->fmo = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, NULL);
  /* open failed */
  if (fi->fmo == NULL) {
    // flock
    /* retry to open */
    fi->fmo = OpenFileMapping(FILE_MAP_ALL_ACCESS, FALSE, NULL);
    /* failed again */
    if (fi->fmo == NULL) {
      /* try to create fmo */
      fi->fmo = CreateFileMapping(fi->fh, NULL, PAGE_READWRITE, 0, GRN_IO_FILE_SIZE_V0, NULL);
    }
    // funlock
  }
  if (fi->fmo != NULL) {
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
      CRITICAL_SECTION_INIT(fi->cs);
      return GRN_SUCCESS;
    } else {
      GRN_LOG(ctx, GRN_LOG_ERROR, "fmo object already exists! handle=%p", fi->fh);
      CloseHandle(fi->fmo);
    }
  } else {
    GRN_LOG(ctx, GRN_LOG_ALERT,
            "failed to get FileMappingObject #%lu", GetLastError());
  }
  CloseHandle(fi->fh);
  SERR("OpenFileMapping");
  return ctx->rc;
}

inline static void *
grn_mmap_v0(grn_ctx *ctx, fileinfo *fi, off_t offset, size_t length)
{
  void *res;
  if (!fi) { return GRN_GCALLOC(length); }
  /* file must be exceeded to GRN_IO_FILE_SIZE_V0 when FileMappingObject created.
     and, after fmo created, it's not allowed to expand the size of file.
  DWORD tail = (DWORD)(offset + length);
  DWORD filesize = GetFileSize(fi->fh, NULL);
  if (filesize < tail) {
    if (SetFilePointer(fi->fh, tail, NULL, FILE_BEGIN) != tail) {
      grn_log("SetFilePointer failed");
      return NULL;
    }
    if (!SetEndOfFile(fi->fh)) {
      grn_log("SetEndOfFile failed");
      return NULL;
    }
    filesize = tail;
  }
  */
  res = MapViewOfFile(fi->fmo, FILE_MAP_WRITE, 0, (DWORD)offset, (SIZE_T)length);
  if (!res) {
    MERR("MapViewOfFile failed: <%" GRN_FMT_SIZE ">: %s",
         mmap_size, grn_current_error_message());
    return NULL;
  }
  mmap_size += length;
  return res;
}

inline static int
grn_munmap_v0(grn_ctx *ctx, fileinfo *fi, void *start, size_t length)
{
  if (!fi) {
    GRN_GFREE(start);
    return 0;
  }

  if (UnmapViewOfFile(start)) {
    mmap_size -= length;
    return 0;
  } else {
    SERR("UnmapViewOfFile");
    GRN_LOG(ctx, GRN_LOG_ERROR,
            "UnmapViewOfFile(%p,%" GRN_FMT_SIZE ") failed <%" GRN_FMT_SIZE ">",
            start, length, mmap_size);
    return -1;
  }
}

inline static grn_rc
grn_open_common(grn_ctx *ctx, fileinfo *fi, const char *path, int flags)
{
  /* may be wrong if flags is just only O_RDWR */
  if ((flags & O_CREAT)) {
    DWORD dwCreationDisposition;
    if (flags & O_EXCL) {
      dwCreationDisposition = CREATE_NEW;
    } else {
      dwCreationDisposition = OPEN_ALWAYS;
    }
    fi->fh = CreateFile(path, GRN_IO_FILE_CREATE_MODE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        dwCreationDisposition, FILE_ATTRIBUTE_NORMAL, 0);
    if (fi->fh == INVALID_HANDLE_VALUE) {
      SERR("CreateFile");
      goto exit;
    }
    goto exit;
  }
  if ((flags & O_TRUNC)) {
    CloseHandle(fi->fh);
    /* unable to assign OPEN_ALWAYS and TRUNCATE_EXISTING at once */
    fi->fh = CreateFile(path, GRN_IO_FILE_CREATE_MODE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (fi->fh == INVALID_HANDLE_VALUE) {
      SERR("CreateFile");
      goto exit;
    }
    goto exit;
  }
  /* O_RDWR only */
  fi->fh = CreateFile(path, GRN_IO_FILE_CREATE_MODE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
  if (fi->fh == INVALID_HANDLE_VALUE) {
    SERR("CreateFile");
    goto exit;
  }

exit :
  return ctx->rc;
}

inline static grn_rc
grn_open(grn_ctx *ctx, fileinfo *fi, const char *path, int flags)
{
  grn_rc rc;
  struct _grn_io_header io_header;
  DWORD header_size;
  DWORD read_bytes;
  int version = grn_io_version_default;

  rc = grn_open_common(ctx, fi, path, flags);
  if (rc != GRN_SUCCESS) {
    return rc;
  }

  if (!(flags & O_CREAT)) {
    header_size = sizeof(struct _grn_io_header);
    ReadFile(fi->fh, &io_header, header_size, &read_bytes, NULL);
    if (read_bytes == header_size) {
      version = io_header.version;
    }
    SetFilePointer(fi->fh, 0, NULL, FILE_BEGIN);
  }

  if (version == 0) {
    return grn_open_v0(ctx, fi, path, flags);
  } else {
    return grn_open_v1(ctx, fi, path, flags);
  }
}

inline static int
grn_guess_io_version(grn_ctx *ctx, grn_io *io, fileinfo *fi)
{
  if (io) {
    return io->header->version;
  }

  if (fi) {
    if (fi->fmo) {
      return 0;
    } else {
      return 1;
    }
  }

  return grn_io_version_default;
}

inline static void *
grn_mmap(grn_ctx *ctx, grn_io *io, HANDLE *fmo,
         fileinfo *fi, off_t offset, size_t length)
{
  int version;

  version = grn_guess_io_version(ctx, io, fi);

  if (version == 0) {
    return grn_mmap_v0(ctx, fi, offset, length);
  } else {
    return grn_mmap_v1(ctx, fmo, fi, offset, length);
  }
}

inline static int
grn_munmap(grn_ctx *ctx, grn_io *io,
           HANDLE *fmo, fileinfo *fi, void *start, size_t length)
{
  int version;

  version = grn_guess_io_version(ctx, io, fi);

  if (version == 0) {
    return grn_munmap_v0(ctx, fi, start, length);
  } else {
    return grn_munmap_v1(ctx, fmo, fi, start, length);
  }
}

inline static grn_rc
grn_close(grn_ctx *ctx, fileinfo *fi)
{
  if (fi->fmo != NULL) {
    CloseHandle(fi->fmo);
    fi->fmo = NULL;
  }
  if (fi->fh != INVALID_HANDLE_VALUE) {
    CloseHandle(fi->fh);
    CRITICAL_SECTION_FIN(fi->cs);
    fi->fh = INVALID_HANDLE_VALUE;
  }
  return GRN_SUCCESS;
}

inline static void
grn_fileinfo_init(fileinfo *fis, int nfis)
{
  for (; nfis--; fis++) {
    fis->fh = INVALID_HANDLE_VALUE;
    fis->fmo = NULL;
  }
}

inline static int
grn_opened(fileinfo *fi)
{
  return fi->fh != INVALID_HANDLE_VALUE;
}

inline static int
grn_msync(grn_ctx *ctx, void *start, size_t length)
{
  /* return value may be wrong... */
  return FlushViewOfFile(start, length);
}

inline static grn_rc
grn_pread(grn_ctx *ctx, fileinfo *fi, void *buf, size_t count, off_t offset)
{
  DWORD r, len;
  CRITICAL_SECTION_ENTER(fi->cs);
  r = SetFilePointer(fi->fh, offset, NULL, FILE_BEGIN);
  if (r == INVALID_SET_FILE_POINTER) {
    SERR("SetFilePointer");
  } else {
    if (!ReadFile(fi->fh, buf, (DWORD)count, &len, NULL)) {
      SERR("ReadFile");
    } else if (len != count) {
      /* todo : should retry ? */
      ERR(GRN_INPUT_OUTPUT_ERROR,
          "ReadFile %" GRN_FMT_SIZE " != %lu",
          count, len);
    }
  }
  CRITICAL_SECTION_LEAVE(fi->cs);
  return ctx->rc;
}

inline static grn_rc
grn_pwrite(grn_ctx *ctx, fileinfo *fi, void *buf, size_t count, off_t offset)
{
  DWORD r, len;
  CRITICAL_SECTION_ENTER(fi->cs);
  r = SetFilePointer(fi->fh, offset, NULL, FILE_BEGIN);
  if (r == INVALID_SET_FILE_POINTER) {
    SERR("SetFilePointer");
  } else {
    if (!WriteFile(fi->fh, buf, (DWORD)count, &len, NULL)) {
      SERR("WriteFile");
    } else if (len != count) {
      /* todo : should retry ? */
      ERR(GRN_INPUT_OUTPUT_ERROR,
          "WriteFile %" GRN_FMT_SIZE " != %lu",
          count, len);
    }
  }
  CRITICAL_SECTION_LEAVE(fi->cs);
  return ctx->rc;
}

#else /* WIN32 */

inline static grn_rc
grn_open(grn_ctx *ctx, fileinfo *fi, const char *path, int flags)
{
  struct stat st;
  if ((fi->fd = GRN_OPEN(path, flags, GRN_IO_FILE_CREATE_MODE)) == -1) {
    SERR(path);
    return ctx->rc;
  }
  if (fstat(fi->fd, &st) == -1) {
    SERR(path);
    return ctx->rc;
  }
  fi->dev = st.st_dev;
  fi->inode = st.st_ino;
  return GRN_SUCCESS;
}

inline static void
grn_fileinfo_init(fileinfo *fis, int nfis)
{
  for (; nfis--; fis++) { fis->fd = -1; }
}

inline static int
grn_opened(fileinfo *fi)
{
  return fi->fd != -1;
}

inline static grn_rc
grn_close(grn_ctx *ctx, fileinfo *fi)
{
  if (fi->fd != -1) {
    if (GRN_CLOSE(fi->fd) == -1) {
      SERR("close");
      return ctx->rc;
    }
    fi->fd = -1;
  }
  return GRN_SUCCESS;
}

#if defined(MAP_ANON) && !defined(MAP_ANONYMOUS)
#define MAP_ANONYMOUS MAP_ANON
#endif

#include <sys/mman.h>

inline static void *
grn_mmap(grn_ctx *ctx, grn_io *io, fileinfo *fi, off_t offset, size_t length)
{
  void *res;
  int fd, flags;
  if (fi) {
    struct stat s;
    off_t tail = offset + length;
    fd = fi->fd;
    if ((fstat(fd, &s) == -1) || (s.st_size < tail && ftruncate(fd, tail) == -1)) {
      SERR("fstat");
      return NULL;
    }
    flags = MAP_SHARED;
  } else {
    fd = -1;
    flags = MAP_PRIVATE|MAP_ANONYMOUS;
  }
  res = mmap(NULL, length, PROT_READ|PROT_WRITE, flags, fd, offset);
  if (MAP_FAILED == res) {
    MERR("mmap(%" GRN_FMT_LLU ",%d,%" GRN_FMT_LLD ")=%s <%" GRN_FMT_LLU ">",
         (unsigned long long int)length, fd, (long long int)offset, strerror(errno),
         (unsigned long long int)mmap_size);
    return NULL;
  }
  mmap_size += length;
  return res;
}

#ifdef USE_FAIL_MALLOC
inline static void *
grn_fail_mmap(grn_ctx *ctx, grn_io *io, fileinfo *fi,
              off_t offset, size_t length,
              const char* file, int line, const char *func)
{
  if (grn_fail_malloc_check(length, file, line, func)) {
    return grn_mmap(ctx, fi, offset, length);
  } else {
    MERR("fail_mmap(%" GRN_FMT_SIZE ",%d,%" GRN_FMT_LLU ") "
         "(%s:%d@%s) <%" GRN_FMT_SIZE ">",
          length, fi ? fi->fd : 0, offset, file, line, func, mmap_size);
    return NULL;
  }
}
#endif /* USE_FAIL_MALLOC */

inline static int
grn_msync(grn_ctx *ctx, void *start, size_t length)
{
  int r = msync(start, length, MS_SYNC);
  if (r == -1) { SERR("msync"); }
  return r;
}

inline static int
grn_munmap(grn_ctx *ctx, grn_io *io, fileinfo *fi, void *start, size_t length)
{
  int res;
  res = munmap(start, length);
  if (res) {
    SERR("munmap");
    GRN_LOG(ctx, GRN_LOG_ERROR, "munmap(%p,%" GRN_FMT_LLU ") failed <%" GRN_FMT_LLU ">",
            start, (unsigned long long int)length, (unsigned long long int)mmap_size);
  } else {
    mmap_size -= length;
  }
  return res;
}

inline static grn_rc
grn_pread(grn_ctx *ctx, fileinfo *fi, void *buf, size_t count, off_t offset)
{
  ssize_t r = pread(fi->fd, buf, count, offset);
  if (r != count) {
    if (r == -1) {
      SERR("pread");
    } else {
      /* todo : should retry ? */
      ERR(GRN_INPUT_OUTPUT_ERROR, "pread returned %" GRN_FMT_LLD " != %" GRN_FMT_LLU,
          (long long int)r, (unsigned long long int)count);
    }
    return ctx->rc;
  }
  return GRN_SUCCESS;
}

inline static grn_rc
grn_pwrite(grn_ctx *ctx, fileinfo *fi, void *buf, size_t count, off_t offset)
{
  ssize_t r = pwrite(fi->fd, buf, count, offset);
  if (r != count) {
    if (r == -1) {
      SERR("pwrite");
    } else {
      /* todo : should retry ? */
      ERR(GRN_INPUT_OUTPUT_ERROR, "pwrite returned %" GRN_FMT_LLD " != %" GRN_FMT_LLU,
          (long long int)r, (unsigned long long int)count);
    }
    return ctx->rc;
  }
  return GRN_SUCCESS;
}

#endif /* WIN32 */
