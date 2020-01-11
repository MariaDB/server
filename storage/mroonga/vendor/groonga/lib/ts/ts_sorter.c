/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015 Brazil

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

#include "ts_sorter.h"

#include <string.h>

#include "ts_expr_parser.h"
#include "ts_log.h"
#include "ts_util.h"

/*-------------------------------------------------------------
 * grn_ts_sorter_node.
 */

/* grn_ts_sorter_node_init() initializes a sorter node. */
static void
grn_ts_sorter_node_init(grn_ctx *ctx, grn_ts_sorter_node *node)
{
  memset(node, 0, sizeof(*node));
  node->expr = NULL;
  grn_ts_buf_init(ctx, &node->buf);
  node->next = NULL;
}

/* grn_ts_sorter_node_fin() finalizes a sorter node. */
static void
grn_ts_sorter_node_fin(grn_ctx *ctx, grn_ts_sorter_node *node)
{
  grn_ts_buf_fin(ctx, &node->buf);
  if (node->expr) {
    grn_ts_expr_close(ctx, node->expr);
  }
}

/* grn_ts_sorter_node_open() creates a sorter nodes. */
static grn_rc
grn_ts_sorter_node_open(grn_ctx *ctx, grn_ts_expr *expr, grn_ts_bool reverse,
                        grn_ts_sorter_node **node)
{
  grn_ts_sorter_node *new_node;
  new_node = GRN_MALLOCN(grn_ts_sorter_node, 1);
  if (!new_node) {
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                      "GRN_MALLOCN failed: %" GRN_FMT_SIZE " x 1",
                      sizeof(grn_ts_sorter_node));
  }
  grn_ts_sorter_node_init(ctx, new_node);
  new_node->expr = expr;
  new_node->reverse = reverse;
  *node = new_node;
  return GRN_SUCCESS;
}

/* grn_ts_sorter_node_close() destroys a sorter node. */
static void
grn_ts_sorter_node_close(grn_ctx *ctx, grn_ts_sorter_node *node)
{
  grn_ts_sorter_node_fin(ctx, node);
  GRN_FREE(node);
}

/* grn_ts_sorter_node_list_close() destroys a linked list of sorter nodes. */
static void
grn_ts_sorter_node_list_close(grn_ctx *ctx, grn_ts_sorter_node *head)
{
  grn_ts_sorter_node *node = head;
  while (node) {
    grn_ts_sorter_node *next = node->next;
    grn_ts_sorter_node_close(ctx, node);
    node = next;
  }
}

/* grn_ts_sorter_node_progress() progresses sorting. */
static grn_rc
grn_ts_sorter_node_progress(grn_ctx *ctx, grn_ts_sorter_node *node,
                            size_t offset, size_t limit,
                            grn_ts_record *recs, size_t n_recs, size_t *n_rest)
{
  // TODO
  return GRN_FUNCTION_NOT_IMPLEMENTED;
}

/* grn_ts_sorter_node_complete() completes sorting. */
static grn_rc
grn_ts_sorter_node_complete(grn_ctx *ctx, grn_ts_sorter_node *node,
                            size_t offset, size_t limit,
                            grn_ts_record *recs, size_t n_recs, size_t *n_rest)
{
  // TODO
  return GRN_FUNCTION_NOT_IMPLEMENTED;
}

/* Forward declarations. */
static grn_rc
grn_ts_sorter_node_sort(grn_ctx *ctx, grn_ts_sorter_node *node,
                        size_t offset, size_t limit,
                        grn_ts_record *recs, size_t n_recs);

/* grn_ts_rec_swap() swaps records. */
inline static void
grn_ts_rec_swap(grn_ts_record *lhs, grn_ts_record *rhs)
{
  grn_ts_record tmp = *lhs;
  *lhs = *rhs;
  *rhs = tmp;
}

/* grn_ts_int_swap() swaps Int values. */
inline static void
grn_ts_int_swap(grn_ts_int *lhs, grn_ts_int *rhs)
{
  grn_ts_int tmp = *lhs;
  *lhs = *rhs;
  *rhs = tmp;
}

/* FIXME: Sorting by _id does not assume ID duplicates. */

/* grn_ts_move_pivot_by_id_asc() moves the pivot to the front. */
static void
grn_ts_move_pivot_by_id_asc(grn_ts_record *recs, size_t n_recs)
{
  /* Choose the median from recs[1], recs[n_recs / 2], and recs[n_recs - 2]. */
  size_t first = 1;
  size_t middle = n_recs / 2;
  size_t last = n_recs - 2;
  if (recs[first].id < recs[middle].id) {
    /* first < middle. */
    if (recs[middle].id < recs[last].id) {
      /* first < middle < last */
      grn_ts_rec_swap(&recs[0], &recs[middle]);
    } else if (recs[first].id < recs[last].id) {
      /* first < last < middle. */
      grn_ts_rec_swap(&recs[0], &recs[last]);
    } else {
      /* last < first < middle. */
      grn_ts_rec_swap(&recs[0], &recs[first]);
    }
  } else if (recs[last].id < recs[middle].id) {
    /* last < middle < first. */
    grn_ts_rec_swap(&recs[0], &recs[middle]);
  } else if (recs[last].id < recs[first].id) {
    /* middle < last < first. */
    grn_ts_rec_swap(&recs[0], &recs[last]);
  } else {
    /* middle < first < last. */
    grn_ts_rec_swap(&recs[0], &recs[first]);
  }
}

/* grn_ts_isort_by_id_asc() sorts records. */
static grn_rc
grn_ts_isort_by_id_asc(grn_ctx *ctx, grn_ts_sorter_node *node,
                       size_t offset, size_t limit,
                       grn_ts_record *recs, size_t n_recs)
{
  for (size_t i = 1; i < n_recs; ++i) {
    for (size_t j = i; j > 0; --j) {
      if (recs[j].id < recs[j - 1].id) {
        grn_ts_rec_swap(&recs[j], &recs[j - 1]);
      } else {
        break;
      }
    }
  }
  return GRN_SUCCESS;
}

/* grn_ts_qsort_by_id_asc() sorts records. */
static grn_rc
grn_ts_qsort_by_id_asc(grn_ctx *ctx, grn_ts_sorter_node *node,
                       size_t offset, size_t limit,
                       grn_ts_record *recs, size_t n_recs)
{
  grn_rc rc;
  /*
   * FIXME: Currently, the threshold is 16.
   *        This value should be optimized and replaced with a named constant.
   */
  while (n_recs >= 16) {
    grn_ts_record pivot;
    size_t left, right;
    grn_ts_move_pivot_by_id_asc(recs, n_recs);
    pivot = recs[0];
    left = 1;
    right = n_recs;
    for ( ; ; ) {
      /* Move prior records to left. */
      while (left < right) {
        if (pivot.id < recs[left].id) {
          break;
        }
        ++left;
      }
      while (left < right) {
        --right;
        if (recs[right].id < pivot.id) {
          break;
        }
      }
      if (left >= right) {
        break;
      }
      grn_ts_rec_swap(&recs[left], &recs[right]);
      ++left;
    }
    /* Move the pivot to the boundary. */
    --left;
    grn_ts_rec_swap(&recs[0], &recs[left]);
    /*
     * Use a recursive call to sort the smaller group so that the recursion
     * depth is less than log_2(n_recs).
     */
    if (left < (n_recs - right)) {
      if ((offset < left) && (left >= 2)) {
        size_t next_limit = (limit < left) ? limit : left;
        rc = grn_ts_qsort_by_id_asc(ctx, node, offset, next_limit, recs, left);
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
      if (limit <= right) {
        return GRN_SUCCESS;
      }
      recs += right;
      n_recs -= right;
      offset = (offset < right) ? 0 : (offset - right);
      limit -= right;
    } else {
      if ((limit > right) && ((n_recs - right) >= 2)) {
        size_t next_offset = (offset < right) ? 0 : (offset - right);
        size_t next_limit = limit - right;
        rc = grn_ts_qsort_by_id_asc(ctx, node, next_offset, next_limit,
                                    recs + right, n_recs - right);
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
      if (offset >= left) {
        return GRN_SUCCESS;
      }
      n_recs = left;
      if (limit > left) {
        limit = left;
      }
    }
  }
  if (n_recs >= 2) {
    return grn_ts_isort_by_id_asc(ctx, node, offset, limit, recs, n_recs);
  }
  return GRN_SUCCESS;
}

/* grn_ts_move_pivot_by_id_desc() moves the pivot to the front. */
static void
grn_ts_move_pivot_by_id_desc(grn_ts_record *recs, size_t n_recs)
{
  /* Choose the median from recs[1], recs[n_recs / 2], and recs[n_recs - 2]. */
  size_t first = 1;
  size_t middle = n_recs / 2;
  size_t last = n_recs - 2;
  if (recs[first].id > recs[middle].id) {
    /* first > middle. */
    if (recs[middle].id > recs[last].id) {
      /* first > middle > last */
      grn_ts_rec_swap(&recs[0], &recs[middle]);
    } else if (recs[first].id > recs[last].id) {
      /* first > last > middle. */
      grn_ts_rec_swap(&recs[0], &recs[last]);
    } else {
      /* last > first > middle. */
      grn_ts_rec_swap(&recs[0], &recs[first]);
    }
  } else if (recs[last].id > recs[middle].id) {
    /* last > middle > first. */
    grn_ts_rec_swap(&recs[0], &recs[middle]);
  } else if (recs[last].id > recs[first].id) {
    /* middle > last > first. */
    grn_ts_rec_swap(&recs[0], &recs[last]);
  } else {
    /* middle > first > last. */
    grn_ts_rec_swap(&recs[0], &recs[first]);
  }
}

/* grn_ts_isort_by_id_desc() sorts records. */
static grn_rc
grn_ts_isort_by_id_desc(grn_ctx *ctx, grn_ts_sorter_node *node,
                        size_t offset, size_t limit,
                        grn_ts_record *recs, size_t n_recs)
{
  for (size_t i = 1; i < n_recs; ++i) {
    for (size_t j = i; j > 0; --j) {
      if (recs[j].id > recs[j - 1].id) {
        grn_ts_rec_swap(&recs[j], &recs[j - 1]);
      } else {
        break;
      }
    }
  }
  return GRN_SUCCESS;
}

/* grn_ts_qsort_by_id_desc() sorts records. */
static grn_rc
grn_ts_qsort_by_id_desc(grn_ctx *ctx, grn_ts_sorter_node *node,
                        size_t offset, size_t limit,
                        grn_ts_record *recs, size_t n_recs)
{
  grn_rc rc;
  /*
   * FIXME: Currently, the threshold is 16.
   *        This value should be optimized and replaced with a named constant.
   */
  while (n_recs >= 16) {
    grn_ts_record pivot;
    size_t left, right;
    grn_ts_move_pivot_by_id_desc(recs, n_recs);
    pivot = recs[0];
    left = 1;
    right = n_recs;
    for ( ; ; ) {
      /* Move prior records to left. */
      while (left < right) {
        if (pivot.id > recs[left].id) {
          break;
        }
        ++left;
      }
      while (left < right) {
        --right;
        if (recs[right].id > pivot.id) {
          break;
        }
      }
      if (left >= right) {
        break;
      }
      grn_ts_rec_swap(&recs[left], &recs[right]);
      ++left;
    }
    /* Move the pivot to the boundary. */
    --left;
    grn_ts_rec_swap(&recs[0], &recs[left]);
    /*
     * Use a recursive call to sort the smaller group so that the recursion
     * depth is less than log_2(n_recs).
     */
    if (left < (n_recs - right)) {
      if ((offset < left) && (left >= 2)) {
        size_t next_limit = (limit < left) ? limit : left;
        rc = grn_ts_qsort_by_id_desc(ctx, node, offset, next_limit,
                                     recs, left);
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
      if (limit <= right) {
        return GRN_SUCCESS;
      }
      recs += right;
      n_recs -= right;
      offset = (offset < right) ? 0 : (offset - right);
      limit -= right;
    } else {
      if ((limit > right) && ((n_recs - right) >= 2)) {
        size_t next_offset = (offset < right) ? 0 : (offset - right);
        size_t next_limit = limit - right;
        rc = grn_ts_qsort_by_id_desc(ctx, node, next_offset, next_limit,
                                     recs + right, n_recs - right);
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
      if (offset >= left) {
        return GRN_SUCCESS;
      }
      n_recs = left;
      if (limit > left) {
        limit = left;
      }
    }
  }
  if (n_recs >= 2) {
    return grn_ts_isort_by_id_desc(ctx, node, offset, limit, recs, n_recs);
  }
  return GRN_SUCCESS;
}

/* grn_ts_sorter_node_sort_by_id() sorts records by _id. */
static grn_rc
grn_ts_sorter_node_sort_by_id(grn_ctx *ctx, grn_ts_sorter_node *node,
                              size_t offset, size_t limit,
                              grn_ts_record *recs, size_t n_recs)
{
  if (node->reverse) {
    return grn_ts_qsort_by_id_desc(ctx, node, offset, limit, recs, n_recs);
  } else {
    return grn_ts_qsort_by_id_asc(ctx, node, offset, limit, recs, n_recs);
  }
}

/* grn_ts_move_pivot_by_score_asc() moves the pivot to the front. */
static void
grn_ts_move_pivot_by_score_asc(grn_ts_record *recs, size_t n_recs)
{
  /* Choose the median from recs[1], recs[n_recs / 2], and recs[n_recs - 2]. */
  size_t first = 1;
  size_t middle = n_recs / 2;
  size_t last = n_recs - 2;
  if (recs[first].score < recs[middle].score) {
    /* first < middle. */
    if (recs[middle].score < recs[last].score) {
      /* first < middle < last */
      grn_ts_rec_swap(&recs[0], &recs[middle]);
    } else if (recs[first].score < recs[last].score) {
      /* first < last < middle. */
      grn_ts_rec_swap(&recs[0], &recs[last]);
    } else { /* last < first < middle. */
      grn_ts_rec_swap(&recs[0], &recs[first]);
    }
  } else if (recs[last].score < recs[middle].score) {
    /* last < middle < first. */
    grn_ts_rec_swap(&recs[0], &recs[middle]);
  } else if (recs[last].score < recs[first].score) {
    /* middle < last < first. */
    grn_ts_rec_swap(&recs[0], &recs[last]);
  } else { /* middle < first < last. */
    grn_ts_rec_swap(&recs[0], &recs[first]);
  }
}

/* grn_ts_isort_by_score_asc() sorts records. */
static grn_rc
grn_ts_isort_by_score_asc(grn_ctx *ctx, grn_ts_sorter_node *node,
                           size_t offset, size_t limit,
                           grn_ts_record *recs, size_t n_recs)
{
  for (size_t i = 1; i < n_recs; ++i) {
    for (size_t j = i; j > 0; --j) {
      if (recs[j].score < recs[j - 1].score) {
        grn_ts_rec_swap(&recs[j], &recs[j - 1]);
      } else {
        break;
      }
    }
  }
  /* Apply the next sorting if there are score duplicates. */
  if (node->next) {
    grn_rc rc;
    size_t begin = 0;
    for (size_t i = 1; i < n_recs; ++i) {
      if ((recs[i].score < recs[begin].score) ||
          (recs[i].score > recs[begin].score)) {
        if ((i - begin) >= 2) {
          rc = grn_ts_sorter_node_sort(ctx, node->next, 0, i - begin,
                                       recs + begin, i - begin);
          if (rc != GRN_SUCCESS) {
            return rc;
          }
        }
        begin = i;
      }
    }
    if ((n_recs - begin) >= 2) {
      rc = grn_ts_sorter_node_sort(ctx, node->next, 0, n_recs - begin,
                                   recs + begin, n_recs - begin);
      if (rc != GRN_SUCCESS) {
        return rc;
      }
    }
  }
  return GRN_SUCCESS;
}

/* grn_ts_qsort_by_score_asc() sorts records. */
static grn_rc
grn_ts_qsort_by_score_asc(grn_ctx *ctx, grn_ts_sorter_node *node,
                          size_t offset, size_t limit,
                          grn_ts_record *recs, size_t n_recs)
{
  grn_rc rc;
  /*
   * FIXME: Currently, the threshold is 16.
   *        This value should be optimized and replaced with a named constant.
   */
  while (n_recs >= 16) {
    grn_ts_record pivot;
    size_t left, right;
    size_t pivot_left, pivot_right;
    grn_ts_move_pivot_by_score_asc(recs, n_recs);
    pivot = recs[0];
    left = 1;
    right = n_recs;
    pivot_left = 1;
    pivot_right = n_recs;
    for ( ; ; ) {
      /*
       * Prior entries are moved to left. Less prior entries are moved to
       * right. Entries which equal to the pivot are moved to the edges.
       */
      while (left < right) {
        if (pivot.score < recs[left].score) {
          break;
        } else if ((pivot.score <= recs[left].score) &&
                   (pivot.score >= recs[left].score)) {
          grn_ts_rec_swap(&recs[left], &recs[pivot_left]);
          ++pivot_left;
        }
        ++left;
      }
      while (left < right) {
        --right;
        if (recs[right].score < pivot.score) {
          break;
        } else if ((recs[right].score <= pivot.score) &&
                   (recs[right].score >= pivot.score)) {
          --pivot_right;
          grn_ts_rec_swap(&recs[right], &recs[pivot_right]);
        }
      }
      if (left >= right) {
        break;
      }
      grn_ts_rec_swap(&recs[left], &recs[right]);
      ++left;
    }
    /* Move left pivot-equivalent entries to the left of the boundary. */
    while (pivot_left > 0) {
      --pivot_left;
      --left;
      grn_ts_rec_swap(&recs[pivot_left], &recs[left]);
    }
    /* Move right pivot-equivalent entries to the right of the boundary. */
    while (pivot_right < n_recs) {
      grn_ts_rec_swap(&recs[pivot_right], &recs[right]);
      ++pivot_right;
      ++right;
    }
    /* Apply the next sort condition to the pivot-equivalent recs. */
    if (node->next) {
      if (((right - left) >= 2) && (offset < right) && (limit > left)) {
        size_t next_offset = (offset < left) ? 0 : (offset - left);
        size_t next_limit = ((limit > right) ? right : limit) - left;
        rc = grn_ts_sorter_node_sort(ctx, node->next, next_offset, next_limit,
                                     recs + left, right - left);
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
    }
    /*
     * Use a recursive call to sort the smaller group so that the recursion
     * depth is less than log_2(n_recs).
     */
    if (left < (n_recs - right)) {
      if ((offset < left) && (left >= 2)) {
        size_t next_limit = (limit < left) ? limit : left;
        rc = grn_ts_qsort_by_score_asc(ctx, node, offset, next_limit,
                                     recs, left);
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
      if (limit <= right) {
        return GRN_SUCCESS;
      }
      recs += right;
      n_recs -= right;
      offset = (offset < right) ? 0 : (offset - right);
      limit -= right;
    } else {
      if ((limit > right) && ((n_recs - right) >= 2)) {
        size_t next_offset = (offset < right) ? 0 : (offset - right);
        size_t next_limit = limit - right;
        rc = grn_ts_qsort_by_score_asc(ctx, node, next_offset, next_limit,
                                       recs + right, n_recs - right);
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
      if (offset >= left) {
        return GRN_SUCCESS;
      }
      n_recs = left;
      if (limit > left) {
        limit = left;
      }
    }
  }
  if (n_recs >= 2) {
    rc = grn_ts_isort_by_score_asc(ctx, node, offset, limit, recs, n_recs);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
  }
  return GRN_SUCCESS;
}

/* grn_ts_move_pivot_by_score_desc() moves the pivot to the front. */
static void
grn_ts_move_pivot_by_score_desc(grn_ts_record *recs, size_t n_recs)
{
  /* Choose the median from recs[1], recs[n_recs / 2], and recs[n_recs - 2]. */
  size_t first = 1;
  size_t middle = n_recs / 2;
  size_t last = n_recs - 2;
  if (recs[first].score > recs[middle].score) {
    /* first > middle. */
    if (recs[middle].score > recs[last].score) {
      /* first > middle > last */
      grn_ts_rec_swap(&recs[0], &recs[middle]);
    } else if (recs[first].score > recs[last].score) {
      /* first > last > middle. */
      grn_ts_rec_swap(&recs[0], &recs[last]);
    } else { /* last > first > middle. */
      grn_ts_rec_swap(&recs[0], &recs[first]);
    }
  } else if (recs[last].score > recs[middle].score) {
    /* last > middle > first. */
    grn_ts_rec_swap(&recs[0], &recs[middle]);
  } else if (recs[last].score > recs[first].score) {
    /* middle > last > first. */
    grn_ts_rec_swap(&recs[0], &recs[last]);
  } else { /* middle > first > last. */
    grn_ts_rec_swap(&recs[0], &recs[first]);
  }
}

/* grn_ts_isort_by_score_desc() sorts records. */
static grn_rc
grn_ts_isort_by_score_desc(grn_ctx *ctx, grn_ts_sorter_node *node,
                           size_t offset, size_t limit,
                           grn_ts_record *recs, size_t n_recs)
{
  for (size_t i = 1; i < n_recs; ++i) {
    for (size_t j = i; j > 0; --j) {
      if (recs[j].score > recs[j - 1].score) {
        grn_ts_rec_swap(&recs[j], &recs[j - 1]);
      } else {
        break;
      }
    }
  }
  /* Apply the next sorting if there are score duplicates. */
  if (node->next) {
    grn_rc rc;
    size_t begin = 0;
    for (size_t i = 1; i < n_recs; ++i) {
      if ((recs[i].score < recs[begin].score) ||
          (recs[i].score > recs[begin].score)) {
        if ((i - begin) >= 2) {
          rc = grn_ts_sorter_node_sort(ctx, node->next, 0, i - begin,
                                       recs + begin, i - begin);
          if (rc != GRN_SUCCESS) {
            return rc;
          }
        }
        begin = i;
      }
    }
    if ((n_recs - begin) >= 2) {
      rc = grn_ts_sorter_node_sort(ctx, node->next, 0, n_recs - begin,
                                   recs + begin, n_recs - begin);
      if (rc != GRN_SUCCESS) {
        return rc;
      }
    }
  }
  return GRN_SUCCESS;
}

/* grn_ts_qsort_by_score_desc() sorts records. */
static grn_rc
grn_ts_qsort_by_score_desc(grn_ctx *ctx, grn_ts_sorter_node *node,
                           size_t offset, size_t limit,
                           grn_ts_record *recs, size_t n_recs)
{
  grn_rc rc;
  /*
   * FIXME: Currently, the threshold is 16.
   *        This value should be optimized and replaced with a named constant.
   */
  while (n_recs >= 16) {
    grn_ts_record pivot;
    size_t left = 1, right = n_recs;
    size_t pivot_left = 1, pivot_right = n_recs;
    grn_ts_move_pivot_by_score_desc(recs, n_recs);
    pivot = recs[0];
    for ( ; ; ) {
      /*
       * Prior entries are moved to left. Less prior entries are moved to
       * right. Entries which equal to the pivot are moved to the edges.
       */
      while (left < right) {
        if (pivot.score > recs[left].score) {
          break;
        } else if ((pivot.score <= recs[left].score) &&
                   (pivot.score >= recs[left].score)) {
          grn_ts_rec_swap(&recs[left], &recs[pivot_left]);
          ++pivot_left;
        }
        ++left;
      }
      while (left < right) {
        --right;
        if (recs[right].score > pivot.score) {
          break;
        } else if ((recs[right].score <= pivot.score) &&
                   (recs[right].score >= pivot.score)) {
          --pivot_right;
          grn_ts_rec_swap(&recs[right], &recs[pivot_right]);
        }
      }
      if (left >= right) {
        break;
      }
      grn_ts_rec_swap(&recs[left], &recs[right]);
      ++left;
    }
    /* Move left pivot-equivalent entries to the left of the boundary. */
    while (pivot_left > 0) {
      --pivot_left;
      --left;
      grn_ts_rec_swap(&recs[pivot_left], &recs[left]);
    }
    /* Move right pivot-equivalent entries to the right of the boundary. */
    while (pivot_right < n_recs) {
      grn_ts_rec_swap(&recs[pivot_right], &recs[right]);
      ++pivot_right;
      ++right;
    }
    /* Apply the next sort condition to the pivot-equivalent recs. */
    if (node->next) {
      if (((right - left) >= 2) && (offset < right) && (limit > left)) {
        size_t next_offset = (offset < left) ? 0 : (offset - left);
        size_t next_limit = ((limit > right) ? right : limit) - left;
        rc = grn_ts_sorter_node_sort(ctx, node->next, next_offset, next_limit,
                                     recs + left, right - left);
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
    }
    /*
     * Use a recursive call to sort the smaller group so that the recursion
     * depth is less than log_2(n_recs).
     */
    if (left < (n_recs - right)) {
      if ((offset < left) && (left >= 2)) {
        size_t next_limit = (limit < left) ? limit : left;
        rc = grn_ts_qsort_by_score_desc(ctx, node, offset, next_limit,
                                        recs, left);
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
      if (limit <= right) {
        return GRN_SUCCESS;
      }
      recs += right;
      n_recs -= right;
      offset = (offset < right) ? 0 : (offset - right);
      limit -= right;
    } else {
      if ((limit > right) && ((n_recs - right) >= 2)) {
        size_t next_offset = (offset < right) ? 0 : (offset - right);
        size_t next_limit = limit - right;
        rc = grn_ts_qsort_by_score_desc(ctx, node, next_offset, next_limit,
                                        recs + right, n_recs - right);
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
      if (offset >= left) {
        return GRN_SUCCESS;
      }
      n_recs = left;
      if (limit > left) {
        limit = left;
      }
    }
  }
  if (n_recs >= 2) {
    rc = grn_ts_isort_by_score_desc(ctx, node, offset, limit, recs, n_recs);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
  }
  return GRN_SUCCESS;
}

/* grn_ts_sorter_node_sort_by_score() sorts records by _score. */
static grn_rc
grn_ts_sorter_node_sort_by_score(grn_ctx *ctx, grn_ts_sorter_node *node,
                                 size_t offset, size_t limit,
                                 grn_ts_record *recs, size_t n_recs)
{
  if (node->reverse) {
    return grn_ts_qsort_by_score_desc(ctx, node, offset, limit, recs, n_recs);
  } else {
    return grn_ts_qsort_by_score_asc(ctx, node, offset, limit, recs, n_recs);
  }
}

/* grn_ts_move_pivot_by_int() moves the pivot to the front. */
static void
grn_ts_move_pivot_by_int(grn_ts_sorter_node *node, grn_ts_int *vals,
                         grn_ts_record *recs, size_t n_recs)
{
  /* Choose the median from recs[1], recs[n_recs / 2], and recs[n_recs - 2]. */
  size_t first = 1;
  size_t middle = n_recs / 2;
  size_t last = n_recs - 2;
  if (vals[first] < vals[middle]) {
    /* first < middle. */
    if (vals[middle] < vals[last]) {
      /* first < middle < last */
      grn_ts_rec_swap(&recs[0], &recs[middle]);
      grn_ts_int_swap(&vals[0], &vals[middle]);
    } else if (vals[first] < vals[last]) {
      /* first < last < middle. */
      grn_ts_rec_swap(&recs[0], &recs[last]);
      grn_ts_int_swap(&vals[0], &vals[last]);
    } else { /* last < first < middle. */
      grn_ts_rec_swap(&recs[0], &recs[first]);
      grn_ts_int_swap(&vals[0], &vals[first]);
    }
  } else if (vals[last] < vals[middle]) {
    /* last < middle < first. */
    grn_ts_rec_swap(&recs[0], &recs[middle]);
    grn_ts_int_swap(&vals[0], &vals[middle]);
  } else if (vals[last] < vals[first]) {
    /* middle < last < first. */
    grn_ts_rec_swap(&recs[0], &recs[last]);
    grn_ts_int_swap(&vals[0], &vals[last]);
  } else { /* middle < first < last. */
    grn_ts_rec_swap(&recs[0], &recs[first]);
    grn_ts_int_swap(&vals[0], &vals[first]);
  }
}

/* grn_ts_isort_by_int() sorts records. */
static grn_rc
grn_ts_isort_by_int(grn_ctx *ctx, grn_ts_sorter_node *node,
                    size_t offset, size_t limit,
                    grn_ts_int *vals, grn_ts_record *recs, size_t n_recs)
{
  for (size_t i = 1; i < n_recs; ++i) {
    for (size_t j = i; j > 0; --j) {
      if (vals[j] < vals[j - 1]) {
        grn_ts_rec_swap(&recs[j], &recs[j - 1]);
        grn_ts_int_swap(&vals[j], &vals[j - 1]);
      } else {
        break;
      }
    }
  }
  /* Apply the next sorting if there are score duplicates. */
  if (node->next) {
    grn_rc rc;
    size_t begin = 0;
    for (size_t i = 1; i < n_recs; ++i) {
      if (vals[i] != vals[begin]) {
        if ((i - begin) >= 2) {
          rc = grn_ts_sorter_node_sort(ctx, node->next, 0, i - begin,
                                       recs + begin, i - begin);
          if (rc != GRN_SUCCESS) {
            return rc;
          }
        }
        begin = i;
      }
    }
    if ((n_recs - begin) >= 2) {
      rc = grn_ts_sorter_node_sort(ctx, node->next, 0, n_recs - begin,
                                   recs + begin, n_recs - begin);
      if (rc != GRN_SUCCESS) {
        return rc;
      }
    }
  }
  return GRN_SUCCESS;
}

/* grn_ts_qsort_by_int() sorts records. */
static grn_rc
grn_ts_qsort_by_int(grn_ctx *ctx, grn_ts_sorter_node *node,
                    size_t offset, size_t limit,
                    grn_ts_int *vals, grn_ts_record *recs, size_t n_recs)
{
  grn_rc rc;
  /*
   * FIXME: Currently, the threshold is 16.
   *        This value should be optimized and replaced with a named constant.
   */
  while (n_recs >= 16) {
    grn_ts_int pivot;
    size_t left, right;
    size_t pivot_left, pivot_right;
    grn_ts_move_pivot_by_int(node, vals, recs, n_recs);
    pivot = vals[0];
    left = 1;
    right = n_recs;
    pivot_left = 1;
    pivot_right = n_recs;
    for ( ; ; ) {
      /*
       * Prior entries are moved to left. Less prior entries are moved to
       * right. Entries which equal to the pivot are moved to the edges.
       */
      while (left < right) {
        if (pivot < vals[left]) {
          break;
        } else if (pivot == vals[left]) {
          grn_ts_rec_swap(&recs[left], &recs[pivot_left]);
          grn_ts_int_swap(&vals[left], &vals[pivot_left]);
          ++pivot_left;
        }
        ++left;
      }
      while (left < right) {
        --right;
        if (vals[right] < pivot) {
          break;
        } else if (vals[right] == pivot) {
          --pivot_right;
          grn_ts_rec_swap(&recs[right], &recs[pivot_right]);
          grn_ts_int_swap(&vals[right], &vals[pivot_right]);
        }
      }
      if (left >= right) {
        break;
      }
      grn_ts_rec_swap(&recs[left], &recs[right]);
      grn_ts_int_swap(&vals[left], &vals[right]);
      ++left;
    }
    /* Move left pivot-equivalent entries to the left of the boundary. */
    while (pivot_left > 0) {
      --pivot_left;
      --left;
      grn_ts_rec_swap(&recs[pivot_left], &recs[left]);
      grn_ts_int_swap(&vals[pivot_left], &vals[left]);
    }
    /* Move right pivot-equivalent entries to the right of the boundary. */
    while (pivot_right < n_recs) {
      grn_ts_rec_swap(&recs[pivot_right], &recs[right]);
      grn_ts_int_swap(&vals[pivot_right], &vals[right]);
      ++pivot_right;
      ++right;
    }
    /* Apply the next sort condition to the pivot-equivalent recs. */
    if (node->next) {
      if (((right - left) >= 2) && (offset < right) && (limit > left)) {
        size_t next_offset = (offset < left) ? 0 : (offset - left);
        size_t next_limit = ((limit > right) ? right : limit) - left;
        rc = grn_ts_sorter_node_sort(ctx, node->next, next_offset, next_limit,
                                     recs + left, right - left);
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
    }
    /*
     * Use a recursive call to sort the smaller group so that the recursion
     * depth is less than log_2(n_recs).
     */
    if (left < (n_recs - right)) {
      if ((offset < left) && (left >= 2)) {
        size_t next_limit = (limit < left) ? limit : left;
        rc = grn_ts_qsort_by_int(ctx, node, offset, next_limit,
                                 vals, recs, left);
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
      if (limit <= right) {
        return GRN_SUCCESS;
      }
      vals += right;
      recs += right;
      n_recs -= right;
      offset = (offset < right) ? 0 : (offset - right);
      limit -= right;
    } else {
      if ((limit > right) && ((n_recs - right) >= 2)) {
        size_t next_offset = (offset < right) ? 0 : (offset - right);
        size_t next_limit = limit - right;
        rc = grn_ts_qsort_by_int(ctx, node, next_offset, next_limit,
                                 vals + right, recs + right, n_recs - right);
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
      if (offset >= left) {
        return GRN_SUCCESS;
      }
      n_recs = left;
      if (limit > left) {
        limit = left;
      }
    }
  }
  if (n_recs >= 2) {
    rc = grn_ts_isort_by_int(ctx, node, offset, limit, vals, recs, n_recs);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
  }
  return GRN_SUCCESS;
}

/* grn_ts_text_cmp() compares Text values. */
inline static int
grn_ts_text_cmp(grn_ts_text lhs, grn_ts_text rhs)
{
  size_t min_size = (lhs.size < rhs.size) ? lhs.size : rhs.size;
  int result = memcmp(lhs.ptr, rhs.ptr, min_size);
  if (result != 0) {
    return result;
  }
  if (lhs.size == rhs.size) {
    return 0;
  }
  return (lhs.size < rhs.size) ? -1 : 1;
}

/* grn_ts_text_swap() swaps Text values. */
inline static void
grn_ts_text_swap(grn_ts_text *lhs, grn_ts_text *rhs)
{
  grn_ts_text tmp = *lhs;
  *lhs = *rhs;
  *rhs = tmp;
}

#if 0
/* grn_ts_move_pivot_by_text_asc() moves the pivot to the front. */
static void
grn_ts_move_pivot_by_text_asc(grn_ts_sorter_node *node, grn_ts_text *vals,
                              grn_ts_record *recs, size_t n_recs)
{
  /* Choose the median from recs[1], recs[n_recs / 2], and recs[n_recs - 2]. */
  size_t first = 1;
  size_t middle = n_recs / 2;
  size_t last = n_recs - 2;
  if (grn_ts_text_cmp(vals[first], vals[middle]) < 0) {
    /* first < middle. */
    if (grn_ts_text_cmp(vals[middle], vals[last]) < 0) {
      /* first < middle < last */
      grn_ts_rec_swap(&recs[0], &recs[middle]);
      grn_ts_text_swap(&vals[0], &vals[middle]);
    } else if (grn_ts_text_cmp(vals[first], vals[last]) < 0) {
      /* first < last < middle. */
      grn_ts_rec_swap(&recs[0], &recs[last]);
      grn_ts_text_swap(&vals[0], &vals[last]);
    } else { /* last < first < middle. */
      grn_ts_rec_swap(&recs[0], &recs[first]);
      grn_ts_text_swap(&vals[0], &vals[first]);
    }
  } else if (grn_ts_text_cmp(vals[last], vals[middle]) < 0) {
    /* last < middle < first. */
    grn_ts_rec_swap(&recs[0], &recs[middle]);
    grn_ts_text_swap(&vals[0], &vals[middle]);
  } else if (grn_ts_text_cmp(vals[last], vals[first]) < 0) {
    /* middle < last < first. */
    grn_ts_rec_swap(&recs[0], &recs[last]);
    grn_ts_text_swap(&vals[0], &vals[last]);
  } else { /* middle < first < last. */
    grn_ts_rec_swap(&recs[0], &recs[first]);
    grn_ts_text_swap(&vals[0], &vals[first]);
  }
}

/* grn_ts_isort_by_text_asc() sorts records. */
static grn_rc
grn_ts_isort_by_text_asc(grn_ctx *ctx, grn_ts_sorter_node *node,
                         size_t offset, size_t limit,
                         grn_ts_text *vals, grn_ts_record *recs, size_t n_recs)
{
  for (size_t i = 1; i < n_recs; ++i) {
    for (size_t j = i; j > 0; --j) {
      if (grn_ts_text_cmp(vals[j], vals[j - 1]) < 0) {
        grn_ts_rec_swap(&recs[j], &recs[j - 1]);
        grn_ts_text_swap(&vals[j], &vals[j - 1]);
      } else {
        break;
      }
    }
  }
  /* Apply the next sorting if there are score duplicates. */
  if (node->next) {
    grn_rc rc;
    size_t begin = 0;
    for (size_t i = 1; i < n_recs; ++i) {
      if (grn_ts_text_cmp(vals[i], vals[begin]) != 0) {
        if ((i - begin) >= 2) {
          rc = grn_ts_sorter_node_sort(ctx, node->next, 0, i - begin,
                                       recs + begin, i - begin);
          if (rc != GRN_SUCCESS) {
            return rc;
          }
        }
        begin = i;
      }
    }
    if ((n_recs - begin) >= 2) {
      rc = grn_ts_sorter_node_sort(ctx, node->next, 0, n_recs - begin,
                                   recs + begin, n_recs - begin);
      if (rc != GRN_SUCCESS) {
        return rc;
      }
    }
  }
  return GRN_SUCCESS;
}

/* grn_ts_qsort_by_text_asc() sorts records. */
static grn_rc
grn_ts_qsort_by_text_asc(grn_ctx *ctx, grn_ts_sorter_node *node,
                         size_t offset, size_t limit,
                         grn_ts_text *vals, grn_ts_record *recs, size_t n_recs)
{
  grn_rc rc;
  /*
   * FIXME: Currently, the threshold is 16.
   *        This value should be optimized and replaced with a named constant.
   */
  while (n_recs >= 16) {
    grn_ts_move_pivot_by_text_asc(node, vals, recs, n_recs);
    grn_ts_text pivot = vals[0];
    size_t left = 1, right = n_recs;
    size_t pivot_left = 1, pivot_right = n_recs;
    for ( ; ; ) {
      /*
       * Prior entries are moved to left. Less prior entries are moved to
       * right. Entries which equal to the pivot are moved to the edges.
       */
      while (left < right) {
        int result = grn_ts_text_cmp(pivot, vals[left]);
        if (result < 0) {
          break;
        } else if (result == 0) {
          grn_ts_rec_swap(&recs[left], &recs[pivot_left]);
          grn_ts_text_swap(&vals[left], &vals[pivot_left]);
          ++pivot_left;
        }
        ++left;
      }
      while (left < right) {
        int result;
        --right;
        result = grn_ts_text_cmp(vals[right], pivot);
        if (result < 0) {
          break;
        } else if (result == 0) {
          --pivot_right;
          grn_ts_rec_swap(&recs[right], &recs[pivot_right]);
          grn_ts_text_swap(&vals[right], &vals[pivot_right]);
        }
      }
      if (left >= right) {
        break;
      }
      grn_ts_rec_swap(&recs[left], &recs[right]);
      grn_ts_text_swap(&vals[left], &vals[right]);
      ++left;
    }
    /* Move left pivot-equivalent entries to the left of the boundary. */
    while (pivot_left > 0) {
      --pivot_left;
      --left;
      grn_ts_rec_swap(&recs[pivot_left], &recs[left]);
      grn_ts_text_swap(&vals[pivot_left], &vals[left]);
    }
    /* Move right pivot-equivalent entries to the right of the boundary. */
    while (pivot_right < n_recs) {
      grn_ts_rec_swap(&recs[pivot_right], &recs[right]);
      grn_ts_text_swap(&vals[pivot_right], &vals[right]);
      ++pivot_right;
      ++right;
    }
    /* Apply the next sort condition to the pivot-equivalent recs. */
    if (node->next) {
      if (((right - left) >= 2) && (offset < right) && (limit > left)) {
        size_t next_offset = (offset < left) ? 0 : (offset - left);
        size_t next_limit = ((limit > right) ? right : limit) - left;
        rc = grn_ts_sorter_node_sort(ctx, node->next, next_offset, next_limit,
                                     recs + left, right - left);
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
    }
    /*
     * Use a recursive call to sort the smaller group so that the recursion
     * depth is less than log_2(n_recs).
     */
    if (left < (n_recs - right)) {
      if ((offset < left) && (left >= 2)) {
        size_t next_limit = (limit < left) ? limit : left;
        rc = grn_ts_qsort_by_text_asc(ctx, node, offset, next_limit,
                                      vals, recs, left);
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
      if (limit <= right) {
        return GRN_SUCCESS;
      }
      vals += right;
      recs += right;
      n_recs -= right;
      offset = (offset < right) ? 0 : (offset - right);
      limit -= right;
    } else {
      if ((limit > right) && ((n_recs - right) >= 2)) {
        size_t next_offset = (offset < right) ? 0 : (offset - right);
        size_t next_limit = limit - right;
        rc = grn_ts_qsort_by_text_asc(ctx, node, next_offset, next_limit,
                                      vals + right, recs + right,
                                      n_recs - right);
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
      if (offset >= left) {
        return GRN_SUCCESS;
      }
      n_recs = left;
      if (limit > left) {
        limit = left;
      }
    }
  }
  if (n_recs >= 2) {
    rc = grn_ts_isort_by_text_asc(ctx, node, offset, limit,
                                  vals, recs, n_recs);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
  }
  return GRN_SUCCESS;
}
#endif

/* grn_ts_move_pivot_by_text_desc() moves the pivot to the front. */
static void
grn_ts_move_pivot_by_text_desc(grn_ts_sorter_node *node, grn_ts_text *vals,
                               grn_ts_record *recs, size_t n_recs)
{
  /* Choose the median from recs[1], recs[n_recs / 2], and recs[n_recs - 2]. */
  size_t first = 1;
  size_t middle = n_recs / 2;
  size_t last = n_recs - 2;
  if (grn_ts_text_cmp(vals[first], vals[middle]) > 0) {
    /* first < middle. */
    if (grn_ts_text_cmp(vals[middle], vals[last]) > 0) {
      /* first < middle < last */
      grn_ts_rec_swap(&recs[0], &recs[middle]);
      grn_ts_text_swap(&vals[0], &vals[middle]);
    } else if (grn_ts_text_cmp(vals[first], vals[last]) > 0) {
      /* first < last < middle. */
      grn_ts_rec_swap(&recs[0], &recs[last]);
      grn_ts_text_swap(&vals[0], &vals[last]);
    } else { /* last < first < middle. */
      grn_ts_rec_swap(&recs[0], &recs[first]);
      grn_ts_text_swap(&vals[0], &vals[first]);
    }
  } else if (grn_ts_text_cmp(vals[last], vals[middle]) > 0) {
    /* last < middle < first. */
    grn_ts_rec_swap(&recs[0], &recs[middle]);
    grn_ts_text_swap(&vals[0], &vals[middle]);
  } else if (grn_ts_text_cmp(vals[last], vals[first]) > 0) {
    /* middle < last < first. */
    grn_ts_rec_swap(&recs[0], &recs[last]);
    grn_ts_text_swap(&vals[0], &vals[last]);
  } else { /* middle < first < last. */
    grn_ts_rec_swap(&recs[0], &recs[first]);
    grn_ts_text_swap(&vals[0], &vals[first]);
  }
}

/* grn_ts_isort_by_text_desc() sorts records. */
static grn_rc
grn_ts_isort_by_text_desc(grn_ctx *ctx, grn_ts_sorter_node *node,
                          size_t offset, size_t limit,
                          grn_ts_text *vals, grn_ts_record *recs,
                          size_t n_recs)
{
  for (size_t i = 1; i < n_recs; ++i) {
    for (size_t j = i; j > 0; --j) {
      if (grn_ts_text_cmp(vals[j], vals[j - 1]) > 0) {
        grn_ts_rec_swap(&recs[j], &recs[j - 1]);
        grn_ts_text_swap(&vals[j], &vals[j - 1]);
      } else {
        break;
      }
    }
  }
  /* Apply the next sorting if there are score duplicates. */
  if (node->next) {
    grn_rc rc;
    size_t begin = 0;
    for (size_t i = 1; i < n_recs; ++i) {
      if (grn_ts_text_cmp(vals[i], vals[begin]) != 0) {
        if ((i - begin) >= 2) {
          rc = grn_ts_sorter_node_sort(ctx, node->next, 0, i - begin,
                                       recs + begin, i - begin);
          if (rc != GRN_SUCCESS) {
            return rc;
          }
        }
        begin = i;
      }
    }
    if ((n_recs - begin) >= 2) {
      rc = grn_ts_sorter_node_sort(ctx, node->next, 0, n_recs - begin,
                                   recs + begin, n_recs - begin);
      if (rc != GRN_SUCCESS) {
        return rc;
      }
    }
  }
  return GRN_SUCCESS;
}

/* grn_ts_qsort_by_text_desc() sorts records. */
static grn_rc
grn_ts_qsort_by_text_desc(grn_ctx *ctx, grn_ts_sorter_node *node,
                          size_t offset, size_t limit,
                          grn_ts_text *vals, grn_ts_record *recs,
                          size_t n_recs)
{
  grn_rc rc;
  /*
   * FIXME: Currently, the threshold is 16.
   *        This value should be optimized and replaced with a named constant.
   */
  while (n_recs >= 16) {
    grn_ts_text pivot;
    size_t left, right;
    size_t pivot_left, pivot_right;
    grn_ts_move_pivot_by_text_desc(node, vals, recs, n_recs);
    pivot = vals[0];
    left = 1;
    right = n_recs;
    pivot_left = 1;
    pivot_right = n_recs;
    for ( ; ; ) {
      /*
       * Prior entries are moved to left. Less prior entries are moved to
       * right. Entries which equal to the pivot are moved to the edges.
       */
      while (left < right) {
        int result = grn_ts_text_cmp(pivot, vals[left]);
        if (result > 0) {
          break;
        } else if (result == 0) {
          grn_ts_rec_swap(&recs[left], &recs[pivot_left]);
          grn_ts_text_swap(&vals[left], &vals[pivot_left]);
          ++pivot_left;
        }
        ++left;
      }
      while (left < right) {
        int result;
        --right;
        result = grn_ts_text_cmp(vals[right], pivot);
        if (result > 0) {
          break;
        } else if (result == 0) {
          --pivot_right;
          grn_ts_rec_swap(&recs[right], &recs[pivot_right]);
          grn_ts_text_swap(&vals[right], &vals[pivot_right]);
        }
      }
      if (left >= right) {
        break;
      }
      grn_ts_rec_swap(&recs[left], &recs[right]);
      grn_ts_text_swap(&vals[left], &vals[right]);
      ++left;
    }
    /* Move left pivot-equivalent entries to the left of the boundary. */
    while (pivot_left > 0) {
      --pivot_left;
      --left;
      grn_ts_rec_swap(&recs[pivot_left], &recs[left]);
      grn_ts_text_swap(&vals[pivot_left], &vals[left]);
    }
    /* Move right pivot-equivalent entries to the right of the boundary. */
    while (pivot_right < n_recs) {
      grn_ts_rec_swap(&recs[pivot_right], &recs[right]);
      grn_ts_text_swap(&vals[pivot_right], &vals[right]);
      ++pivot_right;
      ++right;
    }
    /* Apply the next sort condition to the pivot-equivalent recs. */
    if (node->next) {
      if (((right - left) >= 2) && (offset < right) && (limit > left)) {
        size_t next_offset = (offset < left) ? 0 : (offset - left);
        size_t next_limit = ((limit > right) ? right : limit) - left;
        rc = grn_ts_sorter_node_sort(ctx, node->next, next_offset, next_limit,
                                     recs + left, right - left);
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
    }
    /*
     * Use a recursive call to sort the smaller group so that the recursion
     * depth is less than log_2(n_recs).
     */
    if (left < (n_recs - right)) {
      if ((offset < left) && (left >= 2)) {
        size_t next_limit = (limit < left) ? limit : left;
        rc = grn_ts_qsort_by_text_desc(ctx, node, offset, next_limit,
                                       vals, recs, left);
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
      if (limit <= right) {
        return GRN_SUCCESS;
      }
      vals += right;
      recs += right;
      n_recs -= right;
      offset = (offset < right) ? 0 : (offset - right);
      limit -= right;
    } else {
      if ((limit > right) && ((n_recs - right) >= 2)) {
        size_t next_offset = (offset < right) ? 0 : (offset - right);
        size_t next_limit = limit - right;
        rc = grn_ts_qsort_by_text_desc(ctx, node, next_offset, next_limit,
                                       vals + right, recs + right,
                                       n_recs - right);
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
      if (offset >= left) {
        return GRN_SUCCESS;
      }
      n_recs = left;
      if (limit > left) {
        limit = left;
      }
    }
  }
  if (n_recs >= 2) {
    rc = grn_ts_isort_by_text_desc(ctx, node, offset, limit,
                                   vals, recs, n_recs);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
  }
  return GRN_SUCCESS;
}

/* grn_ts_text_get_label() returns a label. */
inline static int
grn_ts_text_get_label(grn_ts_text val, size_t depth)
{
  return (depth < val.size) ? (uint8_t)val.ptr[depth] : -1;
}

/* grn_ts_text_cmp2() compares Text values. */
inline static int
grn_ts_text_cmp2(grn_ts_text lhs, grn_ts_text rhs, size_t depth)
{
  size_t min_size = (lhs.size < rhs.size) ? lhs.size : rhs.size;
  int result = memcmp(lhs.ptr + depth, rhs.ptr + depth, min_size - depth);
  if (result != 0) {
    return result;
  }
  if (lhs.size == rhs.size) {
    return 0;
  }
  return (lhs.size < rhs.size) ? -1 : 1;
}

/* grn_ts_move_pivot_by_text_asc2() moves the pivot to the front. */
static void
grn_ts_move_pivot_by_text_asc2(grn_ts_sorter_node *node, grn_ts_text *vals,
                               grn_ts_record *recs, size_t n_recs, size_t depth)
{
  /* Choose the median from recs[1], recs[n_recs / 2], and recs[n_recs - 2]. */
  size_t first = 1;
  size_t middle = n_recs / 2;
  size_t last = n_recs - 2;
  int first_label = grn_ts_text_get_label(vals[first], depth);
  int middle_label = grn_ts_text_get_label(vals[middle], depth);
  int last_label = grn_ts_text_get_label(vals[last], depth);
  if (first_label < middle_label) {
    /* first < middle. */
    if (middle_label < last_label) {
      /* first < middle < last */
      grn_ts_rec_swap(&recs[0], &recs[middle]);
      grn_ts_text_swap(&vals[0], &vals[middle]);
    } else if (first_label < last_label) {
      /* first < last < middle. */
      grn_ts_rec_swap(&recs[0], &recs[last]);
      grn_ts_text_swap(&vals[0], &vals[last]);
    } else { /* last < first < middle. */
      grn_ts_rec_swap(&recs[0], &recs[first]);
      grn_ts_text_swap(&vals[0], &vals[first]);
    }
  } else if (last_label < middle_label) {
    /* last < middle < first. */
    grn_ts_rec_swap(&recs[0], &recs[middle]);
    grn_ts_text_swap(&vals[0], &vals[middle]);
  } else if (last_label < first_label) {
    /* middle < last < first. */
    grn_ts_rec_swap(&recs[0], &recs[last]);
    grn_ts_text_swap(&vals[0], &vals[last]);
  } else { /* middle < first < last. */
    grn_ts_rec_swap(&recs[0], &recs[first]);
    grn_ts_text_swap(&vals[0], &vals[first]);
  }
}

/* grn_ts_isort_by_text_asc2() sorts records. */
static grn_rc
grn_ts_isort_by_text_asc2(grn_ctx *ctx, grn_ts_sorter_node *node,
                          size_t offset, size_t limit, grn_ts_text *vals,
                          grn_ts_record *recs, size_t n_recs, size_t depth)
{
  for (size_t i = 1; i < n_recs; ++i) {
    for (size_t j = i; j > 0; --j) {
      if (grn_ts_text_cmp2(vals[j], vals[j - 1], depth) < 0) {
        grn_ts_rec_swap(&recs[j], &recs[j - 1]);
        grn_ts_text_swap(&vals[j], &vals[j - 1]);
      } else {
        break;
      }
    }
  }
  /* Apply the next sorting if there are score duplicates. */
  if (node->next) {
    grn_rc rc;
    size_t begin = 0;
    for (size_t i = 1; i < n_recs; ++i) {
      if (grn_ts_text_cmp2(vals[i], vals[begin], depth) != 0) {
        if ((i - begin) >= 2) {
          rc = grn_ts_sorter_node_sort(ctx, node->next, 0, i - begin,
                                       recs + begin, i - begin);
          if (rc != GRN_SUCCESS) {
            return rc;
          }
        }
        begin = i;
      }
    }
    if ((n_recs - begin) >= 2) {
      rc = grn_ts_sorter_node_sort(ctx, node->next, 0, n_recs - begin,
                                   recs + begin, n_recs - begin);
      if (rc != GRN_SUCCESS) {
        return rc;
      }
    }
  }
  return GRN_SUCCESS;
}

/* grn_ts_qsort_by_text_asc() sorts records. */
static grn_rc
grn_ts_qsort_by_text_asc2(grn_ctx *ctx, grn_ts_sorter_node *node,
                          size_t offset, size_t limit, grn_ts_text *vals,
                          grn_ts_record *recs, size_t n_recs, size_t depth)
{
  grn_rc rc;
  /*
   * FIXME: Currently, the threshold is 16.
   *        This value should be optimized and replaced with a named constant.
   */
  while (n_recs >= 16) {
    int pivot;
    size_t left, right;
    size_t pivot_left, pivot_right;
    grn_ts_move_pivot_by_text_asc2(node, vals, recs, n_recs, depth);
    pivot = grn_ts_text_get_label(vals[0], depth);
    left = 1;
    right = n_recs;
    pivot_left = 1;
    pivot_right = n_recs;
    for ( ; ; ) {
      /*
       * Prior entries are moved to left. Less prior entries are moved to
       * right. Entries which equal to the pivot are moved to the edges.
       */
      while (left < right) {
        int label = grn_ts_text_get_label(vals[left], depth);
        if (label > pivot) {
          break;
        } else if (label == pivot) {
          grn_ts_rec_swap(&recs[left], &recs[pivot_left]);
          grn_ts_text_swap(&vals[left], &vals[pivot_left]);
          ++pivot_left;
        }
        ++left;
      }
      while (left < right) {
        int label;
        --right;
        label = grn_ts_text_get_label(vals[right], depth);
        if (label < pivot) {
          break;
        } else if (label == pivot) {
          --pivot_right;
          grn_ts_rec_swap(&recs[right], &recs[pivot_right]);
          grn_ts_text_swap(&vals[right], &vals[pivot_right]);
        }
      }
      if (left >= right) {
        break;
      }
      grn_ts_rec_swap(&recs[left], &recs[right]);
      grn_ts_text_swap(&vals[left], &vals[right]);
      ++left;
    }
    /* Move left pivot-equivalent entries to the left of the boundary. */
    while (pivot_left > 0) {
      --pivot_left;
      --left;
      grn_ts_rec_swap(&recs[pivot_left], &recs[left]);
      grn_ts_text_swap(&vals[pivot_left], &vals[left]);
    }
    /* Move right pivot-equivalent entries to the right of the boundary. */
    while (pivot_right < n_recs) {
      grn_ts_rec_swap(&recs[pivot_right], &recs[right]);
      grn_ts_text_swap(&vals[pivot_right], &vals[right]);
      ++pivot_right;
      ++right;
    }
    /* Apply the next sort condition to the pivot-equivalent recs. */
    if (((right - left) >= 2) && (offset < right) && (limit > left)) {
      size_t next_offset = (offset < left) ? 0 : (offset - left);
      size_t next_limit = ((limit > right) ? right : limit) - left;
      if (pivot != -1) {
        rc = grn_ts_qsort_by_text_asc2(ctx, node, next_offset, next_limit,
                                       vals, recs + left, right - left,
                                       depth + 1);
      } else if (node->next) {
        rc = grn_ts_sorter_node_sort(ctx, node->next, next_offset, next_limit,
                                     recs + left, right - left);
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
    }
    /*
     * Use a recursive call to sort the smaller group so that the recursion
     * depth is less than log_2(n_recs).
     */
    if (left < (n_recs - right)) {
      if ((offset < left) && (left >= 2)) {
        size_t next_limit = (limit < left) ? limit : left;
        rc = grn_ts_qsort_by_text_asc2(ctx, node, offset, next_limit,
                                       vals, recs, left, depth);
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
      if (limit <= right) {
        return GRN_SUCCESS;
      }
      vals += right;
      recs += right;
      n_recs -= right;
      offset = (offset < right) ? 0 : (offset - right);
      limit -= right;
    } else {
      if ((limit > right) && ((n_recs - right) >= 2)) {
        size_t next_offset = (offset < right) ? 0 : (offset - right);
        size_t next_limit = limit - right;
        rc = grn_ts_qsort_by_text_asc2(ctx, node, next_offset, next_limit,
                                       vals + right, recs + right,
                                       n_recs - right, depth);
        if (rc != GRN_SUCCESS) {
          return rc;
        }
      }
      if (offset >= left) {
        return GRN_SUCCESS;
      }
      n_recs = left;
      if (limit > left) {
        limit = left;
      }
    }
  }
  if (n_recs >= 2) {
    rc = grn_ts_isort_by_text_asc2(ctx, node, offset, limit,
                                   vals, recs, n_recs, depth);
    if (rc != GRN_SUCCESS) {
      return rc;
    }
  }
  return GRN_SUCCESS;
}

/* grn_ts_sorter_node_sort_by_var() sorts records. */
static grn_rc
grn_ts_sorter_node_sort_by_var(grn_ctx *ctx, grn_ts_sorter_node *node,
                               size_t offset, size_t limit,
                               grn_ts_record *recs, size_t n_recs)
{
  size_t i;
  grn_rc rc;
  switch (node->expr->data_kind) {
    case GRN_TS_INT: {
      grn_ts_int *vals;
      rc = grn_ts_expr_evaluate_to_buf(ctx, node->expr, recs, n_recs,
                                       &node->buf);
      if (rc != GRN_SUCCESS) {
        return rc;
      }
      vals = (grn_ts_int *)node->buf.ptr;
      if (node->reverse) {
        for (i = 0; i < n_recs; i++) {
          vals[i] = -1 - vals[i];
        }
      }
      return grn_ts_qsort_by_int(ctx, node, offset, limit, vals, recs, n_recs);
    }
    case GRN_TS_FLOAT: {
      grn_ts_int *vals;
      rc = grn_ts_expr_evaluate_to_buf(ctx, node->expr, recs, n_recs,
                                       &node->buf);
      if (rc != GRN_SUCCESS) {
        return rc;
      }
      vals = (grn_ts_int *)node->buf.ptr;
      if (node->reverse) {
        for (i = 0; i < n_recs; i++) {
          if (vals[i] < 0) {
            vals[i] = (vals[i] ^ INT64_MAX) + 1;
          }
          vals[i] = -1 - vals[i];
        }
      } else {
        for (i = 0; i < n_recs; i++) {
          if (vals[i] < 0) {
            vals[i] = (vals[i] ^ INT64_MAX) + 1;
          }
        }
      }
      return grn_ts_qsort_by_int(ctx, node, offset, limit, vals, recs, n_recs);
    }
    case GRN_TS_TIME: {
      grn_ts_int *vals;
      rc = grn_ts_expr_evaluate_to_buf(ctx, node->expr, recs, n_recs,
                                       &node->buf);
      if (rc != GRN_SUCCESS) {
        return rc;
      }
      vals = (grn_ts_int *)node->buf.ptr;
      if (node->reverse) {
        for (i = 0; i < n_recs; i++) {
          vals[i] = -1 - vals[i];
        }
      }
      return grn_ts_qsort_by_int(ctx, node, offset, limit, vals, recs, n_recs);
    }
    case GRN_TS_TEXT: {
      grn_ts_text *vals;
      rc = grn_ts_expr_evaluate_to_buf(ctx, node->expr, recs, n_recs,
                                       &node->buf);
      if (rc != GRN_SUCCESS) {
        return rc;
      }
      vals = (grn_ts_text *)node->buf.ptr;
      if (node->reverse) {
        return grn_ts_qsort_by_text_desc(ctx, node, offset, limit,
                                         vals, recs, n_recs);
      } else {
        return grn_ts_qsort_by_text_asc2(ctx, node, offset, limit,
                                         vals, recs, n_recs, 0);
      }
    }
    case GRN_TS_INT_VECTOR:
    case GRN_TS_FLOAT_VECTOR:
    case GRN_TS_TIME_VECTOR:
    case GRN_TS_TEXT_VECTOR: {
      // TODO
      GRN_TS_ERR_RETURN(GRN_OPERATION_NOT_SUPPORTED, "not supported yet");
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid data kind: %d",
                        node->expr->data_kind);
    }
  }
  return GRN_FUNCTION_NOT_IMPLEMENTED;
}

/* grn_ts_sorter_node_sort() sorts records. */
static grn_rc
grn_ts_sorter_node_sort(grn_ctx *ctx, grn_ts_sorter_node *node,
                        size_t offset, size_t limit,
                        grn_ts_record *recs, size_t n_recs)
{
  switch (node->expr->type) {
    case GRN_TS_EXPR_ID: {
      return grn_ts_sorter_node_sort_by_id(ctx, node, offset, limit,
                                           recs, n_recs);
    }
    case GRN_TS_EXPR_SCORE: {
      return grn_ts_sorter_node_sort_by_score(ctx, node, offset, limit,
                                              recs, n_recs);
    }
    case GRN_TS_EXPR_CONST: {
      if (!node->next) {
        return GRN_SUCCESS;
      }
      return grn_ts_sorter_node_sort(ctx, node->next, offset, limit, recs,
                                     n_recs);
    }
    case GRN_TS_EXPR_VARIABLE: {
      return grn_ts_sorter_node_sort_by_var(ctx, node, offset, limit,
                                            recs, n_recs);
      break;
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_OBJECT_CORRUPT, "invalid expr type: %d",
                        node->expr->type);
    }
  }
}

/*-------------------------------------------------------------
 * grn_ts_sorter.
 */

static void
grn_ts_sorter_init(grn_ctx *ctx, grn_ts_sorter *sorter)
{
  memset(sorter, 0, sizeof(*sorter));
  sorter->table = NULL;
  sorter->head = NULL;
}

static void
grn_ts_sorter_fin(grn_ctx *ctx, grn_ts_sorter *sorter)
{
  if (sorter->head) {
    grn_ts_sorter_node_list_close(ctx, sorter->head);
  }
  if (sorter->table) {
    grn_obj_unlink(ctx, sorter->table);
  }
}

grn_rc
grn_ts_sorter_open(grn_ctx *ctx, grn_obj *table, grn_ts_sorter_node *head,
                   size_t offset, size_t limit, grn_ts_sorter **sorter)
{
  grn_rc rc;
  grn_ts_sorter *new_sorter;
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!table || !grn_ts_obj_is_table(ctx, table) || !head || !sorter) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  new_sorter = GRN_MALLOCN(grn_ts_sorter, 1);
  if (!new_sorter) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT,
                      "GRN_MALLOCN failed: %" GRN_FMT_SIZE " x 1",
                      sizeof(grn_ts_sorter));
  }
  rc = grn_ts_obj_increment_ref_count(ctx, table);
  if (rc != GRN_SUCCESS) {
    GRN_FREE(new_sorter);
    return rc;
  }
  grn_ts_sorter_init(ctx, new_sorter);
  new_sorter->table = table;
  new_sorter->head = head;
  new_sorter->offset = offset;
  new_sorter->limit = limit;
  /* FIXME: Enable partial sorting. */
/*  new_sorter->partial = (offset + limit) < 1000;*/
  *sorter = new_sorter;
  return GRN_SUCCESS;
}

grn_rc
grn_ts_sorter_parse(grn_ctx *ctx, grn_obj *table,
                    grn_ts_str str, size_t offset,
                    size_t limit, grn_ts_sorter **sorter)
{
  grn_rc rc;
  grn_ts_sorter *new_sorter = NULL;
  grn_ts_expr_parser *parser;
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!table || !grn_ts_obj_is_table(ctx, table) || !str.size || !sorter) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  rc = grn_ts_expr_parser_open(ctx, table, &parser);
  if (rc == GRN_SUCCESS) {
    grn_ts_sorter_builder *builder;
    rc = grn_ts_sorter_builder_open(ctx, table, &builder);
    if (rc == GRN_SUCCESS) {
      grn_ts_str first, rest = str;
      for ( ; ; ) {
        grn_ts_expr *expr;
        grn_ts_bool reverse = GRN_FALSE;
        rc = grn_ts_expr_parser_split(ctx, parser, rest, &first, &rest);
        if (rc == GRN_END_OF_DATA) {
          rc = grn_ts_sorter_builder_complete(ctx, builder, offset, limit,
                                              &new_sorter);
          break;
        } else if (rc != GRN_SUCCESS) {
          break;
        }
        if (first.ptr[0] == '-') {
          reverse = GRN_TRUE;
          first.ptr++;
          first.size--;
        }
        rc = grn_ts_expr_parser_parse(ctx, parser, first, &expr);
        if (rc != GRN_SUCCESS) {
          break;
        }
        rc = grn_ts_sorter_builder_push(ctx, builder, expr, reverse);
        if (rc != GRN_SUCCESS) {
          grn_ts_expr_close(ctx, expr);
          break;
        }
      }
      grn_ts_sorter_builder_close(ctx, builder);
    }
    grn_ts_expr_parser_close(ctx, parser);
  }
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  *sorter = new_sorter;
  return GRN_SUCCESS;
}

grn_rc
grn_ts_sorter_close(grn_ctx *ctx, grn_ts_sorter *sorter)
{
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!sorter) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  grn_ts_sorter_fin(ctx, sorter);
  GRN_FREE(sorter);
  return GRN_SUCCESS;
}

grn_rc
grn_ts_sorter_progress(grn_ctx *ctx, grn_ts_sorter *sorter,
                       grn_ts_record *recs, size_t n_recs, size_t *n_rest)
{
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!sorter || (!recs && n_recs) || !n_rest) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  if (sorter->partial) {
    return grn_ts_sorter_node_progress(ctx, sorter->head, sorter->offset,
                                       sorter->limit, recs, n_recs, n_rest);
  }
  return GRN_SUCCESS;
}

grn_rc
grn_ts_sorter_complete(grn_ctx *ctx, grn_ts_sorter *sorter,
                       grn_ts_record *recs, size_t n_recs, size_t *n_rest)
{
  grn_rc rc;
  size_t i, limit;
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!sorter || (!recs && n_recs) || !n_rest) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  if (sorter->offset >= n_recs) {
    return GRN_SUCCESS;
  }
  limit = sorter->limit;
  if (limit > (n_recs - sorter->offset)) {
    limit = n_recs;
  } else {
    limit += sorter->offset;
  }
  if (sorter->partial) {
    // FIXME: If there was no input. Partial sorting is not required.
    rc = grn_ts_sorter_node_progress(ctx, sorter->head, sorter->offset,
                                     limit, recs, n_recs, n_rest);
    if (rc == GRN_SUCCESS) {
      rc = grn_ts_sorter_node_complete(ctx, sorter->head, sorter->offset,
                                       limit, recs, n_recs, n_rest);
    }
  } else {
    rc = grn_ts_sorter_node_sort(ctx, sorter->head, sorter->offset,
                                 limit, recs, n_recs);
  }
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  if (sorter->offset) {
    for (i = 0; i < limit; i++) {
      recs[i] = recs[sorter->offset + i];
    }
  }
  *n_rest = limit;
  return GRN_SUCCESS;
}

/*-------------------------------------------------------------
 * grn_ts_sorter_builder.
 */

/* grn_ts_sorter_builder_init() initializes a sorter builder. */
static void
grn_ts_sorter_builder_init(grn_ctx *ctx, grn_ts_sorter_builder *builder)
{
  memset(builder, 0, sizeof(*builder));
  builder->table = NULL;
  builder->head = NULL;
  builder->tail = NULL;
}

/* grn_ts_sorter_builder_fin() finalizes a sorter builder. */
static void
grn_ts_sorter_builder_fin(grn_ctx *ctx, grn_ts_sorter_builder *builder)
{
  if (builder->head) {
    grn_ts_sorter_node_list_close(ctx, builder->head);
  }
  if (builder->table) {
    grn_obj_unlink(ctx, builder->table);
  }
}

grn_rc
grn_ts_sorter_builder_open(grn_ctx *ctx, grn_obj *table,
                           grn_ts_sorter_builder **builder)
{
  grn_rc rc;
  grn_ts_sorter_builder *new_builder;
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!table || !grn_ts_obj_is_table(ctx, table) || !builder) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  new_builder = GRN_MALLOCN(grn_ts_sorter_builder, 1);
  if (!new_builder) {
    GRN_TS_ERR_RETURN(GRN_NO_MEMORY_AVAILABLE,
                      "GRN_MALLOCN failed: %" GRN_FMT_SIZE " x 1",
                      sizeof(grn_ts_sorter_builder));
  }
  grn_ts_sorter_builder_init(ctx, new_builder);
  rc = grn_ts_obj_increment_ref_count(ctx, table);
  if (rc != GRN_SUCCESS) {
    grn_ts_sorter_builder_fin(ctx, new_builder);
    GRN_FREE(new_builder);
    return rc;
  }
  new_builder->table = table;
  *builder = new_builder;
  return GRN_SUCCESS;
}

grn_rc
grn_ts_sorter_builder_close(grn_ctx *ctx, grn_ts_sorter_builder *builder)
{
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!builder) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  grn_ts_sorter_builder_fin(ctx, builder);
  GRN_FREE(builder);
  return GRN_SUCCESS;
}

grn_rc
grn_ts_sorter_builder_complete(grn_ctx *ctx, grn_ts_sorter_builder *builder,
                               size_t offset, size_t limit,
                               grn_ts_sorter **sorter)
{
  grn_rc rc;
  grn_ts_sorter *new_sorter;
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!builder || !builder->head || !sorter) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  rc = grn_ts_sorter_open(ctx, builder->table, builder->head,
                          offset, limit, &new_sorter);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  builder->head = NULL;
  builder->tail = NULL;
  *sorter = new_sorter;
  return GRN_SUCCESS;
}

grn_rc
grn_ts_sorter_builder_push(grn_ctx *ctx, grn_ts_sorter_builder *builder,
                           grn_ts_expr *expr, grn_ts_bool reverse)
{
  grn_rc rc;
  grn_ts_sorter_node *new_node;
  if (!ctx) {
    return GRN_INVALID_ARGUMENT;
  }
  if (!builder || !expr || expr->table != builder->table) {
    GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
  }
  switch (expr->data_kind) {
    case GRN_TS_INT:
    case GRN_TS_FLOAT:
    case GRN_TS_TIME:
    case GRN_TS_TEXT: {
      break;
    }
    case GRN_TS_INT_VECTOR:
    case GRN_TS_FLOAT_VECTOR:
    case GRN_TS_TIME_VECTOR:
    case GRN_TS_TEXT_VECTOR: {
      GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "not supported yet");
    }
    default: {
      GRN_TS_ERR_RETURN(GRN_INVALID_ARGUMENT, "invalid argument");
    }
  }
  rc = grn_ts_sorter_node_open(ctx, expr, reverse, &new_node);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  if (builder->tail) {
    builder->tail->next = new_node;
  } else {
    builder->head = new_node;
  }
  builder->tail = new_node;
  return GRN_SUCCESS;
}
