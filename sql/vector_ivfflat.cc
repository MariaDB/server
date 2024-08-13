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
                     "  pointsRef blob not null,                "
                     "  pointsVec blob not null,                   "
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
        printf("%02x", static_cast<unsigned char>(value[i]));
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

int REINDEX(TABLE *table);

double distance_func(char *vec1, char *vec2, size_t vec_len){
  return euclidean_vec_distance(reinterpret_cast<float *>(vec1),reinterpret_cast< float *>(vec2), vec_len/4);
}

class cluster_node{
private:
  uchar *ref;
  uchar *vec;
  cluster_node *next;
  cluster_node *prev;
public:
  cluster_node(){
    this->ref = nullptr;
    this->next = nullptr;
    this->prev = nullptr;
    this->vec = nullptr;
  }
  cluster_node(uchar *ref, uchar *vec){
    this->ref = ref;
    this->vec = vec;
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
  uchar *getVec(){
    return this->vec;
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
  void push_back(uchar *ref, uchar *vec){
    cluster_node *new_node = new cluster_node(ref, vec);
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
  ~cluster_list(){
    cluster_node *current = this->head;
    while(current != nullptr){
      cluster_node *next = current->getNext();
      delete current;
      current = next;
    }
  }
};

int write_cluster(TABLE *graph,
                  size_t cluster_id,
                  const char *centroid,
                  size_t vec_len,
                  cluster_list& cluster_nodes,
                  size_t ref_len){

  DBUG_ASSERT(cluster_nodes.getElements() <= INT16_MAX);
  auto head=cluster_nodes.getHead();
  while(head!=nullptr){
    head=head->getNext();
  }
  size_t total_size_ref=sizeof(uint16_t) +
  cluster_nodes.getElements() * ref_len;

  size_t total_size_vec=sizeof(uint16_t) +
  cluster_nodes.getElements() * vec_len;

  // Allocate memory for the struct and the flexible array member
  char *neighbor_array_bytes= static_cast<char *>(alloca(total_size_ref));
  // store the size of the data in the first 2 bytes
  *(uint16_t *) neighbor_array_bytes= cluster_nodes.getElements();
  char *pos= neighbor_array_bytes + sizeof(uint16_t);

  head=cluster_nodes.getHead();
  while(head!=nullptr){
    memcpy(pos, head->getRef(), ref_len);
    pos+= ref_len;
    head=head->getNext();
  }

  // Allocate memory for the struct and the flexible array member
  char *neighbor_array_bytes_vec= static_cast<char *>(alloca(total_size_vec));
  // store the size of the data in the first 2 bytes
  *(uint16_t *) neighbor_array_bytes_vec= vec_len;
  pos= neighbor_array_bytes_vec + sizeof(uint16_t);
  head=cluster_nodes.getHead();
  while(head!=nullptr){
    memcpy(pos, head->getVec(), vec_len);
    pos+= vec_len;
    head=head->getNext();
  }

  graph->field[0]->store(cluster_id);
  graph->field[1]->store_binary(
    centroid,
    vec_len);
  graph->field[2]->set_null();
  graph->field[3]->set_null();
  
  uchar *key= (uchar*)alloca(graph->key_info->key_length);
  key_copy(key, graph->record[0], graph->key_info, graph->key_info->key_length);
  int err= graph->file->ha_index_read_map(graph->record[1], key,
                                          HA_WHOLE_KEY,
                                          HA_READ_KEY_EXACT);

  graph->field[2]->store_binary(neighbor_array_bytes, total_size_ref);
  graph->field[3]->store_binary(neighbor_array_bytes_vec, total_size_vec);
  // no record
  if (err == HA_ERR_KEY_NOT_FOUND)
  {
    graph->file->ha_write_row(graph->record[0]);
    graph->file->position(graph->record[0]);  
  }else{
    graph->file->ha_update_row(graph->record[1], graph->record[0]);
  }
  my_safe_afree(neighbor_array_bytes, total_size_ref);
  my_safe_afree(neighbor_array_bytes_vec, total_size_vec);
  return false;
}


int find_nearst_class(TABLE*graph,  char* vec, size_t vec_len){

  graph->file->ha_index_first(graph->record[0]);
  String buf;
  double distance =distance_func(vec, graph->field[1]->val_str(&buf)->c_ptr(), vec_len);
  int id = graph->field[0]->val_int();
  int count  =1;
  while (!graph->file->ha_index_next(graph->record[0]))
  {
    double cur_distance =distance_func(vec, graph->field[1]->val_str(&buf)->c_ptr(), vec_len);
    if(cur_distance < distance){
      distance = cur_distance;
      id = graph->field[0]->val_int();
    }
    count++;
  }
  return id;
}

static int bad_value_on_insert(Field *f)
{
  my_error(ER_TRUNCATED_WRONG_VALUE_FOR_FIELD, MYF(0), "vector", "...",
           f->table->s->db.str, f->table->s->table_name.str, f->field_name.str,
           f->table->in_use->get_stmt_da()->current_row_for_warning());
  return HA_ERR_GENERIC;
}
int ivfflat_insert(TABLE *table, KEY *keyinfo)
{
  printf("ivfflat_insert\n");
  // THD *thd= table->in_use;
  TABLE *graph= table->hlindex;
  MY_BITMAP *old_map= dbug_tmp_use_all_columns(table, &table->read_set);
  Field *vec_field= keyinfo->key_part->field;
  String buf, *res= vec_field->val_str(&buf);
  handler *h= table->file->lookup_handler;

  /* metadata are checked on open */
  DBUG_ASSERT(graph);
  DBUG_ASSERT(keyinfo->algorithm == HA_KEY_ALG_MHNSW);
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
  printf("the size of the vector is : %d\n",sz);
  printAsHex(res->c_ptr(), sz);
  table->file->position(table->record[0]);
  
  if (int err= h->ha_rnd_init(0))
    return err;

  SCOPE_EXIT([h](){ h->ha_rnd_end(); });

  if (int err= graph->file->ha_index_init(0, 1))
    return err;

  int err=graph->file->ha_index_last(graph->record[0]);
  
  cluster_list cluster_nodes;
  // insert the refreence of the new record in the cluster
  cluster_nodes.push_back(h->ref, (uchar*)res->c_ptr());
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
  String strbuf,strbuf2;
  String *str= graph->field[2]->val_str(&strbuf);
  String *str2= graph->field[3]->val_str(&strbuf2);
  uchar *cluster_data= reinterpret_cast<uchar *>(str->c_ptr());
  uchar *cluster_data_vec= reinterpret_cast<uchar *>(str2->c_ptr());
  // load the number of points from the first 2 bytes
  uint16_t number_of_points=
    *reinterpret_cast<uint16_t*>(cluster_data);
  printf("cluster size data before insert new record : %d\n",number_of_points);
  uchar *start = cluster_data + sizeof(uint16_t);
  uchar *start_vec = cluster_data_vec + sizeof(uint16_t);
  // print all refrences in this id for debuging
  for (uint16_t i= 0; i < number_of_points; i++)
  {
    cluster_nodes.push_back(start, start_vec);
    start+= h->ref_length;
    start_vec+= res->length();
  }
  // insert the refreence of the new record in the cluster
  // now record0 is the cluster record wich we need to insert in the data field
  err= write_cluster(graph, id, res->ptr(), res->length(),cluster_nodes ,h->ref_length);
  graph->file->ha_index_end();
  dbug_tmp_restore_column_map(&table->read_set, old_map);
  REINDEX(table);
  return err;
}


class cluster{
private:
  int id;
  char *centroid;
  size_t centroid_size;
  double distance;
  uchar *data;
  size_t data_size;
  size_t ref_size;
  size_t num_points;
public:
  cluster(char *centroid,size_t centroid_size,int id){
    this->id = id;
    this->centroid_size = centroid_size;
    // allocate memory for the centroid
    // this->centroid = static_cast<char *>(my_safe_alloca(centroid_size));
    // memcpy(this->centroid,centroid,centroid_size);
    this->centroid=new char[centroid_size];
    for(size_t i=0;i<centroid_size;i++){
      this->centroid[i]=centroid[i];
    }
    this->data = nullptr;
    this->data_size = 0;
    this->num_points = 0;
    this->distance = -1;
  }
  void set_distance(double distance){
    this->distance = distance;
  }
  double get_distance()const{
    return this->distance;
  }
  int get_id()const{
    return this->id;
  }
  char *get_centroid()const{
    return this->centroid;
  }
  size_t get_centroid_size()const{
    return this->centroid_size;
  }
  ~cluster(){
    if(this->centroid != nullptr){
      my_safe_afree(this->centroid, this->centroid_size);
    }
    if(this->data != nullptr){
      my_safe_afree(this->data, this->data_size);
    }
  }
};

class cluster_point{
private:
  char* ref;
  char* vec;
  int ref_size;
  int vec_size;
  double distance;
public:
  cluster_point(char*ref,char*vec,int ref_size,int vec_size){
    this->distance=0;
    this->ref_size=ref_size;
    this->vec_size=vec_size;

    this->ref=new char[this->ref_size];
    for(int i=0;i<this->ref_size;i++)
      this->ref[i]=ref[i];

    this->vec=new char[this->vec_size];
    for(int i=0;i<this->vec_size;i++)
      this->vec[i]=vec[i];
  }
  double get_distance ()const{
    return this->distance;
  }
  void set_distance(double distance){
    this->distance=distance;
  }
  char* get_ref()const{
    return this->ref;
  }
  char* get_vec()const{
    return this->vec;
  }
  ~cluster_point(){
    delete[] ref;
    delete[] vec;
  }
};

int cmp_clusters(void *param,const cluster  *a, const cluster *b){
  if (a->get_distance() < b->get_distance())
    return -1;
  if (a->get_distance() > b->get_distance())
    return 1;
  return 0;
}

int cmp_points(void *param,const cluster_point *a,const cluster_point *b){
  if (a->get_distance() < b->get_distance())
    return -1;
  if (a->get_distance() > b->get_distance())
    return 1;
  return 0;
}

int ivfflat_first(TABLE *table, KEY *keyinfo, Item *dist, ulonglong limit)
{
  printf("ivfvlat _first\n");
  THD *thd= table->in_use;
  TABLE *graph= table->hlindex;
  Item_func_vec_distance *fun= (Item_func_vec_distance *)dist;
  String buf, *res= fun->get_const_arg()->val_str(&buf);
  handler *h= table->file;

  if (int err= h->ha_rnd_init(0))
    return err;

  if (int err= graph->file->ha_index_init(0, 1))
    return err;

  int err= graph->file->ha_index_last(graph->record[0]);
  graph->file->ha_index_end();

  if (err)
    return err;

  // step 1: sort the clusters by distance to the query vector
  Queue<cluster>cluster_pq; 
  cluster_pq.init(num_clusters, false, cmp_clusters);
  List<cluster> clusters;
  // loop over all clusters
  graph->file->ha_index_init(0, 1);
  graph->file->ha_index_first(graph->record[0]);
  double distance= distance_func(res->c_ptr(),graph->field[1]->val_str(&buf)->c_ptr(),res->length());
  cluster* c = new cluster(graph->field[1]->val_str(&buf)->c_ptr(),res->length(),graph->field[0]->val_int());
  c->set_distance(distance);
  cluster_pq.push(c);
  clusters.push_back(c);
  // c= nullptr;
  while (!graph->file->ha_index_next(graph->record[0]))
  {
    // load the cluster centroid
    String buf;
    char *centroid= graph->field[1]->val_str(&buf)->c_ptr();
    int id = graph->field[0]->val_int();
    // calculate the distance between the query vector and the centroid
    distance = distance_func(res->c_ptr(),centroid,res->length());
    // insert the cluster in the priority queue
    c = new cluster(centroid,res->length(),id);
    c->set_distance(distance);
    cluster_pq.push(c);
    clusters.push_back(c);
  }
  // select nprobs clusters
  // TODO if limit is large and near the num_clusters , then set _limit=limit will load all the table in the ram
  // _limit need to be ratio from num_clusters (from experments it is good to be 5% in case num_clusters exceed 1000)
  int _limit=limit;
  Queue<cluster_point>points_pq;
  points_pq.init(10000, false, cmp_points);
  List<cluster_point> points;
  cluster_point*c_p;

  while(cluster_pq.elements() && _limit--)
  {
    cluster *c = cluster_pq.pop();
    int id=c->get_id();
    // load all data in this cluster
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
    //load the cluster nodes from graph->fiels[2]
    String strbuf,strbuf2;
    String *str= graph->field[2]->val_str(&strbuf);
    String *str2= graph->field[3]->val_str(&strbuf2);
    char *cluster_data_ref= reinterpret_cast<char *>(str->c_ptr());
    char *cluster_data_vec= reinterpret_cast<char *>(str2->c_ptr());
    // load the number of points from the first 2 bytes
    uint16_t number_of_points=
      *reinterpret_cast<uint16_t*>(cluster_data_ref);
    char *start = cluster_data_ref + sizeof(uint16_t);
    char *start_vec = cluster_data_vec + sizeof(uint16_t);
    for (uint16_t i= 0; i < number_of_points; i++)
    {
      c_p=new cluster_point(start,start_vec,h->ref_length,res->length());
      double distance= distance_func(res->c_ptr(),start_vec,res->length());
      c_p->set_distance(distance);
      points_pq.push(c_p);
      points.push_back(c_p);
      start+= h->ref_length;
      start_vec+= res->length();
    }
  }

  SCOPE_EXIT([graph](){ graph->file->ha_index_end(); });
  size_t context_size=limit * h->ref_length + sizeof(ulonglong);
  char *context= thd->alloc(context_size);
  graph->context= context;

  *(ulonglong*)context= limit;
  context+= context_size;
  _limit=limit;
  while (points_pq.elements() && _limit--)
  {
    cluster_point *c_p = points_pq.pop();
    context-= h->ref_length;
    memcpy(context, c_p->get_ref(), h->ref_length);
  }
  while (_limit--)
  {
    context-= h->ref_length;
    memset(context, 0, h->ref_length);
  }
  DBUG_ASSERT(context - sizeof(ulonglong) == graph->context);

  return mhnsw_next(table);
}

int ivfflat_next(TABLE *table)
{
  uchar *ref= (uchar*)(table->hlindex->context);
  if (ulonglong *limit= (ulonglong*)ref)
  {
    ref+= sizeof(ulonglong) + (--*limit) * table->file->ref_length;
    return table->file->ha_rnd_pos(table->record[0], ref);
  }
  return HA_ERR_END_OF_FILE;
}

/////////////////////////////////////////// REINDEX //////////////////////////////////////////// 
class VectorNode{
private:
  uchar* gref;
  uchar* vec;
  size_t vec_len;
  size_t ref_len;
  int cluster_id;
public:
  VectorNode(uchar* gref,uchar* vec,size_t vec_len,size_t ref_len){
    this->cluster_id=-1;
    this->vec_len=vec_len;
    this->ref_len=ref_len;
    this->gref=new uchar[ref_len];
    this->vec=new uchar[vec_len];
    for(size_t i=0;i<ref_len;i++){
      this->gref[i]=gref[i];
    }
    for(size_t i=0;i<vec_len;i++){
      this->vec[i]=vec[i];
    }
  }
  uchar* get_gref(){
    return this->gref;
  }
  uchar* get_vec(){
    return this->vec;
  }
  size_t get_vec_len(){
    return this->vec_len;
  }
  size_t get_ref_len(){
    return this->ref_len;
  }
  int get_cluster_id(){
    return this->cluster_id;
  }
  void set_cluster_id(int cluster_id){
    this->cluster_id=cluster_id;
  }
  ~VectorNode(){
    delete[] gref;
    delete[] vec;
  }
};


template<typename T>
class Linked_list{
private:

  class Node{
  private:
    Node *next;
    Node *prev;
    T data;
  public:
    Node(){
      this->next=nullptr;
      this->prev=nullptr;
    }
    Node(T data){
      this->data=data;
      this->next=nullptr;
      this->prev=nullptr;
    }
    void setNext(Node *next){
      this->next=next;
    }
    void setPrev(Node *prev){
      this->prev=prev;
    }
    Node *getNext(){
      return this->next;
    }
    Node *getPrev(){
      return this->prev;
    }
    T getData(){
      return this->data;
    }
  };
  
  Node *head;
  Node *tail;
  size_t elements;
public:
  Linked_list(){
    this->head=nullptr;
    this->tail=nullptr;
    this->elements=0;
  }
  void push_back(T data){
    if(this->head==nullptr){
      this->head=new Node(data);
      this->tail=this->head;
    
    }else{
      Node *node=new Node(data);
      this->tail->setNext(node);
      node->setPrev(this->tail);
      this->tail=node;
    }
    this->elements++;
  }
  size_t getElements(){
    return this->elements;
  }
  Node* getHead(){
    return this->head;
  }
  Node* getTail(){
    return this->tail;
  }
  void convert_to_array(T *&nodes){
    nodes=new T[this->elements];
    Node *current=this->head;
    size_t i=0;
    while(current!=nullptr){
      nodes[i]=current->getData();
      current=current->getNext();
      i++;
    }
  }
  ~Linked_list(){
    Node *current=this->head;
    while(current!=nullptr){
      Node *next=current->getNext();
      delete current;
      current=next;
    }
  }
};
class Cluster_Centroid{
private:
  uchar *centroid;
  uchar *temp_centroid;
  size_t centroid_size;
  size_t num_points;
  size_t temp_num_points;
public:
  Cluster_Centroid(){
    this->centroid=nullptr;
    this->temp_centroid=nullptr;
    this->centroid_size=0;
    this->num_points=0;
    this->temp_num_points=0;
  }
  Cluster_Centroid(size_t centroid_size){
    this->centroid=new uchar[centroid_size];
    this->temp_centroid=new uchar[centroid_size];
    this->centroid_size=centroid_size;
    this->num_points=0;
    this->temp_num_points=0;
    for(size_t i=0;i<centroid_size;i++){
      this->centroid[i]=0;
      this->temp_centroid[i]=0;
    }
  }
  Cluster_Centroid(uchar *centroid,size_t centroid_size){
    this->centroid=new uchar[centroid_size];
    this->temp_centroid=new uchar[centroid_size];
    for(size_t i=0;i<centroid_size;i++){
      this->centroid[i]=centroid[i];
      this->temp_centroid[i]=0;
    }
    this->centroid_size=centroid_size;
    this->num_points=1;
    this->temp_num_points=0;
  }
  void add_point(uchar* point){
    float* p1=reinterpret_cast<float*>(this->temp_centroid);
    float* p2=reinterpret_cast<float*>(point);
    for(size_t i=0;i<this->centroid_size/4;i++){
      p1[i]+=p2[i];
    }
    this->temp_num_points++;
  }
  void set_centroid(uchar* centroid){
    for(size_t i=0;i<this->centroid_size;i++){
      this->centroid[i]=centroid[i];
    }
    this->num_points=1;
  }
  void set_centroid_size(size_t centroid_size,bool reset=false){
    this->centroid_size=centroid_size;
    if(!reset){
      return;
    }
    // delete the old if not null
    if(this->centroid!=nullptr){
      delete[] centroid;
    }
    if(this->temp_centroid!=nullptr){
      delete[] temp_centroid;
    }
    this->centroid=new uchar[centroid_size];
    this->temp_centroid=new uchar[centroid_size];
    for(size_t i=0;i<centroid_size;i++){
      this->centroid[i]=0;
    }
    for(size_t i=0;i<centroid_size;i++){
      this->temp_centroid[i]=0;
    }
  }
  void calculate_centroid(){
    float* p1=reinterpret_cast<float*>(this->temp_centroid);
    float* p2=reinterpret_cast<float*>(this->centroid);
    for(size_t i=0;i<this->centroid_size/4;i++){
      p1[i]/=this->temp_num_points;
      p2[i]=p1[i];
      p1[i]=0;
    }
    this->num_points=this->temp_num_points;
    this->temp_num_points=0;
  }
  size_t get_num_points(){
    return this->num_points;
  }
  uchar *get_centroid(){
    return this->centroid;
  }
  size_t get_centroid_size(){
    return this->centroid_size;
  }
  ~Cluster_Centroid(){
    delete[] centroid;
    delete[] temp_centroid;
  }
};
void init_centroids(Cluster_Centroid *centroids,VectorNode **nodes,size_t num_points,THD *thd){
  int select[num_clusters];
  for(size_t i=0;i<num_clusters;i++){
    select[i]=-1;
  }
  int num=num_points;
  int *select_to_cluster=new int[num_points];
  for(size_t i=0;i<num_points;i++){
    select_to_cluster[i]=0;
  }
  for(size_t i=0;i<num_clusters;i++)
  {
    int ret=thd_rnd(thd)*num;
    if(ret==num)ret--;
    for(int j=0;j<=ret;j++)
      ret+=select_to_cluster[j];
    select_to_cluster[ret]+=1;
    select[i]=ret;
    num--;
  }
  // sort the selected indexes
  for(size_t i=0;i<num_clusters;i++){
    printf("cluster %d take vector index as initial centroid %d\n",(int)i,select[i]);
  }
  for(size_t i=0;i<num_clusters;i++){
    size_t index=select[i];
    centroids[i].set_centroid_size(nodes[index]->get_vec_len(),true);
    centroids[i].set_centroid(nodes[index]->get_vec());
  }
  delete[] select_to_cluster;
}
int REINDEX(TABLE *table)
{
  printf("ivfflat_reindex\n");
  const int max_iterations=100;
  TABLE *graph= table->hlindex;
  handler *h= table->file;
  THD *thd= table->in_use;
  DBUG_ASSERT(graph);
  if (int err= graph->file->ha_index_init(0, 1))
    return err;
  int err=graph->file->ha_index_last(graph->record[0]);
  if (err)
  {
    if (err != HA_ERR_END_OF_FILE)
    {
      graph->file->ha_index_end();
      return err;
    }
    graph->file->ha_index_end();
    return 0;
  }
  else if(graph->field[0]->val_int() < num_clusters-1){
    graph->file->ha_index_end();
    return 0;
  }
  graph->file->ha_index_first(graph->record[0]);
  // load all points in all clusters
  Linked_list<VectorNode*> cluster_nodes;
  do{
    //load the cluster nodes from graph->fiels[2]
    String strbuf,strbuf2;
    String *str= graph->field[2]->val_str(&strbuf);//ref
    String *str2= graph->field[3]->val_str(&strbuf2);//vec
    uchar *cluster_data= reinterpret_cast<uchar *>(str->c_ptr());
    uchar *cluster_data_vec= reinterpret_cast<uchar *>(str2->c_ptr());
    // load the number of points from the first 2 bytes
    uint16_t number_of_points=
      *reinterpret_cast<uint16_t*>(cluster_data);
    uint16_t vec_len=
      *reinterpret_cast<uint16_t*>(cluster_data_vec);
    uchar *start = cluster_data + sizeof(uint16_t);
    uchar *start_vec = cluster_data_vec + sizeof(uint16_t);
    for (uint16_t i= 0; i < number_of_points; i++)
    {
      VectorNode *node=new VectorNode(start,start_vec,vec_len,h->ref_length);
      cluster_nodes.push_back(node);
      start+= h->ref_length;
      start_vec+= vec_len;
    }
  }while (!graph->file->ha_index_next(graph->record[0]));
  
  VectorNode **nodes;
  cluster_nodes.convert_to_array(nodes);
  // select num_clusters centroids random from the points
  Cluster_Centroid centroids[num_clusters];
  init_centroids(centroids,nodes,cluster_nodes.getElements(),thd);
  for(size_t i=0;i<cluster_nodes.getElements();i++){
    nodes[i]->set_cluster_id(-1);
  }
  bool change=true;
  int iteration=0;
  while (change && iteration<max_iterations)
  {
    change=false;
    for(size_t i=0;i<cluster_nodes.getElements();i++){
      double min_distance=distance_func((char*)nodes[i]->get_vec(),(char*)centroids[0].get_centroid(),centroids[0].get_centroid_size());
      int cluster_id=0;
      for(size_t j=1;j<num_clusters;j++){
        double distance=distance_func((char*)nodes[i]->get_vec(),(char*)centroids[j].get_centroid(),centroids[j].get_centroid_size());
        if(distance<min_distance){
          min_distance=distance;
          cluster_id=j;
        }
      }
      if(nodes[i]->get_cluster_id()!=cluster_id){
        change=true;
        nodes[i]->set_cluster_id(cluster_id);
      }
      centroids[cluster_id].add_point(nodes[i]->get_vec());
    }
    for(size_t i=0;i<num_clusters;i++){
      centroids[i].calculate_centroid();
    }
    iteration++;
  }
  cluster_list clusters[num_clusters];
  for(size_t i=0;i<cluster_nodes.getElements();i++){
    clusters[nodes[i]->get_cluster_id()].push_back(nodes[i]->get_gref(),nodes[i]->get_vec());
  }
  for(size_t i=0;i<num_clusters;i++){
    write_cluster(graph, i, (char*)centroids[i].get_centroid(), centroids[i].get_centroid_size(), clusters[i], h->ref_length);
  }
  graph->file->ha_index_end();
  // clean memory
  for(size_t i=0;i<cluster_nodes.getElements();i++){
    delete nodes[i];
  }
  delete[] nodes;

  return 0;
}
//////////////////////////////////////////////////////////////
