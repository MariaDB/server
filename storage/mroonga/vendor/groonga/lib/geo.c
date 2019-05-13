/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2009-2017 Brazil

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

#include "grn_geo.h"
#include "grn_pat.h"
#include "grn_util.h"

#include <string.h>
#include <stdlib.h>

#define GRN_GEO_POINT_IN_NORTH_EAST(point) \
  ((point)->latitude >= 0 && (point)->longitude >= 0)
#define GRN_GEO_POINT_IN_NORTH_WEST(point) \
  ((point)->latitude >= 0 && (point)->longitude < 0)
#define GRN_GEO_POINT_IN_SOUTH_WEST(point) \
  ((point)->latitude < 0 && (point)->longitude < 0)
#define GRN_GEO_POINT_IN_SOUTH_EAST(point) \
  ((point)->latitude < 0 && (point)->longitude >= 0)

#define GRN_GEO_LONGITUDE_IS_WRAPPED(top_left, bottom_right) \
  ((top_left)->longitude > 0 && (bottom_right)->longitude < 0)

typedef struct {
  grn_id id;
  double d;
} geo_entry;

typedef struct
{
  grn_geo_point key;
  int key_size;
} mesh_entry;

typedef struct {
  grn_obj *pat;
  grn_obj top_left_point_buffer;
  grn_obj bottom_right_point_buffer;
  grn_geo_point *top_left;
  grn_geo_point *bottom_right;
} in_rectangle_data;

typedef struct {
  grn_geo_point min;
  grn_geo_point max;
  int rectangle_common_bit;
  uint8_t rectangle_common_key[sizeof(grn_geo_point)];
} in_rectangle_area_data;

static int
compute_diff_bit(uint8_t *geo_key1, uint8_t *geo_key2)
{
  int i, j, diff_bit = 0;

  for (i = 0; i < sizeof(grn_geo_point); i++) {
    if (geo_key1[i] != geo_key2[i]) {
      diff_bit = 8;
      for (j = 0; j < 8; j++) {
        if ((geo_key1[i] & (1 << (7 - j))) != (geo_key2[i] & (1 << (7 - j)))) {
          diff_bit = j;
          break;
        }
      }
      break;
    }
  }

  return i * 8 + diff_bit;
}

static void
compute_min_and_max_key(uint8_t *key_base, int diff_bit,
                        uint8_t *key_min, uint8_t *key_max)
{
  int diff_byte, diff_bit_mask;

  diff_byte = diff_bit / 8;
  diff_bit_mask = 0xff >> (diff_bit % 8);

  if (diff_byte == sizeof(grn_geo_point)) {
    if (key_min) { grn_memcpy(key_min, key_base, diff_byte); }
    if (key_max) { grn_memcpy(key_max, key_base, diff_byte); }
  } else {
    if (key_min) {
      grn_memcpy(key_min, key_base, diff_byte + 1);
      key_min[diff_byte] &= ~diff_bit_mask;
      memset(key_min + diff_byte + 1, 0,
             sizeof(grn_geo_point) - diff_byte - 1);
    }

    if (key_max) {
      grn_memcpy(key_max, key_base, diff_byte + 1);
      key_max[diff_byte] |= diff_bit_mask;
      memset(key_max + diff_byte + 1, 0xff,
             sizeof(grn_geo_point) - diff_byte - 1);
    }
  }
}

static void
compute_min_and_max(grn_geo_point *base_point, int diff_bit,
                    grn_geo_point *geo_min, grn_geo_point *geo_max)
{
  uint8_t geo_key_base[sizeof(grn_geo_point)];
  uint8_t geo_key_min[sizeof(grn_geo_point)];
  uint8_t geo_key_max[sizeof(grn_geo_point)];

  grn_gton(geo_key_base, base_point, sizeof(grn_geo_point));
  compute_min_and_max_key(geo_key_base, diff_bit,
                          geo_min ? geo_key_min : NULL,
                          geo_max ? geo_key_max : NULL);
  if (geo_min) {
    grn_ntog((uint8_t *)geo_min, geo_key_min, sizeof(grn_geo_point));
  }
  if (geo_max) {
    grn_ntog((uint8_t *)geo_max, geo_key_max, sizeof(grn_geo_point));
  }
}

/* #define GEO_DEBUG */

#ifdef GEO_DEBUG
#include <stdio.h>

static void
inspect_mesh(grn_ctx *ctx, grn_geo_point *key, int key_size, int n)
{
  grn_geo_point min, max;

  printf("mesh: %d:%d\n", n, key_size);

  printf("key: ");
  grn_p_geo_point(ctx, key);

  compute_min_and_max(key, key_size, &min, &max);
  printf("min: ");
  grn_p_geo_point(ctx, &min);
  printf("max: ");
  grn_p_geo_point(ctx, &max);
}

static void
inspect_mesh_entry(grn_ctx *ctx, mesh_entry *entries, int n)
{
  mesh_entry *entry;

  entry = entries + n;
  inspect_mesh(ctx, &(entry->key), entry->key_size, n);
}

static void
inspect_tid(grn_ctx *ctx, grn_id tid, grn_geo_point *point, double d)
{
  printf("tid: %d:%g", tid, d);
  grn_p_geo_point(ctx, point);
}

static void
inspect_key(grn_ctx *ctx, uint8_t *key)
{
  int i;
  for (i = 0; i < 8; i++) {
    int j;
    for (j = 0; j < 8; j++) {
      printf("%d", (key[i] & (1 << (7 - j))) >> (7 - j));
    }
    printf(" ");
  }
  printf("\n");
}

static void
print_key_mark(grn_ctx *ctx, int target_bit)
{
  int i;

  for (i = 0; i < target_bit; i++) {
    printf(" ");
    if (i > 0 && i % 8 == 0) {
      printf(" ");
    }
  }
  if (i > 0 && i % 8 == 0) {
    printf(" ");
  }
  printf("^\n");
}

static void
inspect_cursor_entry(grn_ctx *ctx, grn_geo_cursor_entry *entry)
{
  grn_geo_point point;

  printf("entry: ");
  grn_ntog((uint8_t *)&point, entry->key, sizeof(grn_geo_point));
  grn_p_geo_point(ctx, &point);
  inspect_key(ctx, entry->key);
  print_key_mark(ctx, entry->target_bit);

  printf("     target bit:    %d\n", entry->target_bit);

#define INSPECT_STATUS_FLAG(name) \
  ((entry->status_flags & GRN_GEO_CURSOR_ENTRY_STATUS_ ## name) ? "true" : "false")

  printf("   top included:    %s\n", INSPECT_STATUS_FLAG(TOP_INCLUDED));
  printf("bottom included:    %s\n", INSPECT_STATUS_FLAG(BOTTOM_INCLUDED));
  printf("  left included:    %s\n", INSPECT_STATUS_FLAG(LEFT_INCLUDED));
  printf(" right included:    %s\n", INSPECT_STATUS_FLAG(RIGHT_INCLUDED));
  printf(" latitude inner:    %s\n", INSPECT_STATUS_FLAG(LATITUDE_INNER));
  printf("longitude inner:    %s\n", INSPECT_STATUS_FLAG(LONGITUDE_INNER));

#undef INSPECT_STATUS_FLAG
}

static void
inspect_cursor_entry_targets(grn_ctx *ctx, grn_geo_cursor_entry *entry,
                             uint8_t *top_left_key, uint8_t *bottom_right_key,
                             grn_geo_cursor_entry *next_entry0,
                             grn_geo_cursor_entry *next_entry1)
{
  printf("entry:        ");
  inspect_key(ctx, entry->key);
  printf("top-left:     ");
  inspect_key(ctx, top_left_key);
  printf("bottom-right: ");
  inspect_key(ctx, bottom_right_key);
  printf("next-entry-0: ");
  inspect_key(ctx, next_entry0->key);
  printf("next-entry-1: ");
  inspect_key(ctx, next_entry1->key);
  printf("              ");
  print_key_mark(ctx, entry->target_bit + 1);
}
#else
#  define inspect_mesh(...)
#  define inspect_mesh_entry(...)
#  define inspect_tid(...)
#  define inspect_key(...)
#  define print_key_mark(...)
#  define inspect_cursor_entry(...)
#  define inspect_cursor_entry_targets(...)
#endif

static int
grn_geo_table_sort_detect_far_point(grn_ctx *ctx, grn_obj *table, grn_obj *index,
                                    grn_pat *pat, geo_entry *entries,
                                    grn_pat_cursor *pc, int n,
                                    grn_bool accessorp,
                                    grn_geo_point *base_point,
                                    double *d_far, int *diff_bit)
{
  int i = 0, diff_bit_prev, diff_bit_current;
  grn_id tid;
  geo_entry *ep, *p;
  double d;
  uint8_t geo_key_prev[sizeof(grn_geo_point)];
  uint8_t geo_key_curr[sizeof(grn_geo_point)];
  grn_geo_point point;

  *d_far = 0.0;
  grn_gton(geo_key_curr, base_point, sizeof(grn_geo_point));
  *diff_bit = sizeof(grn_geo_point) * 8;
  diff_bit_current = sizeof(grn_geo_point) * 8;
  grn_memcpy(&point, base_point, sizeof(grn_geo_point));
  ep = entries;
  inspect_mesh(ctx, &point, *diff_bit, -1);
  while ((tid = grn_pat_cursor_next(ctx, pc))) {
    grn_ii_cursor *ic = grn_ii_cursor_open(ctx, (grn_ii *)index, tid, 0, 0, 1, 0);
    if (ic) {
      grn_posting *posting;
      grn_gton(geo_key_prev, &point, sizeof(grn_geo_point));
      grn_pat_get_key(ctx, pat, tid, &point, sizeof(grn_geo_point));
      grn_gton(geo_key_curr, &point, sizeof(grn_geo_point));
      d = grn_geo_distance_rectangle_raw(ctx, base_point, &point);
      inspect_tid(ctx, tid, &point, d);

      diff_bit_prev = diff_bit_current;
      diff_bit_current = compute_diff_bit(geo_key_curr, geo_key_prev);
#ifdef GEO_DEBUG
      printf("diff: %d:%d:%d\n", *diff_bit, diff_bit_prev, diff_bit_current);
#endif
      if ((diff_bit_current % 2) == 1) {
        diff_bit_current--;
      }
      if (diff_bit_current < diff_bit_prev && *diff_bit > diff_bit_current) {
        if (i == n) {
          grn_ii_cursor_close(ctx, ic);
          break;
        }
        *diff_bit = diff_bit_current;
      }

      if (d > *d_far) {
        *d_far = d;
      }
      while ((posting = grn_ii_cursor_next(ctx, ic))) {
        grn_id rid = accessorp
          ? grn_table_get(ctx, table, &posting->rid, sizeof(grn_id))
          : posting->rid;
        if (rid) {
          for (p = ep; entries < p && p[-1].d > d; p--) {
            p->id = p[-1].id;
            p->d = p[-1].d;
          }
          p->id = rid;
          p->d = d;
          if (i < n) {
            ep++;
            i++;
          }
        }
      }
      grn_ii_cursor_close(ctx, ic);
    }
  }

  return i;
}

typedef enum {
  MESH_LEFT_TOP,
  MESH_RIGHT_TOP,
  MESH_RIGHT_BOTTOM,
  MESH_LEFT_BOTTOM
} mesh_position;

/*
  meshes should have
    86 >= spaces when include_base_point_hash == GRN_FALSE,
    87 >= spaces when include_base_point_hash == GRN_TRUE.
*/
static int
grn_geo_get_meshes_for_circle(grn_ctx *ctx, grn_geo_point *base_point,
                              double d_far, int diff_bit,
                              int include_base_point_mesh,
                              mesh_entry *meshes)
{
  double d;
  int n_meshes;
  int lat_diff, lng_diff;
  mesh_position position;
  grn_geo_point geo_base, geo_min, geo_max;

  compute_min_and_max(base_point, diff_bit - 2, &geo_min, &geo_max);

  lat_diff = (geo_max.latitude - geo_min.latitude + 1) / 2;
  lng_diff = (geo_max.longitude - geo_min.longitude + 1) / 2;
  geo_base.latitude = geo_min.latitude + lat_diff;
  geo_base.longitude = geo_min.longitude + lng_diff;
  if (base_point->latitude >= geo_base.latitude) {
    if (base_point->longitude >= geo_base.longitude) {
      position = MESH_RIGHT_TOP;
    } else {
      position = MESH_LEFT_TOP;
    }
  } else {
    if (base_point->longitude >= geo_base.longitude) {
      position = MESH_RIGHT_BOTTOM;
    } else {
      position = MESH_LEFT_BOTTOM;
    }
  }
  /*
    base_point: b
    geo_min: i
    geo_max: a
    geo_base: x: must be at the left bottom in the top right mesh.

    e.g.: base_point is at the left bottom mesh case:
              +------+------+
              |      |     a|
              |      |x     |
             ^+------+------+
             ||      |      |
    lng_diff || b    |      |
            \/i------+------+
              <------>
              lat_diff

    grn_min + lat_diff -> the right mesh.
    grn_min + lng_diff -> the top mesh.
   */
#ifdef GEO_DEBUG
  grn_p_geo_point(ctx, base_point);
  printf("base: ");
  grn_p_geo_point(ctx, &geo_base);
  printf("min:  ");
  grn_p_geo_point(ctx, &geo_min);
  printf("max:  ");
  grn_p_geo_point(ctx, &geo_max);
  printf("diff: %d (%d, %d)\n", diff_bit, lat_diff, lng_diff);
  switch (position) {
  case MESH_LEFT_TOP :
    printf("position: left-top\n");
    break;
  case MESH_RIGHT_TOP :
    printf("position: right-top\n");
    break;
  case MESH_RIGHT_BOTTOM :
    printf("position: right-bottom\n");
    break;
  case MESH_LEFT_BOTTOM :
    printf("position: left-bottom\n");
    break;
  }
#endif

  n_meshes = 0;

#define add_mesh(lat_diff_,lng_diff_,key_size_) do {\
  meshes[n_meshes].key.latitude = geo_base.latitude + (lat_diff_);\
  meshes[n_meshes].key.longitude = geo_base.longitude + (lng_diff_);\
  meshes[n_meshes].key_size = key_size_;\
  n_meshes++;\
} while (0)

  if (include_base_point_mesh || position != MESH_LEFT_TOP) {
    add_mesh(0, -lng_diff, diff_bit);
  }
  if (include_base_point_mesh || position != MESH_RIGHT_TOP) {
    add_mesh(0, 0, diff_bit);
  }
  if (include_base_point_mesh || position != MESH_RIGHT_BOTTOM) {
    add_mesh(-lat_diff, 0, diff_bit);
  }
  if (include_base_point_mesh || position != MESH_LEFT_BOTTOM) {
    add_mesh(-lat_diff, -lng_diff, diff_bit);
  }

  /*
    b: base_point
    x: geo_base
    0-83: sub meshes. 0-83 are added order.

  j: -5  -4  -3  -2  -1   0   1   2   3   4
    +---+---+---+---+---+---+---+---+---+---+
    |74 |75 |76 |77 |78 |79 |80 |81 |82 |83 | 4
    +---+---+---+---+---+---+---+---+---+---+
    |64 |65 |66 |67 |68 |69 |70 |71 |72 |73 | 3
    +---+---+---+---+---+---+---+---+---+---+
    |54 |55 |56 |57 |58 |59 |60 |61 |62 |63 | 2
    +---+---+---+---+---+---+---+---+---+---+
    |48 |49 |50 |  b    |       |51 |52 |53 | 1
    +---+---+---+       |       +---+---+---+
    |42 |43 |44 |       |x      |45 |46 |47 | 0
    +---+---+---+-------+-------+---+---+---+
    |36 |37 |38 |       |       |39 |40 |41 | -1
    +---+---+---+  base meshes  +---+---+---+
    |30 |31 |32 |       |       |33 |34 |35 | -2
    +---+---+---+---+---+---+---+---+---+---+
    |20 |21 |22 |23 |24 |25 |26 |27 |28 |29 | -3
    +---+---+---+---+---+---+---+---+---+---+
    |10 |11 |12 |13 |14 |15 |16 |17 |18 |19 | -4
    +---+---+---+---+---+---+---+---+---+---+
    | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | -5
    +---+---+---+---+---+---+---+---+---+---+
                                              i
  */
  {
    int i, j, n_sub_meshes, lat, lat_min, lat_max, lng, lng_min, lng_max;
    n_sub_meshes = 0;
    for (i = -5; i < 5; i++) {
      lat_min = ((lat_diff + 1) / 2) * i;
      lat_max = ((lat_diff + 1) / 2) * (i + 1) - 1;
      for (j = -5; j < 5; j++) {
        if (-3 < i && i < 2 && -3 < j && j < 2) {
          continue;
        }
        lng_min = ((lng_diff + 1) / 2) * j;
        lng_max = ((lng_diff + 1) / 2) * (j + 1) - 1;
        if (base_point->latitude <= geo_base.latitude + lat_min) {
          lat = geo_base.latitude + lat_min;
        } else if (geo_base.latitude + lat_max < base_point->latitude) {
          lat = geo_base.latitude + lat_max;
        } else {
          lat = base_point->latitude;
        }
        if (base_point->longitude <= geo_base.longitude + lng_min) {
          lng = geo_base.longitude + lng_min;
        } else if (geo_base.longitude + lng_max < base_point->longitude) {
          lng = geo_base.longitude + lng_max;
        } else {
          lng = base_point->longitude;
        }
        meshes[n_meshes].key.latitude = lat;
        meshes[n_meshes].key.longitude = lng;
        d = grn_geo_distance_rectangle_raw(ctx, base_point,
                                           &(meshes[n_meshes].key));
        if (d < d_far) {
#ifdef GEO_DEBUG
          printf("sub-mesh: %d: (%d,%d): (%d,%d;%d,%d)\n",
                 n_sub_meshes, base_point->latitude, base_point->longitude,
                 geo_base.latitude + lat_min,
                 geo_base.latitude + lat_max,
                 geo_base.longitude + lng_min,
                 geo_base.longitude + lng_max);
          grn_p_geo_point(ctx, &(meshes[n_meshes].key));
#endif
          meshes[n_meshes].key_size = diff_bit + 2;
          n_meshes++;
        }
        n_sub_meshes++;
      }
    }
  }

#undef add_mesh

  return n_meshes;
}

static int
grn_geo_table_sort_collect_points(grn_ctx *ctx, grn_obj *table, grn_obj *index,
                                  grn_pat *pat,
                                  geo_entry *entries, int n_entries,
                                  int n, grn_bool accessorp,
                                  grn_geo_point *base_point,
                                  double d_far, int diff_bit)
{
  int n_meshes;
  mesh_entry meshes[86];
  geo_entry *ep, *p;

  n_meshes = grn_geo_get_meshes_for_circle(ctx, base_point, d_far, diff_bit,
                                           GRN_FALSE, meshes);

  ep = entries + n_entries;
  while (n_meshes--) {
    grn_id tid;
    grn_pat_cursor *pc = grn_pat_cursor_open(ctx, pat,
                                             &(meshes[n_meshes].key),
                                             meshes[n_meshes].key_size,
                                             NULL, 0,
                                             0, -1,
                                             GRN_CURSOR_PREFIX|GRN_CURSOR_SIZE_BY_BIT);
    inspect_mesh_entry(ctx, meshes, n_meshes);
    if (pc) {
      while ((tid = grn_pat_cursor_next(ctx, pc))) {
        grn_ii_cursor *ic = grn_ii_cursor_open(ctx, (grn_ii *)index, tid, 0, 0, 1, 0);
        if (ic) {
          double d;
          grn_geo_point pos;
          grn_posting *posting;
          grn_pat_get_key(ctx, pat, tid, &pos, sizeof(grn_geo_point));
          d = grn_geo_distance_rectangle_raw(ctx, base_point, &pos);
          inspect_tid(ctx, tid, &pos, d);
          while ((posting = grn_ii_cursor_next(ctx, ic))) {
            grn_id rid = accessorp
              ? grn_table_get(ctx, table, &posting->rid, sizeof(grn_id))
              : posting->rid;
            if (rid) {
              for (p = ep; entries < p && p[-1].d > d; p--) {
                p->id = p[-1].id;
                p->d = p[-1].d;
              }
              p->id = rid;
              p->d = d;
              if (n_entries < n) {
                ep++;
                n_entries++;
              }
            }
          }
          grn_ii_cursor_close(ctx, ic);
        }
      }
      grn_pat_cursor_close(ctx, pc);
    }
  }
  return n_entries;
}

static inline grn_obj *
find_geo_sort_index(grn_ctx *ctx, grn_obj *key)
{
  grn_obj *index = NULL;

  if (GRN_ACCESSORP(key)) {
    grn_accessor *accessor = (grn_accessor *)key;
    if (accessor->action != GRN_ACCESSOR_GET_KEY) {
      return NULL;
    }
    if (!(DB_OBJ(accessor->obj)->id & GRN_OBJ_TMP_OBJECT)) {
      return NULL;
    }
    if (accessor->obj->header.type != GRN_TABLE_HASH_KEY) {
      return NULL;
    }
    if (!accessor->next) {
      return NULL;
    }
    grn_column_index(ctx, accessor->next->obj, GRN_OP_LESS, &index, 1, NULL);
  } else {
    grn_column_index(ctx, key, GRN_OP_LESS, &index, 1, NULL);
  }

  return index;
}

static inline int
grn_geo_table_sort_by_distance(grn_ctx *ctx,
                               grn_obj *table,
                               grn_obj *index,
                               grn_pat *pat,
                               grn_pat_cursor *pc,
                               grn_bool accessorp,
                               grn_geo_point *base_point,
                               int offset,
                               int limit,
                               grn_obj *result)
{
  int n_entries = 0, e = offset + limit;
  geo_entry *entries;

  if ((entries = GRN_MALLOC(sizeof(geo_entry) * (e + 1)))) {
    int n, diff_bit;
    double d_far;
    geo_entry *ep;
    grn_bool need_not_indexed_records;
    grn_hash *indexed_records = NULL;

    n = grn_geo_table_sort_detect_far_point(ctx, table, index, pat,
                                            entries, pc, e, accessorp,
                                            base_point,
                                            &d_far, &diff_bit);
    if (diff_bit > 0) {
      n = grn_geo_table_sort_collect_points(ctx, table, index, pat,
                                            entries, n, e, accessorp,
                                            base_point, d_far, diff_bit);
    }
    need_not_indexed_records = offset + limit > n;
    if (need_not_indexed_records) {
      indexed_records = grn_hash_create(ctx, NULL, sizeof(grn_id), 0,
                                        GRN_OBJ_TABLE_HASH_KEY|GRN_HASH_TINY);
    }
    for (ep = entries + offset;
         n_entries < limit && ep < entries + n;
         n_entries++, ep++) {
      grn_id *sorted_id;
      if (!grn_array_add(ctx, (grn_array *)result, (void **)&sorted_id)) {
        if (indexed_records) {
          grn_hash_close(ctx, indexed_records);
          indexed_records = NULL;
        }
        break;
      }
      *sorted_id = ep->id;
      if (indexed_records) {
        grn_hash_add(ctx, indexed_records, &(ep->id), sizeof(grn_id),
                     NULL, NULL);
      }
    }
    GRN_FREE(entries);
    if (indexed_records) {
      GRN_TABLE_EACH(ctx, table, GRN_ID_NIL, GRN_ID_MAX, id, NULL, NULL, NULL, {
        if (!grn_hash_get(ctx, indexed_records, &id, sizeof(grn_id), NULL)) {
          grn_id *sorted_id;
          if (grn_array_add(ctx, (grn_array *)result, (void **)&sorted_id)) {
            *sorted_id = id;
          }
          n_entries++;
          if (n_entries == limit) {
            break;
          }
        };
      });
      grn_hash_close(ctx, indexed_records);
    }
  }

  return n_entries;
}

int
grn_geo_table_sort(grn_ctx *ctx, grn_obj *table, int offset, int limit,
                   grn_obj *result, grn_obj *column, grn_obj *geo_point)
{
  grn_obj *index;
  int i = 0;

  GRN_API_ENTER;

  if (offset < 0 || limit < 0) {
    unsigned int size;
    grn_rc rc;
    size = grn_table_size(ctx, table);
    rc = grn_normalize_offset_and_limit(ctx, size, &offset, &limit);
    if (rc != GRN_SUCCESS) {
      ERR(rc,
          "[sort][geo] failed to normalize offset and limit: "
          "offset:%d limit:%d table-size:%u",
          offset, limit, size);
      GRN_API_RETURN(i);
    }
  }

  if ((index = find_geo_sort_index(ctx, column))) {
    grn_id tid;
    grn_pat *pat = (grn_pat *)grn_ctx_at(ctx, index->header.domain);
    grn_id domain;
    grn_pat_cursor *pc;
    if (!pat) {
      char index_name[GRN_TABLE_MAX_KEY_SIZE];
      int index_name_size;
      char lexicon_name[GRN_TABLE_MAX_KEY_SIZE];
      int lexicon_name_size;
      index_name_size = grn_obj_name(ctx,
                                     index,
                                     index_name,
                                     GRN_TABLE_MAX_KEY_SIZE);
      lexicon_name_size = grn_table_get_key(ctx,
                                            grn_ctx_db(ctx),
                                            index->header.domain,
                                            lexicon_name,
                                            GRN_TABLE_MAX_KEY_SIZE);
      ERR(GRN_OBJECT_CORRUPT,
          "[sort][geo] lexicon is broken: <%.*s>: <%.*s>(%d)",
          index_name_size, index_name,
          lexicon_name_size, lexicon_name,
          index->header.domain);
      GRN_API_RETURN(i);
    }
    domain = pat->obj.header.domain;
    pc = grn_pat_cursor_open(ctx, pat, NULL, 0,
                             GRN_BULK_HEAD(geo_point),
                             GRN_BULK_VSIZE(geo_point),
                             0, -1, GRN_CURSOR_PREFIX);
    if (pc) {
      if (domain != GRN_DB_TOKYO_GEO_POINT && domain != GRN_DB_WGS84_GEO_POINT) {
        int e = offset + limit;
        while (i < e && (tid = grn_pat_cursor_next(ctx, pc))) {
          grn_ii_cursor *ic = grn_ii_cursor_open(ctx, (grn_ii *)index, tid, 0, 0, 1, 0);
          if (ic) {
            grn_posting *posting;
            while (i < e && (posting = grn_ii_cursor_next(ctx, ic))) {
              if (offset <= i) {
                grn_id *v;
                if (!grn_array_add(ctx, (grn_array *)result, (void **)&v)) { break; }
                *v = posting->rid;
              }
              i++;
            }
            grn_ii_cursor_close(ctx, ic);
          }
        }
      } else {
        grn_geo_point *base_point = (grn_geo_point *)GRN_BULK_HEAD(geo_point);
        i = grn_geo_table_sort_by_distance(ctx, table, index, pat,
                                           pc,
                                           GRN_ACCESSORP(column),
                                           base_point,
                                           offset, limit, result);
      }
      grn_pat_cursor_close(ctx, pc);
    }
  }
  GRN_API_RETURN(i);
}

grn_rc
grn_geo_resolve_approximate_type(grn_ctx *ctx, grn_obj *type_name,
                                 grn_geo_approximate_type *type)
{
  grn_rc rc;
  grn_obj approximate_type;

  GRN_TEXT_INIT(&approximate_type, 0);
  rc = grn_obj_cast(ctx, type_name, &approximate_type, GRN_FALSE);
  if (rc == GRN_SUCCESS) {
    const char *name;
    unsigned int size;
    name = GRN_TEXT_VALUE(&approximate_type);
    size = GRN_TEXT_LEN(&approximate_type);
    if ((strncmp("rectangle", name, size) == 0) ||
        (strncmp("rect", name, size) == 0)) {
      *type = GRN_GEO_APPROXIMATE_RECTANGLE;
    } else if ((strncmp("sphere", name, size) == 0) ||
               (strncmp("sphr", name, size) == 0)) {
      *type = GRN_GEO_APPROXIMATE_SPHERE;
    } else if ((strncmp("ellipsoid", name, size) == 0) ||
               (strncmp("ellip", name, size) == 0)) {
      *type = GRN_GEO_APPROXIMATE_ELLIPSOID;
    } else {
      ERR(GRN_INVALID_ARGUMENT,
          "geo distance approximate type must be one of "
          "[rectangle, rect, sphere, sphr, ellipsoid, ellip]"
          ": <%.*s>",
          size, name);
    }
  }
  GRN_OBJ_FIN(ctx, &approximate_type);

  return rc;
}

typedef double (*grn_geo_distance_raw_func)(grn_ctx *ctx,
                                            grn_geo_point *point1,
                                            grn_geo_point *point2);

grn_rc
grn_selector_geo_in_circle(grn_ctx *ctx, grn_obj *table, grn_obj *index,
                           int nargs, grn_obj **args,
                           grn_obj *res, grn_operator op)
{
  grn_geo_approximate_type type = GRN_GEO_APPROXIMATE_RECTANGLE;

  if (!(nargs == 4 || nargs == 5)) {
    ERR(GRN_INVALID_ARGUMENT,
        "geo_in_circle(): requires 3 or 4 arguments but was <%d> arguments",
        nargs - 1);
    return ctx->rc;
  }

  if (!index) {
    grn_obj *point_column;
    char column_name[GRN_TABLE_MAX_KEY_SIZE];
    int column_name_size;
    point_column = args[1];
    column_name_size = grn_obj_name(ctx, point_column,
                                    column_name, GRN_TABLE_MAX_KEY_SIZE);
    ERR(GRN_FUNCTION_NOT_IMPLEMENTED,
        "geo_in_circle(): index for <%.*s> is missing",
        column_name_size, column_name);
    return ctx->rc;
  }

  if (nargs == 5) {
    if (grn_geo_resolve_approximate_type(ctx, args[4], &type) != GRN_SUCCESS) {
      return ctx->rc;
    }
  }

  {
    grn_obj *center_point, *distance;
    center_point = args[2];
    distance = args[3];
    grn_geo_select_in_circle(ctx, index, center_point, distance, type, res, op);
  }

  return ctx->rc;
}

static grn_geo_distance_raw_func
grn_geo_resolve_distance_raw_func (grn_ctx *ctx,
                                   grn_geo_approximate_type approximate_type,
                                   grn_id domain)
{
  grn_geo_distance_raw_func distance_raw_func = NULL;

  switch (approximate_type) {
  case GRN_GEO_APPROXIMATE_RECTANGLE :
    distance_raw_func = grn_geo_distance_rectangle_raw;
    break;
  case GRN_GEO_APPROXIMATE_SPHERE :
    distance_raw_func = grn_geo_distance_sphere_raw;
    break;
  case GRN_GEO_APPROXIMATE_ELLIPSOID :
    if (domain == GRN_DB_WGS84_GEO_POINT) {
      distance_raw_func = grn_geo_distance_ellipsoid_raw_wgs84;
    } else {
      distance_raw_func = grn_geo_distance_ellipsoid_raw_tokyo;
    }
    break;
  default :
    break;
  }

  return distance_raw_func;
}

grn_rc
grn_geo_select_in_circle(grn_ctx *ctx, grn_obj *index,
                         grn_obj *center_point, grn_obj *distance,
                         grn_geo_approximate_type approximate_type,
                         grn_obj *res, grn_operator op)
{
  grn_id domain;
  double center_longitude, center_latitude;
  double d;
  grn_obj *pat, *point_on_circle = NULL, center_point_, point_on_circle_;
  grn_geo_point *center, on_circle;
  grn_geo_distance_raw_func distance_raw_func;
  pat = grn_ctx_at(ctx, index->header.domain);
  if (!pat) {
    char index_name[GRN_TABLE_MAX_KEY_SIZE];
    int index_name_size;
    char lexicon_name[GRN_TABLE_MAX_KEY_SIZE];
    int lexicon_name_size;
    index_name_size = grn_obj_name(ctx,
                                   index,
                                   index_name,
                                   GRN_TABLE_MAX_KEY_SIZE);
    lexicon_name_size = grn_table_get_key(ctx,
                                          grn_ctx_db(ctx),
                                          index->header.domain,
                                          lexicon_name,
                                          GRN_TABLE_MAX_KEY_SIZE);
    ERR(GRN_OBJECT_CORRUPT,
        "geo_in_circle(): lexicon is broken: <%.*s>: <%.*s>(%d)",
        index_name_size, index_name,
        lexicon_name_size, lexicon_name,
        index->header.domain);
    goto exit;
  }
  domain = pat->header.domain;
  if (domain != GRN_DB_TOKYO_GEO_POINT && domain != GRN_DB_WGS84_GEO_POINT) {
    char name[GRN_TABLE_MAX_KEY_SIZE];
    int name_size = 0;
    grn_obj *domain_object;
    domain_object = grn_ctx_at(ctx, domain);
    if (domain_object) {
      name_size = grn_obj_name(ctx, domain_object, name, GRN_TABLE_MAX_KEY_SIZE);
      grn_obj_unlink(ctx, domain_object);
    } else {
      grn_strcpy(name, GRN_TABLE_MAX_KEY_SIZE, "(null)");
      name_size = strlen(name);
    }
    ERR(GRN_INVALID_ARGUMENT,
        "geo_in_circle(): index table must be "
        "TokyoGeoPoint or WGS84GeoPoint key type table: <%.*s>",
        name_size, name);
    goto exit;
  }

  if (center_point->header.domain != domain) {
    GRN_OBJ_INIT(&center_point_, GRN_BULK, 0, domain);
    if (grn_obj_cast(ctx, center_point, &center_point_, GRN_FALSE)) { goto exit; }
    center_point = &center_point_;
  }
  center = GRN_GEO_POINT_VALUE_RAW(center_point);
  center_longitude = GRN_GEO_INT2RAD(center->longitude);
  center_latitude = GRN_GEO_INT2RAD(center->latitude);

  distance_raw_func = grn_geo_resolve_distance_raw_func(ctx,
                                                        approximate_type,
                                                        domain);
  if (!distance_raw_func) {
    ERR(GRN_INVALID_ARGUMENT,
        "unknown approximate type: <%d>", approximate_type);
    goto exit;
  }

  switch (distance->header.domain) {
  case GRN_DB_INT32 :
    d = GRN_INT32_VALUE(distance);
    on_circle.latitude = center->latitude + GRN_GEO_RAD2INT(d / (double)GRN_GEO_RADIUS);
    on_circle.longitude = center->longitude;
    break;
  case GRN_DB_UINT32 :
    d = GRN_UINT32_VALUE(distance);
    on_circle.latitude = center->latitude + GRN_GEO_RAD2INT(d / (double)GRN_GEO_RADIUS);
    on_circle.longitude = center->longitude;
    break;
  case GRN_DB_INT64 :
    d = GRN_INT64_VALUE(distance);
    on_circle.latitude = center->latitude + GRN_GEO_RAD2INT(d / (double)GRN_GEO_RADIUS);
    on_circle.longitude = center->longitude;
    break;
  case GRN_DB_UINT64 :
    d = GRN_UINT64_VALUE(distance);
    on_circle.latitude = center->latitude + GRN_GEO_RAD2INT(d / (double)GRN_GEO_RADIUS);
    on_circle.longitude = center->longitude;
    break;
  case GRN_DB_FLOAT :
    d = GRN_FLOAT_VALUE(distance);
    on_circle.latitude = center->latitude + GRN_GEO_RAD2INT(d / (double)GRN_GEO_RADIUS);
    on_circle.longitude = center->longitude;
    break;
  case GRN_DB_SHORT_TEXT :
  case GRN_DB_TEXT :
  case GRN_DB_LONG_TEXT :
    GRN_OBJ_INIT(&point_on_circle_, GRN_BULK, 0, domain);
    if (grn_obj_cast(ctx, distance, &point_on_circle_, GRN_FALSE)) { goto exit; }
    point_on_circle = &point_on_circle_;
    /* fallthru */
  case GRN_DB_TOKYO_GEO_POINT :
  case GRN_DB_WGS84_GEO_POINT :
    if (!point_on_circle) {
      if (domain != distance->header.domain) { /* todo */ goto exit; }
      point_on_circle = distance;
    }
    GRN_GEO_POINT_VALUE(point_on_circle,
                        on_circle.latitude, on_circle.longitude);
    d = distance_raw_func(ctx, center, &on_circle);
    if (point_on_circle == &point_on_circle_) {
      grn_obj_unlink(ctx, point_on_circle);
    }
    break;
  default :
    goto exit;
  }
  {
    int n_meshes, diff_bit;
    double d_far;
    mesh_entry meshes[87];
    uint8_t geo_key1[sizeof(grn_geo_point)];
    uint8_t geo_key2[sizeof(grn_geo_point)];

    d_far = grn_geo_distance_rectangle_raw(ctx, center, &on_circle);
    grn_gton(geo_key1, center, sizeof(grn_geo_point));
    grn_gton(geo_key2, &on_circle, sizeof(grn_geo_point));
    diff_bit = compute_diff_bit(geo_key1, geo_key2);
#ifdef GEO_DEBUG
    printf("center point: ");
    grn_p_geo_point(ctx, center);
    printf("point on circle: ");
    grn_p_geo_point(ctx, &on_circle);
    printf("diff:   %d\n", diff_bit);
#endif
    if ((diff_bit % 2) == 1) {
      diff_bit--;
    }
    n_meshes = grn_geo_get_meshes_for_circle(ctx, center,
                                             d_far, diff_bit, GRN_TRUE,
                                             meshes);
    while (n_meshes--) {
      grn_table_cursor *tc;
      tc = grn_table_cursor_open(ctx, pat,
                                 &(meshes[n_meshes].key),
                                 meshes[n_meshes].key_size,
                                 NULL, 0,
                                 0, -1,
                                 GRN_CURSOR_PREFIX|GRN_CURSOR_SIZE_BY_BIT);
      inspect_mesh_entry(ctx, meshes, n_meshes);
      if (tc) {
        grn_id tid;
        grn_geo_point point;
        while ((tid = grn_table_cursor_next(ctx, tc))) {
          double point_distance;
          grn_table_get_key(ctx, pat, tid, &point, sizeof(grn_geo_point));
          point_distance = distance_raw_func(ctx, &point, center);
          if (point_distance <= d) {
            inspect_tid(ctx, tid, &point, point_distance);
            grn_ii_at(ctx, (grn_ii *)index, tid, (grn_hash *)res, op);
          }
        }
        grn_table_cursor_close(ctx, tc);
      }
    }
  }
exit :
  grn_ii_resolve_sel_and(ctx, (grn_hash *)res, op);
  return ctx->rc;
}

grn_rc
grn_selector_geo_in_rectangle(grn_ctx *ctx, grn_obj *table, grn_obj *index,
                              int nargs, grn_obj **args,
                              grn_obj *res, grn_operator op)
{
  if (nargs == 4) {
    grn_obj *top_left_point, *bottom_right_point;
    top_left_point = args[2];
    bottom_right_point = args[3];
    grn_geo_select_in_rectangle(ctx, index,
                                top_left_point, bottom_right_point,
                                res, op);
  } else {
    ERR(GRN_INVALID_ARGUMENT,
        "geo_in_rectangle(): requires 3 arguments but was <%d> arguments",
        nargs - 1);
  }
  return ctx->rc;
}

static void
in_rectangle_data_fill(grn_ctx *ctx, grn_obj *index,
                       grn_obj *top_left_point,
                       grn_obj *bottom_right_point,
                       const char *process_name,
                       in_rectangle_data *data)
{
  grn_id domain;
  const char *domain_name;

  data->pat = grn_ctx_at(ctx, index->header.domain);
  if (!data->pat) {
    char index_name[GRN_TABLE_MAX_KEY_SIZE];
    int index_name_size;
    char lexicon_name[GRN_TABLE_MAX_KEY_SIZE];
    int lexicon_name_size;
    index_name_size = grn_obj_name(ctx,
                                   index,
                                   index_name,
                                   GRN_TABLE_MAX_KEY_SIZE);
    lexicon_name_size = grn_table_get_key(ctx,
                                          grn_ctx_db(ctx),
                                          index->header.domain,
                                          lexicon_name,
                                          GRN_TABLE_MAX_KEY_SIZE);
    ERR(GRN_OBJECT_CORRUPT,
        "%s: lexicon lexicon is broken: <%.*s>: <%.*s>(%d)",
        process_name,
        index_name_size, index_name,
        lexicon_name_size, lexicon_name,
        index->header.domain);
    return;
  }

  domain = data->pat->header.domain;
  if (domain != GRN_DB_TOKYO_GEO_POINT && domain != GRN_DB_WGS84_GEO_POINT) {
    char name[GRN_TABLE_MAX_KEY_SIZE];
    int name_size = 0;
    grn_obj *domain_object;
    domain_object = grn_ctx_at(ctx, domain);
    if (domain_object) {
      name_size = grn_obj_name(ctx, domain_object, name, GRN_TABLE_MAX_KEY_SIZE);
      grn_obj_unlink(ctx, domain_object);
    } else {
      grn_strcpy(name, GRN_TABLE_MAX_KEY_SIZE, "(null)");
      name_size = strlen(name);
    }
    ERR(GRN_INVALID_ARGUMENT,
        "%s: index table must be "
        "TokyoGeoPoint or WGS84GeoPoint key type table: <%.*s>",
        process_name, name_size, name);
    return;
  }

  if (domain == GRN_DB_TOKYO_GEO_POINT) {
    domain_name = "TokyoGeoPoint";
  } else {
    domain_name = "WGS84GeoPoint";
  }

  if (top_left_point->header.domain != domain) {
    grn_obj_reinit(ctx, &(data->top_left_point_buffer), domain, GRN_BULK);
    if (grn_obj_cast(ctx, top_left_point, &(data->top_left_point_buffer),
                     GRN_FALSE)) {
      ERR(GRN_INVALID_ARGUMENT,
          "%s: failed to cast to %s: <%.*s>",
          process_name, domain_name,
          (int)GRN_TEXT_LEN(top_left_point),
          GRN_TEXT_VALUE(top_left_point));
      return;
    }
    top_left_point = &(data->top_left_point_buffer);
  }
  data->top_left = GRN_GEO_POINT_VALUE_RAW(top_left_point);

  if (bottom_right_point->header.domain != domain) {
    grn_obj_reinit(ctx, &(data->bottom_right_point_buffer), domain, GRN_BULK);
    if (grn_obj_cast(ctx, bottom_right_point, &(data->bottom_right_point_buffer),
                     GRN_FALSE)) {
      ERR(GRN_INVALID_ARGUMENT,
          "%s: failed to cast to %s: <%.*s>",
          process_name, domain_name,
          (int)GRN_TEXT_LEN(bottom_right_point),
          GRN_TEXT_VALUE(bottom_right_point));
      return;
    }
    bottom_right_point = &(data->bottom_right_point_buffer);
  }
  data->bottom_right = GRN_GEO_POINT_VALUE_RAW(bottom_right_point);
}

static void
in_rectangle_data_validate(grn_ctx *ctx,
                           const char *process_name,
                           in_rectangle_data *data)
{
  grn_geo_point *top_left, *bottom_right;

  top_left = data->top_left;
  bottom_right = data->bottom_right;

  if (top_left->latitude >= GRN_GEO_MAX_LATITUDE) {
    ERR(GRN_INVALID_ARGUMENT,
        "%s: top left point's latitude is too big: "
        "<%d>(max:%d): (%d,%d) (%d,%d)",
        process_name,
        GRN_GEO_MAX_LATITUDE, top_left->latitude,
        top_left->latitude, top_left->longitude,
        bottom_right->latitude, bottom_right->longitude);
    return;
  }

  if (top_left->latitude <= GRN_GEO_MIN_LATITUDE) {
    ERR(GRN_INVALID_ARGUMENT,
        "%s: top left point's latitude is too small: "
        "<%d>(min:%d): (%d,%d) (%d,%d)",
        process_name,
        GRN_GEO_MIN_LATITUDE, top_left->latitude,
        top_left->latitude, top_left->longitude,
        bottom_right->latitude, bottom_right->longitude);
    return;
  }

  if (top_left->longitude >= GRN_GEO_MAX_LONGITUDE) {
    ERR(GRN_INVALID_ARGUMENT,
        "%s: top left point's longitude is too big: "
        "<%d>(max:%d): (%d,%d) (%d,%d)",
        process_name,
        GRN_GEO_MAX_LONGITUDE, top_left->longitude,
        top_left->latitude, top_left->longitude,
        bottom_right->latitude, bottom_right->longitude);
    return;
  }

  if (top_left->longitude <= GRN_GEO_MIN_LONGITUDE) {
    ERR(GRN_INVALID_ARGUMENT,
        "%s: top left point's longitude is too small: "
        "<%d>(min:%d): (%d,%d) (%d,%d)",
        process_name,
        GRN_GEO_MIN_LONGITUDE, top_left->longitude,
        top_left->latitude, top_left->longitude,
        bottom_right->latitude, bottom_right->longitude);
    return;
  }

  if (bottom_right->latitude >= GRN_GEO_MAX_LATITUDE) {
    ERR(GRN_INVALID_ARGUMENT,
        "%s: bottom right point's latitude is too big: "
        "<%d>(max:%d): (%d,%d) (%d,%d)",
        process_name,
        GRN_GEO_MAX_LATITUDE, bottom_right->latitude,
        top_left->latitude, top_left->longitude,
        bottom_right->latitude, bottom_right->longitude);
    return;
  }

  if (bottom_right->latitude <= GRN_GEO_MIN_LATITUDE) {
    ERR(GRN_INVALID_ARGUMENT,
        "%s: bottom right point's latitude is too small: "
        "<%d>(min:%d): (%d,%d) (%d,%d)",
        process_name,
        GRN_GEO_MIN_LATITUDE, bottom_right->latitude,
        top_left->latitude, top_left->longitude,
        bottom_right->latitude, bottom_right->longitude);
    return;
  }

  if (bottom_right->longitude >= GRN_GEO_MAX_LONGITUDE) {
    ERR(GRN_INVALID_ARGUMENT,
        "%s: bottom right point's longitude is too big: "
        "<%d>(max:%d): (%d,%d) (%d,%d)",
        process_name,
        GRN_GEO_MAX_LONGITUDE, bottom_right->longitude,
        top_left->latitude, top_left->longitude,
        bottom_right->latitude, bottom_right->longitude);
    return;
  }

  if (bottom_right->longitude <= GRN_GEO_MIN_LONGITUDE) {
    ERR(GRN_INVALID_ARGUMENT,
        "%s: bottom right point's longitude is too small: "
        "<%d>(min:%d): (%d,%d) (%d,%d)",
        process_name,
        GRN_GEO_MIN_LONGITUDE, bottom_right->longitude,
        top_left->latitude, top_left->longitude,
        bottom_right->latitude, bottom_right->longitude);
    return;
  }
}

static void
in_rectangle_area_data_compute(grn_ctx *ctx,
                               grn_geo_point *top_left,
                               grn_geo_point *bottom_right,
                               in_rectangle_area_data *data)
{
  int latitude_distance, longitude_distance;
  int diff_bit;
  grn_geo_point base;
  grn_geo_point *geo_point_input;
  uint8_t geo_key_input[sizeof(grn_geo_point)];
  uint8_t geo_key_base[sizeof(grn_geo_point)];
  uint8_t geo_key_top_left[sizeof(grn_geo_point)];
  uint8_t geo_key_bottom_right[sizeof(grn_geo_point)];

  latitude_distance = top_left->latitude - bottom_right->latitude;
  longitude_distance = bottom_right->longitude - top_left->longitude;
  if (latitude_distance > longitude_distance) {
    geo_point_input = bottom_right;
    base.latitude = bottom_right->latitude;
    base.longitude = bottom_right->longitude - longitude_distance;
  } else {
    geo_point_input = top_left;
    base.latitude = top_left->latitude - latitude_distance;
    base.longitude = top_left->longitude;
  }
  grn_gton(geo_key_input, geo_point_input, sizeof(grn_geo_point));
  grn_gton(geo_key_base, &base, sizeof(grn_geo_point));
  diff_bit = compute_diff_bit(geo_key_input, geo_key_base);
  compute_min_and_max(&base, diff_bit, &(data->min), &(data->max));

  grn_gton(geo_key_top_left, top_left, sizeof(grn_geo_point));
  grn_gton(geo_key_bottom_right, bottom_right, sizeof(grn_geo_point));
  data->rectangle_common_bit =
    compute_diff_bit(geo_key_top_left, geo_key_bottom_right) - 1;
  compute_min_and_max_key(geo_key_top_left, data->rectangle_common_bit + 1,
                          data->rectangle_common_key, NULL);

#ifdef GEO_DEBUG
  printf("base:         ");
  grn_p_geo_point(ctx, &base);
  printf("min:          ");
  grn_p_geo_point(ctx, &(data->min));
  printf("max:          ");
  grn_p_geo_point(ctx, &(data->max));
  printf("top-left:     ");
  grn_p_geo_point(ctx, top_left);
  printf("bottom-right: ");
  grn_p_geo_point(ctx, bottom_right);
  printf("rectangle-common-bit:%10d\n", data->rectangle_common_bit);
  printf("distance(latitude):  %10d\n", latitude_distance);
  printf("distance(longitude): %10d\n", longitude_distance);
#endif
}

static grn_rc
in_rectangle_data_prepare(grn_ctx *ctx, grn_obj *index,
                          grn_obj *top_left_point,
                          grn_obj *bottom_right_point,
                          const char *process_name,
                          in_rectangle_data *data)
{
  if (!index) {
    ERR(GRN_FUNCTION_NOT_IMPLEMENTED,
        "%s: index column is missing", process_name);
    goto exit;
  }

  in_rectangle_data_fill(ctx, index, top_left_point, bottom_right_point,
                         process_name, data);
  if (ctx->rc != GRN_SUCCESS) {
    goto exit;
  }

  in_rectangle_data_validate(ctx, process_name, data);
  if (ctx->rc != GRN_SUCCESS) {
    goto exit;
  }

exit :
  return ctx->rc;
}

#define SAME_BIT_P(a, b, n_bit)\
  ((((uint8_t *)(a))[(n_bit) / 8] & (1 << (7 - ((n_bit) % 8)))) ==\
   (((uint8_t *)(b))[(n_bit) / 8] & (1 << (7 - ((n_bit) % 8)))))

#define CURSOR_ENTRY_UPDATE_STATUS(entry, name, other_key) do {\
  if (SAME_BIT_P((entry)->key, (other_key), (entry)->target_bit)) {\
    (entry)->status_flags |= GRN_GEO_CURSOR_ENTRY_STATUS_ ## name;\
  } else {\
    (entry)->status_flags &= ~GRN_GEO_CURSOR_ENTRY_STATUS_ ## name;\
  }\
} while (0)

#define CURSOR_ENTRY_CHECK_STATUS(entry, name)\
  ((entry)->status_flags & GRN_GEO_CURSOR_ENTRY_STATUS_ ## name)
#define CURSOR_ENTRY_IS_INNER(entry)\
  (((entry)->status_flags &\
    (GRN_GEO_CURSOR_ENTRY_STATUS_LATITUDE_INNER |\
     GRN_GEO_CURSOR_ENTRY_STATUS_LONGITUDE_INNER)) ==\
   (GRN_GEO_CURSOR_ENTRY_STATUS_LATITUDE_INNER |\
    GRN_GEO_CURSOR_ENTRY_STATUS_LONGITUDE_INNER))
#define CURSOR_ENTRY_INCLUDED_IN_LATITUDE_DIRECTION(entry)\
  ((entry)->status_flags &\
   (GRN_GEO_CURSOR_ENTRY_STATUS_LATITUDE_INNER |\
    GRN_GEO_CURSOR_ENTRY_STATUS_TOP_INCLUDED |\
    GRN_GEO_CURSOR_ENTRY_STATUS_BOTTOM_INCLUDED))
#define CURSOR_ENTRY_INCLUDED_IN_LONGITUDE_DIRECTION(entry)\
  ((entry)->status_flags &\
   (GRN_GEO_CURSOR_ENTRY_STATUS_LONGITUDE_INNER |\
    GRN_GEO_CURSOR_ENTRY_STATUS_LEFT_INCLUDED |\
    GRN_GEO_CURSOR_ENTRY_STATUS_RIGHT_INCLUDED))

#define SET_N_BIT(a, n_bit)\
  (((uint8_t *)(a))[((n_bit) / 8)] ^= (1 << (7 - ((n_bit) % 8))))

#define N_BIT(a, n_bit)\
  ((((uint8_t *)(a))[((n_bit) / 8)] &\
    (1 << (7 - ((n_bit) % 8)))) >> (1 << (7 - ((n_bit) % 8))))

static grn_bool
extract_rectangle_in_area(grn_ctx *ctx,
                          grn_geo_area_type area_type,
                          const grn_geo_point *top_left,
                          const grn_geo_point *bottom_right,
                          grn_geo_point *area_top_left,
                          grn_geo_point *area_bottom_right)
{
  grn_bool out_of_area = GRN_FALSE;
  grn_bool cover_all_areas = GRN_FALSE;

  if ((GRN_GEO_POINT_IN_NORTH_WEST(top_left) &&
       GRN_GEO_POINT_IN_SOUTH_EAST(bottom_right)) ||
      (GRN_GEO_POINT_IN_NORTH_EAST(top_left) &&
       GRN_GEO_POINT_IN_SOUTH_WEST(bottom_right))) {
    cover_all_areas = GRN_TRUE;
  }

  switch (area_type) {
  case GRN_GEO_AREA_NORTH_EAST :
    if (cover_all_areas ||
        GRN_GEO_POINT_IN_NORTH_EAST(top_left) ||
        GRN_GEO_POINT_IN_NORTH_EAST(bottom_right)) {
      area_top_left->latitude     = MAX(top_left->latitude,      0);
      area_bottom_right->latitude = MAX(bottom_right->latitude,  0);
      if (GRN_GEO_LONGITUDE_IS_WRAPPED(top_left, bottom_right)) {
        area_top_left->longitude     = top_left->longitude;
        area_bottom_right->longitude = GRN_GEO_MAX_LONGITUDE;
      } else {
        area_top_left->longitude     = MAX(top_left->longitude,     0);
        area_bottom_right->longitude = MAX(bottom_right->longitude, 0);
      }
    } else {
      out_of_area = GRN_TRUE;
    }
    break;
  case GRN_GEO_AREA_NORTH_WEST :
    if (cover_all_areas ||
        GRN_GEO_POINT_IN_NORTH_WEST(top_left) ||
        GRN_GEO_POINT_IN_NORTH_WEST(bottom_right)) {
      area_top_left->latitude     = MAX(top_left->latitude,       0);
      area_bottom_right->latitude = MAX(bottom_right->latitude,   0);
      if (GRN_GEO_LONGITUDE_IS_WRAPPED(top_left, bottom_right)) {
        area_top_left->longitude     = GRN_GEO_MIN_LONGITUDE;
        area_bottom_right->longitude = bottom_right->longitude;
      } else {
        area_top_left->longitude     = MIN(top_left->longitude,     -1);
        area_bottom_right->longitude = MIN(bottom_right->longitude, -1);
      }
    } else {
      out_of_area = GRN_TRUE;
    }
    break;
  case GRN_GEO_AREA_SOUTH_WEST :
    if (cover_all_areas ||
        GRN_GEO_POINT_IN_SOUTH_WEST(top_left) ||
        GRN_GEO_POINT_IN_SOUTH_WEST(bottom_right)) {
      area_top_left->latitude     = MIN(top_left->latitude,      -1);
      area_bottom_right->latitude = MIN(bottom_right->latitude,  -1);
      if (GRN_GEO_LONGITUDE_IS_WRAPPED(top_left, bottom_right)) {
        area_top_left->longitude     = GRN_GEO_MIN_LONGITUDE;
        area_bottom_right->longitude = bottom_right->longitude;
      } else {
        area_top_left->longitude     = MIN(top_left->longitude,     -1);
        area_bottom_right->longitude = MIN(bottom_right->longitude, -1);
      }
    } else {
      out_of_area = GRN_TRUE;
    }
    break;
  case GRN_GEO_AREA_SOUTH_EAST :
    if (cover_all_areas ||
        GRN_GEO_POINT_IN_SOUTH_EAST(top_left) ||
        GRN_GEO_POINT_IN_SOUTH_EAST(bottom_right)) {
      area_top_left->latitude     = MIN(top_left->latitude,      -1);
      area_bottom_right->latitude = MIN(bottom_right->latitude,  -1);
      if (GRN_GEO_LONGITUDE_IS_WRAPPED(top_left, bottom_right)) {
        area_top_left->longitude     = top_left->longitude;
        area_bottom_right->longitude = GRN_GEO_MAX_LONGITUDE;
      } else {
        area_top_left->longitude     = MAX(top_left->longitude,      0);
        area_bottom_right->longitude = MAX(bottom_right->longitude,  0);
      }
    } else {
      out_of_area = GRN_TRUE;
    }
    break;
  default :
    out_of_area = GRN_TRUE;
    break;
  }

  return out_of_area;
}

static void
grn_geo_cursor_area_init(grn_ctx *ctx,
                         grn_geo_cursor_area *area,
                         grn_geo_area_type area_type,
                         const grn_geo_point *top_left,
                         const grn_geo_point *bottom_right)
{
  grn_geo_point area_top_left, area_bottom_right;
  in_rectangle_area_data data;
  grn_geo_cursor_entry *entry;
  grn_bool out_of_area;

  out_of_area = extract_rectangle_in_area(ctx,
                                          area_type,
                                          top_left,
                                          bottom_right,
                                          &area_top_left,
                                          &area_bottom_right);
  if (out_of_area) {
    area->current_entry = -1;
    return;
  }

  area->current_entry = 0;
  grn_memcpy(&(area->top_left), &area_top_left, sizeof(grn_geo_point));
  grn_memcpy(&(area->bottom_right), &area_bottom_right, sizeof(grn_geo_point));
  grn_gton(area->top_left_key, &area_top_left, sizeof(grn_geo_point));
  grn_gton(area->bottom_right_key, &area_bottom_right, sizeof(grn_geo_point));

  entry = &(area->entries[area->current_entry]);
  in_rectangle_area_data_compute(ctx,
                                 &area_top_left,
                                 &area_bottom_right,
                                 &data);
  entry->target_bit = data.rectangle_common_bit;
  grn_memcpy(entry->key, data.rectangle_common_key, sizeof(grn_geo_point));
  entry->status_flags =
    GRN_GEO_CURSOR_ENTRY_STATUS_TOP_INCLUDED |
    GRN_GEO_CURSOR_ENTRY_STATUS_BOTTOM_INCLUDED |
    GRN_GEO_CURSOR_ENTRY_STATUS_LEFT_INCLUDED |
    GRN_GEO_CURSOR_ENTRY_STATUS_RIGHT_INCLUDED;
  if (data.min.latitude == area_bottom_right.latitude &&
      data.max.latitude == area_top_left.latitude) {
    entry->status_flags |= GRN_GEO_CURSOR_ENTRY_STATUS_LATITUDE_INNER;
  }
  if (data.min.longitude == area_top_left.longitude &&
      data.max.longitude == area_bottom_right.longitude) {
    entry->status_flags |= GRN_GEO_CURSOR_ENTRY_STATUS_LONGITUDE_INNER;
  }
}

grn_obj *
grn_geo_cursor_open_in_rectangle(grn_ctx *ctx,
                                 grn_obj *index,
                                 grn_obj *top_left_point,
                                 grn_obj *bottom_right_point,
                                 int offset,
                                 int limit)
{
  grn_geo_cursor_in_rectangle *cursor = NULL;
  in_rectangle_data data;

  GRN_API_ENTER;
  GRN_VOID_INIT(&(data.top_left_point_buffer));
  GRN_VOID_INIT(&(data.bottom_right_point_buffer));
  if (in_rectangle_data_prepare(ctx, index, top_left_point, bottom_right_point,
                                "geo_in_rectangle()", &data)) {
    goto exit;
  }

  cursor = GRN_MALLOCN(grn_geo_cursor_in_rectangle, 1);
  if (!cursor) {
    ERR(GRN_NO_MEMORY_AVAILABLE,
        "[geo][cursor][in-rectangle] failed to allocate memory for geo cursor");
    goto exit;
  }

  cursor->pat = data.pat;
  cursor->index = index;
  grn_memcpy(&(cursor->top_left), data.top_left, sizeof(grn_geo_point));
  grn_memcpy(&(cursor->bottom_right), data.bottom_right, sizeof(grn_geo_point));
  cursor->pat_cursor = NULL;
  cursor->ii_cursor = NULL;
  cursor->offset = offset;
  cursor->rest = limit;

  cursor->current_area = GRN_GEO_AREA_NORTH_EAST;
  {
    grn_geo_area_type area_type;
    const grn_geo_point *top_left = &(cursor->top_left);
    const grn_geo_point *bottom_right = &(cursor->bottom_right);
    for (area_type = GRN_GEO_AREA_NORTH_EAST;
         area_type < GRN_GEO_AREA_LAST;
         area_type++) {
      grn_geo_cursor_area_init(ctx, &(cursor->areas[area_type]),
                               area_type, top_left, bottom_right);
    }
  }
  {
    char minimum_reduce_bit_env[GRN_ENV_BUFFER_SIZE];
    cursor->minimum_reduce_bit = 0;
    grn_getenv("GRN_GEO_IN_RECTANGLE_MINIMUM_REDUCE_BIT",
               minimum_reduce_bit_env,
               GRN_ENV_BUFFER_SIZE);
    if (minimum_reduce_bit_env[0]) {
      cursor->minimum_reduce_bit = atoi(minimum_reduce_bit_env);
    }
    if (cursor->minimum_reduce_bit < 1) {
      cursor->minimum_reduce_bit = 1;
    }
  }
  GRN_DB_OBJ_SET_TYPE(cursor, GRN_CURSOR_COLUMN_GEO_INDEX);
  {
    grn_obj *db;
    grn_id id;
    db = grn_ctx_db(ctx);
    id = grn_obj_register(ctx, db, NULL, 0);
    DB_OBJ(cursor)->header.domain = GRN_ID_NIL;
    DB_OBJ(cursor)->range = GRN_ID_NIL;
    grn_db_obj_init(ctx, db, id, DB_OBJ(cursor));
  }

exit :
  grn_obj_unlink(ctx, &(data.top_left_point_buffer));
  grn_obj_unlink(ctx, &(data.bottom_right_point_buffer));
  GRN_API_RETURN((grn_obj *)cursor);
}

static inline grn_bool
grn_geo_cursor_entry_next_push(grn_ctx *ctx,
                               grn_geo_cursor_in_rectangle *cursor,
                               grn_geo_cursor_entry *entry)
{
  grn_geo_cursor_entry *next_entry;
  grn_geo_point entry_base;
  grn_table_cursor *pat_cursor;
  grn_bool pushed = GRN_FALSE;

  grn_ntog((uint8_t*)(&entry_base), entry->key, sizeof(grn_geo_point));
  pat_cursor = grn_table_cursor_open(ctx,
                                     cursor->pat,
                                     &entry_base,
                                     entry->target_bit + 1,
                                     NULL, 0,
                                     0, -1,
                                     GRN_CURSOR_PREFIX|GRN_CURSOR_SIZE_BY_BIT);
  if (pat_cursor) {
    if (grn_table_cursor_next(ctx, pat_cursor)) {
      grn_geo_cursor_area *area;
      area = &(cursor->areas[cursor->current_area]);
      next_entry = &(area->entries[++area->current_entry]);
      grn_memcpy(next_entry, entry, sizeof(grn_geo_cursor_entry));
      pushed = GRN_TRUE;
    }
    grn_table_cursor_close(ctx, pat_cursor);
  }

  return pushed;
}

static inline grn_bool
grn_geo_cursor_entry_next(grn_ctx *ctx,
                          grn_geo_cursor_in_rectangle *cursor,
                          grn_geo_cursor_entry *entry)
{
  uint8_t *top_left_key;
  uint8_t *bottom_right_key;
  int max_target_bit = GRN_GEO_KEY_MAX_BITS - cursor->minimum_reduce_bit;
  grn_geo_cursor_area *area = NULL;

  while (cursor->current_area < GRN_GEO_AREA_LAST) {
    area = &(cursor->areas[cursor->current_area]);
    if (area->current_entry >= 0) {
      break;
    }
    cursor->current_area++;
    area = NULL;
  }

  if (!area) {
    return GRN_FALSE;
  }

  top_left_key = area->top_left_key;
  bottom_right_key = area->bottom_right_key;
  grn_memcpy(entry,
             &(area->entries[area->current_entry--]),
             sizeof(grn_geo_cursor_entry));
  while (GRN_TRUE) {
    grn_geo_cursor_entry next_entry0, next_entry1;
    grn_bool pushed = GRN_FALSE;

    /*
      top_left_key: tl
      bottom_right_key: br

      e.g.: top_left_key is at the top left sub mesh and
            bottom_right_key is at the bottom right sub mesh.
            top_left_key is also at the top left - bottom right
            sub-sub mesh and
            bottom_right_key is at the bottom right - bottom left
            sub-sub mesh.

      ^latitude +----+----+----+----+
      |       1 |1010|1011|1110|1111|
      |         |    |    |    |    |
      |    1    +----+----+----+----+
     \/       0 |1000|1001|1100|1101|
                |    | tl |    |    |
                +----+----+----+----+
              1 |0010|0011|0110|0111|
                |    |    |    |    |
           0    +----+----+----+----+
              0 |0000|0001|0100|0101|
                |    |    | br |    |
                +----+----+----+----+
                  0    1    0    1
                 |-------| |-------|
                     0         1
                <------>
                longitude

      entry.target_bit + 1      -> next_entry0
      entry.target_bit + 1 and entry.key ^ (entry.target_bit + 1) in bit
                                -> next_entry1

      entry: represents the biggest mesh.
             (1010, 1011, 1110, 1111,
              1000, 1001, 1100, 1101,
              0010, 0011, 0110, 0111,
              0000, 0001, 0100, 0101)
      next_entry0: represents bottom sub-mesh.
             (0010, 0011, 0110, 0111,
              0000, 0001, 0100, 0101)
      next_entry1: represents top sub-mesh.
             (1010, 1011, 1110, 1111,
              1000, 1001, 1100, 1101)

      entry->status_flags       = TOP_INCLUDED |
                                  BOTTOM_INCLUDED |
                                  LEFT_INCLUDED |
                                  RIGHT_INCLUDED
      next_entry0->status_flags = BOTTOM_INCLUDED |
                                  LEFT_INCLUDED |
                                  RIGHT_INCLUDED
      next_entry1->status_flags = TOP_INCLUDED |
                                  LEFT_INCLUDED |
                                  RIGHT_INCLUDED

      Both next_entry1 and next_entry0 are pushed to the stack in cursor.
    */

#ifdef GEO_DEBUG
    inspect_cursor_entry(ctx, entry);
#endif

    if (entry->target_bit >= max_target_bit) {
#ifdef GEO_DEBUG
      printf("%d: force stopping to reduce a mesh\n", entry->target_bit);
#endif
      break;
    }

    if (CURSOR_ENTRY_IS_INNER(entry)) {
#ifdef GEO_DEBUG
      printf("%d: inner entries\n", entry->target_bit);
#endif
      break;
    }

    grn_memcpy(&next_entry0, entry, sizeof(grn_geo_cursor_entry));
    next_entry0.target_bit++;
    grn_memcpy(&next_entry1, entry, sizeof(grn_geo_cursor_entry));
    next_entry1.target_bit++;
    SET_N_BIT(next_entry1.key, next_entry1.target_bit);

#ifdef GEO_DEBUG
    inspect_cursor_entry_targets(ctx, entry, top_left_key, bottom_right_key,
                                 &next_entry0, &next_entry1);
#endif

    if ((entry->target_bit + 1) % 2 == 0) {
      if (CURSOR_ENTRY_CHECK_STATUS(entry, TOP_INCLUDED)) {
        CURSOR_ENTRY_UPDATE_STATUS(&next_entry0, TOP_INCLUDED, top_left_key);
        CURSOR_ENTRY_UPDATE_STATUS(&next_entry1, TOP_INCLUDED, top_left_key);
      }
      if (CURSOR_ENTRY_CHECK_STATUS(entry, BOTTOM_INCLUDED)) {
        CURSOR_ENTRY_UPDATE_STATUS(&next_entry0, BOTTOM_INCLUDED,
                                   bottom_right_key);
        CURSOR_ENTRY_UPDATE_STATUS(&next_entry1, BOTTOM_INCLUDED,
                                   bottom_right_key);
      }

      if (CURSOR_ENTRY_CHECK_STATUS(entry, TOP_INCLUDED) &&
          !CURSOR_ENTRY_CHECK_STATUS(entry, BOTTOM_INCLUDED) &&
          CURSOR_ENTRY_CHECK_STATUS(&next_entry1, TOP_INCLUDED)) {
        next_entry0.status_flags |= GRN_GEO_CURSOR_ENTRY_STATUS_LATITUDE_INNER;
      } else if (!CURSOR_ENTRY_CHECK_STATUS(entry, TOP_INCLUDED) &&
                 CURSOR_ENTRY_CHECK_STATUS(entry, BOTTOM_INCLUDED) &&
                 CURSOR_ENTRY_CHECK_STATUS(&next_entry0, BOTTOM_INCLUDED)) {
        next_entry1.status_flags |= GRN_GEO_CURSOR_ENTRY_STATUS_LATITUDE_INNER;
      }

      if (CURSOR_ENTRY_INCLUDED_IN_LATITUDE_DIRECTION(&next_entry1)) {
        if (grn_geo_cursor_entry_next_push(ctx, cursor, &next_entry1)) {
          pushed = GRN_TRUE;
#ifdef GEO_DEBUG
          printf("%d: latitude: push 1\n", next_entry1.target_bit);
#endif
        }
      }
      if (CURSOR_ENTRY_INCLUDED_IN_LATITUDE_DIRECTION(&next_entry0)) {
        if (grn_geo_cursor_entry_next_push(ctx, cursor, &next_entry0)) {
          pushed = GRN_TRUE;
#ifdef GEO_DEBUG
          printf("%d: latitude: push 0\n", next_entry0.target_bit);
#endif
        }
      }
    } else {
      if (CURSOR_ENTRY_CHECK_STATUS(entry, RIGHT_INCLUDED)) {
        CURSOR_ENTRY_UPDATE_STATUS(&next_entry0, RIGHT_INCLUDED,
                                   bottom_right_key);
        CURSOR_ENTRY_UPDATE_STATUS(&next_entry1, RIGHT_INCLUDED,
                                   bottom_right_key);
      }
      if (CURSOR_ENTRY_CHECK_STATUS(entry, LEFT_INCLUDED)) {
        CURSOR_ENTRY_UPDATE_STATUS(&next_entry0, LEFT_INCLUDED, top_left_key);
        CURSOR_ENTRY_UPDATE_STATUS(&next_entry1, LEFT_INCLUDED, top_left_key);
      }

      if (CURSOR_ENTRY_CHECK_STATUS(entry, LEFT_INCLUDED) &&
          !CURSOR_ENTRY_CHECK_STATUS(entry, RIGHT_INCLUDED) &&
          CURSOR_ENTRY_CHECK_STATUS(&next_entry0, LEFT_INCLUDED)) {
        next_entry1.status_flags |= GRN_GEO_CURSOR_ENTRY_STATUS_LONGITUDE_INNER;
      } else if (!CURSOR_ENTRY_CHECK_STATUS(entry, LEFT_INCLUDED) &&
                 CURSOR_ENTRY_CHECK_STATUS(entry, RIGHT_INCLUDED) &&
                 CURSOR_ENTRY_CHECK_STATUS(&next_entry1, RIGHT_INCLUDED)) {
        next_entry0.status_flags |= GRN_GEO_CURSOR_ENTRY_STATUS_LONGITUDE_INNER;
      }

      if (CURSOR_ENTRY_INCLUDED_IN_LONGITUDE_DIRECTION(&next_entry1)) {
        if (grn_geo_cursor_entry_next_push(ctx, cursor, &next_entry1)) {
          pushed = GRN_TRUE;
#ifdef GEO_DEBUG
          printf("%d: longitude: push 1\n", next_entry1.target_bit);
#endif
        }
      }
      if (CURSOR_ENTRY_INCLUDED_IN_LONGITUDE_DIRECTION(&next_entry0)) {
        if (grn_geo_cursor_entry_next_push(ctx, cursor, &next_entry0)) {
          pushed = GRN_TRUE;
#ifdef GEO_DEBUG
          printf("%d: longitude: push 0\n", next_entry0.target_bit);
#endif
        }
      }
    }

    if (pushed) {
#ifdef GEO_DEBUG
      int i;

      printf("%d: pushed\n", entry->target_bit);
      printf("stack:\n");
      for (i = area->current_entry; i >= 0; i--) {
        grn_geo_cursor_entry *stack_entry;
        stack_entry = &(area->entries[i]);
        printf("%2d: ", i);
        inspect_key(ctx, stack_entry->key);
        printf("    ");
        print_key_mark(ctx, stack_entry->target_bit);
      }
#endif
      grn_memcpy(entry,
                 &(area->entries[area->current_entry--]),
                 sizeof(grn_geo_cursor_entry));
#ifdef GEO_DEBUG
      printf("%d: pop entry\n", entry->target_bit);
#endif
    } else {
      break;
    }
  }

#ifdef GEO_DEBUG
  printf("found:\n");
  inspect_cursor_entry(ctx, entry);
#endif

  return GRN_TRUE;
}

typedef grn_bool (*grn_geo_cursor_callback)(grn_ctx *ctx, grn_posting *posting, void *user_data);

static void
grn_geo_cursor_each(grn_ctx *ctx, grn_obj *geo_cursor,
                    grn_geo_cursor_callback callback, void *user_data)
{
  grn_geo_cursor_in_rectangle *cursor;
  grn_obj *pat;
  grn_table_cursor *pat_cursor;
  grn_ii *ii;
  grn_ii_cursor *ii_cursor;
  grn_posting *posting = NULL;
  grn_geo_point *current, *top_left, *bottom_right;
  grn_id index_id;

  cursor = (grn_geo_cursor_in_rectangle *)geo_cursor;
  if (cursor->rest == 0) {
    return;
  }

  pat = cursor->pat;
  pat_cursor = cursor->pat_cursor;
  ii = (grn_ii *)(cursor->index);
  ii_cursor = cursor->ii_cursor;
  current = &(cursor->current);
  top_left = &(cursor->top_left);
  bottom_right = &(cursor->bottom_right);

  while (GRN_TRUE) {
    if (!pat_cursor) {
      grn_geo_cursor_entry entry;
      grn_geo_point entry_base;
      if (!grn_geo_cursor_entry_next(ctx, cursor, &entry)) {
        cursor->rest = 0;
        return;
      }
      grn_ntog((uint8_t*)(&entry_base), entry.key, sizeof(grn_geo_point));
      if (!(cursor->pat_cursor = pat_cursor =
            grn_table_cursor_open(ctx,
                                  pat,
                                  &entry_base,
                                  entry.target_bit + 1,
                                  NULL, 0,
                                  0, -1,
                                  GRN_CURSOR_PREFIX|GRN_CURSOR_SIZE_BY_BIT))) {
        cursor->rest = 0;
        return;
      }
#ifdef GEO_DEBUG
      inspect_mesh(ctx, &entry_base, entry.target_bit, 0);
#endif
    }

    while (ii_cursor || (index_id = grn_table_cursor_next(ctx, pat_cursor))) {
      if (!ii_cursor) {
        grn_table_get_key(ctx, pat, index_id, current, sizeof(grn_geo_point));
        if (grn_geo_in_rectangle_raw(ctx, current, top_left, bottom_right)) {
          inspect_tid(ctx, index_id, current, 0);
          if (!(cursor->ii_cursor = ii_cursor =
                grn_ii_cursor_open(ctx,
                                   ii,
                                   index_id,
                                   GRN_ID_NIL,
                                   GRN_ID_MAX,
                                   ii->n_elements,
                                   0))) {
            continue;
          }
        } else {
          continue;
        }
      }

      while ((posting = grn_ii_cursor_next(ctx, ii_cursor))) {
        if (cursor->offset == 0) {
          grn_bool keep_each;
          keep_each = callback(ctx, posting, user_data);
          if (cursor->rest > 0) {
            if (--(cursor->rest) == 0) {
              keep_each = GRN_FALSE;
            }
          }
          if (!keep_each) {
            return;
          }
        } else {
          cursor->offset--;
        }
      }
      grn_ii_cursor_close(ctx, ii_cursor);
      cursor->ii_cursor = ii_cursor = NULL;
    }
    grn_table_cursor_close(ctx, pat_cursor);
    cursor->pat_cursor = pat_cursor = NULL;
  }
}

static grn_bool
grn_geo_cursor_next_callback(grn_ctx *ctx, grn_posting *posting,
                             void *user_data)
{
  grn_posting **return_posting = user_data;
  *return_posting = posting;
  return GRN_FALSE;
}

grn_posting *
grn_geo_cursor_next(grn_ctx *ctx, grn_obj *geo_cursor)
{
  grn_posting *posting = NULL;
  grn_geo_cursor_each(ctx, geo_cursor, grn_geo_cursor_next_callback, &posting);
  return (grn_posting *)posting;
}

grn_rc
grn_geo_cursor_close(grn_ctx *ctx, grn_obj *geo_cursor)
{
  grn_geo_cursor_in_rectangle *cursor;

  if (!geo_cursor) { return GRN_INVALID_ARGUMENT; }

  cursor = (grn_geo_cursor_in_rectangle *)geo_cursor;
  if (cursor->pat) { grn_obj_unlink(ctx, cursor->pat); }
  if (cursor->index) { grn_obj_unlink(ctx, cursor->index); }
  if (cursor->pat_cursor) { grn_table_cursor_close(ctx, cursor->pat_cursor); }
  if (cursor->ii_cursor) { grn_ii_cursor_close(ctx, cursor->ii_cursor); }
  GRN_FREE(cursor);

  return GRN_SUCCESS;
}

typedef struct {
  grn_hash *res;
  grn_operator op;
} grn_geo_select_in_rectangle_data;

static grn_bool
grn_geo_select_in_rectangle_callback(grn_ctx *ctx, grn_posting *posting,
                                     void *user_data)
{
  grn_geo_select_in_rectangle_data *data = user_data;
  grn_ii_posting_add(ctx, posting, data->res, data->op);
  return GRN_TRUE;
}

grn_rc
grn_geo_select_in_rectangle(grn_ctx *ctx, grn_obj *index,
                            grn_obj *top_left_point,
                            grn_obj *bottom_right_point,
                            grn_obj *res, grn_operator op)
{
  grn_obj *cursor;

  cursor = grn_geo_cursor_open_in_rectangle(ctx, index,
                                            top_left_point, bottom_right_point,
                                            0, -1);
  if (cursor) {
    grn_geo_select_in_rectangle_data data;
    data.res = (grn_hash *)res;
    data.op = op;
    grn_geo_cursor_each(ctx, cursor, grn_geo_select_in_rectangle_callback,
                        &data);
    grn_obj_unlink(ctx, cursor);
    grn_ii_resolve_sel_and(ctx, (grn_hash *)res, op);
  }

  return ctx->rc;
}

static grn_rc
geo_point_get(grn_ctx *ctx, grn_obj *pat, int flags, grn_geo_point *geo_point)
{
  grn_rc rc = GRN_SUCCESS;
  grn_id id;
  grn_table_cursor *cursor = NULL;

  cursor = grn_table_cursor_open(ctx, pat,
                                 NULL, 0,
                                 NULL, 0,
                                 0, 1,
                                 GRN_CURSOR_BY_KEY | flags);
  if (!cursor) {
    rc = ctx->rc;
    goto exit;
  }

  id = grn_table_cursor_next(ctx, cursor);
  if (id == GRN_ID_NIL) {
    rc = GRN_END_OF_DATA;
  } else {
    void *key;
    int key_size;
    key_size = grn_table_cursor_get_key(ctx, cursor, &key);
    grn_memcpy(geo_point, key, key_size);
  }

exit:
  if (cursor) {
    grn_table_cursor_close(ctx, cursor);
  }
  return rc;
}

uint32_t
grn_geo_estimate_size_in_rectangle(grn_ctx *ctx,
                                   grn_obj *index,
                                   grn_obj *top_left_point,
                                   grn_obj *bottom_right_point)
{
  uint32_t n = 0;
  int total_records;
  grn_rc rc;
  in_rectangle_data data;

  GRN_VOID_INIT(&(data.top_left_point_buffer));
  GRN_VOID_INIT(&(data.bottom_right_point_buffer));
  if (in_rectangle_data_prepare(ctx, index, top_left_point, bottom_right_point,
                                "grn_geo_estimate_in_rectangle()", &data)) {
    goto exit;
  }

  total_records = grn_table_size(ctx, data.pat);
  if (total_records > 0) {
    grn_geo_point min, max;
    int select_latitude_distance, select_longitude_distance;
    int total_latitude_distance, total_longitude_distance;
    double select_ratio;
    double estimated_n_records;
    in_rectangle_area_data area_data;

    rc = geo_point_get(ctx, data.pat, GRN_CURSOR_ASCENDING, &min);
    if (!rc) {
      rc = geo_point_get(ctx, data.pat, GRN_CURSOR_DESCENDING, &max);
    }
    if (rc) {
      if (rc == GRN_END_OF_DATA) {
        n = total_records;
        rc = GRN_SUCCESS;
      }
      goto exit;
    }

    in_rectangle_area_data_compute(ctx,
                                   data.top_left,
                                   data.bottom_right,
                                   &area_data);
    select_latitude_distance =
      abs(area_data.max.latitude - area_data.min.latitude);
    select_longitude_distance =
      abs(area_data.max.longitude - area_data.min.longitude);
    total_latitude_distance = abs(max.latitude - min.latitude);
    total_longitude_distance = abs(max.longitude - min.longitude);

    select_ratio = 1.0;
    if (select_latitude_distance < total_latitude_distance) {
      select_ratio *= ((double)select_latitude_distance /
                       (double)total_latitude_distance);
    }
    if (select_longitude_distance < total_longitude_distance) {
      select_ratio *= ((double)select_longitude_distance /
                       (double)total_longitude_distance);
    }
    estimated_n_records = ceil(total_records * select_ratio);
    n = (uint32_t)estimated_n_records;
  }

exit :
  grn_obj_unlink(ctx, &(data.top_left_point_buffer));
  grn_obj_unlink(ctx, &(data.bottom_right_point_buffer));
  return n;
}

int
grn_geo_estimate_in_rectangle(grn_ctx *ctx,
                              grn_obj *index,
                              grn_obj *top_left_point,
                              grn_obj *bottom_right_point)
{
  uint32_t size;

  size = grn_geo_estimate_size_in_rectangle(ctx,
                                            index,
                                            top_left_point,
                                            bottom_right_point);
  if (ctx->rc != GRN_SUCCESS) {
    return -1;
  }

  return size;
}

grn_bool
grn_geo_in_circle(grn_ctx *ctx, grn_obj *point, grn_obj *center,
                  grn_obj *radius_or_point,
                  grn_geo_approximate_type approximate_type)
{
  grn_bool r = GRN_FALSE;
  grn_obj center_, radius_or_point_;
  grn_id domain = point->header.domain;
  if (domain == GRN_DB_TOKYO_GEO_POINT || domain == GRN_DB_WGS84_GEO_POINT) {
    grn_geo_distance_raw_func distance_raw_func;
    double d;
    if (center->header.domain != domain) {
      GRN_OBJ_INIT(&center_, GRN_BULK, 0, domain);
      if (grn_obj_cast(ctx, center, &center_, GRN_FALSE)) { goto exit; }
      center = &center_;
    }

    distance_raw_func = grn_geo_resolve_distance_raw_func(ctx,
                                                          approximate_type,
                                                          domain);
    if (!distance_raw_func) {
      ERR(GRN_INVALID_ARGUMENT,
          "unknown approximate type: <%d>", approximate_type);
      goto exit;
    }
    d = distance_raw_func(ctx,
                          GRN_GEO_POINT_VALUE_RAW(point),
                          GRN_GEO_POINT_VALUE_RAW(center));
    switch (radius_or_point->header.domain) {
    case GRN_DB_INT32 :
      r = d <= GRN_INT32_VALUE(radius_or_point);
      break;
    case GRN_DB_UINT32 :
      r = d <= GRN_UINT32_VALUE(radius_or_point);
      break;
    case GRN_DB_INT64 :
      r = d <= GRN_INT64_VALUE(radius_or_point);
      break;
    case GRN_DB_UINT64 :
      r = d <= GRN_UINT64_VALUE(radius_or_point);
      break;
    case GRN_DB_FLOAT :
      r = d <= GRN_FLOAT_VALUE(radius_or_point);
      break;
    case GRN_DB_SHORT_TEXT :
    case GRN_DB_TEXT :
    case GRN_DB_LONG_TEXT :
      GRN_OBJ_INIT(&radius_or_point_, GRN_BULK, 0, domain);
      if (grn_obj_cast(ctx, radius_or_point, &radius_or_point_, GRN_FALSE)) { goto exit; }
      radius_or_point = &radius_or_point_;
      /* fallthru */
    case GRN_DB_TOKYO_GEO_POINT :
    case GRN_DB_WGS84_GEO_POINT :
      if (domain != radius_or_point->header.domain) { /* todo */ goto exit; }
      r = d <= distance_raw_func(ctx,
                                 GRN_GEO_POINT_VALUE_RAW(radius_or_point),
                                 GRN_GEO_POINT_VALUE_RAW(center));
      break;
    default :
      goto exit;
    }
  } else {
    /* todo */
  }
exit :
  return r;
}

grn_bool
grn_geo_in_rectangle_raw(grn_ctx *ctx, grn_geo_point *point,
                         grn_geo_point *top_left, grn_geo_point *bottom_right)
{
  if (point->latitude > top_left->latitude) {
    return GRN_FALSE;
  }
  if (point->latitude < bottom_right->latitude) {
    return GRN_FALSE;
  }

  if (GRN_GEO_LONGITUDE_IS_WRAPPED(top_left, bottom_right)) {
    if (point->longitude >= top_left->longitude) {
      return GRN_TRUE;
    }
    if (point->longitude <= bottom_right->longitude) {
      return GRN_TRUE;
    }
    return GRN_FALSE;
  } else {
    if (point->longitude < top_left->longitude) {
      return GRN_FALSE;
    }
    if (point->longitude > bottom_right->longitude) {
      return GRN_FALSE;
    }
    return GRN_TRUE;
  }
}

grn_bool
grn_geo_in_rectangle(grn_ctx *ctx, grn_obj *point,
                     grn_obj *top_left, grn_obj *bottom_right)
{
  grn_bool r = GRN_FALSE;
  grn_obj top_left_, bottom_right_;
  grn_id domain = point->header.domain;
  if (domain == GRN_DB_TOKYO_GEO_POINT || domain == GRN_DB_WGS84_GEO_POINT) {
    if (top_left->header.domain != domain) {
      GRN_OBJ_INIT(&top_left_, GRN_BULK, 0, domain);
      if (grn_obj_cast(ctx, top_left, &top_left_, GRN_FALSE)) { goto exit; }
      top_left = &top_left_;
    }
    if (bottom_right->header.domain != domain) {
      GRN_OBJ_INIT(&bottom_right_, GRN_BULK, 0, domain);
      if (grn_obj_cast(ctx, bottom_right, &bottom_right_, GRN_FALSE)) { goto exit; }
      bottom_right = &bottom_right_;
    }
    r = grn_geo_in_rectangle_raw(ctx,
                                 GRN_GEO_POINT_VALUE_RAW(point),
                                 GRN_GEO_POINT_VALUE_RAW(top_left),
                                 GRN_GEO_POINT_VALUE_RAW(bottom_right));
  } else {
    /* todo */
  }
exit :
  return r;
}

typedef enum {
  LONGITUDE_SHORT,
  LONGITUDE_LONG,
} distance_type;

typedef enum {
  QUADRANT_1ST,
  QUADRANT_2ND,
  QUADRANT_3RD,
  QUADRANT_4TH,
  QUADRANT_1ST_TO_2ND,
  QUADRANT_1ST_TO_3RD,
  QUADRANT_1ST_TO_4TH,
  QUADRANT_2ND_TO_1ST,
  QUADRANT_2ND_TO_3RD,
  QUADRANT_2ND_TO_4TH,
  QUADRANT_3RD_TO_1ST,
  QUADRANT_3RD_TO_2ND,
  QUADRANT_3RD_TO_4TH,
  QUADRANT_4TH_TO_1ST,
  QUADRANT_4TH_TO_2ND,
  QUADRANT_4TH_TO_3RD,
} quadrant_type;

static distance_type
geo_longitude_distance_type(int start_longitude, int end_longitude)
{
  int diff_longitude;
  int east_to_west;
  int west_to_east;
  if (start_longitude >= 0) {
    diff_longitude = abs(start_longitude - end_longitude);
  } else {
    diff_longitude = abs(end_longitude - start_longitude);
  }
  east_to_west = start_longitude > 0 && end_longitude < 0;
  west_to_east = start_longitude < 0 && end_longitude > 0;
  if (start_longitude != end_longitude &&
      (east_to_west || west_to_east) &&
      diff_longitude > 180 * GRN_GEO_RESOLUTION) {
    return LONGITUDE_LONG;
  } else {
    return LONGITUDE_SHORT;
  }
}

static inline quadrant_type
geo_quadrant_type(grn_geo_point *point1, grn_geo_point *point2)
{
#define QUADRANT_1ST_WITH_AXIS(point) \
  (point->longitude >= 0) && (point->latitude >= 0)
#define QUADRANT_2ND_WITH_AXIS(point) \
  (point->longitude <= 0) && (point->latitude >= 0)
#define QUADRANT_3RD_WITH_AXIS(point) \
  (point->longitude <= 0) && (point->latitude <= 0)
#define QUADRANT_4TH_WITH_AXIS(point) \
  (point->longitude >= 0) && (point->latitude <= 0)

  if (QUADRANT_1ST_WITH_AXIS(point1) && QUADRANT_1ST_WITH_AXIS(point2)) {
    return QUADRANT_1ST;
  } else if (QUADRANT_2ND_WITH_AXIS(point1) && QUADRANT_2ND_WITH_AXIS(point2)) {
    return QUADRANT_2ND;
  } else if (QUADRANT_3RD_WITH_AXIS(point1) && QUADRANT_3RD_WITH_AXIS(point2)) {
    return QUADRANT_3RD;
  } else if (QUADRANT_4TH_WITH_AXIS(point1) && QUADRANT_4TH_WITH_AXIS(point2)) {
    return QUADRANT_4TH;
  } else {
    if (point1->longitude > 0 && point2->longitude < 0 &&
        point1->latitude >= 0 && point2->latitude >= 0) {
      return QUADRANT_1ST_TO_2ND;
    } else if (point1->longitude < 0 && point2->longitude > 0 &&
               point1->latitude >= 0 && point2->latitude >= 0) {
      return QUADRANT_2ND_TO_1ST;
    } else if (point1->longitude < 0 && point2->longitude > 0 &&
               point1->latitude <= 0 && point2->latitude <= 0) {
      return QUADRANT_3RD_TO_4TH;
    } else if (point1->longitude > 0 && point2->longitude < 0 &&
               point1->latitude <= 0 && point2->latitude <= 0) {
      return QUADRANT_4TH_TO_3RD;
    } else if (point1->longitude >= 0 && point2->longitude >= 0 &&
               point1->latitude > 0 && point2->latitude < 0) {
      return QUADRANT_1ST_TO_4TH;
    } else if (point1->longitude >= 0 && point2->longitude >= 0 &&
               point1->latitude < 0 && point2->latitude > 0) {
      return QUADRANT_4TH_TO_1ST;
    } else if (point1->longitude <= 0 && point2->longitude <= 0 &&
               point1->latitude > 0 && point2->latitude < 0) {
      return QUADRANT_2ND_TO_3RD;
    } else if (point1->longitude <= 0 && point2->longitude <= 0 &&
               point1->latitude < 0 && point2->latitude > 0) {
      return QUADRANT_3RD_TO_2ND;
    } else if (point1->longitude >= 0 && point2->longitude <= 0 &&
               point1->latitude > 0 && point2->latitude < 0) {
      return QUADRANT_1ST_TO_3RD;
    } else if (point1->longitude <= 0 && point2->longitude >= 0 &&
               point1->latitude < 0 && point2->latitude > 0) {
      return QUADRANT_3RD_TO_1ST;
    } else if (point1->longitude <= 0 && point2->longitude >= 0 &&
               point1->latitude > 0 && point2->latitude < 0) {
      return QUADRANT_2ND_TO_4TH;
    } else if (point1->longitude >= 0 && point2->longitude <= 0 &&
               point1->latitude < 0 && point2->latitude > 0) {
      return QUADRANT_4TH_TO_2ND;
    } else {
      /* FIXME */
      return QUADRANT_1ST;
    }
  }
#undef QUADRANT_1ST_WITH_AXIS
#undef QUADRANT_2ND_WITH_AXIS
#undef QUADRANT_3RD_WITH_AXIS
#undef QUADRANT_4TH_WITH_AXIS
}

static inline double
geo_distance_rectangle_square_root(double start_longitude, double start_latitude,
                                   double end_longitude, double end_latitude)
{
  double diff_longitude;
  double x, y;

  diff_longitude = end_longitude - start_longitude;
  x = diff_longitude * cos((start_latitude + end_latitude) * 0.5);
  y = end_latitude - start_latitude;
  return sqrt((x * x) + (y * y));
}

static inline double
geo_distance_rectangle_short_dist_type(quadrant_type quad_type,
                                       double lng1, double lat1,
                                       double lng2, double lat2)
{
  double distance;
  double longitude_delta, latitude_delta;

  if (quad_type == QUADRANT_1ST_TO_4TH ||
      quad_type == QUADRANT_4TH_TO_1ST ||
      quad_type == QUADRANT_2ND_TO_3RD ||
      quad_type == QUADRANT_3RD_TO_2ND) {
    longitude_delta = lng2 - lng1;
    if (longitude_delta > 0 || longitude_delta < 0) {
      if (lat2 > lat1) {
        distance = geo_distance_rectangle_square_root(lng1,
                                                      lat1,
                                                      lng2,
                                                      lat2) * GRN_GEO_RADIUS;
      } else {
        distance = geo_distance_rectangle_square_root(lng2,
                                                      lat2,
                                                      lng1,
                                                      lat1) * GRN_GEO_RADIUS;
      }
    } else {
      latitude_delta = fabs(lat1) + fabs(lat2);
      distance = sqrt(latitude_delta * latitude_delta) * GRN_GEO_RADIUS;
    }
  } else if (quad_type == QUADRANT_1ST_TO_3RD ||
             quad_type == QUADRANT_2ND_TO_4TH) {
    distance = geo_distance_rectangle_square_root(lng1,
                                                  lat1,
                                                  lng2,
                                                  lat2) * GRN_GEO_RADIUS;
  } else if (quad_type == QUADRANT_3RD_TO_1ST ||
             quad_type == QUADRANT_4TH_TO_2ND) {
    distance = geo_distance_rectangle_square_root(lng2,
                                                  lat2,
                                                  lng1,
                                                  lat1) * GRN_GEO_RADIUS;
  } else if (quad_type == QUADRANT_1ST_TO_2ND ||
             quad_type == QUADRANT_2ND_TO_1ST ||
             quad_type == QUADRANT_3RD_TO_4TH ||
             quad_type == QUADRANT_4TH_TO_3RD) {
    if (lat2 > lat1) {
      distance = geo_distance_rectangle_square_root(lng1,
                                                    lat1,
                                                    lng2,
                                                    lat2) * GRN_GEO_RADIUS;
    } else if (lat2 < lat1) {
      distance = geo_distance_rectangle_square_root(lng2,
                                                    lat2,
                                                    lng1,
                                                    lat1) * GRN_GEO_RADIUS;
    } else {
      longitude_delta = lng2 - lng1;
      distance = longitude_delta * cos(lat1);
      distance = sqrt(distance * distance) * GRN_GEO_RADIUS;
    }
  } else {
    distance = geo_distance_rectangle_square_root(lng1,
                                                  lat1,
                                                  lng2,
                                                  lat2) * GRN_GEO_RADIUS;
  }
  return distance;
}

static inline double
geo_distance_rectangle_long_dist_type(quadrant_type quad_type,
                                      double lng1, double lat1,
                                      double lng2, double lat2)
{
#define M_2PI 6.28318530717958647692

  double distance;

  if (quad_type == QUADRANT_1ST_TO_2ND ||
      quad_type == QUADRANT_4TH_TO_3RD) {
    if (lat1 > lat2) {
      distance = geo_distance_rectangle_square_root(lng2 + M_2PI,
                                                    lat2,
                                                    lng1,
                                                    lat1) * GRN_GEO_RADIUS;
    } else {
      distance = geo_distance_rectangle_square_root(lng1,
                                                    lat1,
                                                    lng2 + M_2PI,
                                                    lat2) * GRN_GEO_RADIUS;
    }
  } else if (quad_type == QUADRANT_2ND_TO_1ST ||
             quad_type == QUADRANT_3RD_TO_4TH) {
    if (lat1 > lat2) {
      distance = geo_distance_rectangle_square_root(lng2,
                                                    lat2,
                                                    lng1 + M_2PI,
                                                    lat1) * GRN_GEO_RADIUS;
    } else {
      distance = geo_distance_rectangle_square_root(lng1 + M_2PI,
                                                    lat1,
                                                    lng2,
                                                    lat2) * GRN_GEO_RADIUS;
    }
  } else if (quad_type == QUADRANT_1ST_TO_3RD) {
    distance = geo_distance_rectangle_square_root(lng2 + M_2PI,
                                                  lat2,
                                                  lng1,
                                                  lat1) * GRN_GEO_RADIUS;
  } else if (quad_type == QUADRANT_3RD_TO_1ST) {
    distance = geo_distance_rectangle_square_root(lng1 + M_2PI,
                                                  lat1,
                                                  lng2,
                                                  lat2) * GRN_GEO_RADIUS;
  } else if (quad_type == QUADRANT_2ND_TO_4TH) {
    distance = geo_distance_rectangle_square_root(lng2,
                                                  lat2,
                                                  lng1 + M_2PI,
                                                  lat1) * GRN_GEO_RADIUS;
  } else if (quad_type == QUADRANT_4TH_TO_2ND) {
    distance = geo_distance_rectangle_square_root(lng1,
                                                  lat1,
                                                  lng2 + M_2PI,
                                                  lat2) * GRN_GEO_RADIUS;
  } else {
    if (lng1 > lng2) {
      distance = geo_distance_rectangle_square_root(lng1,
                                                    lat1,
                                                    lng2 + M_2PI,
                                                    lat2) * GRN_GEO_RADIUS;
    } else {
      distance = geo_distance_rectangle_square_root(lng2,
                                                    lat2,
                                                    lng1 + M_2PI,
                                                    lat1) * GRN_GEO_RADIUS;
    }
  }
  return distance;
#undef M_2PI
}

double
grn_geo_distance_rectangle_raw(grn_ctx *ctx,
                               grn_geo_point *point1, grn_geo_point *point2)
{

  double lng1, lat1, lng2, lat2, distance;
  distance_type dist_type;
  quadrant_type quad_type;

  lat1 = GRN_GEO_INT2RAD(point1->latitude);
  lng1 = GRN_GEO_INT2RAD(point1->longitude);
  lat2 = GRN_GEO_INT2RAD(point2->latitude);
  lng2 = GRN_GEO_INT2RAD(point2->longitude);
  quad_type = geo_quadrant_type(point1, point2);
  if (quad_type <= QUADRANT_4TH) {
    distance = geo_distance_rectangle_square_root(lng1,
                                                  lat1,
                                                  lng2,
                                                  lat2) * GRN_GEO_RADIUS;
  } else {
    dist_type = geo_longitude_distance_type(point1->longitude,
                                            point2->longitude);
    if (dist_type == LONGITUDE_SHORT) {
      distance = geo_distance_rectangle_short_dist_type(quad_type,
                                                        lng1, lat1,
                                                        lng2, lat2);
    } else {
      distance = geo_distance_rectangle_long_dist_type(quad_type,
                                                       lng1, lat1,
                                                       lng2, lat2);
    }
  }
  return distance;
}

double
grn_geo_distance_sphere_raw(grn_ctx *ctx,
                            grn_geo_point *point1, grn_geo_point *point2)
{
  double lng1, lat1, lng2, lat2, x, y;

  lat1 = GRN_GEO_INT2RAD(point1->latitude);
  lng1 = GRN_GEO_INT2RAD(point1->longitude);
  lat2 = GRN_GEO_INT2RAD(point2->latitude);
  lng2 = GRN_GEO_INT2RAD(point2->longitude);
  x = sin(fabs(lng2 - lng1) * 0.5);
  y = sin(fabs(lat2 - lat1) * 0.5);
  return asin(sqrt((y * y) + cos(lat1) * cos(lat2) * x * x)) * 2 * GRN_GEO_RADIUS;
}

double
grn_geo_distance_ellipsoid_raw(grn_ctx *ctx,
                               grn_geo_point *point1, grn_geo_point *point2,
                               int c1, int c2, double c3)
{
  double lng1, lat1, lng2, lat2, p, q, r, m, n, x, y;

  lat1 = GRN_GEO_INT2RAD(point1->latitude);
  lng1 = GRN_GEO_INT2RAD(point1->longitude);
  lat2 = GRN_GEO_INT2RAD(point2->latitude);
  lng2 = GRN_GEO_INT2RAD(point2->longitude);
  p = (lat1 + lat2) * 0.5;
  q = (1 - c3 * sin(p) * sin(p));
  r = sqrt(q);
  m = c1 / (q * r);
  n = c2 / r;
  x = n * cos(p) * fabs(lng1 - lng2);
  y = m * fabs(lat1 - lat2);
  return sqrt((x * x) + (y * y));
}

double
grn_geo_distance_ellipsoid_raw_tokyo(grn_ctx *ctx,
                                     grn_geo_point *point1,
                                     grn_geo_point *point2)
{
  return grn_geo_distance_ellipsoid_raw(ctx, point1, point2,
                                        GRN_GEO_BES_C1,
                                        GRN_GEO_BES_C2,
                                        GRN_GEO_BES_C3);
}

double
grn_geo_distance_ellipsoid_raw_wgs84(grn_ctx *ctx,
                                     grn_geo_point *point1,
                                     grn_geo_point *point2)
{
  return grn_geo_distance_ellipsoid_raw(ctx, point1, point2,
                                        GRN_GEO_GRS_C1,
                                        GRN_GEO_GRS_C2,
                                        GRN_GEO_GRS_C3);
}

double
grn_geo_distance(grn_ctx *ctx, grn_obj *point1, grn_obj *point2,
                 grn_geo_approximate_type type)
{
  double d = 0.0;

  switch (type) {
  case GRN_GEO_APPROXIMATE_RECTANGLE :
    d = grn_geo_distance_rectangle(ctx, point1, point2);
    break;
  case GRN_GEO_APPROXIMATE_SPHERE :
    d = grn_geo_distance_sphere(ctx, point1, point2);
    break;
  case GRN_GEO_APPROXIMATE_ELLIPSOID :
    d = grn_geo_distance_ellipsoid(ctx, point1, point2);
    break;
  default :
    ERR(GRN_INVALID_ARGUMENT, "unknown approximate type: <%d>", type);
    break;
  }
  return d;
}

double
grn_geo_distance_rectangle(grn_ctx *ctx, grn_obj *point1, grn_obj *point2)
{
  double d = 0;
  grn_bool point1_initialized = GRN_FALSE;
  grn_bool point2_initialized = GRN_FALSE;
  grn_obj point1_, point2_;
  grn_id domain1 = point1->header.domain;
  grn_id domain2 = point2->header.domain;
  if (domain1 == GRN_DB_TOKYO_GEO_POINT || domain1 == GRN_DB_WGS84_GEO_POINT) {
    if (domain1 != domain2) {
      GRN_OBJ_INIT(&point2_, GRN_BULK, 0, domain1);
      point2_initialized = GRN_TRUE;
      if (grn_obj_cast(ctx, point2, &point2_, GRN_FALSE)) { goto exit; }
      point2 = &point2_;
    }
  } else if (domain2 == GRN_DB_TOKYO_GEO_POINT ||
             domain2 == GRN_DB_WGS84_GEO_POINT) {
    GRN_OBJ_INIT(&point1_, GRN_BULK, 0, domain2);
    point1_initialized = GRN_TRUE;
    if (grn_obj_cast(ctx, point1, &point1_, GRN_FALSE)) { goto exit; }
    point1 = &point1_;
  } else if ((GRN_DB_SHORT_TEXT <= domain1 && domain1 <= GRN_DB_LONG_TEXT) &&
             (GRN_DB_SHORT_TEXT <= domain2 && domain2 <= GRN_DB_LONG_TEXT)) {
    GRN_OBJ_INIT(&point1_, GRN_BULK, 0, GRN_DB_WGS84_GEO_POINT);
    point1_initialized = GRN_TRUE;
    if (grn_obj_cast(ctx, point1, &point1_, GRN_FALSE)) { goto exit; }
    point1 = &point1_;

    GRN_OBJ_INIT(&point2_, GRN_BULK, 0, GRN_DB_WGS84_GEO_POINT);
    point2_initialized = GRN_TRUE;
    if (grn_obj_cast(ctx, point2, &point2_, GRN_FALSE)) { goto exit; }
    point2 = &point2_;
  } else {
    goto exit;
  }
  d = grn_geo_distance_rectangle_raw(ctx,
                                     GRN_GEO_POINT_VALUE_RAW(point1),
                                     GRN_GEO_POINT_VALUE_RAW(point2));
exit :
  if (point1_initialized) {
    GRN_OBJ_FIN(ctx, &point1_);
  }
  if (point2_initialized) {
    GRN_OBJ_FIN(ctx, &point2_);
  }
  return d;
}

double
grn_geo_distance_sphere(grn_ctx *ctx, grn_obj *point1, grn_obj *point2)
{
  double d = 0;
  grn_bool point2_initialized = GRN_FALSE;
  grn_obj point2_;
  grn_id domain = point1->header.domain;
  if (domain == GRN_DB_TOKYO_GEO_POINT || domain == GRN_DB_WGS84_GEO_POINT) {
    if (point2->header.domain != domain) {
      GRN_OBJ_INIT(&point2_, GRN_BULK, 0, domain);
      point2_initialized = GRN_TRUE;
      if (grn_obj_cast(ctx, point2, &point2_, GRN_FALSE)) { goto exit; }
      point2 = &point2_;
    }
    d = grn_geo_distance_sphere_raw(ctx,
                                    GRN_GEO_POINT_VALUE_RAW(point1),
                                    GRN_GEO_POINT_VALUE_RAW(point2));
  } else {
    /* todo */
  }
exit :
  if (point2_initialized) {
    GRN_OBJ_FIN(ctx, &point2_);
  }
  return d;
}

double
grn_geo_distance_ellipsoid(grn_ctx *ctx, grn_obj *point1, grn_obj *point2)
{
  double d = 0;
  grn_bool point2_initialized = GRN_FALSE;
  grn_obj point2_;
  grn_id domain = point1->header.domain;
  if (domain == GRN_DB_TOKYO_GEO_POINT || domain == GRN_DB_WGS84_GEO_POINT) {
    if (point2->header.domain != domain) {
      GRN_OBJ_INIT(&point2_, GRN_BULK, 0, domain);
      point2_initialized = GRN_TRUE;
      if (grn_obj_cast(ctx, point2, &point2_, GRN_FALSE)) { goto exit; }
      point2 = &point2_;
    }
    if (domain == GRN_DB_TOKYO_GEO_POINT) {
      d = grn_geo_distance_ellipsoid_raw_tokyo(ctx,
                                               GRN_GEO_POINT_VALUE_RAW(point1),
                                               GRN_GEO_POINT_VALUE_RAW(point2));
    } else {
      d = grn_geo_distance_ellipsoid_raw_wgs84(ctx,
                                               GRN_GEO_POINT_VALUE_RAW(point1),
                                               GRN_GEO_POINT_VALUE_RAW(point2));
    }
  } else {
    /* todo */
  }
exit :
  if (point2_initialized) {
    GRN_OBJ_FIN(ctx, &point2_);
  }
  return d;
}
