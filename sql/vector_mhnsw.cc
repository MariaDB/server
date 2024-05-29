/*
   Copyright (c) 2024, MariaDB plc

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

#include <my_global.h>
#include "vector_mhnsw.h"
#include "field.h"
#include "item.h"
#include "sql_queue.h"
#include <scope.h>

const LEX_CSTRING mhnsw_hlindex_table={STRING_WITH_LEN("\
  CREATE TABLE i (                                      \
    src varbinary(255) not null,                        \
    dst varbinary(255) not null,                        \
    index (src))                                        \
")};

static void store_ref(TABLE *t, handler *h, uint n)
{
  t->hlindex->field[n]->store((char*)h->ref, h->ref_length, &my_charset_bin);
}

int mhnsw_insert(TABLE *table, KEY *keyinfo)
{
  TABLE *graph= table->hlindex;
  MY_BITMAP *old_map= dbug_tmp_use_all_columns(table, &table->read_set);
  Field *field= keyinfo->key_part->field;
  String buf, *res= field->val_str(&buf);
  handler *h= table->file;
  int err= 0;
  dbug_tmp_restore_column_map(&table->read_set, old_map);

  /* metadata are checked on open */
  DBUG_ASSERT(graph);
  DBUG_ASSERT(keyinfo->algorithm == HA_KEY_ALG_VECTOR);
  DBUG_ASSERT(keyinfo->usable_key_parts == 1);
  DBUG_ASSERT(field->binary());
  DBUG_ASSERT(field->cmp_type() == STRING_RESULT);
  DBUG_ASSERT(res); // ER_INDEX_CANNOT_HAVE_NULL
  DBUG_ASSERT(h->ref_length <= graph->field[0]->field_length);
  DBUG_ASSERT(h->ref_length <= graph->field[1]->field_length);

  if (res->length() == 0 || res->length() % 4)
    return 1;

  // let's do every node to every node
  h->position(table->record[0]);
  graph->field[0]->store(1);
  store_ref(table, h, 0);

  if (h->lookup_handler->ha_rnd_init(1))
    return 1;
  while (! ((err= h->lookup_handler->ha_rnd_next(h->lookup_buffer))))
  {
    h->lookup_handler->position(h->lookup_buffer);
    if (graph->field[0]->cmp(h->lookup_handler->ref) == 0)
      continue;
    store_ref(table, h->lookup_handler, 1);
    if ((err= graph->file->ha_write_row(graph->record[0])))
      break;
  }
  h->lookup_handler->ha_rnd_end();

  return err == HA_ERR_END_OF_FILE ? 0 : err;
}

struct Node
{
  float distance;
  uchar ref[1000];
};

static int cmp_float(void *, const Node *a, const Node *b)
{
  return a->distance < b->distance ? -1 : a->distance == b->distance ? 0 : 1;
}

int mhnsw_first(TABLE *table, Item *dist, ulonglong limit)
{
  TABLE *graph= table->hlindex;
  Queue<Node> todo, result;
  Node *cur;
  String *str, strbuf;
  const size_t ref_length= table->file->ref_length;
  const size_t element_size= ref_length + sizeof(float);
  uchar *key= (uchar*)alloca(ref_length + 32);
  Hash_set<Node> visited(PSI_INSTRUMENT_MEM, &my_charset_bin, limit,
                         sizeof(float), ref_length, 0, 0, HASH_UNIQUE);
  uint keylen;
  int err= 0;

  DBUG_ASSERT(graph);

  if (todo.init(1000, 0, cmp_float)) // XXX + autoextent
    return HA_ERR_OUT_OF_MEM;

  if (result.init(limit, 1, cmp_float))
    return HA_ERR_OUT_OF_MEM;

  if ((err= graph->file->ha_index_init(0, 1)))
    return err;

  SCOPE_EXIT([graph](){ graph->file->ha_index_end(); });

  // 1. read a start row
  if ((err= graph->file->ha_index_last(graph->record[0])))
    return err;

  if (!(str= graph->field[0]->val_str(&strbuf)))
    return HA_ERR_CRASHED;

  DBUG_ASSERT(str->length() == ref_length);

  cur= (Node*)table->in_use->alloc(element_size);
  memcpy(cur->ref, str->ptr(), ref_length);

  if ((err= table->file->ha_rnd_init(0)))
    return err;

  if ((err= table->file->ha_rnd_pos(table->record[0], cur->ref)))
    return HA_ERR_CRASHED;

  // 2. add it to the todo
  cur->distance= dist->val_real();
  if (dist->is_null())
    return HA_ERR_END_OF_FILE;
  todo.push(cur);
  visited.insert(cur);

  while (todo.elements())
  {
    // 3. pick the top node from the todo
    cur= todo.pop();

    // 4. add it to the result
    if (result.is_full())
    {
      // 5. if not added, greedy search done
      if (cur->distance > result.top()->distance)
        break;
      result.replace_top(cur);
    }
    else
      result.push(cur);

    float threshold= result.is_full() ? result.top()->distance : FLT_MAX;

    // 6. add all its [yet unvisited] neighbours to the todo heap
    keylen= graph->field[0]->get_key_image(key, ref_length, Field::itRAW);
    if ((err= graph->file->ha_index_read_map(graph->record[0], key, 3,
                                             HA_READ_KEY_EXACT)))
      return HA_ERR_CRASHED;

    do {
      if (!(str= graph->field[1]->val_str(&strbuf)))
        return HA_ERR_CRASHED;

      if (visited.find(str->ptr(), ref_length))
        continue;

      if ((err= table->file->ha_rnd_pos(table->record[0], (uchar*)str->ptr())))
        return HA_ERR_CRASHED;

      float distance= dist->val_real();
      if (distance > threshold)
        continue;

      cur= (Node*)table->in_use->alloc(element_size);
      cur->distance= distance;
      memcpy(cur->ref, str->ptr(), ref_length);
      todo.push(cur);
      visited.insert(cur);
    } while (!graph->file->ha_index_next_same(graph->record[0], key, keylen));
    // 7. goto 3
  }

  // 8. return results
  Node **context= (Node**)table->in_use->alloc(sizeof(Node**)*result.elements()+1);
  graph->context= context;

  Node **ptr= context+result.elements();
  *ptr= 0;
  while (result.elements())
    *--ptr= result.pop();

  return mhnsw_next(table);
}

int mhnsw_next(TABLE *table)
{
  Node ***context= (Node***)&table->hlindex->context;
  if (**context)
    return table->file->ha_rnd_pos(table->record[0], (*(*context)++)->ref);
  return HA_ERR_END_OF_FILE;
}
