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
#include <random>

#include <my_global.h>
#include "vector_mhnsw.h"

#include "field.h"
#include "hash.h"
#include "item.h"
#include "item_vectorfunc.h"
#include "key.h"
#include "my_base.h"
#include "mysql/psi/psi_base.h"
#include "sql_queue.h"
/*
general questions:
1- why there is custom data structure for hash_set, why not use std::unordered_set?also the same for list and any other data structure used here

project questions:
1- what is the importance of FVector over FVectorRef , why not use FVectorRef directly? 
*/

static const uint num_clusters=3; 

// const LEX_CSTRING ivflfat_hlindex_table_def(THD *thd, uint ref_length)
// {
//   const char templ[]="CREATE TABLE i (                   "
//                      "  clusterID int not null ,          "
//                      "  centroid blob not null,          "
//                      "  data blob not null,              "
//                      "  key (clusterID))                 ";
//   size_t len= sizeof(templ) + 32;
//   char *s= thd->alloc(len);
//   len= my_snprintf(s, len, templ, ref_length);
//   return {s, len};
// }

const LEX_CSTRING ivflfat_hlindex_table_def={STRING_WITH_LEN("\
  CREATE TABLE i (                                      \
    clusterID int not null                              \
    centroid blob not null,                        \
    data varbinary(10000) not null,                 \
    key (clusterID))                                 \
")};




void printAsString(const  char* value) {
    printf("%s\n", value);
}

void printAsHex(const  char* value, size_t length) {
    printf("As Hex: ");
    for (size_t i = 0; i < length; ++i) {
        printf("%02x", value[i]);
    }
    printf("\n");
}

void printAsDecimal(const  char* value, size_t length) {
    printf("As Decimal: ");
    for (size_t i = 0; i < length; ++i) {
        printf("%u", value[i]);
    }
    printf("\n");
}

int write_cluster(TABLE *graph,
                  size_t cluster_id,
                  const char *vec,
                  size_t vec_len,
                  const List<void*> &cluser_nodes,
                  size_t ref_len){
  size_t total_size= sizeof(uint16_t) +
    cluser_nodes.elements * ref_len;

  // Allocate memory for the struct and the flexible array member
  char *neighbor_array_bytes= static_cast<char *>(alloca(total_size));

  DBUG_ASSERT(cluser_nodes.elements <= INT16_MAX);
  *(uint16_t *) neighbor_array_bytes= cluser_nodes.elements;
  // char *pos= neighbor_array_bytes + sizeof(uint16_t);
  // for (const auto &node: cluser_nodes)
  // {
  //   DBUG_ASSERT(node.get_ref_len() == ref_len);
  //   memcpy(pos, node.get_ref(), node.get_ref_len());
  //   pos+= node.get_ref_len();
  // }

  graph->field[0]->store(cluster_id);
  graph->field[1]->store_binary(
    vec,
    vec_len);
  graph->field[2]->set_null();


  // uchar *key= (uchar*)alloca(graph->key_info->key_length);
  // key_copy(key, graph->record[0], graph->key_info, graph->key_info->key_length);

  // int err= graph->file->ha_index_read_map(graph->record[1], key,
  //                                         HA_WHOLE_KEY,
  //                                         HA_READ_KEY_EXACT);

  graph->field[2]->store_binary(neighbor_array_bytes, total_size);
  // no record
  // if (err == HA_ERR_KEY_NOT_FOUND)
  // {
    
  graph->file->ha_write_row(graph->record[0]);
  graph->file->position(graph->record[0]);
    
  // }
  // graph->file->ha_update_row(graph->record[1], graph->record[0]);
  my_safe_afree(neighbor_array_bytes, total_size);
  return false;
}




int ivfflat_insert(TABLE *table, KEY *keyinfo)
{
  printf("ivfflat_insert\n");
  THD *thd= table->in_use;
  TABLE *graph= table->hlindex;
  MY_BITMAP *old_map= dbug_tmp_use_all_columns(table, &table->read_set);
  Field *vec_field= keyinfo->key_part->field;
  String buf, *res= vec_field->val_str(&buf);
  handler *h= table->file->lookup_handler;
  MHNSW_Context ctx(table, vec_field);

  /* metadata are checked on open */
  DBUG_ASSERT(graph);
  DBUG_ASSERT(keyinfo->algorithm == HA_KEY_ALG_IVFFLAT);
  DBUG_ASSERT(keyinfo->usable_key_parts == 1);
  DBUG_ASSERT(vec_field->binary());
  DBUG_ASSERT(vec_field->cmp_type() == STRING_RESULT);
  DBUG_ASSERT(res); // ER_INDEX_CANNOT_HAVE_NULL

  // XXX returning an error here will rollback the insert in InnoDB
  // but in MyISAM the row will stay inserted, making the index out of sync:
  // invalid vector values are present in the table but cannot be found
  // via an index. The easiest way to fix it is with a VECTOR(N) type
  if (res->length() == 0 || res->length() % 4)
    return bad_value_on_insert(vec_field);
    
  printf("%d\n",res->length());//lenth of the vector in bytes must be 
  printf("%lld\n",graph->field[0]->val_int());//max value for layer field in the index table layer "frandom if empty,0 at the beginneing"
  printf("%d\n", h->ref_length);//lenght of the id"primary key" 4bytes for int , 8 bytes for bigINT
  // res is the actual vector
  // ref is id for the row

  //const double NORMALIZATION_FACTOR= 1 / std::log(thd->variables.mhnsw_max_edges_per_node);

  int sz= res->length();
  printAsHex(table->record[0], sz);
  printAsHex(graph->record[0], sz);
  table->file->position(table->record[0]);
  
  printAsHex(table->record[0], sz);
  printAsHex(graph->record[0], sz);

  if (int err= h->ha_rnd_init(0))
    return err;

  //SCOPE_EXIT([h](){ h->ha_rnd_end(); });

  if (int err= graph->file->ha_index_init(0, 1))
    return err;

  int err=graph->file->ha_index_last(graph->record[0]);
  graph->file->ha_index_end();
  
  if (err)
  {
    if (err != HA_ERR_END_OF_FILE)
    {
      graph->file->ha_index_end();
      return err;
    }
    // First insert!
    return write_cluster(graph, 0, res->ptr(), res->length(), {},h->ref_length);
  }
  else if(graph->field[0]->val_int() < num_clusters-1){
    // continue instert untill we have "num_clusters" points
    return write_cluster(graph, graph->field[0]->val_int() +1, res->ptr(), res->length(), {},h->ref_length);
  }
  // TODO (inplement the second stage of the insertion process)
  // 1- get the cluster id of the nearest cluster to the vector
  // 2- insert the vector in the cluster "append to the data field"
}


