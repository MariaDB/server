/*
   Copyright (c) 2005, 2012, Oracle and/or its affiliates.
   Copyright (c) 2014, 2015, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

/*
  InnoDB offline file checksum utility.  85% of the code in this utility
  is included from the InnoDB codebase.

  The final 15% was originally written by Mark Smith of Danga
  Interactive, Inc. <junior@danga.com>

  Published with a permission.
*/

#include <my_global.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#ifndef __WIN__
# include <unistd.h>
#endif
#include <my_getopt.h>
#include <m_string.h>
#include <welcome_copyright_notice.h> /* ORACLE_WELCOME_COPYRIGHT_NOTICE */

/* Only parts of these files are included from the InnoDB codebase.
The parts not included are excluded by #ifndef UNIV_INNOCHECKSUM. */

#include "univ.i"                /*  include all of this */

#define FLST_BASE_NODE_SIZE (4 + 2 * FIL_ADDR_SIZE)
#define FLST_NODE_SIZE (2 * FIL_ADDR_SIZE)
#define FSEG_PAGE_DATA FIL_PAGE_DATA
#define MLOG_1BYTE (1)

#include "ut0ut.h"
#include "ut0byte.h"
#include "mach0data.h"
#include "fsp0types.h"
#include "rem0rec.h"
#include "buf0checksum.h"        /* buf_calc_page_*() */
#include "fil0fil.h"             /* FIL_* */
#include "page0page.h"           /* PAGE_* */
#include "page0zip.h"            /* page_zip_*() */
#include "trx0undo.h"            /* TRX_* */
#include "fsp0fsp.h"             /* fsp_flags_get_page_size() &
                                    fsp_flags_get_zip_size() */
#include "ut0crc32.h"            /* ut_crc32_init() */
#include "fsp0pagecompress.h"    /* fil_get_compression_alg_name */

#ifdef UNIV_NONINL
# include "fsp0fsp.ic"
# include "mach0data.ic"
# include "ut0rnd.ic"
#endif

/* Global variables */
static my_bool verbose;
static my_bool debug;
static my_bool skip_corrupt;
static my_bool just_count;
static ulong start_page;
static ulong end_page;
static ulong do_page;
static my_bool use_end_page;
static my_bool do_one_page;
static my_bool per_page_details;
static my_bool do_leaf;
static ulong n_merge;
ulong srv_page_size;              /* replaces declaration in srv0srv.c */
static ulong physical_page_size;  /* Page size in bytes on disk. */
static ulong logical_page_size;   /* Page size when uncompressed. */
static bool compressed= false;    /* Is tablespace compressed */

int n_undo_state_active;
int n_undo_state_cached;
int n_undo_state_to_free;
int n_undo_state_to_purge;
int n_undo_state_prepared;
int n_undo_state_other;
int n_undo_insert, n_undo_update, n_undo_other;
int n_bad_checksum;
int n_fil_page_index;
int n_fil_page_undo_log;
int n_fil_page_inode;
int n_fil_page_ibuf_free_list;
int n_fil_page_allocated;
int n_fil_page_ibuf_bitmap;
int n_fil_page_type_sys;
int n_fil_page_type_trx_sys;
int n_fil_page_type_fsp_hdr;
int n_fil_page_type_allocated;
int n_fil_page_type_xdes;
int n_fil_page_type_blob;
int n_fil_page_type_zblob;
int n_fil_page_type_other;
int n_fil_page_type_page_compressed;
int n_fil_page_type_page_compressed_encrypted;

int n_fil_page_max_index_id;

#define SIZE_RANGES_FOR_PAGE 10
#define NUM_RETRIES 3
#define DEFAULT_RETRY_DELAY 1000000

struct per_page_stats {
  ulint n_recs;
  ulint data_size;
  ulint left_page_no;
  ulint right_page_no;
  per_page_stats(ulint n, ulint data, ulint left, ulint right) :
      n_recs(n), data_size(data), left_page_no(left), right_page_no(right) {}
  per_page_stats() : n_recs(0), data_size(0), left_page_no(0), right_page_no(0) {}
};

struct per_index_stats {
  unsigned long long pages;
  unsigned long long leaf_pages;
  ulint first_leaf_page;
  ulint count;
  ulint free_pages;
  ulint max_data_size;
  unsigned long long total_n_recs;
  unsigned long long total_data_bytes;

  /*!< first element for empty pages,
  last element for pages with more than logical_page_size */
  unsigned long long pages_in_size_range[SIZE_RANGES_FOR_PAGE+2];

  std::map<ulint, per_page_stats> leaves;

  per_index_stats():pages(0), leaf_pages(0), first_leaf_page(0),
                    count(0), free_pages(0), max_data_size(0), total_n_recs(0),
                    total_data_bytes(0)
  {
    memset(pages_in_size_range, 0, sizeof(pages_in_size_range));
  }
};

std::map<unsigned long long, per_index_stats> index_ids;

bool encrypted = false;

/* Get the page size of the filespace from the filespace header. */
static
my_bool
get_page_size(
/*==========*/
  FILE*  f,                     /*!< in: file pointer, must be open
                                         and set to start of file */
  byte* buf,                    /*!< in: buffer used to read the page */
  ulong* logical_page_size,     /*!< out: Logical/Uncompressed page size */
  ulong* physical_page_size)    /*!< out: Physical/Commpressed page size */
{
  ulong flags;

  int bytes= fread(buf, 1, UNIV_PAGE_SIZE_MIN, f);

  if (ferror(f))
  {
    perror("Error reading file header");
    return FALSE;
  }

  if (bytes != UNIV_PAGE_SIZE_MIN)
  {
    fprintf(stderr, "Error; Was not able to read the minimum page size ");
    fprintf(stderr, "of %d bytes.  Bytes read was %d\n", UNIV_PAGE_SIZE_MIN, bytes);
    return FALSE;
  }

  rewind(f);

  flags = mach_read_from_4(buf + FIL_PAGE_DATA + FSP_SPACE_FLAGS);

  /* srv_page_size is used by InnoDB code as UNIV_PAGE_SIZE */
  srv_page_size = *logical_page_size = fsp_flags_get_page_size(flags);

  /* fsp_flags_get_zip_size() will return zero if not compressed. */
  *physical_page_size = fsp_flags_get_zip_size(flags);
  if (*physical_page_size == 0)
  {
    *physical_page_size= *logical_page_size;
  }
  else
  {
    compressed= true;
  }


  return TRUE;
}


/* command line argument to do page checks (that's it) */
/* another argument to specify page ranges... seek to right spot and go from there */

static struct my_option innochecksum_options[] =
{
  {"help", '?', "Displays this help and exits.",
    0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"info", 'I', "Synonym for --help.",
    0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"version", 'V', "Displays version information and exits.",
    0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"verbose", 'v', "Verbose (prints progress every 5 seconds).",
    &verbose, &verbose, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"debug", 'd', "Debug mode (prints checksums for each page, implies verbose).",
    &debug, &debug, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"skip_corrupt", 'u', "Skip corrupt pages.",
    &skip_corrupt, &skip_corrupt, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"count", 'c', "Print the count of pages in the file.",
    &just_count, &just_count, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"start_page", 's', "Start on this page number (0 based).",
    &start_page, &start_page, 0, GET_ULONG, REQUIRED_ARG,
    0, 0, (longlong) 2L*1024L*1024L*1024L, 0, 1, 0},
  {"end_page", 'e', "End at this page number (0 based).",
    &end_page, &end_page, 0, GET_ULONG, REQUIRED_ARG,
    0, 0, (longlong) 2L*1024L*1024L*1024L, 0, 1, 0},
  {"page", 'p', "Check only this page (0 based).",
    &do_page, &do_page, 0, GET_ULONG, REQUIRED_ARG,
    0, 0, (longlong) 2L*1024L*1024L*1024L, 0, 1, 0},
  {"per_page_details", 'i', "Print out per-page detail information.",
    &per_page_details, &per_page_details, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0}
    ,
  {"leaf", 'l', "Examine leaf index pages",
    &do_leaf, &do_leaf, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"merge", 'm', "leaf page count if merge given number of consecutive pages",
   &n_merge, &n_merge, 0, GET_ULONG, REQUIRED_ARG,
   0, 0, (longlong)10L, 0, 1, 0},
  {0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};

static void print_version(void)
{
  printf("%s Ver %s, for %s (%s)\n",
         my_progname, INNODB_VERSION_STR,
         SYSTEM_TYPE, MACHINE_TYPE);
}

static void usage(void)
{
  print_version();
  puts(ORACLE_WELCOME_COPYRIGHT_NOTICE("2000"));
  printf("InnoDB offline file checksum utility.\n");
  printf("Usage: %s [-c] [-s <start page>] [-e <end page>] [-p <page>] [-v] [-d] <filename>\n", my_progname);
  my_print_help(innochecksum_options);
  my_print_variables(innochecksum_options);
}

extern "C" my_bool
innochecksum_get_one_option(
/*========================*/
  int optid,
  const struct my_option *opt __attribute__((unused)),
  char *argument __attribute__((unused)))
{
  switch (optid) {
  case 'd':
    verbose=1;	/* debug implies verbose... */
    break;
  case 'e':
    use_end_page= 1;
    break;
  case 'p':
    end_page= start_page= do_page;
    use_end_page= 1;
    do_one_page= 1;
    break;
  case 'V':
    print_version();
    exit(0);
    break;
  case 'I':
  case '?':
    usage();
    exit(0);
    break;
  }
  return 0;
}

static int get_options(
/*===================*/
  int *argc,
  char ***argv)
{
  int ho_error;

  if ((ho_error=handle_options(argc, argv, innochecksum_options, innochecksum_get_one_option)))
    exit(ho_error);

  /* The next arg must be the filename */
  if (!*argc)
  {
    usage();
    return 1;
  }
  return 0;
} /* get_options */

/*********************************************************************//**
Gets the file page type.
@return type; NOTE that if the type has not been written to page, the
return value not defined */
ulint
fil_page_get_type(
/*==============*/
       uchar*  page)   /*!< in: file page */
{
       return(mach_read_from_2(page + FIL_PAGE_TYPE));
}

/**************************************************************//**
Gets the index id field of a page.
@return        index id */
ib_uint64_t
btr_page_get_index_id(
/*==================*/
       uchar*  page)   /*!< in: index page */
{
       return(mach_read_from_8(page + PAGE_HEADER + PAGE_INDEX_ID));
}

/********************************************************//**
Gets the next index page number.
@return	next page number */
ulint
btr_page_get_next(
/*==============*/
  const page_t* page) /*!< in: index page */
{
  return(mach_read_from_4(page + FIL_PAGE_NEXT));
}

/********************************************************//**
Gets the previous index page number.
@return	prev page number */
ulint
btr_page_get_prev(
/*==============*/
  const page_t* page) /*!< in: index page */
{
  return(mach_read_from_4(page + FIL_PAGE_PREV));
}

void
parse_page(
/*=======*/
  uchar* page, /* in: buffer page */
  uchar* xdes) /* in: extend descriptor page */
{
       ib_uint64_t id;
       ulint x;
       ulint n_recs;
       ulint page_no;
       ulint left_page_no;
       ulint right_page_no;
       ulint data_bytes;
       int is_leaf;
       int size_range_id;

       switch (fil_page_get_type(page)) {
       case FIL_PAGE_INDEX:
               n_fil_page_index++;
               id = btr_page_get_index_id(page);
               n_recs = page_get_n_recs(page);
               page_no = page_get_page_no(page);
               left_page_no = btr_page_get_prev(page);
               right_page_no = btr_page_get_next(page);
               data_bytes = page_get_data_size(page);
               is_leaf = page_is_leaf(page);
               size_range_id = (data_bytes * SIZE_RANGES_FOR_PAGE
                                + logical_page_size - 1) /
                                logical_page_size;
               if (size_range_id > SIZE_RANGES_FOR_PAGE + 1) {
                 /* data_bytes is bigger than logical_page_size */
                 size_range_id = SIZE_RANGES_FOR_PAGE + 1;
               }
               if (per_page_details) {
                 printf("index %lu page %lu leaf %u n_recs %lu data_bytes %lu"
                         "\n", id, page_no, is_leaf, n_recs, data_bytes);
               }
               /* update per-index statistics */
               {
                 if (index_ids.count(id) == 0) {
                   index_ids[id] = per_index_stats();
                 }
		 std::map<unsigned long long, per_index_stats>::iterator it;
		 it = index_ids.find(id);
                 per_index_stats &index = (it->second);
                 uchar* des = xdes + XDES_ARR_OFFSET
                   + XDES_SIZE * ((page_no & (physical_page_size - 1))
                                  / FSP_EXTENT_SIZE);
                 if (xdes_get_bit(des, XDES_FREE_BIT,
                                  page_no % FSP_EXTENT_SIZE)) {
                   index.free_pages++;
                   return;
                 }
                 index.pages++;
                 if (is_leaf) {
                   index.leaf_pages++;
                   if (data_bytes > index.max_data_size) {
                     index.max_data_size = data_bytes;
                   }
		   struct per_page_stats pp(n_recs, data_bytes,
			   left_page_no, right_page_no);

                   index.leaves[page_no] = pp;

                   if (left_page_no == ULINT32_UNDEFINED) {
                     index.first_leaf_page = page_no;
                     index.count++;
                   }
                 }
                 index.total_n_recs += n_recs;
                 index.total_data_bytes += data_bytes;
                 index.pages_in_size_range[size_range_id] ++;
               }

               break;
       case FIL_PAGE_UNDO_LOG:
               if (per_page_details) {
                       printf("FIL_PAGE_UNDO_LOG\n");
               }
               n_fil_page_undo_log++;
               x = mach_read_from_2(page + TRX_UNDO_PAGE_HDR +
                                    TRX_UNDO_PAGE_TYPE);
               if (x == TRX_UNDO_INSERT)
                       n_undo_insert++;
               else if (x == TRX_UNDO_UPDATE)
                       n_undo_update++;
               else
                       n_undo_other++;

               x = mach_read_from_2(page + TRX_UNDO_SEG_HDR + TRX_UNDO_STATE);
               switch (x) {
                       case TRX_UNDO_ACTIVE: n_undo_state_active++; break;
                       case TRX_UNDO_CACHED: n_undo_state_cached++; break;
                       case TRX_UNDO_TO_FREE: n_undo_state_to_free++; break;
                       case TRX_UNDO_TO_PURGE: n_undo_state_to_purge++; break;
                       case TRX_UNDO_PREPARED: n_undo_state_prepared++; break;
                       default: n_undo_state_other++; break;
               }
               break;
       case FIL_PAGE_INODE:
               if (per_page_details) {
                       printf("FIL_PAGE_INODE\n");
               }
               n_fil_page_inode++;
               break;
       case FIL_PAGE_IBUF_FREE_LIST:
               if (per_page_details) {
                       printf("FIL_PAGE_IBUF_FREE_LIST\n");
               }
               n_fil_page_ibuf_free_list++;
               break;
       case FIL_PAGE_TYPE_ALLOCATED:
               if (per_page_details) {
                       printf("FIL_PAGE_TYPE_ALLOCATED\n");
               }
               n_fil_page_type_allocated++;
               break;
       case FIL_PAGE_IBUF_BITMAP:
               if (per_page_details) {
                       printf("FIL_PAGE_IBUF_BITMAP\n");
               }
               n_fil_page_ibuf_bitmap++;
               break;
       case FIL_PAGE_TYPE_SYS:
               if (per_page_details) {
                       printf("FIL_PAGE_TYPE_SYS\n");
               }
               n_fil_page_type_sys++;
               break;
       case FIL_PAGE_TYPE_TRX_SYS:
               if (per_page_details) {
                       printf("FIL_PAGE_TYPE_TRX_SYS\n");
               }
               n_fil_page_type_trx_sys++;
               break;
       case FIL_PAGE_TYPE_FSP_HDR:
               if (per_page_details) {
                       printf("FIL_PAGE_TYPE_FSP_HDR\n");
               }
               memcpy(xdes, page, physical_page_size);
               n_fil_page_type_fsp_hdr++;
               break;
       case FIL_PAGE_TYPE_XDES:
               if (per_page_details) {
                       printf("FIL_PAGE_TYPE_XDES\n");
               }
               memcpy(xdes, page, physical_page_size);
               n_fil_page_type_xdes++;
               break;
       case FIL_PAGE_TYPE_BLOB:
               if (per_page_details) {
                       printf("FIL_PAGE_TYPE_BLOB\n");
               }
               n_fil_page_type_blob++;
               break;
       case FIL_PAGE_TYPE_ZBLOB:
       case FIL_PAGE_TYPE_ZBLOB2:
               if (per_page_details) {
                       printf("FIL_PAGE_TYPE_ZBLOB/2\n");
               }
               n_fil_page_type_zblob++;
               break;
       case FIL_PAGE_PAGE_COMPRESSED:
	       if (per_page_details) {
		       printf("FIL_PAGE_PAGE_COMPRESSED\n");
	       }
	       n_fil_page_type_page_compressed++;
	       break;
       case FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED:
	       if (per_page_details) {
		       printf("FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED\n");
	       }
	       n_fil_page_type_page_compressed_encrypted++;
	       break;
       default:
               if (per_page_details) {
                       printf("FIL_PAGE_TYPE_OTHER\n");
               }
               n_fil_page_type_other++;
       }
}

void print_index_leaf_stats(unsigned long long id, const per_index_stats& index)
{
  ulint page_no = index.first_leaf_page;
  std::map<ulint, per_page_stats>::const_iterator it_page = index.leaves.find(page_no);
  printf("\nindex: %llu leaf page stats: n_pages = %llu\n",
         id, index.leaf_pages);
  printf("page_no\tdata_size\tn_recs\n");
  while (it_page != index.leaves.end()) {
    const per_page_stats& stat = it_page->second;
    printf("%lu\t%lu\t%lu\n", it_page->first, stat.data_size, stat.n_recs);
    page_no = stat.right_page_no;
    it_page = index.leaves.find(page_no);
  }
}

void defrag_analysis(unsigned long long id, const per_index_stats& index)
{
  // TODO: make it work for compressed pages too
  std::map<ulint, per_page_stats>::const_iterator it = index.leaves.find(index.first_leaf_page);
  ulint n_pages = 0;
  ulint n_leaf_pages = 0;
  while (it != index.leaves.end()) {
    ulint data_size_total = 0;
    for (ulong i = 0; i < n_merge; i++) {
      const per_page_stats& stat = it->second;
      n_leaf_pages ++;
      data_size_total += stat.data_size;
      it = index.leaves.find(stat.right_page_no);
      if (it == index.leaves.end()) {
        break;
      }
    }
    if (index.max_data_size) {
      n_pages += data_size_total / index.max_data_size;
      if (data_size_total % index.max_data_size != 0) {
        n_pages += 1;
      }
    }
  }
  if (index.leaf_pages)
    printf("count = %lu free = %lu\n", index.count, index.free_pages);
    printf("%llu\t\t%llu\t\t%lu\t\t%lu\t\t%lu\t\t%.2f\t%lu\n",
           id, index.leaf_pages, n_leaf_pages, n_merge, n_pages,
           1.0 - (double)n_pages / (double)n_leaf_pages, index.max_data_size);
}

void print_leaf_stats()
{
  printf("\n**************************************************\n");
  printf("index_id\t#leaf_pages\t#actual_leaf_pages\tn_merge\t"
         "#leaf_after_merge\tdefrag\n");
  for (std::map<unsigned long long, per_index_stats>::const_iterator it = index_ids.begin(); it != index_ids.end(); it++) {
    const per_index_stats& index = it->second;
    if (verbose) {
      print_index_leaf_stats(it->first, index);
    }
    if (n_merge) {
      defrag_analysis(it->first, index);
    }
  }
}

void
print_stats()
/*========*/
{
       unsigned long long i;

       printf("%d\tbad checksum\n", n_bad_checksum);
       printf("%d\tFIL_PAGE_INDEX\n", n_fil_page_index);
       printf("%d\tFIL_PAGE_UNDO_LOG\n", n_fil_page_undo_log);
       printf("%d\tFIL_PAGE_INODE\n", n_fil_page_inode);
       printf("%d\tFIL_PAGE_IBUF_FREE_LIST\n", n_fil_page_ibuf_free_list);
       printf("%d\tFIL_PAGE_TYPE_ALLOCATED\n", n_fil_page_type_allocated);
       printf("%d\tFIL_PAGE_IBUF_BITMAP\n", n_fil_page_ibuf_bitmap);
       printf("%d\tFIL_PAGE_TYPE_SYS\n", n_fil_page_type_sys);
       printf("%d\tFIL_PAGE_TYPE_TRX_SYS\n", n_fil_page_type_trx_sys);
       printf("%d\tFIL_PAGE_TYPE_FSP_HDR\n", n_fil_page_type_fsp_hdr);
       printf("%d\tFIL_PAGE_TYPE_XDES\n", n_fil_page_type_xdes);
       printf("%d\tFIL_PAGE_TYPE_BLOB\n", n_fil_page_type_blob);
       printf("%d\tFIL_PAGE_TYPE_ZBLOB\n", n_fil_page_type_zblob);
       printf("%d\tFIL_PAGE_PAGE_COMPRESSED\n", n_fil_page_type_page_compressed);
       printf("%d\tFIL_PAGE_PAGE_COMPRESSED_ENCRYPTED\n", n_fil_page_type_page_compressed_encrypted);
       printf("%d\tother\n", n_fil_page_type_other);
       printf("%d\tmax index_id\n", n_fil_page_max_index_id);
       printf("undo type: %d insert, %d update, %d other\n",
               n_undo_insert, n_undo_update, n_undo_other);
       printf("undo state: %d active, %d cached, %d to_free, %d to_purge,"
               " %d prepared, %d other\n", n_undo_state_active,
               n_undo_state_cached, n_undo_state_to_free,
               n_undo_state_to_purge, n_undo_state_prepared,
               n_undo_state_other);

       printf("index_id\t#pages\t\t#leaf_pages\t#recs_per_page"
               "\t#bytes_per_page\n");
       for (std::map<unsigned long long, per_index_stats>::const_iterator it = index_ids.begin(); it != index_ids.end(); it++) {
         const per_index_stats& index = it->second;
         printf("%lld\t\t%lld\t\t%lld\t\t%lld\t\t%lld\n",
                it->first, index.pages, index.leaf_pages,
                index.total_n_recs / index.pages,
                index.total_data_bytes / index.pages);
       }
       printf("\n");
       printf("index_id\tpage_data_bytes_histgram(empty,...,oversized)\n");
       for (std::map<unsigned long long, per_index_stats>::const_iterator it = index_ids.begin(); it != index_ids.end(); it++) {
         printf("%lld\t", it->first);
         const per_index_stats& index = it->second;
         for (i = 0; i < SIZE_RANGES_FOR_PAGE+2; i++) {
           printf("\t%lld", index.pages_in_size_range[i]);
         }
         printf("\n");
       }
       if (do_leaf) {
         print_leaf_stats();
       }
}

int main(int argc, char **argv)
{
  FILE* f;                       /* our input file */
  char* filename;                /* our input filename. */
  unsigned char *big_buf= 0, *buf;
  unsigned char *big_xdes= 0, *xdes;
  ulong bytes;                   /* bytes read count */
  ulint ct;                      /* current page number (0 based) */
  time_t now;                    /* current time */
  time_t lastt;                  /* last time */
  ulint oldcsum, oldcsumfield, csum, csumfield, crc32, logseq, logseqfield;
                                 /* ulints for checksum storage */
  struct stat st;                /* for stat, if you couldn't guess */
  unsigned long long int size;   /* size of file (has to be 64 bits) */
  ulint pages;                   /* number of pages in file */
  off_t offset= 0;
  int fd;

  printf("InnoDB offline file checksum utility.\n");

  ut_crc32_init();

  MY_INIT(argv[0]);

  if (get_options(&argc,&argv))
    exit(1);

  if (verbose)
    my_print_variables(innochecksum_options);

  /* The file name is not optional */
  filename = *argv;
  if (*filename == '\0')
  {
    fprintf(stderr, "Error; File name missing\n");
    goto error_out;
  }

  /* stat the file to get size and page count */
  if (stat(filename, &st))
  {
    fprintf(stderr, "Error; %s cannot be found\n", filename);
    goto error_out;
  }
  size= st.st_size;

  /* Open the file for reading */
  f= fopen(filename, "rb");
  if (f == NULL)
  {
    fprintf(stderr, "Error; %s cannot be opened", filename);
    perror(" ");
    goto error_out;
  }

  big_buf = (unsigned char *)malloc(2 * UNIV_PAGE_SIZE_MAX);
  if (big_buf == NULL)
  {
    fprintf(stderr, "Error; failed to allocate memory\n");
    perror("");
    goto error_f;
  }

  /* Make sure the page is aligned */
  buf = (unsigned char*)ut_align_down(big_buf
                                      + UNIV_PAGE_SIZE_MAX, UNIV_PAGE_SIZE_MAX);

  big_xdes = (unsigned char *)malloc(2 * UNIV_PAGE_SIZE_MAX);
  if (big_xdes == NULL)
  {
    fprintf(stderr, "Error; failed to allocate memory\n");
    perror("");
    goto error_big_buf;
  }

  /* Make sure the page is aligned */
  xdes = (unsigned char*)ut_align_down(big_xdes
                                      + UNIV_PAGE_SIZE_MAX, UNIV_PAGE_SIZE_MAX);


  if (!get_page_size(f, buf, &logical_page_size, &physical_page_size))
    goto error;

  if (compressed)
  {
    printf("Table is compressed\n");
    printf("Key block size is %lu\n", physical_page_size);
  }
  else
  {
    printf("Table is uncompressed\n");
    printf("Page size is %lu\n", physical_page_size);
  }

  pages= (ulint) (size / physical_page_size);

  if (just_count)
  {
    if (verbose)
      printf("Number of pages: ");
    printf("%lu\n", pages);
    goto ok;
  }
  else if (verbose)
  {
    printf("file %s = %llu bytes (%lu pages)...\n", filename, size, pages);
    if (do_one_page)
      printf("InnoChecksum; checking page %lu\n", do_page);
    else
      printf("InnoChecksum; checking pages in range %lu to %lu\n", start_page, use_end_page ? end_page : (pages - 1));
  }

#ifdef UNIV_LINUX
  if (posix_fadvise(fileno(f), 0, 0, POSIX_FADV_SEQUENTIAL) ||
      posix_fadvise(fileno(f), 0, 0, POSIX_FADV_NOREUSE))
  {
    perror("posix_fadvise failed");
  }
#endif

  /* seek to the necessary position */
  if (start_page)
  {
    fd= fileno(f);
    if (!fd)
    {
      perror("Error; Unable to obtain file descriptor number");
      goto error;
    }

    offset= (off_t)start_page * (off_t)physical_page_size;

    if (lseek(fd, offset, SEEK_SET) != offset)
    {
      perror("Error; Unable to seek to necessary offset");
      goto error;
    }
  }

  /* main checksumming loop */
  ct= start_page;
  lastt= 0;
  while (!feof(f))
  {
    int page_ok = 1;

    bytes= fread(buf, 1, physical_page_size, f);

    if (!bytes && feof(f))
      goto ok;

    if (ferror(f))
    {
      fprintf(stderr, "Error reading %lu bytes", physical_page_size);
      perror(" ");
      goto error;
    }

    ulint page_type = mach_read_from_2(buf+FIL_PAGE_TYPE);
    ulint key_version = mach_read_from_4(buf + FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);

    if (key_version && page_type != FIL_PAGE_PAGE_COMPRESSED) {
	    encrypted = true;
    } else {
	    encrypted = false;
    }

    ulint comp_method = 0;

    if (encrypted) {
	    comp_method = mach_read_from_2(buf+FIL_PAGE_DATA+FIL_PAGE_COMPRESSED_SIZE);
    } else {
	    comp_method = mach_read_from_8(buf+FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION);
    }

    ulint comp_size = mach_read_from_2(buf+FIL_PAGE_DATA);
    ib_uint32_t encryption_checksum = mach_read_from_4(buf+FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION + 4);


    if (page_type == FIL_PAGE_PAGE_COMPRESSED) {
      /* Page compressed tables do not have any checksum */
      if (debug)
	fprintf(stderr, "Page %lu page compressed with method %s real_size %lu\n", ct,
	        fil_get_compression_alg_name(comp_method), comp_size);
      page_ok = 1;
    } else if (compressed) {
        /* compressed pages */
	ulint crccsum = page_zip_calc_checksum(buf, physical_page_size, SRV_CHECKSUM_ALGORITHM_CRC32);
	ulint icsum = page_zip_calc_checksum(buf, physical_page_size,  SRV_CHECKSUM_ALGORITHM_INNODB);

        if (debug) {
	  if (key_version != 0) {
	    fprintf(stderr,
		    "Page %lu encrypted key_version %lu calculated = %lu; crc32 = %lu; recorded = %u\n",
		    ct, key_version, icsum, crccsum, encryption_checksum);
	  }
        }

	if (encrypted) {
	  if (encryption_checksum != 0 && crccsum != encryption_checksum && icsum != encryption_checksum) {
	    if (debug)
	      fprintf(stderr, "page %lu: compressed: calculated = %lu; crc32 = %lu; recorded = %u\n",
		      ct, icsum, crccsum, encryption_checksum);
	    fprintf(stderr, "Fail; page %lu invalid (fails compressed page checksum).\n", ct);
	  }
	} else {
          if (!page_zip_verify_checksum(buf, physical_page_size)) {
            fprintf(stderr, "Fail; page %lu invalid (fails compressed page checksum).\n", ct);
            if (!skip_corrupt)
              goto error;
            page_ok = 0;
          }
	}
    } else {
      if (key_version != 0) {
      /* Encrypted page */
        if (debug) {
	  if (page_type == FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED) {
	    fprintf(stderr,
		    "Page %lu page compressed with method %s real_size %lu and encrypted key_version %lu checksum %u\n",
		    ct, fil_get_compression_alg_name(comp_method), comp_size, key_version, encryption_checksum);
	  } else {
	    fprintf(stderr,
		    "Page %lu encrypted key_version %lu checksum %u\n",
		    ct, key_version, encryption_checksum);
	  }
        }
      }

      /* Page compressed tables do not contain FIL tailer */
      if (page_type != FIL_PAGE_PAGE_COMPRESSED_ENCRYPTED && page_type != FIL_PAGE_PAGE_COMPRESSED) {
        /* check the "stored log sequence numbers" */
         logseq= mach_read_from_4(buf + FIL_PAGE_LSN + 4);
        logseqfield= mach_read_from_4(buf + logical_page_size - FIL_PAGE_END_LSN_OLD_CHKSUM + 4);
        if (debug)
          printf("page %lu: log sequence number: first = %lu; second = %lu\n", ct, logseq, logseqfield);
        if (logseq != logseqfield)
        {
          fprintf(stderr, "Fail; page %lu invalid (fails log sequence number check)\n", ct);
          if (!skip_corrupt)
            goto error;
          page_ok = 0;
        }

        /* check old method of checksumming */
        oldcsum= buf_calc_page_old_checksum(buf);
        oldcsumfield= mach_read_from_4(buf + logical_page_size - FIL_PAGE_END_LSN_OLD_CHKSUM);
        if (debug)
          printf("page %lu: old style: calculated = %lu; recorded = %lu\n", ct, oldcsum, oldcsumfield);
        if (oldcsumfield != mach_read_from_4(buf + FIL_PAGE_LSN) && oldcsumfield != oldcsum)
        {
          fprintf(stderr, "Fail;  page %lu invalid (fails old style checksum)\n", ct);
          if (!skip_corrupt)
            goto error;
          page_ok = 0;
        }
      }

      /* now check the new method */
      csum= buf_calc_page_new_checksum(buf);
      crc32= buf_calc_page_crc32(buf);
      csumfield= mach_read_from_4(buf + FIL_PAGE_SPACE_OR_CHKSUM);

      if (key_version)
	      csumfield = encryption_checksum;

      if (debug)
        printf("page %lu: new style: calculated = %lu; crc32 = %lu; recorded = %lu\n",
               ct, csum, crc32, csumfield);
      if (csumfield != 0 && crc32 != csumfield && csum != csumfield)
      {
        fprintf(stderr, "Fail; page %lu invalid (fails innodb and crc32 checksum)\n", ct);
        if (!skip_corrupt)
          goto error;
        page_ok = 0;
      }
    }
    /* end if this was the last page we were supposed to check */
    if (use_end_page && (ct >= end_page))
      goto ok;

    if (per_page_details)
    {
      printf("page %ld ", ct);
    }

    /* do counter increase and progress printing */
    ct++;

    if (!page_ok)
    {
      if (per_page_details)
      {
        printf("BAD_CHECKSUM\n");
      }
      n_bad_checksum++;
      continue;
    }

    /* Can't parse compressed or/and encrypted pages */
    if (page_type != FIL_PAGE_PAGE_COMPRESSED && !encrypted) {
      parse_page(buf, xdes);
    }

    if (verbose)
    {
      if (ct % 64 == 0)
      {
        now= time(0);
        if (!lastt) lastt= now;
        if (now - lastt >= 1)
        {
          printf("page %lu okay: %.3f%% done\n", (ct - 1), (float) ct / pages * 100);
          lastt= now;
        }
      }
    }
  }

ok:
  if (!just_count)
    print_stats();
  free(big_xdes);
  free(big_buf);
  fclose(f);
  my_end(0);
  exit(0);

error:
  free(big_xdes);
error_big_buf:
  free(big_buf);
error_f:
  fclose(f);
error_out:
  my_end(0);
  exit(1);
}
