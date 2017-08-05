#ifndef MYSQL_SERVICE_NUMA_INCLUDED
/* Copyright (c) 2017, Monty Program Ab

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

/**
  @file
  numa service

  Functions to numa multi processor architectures.
*/

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HAVE_LIBNUMA
#include <numa.h>
#include <numaif.h>

#define MYSQL_MAX_NUM_NUMA_NODES 16

extern unsigned long int no_of_allowed_nodes;
extern unsigned long int allowed_numa_nodes[MYSQL_MAX_NUM_NUMA_NODES];
extern unsigned long int size_of_numa_node[MYSQL_MAX_NUM_NUMA_NODES];
extern unsigned long int total_numa_nodes_size;

#define mysql_numa_bind(X) numa_bind(X)
#define mysql_numa_get_membind(X) numa_get_membind(X)
#define mysql_numa_get_mems_allowed(X) numa_get_mems_allowed(X)
#define mysql_numa_bitmask_alloc(X) numa_bitmask_alloc(X)
#define mysql_numa_bitmask_clearall(X) numa_bitmask_clearall(X)
#define mysql_numa_bitmask_setbit(X, Y) numa_bitmask_setbit(X, Y)
#define mysql_numa_bitmask_isbitset(X, Y) numa_bitmask_isbitset(X, Y)
#define mysql_numa_node_size(X, Y) numa_node_size(X, Y)

static inline void mysql_bind_thread_to_node(unsigned long int node)
{
  struct bitmask* node_mask = mysql_numa_bitmask_alloc(MYSQL_MAX_NUM_NUMA_NODES);
  mysql_numa_bitmask_setbit(node_mask, node);
  mysql_numa_bind(node_mask);
}

static inline int mysql_node_of_cur_thread(void)
{
  struct bitmask* node_mask = mysql_numa_get_membind();
  int num_nodes = 0;
  int node = 0;

  for (int i = 0; i < MYSQL_MAX_NUM_NUMA_NODES; i++) {
    if (mysql_numa_bitmask_isbitset(node_mask, i)) {
      node = i;
      num_nodes++;
    }
  }

  if (num_nodes == 1) {
    return node;
  } else {
    return -1;
  }
}

#endif // HAVE_LIBNUMA

#ifdef __cplusplus
}
#endif

#define MYSQL_SERVICE_NUMA_INCLUDED
#endif

