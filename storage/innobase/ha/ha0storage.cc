/*****************************************************************************

Copyright (c) 2007, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2020, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file ha/ha0storage.cc
Hash storage.
Provides a data structure that stores chunks of data in
its own storage, avoiding duplicates.

Created September 22, 2007 Vasil Dimov
*******************************************************/

#include "ha0storage.h"
#include "hash0hash.h"
#include "mem0mem.h"

/*******************************************************************//**
Copies data into the storage and returns a pointer to the copy. If the
same data chunk is already present, then pointer to it is returned.
Data chunks are considered to be equal if len1 == len2 and
memcmp(data1, data2, len1) == 0. If "data" is not present (and thus
data_len bytes need to be allocated) and the size of storage is going to
become more than "memlim" then "data" is not added and NULL is returned.
To disable this behavior "memlim" can be set to 0, which stands for
"no limit". */
const void*
ha_storage_put_memlim(
/*==================*/
	ha_storage_t*	storage,	/*!< in/out: hash storage */
	const void*	data,		/*!< in: data to store */
	ulint		data_len,	/*!< in: data length */
	ulint		memlim)		/*!< in: memory limit to obey */
{
  const uint32_t fold= my_crc32c(0, data, data_len);
  ha_storage_node_t** after = reinterpret_cast<ha_storage_node_t**>
    (&storage->hash.cell_get(fold)->node);
  for (; *after; after= &(*after)->next)
    if ((*after)->data_len == data_len &&
        !memcmp((*after)->data, data, data_len))
      return (*after)->data;

  /* not present */

  /* check if we are allowed to allocate data_len bytes */
  if (memlim > 0 && ha_storage_get_size(storage) + data_len > memlim)
    return nullptr;

  /* we put the auxiliary node struct and the data itself in one
  continuous block */
  ha_storage_node_t *node= static_cast<ha_storage_node_t*>
    (mem_heap_alloc(storage->heap, sizeof *node + data_len));
  node->data_len= data_len;
  node->data= &node[1];
  node->next= nullptr;
  memcpy(const_cast<void*>(node->data), data, data_len);
  *after= node;
  return node->data;
}

#ifdef UNIV_COMPILE_TEST_FUNCS

void
test_ha_storage()
{
	ha_storage_t*	storage;
	char		buf[1024];
	int		i;
	const void*	stored[256];
	const void*	p;

	storage = ha_storage_create(0, 0);

	for (i = 0; i < 256; i++) {

		memset(buf, i, sizeof(buf));
		stored[i] = ha_storage_put(storage, buf, sizeof(buf));
	}

	//ha_storage_empty(&storage);

	for (i = 255; i >= 0; i--) {

		memset(buf, i, sizeof(buf));
		p = ha_storage_put(storage, buf, sizeof(buf));

		if (p != stored[i]) {
			ib::warn() << "ha_storage_put() returned " << p
				<< " instead of " << stored[i] << ", i=" << i;
			return;
		}
	}

	ib::info() << "all ok";

	ha_storage_free(storage);
}

#endif /* UNIV_COMPILE_TEST_FUNCS */
