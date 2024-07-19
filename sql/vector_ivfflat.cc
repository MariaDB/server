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

const LEX_CSTRING ivflfat_hlindex_table_def(THD *thd, uint ref_length)
{
  const char templ[]="CREATE TABLE i (                        "
                     "  clusterID tinyint not null,           "
                     "  centroid blob not null,               "
                     "  data blob not null,                   "
                     "  key (clusterID))                      ";
  size_t len= sizeof(templ) + 32;
  char *s= thd->alloc(len);
  len= my_snprintf(s, len, templ, ref_length);
  return {s, len};
}

template<typename T>
void printAsString(  T* value) {
    printf("%s\n", value);
}

template<typename T>
void printAsHex( T* value, size_t length) {
    printf("As Hex: ");
    for (size_t i = 0; i < length; ++i) {
        printf("%02x", value[i]);
    }
    printf("\n");
}

template<typename T>
void printAsDecimal(  T* value, size_t length) {
    printf("As Decimal: ");
    for (size_t i = 0; i < length; ++i) {
        printf("%u", value[i]);
    }
    printf("\n");
}

class cluster_node{
private:
  uchar *ref;
  cluster_node *next;
  cluster_node *prev;
public:
  cluster_node(){
    this->ref = nullptr;
    this->next = nullptr;
    this->prev = nullptr;
  }
  cluster_node(uchar *ref){
    this->ref = ref;
    this->next = nullptr;
    this->prev = nullptr;
  }
  void setNext(cluster_node *next){
    this->next = next;
  }
  void setPrev(cluster_node *prev){
    this->prev = prev;
  }
  uchar *getRef(){
    return this->ref;
  }
  cluster_node *getNext(){
    return this->next;
  }
  cluster_node *getPrev(){
    return this->prev;
  }
};
class cluster_list{
private:
  cluster_node *head;
  cluster_node *tail;
  size_t elements;
public:
  cluster_list(){
    this->head = nullptr;
    this->tail = nullptr;
    this->elements = 0;
  }
  void push_back(uchar *ref){
    cluster_node *new_node = new cluster_node(ref);
    if(this->head == nullptr){
      this->head = new_node;
      this->tail = new_node;
    }else{
      this->tail->setNext(new_node);
      new_node->setPrev(this->tail);
      this->tail = new_node;
    }
    this->elements++;
  }
  size_t getElements(){
    return this->elements;
  }
  cluster_node *getHead(){
    return this->head;
  }
  cluster_node *getTail(){
    return this->tail;
  }
};

int write_cluster(TABLE *graph,
                  size_t cluster_id,
                  const char *centroid,
                  size_t vec_len,
                  cluster_list& cluster_nodes,
                  size_t ref_len){

  printf("from write cluster \n");
  printf("ref_len %d\n", (int)ref_len);
  auto head=cluster_nodes.getHead();
  while(head!=nullptr){
    printAsHex(head->getRef(), ref_len);
    head=head->getNext();
  }

  size_t total_size= sizeof(uint16_t) +
  cluster_nodes.getElements() * ref_len;

  // Allocate memory for the struct and the flexible array member
  char *neighbor_array_bytes= static_cast<char *>(alloca(total_size));

  DBUG_ASSERT(cluster_nodes.getElements() <= INT16_MAX);
  // store the size of the data in the first 2 bytes
  *(uint16_t *) neighbor_array_bytes= cluster_nodes.getElements();
  char *pos= neighbor_array_bytes + sizeof(uint16_t);

  head=cluster_nodes.getHead();
  while(head!=nullptr){
    memcpy(pos, head->getRef(), ref_len);
    pos+= ref_len;
    head=head->getNext();
  }
  graph->field[0]->store(cluster_id);
  graph->field[1]->store_binary(
    centroid,
    vec_len);
  graph->field[2]->set_null();

  uchar *key= (uchar*)alloca(graph->key_info->key_length);
  key_copy(key, graph->record[0], graph->key_info, graph->key_info->key_length);
  int err= graph->file->ha_index_read_map(graph->record[1], key,
                                          HA_WHOLE_KEY,
                                          HA_READ_KEY_EXACT);

  graph->field[2]->store_binary(neighbor_array_bytes, total_size);
  // no record
  if (err == HA_ERR_KEY_NOT_FOUND)
  {
    graph->file->ha_write_row(graph->record[0]);
    graph->file->position(graph->record[0]);  
  }else{
    graph->file->ha_update_row(graph->record[1], graph->record[0]);
  }
  my_safe_afree(neighbor_array_bytes, total_size);
  return false;
}



int find_nearst_class(TABLE*graph,  char* vec, size_t vec_len){
  printAsHex(vec, vec_len);

  graph->file->ha_index_first(graph->record[0]);
  String buf;
  double distance = euclidean_vec_distance(reinterpret_cast<float *>(vec),reinterpret_cast< float *>( graph->field[1]->val_str(&buf)->c_ptr() ), vec_len/4);
  printAsHex(graph->field[1]->val_str(&buf)->c_ptr(), vec_len);
  int id = graph->field[0]->val_int();
  int count  =1;
  while (!graph->file->ha_index_next(graph->record[0]))
  {
    double cur_distance = euclidean_vec_distance(reinterpret_cast<float*>(vec),reinterpret_cast< float *>( graph->field[1]->val_str(&buf)->c_ptr()), vec_len/4);
    printAsHex(graph->field[1]->val_str(&buf)->c_ptr(), vec_len);

    if(cur_distance < distance){
      distance = cur_distance;
      id = graph->field[0]->val_int();
    }
    count++;
  }
  return id;
}

int ivfflat_insert(TABLE *table, KEY *keyinfo)
{
  printf("ivfflat_insert\n");
  TABLE *graph= table->hlindex;
  Field *vec_field= keyinfo->key_part->field;
  String buf, *res= vec_field->val_str(&buf);
  handler *h= table->file->lookup_handler;

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
  printf("%d\n",res->length());//lenth of the vector in bytes must be 
  printf("%lld\n",graph->field[0]->val_int());//max value for layer field in the index table layer "frandom if empty,0 at the beginneing"
  printf("%d\n", h->ref_length);//lenght of the id"primary key" 4bytes for int , 8 bytes for bigINT

  if (res->length() == 0 || res->length() % 4)
    return bad_value_on_insert(vec_field);
    
  // res is the actual vector
  // ref is id for the row

  int sz= res->length();
  printAsHex(table->record[0], h->ref_length);
  printAsHex(graph->record[0], sz);
  table->file->position(table->record[0]);
  
  printAsHex(table->record[0], h->ref_length);
  printAsHex(graph->record[0], sz);
  printAsHex(h->ref, h->ref_length);
  if (int err= h->ha_rnd_init(0))
    return err;

  SCOPE_EXIT([h](){ h->ha_rnd_end(); });

  if (int err= graph->file->ha_index_init(0, 1))
    return err;

  int err=graph->file->ha_index_last(graph->record[0]);
  
  cluster_list cluster_nodes;
  // insert the refreence of the new record in the cluster
  cluster_nodes.push_back(h->ref);
  if (err)
  {
    if (err != HA_ERR_END_OF_FILE)
    {
      graph->file->ha_index_end();
      return err;
    }
    // First insert!
    err = write_cluster(graph, 0, res->ptr(), res->length(), cluster_nodes,h->ref_length);
    graph->file->ha_index_end();
    return err;
  }
  else if(graph->field[0]->val_int() < num_clusters-1){
    // continue instert untill we have "num_clusters" points
    err=write_cluster(graph, graph->field[0]->val_int() +1, res->ptr(), res->length(), cluster_nodes ,h->ref_length);
    graph->file->ha_index_end();
    return err;
  }
  printf("find the nearset cendroid\n");
  int id=find_nearst_class(graph, res->c_ptr(), res->length());
  printf("nearset cluster is :%d\n",id);

  graph->field[0]->store(id);

  uchar* key=(uchar*)alloca(graph->key_info->key_length); 
  key_copy(key, graph->record[0], graph->key_info, graph->key_info->key_length);
   err= graph->file->ha_index_read_map(graph->record[0], key,
                                          HA_WHOLE_KEY,
                                          HA_READ_KEY_EXACT);
  if (err)
  {
    graph->file->ha_index_end();
    return err;
  }
  printf("current record cluster id :  %d\n",(int)graph->field[0]->val_int());
  //load the cluster nodes from graph->fiels[2]
  String strbuf;
  String *str= graph->field[2]->val_str(&strbuf);
  uchar *cluster_data= reinterpret_cast<uchar *>(str->c_ptr());
  // load the number of points from the first 2 bytes
  uint16_t number_of_points=
    *reinterpret_cast<uint16_t*>(cluster_data);
  printf("cluster size data before insert new record : %d\n",number_of_points);
  uchar *start = cluster_data + sizeof(uint16_t);
  // print all refrences in this id for debuging
  for (uint16_t i= 0; i < number_of_points; i++)
  {
    cluster_nodes.push_back(start);
    start+= h->ref_length;
  }
  // insert the refreence of the new record in the cluster
  // now record0 is the cluster record wich we need to insert in the data field
  err= write_cluster(graph, id, res->ptr(), res->length(),cluster_nodes ,h->ref_length);
  graph->file->ha_index_end();
  return err;
}

